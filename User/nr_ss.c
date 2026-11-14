/*
 * Noise reduction by Spectral Subtraction - ported 03/08/2026 from
 * JF3HZB / T. Uebo's ESP32 nr_ss.c/h (uploaded by the project owner,
 * dated June 10, 2025) to this project's bare-metal GD32F450
 * pipeline. See nr_ss.h for the module's contract; this comment is
 * about what changed getting here from the original.
 *
 * SCOPE: AM/USB/LSB only, mainly SSB - NOT WFM/NFM, per the project
 * owner. demod_am.c gates the call accordingly; this file has no idea
 * what a "demod mode" even is.
 *
 * WHAT CHANGED FROM THE ORIGINAL:
 *
 *   - dsps_fft2r_fc32()/dsps_bit_rev2r_fc32() (ESP-DSP, Xtensa) ->
 *     nr_fft_transform() below: a small self-contained radix-2
 *     complex FFT, same style (local Bhaskara-I sin/cos
 *     approximation, no libm dependency in the per-block hot path) as
 *     this project's own fft.c - see its "reimplemented locally to
 *     keep the two modules decoupled" note; same reasoning applied
 *     here rather than sharing fft.c's engine (which is hardwired to
 *     FFT_SIZE=512 real-input for the panadapter - generalizing it
 *     risked that well-exercised code for a completely different
 *     use, when a fresh ~130-line engine at this module's own fixed
 *     size is simpler and safer). CMSIS-DSP's arm_cfft_f32() would
 *     have been the other option, but it needs the CommonTables
 *     twiddle infrastructure this project has deliberately never
 *     pulled in (see the Makefile's comment by the CMSIS-DSP source
 *     list) - not worth the flash for a 128-point transform this
 *     cheap to hand-roll.
 *
 *   - Nstfft: the original computed this from the ESP32 project's own
 *     SAMPLE_BUFFER_SIZE/DR (not ours - sdr.h was never provided).
 *     Here it's NR_SS_BLOCK_SAMPLES (32, matching demod_am.c's
 *     EXISTING DEC_BLOCK_SAMPLES - the SSB phasing chain's decimated
 *     rate, reused rather than duplicated) * Nframe (4) = 128. A
 *     naive port running at this project's full 192kHz audio rate
 *     instead (matching the "no decimation" note elsewhere in this
 *     codebase) would have needed Nstfft=2048 - 16x the FFT cost, and
 *     inside a hard-realtime ISR with a documented ~2.667ms/block
 *     budget (see demod_am.c's ISR TIMING INSTRUMENTATION comment,
 *     added after a past deadline-overrun hang). Running at the
 *     already-decimated 12kHz rate instead was the project owner's
 *     call, and is why this fits.
 *
 *   - extern V_Rsv (an ESP32-side potentiometer reading, 0-4095) ->
 *     nr_ss_set_strength(), called from a new NR tile in the settings
 *     menu (RADIO page, ENCODER_TARGET_NR - see main.c).
 *
 *   - extern nr_ss_debug -> dropped. This project's debug_print() is
 *     used directly wherever useful instead of a one-shot latched
 *     flag.
 *
 *   - fabs()/pow() (double) -> fabsf()/powf() (float). This project
 *     is float32_t throughout; the originals were double-precision
 *     calls sitting in an otherwise-float pipeline - presumably
 *     harmless on the ESP32's FPU, but not a habit worth carrying
 *     over (and this project links -lm already, as of the WFM
 *     atan2f() addition, so powf() is free to use here without
 *     needing its own local approximation - it only runs once per
 *     nr_ss_set_strength() call, not per-sample, so it wouldn't have
 *     mattered much either way).
 *
 *   - NR_SS_init()/NR_SS() (CamelCase) -> nr_ss_init()/nr_ss_process()
 *     - this project's naming convention (see any other module here).
 *
 *   - The overlap-add reconstruction's final scale factor: the
 *     original's Nframe==4 branch has the "correct" unity-gain
 *     normalization (ATT_Ratio * (1.0f/Nframe)) commented OUT, with a
 *     bare "*= 500" active instead - almost certainly a leftover
 *     empirical scale factor tuned for THAT project's own signal
 *     range at that point in ITS pipeline, not a deliberate part of
 *     the algorithm (the commented-out line is the mathematically
 *     "proper" one - it's what ATT_Ratio's Hann/4x-overlap derivation
 *     is FOR). This port uses that proper normalization
 *     (NR_SS_ATT_RATIO below) instead of guessing at a scale factor
 *     that has no reason to match our signal range. Expect this may
 *     still need an empirical makeup-gain tweak once tried on real
 *     hardware, same as any NR stage's "how strong should this sound"
 *     - the strength control is exactly the knob for that, but if the
 *     OVERALL level (not just the reduction amount) needs a nudge,
 *     this is the constant to revisit.
 *
 * WHAT DIDN'T CHANGE: the actual spectral-subtraction algorithm
 * (Hann window, FFT -> magnitude-domain gain via a fixed threshold ->
 * IFFT, 4-frame 75%-overlap-add) is the same as the original, same
 * constants (the amp/gain formula's 0.96043457f/0.39782422f weights,
 * the Noise_min/max threshold range).
 */

#include <string.h>
#include <math.h>

#include "nr_ss.h"

#define NR_SS_NFRAME 4U
#define NR_SS_NSTFFT (NR_SS_BLOCK_SAMPLES * NR_SS_NFRAME) /* 32*4 = 128 */
#define NR_SS_HOP    (NR_SS_NSTFFT / NR_SS_NFRAME)        /* 32: frame advance = block length */

/* Threshold setting range - same as the original's Noise_min/Noise_max. */
#define NR_SS_NOISE_MIN 0.0f
#define NR_SS_NOISE_MAX 11.0f

