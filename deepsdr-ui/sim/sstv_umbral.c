/*
 * ======================================================================
 * HASTA DONDE AGUANTA EL SINCRONISMO DE SSTV, Y QUE SE ROMPE PRIMERO
 * ======================================================================
 *
 * EL AVISO - 13/11/2026. Un usuario da por demasiado alto el umbral de
 * captura de sincronismo: a veces empieza a decodificar y el sincronismo de
 * linea no llega, y en una sesion con buena propagacion en 20 metros y mucho
 * SSTV no se capturo ninguna imagen.
 *
 * POR QUE HACE FALTA ESTE BANCO Y NO VALEN LOS QUE HAY. Los dos que tocan
 * esto se paran justo antes de la pregunta:
 *
 *   - sim/sstvtest.c barre ruido de 0,02 a 0,60, pero lo hace CON CABECERA
 *     y su propio comentario dice que lo primero que se rompe es la
 *     cabecera.
 *     Con cabecera legible nunca ve fallar el sincronismo de linea, que es
 *     justo lo que el aviso describe.
 *   - sim/sstv_auto.c promete medir hasta donde llega, y no "aguanta ruido"
 *     sino CUANTO... pero barre hasta sigma 0,30, donde todavia acierta. O
 *     sea que mide que aguanta 0,30, no DONDE SE CAE.
 *
 * Asi que ninguno de los dos contesta cuanto ruido hace falta para que no
 * salga la foto. Esto si.
 *
 * COMO SE MIDE. Las dos entradas de verdad, por separado:
 *
 *   CAYENDO ENCIMA, que es el caso del aviso: se emite a mitad de foto, sin
 *   lider, sin corte y sin bits. No hay cabecera que leer, asi que el modo
 *   tiene que salir del RITMO de los pulsos de sincronismo - y ese camino
 *   es el que el aviso pone en duda.
 *
 *   CON CABECERA, como testigo: si con VIS aguanta mucho mas, el problema
 *   esta en el detector de ritmo y no en el pulso.
 *
 * Y LLEVA SUELOS, por debajo de lo medido el 13/11/2026, para que esto sea
 * un vigilante y no solo una foto. Lo medido, EXIGIENDO QUE EL MODO SEA EL
 * QUE SE EMITIO:
 *
 *   Martin 1  cayendo encima .... sigma 0,70
 *   Martin 1  con cabecera ...... sigma 0,50
 *   Scottie 1 cayendo encima .... sigma 0,40   <- el mas flojo, y es el
 *   Scottie 1 con cabecera ...... sigma 0,50      caso del aviso
 *
 * y se exige un escalon por debajo de cada uno: 0,50 / 0,40 / 0,30 / 0,40. El margen es a proposito: el ruido del
 * banco es gaussiano y un suelo pegado a la medida se rompe con cualquier
 * cambio de semilla.
 *
 * El otro lado de la balanza -que la banda vacia NO enganche- no se repite
 * aqui: lo vigilan sstvtest.c con 30 s de ruido y sstv_auto.c con 8 s.
 */
#include "sstv.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* El pozo de RAM prestada. Las firmas, copiadas de hfdl_ram.h. */
static unsigned char g_ram_pozo[0xD4A0];
void           hfdl_ram_coge(void) { }
unsigned char *hfdl_ram_todo(unsigned *cap)
{
    if (cap) { *cap = (unsigned)sizeof g_ram_pozo; }
    return g_ram_pozo;
}
void           hfdl_ram_suelta(void) { }

#define FS 12000.0

#include "sstv_genera.h"

static int fallos = 0;

typedef struct {
    const char *modo;       /* el que dijo el decodificador */
    int   engancho_en;      /* en que linea engancho, -1 si nunca */
    int   lineas;           /* cuantas pinto */
    uint32_t sincs_malos;
    uint8_t como;           /* SSTV_COMO_*: de donde salio el modo */
} medida_t;

static double g_desvio = 0.0;   /* Hz de error de sintonia de la pasada */

