/*
 * ======================================================================
 * EL BOTON DE RUIDO ENCENDIDO Y APAGADO MUCHAS VECES - 12/11/2026
 * ======================================================================
 *
 * Aviso de un usuario: dandole al boton de Ruido unas cuantas veces el
 * audio se queda MUDO, y solo vuelve apagando y encendiendo la radio.
 * Y el dato que lo decide todo: EN FM ANCHA SI HAY AUDIO. Volviendo a
 * cualquier otro modo, sigue mudo.
 *
 * FM ancha lleva su propia cadena y su propio pico de automatico de
 * ganancia -s_wfm_agc_peak-; todo lo demas pasa por demod_am_process_raw()
 * y comparte s_agc_peak. O sea que lo que se queda pegado vive en esta
 * cadena, sobrevive a un cambio de modo, y solo lo limpia demod_am_init().
 *
 * Este banco mete una portadora de verdad por la entrada de verdad y
 * conmuta el boton como lo conmuta un dedo: cada tantos bloques, y en
 * FASES DISTINTAS, porque el aviso dice "aleatoriamente" y una sola fase
 * mediria una sola alineacion entre el dedo y el cuadro del reductor.
 *
 * LO QUE SE EXIGE es lo unico que el usuario nota: que despues de cada
 * conmutacion el audio VUELVA. No se mide cuanto reduce el ruido -de eso
 * ya se encarga sim/sam_nr.c-, se mide que siga habiendo radio.
 */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "demod_am.h"
#include "sdr_rx.h"
#include "demod_stubs.h"
#include "nr_ss.h"

#define NBLK    SDR_RX_BLOCK_SAMPLES
#define F_MOD   1000.0
#define M_PROF  0.6
#define F_PORT  50.0

static int fallos;

static double g_fi, g_t, g_fs;
static uint32_t sem;
static double ruidito(void)
{
    sem = sem * 1103515245u + 12345u;
    return (double)((int32_t)(sem >> 8) % 20001 - 10000) / 10000.0;
}

static void bloque(int16_t *raw, double amp, double ruido)
{
    uint32_t n;
    for (n = 0; n < (uint32_t)NBLK; n++) {
        double env = 1.0 + M_PROF * sin(2.0 * M_PI * F_MOD * g_t / g_fs);
        double i = amp * env * cos(g_fi) + amp * ruido * ruidito();
        double q = amp * env * sin(g_fi) + amp * ruido * ruidito();
        g_fi += 2.0 * M_PI * F_PORT / g_fs;
        if (g_fi > M_PI) { g_fi -= 2.0 * M_PI; }
        g_t += 1.0;
        if (i >  32767.0) { i =  32767.0; }
        if (i < -32768.0) { i = -32768.0; }
        if (q >  32767.0) { q =  32767.0; }
        if (q < -32768.0) { q = -32768.0; }
        raw[2U * n]      = (int16_t)lrint(i);
        raw[2U * n + 1U] = (int16_t)lrint(q);
    }
}

/* El valor eficaz del audio de UN bloque, por el mismo sitio por el que
 * sale hacia el codec. */
static double rms_bloque(void)
{
    const int16_t *sal = cons_audio();
    double e = 0.0;
    uint32_t n;
    for (n = 0; n < (uint32_t)NBLK; n++) {
        double y = (double)sal[2U * n];
        e += y * y;
    }
    return sqrt(e / (double)NBLK);
}

/* Mueve `n` bloques y devuelve el valor eficaz MAXIMO de los ultimos
 * `cola`: si el audio vuelve en algun momento de esa ventana, vuelve. */
static double corre(int16_t *raw, int n, int cola)
{
    double mx = 0.0;
    int b;
    for (b = 0; b < n; b++) {
        double r;
        bloque(raw, 6000.0, 0.05);
        demod_am_process_raw(raw);
        r = rms_bloque();
        if (b >= n - cola && r > mx) { mx = r; }
    }
    return mx;
}

/*
 * Un modo, `vueltas` conmutaciones, empezando `fase` bloques despues del
 * asentamiento para barrer la alineacion.
 */
