/*
 * RWM - 4996 / 9996 / 14996 kHz, Moscu. Ver hora_rwm.h para QUE da esta
 * emisora y, sobre todo, que es lo que NO da.
 *
 * ESTE FICHERO NO INCLUYE NADA DEL GD32 A PROPOSITO, igual que dcf77.c: asi
 * el simulador lo compila tal cual y el banco mide el decodificador de
 * verdad y no una copia.
 */
#include "hora_rwm.h"

/* ---------------------------------------------------------------------
 * LAS DURACIONES, EN MILISEGUNDOS, Y DE DONDE SALEN
 * ---------------------------------------------------------------------
 * De la norma, no de oido:
 *
 *   tren de 1 Hz    pulsos de 100 ms, uno por segundo; el del minuto, 500
 *   tren de 10 Hz   pulsos de 20 ms, diez por segundo; 40 ms el que cae en
 *                   el segundo, 500 el que cae en el minuto
 *   DUT1            el pulso del segundo se DOBLA: sale un segundo pulso
 *                   entre los 200 y los 300 ms de ese segundo
 *
 * Las ventanas de aceptacion son anchas a proposito. El ruido se come los
 * bordes de un pulso por los dos lados, asi que lo medido siempre sale mas
 * corto que lo nominal, nunca mas largo; por eso los minimos estan holgados
 * y los maximos no tanto.
 * --------------------------------------------------------------------- */
#define RWM_P100_MIN_MS    40U    /* el de 100 ms del tren de 1 Hz */
#define RWM_P100_MAX_MS   180U
#define RWM_P20_MIN_MS      6U    /* los de 20 y 40 ms del tren de 10 Hz */
#define RWM_P20_MAX_MS     70U
#define RWM_MIN_MIN_MS    300U    /* el del minuto, 500 ms */
#define RWM_MIN_MAX_MS    700U
#define RWM_PORTA_MS     1500U    /* mas que esto seguido ya es portadora */

/* Separaciones entre principios de pulso. */
#define RWM_SEP_1HZ_MIN_MS   700U
#define RWM_SEP_1HZ_MAX_MS  1300U
#define RWM_SEP_10HZ_MIN_MS   60U
#define RWM_SEP_10HZ_MAX_MS  160U

/* El pulso doblado del DUT1 empieza entre 200 y 300 ms despues del que
 * marca el segundo. Se acepta de 150 a 380 por lo mismo que arriba. */
#define RWM_DOBLE_MIN_MS   150U
#define RWM_DOBLE_MAX_MS   380U

/* Dos subidas separadas por menos de esto son la misma, con un mordisco de
 * ruido en medio. El programa del PC usa 15 ms y es un numero razonable:
 * esta cuatro veces por debajo del pulso mas corto que hay que distinguir
 * -los 20 ms del tren rapido- y bastante por encima de lo que dura un
 * mordisco. */
#define RWM_FUNDE_MS        15U

/* Cuanto hay que estar sin un solo pulso para dejar de creerse el ritmo. Un
 * tren de 1 Hz con un pulso perdido sigue siendo un tren de 1 Hz. */
#define RWM_OLVIDO_MS     3000U

/* ---------------------------------------------------------------------
 * EL SUELO Y EL PICO
 * ---------------------------------------------------------------------
 * El suelo BAJA deprisa y SUBE despacio: lo que interesa es el valle entre
 * pulsos, y un seguidor que suba deprisa se lo come el propio pulso. El
 * pico al reves, que para eso es el pico.
 *
 * Los dos son por bloque de muestra, no por ventana, para no tener que
 * guardar nada: este decodificador corre con cuatro flotantes de estado.
 * --------------------------------------------------------------------- */
#define RWM_PISO_BAJA   0.0040f
#define RWM_PISO_SUBE   0.0002f
#define RWM_PICO_SUBE   0.0500f
#define RWM_PICO_BAJA   0.00003f

/* Donde se pone el corte entre el suelo y el pico, y cuanto hay que caer
 * para considerar que el pulso ha terminado. El 0,6 de salida es la
 * histeresis: sin ella, un pulso cuyo nivel roza el corte se parte en
 * varios y las duraciones salen mal. */
#define RWM_CORTE       0.45f
#define RWM_SALIDA      0.60f

/* Por debajo de esta razon pico/suelo no hay nada que decodificar: ni se
 * intenta, para que el ruido no fabrique pulsos. */
