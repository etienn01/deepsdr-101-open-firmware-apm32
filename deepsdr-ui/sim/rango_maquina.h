/* GENERADO por tools/barrido_dump.py desde User/main.c. No se
 * edita: lo que valga se cambia alli. */

static uint8_t rng_pon_mhz(char *d, uint32_t hz)
{
    uint32_t mhz = hz / 1000000UL;
    uint32_t khz = (hz / 1000UL) % 1000UL;
    uint8_t  j = 0U;

    if (mhz >= 100UL) { d[j++] = (char)('0' + (mhz / 100UL) % 10UL); }
    if (mhz >= 10UL)  { d[j++] = (char)('0' + (mhz / 10UL) % 10UL); }
    d[j++] = (char)('0' + mhz % 10UL);
    d[j++] = ',';
    d[j++] = (char)('0' + (khz / 100UL) % 10UL);
    d[j++] = (char)('0' + (khz / 10UL) % 10UL);
    d[j++] = (char)('0' + khz % 10UL);
    return j;
}

static uint32_t rng_paso_hz(void)
{
    uint32_t p = k_tune_steps[s_tune_step_idx];

    /* Un paso de 10 Hz sobre un megahercio son cien mil puntos a casi
     * medio segundo cada uno: catorce horas. No se prohibe -puede ser lo
     * que alguien quiera para un tramo de dos kilohercios- pero el suelo
     * de 100 Hz evita el caso absurdo de dejarlo en 10 por descuido. */
    return (p < 100UL) ? 100UL : p;
}

static void rng_scan_salta(void)
{
    uint32_t paso = rng_paso_hz();
    uint32_t f    = s_tune_hz;

    s_scan_para_en = 0U;
    if (f < s_rng_lo || f >= s_rng_hi) { f = s_rng_lo; }
    else                               { f += paso; }
    if (f > s_rng_hi) { f = s_rng_lo; }

    s_tune_hz = f;
    /* El modo NO se toca: dentro de un tramo es el que haya puesto el
     * usuario. Asi que esto es el camino corto -oscilador y numero de la
     * cabecera- y no mem_recupera(), que ademas rehace filtro y paso. */
    apply_lo_tune(s_tune_hz);
    freq_display_draw();
    /* Sin cambio de modo la cadena no se rehace, y ademas el tramo corre
     * con sus propios tiempos: ver SCAN_RNG_ESPERA_MS. */
    scan_ventana(SCAN_RNG_ESPERA_MS);
    mem_boton_repinta();
    mem_rejilla_repinta();
}

static void rng_apunta(void)
{
    uint32_t hz = s_tune_hz;
    uint8_t  i;

    i = banco_busca_hz(BANCO_SCANNER, hz, rng_paso_hz());
    if (i != 0xFFU) { banco_vista(BANCO_SCANNER, i); return; }

    i = banco_libre(BANCO_SCANNER);
    if (i == 0xFFU) { i = banco_menos_oida(BANCO_SCANNER); }
    if (i == 0xFFU) { return; }      /* lleno y todo a mano: no se toca nada */

    {
        banco_mem_t m;

        memset(&m, 0, sizeof m);
        m.hz     = hz;
        m.modo   = (uint8_t)demod_am_get_mode();
        m.paso   = s_tune_step_idx;
        m.vistas = 1U;
        (void)banco_graba(BANCO_SCANNER, i, &m);
    }
}

static void rng_scan_conmuta(void)
{
    if (s_scan_on && s_rng_on) { mem_scan_para(); return; }
    if (s_rng_lo == 0UL || s_rng_hi <= s_rng_lo) { bip_error(); return; }

    s_scan_on    = 1U;
    s_rng_on     = 1U;
    s_rng_piso   = 9999.0f;          /* cada barrido aprende el suyo */
    s_scan_banco = (uint8_t)BANCO_SCANNER;
    s_scan_mem   = 0xFFU;
    demod_am_set_nivel_medir(1U);
    /*
     * EMPIEZA POR EL PRINCIPIO DEL TRAMO, SIEMPRE - 11/11/2026.
     *
     * Aqui se llamaba a rng_scan_salta() y ya esta, confiando en que "la
     * frecuencia de ahora casi siempre cae fuera del tramo". Lo cazo
     * sim/escaner.c al primer intento: arrancando con el dial puesto
     * JUSTO en el principio -que es lo que pasa si acabas de teclearlo-,
     * la frecuencia cae dentro, el salto suma un paso y el primer punto
     * del tramo no se visita hasta dar la vuelta entera.
     *
     * No es grave y se arregla solo al cabo de una vuelta, pero "empieza
     * por donde le dije" es de las pocas cosas que un escaner tiene que
     * hacer sin sorpresas. Se pone el dial a mano y se abre la ventana
     * aqui, que es lo mismo que hace el salto menos el avance.
     */
    s_tune_hz      = s_rng_lo;
    s_scan_para_en = 0U;
    apply_lo_tune(s_tune_hz);
    freq_display_draw();
    scan_ventana(SCAN_RNG_ESPERA_MS);
    mem_boton_repinta();
    mem_rejilla_repinta();
    menu_screen_close();
}