/*
 * LA NORMALIZACION DEL SOLAPADO, MEDIDA - 06/10/2026.
 *
 * Era sqrtf(2,667) = 1,63309522, presentado en la cabecera de este fichero
 * como "la normalizacion matematicamente correcta de ganancia unidad". No
 * lo es, y se ve contandolo:
 *
 * La ventana de Hann se aplica SOLO EN ANALISIS -no hay ventana de
 * sintesis- y el solape es del 75 %, o sea cuatro copias desplazadas un
 * cuarto de ventana. La suma de esas cuatro copias vale EXACTAMENTE 2,0 en
 * todo el tramo (medido, rizado 0,0000 dB: es la propiedad de solapado-suma
 * de Hann a 75 %). Asi que para ganancia unidad hay que multiplicar por
 * 1/2,0 = 0,5.
 *
 * Lo que habia multiplicaba por 1,63309522/4 = 0,408274, o sea ganancia
 * total 0,816548 = -1,76 dB. Encender el reductor de ruido bajaba el audio
 * 1,76 dB, siempre, aunque la fuerza estuviera a cero.
 *
 * De donde salia el numero: sqrt(2,667) es la compensacion de POTENCIA de
 * Hann con solape 4x, que es otra cuenta -la buena para estimar cuanta
 * potencia de ruido deja pasar la ventana- y no la de reconstruir la señal.
 * El comentario de la cabecera ya avisaba de que "puede necesitar un ajuste
 * empirico"; no hacia falta ajustar nada a oido, el numero se deduce.
 *
 * Se deja como NR_SS_ATT_RATIO = 2,0 para que la cuenta de abajo
 * -ATT_RATIO * (1/NFRAME)- siga leyendose igual y de 0,5.
 */
#define NR_SS_ATT_RATIO 2.0f

/*
 * *** 01/09/2026: moved to TCM RAM *** - same reasoning as fft.c's own
 * TCM comment (freed main-RAM headroom for the widened waterfall;
 * none of these are DMA targets - this whole module runs as plain
 * CPU-computed DSP inside the AM/SSB audio chain).
 */
#define TCMRAM_BSS __attribute__((section(".tcmram")))

static float s_dat_stfft[2U * NR_SS_NSTFFT] TCMRAM_BSS; /* interleaved re/im, in place through FFT->gain->IFFT */
/*
 * Overlap-add state: the partial sums of the frames already processed, for
 * the next NR_SS_NFRAME - 1 output blocks (96 floats instead of the last four
 * whole frames, 512 floats, of which only these 96 values were ever read back).
 *
 *   s_ola[0..31]   frames t-3 + t-2 + t-1, for the next block
 *   s_ola[32..63]  frames t-2 + t-1,       for the block after
 *   s_ola[64..95]  frame t-1,              for the one after that
 *
 * Bit for bit what the four-frame version computed: every output sample is
 * still 0.0f + oldest + ... + newest, added in that order (the 0.0f start
 * matters: it turns a -0.0f oldest term into +0.0f, as the old running sum
 * did). See the end of nr_ss_process().
 */
static float s_ola[NR_SS_NSTFFT - NR_SS_HOP] TCMRAM_BSS;
static float s_sigt[NR_SS_NSTFFT] TCMRAM_BSS; /* input ring buffer */
#ifdef CHIP_APM32F407
/* APM32 build: the window, twiddle and bit-reversal tables precomputed in
 * flash (User/nr_ss_tables.h, generated by tools/gen_nr_tables.py with the
 * exact roundings nr_ss_init() gets on the radio, fused multiply-adds
 * included), to free 1,280 bytes of CCM. Same arrangement as fft.c's. */
#include "nr_ss_tables.h"
_Static_assert(NR_SS_TABLES_SIZE == NR_SS_NSTFFT, "regenerate User/nr_ss_tables.h for this NR_SS_NSTFFT");
#define s_win         k_nr_win
#define s_twiddle_cos k_nr_twiddle_cos
#define s_twiddle_sin k_nr_twiddle_sin
#define s_bitrev      k_nr_bitrev
#else
static float s_win[NR_SS_NSTFFT] TCMRAM_BSS; /* Hann window */
static float s_twiddle_cos[NR_SS_NSTFFT / 2U] TCMRAM_BSS;
static float s_twiddle_sin[NR_SS_NSTFFT / 2U] TCMRAM_BSS;
static uint16_t s_bitrev[NR_SS_NSTFFT] TCMRAM_BSS;
#endif
static uint32_t s_pt_w;   /* ring buffer write pointer into s_sigt */

/*
 * THE REAL FFT, APM32 BUILD - 14/11/2026.
 *
 * The frame is real, but the port kept the original's complex 128-point FFT with the imaginary part zeroed, through
 * nr_fft_transform()'s plain radix-2: measured on the radio at 192 kHz, the two transforms cost about 37,000 cycles
 * a block, two thirds of the noise reduction, which itself was 11 % of the CPU and what pulled the display below
 * its frame rate. CMSIS-DSP's arm_rfft_fast_f32() does the same 128-point transform of a real frame for a fraction
 * of that. The spectrum comes packed (X0 and X64 real, then X1..X63), so NR_BIN() reads bins 65..127 as the
 * conjugates of 63..1 they are, and the gains go on through the mean of each bin and its mirror (see the inverse
 * below). Not bit-exact with the complex path, which rounds differently: the difference was measured on the host
 * before this went in. The GD32 build keeps nr_fft_transform(): its flash cannot take the CMSIS code and tables.
 */
#ifdef CHIP_APM32F407
#define NR_RFFT 1
#else
#define NR_RFFT 0
#endif
#if NR_RFFT
#include "arm_math.h"
static arm_rfft_fast_instance_f32 s_rfft;
/* |re| and |im| of bin j (0..127) from the packed spectrum in the second half of s_dat_stfft. */
static inline void nr_bin(uint32_t j, float *ar, float *ai)
{
    const float *p = &s_dat_stfft[NR_SS_NSTFFT];
    uint32_t     k = (j <= NR_SS_NSTFFT / 2U) ? j : (NR_SS_NSTFFT - j);

    if (k == 0U)                     { *ar = fabsf(p[0]); *ai = 0.0f; }
    else if (k == NR_SS_NSTFFT / 2U) { *ar = fabsf(p[1]); *ai = 0.0f; }
    else                             { *ar = fabsf(p[2U * k]); *ai = fabsf(p[2U * k + 1U]); }
}
#define NR_BIN(j, ar, ai) nr_bin((j), &(ar), &(ai))
/* Bin j's magnitude, from the table nr_ss_process() fills once per frame (see "THE MAGNITUDES, ONCE"). */
#define NR_AMP(j, a) do { (a) = s_dat_stfft[(j)]; } while (0)
#else
#define NR_BIN(j, ar, ai) \
    do { (ar) = fabsf(s_dat_stfft[2U * (j)]); (ai) = fabsf(s_dat_stfft[2U * (j) + 1U]); } while (0)
