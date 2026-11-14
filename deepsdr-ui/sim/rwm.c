/*
 * ======================================================================
 * RWM: EL BORDE DE MINUTO, LOS DOS TRENES Y EL DUT1 - 13/11/2026
 * ======================================================================
 *
 * Mide el decodificador de verdad -User/hora_rwm.c, el mismo que se compila
 * para el GD32- con una señal fabricada segun la norma, no segun lo que el
 * decodificador espera. Eso importa: si el generador copiara el plan del
 * decodificador, el banco comprobaria que las dos mitades estan de acuerdo y
 * no que ninguna de las dos se parece a Moscu.
 *
 * LO QUE SE EMITE, en ENVOLVENTE a 3.000 muestras por segundo, que es lo que
 * demod_am.c le entrega a estos decodificadores con `sub` = 8:
 *
 *   tren de 1 Hz    un pulso de 100 ms por segundo; 500 ms el del minuto;
 *                   y el doblado del DUT1, 100 ms a los 200 del segundo
 *   tren de 10 Hz   diez pulsos por segundo de 20 ms; 40 ms el que cae en
 *                   el segundo; 500 el del minuto
 *   portadora       encendida y quieta
 *
 * LAS PREGUNTAS, EN ESTE ORDEN:
 *   1. ¿reconoce cada tren, y cuenta los segundos?
 *   2. ¿encuentra el borde de minuto, y dice bien cuanto hace? Es lo UNICO
 *      que la radio necesita de esta emisora.
 *   3. ¿lee el DUT1, con signo?
 *   4. ¿se calla con ruido solo? Un borde de minuto inventado pondria el
 *      reloj mal y con cara de estar bien, que es lo peor que puede pasar.
 *   5. ¿hasta cuanto ruido aguanta?
 */
#include "hora_rwm.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define FS 3000.0

static int fallos = 0;
static void comprueba(int ok, const char *que)
{
    printf("    %-58s %s\n", que, ok ? "ok" : "MAL");
    if (!ok) { fallos++; }
}

/* Ruido gaussiano por Box-Muller, con semilla propia para que el banco sea
 * repetible. */
static unsigned long g_sem = 12345UL;
static double ruido(void)
{
    double a, b;
    g_sem = g_sem * 1103515245UL + 12345UL;
    a = (double)((g_sem >> 8) & 0xFFFFFF) / 16777216.0;
    g_sem = g_sem * 1103515245UL + 12345UL;
    b = (double)((g_sem >> 8) & 0xFFFFFF) / 16777216.0;
    if (a < 1e-12) { a = 1e-12; }
    return sqrt(-2.0 * log(a)) * cos(2.0 * M_PI * b);
}

static double g_sigma = 0.0;

/* `on` = portadora encendida. La envolvente de una portadora conmutada es
 * eso: 1 o 0, mas el ruido, que esta SIEMPRE -tambien con la portadora
 * apagada, que es lo que hace que el suelo sea un suelo y no un cero-. */
static void emite(int on, double ms)
{
    long n = (long)(FS * ms / 1000.0 + 0.5), i;
    for (i = 0; i < n; i++) {
        double v = (on ? 1.0 : 0.0) + g_sigma * ruido();
        if (v < 0.0) { v = 0.0; }   /* una envolvente no es negativa */
        rwm_feed((float)v);
    }
}

/* Un segundo del tren de 1 Hz. `doble` pide el pulso del DUT1. */
static void seg_1hz(int es_minuto, int doble)
{
    if (es_minuto) {
        emite(1, 500.0);
        emite(0, 500.0);
    } else if (doble) {
        emite(1, 100.0);
        emite(0, 100.0);   /* el doblado empieza a los 200 ms del segundo */
        emite(1, 100.0);
        emite(0, 700.0);
    } else {
        emite(1, 100.0);
        emite(0, 900.0);
    }
}

