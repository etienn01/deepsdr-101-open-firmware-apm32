/*
 * ======================================================================
 * EL ESCANER DE TRAMO, CONTRA EL BANCO DE MEMORIAS DE VERDAD - 11/11/2026
 * ======================================================================
 *
 * POR QUE NO VALIA sim/barrido.c. Aquel corre la maquina de verdad pero
 * con un banco de memorias APAÑADO: banco_usadas() y banco_siguiente()
 * son dos funciones de mentira dentro del propio fichero. Para el barrido
 * de memorias da igual, porque lo que se mide alli es cuando se para y
 * cuando sigue.
 *
 * Aqui lo que hay que medir es OTRA COSA: que una emisora encontrada dos
 * veces SUBA EL CONTADOR en vez de ocupar dos celdas, que el banco no se
 * llene de duplicados, y que una memoria grabada a mano no la pise nadie.
 * Eso vive dentro de banco.c -banco_busca_hz(), banco_libre(),
 * banco_menos_oida(), banco_vista()- y contra un banco de mentira no se
 * mide: se mediria el apaño.
 *
 * Asi que este banco enlaza User/banco.c DE VERDAD, y la maquina sale de
 * User/main.c tal cual por tools/barrido_dump.py. Lo unico inventado es
 * la radio: una banda con emisoras en frecuencias conocidas, un reloj y
 * un nivel que sube donde hay alguien.
 */
#include "demod_am.h"
#include "banco.h"
#include <stdio.h>
#include <string.h>

static int fallos = 0;

static void pasa(const char *que, int bien)
{
    printf("    %-58s %s\n", que, bien ? "ok " : "*** MAL ***");
    if (!bien) { fallos++; }
}

/* ======================================================================
 * LA RADIO DE MENTIRA: UNA BANDA CON EMISORAS
 * ====================================================================== */

volatile uint32_t g_msticks;

#define BANDA_PISO_DB   (-100.0f)
#define EMIS_N            3U

/* Las tres emisoras del tramo de prueba, en 145,000-146,000. */
static const uint32_t k_emis_hz[EMIS_N] = { 145150000UL, 145500000UL, 145775000UL };

static uint32_t     s_tune_hz = 145000000UL;
static uint8_t      s_tune_step_idx = 0U;
static const uint32_t k_tune_steps[] = { 25000UL, 12500UL, 5000UL, 1000UL };
static demod_mode_t s_modo = DEMOD_MODE_NFM;
static unsigned     s_sem = 1u;
static unsigned     s_bips = 0u, s_bips_mal = 0u;
static unsigned     s_nivel_on = 0u, s_cerrada = 0u, s_guardados = 0u;
static uint8_t      s_scanner_bk = 0U;   /* lo pone main() desde BANCO_SCANNER */

/*
 * LAS EMISORAS HABLAN A RAFAGAS, y no es un adorno: el barrido se QUEDA
 * mientras se oye algo y solo sigue cuando el canal lleva SCAN_VUELVE_MS
 * callado. Con emisoras encendidas para siempre -que es como estaba este
 * banco al escribirlo- el barrido se para en la primera y no pasa de ahi
 * NUNCA, y eso es lo correcto: el fallo era del banco.
 *
 * Asi que cada una habla SEIS segundos y calla CUATRO. Seis son muchas
 * veces los 250 ms de la ventana de escucha -o sea que una pasada la
 * caza con holgura- y cuatro son mas que los 2.500 de SCAN_VUELVE_MS -o
 * sea que el barrido se despega y sigue-. Con el tramo a 5 kHz son 200
 * puntos y 400 ms por punto: unos 80 segundos por vuelta.
 */
/*
 * Y EL CICLO NO DIVIDE A LA VUELTA DEL BARRIDO, QUE NO ES UN DETALLE.
 *
 * La primera version puso ciclo de 10 s, y la vuelta del tramo son 80 s
 * justos: 80 es ocho veces 10, asi que el dial llegaba a cada emisora
 * SIEMPRE EN LA MISMA FASE del ciclo. Las que caian en el hueco no se
 * encontraban jamas, vuelta tras vuelta, y el banco decia que el escaner
 * no las veia. No era el escaner: era que mi radio de mentira estaba
 * sincronizada con el.
 *
 * Con 7 segundos -que no divide a 80- la fase pasea, y ademas cada
 * emisora lleva su propio desfase para que no hablen las tres a la vez.
 */