#define NR_AMP(j, a)                                                                   \
    do {                                                                               \
        float ar_, ai_;                                                                \
        NR_BIN((j), ar_, ai_);                                                         \
        (a) = (ar_ > ai_) ? (0.96043457f * ar_ + 0.39782422f * ai_)                   \
                          : (0.96043457f * ai_ + 0.39782422f * ar_);                   \
    } while (0)
#endif
/*
 * LA GANANCIA DE CADA BIN, SUAVIZADA EN EL TIEMPO - 07/10/2026.
 *
 * *** De un aviso recibido: con la señal poco por encima del ruido se oye un
 * burbujeo fuerte, y se preguntaba si lo producia el reductor de ruido. ***
 *
 * La version anterior decidia la ganancia de cada bin desde cero en cada
 * cuadro. La FFT es de 128 con salto de 32 a 12 kHz, o sea TRESCIENTAS
 * SETENTA Y CINCO decisiones por segundo y por bin, cada una independiente de
 * la anterior. La magnitud de un bin que solo lleva ruido es Rayleigh y
 * fluctua un 52 % alrededor de su media, asi que los bins de ruido cruzan el
 * umbral arriba y abajo a cada cuadro: uno sobrevive a ganancia 1 durante 2,7
 * ms y al siguiente lo matan. Eso son rafagas de tono cortas en frecuencias
 * al azar, que es el ruido musical de toda la vida.
 *
 * NO SE HA PODIDO REPRODUCIR EL BURBUJEO FUERTE que se describia -ni en el
 * banco ni en el video de referencia, y el porque esta en sim/nr_burbujeo.c-. Asi
 * que esto NO entra por oido, entra porque las tres medidas salen mejores o
 * iguales y ninguna peor:
 *
 *                       vaiven que añade   quita ruido   voz a 6 dB
 *     antes                  +1,40 dB        -15,0 dB      -9,5 dB
 *     con esto               +0,69 dB        -15,5 dB      -9,5 dB
 *
 * El vaiven se queda en la mitad, el ruido se quita incluso un poco MEJOR y
 * la voz no se mueve ni una decima. No es un compromiso: es gratis. Si
 * hubiera costado reduccion, no se habria puesto sin una grabacion del efecto.
 *
 * LOS DOS NUMEROS. El cuadro dura 2,67 ms. Subir con 0,35 es una constante de
 * tiempo de ~6 ms -una silaba empieza en 10 ms, asi que no se pierde ningun
 * arranque de voz- y bajar con 0,10 son ~25 ms, bastante para que una rafaga
 * de ruido de un cuadro no se oiga como un pitido suelto. Probado tambien con
 * 0,50/0,30/0,15 por separado: ver la tabla de sim/nr_burbujeo.c.
 *
 * LO QUE NO SE HA PUESTO, y es a proposito: un SUELO espectral. Baja el
 * vaiven hasta +0,32 dB -la mitad otra vez- pero cuesta medio decibelio de
 * reduccion de ruido. Eso ya es un cambalache, y un cambalache no se elige
 * sin oido. Queda medido y escrito aqui para cuando haya una grabacion.
 */
/*
 * MAS LENTOS DESDE EL 11/10/2026, Y POR UNA MEDIDA.
 *
 * Eran 0,35 y 0,10 (ataque ~6 ms, caida ~25 ms). Con el umbral nuevo -que
 * se mide contra el suelo de ruido y por tanto se mueve mas- esos valores
 * dejaban mas BURBUJEO: el vaiven que mide sim/nr_burbujeo.c subia de
 * +0,90 dB a +1,20, por encima del tope que ese banco ya tenia puesto.
 *
 * El tope NO se ha subido. Subir un tope para que pase lo que mides es
 * dejar de medir. Lo que se hizo fue barrer el suavizado:
 *
 *     sube/baja     vaiven   picos   S/N de la voz
 *     0,35 / 0,10    +1,20   +2,70      +3,8 dB
 *     0,20 / 0,06    +0,92   +2,12      +3,6 dB
 *     0,12 / 0,04    +0,83   +1,67      +3,4 dB   <- este
 *     0,08 / 0,02    +1,42   +1,16      +3,0 dB
 *
 * Cuesta 0,4 dB de relacion senal/ruido y devuelve el burbujeo a por
 * debajo de donde estaba. Es el cambio bueno: el burbujeo es lo que hace
 * desagradable un reductor de ruido, y 0,4 dB no se oyen.
 *
 * (Y abajo del todo vuelve a subir el vaiven: con el ataque demasiado
 * lento la ganancia ya no sigue a las silabas y lo que aparece es otro
 * artefacto. No es monotono, por eso se barrio en vez de elegirlo.)
 */
#ifndef NR_SS_SUBE
#define NR_SS_SUBE 0.12f   /* ataque, ~22 ms */
#endif
#ifndef NR_SS_BAJA
#define NR_SS_BAJA 0.04f   /* caida,  ~67 ms */
#endif
static float s_g[NR_SS_NSTFFT] TCMRAM_BSS;

/*
 * ======================================================================
 * EL UMBRAL SE MIDE CONTRA EL RUIDO, NO CONTRA UNA CONSTANTE - 11/10/2026
 * ======================================================================
 *
 * Las dos frases son el mismo fallo, y es ESTE.
 *
 * El umbral era un numero ABSOLUTO: 10^(11*mando/4096), o sea once decadas
 * repartidas a lo largo del mando. Y la amplitud contra la que se compara
 * depende del nivel que llegue, que lo mueve el AGC, la banda y la
 * emisora. Asi que el punto bueno del mando SE MUEVE, y un mando calibrado
 * en absoluto no lo puede seguir.
 *
 * LO QUE SE MIDIO con el umbral viejo (sim/nr_curva.c, tono 12 dB sobre
 * ruido, midiendo la GANANCIA de relacion senal/ruido, que es lo unico que
 * dice si un reductor sirve de algo):
 *
 *     mando      S/N      gana
 *        50    13,8 dB   +0,0 dB   <- la posicion de fabrica: no hace nada
 *       500    15,4 dB   +1,6 dB
 *      1000    25,9 dB  +12,1 dB   <- la ventana buena
 *      1500    23,5 dB   +9,7 dB
 *      2000     0,5 dB  -13,3 dB   <- a partir de aqui DESTRUYE
 *      4095   -19,3 dB  -33,1 dB
 *
 * O sea: de 0 a 400 no hace nada, de 1800 para arriba rompe el audio, y la
 * ventana util es un 25% del recorrido. Tres cuartas partes del mando o no
 * sirven o hacen dano.
 *
 * AHORA EL UMBRAL ES k * SUELO, con el suelo medido por bin. El mando
 * elige k, o sea CUANTAS VECES por encima del ruido hay que estar para
 * pasar entero. Eso no se mueve con el nivel: si el AGC sube diez dB,
 * suben la senal y el suelo a la vez y k sigue queriendo decir lo mismo.
 *
 * Es la regla de siempre de este proyecto -medir contra la senal y no
 * contra una constante- y el NR era el unico modulo que la incumplia.
 */