#define RWM_RAZON_MIN   2.0f

/* Cuantas separaciones seguidas hacen falta para creerse un tren. Ver
 * s_racha. */
#define RWM_RACHA       3U

static float    s_fs;
/*
 * LAS TRES CONVERSIONES QUE rwm_feed() NECESITA, PRECALCULADAS.
 *
 * La primera version las sacaba con ms_a_n() DENTRO del bucle de muestra: un
 * multiplicar y un dividir en coma flotante, tres veces, 3.000 veces por
 * segundo y dentro de la interrupcion. Y para convertir constantes, que no
 * cambian. Se calculan en rwm_init() y ya.
 */
static uint32_t s_n_calienta;     /* el cuarto de segundo de arranque */
static uint32_t s_n_funde;        /* RWM_FUNDE_MS en muestras */
static uint32_t s_n_porta;        /* RWM_PORTA_MS en muestras */
static uint32_t s_n_olvido;       /* RWM_OLVIDO_MS en muestras */
static float    s_piso, s_pico;
static uint32_t s_n;              /* muestras desde rwm_init() */

static uint8_t  s_dentro;         /* 1 = dentro de un pulso */
static uint32_t s_ini;            /* donde empezo el pulso de ahora */
static uint32_t s_fin_ant;        /* donde acabo el anterior */
static uint32_t s_ini_ant;        /* donde empezo el anterior */
static uint8_t  s_hay_ant;

static uint8_t  s_estado;
/*
 * EL RITMO HAY QUE GANARSELO, Y ESTO LO DIJO EL BANCO.
 *
 * La primera version declaraba el tren con UNA separacion que cuadrara, y
 * con eso 60 segundos de ruido puro fabricaban 10.391 pulsos y un borde de
 * minuto inventado -sim/rwm.c, prueba 6-. Un borde inventado es lo peor que
 * puede pasar aqui: pone el reloj mal y con cara de estar bien.
 *
 * Asi que hacen falta RWM_RACHA separaciones seguidas del mismo tren, que es
 * la misma regla que sim/sstv_auto.c tiene medida para adivinar el modo de
 * SSTV sin cabecera: tres intervalos que se parezcan entre si. El ruido da
 * separaciones cualesquiera, no tres seguidas de un segundo.
 */
static uint8_t  s_racha;          /* separaciones buenas seguidas */
static uint8_t  s_racha_de;       /* de que tren son */
static uint32_t s_ultimo_pulso;   /* muestra del ultimo pulso valido */
static uint32_t s_marca;          /* muestra del ultimo borde de minuto */
static uint8_t  s_marca_hay;
static uint32_t s_segundo;
static uint32_t s_pulsos;

/* El DUT1 se cuenta por tandas: cuantos segundos doblados han salido en
 * cada una de las cuatro ventanas de la norma. Se publica al llegar al
 * segundo 40, que es cuando las cuatro han pasado. */
static uint8_t  s_dob_pos, s_dob_neg, s_dob_dpos, s_dob_dneg;
static int8_t   s_dut1, s_ddut1;

static uint32_t ms_a_n(uint32_t ms)
{
    return (uint32_t)((s_fs * (float)ms) / 1000.0f + 0.5f);
}

static uint32_t n_a_ms(uint32_t n)
{
    if (s_fs <= 0.0f) { return 0U; }
    return (uint32_t)(((float)n * 1000.0f) / s_fs + 0.5f);
}

void rwm_init(float fs_hz)
{
    s_fs = (fs_hz > 0.0f) ? fs_hz : 1.0f;
    s_n_calienta = ms_a_n(250U);
    s_n_funde    = ms_a_n(RWM_FUNDE_MS);
    s_n_porta    = ms_a_n(RWM_PORTA_MS);
    s_n_olvido   = ms_a_n(RWM_OLVIDO_MS);
    s_piso = 0.0f; s_pico = 0.0f;
    s_n = 0U;
    s_dentro = 0U; s_ini = 0U; s_fin_ant = 0U; s_ini_ant = 0U; s_hay_ant = 0U;
    s_estado = (uint8_t)RWM_BUSCA;
    s_racha = 0U; s_racha_de = (uint8_t)RWM_BUSCA;
    s_ultimo_pulso = 0U;
    s_marca = 0U; s_marca_hay = 0U;
    s_segundo = 0U; s_pulsos = 0U;
    s_dob_pos = 0U; s_dob_neg = 0U; s_dob_dpos = 0U; s_dob_dneg = 0U;
    s_dut1 = RWM_DUT_SIN_MEDIR; s_ddut1 = RWM_DUT_SIN_MEDIR;
}