#define EMIS_HABLA_MS   4000UL
#define EMIS_CICLO_MS   7000UL
static const uint32_t k_emis_fase_ms[EMIS_N] = { 0UL, 2300UL, 4700UL };
static uint32_t s_emis_hasta_ms = 0xFFFFFFFFUL;   /* tope global, para apagarlas todas */

static int emis_hablando(uint8_t i)
{
    return (g_msticks < s_emis_hasta_ms)
           && (((g_msticks + k_emis_fase_ms[i]) % EMIS_CICLO_MS) < EMIS_HABLA_MS);
}

/* Y una portadora que no calla nunca, para el apartado 5b. */
#define PERMA_HZ  145350000UL
static uint8_t s_perma_on = 0U;

static float ruidito(void)
{
    s_sem = s_sem * 1103515245u + 12345u;
    return (float)(((s_sem >> 16) & 0xFFFF) / 65535.0 - 0.5) * 1.5f;
}

/* ¿Cae el dial encima de una emisora? Con una anchura de 6 kHz, que es lo
 * que ocupa una de FM estrecha: por eso una emisora puede responder en dos
 * pasos contiguos de 5 kHz, que es justo el caso que obliga a que
 * banco_busca_hz() no compare con ==. */
static int encima(void)
{
    uint8_t i;

    if (s_perma_on) {
        uint32_t d = (PERMA_HZ > s_tune_hz) ? (PERMA_HZ - s_tune_hz)
                                            : (s_tune_hz - PERMA_HZ);
        if (d <= 3000UL) { return 1; }      /* y esta no calla jamas */
    }
    for (i = 0U; i < EMIS_N; i++) {
        uint32_t d = (k_emis_hz[i] > s_tune_hz) ? (k_emis_hz[i] - s_tune_hz)
                                                : (s_tune_hz - k_emis_hz[i]);
        if (d <= 3000UL && emis_hablando(i)) { return 1; }
    }
    return 0;
}

/*
 * LA LECTURA NO ES INSTANTANEA, Y SIN ESO ESTE BANCO NO MIDE NADA.
 *
 * La radio no lee el nivel: lo SUAVIZA, con alfa 0,9 por bloque de audio
 * -2,667 ms-, o sea unos 24 ms de constante de tiempo (ver NIVEL_ALPHA en
 * demod_am.c). Medido contra la cadena de verdad, despues de un salto de
 * frecuencia con un escalon de 34 dB la lectura tarda unos 100 ms en
 * ponerse a 1 dB del valor final.
 *
 * Hasta el 11/11/2026 este banco devolvia el nivel EXACTO del punto en el
 * que estuviera el dial, sin retardo ninguno. Con eso, acortar los
 * tiempos del barrido salia gratis y el banco seguia verde: estaba
 * midiendo una radio que no existe. Ahora lleva el mismo suavizado, en
 * pasos de un milisegundo -que es como corre este banco-, asi que si los
 * tiempos se acortan de mas, las emisoras dejan de aparecer y se ve.
 *
 * 0,9 por cada 2,667 ms es 0,96126 por milisegundo.
 */
#define LECT_ALFA_MS  0.03874f
static float s_lect = BANDA_PISO_DB;

static void lectura_avanza(void)
{
    float inst = BANDA_PISO_DB + (encima() ? 25.0f : 0.0f) + ruidito();
    s_lect += (inst - s_lect) * LECT_ALFA_MS;
}

static float nivel_ahora(void)
{
    return s_lect;
}

float demod_am_get_squelch_level_db(void) { return nivel_ahora(); }
float demod_am_get_nivel_db(void)         { return nivel_ahora(); }
float demod_wfm_get_sql_db(void)          { return encima() ? 47.0f : 75.0f; } /* el tramo se barre en NFM/AM: esta no entra */
demod_mode_t demod_am_get_mode(void)      { return s_modo; }
void demod_am_set_nivel_medir(uint8_t on) { s_nivel_on = on; }