/*
 * EL SUELO DE RUIDO DE CADA BIN.
 *
 * Baja deprisa y sube despacio, que es lo que distingue el suelo de una
 * silaba: en los huecos entre silabas el bin cae al ruido y el suelo lo
 * sigue en milisegundos; cuando llega voz, el suelo apenas se entera.
 *
 * Y SUBE SOLO DESDE LO QUE PARECE SUELO. Si amp esta muy por encima del
 * suelo que ya teniamos, eso no es ruido que ha subido: es senal, y
 * dejarla entrar en la cuenta haria que un tono sostenido se levantara su
 * propio suelo y acabara cortandose a si mismo. El tope de NR_PISO_SENAL
 * veces es lo que separa "el ruido ha subido" de "ha entrado algo".
 *
 * El escape lento de NR_PISO_FUGA existe para lo contrario: si el ruido de
 * la banda sube de verdad y se queda arriba, el suelo tiene que acabar
 * siguiendolo o el reductor se pondria a recortar todo. Es muy lento a
 * proposito.
 */
/* Con guarda, para que los bancos puedan barrerlos desde la linea de
 * ordenes. Sin ella, un -D no entra y el barrido sale DOCE VECES IGUAL sin
 * quejarse - pasó el 11/10/2026 y costo una ronda entera verlo. */
#ifndef NR_PISO_BAJA
#define NR_PISO_BAJA   0.25f    /* tau ~11 ms a 375 cuadros/s */
#endif
#ifndef NR_PISO_SUBE
#define NR_PISO_SUBE   0.02f    /* tau ~133 ms - medido, ver nr_curva */
#endif
#ifndef NR_PISO_FUGA
#define NR_PISO_FUGA   0.0002f  /* tau ~13 s: solo para ruido que sube de verdad */
#endif
#ifndef NR_PISO_SENAL
#define NR_PISO_SENAL  4.0f     /* por encima de esto, no es suelo: es senal */
#endif
#define NR_PISO_MINIMO 1.0e-12f /* para no dividir por cero antes de aprender */

static float s_piso[NR_SS_NSTFFT] TCMRAM_BSS;

/*
 * ======================================================================
 * Y EL SUELO TAMBIEN SE MIRA DE LADO - 11/10/2026
 * ======================================================================
 *
 * El suelo de arriba mira el TIEMPO: lo que ha valido este bin en los
 * ultimos milisegundos. Eso vale para la voz, que tiene huecos entre
 * silabas, y NO VALE PARA UN TONO SOSTENIDO: un bin que nunca calla no
 * ensena nunca su suelo, asi que el estimador acaba siguiendo al propio
 * tono y el tono se corta a si mismo.
 *
 * Medido, con un tono de 1500 Hz puesto 25 segundos (CW largo, o una
 * portadora), mirando la relacion tono/ruido:
 *
 *               mando util    tono/ruido
 *     viejo        600         +15,4 dB
 *     solo tiempo 1400          -9,6 dB    <- se lo come
 *
 * Veinticinco dB de diferencia. Eso no se entrega.
 *
 * LO QUE LOS SEPARA NO ES EL TIEMPO, ES LA FORMA. Un suelo de ruido es
 * LISO en frecuencia; un tono es un PICO estrecho. Asi que se mira
 * tambien de lado: el minimo de los bins de alrededor. Para un tono, los
 * vecinos estan en el ruido y lo delatan; para la voz, los armonicos
 * estan tan juntos a esta resolucion -93,8 Hz por bin contra 120 Hz de
 * separacion- que el minimo de al lado no baja gran cosa.
 *
 * Y SE QUEDA EL MENOR DE LOS DOS. Un suelo mas bajo es un umbral mas
 * bajo, o sea menos atenuacion: ante la duda, no cortar. Con eso cada
 * caso lo resuelve el estimador que acierta:
 *
 *     tono sostenido -> manda el de al lado (el de tiempo esta inflado)
 *     voz con huecos -> manda el del tiempo (el de al lado esta inflado)
 *
 * El minimo de al lado se saca por grupos de CUATRO bins y se mira SOLO
 * el grupo propio: una ventana de 4 bins -375 Hz- con 32 floats de sitio
 * y sin buscar nada.
 *
 * (Aqui ponia "ocho bins y los dos vecinos, una ventana de 24 bins, 2,2
 * kHz", que es lo que dicen los numeros de mas arriba y NO lo que se
 * compila: NR_VECINOS vale 0 y su #if de nr_ss_process() es codigo
 * muerto. Corregido el texto el 27/10/2026, y NO el codigo, por lo que
 * sigue.)
 */

/*
 * ======================================================================
 * LO QUE CUESTA ESTA ELECCION, MEDIDO LAS DOS VECES - 27/10/2026.
 * ======================================================================
 *
 * Se probo a poner los 8 bins con vecinos que el texto describia, y NO es
 * estrictamente mejor: es un canje, y conviene que este escrito para que
 * nadie lo vuelva a mover a ciegas.
 *
 * LO QUE SE GANA con 8 y vecinos: deja de comerse los tonos sostenidos
 * -CW largo, una portadora, un heterodino, la marca de RTTY o PSK-. Con
 * un tono de 2.000 sobre ruido de 200 y el mando en el tope:
 *
 *                       4 sin vecinos      8 con vecinos
 *      700 Hz           tono -10,3 dB      tono -1,4 dB
 *     1500 Hz           tono  -6,4         tono -0,8
 *     2500 Hz           tono -15,7         tono -0,8
 *
 * y en el peor de esos casos la relacion señal/ruido con 4 sin vecinos
 * sale NEGATIVA, o sea que encender el reductor es peor que no encenderlo.
 *
 * LO QUE SE PIERDE: la ventana ancha baja el suelo estimado -es el minimo
 * sobre 24 bins en vez de 4-, asi que se atenua menos. Con 8 y vecinos,
 * sim/nr_burbujeo.c pasa de quitar ruido de verdad en el centro del mando
 * a quitar solo 4,7 dB, y su criterio de aprobado SALTA.
 *
 * O sea que el mando esta calibrado para esta ventana: NR_K_DECADAS sale
 * del barrido de sim/nr_curva.c, hecho con 4 y sin vecinos. Cambiar la
 * ventana sin rehacer esa calibracion mueve el defecto de sitio en vez de
 * quitarlo, y rehacerla se hace escuchando, no en el simulador.
 *
 * QUEDA ESCRITO EL DEFECTO QUE SI HAY: en el tercio alto del mando este
 * reductor se come un tono sostenido. Con CW o con un digital, bajalo.
 */