static void pasada(uint8_t modo, int lineas, double sigma, int con_vis,
                   medida_t *r)
{
    tx_t t;
    sstv_info_t in;
    sstv_plan_t pl;
    int y;
    static char nombre[24];

    memset(&t, 0, sizeof t);
    t.semilla = 7777UL; t.ppm = 0.0; t.sigma = sigma;
    memset(g_img, 0, sizeof g_img);
    g_max_y = -1;
    g_n = 0; g_t = 0.0; g_off = g_desvio;

    emite_reinicia();
    sstv_start((float)FS);
    sstv_auto(1U);
    sstv_plan(modo, &pl);

    r->engancho_en = -1;
    if (con_vis) { cabecera_vis(&t, sstv_modo_vis(modo)); }

    for (y = 0; y < lineas; y += pl.lineas_img) {
        linea(&t, modo, y);
        sstv_info(&in);
        if (r->engancho_en < 0 && in.estado == SSTV_IMAGEN && in.modo) {
            r->engancho_en = (y / (int)pl.lineas_img) + 1;
        }
    }
    sstv_info(&in);
    nombre[0] = '\0';
    if (in.modo) {
        unsigned k = 0U;
        while (in.modo[k] != '\0' && k < sizeof nombre - 1U) {
            nombre[k] = in.modo[k]; k++;
        }
        nombre[k] = '\0';
    }
    r->modo        = nombre[0] ? nombre : "(ninguno)";
    r->lineas      = (g_max_y >= 0) ? (g_max_y + 1) : 0;
    r->sincs_malos = in.sincs_malos;
    r->como        = in.como;
    sstv_stop();
}

/* Que a ESE ruido siga saliendo la foto, y con el modo que es. */
static void exige(uint8_t modo, const char *nombre, int con_vis,
                  double sigma, const char *que)
{
    medida_t r;

    pasada(modo, 60, sigma, con_vis, &r);
    if (r.lineas <= 0 || strcmp(r.modo, nombre) != 0) {
        printf("    MAL: %s (sigma %.2f): modo \"%s\", %d lineas\n",
               que, sigma, r.modo, r.lineas);
        fallos++;
    } else {
        printf("    %-52s ok  (%d lineas)\n", que, r.lineas);
    }
}

static void barre(const char *titulo, uint8_t modo, int con_vis)
{
    const char *esperado = sstv_modo_nombre(modo);
    double tope_bueno = -1.0;
    static const double k_sigma[] = {
        0.00, 0.20, 0.30, 0.40, 0.50, 0.70, 1.00, 1.40, 2.00
    };
    unsigned i;

    printf("\n  %s\n", titulo);
    printf("    sigma   modo visto    de donde   engancha en   lineas   sincs malos\n");
    for (i = 0U; i < sizeof k_sigma / sizeof k_sigma[0]; i++) {
        medida_t r;
        char eng[12];

        pasada(modo, 60, k_sigma[i], con_vis, &r);
        if (r.engancho_en < 0) {
            eng[0] = '-'; eng[1] = '\0';
        } else {
            snprintf(eng, sizeof eng, "%d", r.engancho_en);
        }
        printf("    %5.2f   %-12s  %-9s  %9s   %6d   %11u%s\n",
               k_sigma[i], r.modo,
               (r.como == (uint8_t)SSTV_COMO_VIS)   ? "cabecera"
             : (r.como == (uint8_t)SSTV_COMO_RITMO) ? "ritmo" : "-",
               eng, r.lineas, (unsigned)r.sincs_malos,
               (r.lineas == 0) ? "   <- NI UNA LINEA"
                               : (strcmp(r.modo, esperado) != 0
                                      ? "   <- MODO EQUIVOCADO" : ""));
        /*
         * EL TOPE BUENO EXIGE LAS DOS COSAS: que salgan lineas Y QUE EL MODO
         * SEA EL QUE SE EMITIO. La primera version de este banco solo miraba
         * que hubiera lineas, y con eso Scottie 1 "aguantaba" hasta 0,70...
         * decodificado como Martin 1. Una foto con el modo equivocado sale
         * torcida: es PEOR que no enganchar, porque parece que funciona.
         */
        if (r.lineas > 0 && strcmp(r.modo, esperado) == 0) {
            tope_bueno = k_sigma[i];
        }
    }
    if (tope_bueno < 0.0) {
        printf("    -> no acierta el modo ni sin ruido\n");
    } else {
        printf("    -> con el modo CORRECTO aguanta hasta sigma %.2f\n", tope_bueno);
    }
}