/*
 * EL BORDE DE MINUTO, que es lo unico que esta emisora da y la radio
 * necesita. Pone el contador de segundos a cero y apunta DONDE cayo, para
 * que rwm_atraso_ms() pueda decir cuanto hace.
 */
static void marca_minuto(uint32_t en)
{
    s_marca = en;
    s_marca_hay = 1U;
    s_segundo = 0U;
    /* Y las cuatro tandas del DUT1 vuelven a empezar: son por minuto. */
    s_dob_pos = 0U; s_dob_neg = 0U; s_dob_dpos = 0U; s_dob_dneg = 0U;
}

/*
 * UN SEGUNDO MAS. En el tren de 1 Hz lo marca cada pulso; en el de 10 Hz,
 * solo el ancho -40 ms contra 20-.
 */
static void pasa_un_segundo(void)
{
    s_segundo++;
    if (s_segundo >= 60U) { s_segundo = 0U; }
    /*
     * EL DUT1 SE PUBLICA AL PASAR LA CUARTA VENTANA, no antes: las de
     * dUT1 son los segundos 21-24 y 31-34, asi que hasta el 35 no estan
     * las cuatro cuentas hechas. Publicarlo a medias daria un numero que
     * cambia solo mientras pasa el minuto.
     */
    if (s_segundo == 40U) {
        if (s_dob_pos != 0U && s_dob_neg == 0U) {
            s_dut1 = (int8_t)s_dob_pos;
        } else if (s_dob_neg != 0U && s_dob_pos == 0U) {
            s_dut1 = (int8_t)(-(int)s_dob_neg);
        } else if (s_dob_pos == 0U && s_dob_neg == 0U) {
            s_dut1 = 0;
        }
        if (s_dob_dpos != 0U && s_dob_dneg == 0U) {
            s_ddut1 = (int8_t)s_dob_dpos;
        } else if (s_dob_dneg != 0U && s_dob_dpos == 0U) {
            s_ddut1 = (int8_t)(-(int)s_dob_dneg);
        } else if (s_dob_dpos == 0U && s_dob_dneg == 0U) {
            s_ddut1 = 0;
        }
    }
}

/*
 * UN PULSO QUE ACABA DE TERMINAR: empezo en `ini` y duro `ancho` muestras.
 * Aqui se decide que es y que dice.
 */