/*
 * ======================================================================
 * *** ESTO COMPILABA 4 Y 0, O SEA NO LO QUE DICE EL COMENTARIO DE ARRIBA
 * NI LO QUE MIDEN LOS NUMEROS QUE LO JUSTIFICAN - 27/10/2026, cuarta
 * pasada de la revision de espejos y armonicos. ***
 * ======================================================================
 *
 * Con 4 y sin vecinos la ventana del "minimo de al lado" son 375 Hz en
 * vez de los 2,2 kHz que el comentario describe, y el #if NR_VECINOS de
 * nr_ss_process() era CODIGO MUERTO. O sea que el mecanismo entero
 * -mirar el suelo de los vecinos para no confundir un tono sostenido con
 * ruido- no estaba puesto.
 *
 * Y se nota en lo que mas duele: un tono sostenido -CW largo, una
 * portadora, un heterodino, la marca de RTTY o PSK- cae dentro de su
 * propio grupo de 4 bins, levanta el suelo de ese grupo, y el reductor lo
 * toma por ruido y se lo come. Medido con un tono de 2.000 sobre ruido de
 * 200, 25 segundos:
 *
 *                              4 bins sin vecinos   8 bins con vecinos
 *     700 Hz, mando 4095       tono -10,3 dB        tono  -1,4 dB
 *                              S/R    +2,6             S/R  +7,6
 *     1500 Hz, mando 4095      tono  -6,4            tono  -0,8
 *                              S/R    +3,5             S/R +11,1
 *     2500 Hz, mando 4095      tono -15,7            tono  -0,8
 *                              S/R    -0,4 <- PEOR     S/R  +9,3
 *
 * En el tercio alto del mando el reductor dejaba de reducir y empezaba a
 * destruir: a 2.500 Hz la relacion señal/ruido salia NEGATIVA, o sea que
 * encenderlo era peor que no encenderlo. Con la configuracion que el
 * fichero describe el tono no se mueve mas de 1,4 dB en ningun caso.
 *
 * sim/nr_curva.c ya enseñaba el sintoma -"mando 4095 -> nivel del tono
 * -10,1 dB"- pero ninguno de sus criterios de aprobado lo miraba, asi que
 * el banco pasaba en verde.
 */
#ifndef NR_GRUPO
#define NR_GRUPO      4U
#endif
#ifndef NR_VECINOS
#define NR_VECINOS    0
#endif
#define NR_GRUPOS     (NR_SS_NSTFFT / NR_GRUPO)
static float s_gmin[NR_GRUPOS] TCMRAM_BSS;

/* Cuantas veces por encima del suelo hay que estar para pasar entero. Lo
 * pone el mando; ver nr_ss_set_strength(). */
/* Decadas de k que barre el mando: k va de 1 a 10^NR_K_DECADAS. El numero
 * sale del barrido de sim/nr_curva.c, no de una eleccion mia. */
#ifndef NR_K_DECADAS
#define NR_K_DECADAS 2.4f
#endif
static float s_k = 1.0f;
static uint8_t s_enabled; /* master on/off - see nr_ss_set_enabled()'s comment in nr_ss.h */

#ifndef CHIP_APM32F407
/*
 * Local sin/cos, same Bhaskara-I approximation fft.c/gd32_i2s.c
 * already use in this project (see fft.c's "reimplemented locally to
 * keep the two modules decoupled" note - same reasoning applied here
 * rather than sharing code across modules). Only used at init (window
 * + twiddle table generation), never in the per-block hot path.
 * (Not in the APM32 build, which has the tables as constants: see
 * tools/gen_nr_tables.py, which reproduces this arithmetic as compiled.)
 */
static float sinf_approx_local(float x)
{
    float pi = 3.14159265358979f;
    float x2;
    int negate = 0;

    while (x > pi) { x -= 2.0f * pi; }
    while (x < -pi) { x += 2.0f * pi; }
    if (x < 0.0f) { x = -x; negate = 1; }
    x2 = (16.0f * x * (pi - x)) / (5.0f * pi * pi - 4.0f * x * (pi - x));
    return negate ? -x2 : x2;
}

static float cosf_approx_local(float x)
{
    return sinf_approx_local(x + (3.14159265358979f / 2.0f));
}
#endif /* !CHIP_APM32F407 */

/*
 * Self-contained radix-2, decimation-in-time, in-place complex FFT
 * (NR_SS_NSTFFT=128 points - fixed size, no general-N support needed
 * here). `re_im` is NR_SS_NSTFFT complex samples, interleaved
 * re0,im0,re1,im1,... `inverse` runs the SAME forward transform on
 * the conjugated input then conjugates the result back - the classic
 * "IFFT via FFT" trick (avoids needing a second twiddle table for the
 * reverse direction), same approach the original ESP-DSP-based code
 * used (negate imaginary before and after dsps_fft2r_fc32()).
 */