/*
 * EL DESVIO DE SINTONIA, QUE ES EL EJE QUE NINGUN BANCO PRUEBA CON RUIDO.
 *
 * sstv_auto.c lo prueba a sigma 0,05, o sea casi limpio. Y un usuario no
 * sintoniza SSTV con un contador: lo pone de oido en banda lateral, asi que
 * decenas de hercios de error son lo NORMAL. El sincronismo esta a 1200 Hz y
 * el corte en 1350: con +150 el pulso deja de ser un pulso.
 */
static void barre_desvio(const char *titulo, uint8_t modo, double sigma,
                         int con_vis)
{
    static const double k_off[] = { 0.0, 25.0, 50.0, 75.0, 100.0, 125.0, 150.0 };
    const char *esperado = sstv_modo_nombre(modo);
    double tope = -1.0;
    unsigned i;

    printf("\n  %s (sigma %.2f)\n", titulo, sigma);
    printf("    desvio   modo visto    lineas   sincs malos\n");
    for (i = 0U; i < sizeof k_off / sizeof k_off[0]; i++) {
        medida_t r;

        g_desvio = k_off[i];
        pasada(modo, 60, sigma, con_vis, &r);
        g_desvio = 0.0;
        printf("    %+6.0f   %-12s  %6d   %11u%s\n",
               k_off[i], r.modo, r.lineas, (unsigned)r.sincs_malos,
               (r.lineas == 0) ? "   <- NI UNA LINEA"
                               : (strcmp(r.modo, esperado) != 0
                                      ? "   <- MODO EQUIVOCADO" : ""));
        if (r.lineas > 0 && strcmp(r.modo, esperado) == 0) { tope = k_off[i]; }
    }
    if (tope < 0.0) { printf("    -> no acierta ni con la sintonia clavada\n"); }
    else { printf("    -> aguanta hasta %+.0f Hz de desvio\n", tope); }
}

int main(void)
{
    printf("EL SINCRONISMO DE SSTV CONTRA EL RUIDO\n");
    printf("  (la senal es un seno de amplitud 1; el ruido va en multiplos\n"
           "   de esa amplitud. 60 lineas por pasada.)\n");

    barre("MARTIN 1, cayendo encima a mitad de foto (sin cabecera)",  0U, 0);
    barre("MARTIN 1, con cabecera (testigo)",                         0U, 1);
    barre("SCOTTIE 1, cayendo encima a mitad de foto (sin cabecera)", 2U, 0);
    barre("SCOTTIE 1, con cabecera (testigo)",                        2U, 1);

    barre_desvio("MARTIN 1 con cabecera, desvio de sintonia",  0U, 0.20, 1);
    barre_desvio("SCOTTIE 1 con cabecera, desvio de sintonia", 2U, 0.20, 1);
    barre_desvio("MARTIN 1 cayendo encima, desvio de sintonia", 0U, 0.20, 0);

    printf("\n  LOS SUELOS\n");
    exige(0U, "Martin 1",  0, 0.50, "Martin 1 cayendo encima aguanta sigma 0,50");
    exige(0U, "Martin 1",  1, 0.40, "Martin 1 con cabecera aguanta sigma 0,40");
    exige(2U, "Scottie 1", 0, 0.30, "Scottie 1 cayendo encima aguanta sigma 0,30");
    exige(2U, "Scottie 1", 1, 0.40, "Scottie 1 con cabecera aguanta sigma 0,40");

    printf("\n  %s\n", fallos ? "HAY FALLOS" : "TODO CORRECTO (0 fallos)");
    return fallos ? 1 : 0;
}