static void pulso(uint32_t ini, uint32_t ancho)
{
    uint32_t ms  = n_a_ms(ancho);
    uint32_t sep = s_hay_ant ? n_a_ms(ini - s_ini_ant) : 0U;

    /* Una manipulacion mas larga que esto no es un pulso del reloj: o es la
     * portadora continua, o es una raya larga del Morse. Ni una ni otra
     * dicen la hora, asi que se descarta y se rompe la racha. */
    if (ms > RWM_PORTA_MS) {
        s_racha = 0U;
        s_hay_ant = 0U;
        return;
    }

    /*
     * EL PULSO DEL MINUTO, 500 ms. Vale en los dos trenes y es el unico que
     * la radio de verdad necesita, asi que se mira el primero.
     */
    if (ms >= RWM_MIN_MIN_MS && ms <= RWM_MIN_MAX_MS) {
        /*
         * *** Y SOLO SI YA HAY RITMO. Ver s_racha. *** Un pulso de medio
         * segundo lo fabrica el ruido sin despeinarse; lo que el ruido no
         * fabrica es un pulso de medio segundo DENTRO de un tren que ya
         * venia marcando el compas. El programa de PC de referencia hace lo
         * mismo: solo busca el marcador mientras cree estar en uno de los
         * dos trenes.
         */
        if (s_estado == (uint8_t)RWM_1HZ || s_estado == (uint8_t)RWM_10HZ) {
            marca_minuto(ini);
        }
        s_ultimo_pulso = ini;
        s_pulsos++;
        s_ini_ant = ini; s_fin_ant = ini + ancho; s_hay_ant = 1U;
        return;
    }

    /*
     * EL PULSO DOBLADO DEL DUT1, que NO es un segundo: cae dentro del
     * segundo que acaba de empezar, entre 200 y 300 ms de su principio. Se
     * reconoce por la separacion respecto del anterior, no por su ancho,
     * porque mide lo mismo que el otro.
     */
    if (s_estado == (uint8_t)RWM_1HZ && s_hay_ant
        && sep >= RWM_DOBLE_MIN_MS && sep <= RWM_DOBLE_MAX_MS
        && ms >= RWM_P100_MIN_MS && ms <= RWM_P100_MAX_MS) {
        uint32_t sg = s_segundo;

        if (sg >= 1U  && sg <= 8U)  { s_dob_pos++; }
        else if (sg >= 9U  && sg <= 16U) { s_dob_neg++; }
        else if (sg >= 21U && sg <= 24U) { s_dob_dpos++; }
        else if (sg >= 31U && sg <= 34U) { s_dob_dneg++; }
        s_pulsos++;
        s_ini_ant = ini; s_fin_ant = ini + ancho; s_hay_ant = 1U;
        return;
    }

    /*
     * LOS DOS TRENES, Y LOS DECIDE LA SEPARACION - NO EL ANCHO.
     *
     * *** Y ESTO LO CAZO EL BANCO. *** La primera version miraba el ancho
     * primero, con la ventana del tren lento de 40 a 180 ms y la del rapido
     * de 6 a 70: se solapan entre 40 y 70, y justo ahi cae el pulso de 40 ms
     * que marca el segundo en el tren de 10 Hz. Como el lento se probaba
     * antes, ese pulso se iba por su rama y el contador de segundos del tren
     * rapido se quedaba clavado en cero -sim/rwm.c, prueba 3-, con el tren
     * reconocido y el borde de minuto encontrado. Un fallo que no se ve: todo
     * lo demas funcionaba.
     *
     * Estrecharlas para que no se solapen seria peor: el ruido se come los
     * bordes y un pulso de 100 ms puede medir 60. Lo que NO se mueve con el
     * ruido es la separacion -un segundo es un segundo, y una decima es una
     * decima-, asi que manda ella y el ancho solo confirma.
     */
    if (s_hay_ant && sep >= RWM_SEP_10HZ_MIN_MS && sep <= RWM_SEP_10HZ_MAX_MS
        && ms >= RWM_P20_MIN_MS && ms <= RWM_P20_MAX_MS) {
        if (s_racha_de != (uint8_t)RWM_10HZ) { s_racha = 0U; }
        s_racha_de = (uint8_t)RWM_10HZ;
        if (s_racha < 255U) { s_racha++; }
        if (s_racha >= RWM_RACHA) {
            s_estado = (uint8_t)RWM_10HZ;
            /* El de 40 ms marca el segundo; los de 20, no. Se parte por la
             * mitad de la ventana. */
            if (ms >= 30U) { pasa_un_segundo(); }
        }
        s_ultimo_pulso = ini;
        s_pulsos++;
        s_ini_ant = ini; s_fin_ant = ini + ancho; s_hay_ant = 1U;
        return;
    }

    if (s_hay_ant && sep >= RWM_SEP_1HZ_MIN_MS && sep <= RWM_SEP_1HZ_MAX_MS
        && ms >= RWM_P100_MIN_MS && ms <= RWM_P100_MAX_MS) {
        if (s_racha_de != (uint8_t)RWM_1HZ) { s_racha = 0U; }
        s_racha_de = (uint8_t)RWM_1HZ;
        if (s_racha < 255U) { s_racha++; }
        if (s_racha >= RWM_RACHA) {
            s_estado = (uint8_t)RWM_1HZ;
            pasa_un_segundo();
        }
        s_ultimo_pulso = ini;
        s_pulsos++;
        s_ini_ant = ini; s_fin_ant = ini + ancho; s_hay_ant = 1U;
        return;
    }

    /*
     * Un pulso del ancho de uno de los dos trenes pero a una separacion que
     * no es de ninguno: se apunta como referencia -hace falta un primer
     * pulso para poder medir la separacion del siguiente- pero no suma
     * racha. Es lo que pasa con el primero de todos y con el que llega
     * despues de un hueco.
     */
    if (ms <= RWM_P100_MAX_MS) {
        s_racha = 0U;
        s_ultimo_pulso = ini;
        s_pulsos++;
        s_ini_ant = ini; s_fin_ant = ini + ancho; s_hay_ant = 1U;
        return;
    }

    /* Cualquier otra duracion es manipulacion que no cuadra con el reloj:
     * la identificacion en Morse cae aqui, y tambien el ruido. */
    s_estado = (uint8_t)RWM_MORSE;
    s_ini_ant = ini; s_fin_ant = ini + ancho; s_hay_ant = 1U;
}