#if !NR_RFFT
static void nr_fft_transform(float *re_im, int inverse)
{
    uint32_t n;
    uint32_t stage_size;

    if (inverse) {
        for (n = 0; n < NR_SS_NSTFFT; n++) { re_im[2U * n + 1U] = -re_im[2U * n + 1U]; }
    }

    /* Bit-reversal permutation, in place. */
    for (n = 0; n < NR_SS_NSTFFT; n++) {
        uint32_t r = s_bitrev[n];
        if (r > n) {
            float tr = re_im[2U * n];
            float ti = re_im[2U * n + 1U];
            re_im[2U * n]      = re_im[2U * r];
            re_im[2U * n + 1U] = re_im[2U * r + 1U];
            re_im[2U * r]      = tr;
            re_im[2U * r + 1U] = ti;
        }
    }

    /* Iterative Cooley-Tukey butterflies, stage_size doubling each
     * pass (2, 4, 8, ..., NR_SS_NSTFFT). */
    for (stage_size = 2U; stage_size <= NR_SS_NSTFFT; stage_size <<= 1U) {
        uint32_t half = stage_size >> 1U;
        uint32_t tw_step = NR_SS_NSTFFT / stage_size;
        uint32_t group;

        /* El giro FUERA del bucle de grupos: lo elige k, no el grupo, asi
         * que con el nido al reves se leia la tabla una vez por mariposa en
         * vez de una por columna. Medido: 24 -> 21 instrucciones y 6 -> 4
         * cargas por mariposa. Es el mismo arreglo que lleva fft.c desde el
         * 19/10/2026 y que a este fichero no llego.
         *
         * Bit a bit igual: son las mismas mariposas con las mismas entradas;
         * lo unico que cambia es cuantas veces se lee s_twiddle_*. */
        {
            uint32_t k;
            for (k = 0; k < half; k++) {
                uint32_t ti = k * tw_step;
                float wr = s_twiddle_cos[ti];
                float wi = s_twiddle_sin[ti];
                for (group = 0; group < NR_SS_NSTFFT; group += stage_size) {
                uint32_t a = group + k;
                uint32_t b = a + half;
                float br = re_im[2U * b];
                float bi = re_im[2U * b + 1U];
                float tr = wr * br - wi * bi;
                float tv = wr * bi + wi * br;
                float ar = re_im[2U * a];
                float ai = re_im[2U * a + 1U];

                re_im[2U * a]       = ar + tr;
                re_im[2U * a + 1U] = ai + tv;
                re_im[2U * b]       = ar - tr;
                re_im[2U * b + 1U] = ai - tv;
                }
            }
        }
    }

    if (inverse) {
        for (n = 0; n < NR_SS_NSTFFT; n++) { re_im[2U * n + 1U] = -re_im[2U * n + 1U]; }
    }
}
#endif /* !NR_RFFT */

void nr_ss_init(void)
{
#ifndef CHIP_APM32F407   /* APM32: the tables are constants, see nr_ss_tables.h */
    uint32_t n;
    uint32_t bits = 0U;
    uint32_t size = NR_SS_NSTFFT;

    /* Exact log2(NR_SS_NSTFFT) by counting shifts (guaranteed a power
     * of two - NR_SS_BLOCK_SAMPLES=32 * NR_SS_NFRAME=4). */
    while (size > 1U) { size >>= 1U; bits++; }

    for (n = 0; n < NR_SS_NSTFFT / 2U; n++) {
        float angle = -2.0f * 3.14159265358979f * (float)n / (float)NR_SS_NSTFFT;
        s_twiddle_cos[n] = cosf_approx_local(angle);
        s_twiddle_sin[n] = sinf_approx_local(angle);
    }

    for (n = 0; n < NR_SS_NSTFFT; n++) {
        uint32_t v = n;
        uint32_t r = 0U;
        uint32_t b;
        for (b = 0; b < bits; b++) {
            r = (r << 1U) | (v & 1U);
            v >>= 1U;
        }
        s_bitrev[n] = (uint16_t)r;
    }

    /* Hann window, same t=i/N (not i/(N-1)) as the original. */
    for (n = 0; n < NR_SS_NSTFFT; n++) {
        float t = (float)n / (float)NR_SS_NSTFFT;
        s_win[n] = 0.5f - 0.5f * cosf_approx_local(2.0f * 3.14159265358979f * t);
    }
#endif
#if NR_RFFT
    (void)arm_rfft_fast_init_128_f32(&s_rfft);
#endif

    memset(s_sigt, 0, sizeof(s_sigt));
    memset(s_ola, 0, sizeof(s_ola));
    s_pt_w = 0U;
    s_enabled = 0U; /* off by default - matches nr_ss_set_strength(0)'s
                      * own default below, and the project owner's ask
                      * that OFF be a genuine, distinct switch. */
    {   /* la ganancia arranca en 1: transparente hasta que haya medidas */
        uint32_t q;
        for (q = 0U; q < NR_SS_NSTFFT; q++) { s_g[q] = 1.0f; }
    }
    /* Y el suelo a cero, que es la senal de "todavia no se ha medido": el
     * primer cuadro lo siembra con lo que haya. Ver nr_ss_process(). */
    memset(s_piso, 0, sizeof(s_piso));
    memset(s_gmin, 0, sizeof(s_gmin));
    nr_ss_set_strength(0U); /* see its own comment */
}

/*
 * ======================================================================
 * Y EL OLVIDO, QUE NO EXISTIA - 24/10/2026.
 * ======================================================================
 *
 * Encontrado en la revision a fondo pedida por el usuario.
 *
 * Este modulo lleva CUATRO trozos de estado con memoria larga -la cola de
 * entrada, los cuatro cuadros del solapado-suma, el suelo de ruido por bin
 * y la ganancia suavizada- y solo se ponian a cero en nr_ss_init(), que
 * corre UNA vez en el arranque. Y demod_am.c se salta el modulo entero
 * mientras esta apagado, que es justo lo que su cabecera promete como
 * ventaja.
 *
 * O sea que al volver a encenderlo, la reconstruccion suma cuatro cuadros
 * de los que TRES son de la ultima vez: salen unos diez milisegundos de
 * audio de otro momento, a nivel completo, y detras otros ciento y pico de
 * ganancias calculadas contra un suelo de ruido que se midio en otra banda,
 * con otra ganancia y a lo mejor hace media hora.
 *
 * La regla esta escrita con todas sus letras en nb.h -"hay que llamarlo
 * cuando cambia algo que altera el nivel de entrada de golpe"- y el
 * supresor de impulsos tiene su nb_reset() llamado desde tres sitios por
 * ese motivo. El reductor de ruido, que esta en la misma cadena y tiene
 * memoria mucho mas larga, no tenia a quien llamar.
 *
 * Esto NO toca las tablas -giros, ventana, inversion de bits-, que son
 * funcion de constantes y no caducan, ni la fuerza elegida, que es una
 * preferencia del usuario y no un estado de la señal.
 *
 * (The overlap-add state is now the s_ola accumulator - partial sums of the
 * last three frames - rather than four whole frames; same reset, same effect.)
 */
void nr_ss_reset(void)
{
    uint32_t q;

    memset(s_sigt, 0, sizeof(s_sigt));
    memset(s_ola, 0, sizeof(s_ola));
    s_pt_w   = 0U;
    for (q = 0U; q < NR_SS_NSTFFT; q++) { s_g[q] = 1.0f; }
    memset(s_piso, 0, sizeof(s_piso));
    memset(s_gmin, 0, sizeof(s_gmin));
}