static void uno(const char *nombre, demod_mode_t modo, int fase)
{
    static int16_t raw[2 * NBLK];
    double base, peor = 1.0e30;
    int v, peor_v = -1;
    uint8_t nr = 0U;

    demod_am_init();
    demod_am_set_mode(modo);
    nr_ss_set_enabled(0U);
    nr_ss_set_strength(2000U);
    cons_captura_tramas((uint32_t)NBLK);
    g_fs = (double)demod_am_get_active_fs_hz();
    g_fi = 0.0; g_t = 0.0; sem = 7777u;

    /* Asentamiento + la fase de este pase. */
    base = corre(raw, 400 + fase, 100);
    if (base < 1.0) {
        printf("  MAL: %s no da audio ni antes de tocar el boton\n", nombre);
        fallos++;
        return;
    }

    for (v = 0; v < 12; v++) {
        double r;
        nr = nr ? 0U : 1U;
        nr_ss_set_enabled(nr);
        /* 150 bloques = 400 ms: lo que tarda un dedo entre pulsacion y
         * pulsacion. La cola son los ultimos 60, ya pasado el silencio de
         * asentamiento de 50 bloques. */
        r = corre(raw, 150, 60);
        if (r < peor) { peor = r; peor_v = v + 1; }
    }

    printf("  %-5s fase %2d  base %7.1f   peor tras conmutar %7.1f  (%.0f%%, pulsacion %d)\n",
           nombre, fase, base, peor, 100.0 * peor / base, peor_v);

    /* El audio tiene que seguir ahi. Un 25% del nivel de partida es muy
     * generoso -el automatico devuelve el nivel- y aun asi caza el mudo. */
    if (peor < 0.25 * base) {
        printf("  MAL: en %s el audio se queda mudo tras la pulsacion %d\n", nombre, peor_v);
        fallos++;
    }
}

/*
 * ======================================================================
 * QUE NINGUNA DISCONTINUIDAD DE LA CADENA PUEDA DEJARLA MUDA
 * ======================================================================
 *
 * El pico del automatico es el divisor de la ganancia -AGC_TARGET/pico-,
 * el ataque es instantaneo y la caida es lenta: 0,7 s de constante de
 * tiempo en el perfil lento. Era el sospechoso natural del aviso, porque
 * NADIE lo pone en su sitio fuera del arranque -ni el cambio de modo, ni
 * el boton de Ruido- mientras que FM ancha si se olvida del suyo al
 * entrar, que es justo la asimetria que el aviso describe.
 *
 * ESTE BANCO LO DESCARTA, Y CON UN NUMERO. Se mete el peor chasquido que
 * la radio puede fabricar -un bloque entero a tope de escala del
 * conversor- y se cuenta cuantos bloques tarda el audio en volver a la
 * mitad de su nivel. Como la entrada esta acotada por el conversor, el
 * pico no puede subir mas que eso, y lo que sale es del orden de 110
 * bloques: 300 ms. Molesto, no mudo. Una radio que solo vuelve apagandola
 * no es esto.
 *
 * Y SE QUEDA COMO VIGILANTE. Si alguna vez alguien toca el automatico o
 * sus constantes de relajacion y lo convierte en algo que SI se queda
 * pegado, este numero se dispara y el banco lo dice.
 *
 * *** Y UNA COSA QUE SE PROBO Y NO VALE, escrita para que no se vuelva a
 * intentar: poner el pico en AGC_TARGET al vuelo -"olvidar" como hace el
 * arranque- EMPEORA las cosas. Medido aqui mismo: en FM estrecha cuesta
 * 137 bloques en vez de 0, porque 18.000 no es el nivel del
 * discriminador y el pico tiene que bajar hasta el de verdad. Y vaciar
 * ademas las colas de los biquads de audio cuesta de 82 a 137 bloques en
 * todos los modos. El valor de arranque en frio es bueno EN FRIO y malo a
 * mitad de camino. ***
 */
#define CHASQUIDO   32000.0   /* a tope de escala del conversor */
#define VUELTA_MAX  1500      /* 4 s: si tarda esto, es que se ha quedado pegado */

static int tarda_en_volver(demod_mode_t modo)
{
    static int16_t raw[2 * NBLK];
    double base;
    int b;

    demod_am_init();
    demod_am_set_mode(modo);
    nr_ss_set_enabled(0U);
    cons_captura_tramas((uint32_t)NBLK);
    g_fs = (double)demod_am_get_active_fs_hz();
    g_fi = 0.0; g_t = 0.0; sem = 31337u;

    base = corre(raw, 400, 100);

    bloque(raw, CHASQUIDO, 0.0);
    demod_am_process_raw(raw);

    for (b = 0; b < VUELTA_MAX; b++) {
        bloque(raw, 6000.0, 0.05);
        demod_am_process_raw(raw);
        if (rms_bloque() > 0.5 * base) { return b; }
    }
    return VUELTA_MAX;
}