/* Un segundo del tren de 10 Hz: el primero de 40 ms y nueve de 20. */
static void seg_10hz(int es_minuto)
{
    int k;
    if (es_minuto) { emite(1, 500.0); emite(0, 500.0); }
    else           { emite(1, 40.0);  emite(0, 60.0);  }
    for (k = 0; k < 9; k++) { emite(1, 20.0); emite(0, 80.0); }
}

/*
 * UN TREN DE 1 Hz REALISTA: primero unos segundos de compas y DESPUES el
 * borde de minuto.
 *
 * *** LA PRIMERA VERSION EMITIA EL BORDE EL PRIMERO, y ese es el unico caso
 * que no puede funcionar: el decodificador no se cree un pulso de medio
 * segundo si no hay ritmo detras que lo avale -y con razon, porque el ruido
 * fabrica pulsos de medio segundo-. Ademas no es lo que pasa en el aire:
 * giras el dial, caes a mitad de tren, el compas se reconoce en tres o
 * cuatro segundos y el borde de minuto llega cuando toca. ***
 */
static void tren_1hz(int antes, int segundos, int dut1)
{
    int s;
    for (s = 0; s < antes; s++) { seg_1hz(0, 0); }
    seg_1hz(1, 0);                       /* el borde de minuto */
    for (s = 1; s <= segundos; s++) {
        /* El DUT1 positivo se codifica doblando los segundos 1..dut1; el
         * negativo, los 9..9+|dut1|-1. */
        int doble = (dut1 > 0 && s <= dut1)
                 || (dut1 < 0 && s >= 9 && s <= 8 - dut1);
        seg_1hz(0, doble);
    }
}