void nr_ss_set_enabled(uint8_t on)
{
    uint8_t nuevo = on ? 1U : 0U;

    /* Al ENCENDERLO, lo que haya dentro es de la ultima vez. Ver
     * nr_ss_reset(). Apagarlo no hace falta limpiarlo: el modulo no corre. */
    if (nuevo && !s_enabled) { nr_ss_reset(); }
    s_enabled = nuevo;
}

uint8_t nr_ss_get_enabled(void)
{
    return s_enabled;
}

void nr_ss_set_strength(uint16_t v)
{
    /*
     * EL MANDO ELIGE k: CUANTAS VECES POR ENCIMA DEL RUIDO HAY QUE ESTAR.
     *
     * Antes este mando barria ONCE DECADAS de umbral absoluto
     * -10^(11*v/4096)- y por eso tres cuartas partes de su recorrido no
     * servian o hacian dano. Ahora elige un numero con sentido fisico, y
     * el recorrido entero cae en una zona utilizable.
     *
     * El rango se decidio MIDIENDO, no eligiendolo: ver sim/nr_curva.c,
     * que barre el mando y saca la ganancia de relacion senal/ruido en
     * cada posicion. NR_K_MAX es el unico numero que hay que tocar si
     * alguna vez hay que estirar o encoger esa zona.
     *
     * v=0 -> k=1 -> un bin que este en su propio suelo pasa entero, o sea
     * transparente de verdad y no "transparente si la amplitud es menor
     * que uno", que es lo que pasaba antes y dependia del nivel.
     */
    float x;

    if (v > 4095U) { v = 4095U; }
    x = NR_K_DECADAS * (float)v / 4095.0f;
    s_k = expf(x * 2.302585093f);   /* 10^x; ver por que expf mas abajo */
}