void rwm_feed(float muestra)
{
    float v = (muestra < 0.0f) ? -muestra : muestra;
    float corte, salida, razon;

    s_n++;

    /* El suelo y el pico, los dos con ataque y caida distintos. Ver sus
     * constantes. */
    if (v < s_piso) { s_piso += RWM_PISO_BAJA * (v - s_piso); }
    else            { s_piso += RWM_PISO_SUBE * (v - s_piso); }
    if (v > s_pico) { s_pico += RWM_PICO_SUBE * (v - s_pico); }
    else            { s_pico += RWM_PICO_BAJA * (v - s_pico); }

    /* Los primeros instantes no valen: los seguidores arrancan en cero y
     * cualquier cosa parece un pulso. Un cuarto de segundo basta. */
    if (s_n < s_n_calienta) { return; }

    razon = (s_piso > 1.0e-9f) ? (s_pico / s_piso) : 0.0f;
    if (razon < RWM_RAZON_MIN) {
        /* Nada que sacar de aqui. Si veniamos de un tren, se olvida solo
         * por el tiempo sin pulsos, abajo. */
        if (s_dentro) { s_dentro = 0U; }
    } else {
        corte  = s_piso + RWM_CORTE * (s_pico - s_piso);
        salida = s_piso + (RWM_CORTE * RWM_SALIDA) * (s_pico - s_piso);

        if (!s_dentro) {
            if (v >= corte) {
                /*
                 * Y SI LA SUBIDA CAE PEGADA A LA BAJADA ANTERIOR, es el
                 * MISMO pulso con un mordisco de ruido en medio: se
                 * reanuda el de antes en vez de abrir uno nuevo. Sin esto
                 * un pulso de 100 ms partido en dos sale como dos de 40 y
                 * ni uno de los dos cuadra con nada.
                 */
                if (s_hay_ant && (s_n - s_fin_ant) <= s_n_funde
                    && s_ini_ant != 0U) {
                    s_ini = s_ini_ant;
                    s_hay_ant = 0U;   /* ya no cuenta como pulso cerrado */
                } else {
                    s_ini = s_n;
                }
                s_dentro = 1U;
            }
        } else {
            if (v <= salida) {
                s_dentro = 0U;
                pulso(s_ini, s_n - s_ini);
            } else if ((s_n - s_ini) > s_n_porta) {
                /* Lleva encendido demasiado: portadora. Se cierra para no
                 * contar un pulso de minuto que no lo es. */
                s_dentro = 0U;
                s_racha = 0U;
                s_hay_ant = 0U;
            }
        }
    }

    /* EL OLVIDO: sin pulsos durante RWM_OLVIDO_MS ya no hay ritmo que
     * conservar. No se borra la marca de minuto -esa sigue valiendo para
     * decir cuanto hace- sino el estado. */
    if (s_ultimo_pulso != 0U
        && (s_n - s_ultimo_pulso) > s_n_olvido) {
        if (s_estado == (uint8_t)RWM_1HZ || s_estado == (uint8_t)RWM_10HZ) {
            s_estado = (uint8_t)RWM_CALLADO;
        }
        s_racha = 0U;
        s_hay_ant = 0U;
    }
}

void rwm_info(rwm_info_t *out)
{
    float razon;
    uint32_t n;

    if (!out) { return; }
    out->estado    = s_estado;
    out->marca_hay = s_marca_hay;
    out->segundo   = s_segundo;
    out->pulsos    = s_pulsos;
    out->dut1      = s_dut1;
    out->ddut1     = s_ddut1;
    out->marca_hace_ms = rwm_atraso_ms();

    /* El nivel con la misma escala que HFDL -(razon - 1) x 12, topado a
     * 99-, para que los dos renglones de la radio se lean igual. */
    razon = (s_piso > 1.0e-9f) ? (s_pico / s_piso) : 0.0f;
    if (razon <= 1.0f) {
        out->nivel = 0U;
    } else {
        n = (uint32_t)((razon - 1.0f) * 12.0f);
        out->nivel = (uint8_t)((n > 99U) ? 99U : n);
    }
}

uint32_t rwm_atraso_ms(void)
{
    if (!s_marca_hay) { return 0U; }
    return n_a_ms(s_n - s_marca);
}