static void apply_lo_tune(uint32_t hz)  { (void)hz; }
static void apply_lo_tune_paseo(uint32_t hz) { (void)hz; }   /* apm32: a range step walks the NCO */
static void freq_display_draw(void)     { }
static void mem_boton_repinta(void)     { }
static void mem_rejilla_repinta(void)   { }
static void menu_screen_close(void)     { s_cerrada++; }
static void bip_listo(void)             { s_bips++; }
static void bip_error(void)             { s_bips_mal++; }
static uint8_t modo_panel_entero(void)  { return 0U; }

/* El buffer del fichero y el guardado: aqui solo se cuenta que se llame
 * UNA vez, al parar, y no una por hallazgo. */
static uint8_t s_gsv_buf[4096];
static uint8_t banco_guarda_falso(void) { s_guardados++; return 1U; }
#define banco_guarda(b, c)  (((void)(b), (void)(c)), banco_guarda_falso())

/* banco.c habla con la flash para cargar y guardar el fichero. Aqui no
 * hay flash y tampoco hace falta: lo que se mide es la TABLA en memoria.
 * Los dos caminos de fichero se cortan con estos dos, que es lo minimo
 * para enlazar sin tocar banco.c. */
uint32_t spi_flash_read_file_by_name(const char n8[8], const char e3[3],
                                     uint8_t *buf, uint32_t cap)
{ (void)n8; (void)e3; (void)buf; (void)cap; return 0UL; }
int spi_flash_write_or_update_file(const char n8[8], const char e3[3],
                                   const uint8_t *d, uint32_t len)
{ (void)n8; (void)e3; (void)d; (void)len; return 1; }

/* Lo que el barrido de MEMORIAS necesita y aqui no se usa. */
static uint8_t mem_recupera(uint8_t bk, uint8_t c) { (void)bk; (void)c; return 0U; }

/* ======================================================================
 * LA MAQUINA DE VERDAD
 * ====================================================================== */
#define BANCO_SCANNER  (BANCO_N - 1U)
static uint8_t  s_rng_on;
static uint32_t s_rng_lo, s_rng_hi;
static uint8_t  s_scan_on, s_scan_banco, s_scan_mem, s_scan_para_en;
static uint32_t s_scan_ms, s_scan_espera, s_scan_oida_ms;
static float    s_scan_pico, s_scan_valle, s_scan_piso[BANCO_MEM_N], s_rng_piso;
static uint8_t  s_rng_pide;
#define RNG_PIDE_NADA 0U
/* mem_scan_conmuta() y s_rng_pide los arrastra la maquina de memorias,
 * que viene entera: aqui no se usan y no pasa nada. */

/* Las del rango se declaran antes porque la maquina de memorias las
 * llama -el barrido de tramo entra por mem_scan_salta(), mem_scan_poll()
 * y mem_scan_conmuta()-, y en main.c estan todas en el mismo fichero y en
 * el orden bueno. Aqui vienen en dos cabeceras generadas. */
static void     rng_scan_salta(void);
static void     rng_apunta(void);
static void     rng_scan_conmuta(void);
static uint32_t rng_paso_hz(void);

#include "barrido_consts.h"
#include "barrido_maquina.h"
#include "rango_maquina.h"

/* ======================================================================
 * EL RELOJ: una vuelta del bucle principal cada milisegundo
 * ====================================================================== */
static void corre_ms(uint32_t ms)
{
    uint32_t i;
    for (i = 0U; i < ms; i++) { g_msticks++; lectura_avanza(); mem_scan_poll(); }
}

static uint8_t apuntadas(void)
{
    return banco_usadas(s_scanner_bk);
}

static uint16_t vistas_de(uint32_t hz)
{
    uint8_t i = banco_busca_hz(s_scanner_bk, hz, 6000UL);
    banco_mem_t m;
    if (i == 0xFFU || !banco_lee(s_scanner_bk, i, &m)) { return 0U; }
    return m.vistas;
}