void nr_ss_process(float *audio, uint32_t n)
{
    uint32_t i;

    /*
     * LA LONGITUD NO SE PIDE POR EDUCACION - 11/10/2026.
     *
     * El firmware ya tenia ese fallo visto y corregido el 01/09/2026 -ver
     * nr_ss_process_chunks() en demod_am.c-, pero el aviso puso el dedo en
     * algo que seguia abierto: esta funcion ACEPTABA cualquier `n`. La
     * cabecera decia "n tiene que valer NR_SS_BLOCK_SAMPLES siempre" y
     * nada lo hacia cumplir.
     *
     * Y no es un detalle de estilo. El solapamiento de abajo da por hecho
     * que el salto entre bloques es NR_SS_NSTFFT/NR_SS_NFRAME = 32: con
     * otra `n` el indice se sale de la fila y lee la de al lado. Con el NR
     * APAGADO -donde esto tiene que ser transparente- medido en
     * sim/nr_curva.c: llamando con 128 en vez de con 32 el ruido sale
     * multiplicado por 9,4 y del tono queda el 21,6%.
     *
     * Que es justo lo que llevaba pasando en ese banco desde el 25/09, sin
     * que nadie lo viera, porque un banco que viola el contrato no se
     * queja: devuelve numeros. Y de ahi salieron las respuestas a "el NR
     * se ha vuelto casi inapreciable".
     *
     * Asi que se rechaza, igual que hace rtty_process() con su propio
     * tamano de bloque por la misma razon y desde el mismo dia. Rechazar
     * es DEJAR EL AUDIO COMO ESTABA, que es lo seguro: el que llama se
     * queda sin reduccion de ruido, no con el audio roto.
     */
    if (n != (uint32_t)NR_SS_BLOCK_SAMPLES) {
        return;
    }

    /* 1. Feed the ring buffer with this block's raw input. */
    for (i = 0; i < n; i++) {
        s_sigt[s_pt_w] = audio[i];
        s_pt_w++;
        if (s_pt_w == NR_SS_NSTFFT) { s_pt_w = 0U; }
    }

    /* 2. Window the last NR_SS_NSTFFT samples (oldest-to-newest,
     * starting right after where step 1 left the write pointer - same
     * read-then-advance walk the original used) into the complex FFT
     * buffer, imaginary part zeroed (real input). */
#if NR_RFFT
    /* APM32: the real frame in the first half, its packed spectrum in the second (see NR_RFFT). */
    for (i = 0; i < NR_SS_NSTFFT; i++) {
        s_dat_stfft[i] = s_sigt[s_pt_w] * s_win[i];
        s_pt_w++;
        if (s_pt_w == NR_SS_NSTFFT) { s_pt_w = 0U; }
    }

    arm_rfft_fast_f32(&s_rfft, s_dat_stfft, &s_dat_stfft[NR_SS_NSTFFT], 0U);

    /*
     * THE MAGNITUDES, ONCE - 14/11/2026. Both passes below took each bin's magnitude through nr_bin() and its
     * branches, 256 times a frame: on the radio that accessor was about 40 % of nr_ss_process(). Bins 0..64 are
     * worked out once here, with the same expression as before, and 65..127 copied from their mirrors 63..1 (the
     * same values nr_bin() gave them), into the first half of s_dat_stfft, which the forward transform left free
     * until the inverse writes the new frame there. Same numbers, so the same gains, to the bit.
     */
    {
        uint32_t k;

        for (k = 0U; k <= NR_SS_NSTFFT / 2U; k++) {
            float ar, ai;

            NR_BIN(k, ar, ai);
            s_dat_stfft[k] = (ar > ai) ? (0.96043457f * ar + 0.39782422f * ai)
                                       : (0.96043457f * ai + 0.39782422f * ar);
        }
        for (; k < NR_SS_NSTFFT; k++) { s_dat_stfft[k] = s_dat_stfft[NR_SS_NSTFFT - k]; }
    }
#else
    for (i = 0; i < NR_SS_NSTFFT; i++) {
        s_dat_stfft[2U * i]      = s_sigt[s_pt_w] * s_win[i];
        s_dat_stfft[2U * i + 1U] = 0.0f;
        s_pt_w++;
        if (s_pt_w == NR_SS_NSTFFT) { s_pt_w = 0U; }
    }

    nr_fft_transform(s_dat_stfft, 0);
#endif

    /* 3. Spectral subtraction. amp uses the same alpha-max-beta-min
     * style weighted sum of |re|/|im| as the original (and this
     * project's own AM envelope-adjacent code elsewhere) - cheaper
     * than a true sqrt(re^2+im^2) magnitude and accurate enough for a
     * noise-floor gain decision. g clamped to <=1.0: this can only
     * ATTENUATE a bin, never boost it. */
    /*
     * PRIMERA PASADA: el minimo de cada grupo de ocho bins, que es de
     * donde sale el suelo "de al lado". Se recalcula la magnitud en vez
     * de guardarla: son ocho operaciones por bin contra 512 bytes de TCM,
     * y en TCM quedan 1.264.
     */
    for (i = 0; i < NR_GRUPOS; i++) {
        uint32_t b;
        float mn = 1.0e30f;
        for (b = 0U; b < NR_GRUPO; b++) {
            uint32_t j = i * NR_GRUPO + b;
            float a;
            NR_AMP(j, a);
            if (a < mn) { mn = a; }
        }
        s_gmin[i] = mn * (1.0f / (float)NR_SS_NSTFFT);
    }

    for (i = 0; i < NR_SS_NSTFFT; i++) {
        float amp;
        float g;

        NR_AMP(i, amp);
        amp *= (1.0f / (float)NR_SS_NSTFFT);

        /*
         * EL SUELO DE ESTE BIN, antes de decidir nada. Ver su comentario
         * largo arriba.
         *
         * La primera vez se SIEMBRA con lo que haya: arrancar desde cero y
         * dejar que suba a su ritmo serian dos segundos largos de reductor
         * sin enterarse de nada, y sembrar no cuesta una linea mas que
         * comprobarlo.
         */
        if (s_piso[i] <= 0.0f) {
            s_piso[i] = amp;
        } else if (amp < s_piso[i]) {
            s_piso[i] += NR_PISO_BAJA * (amp - s_piso[i]);
        } else if (amp < NR_PISO_SENAL * s_piso[i]) {
            s_piso[i] += NR_PISO_SUBE * (amp - s_piso[i]);
        } else {
            /* Muy por encima: es senal. El suelo solo se permite la fuga
             * lenta, por si el ruido de la banda ha subido de verdad. */
            s_piso[i] += NR_PISO_FUGA * (amp - s_piso[i]);
        }

        /*
         * Y LA GANANCIA, contra k veces el suelo en vez de contra un
         * numero fijo. Un bin que este k veces por encima de su propio
         * ruido pasa entero; por debajo se atenua en proporcion, igual que
         * antes. Lo unico que cambia es contra QUE se compara - y eso es
         * justamente lo que hacia que el mando no se pudiera ajustar.
         */
        {
            /* El de al lado: el grupo propio y los dos vecinos. */
            uint32_t gr = i / NR_GRUPO;
            float lado = s_gmin[gr];
            float suelo;
            float umbral;

#if NR_VECINOS
            if (gr > 0U && s_gmin[gr - 1U] < lado) { lado = s_gmin[gr - 1U]; }
            if (gr + 1U < NR_GRUPOS && s_gmin[gr + 1U] < lado) { lado = s_gmin[gr + 1U]; }
#endif

            /* Y se queda el MENOR: ante la duda, no cortar. */
            suelo = (s_piso[i] < lado) ? s_piso[i] : lado;

            umbral = s_k * suelo;
            if (umbral < NR_PISO_MINIMO) { umbral = NR_PISO_MINIMO; }
            g = amp / umbral;
        }
        if (g > 1.0f) { g = 1.0f; }
        /* ataque rapido para no comerse el arranque de una silaba, caida
         * lenta para que una rafaga de ruido de un cuadro no suene como un
         * pitido suelto. Ver el comentario de NR_SS_SUBE/BAJA. */
        if (g > s_g[i]) { s_g[i] += NR_SS_SUBE * (g - s_g[i]); }
        else            { s_g[i] += NR_SS_BAJA * (g - s_g[i]); }
#if !NR_RFFT
        g = s_g[i];
        s_dat_stfft[2U * i]      *= g;
        s_dat_stfft[2U * i + 1U] *= g;
#endif
    }

#if NR_RFFT
    /* The gains on the packed spectrum. Bin k and bin 128 - k carry conjugate values but can get different gains
     * (their groups' minima differ); the complex inverse FFT kept only the real part of the result, which is what
     * applying their mean to both gives. Then the inverse, which already scales by 1/N: the new frame lands in the
     * first NR_SS_NSTFFT floats, as below. */
    {
        float   *p = &s_dat_stfft[NR_SS_NSTFFT];
        uint32_t k;

        p[0] *= s_g[0];
        p[1] *= s_g[NR_SS_NSTFFT / 2U];
        for (k = 1U; k < NR_SS_NSTFFT / 2U; k++) {
            float ge = 0.5f * (s_g[k] + s_g[NR_SS_NSTFFT - k]);

            p[2U * k]      *= ge;
            p[2U * k + 1U] *= ge;
        }
        arm_rfft_fast_f32(&s_rfft, p, s_dat_stfft, 1U);
    }
#else
    nr_fft_transform(s_dat_stfft, 1);

    /* The new frame: real part scaled by 1/N, packed into the first
     * NR_SS_NSTFFT floats of s_dat_stfft (free until the next call). In
     * place is safe: element i is read from 2i, which nothing has written
     * yet. Stored, not folded into the sums below, so the compiler cannot
     * fuse the scaling into an addition: same roundings as before. */
    for (i = 0; i < NR_SS_NSTFFT; i++) {
        s_dat_stfft[i] = s_dat_stfft[2U * i] * (1.0f / (float)NR_SS_NSTFFT);
    }
#endif

    /* 4. Overlap-add (same frame alignment as the original's #if Nframe==4
     * branch): this block is the partial sum of the three older frames plus
     * the first hop of the new one. n == NR_SS_HOP, checked above. */
    for (i = 0; i < n; i++) {
        float sum = s_ola[i] + s_dat_stfft[i];

        audio[i] = sum * NR_SS_ATT_RATIO * (1.0f / (float)NR_SS_NFRAME);
    }
    /* Then the accumulator moves one hop: each partial sum takes the next
     * slice of the new frame, and the last slice starts a fresh sum from
     * 0.0f. Ascending i reads s_ola[i + NR_SS_HOP] before it is overwritten. */
    for (i = 0; i < NR_SS_NSTFFT - 2U * NR_SS_HOP; i++) {
        s_ola[i] = s_ola[i + NR_SS_HOP] + s_dat_stfft[i + NR_SS_HOP];
    }
    for (; i < NR_SS_NSTFFT - NR_SS_HOP; i++) {
        s_ola[i] = 0.0f + s_dat_stfft[i + NR_SS_HOP];
    }
}