static void chasquido_uno(const char *nombre, demod_mode_t modo)
{
    int n = tarda_en_volver(modo);

    printf("  %-5s  tras el peor chasquido vuelve en %4d bloques (%.0f ms)\n",
           nombre, n, (double)n * 1000.0 / 375.0);

    /* 400 bloques son algo mas de un segundo. Por encima de eso ya no es
     * "tarda en recuperarse": es que algo se ha quedado pegado. */
    if (n >= 400) {
        printf("  MAL: en %s el audio no vuelve solo: %d bloques\n", nombre, n);
        fallos++;
    }
}

/*
 * ======================================================================
 * LO QUE ENTREGA EL DEMODULADOR ES LO QUE EL I2S TIENE ARMADO
 * ======================================================================
 *
 * El unico sitio de todo el camino de audio que puede dejar la radio muda
 * DEL TODO y para siempre es gd32_i2s_stream_write_half(): si el numero
 * de tramas que le entregan no es el que tiene armado, no copia. Y como
 * FM ancha entrega el suyo sin preguntar por la bandera de los 192 kHz y
 * los demas modos no, un descuadre calla AM, banda lateral y FM estrecha
 * y deja FM ancha sonando. Que es, palabra por palabra, el aviso del
 * 12/11/2026.
 *
 * Desde ese dia demod_am.c no deduce ese numero: se lo pregunta al I2S,
 * que es quien lo sabe. Esto lo comprueba FORZANDO el desacuerdo -el
 * tapon dice una cosa y la radio esta en otra- y exigiendo las dos cosas
 * que importan: que entregue exactamente lo que le piden, y que siga
 * saliendo audio.
 */
static void tramas_uno(const char *nombre, demod_mode_t modo, uint32_t armado)
{
    static int16_t raw[2 * NBLK];
    double r;

    demod_am_init();
    demod_am_set_mode(modo);
    nr_ss_set_enabled(0U);
    cons_captura_tramas((uint32_t)NBLK);
    cons_i2s_tramas_media(armado);
    g_fs = (double)demod_am_get_active_fs_hz();
    g_fi = 0.0; g_t = 0.0; sem = 4242u;

    r = corre(raw, 400, 100);
    cons_i2s_tramas_media(0U);

    printf("  %-5s  el I2S pide %3u y el demodulador entrega %3u; audio %7.1f\n",
           nombre, (unsigned)armado, (unsigned)g_i2s_tramas, r);

    if (g_i2s_tramas != armado) {
        printf("  MAL: en %s entrega %u cuando le piden %u: el I2S no copiaria\n",
               nombre, (unsigned)g_i2s_tramas, (unsigned)armado);
        fallos++;
    }
    if (r < 1.0) {
        printf("  MAL: en %s no sale audio con el I2S armado a %u\n",
               nombre, (unsigned)armado);
        fallos++;
    }
}

int main(void)
{
    static const struct { const char *n; demod_mode_t m; } modos[] = {
        { "AM",  DEMOD_MODE_AM  },
        { "SAM", DEMOD_MODE_SAM },
        { "USB", DEMOD_MODE_USB },
        { "NFM", DEMOD_MODE_NFM },
    };
    int i, f;

    printf("Boton de Ruido conmutado muchas veces:\n");
    for (i = 0; i < 4; i++) {
        for (f = 0; f < 7; f++) {
            uno(modos[i].n, modos[i].m, f);
        }
    }

    printf("\nQue ninguna discontinuidad de la cadena la deje muda:\n");
    for (i = 0; i < 4; i++) { chasquido_uno(modos[i].n, modos[i].m); }

    printf("\nLo que entrega el demodulador contra lo que el I2S tiene armado:\n");
    for (i = 0; i < 4; i++) {
        tramas_uno(modos[i].n, modos[i].m, (uint32_t)SDR_RX_BLOCK_SAMPLES);
        tramas_uno(modos[i].n, modos[i].m, (uint32_t)SDR_RX_BLOCK_SAMPLES_WFM);
    }

    if (fallos) { printf("nr_conmuta: %d MAL\n", fallos); return 1; }
    printf("nr_conmuta: bien\n");
    return 0;
}