int main(void)
{
    char txt[24];

    s_scanner_bk = (uint8_t)BANCO_SCANNER;
    printf("EL ESCANER DE TRAMO, CON EL BANCO DE VERDAD\n");
    printf("  tramo 145,000-146,000, paso 5 kHz, tres emisoras\n\n");

    banco_init();
    banco_pasos_pon(k_tune_steps, (uint8_t)(sizeof k_tune_steps / sizeof k_tune_steps[0]));

    /* 1. El texto del tramo, que es lo que se ve en la celda SCAN. */
    {
        uint8_t j = rng_pon_mhz(txt, 145000000UL);
        txt[j++] = '-';
        j += rng_pon_mhz(&txt[j], 146000000UL);
        txt[j] = '\0';
        printf("  el tramo se escribe '%s'\n", txt);
        pasa("y es lo que se tecleo, en MHz con tres decimales",
             strcmp(txt, "145,000-146,000") == 0);
    }

    /* 2. Arrancar: sin tramo no arranca; con tramo si. */
    s_rng_lo = 0UL; s_rng_hi = 0UL;
    rng_scan_conmuta();
    pasa("sin tramo fijado NO arranca, y lo dice con el pip",
         !s_scan_on && s_bips_mal == 1u);

    s_rng_lo = 145000000UL; s_rng_hi = 146000000UL;
    s_tune_step_idx = 2U;                 /* 5 kHz */
    rng_scan_conmuta();
    pasa("con tramo arranca", s_scan_on && s_rng_on);
    pasa("y enciende la medida del nivel", s_nivel_on == 1u);
    pasa("y se vuelve a la pantalla principal", s_cerrada == 1u);
    pasa("y empieza por el principio del tramo", s_tune_hz == 145000000UL);

    /*
     * 2b. CUANTO TARDA UNA VUELTA, que es por lo que el dueno dijo que el
     * escaner era lento y tenia razon.
     *
     * Con los tiempos del banco de memorias -150 + 250- eran 400 ms por
     * punto y OCHENTA segundos de vuelta en este tramo. Con los del tramo
     * -60 + 60- son 120 ms y unos veinticuatro. Se cuenta de verdad:
     * desde el arranque hasta que el dial vuelve a pasar por el principio.
     *
     * Se mide SIN emisoras, porque una parada de tres segundos en cada
     * una no es tiempo de barrido sino de escucha, y lo que se vigila
     * aqui es el recorrido.
     */
    {
        uint32_t t0 = g_msticks, vuelta = 0UL, t;
        uint32_t puntos = (s_rng_hi - s_rng_lo) / rng_paso_hz() + 1UL;

        s_emis_hasta_ms = g_msticks;          /* todas calladas */
        for (t = 0U; t < 200000U && !vuelta; t++) {
            corre_ms(1UL);
            if (t > 1000U && s_tune_hz == s_rng_lo) { vuelta = g_msticks - t0; }
        }
        printf("      %u puntos; la vuelta son %u.%03u s (%u ms por punto)\n",
               (unsigned)puntos, (unsigned)(vuelta / 1000UL),
               (unsigned)(vuelta % 1000UL),
               (unsigned)(vuelta / (puntos ? puntos : 1UL)));
        pasa("la vuelta del tramo baja de 30 segundos",
             vuelta > 0UL && vuelta < 30000UL);
        s_emis_hasta_ms = 0xFFFFFFFFUL;       /* y vuelven a hablar */
    }

    /* 3. Varias vueltas: que las encuentre las tres, y SOLO las tres. */
    corre_ms(600UL * 1000UL);
    printf("      apuntadas %u celdas; vistas %u / %u / %u\n",
           (unsigned)apuntadas(),
           (unsigned)vistas_de(k_emis_hz[0]), (unsigned)vistas_de(k_emis_hz[1]),
           (unsigned)vistas_de(k_emis_hz[2]));
    pasa("encuentra las tres emisoras del tramo",
         vistas_de(k_emis_hz[0]) && vistas_de(k_emis_hz[1]) && vistas_de(k_emis_hz[2]));
    pasa("y ocupa TRES celdas, no una por paso ni por vuelta",
         apuntadas() == EMIS_N);
    pasa("y no ha guardado el fichero todavia", s_guardados == 0u);

    /* 4. Mas vueltas: suben los contadores y las celdas NO. */
    {
        uint16_t v0 = vistas_de(k_emis_hz[0]);
        uint16_t v1 = vistas_de(k_emis_hz[1]);
        uint8_t  celdas = apuntadas();

        corre_ms(600UL * 1000UL);
        printf("      vistas %u->%u y %u->%u, celdas %u->%u\n",
               (unsigned)v0, (unsigned)vistas_de(k_emis_hz[0]),
               (unsigned)v1, (unsigned)vistas_de(k_emis_hz[1]),
               (unsigned)celdas, (unsigned)apuntadas());
        pasa("al volver a pasar SUBEN los contadores",
             vistas_de(k_emis_hz[0]) > v0 && vistas_de(k_emis_hz[1]) > v1);
        pasa("y NO se abre ni una celda mas", apuntadas() == celdas);
    }

    /* 5. Una memoria a mano no la pisa el barrido ni con el banco lleno. */
    {
        banco_mem_t m;
        uint8_t i, libres;

        memset(&m, 0, sizeof m);
        m.hz = 145900000UL; m.modo = (uint8_t)DEMOD_MODE_NFM; m.vistas = 0U;
        /* La primera libre, a mano: vistas = 0 la marca como del usuario. */
        i = banco_libre(s_scanner_bk);
        (void)banco_graba(s_scanner_bk, i, &m);

        /* Y se llena el resto a mano tambien, menos una del barrido. */
        libres = 0U;
        while ((i = banco_libre(s_scanner_bk)) != 0xFFU) {
            m.hz = 146500000UL + (uint32_t)libres * 25000UL;
            m.vistas = 0U;
            (void)banco_graba(s_scanner_bk, i, &m);
            libres++;
        }
        pasa("el banco se llena", banco_libre(s_scanner_bk) == 0xFFU);

        /* Ahora el barrido encuentra algo nuevo y no tiene donde: la unica
         * que puede pisar es una suya, y las suyas siguen ahi. */
        {
            uint16_t v0 = vistas_de(k_emis_hz[0]);
            corre_ms(200UL * 1000UL);
            pasa("lleno, la de a mano en 145,900 sigue intacta",
                 banco_busca_hz(s_scanner_bk, 145900000UL, 100UL) != 0xFFU);
            pasa("y las suyas se siguen contando", vistas_de(k_emis_hz[0]) >= v0);
        }
    }

    /*
     * 5b. UNA PORTADORA PERMANENTE NO PARA EL BARRIDO.
     *
     * Es el caso que justifica SCAN_RNG_PARADA_MS. Con la regla del banco
     * de memorias -seguir cuando el canal lleve callado de seguido- un
     * repetidor en reposo o una baliza paran el barrido PARA SIEMPRE y el
     * resto del tramo no se mira nunca. Aqui se enciende una que no calla
     * jamas y se comprueba que el dial la deja atras.
     */
    {
        uint32_t ini = s_tune_hz;
        uint32_t visto_lejos = 0UL;
        uint32_t t;

        s_perma_on = 1U;                /* 145,350 encendida y sin callar */
        s_rng_lo = 145000000UL; s_rng_hi = 146000000UL;
        s_scan_on = 0U; s_rng_on = 0U;  /* se vuelve a arrancar limpio */
        rng_scan_conmuta();
        for (t = 0U; t < 120U; t++) {
            corre_ms(1000UL);
            if (s_tune_hz > 145400000UL) { visto_lejos = s_tune_hz; }
        }
        printf("      con portadora fija en 145,350: el dial llego a %u Hz\n",
               (unsigned)visto_lejos);
        pasa("una portadora que no calla NO para el barrido",
             visto_lejos != 0UL);
        (void)ini;
        s_perma_on = 0U;
    }

    /* 6. Parar: guarda el fichero UNA vez y deja el dial donde este. */
    {
        uint32_t donde = s_tune_hz;

        mem_scan_para();
        pasa("al parar se apaga la medida del nivel", s_nivel_on == 0u);
        pasa("y se guarda MEM.CSV una sola vez", s_guardados == 1u);
        pasa("y la radio se queda donde estaba", s_tune_hz == donde);
        pasa("y el barrido ya no corre", !s_scan_on && !s_rng_on);
    }

    printf("\n%s  (%d mal)\n", fallos ? "*** HAY FALLOS ***" : "TODO CORRECTO", fallos);
    return fallos ? 1 : 0;
}