int main(void)
{
    rwm_info_t in;

    printf("RWM: EL BORDE DE MINUTO, LOS DOS TRENES Y EL DUT1\n");

    printf("\n1. el tren de 1 Hz\n");
    g_sigma = 0.0;
    rwm_init((float)FS);
    emite(0, 1000.0);                    /* un segundo de nada, para el suelo */
    tren_1hz(6, 20, 0);
    rwm_info(&in);
    printf("    estado %u, pulsos %u, segundo %u, marca %u, hace %u ms\n",
           in.estado, (unsigned)in.pulsos, (unsigned)in.segundo,
           in.marca_hay, (unsigned)in.marca_hace_ms);
    comprueba(in.estado == (uint8_t)RWM_1HZ, "reconoce el tren de 1 Hz");
    comprueba(in.marca_hay == 1U, "encuentra el borde de minuto");
    comprueba(in.segundo == 20U, "cuenta los veinte segundos");

    printf("\n2. y dice bien cuanto hace del borde de minuto\n");
    {
        /* Tras 20 segundos de tren, el ultimo pulso cayo en el segundo 20 y
         * desde el borde han pasado 20 s mas los 100 ms del pulso. Se
         * admite medio segundo de margen: el decodificador apunta el
         * PRINCIPIO del pulso de 500 ms, que es el borde de verdad.
         * *** 21.000 y no 20.100: el segundo del minuto OCUPA su segundo
         * entero -500 ms de pulso y 500 de silencio-, asi que tras veinte
         * segundos mas han pasado veintiuno. La primera version de esta
         * cuenta decia 20.100 y la que estaba mal era ella, no el
         * decodificador. *** */
        unsigned esperado = 21000U;
        long err = (long)in.marca_hace_ms - (long)esperado;
        printf("    dice %u ms, deberia ser ~%u (error %+ld)\n",
               (unsigned)in.marca_hace_ms, esperado, err);
        comprueba(err > -500 && err < 500, "el atraso del borde cuadra");
    }

    printf("\n3. el tren de 10 Hz\n");
    g_sigma = 0.0;
    rwm_init((float)FS);
    emite(0, 1000.0);
    { int s; for (s = 0; s < 4; s++) { seg_10hz(0); } }   /* compas primero */
    seg_10hz(1);
    { int s; for (s = 0; s < 10; s++) { seg_10hz(0); } }
    rwm_info(&in);
    printf("    estado %u, pulsos %u, segundo %u, marca %u\n",
           in.estado, (unsigned)in.pulsos, (unsigned)in.segundo, in.marca_hay);
    comprueba(in.estado == (uint8_t)RWM_10HZ, "reconoce el tren de 10 Hz");
    comprueba(in.marca_hay == 1U, "encuentra el borde de minuto");
    comprueba(in.segundo == 10U, "cuenta los diez segundos por los de 40 ms");

    printf("\n4. el DUT1\n");
    {
        static const int k_dut[] = { 0, 3, 5, -2, -4 };
        unsigned i;
        for (i = 0U; i < sizeof k_dut / sizeof k_dut[0]; i++) {
            char que[64];
            g_sigma = 0.0;
            rwm_init((float)FS);
            emite(0, 1000.0);
            tren_1hz(6, 41, k_dut[i]);     /* hasta pasar el segundo 40 */
            rwm_info(&in);
            printf("    emitido DUT1 %+d  ->  leido %+d\n", k_dut[i], in.dut1);
            snprintf(que, sizeof que, "lee DUT1 %+d", k_dut[i]);
            comprueba(in.dut1 == (int8_t)k_dut[i], que);
        }
    }

    /*
     * 5. LA PORTADORA CONTINUA NO SE MIDE AQUI, Y ES A PROPOSITO.
     *
     * Una portadora encendida y quieta no tiene contraste: el suelo sube a
     * su nivel y la razon pico/suelo se va a uno, que es exactamente lo
     * mismo que ve este modulo cuando no hay nada. Distinguir "portadora" de
     * "nada" necesita una referencia ABSOLUTA -el medidor de señal de la
     * radio-, y eso la pantalla lo tiene y este fichero no. Asi que el
     * modulo dice lo que puede probar: que no hay pulsos. Lo intentamos al
     * reves y el banco lo tumbo.
     */

    printf("\n6. RUIDO SOLO, sin un solo pulso: 60 s\n");
    {
        static const double k_sig[] = { 0.10, 0.30, 1.00 };
        unsigned i;
        for (i = 0U; i < sizeof k_sig / sizeof k_sig[0]; i++) {
            char que[72];
            rwm_init((float)FS);
            g_sigma = k_sig[i];
            emite(0, 60000.0);
            rwm_info(&in);
            printf("    sigma %.2f -> estado %u, marca %u, pulsos %u\n",
                   k_sig[i], in.estado, in.marca_hay, (unsigned)in.pulsos);
            snprintf(que, sizeof que,
                     "con sigma %.2f de ruido solo no inventa borde de minuto",
                     k_sig[i]);
            comprueba(in.marca_hay == 0U, que);
        }
    }

    printf("\n7. hasta cuanto ruido aguanta el tren de 1 Hz\n");
    {
        static const double k_sig[] = { 0.00, 0.05, 0.10, 0.20, 0.30, 0.50 };
        unsigned i;
        double tope = -1.0;
        for (i = 0U; i < sizeof k_sig / sizeof k_sig[0]; i++) {
            int bien;
            rwm_init((float)FS);
            g_sigma = k_sig[i];
            emite(0, 1000.0);
            tren_1hz(6, 20, 0);
            rwm_info(&in);
            bien = (in.estado == (uint8_t)RWM_1HZ) && in.marca_hay
                   && (in.segundo >= 18U && in.segundo <= 22U);
            printf("    sigma %.2f -> estado %u, segundo %u, marca %u   %s\n",
                   k_sig[i], in.estado, (unsigned)in.segundo, in.marca_hay,
                   bien ? "" : "<- se cae");
            if (bien) { tope = k_sig[i]; }
        }
        if (tope < 0.0) { printf("    -> no aguanta ni sin ruido\n"); }
        else { printf("    -> aguanta hasta sigma %.2f\n", tope); }
        comprueba(tope >= 0.10, "aguanta al menos sigma 0,10");
    }

    printf("\n  %s\n", fallos ? "HAY FALLOS" : "TODO CORRECTO (0 fallos)");
    return fallos ? 1 : 0;
}
