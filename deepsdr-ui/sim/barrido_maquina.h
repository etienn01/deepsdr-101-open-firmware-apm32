/* GENERADO por tools/barrido_dump.py desde User/main.c. No se
 * edita: lo que valga se cambia alli. */

#include "barrido_consts.h"

static float scan_lee(void)
{
    demod_mode_t md = demod_am_get_mode();

    if (md == DEMOD_MODE_WFM) { return demod_wfm_get_sql_db(); }
    if (md == DEMOD_MODE_AM || md == DEMOD_MODE_NFM) {
        return demod_am_get_squelch_level_db();
    }
    return demod_am_get_nivel_db();
}

static void scan_mira(void)
{
    float v = scan_lee();

    if (demod_am_get_mode() == DEMOD_MODE_WFM) {
        if (v < s_scan_pico) { s_scan_pico = v; }   /* el ruido MAS bajo */
    } else {
        if (v > s_scan_pico)  { s_scan_pico  = v; } /* el nivel MAS alto */
        if (v < s_scan_valle) { s_scan_valle = v; } /* y el MAS callado */
    }
}

static uint8_t mem_hay_senal(uint8_t c, uint8_t sube_ok)
{
    float sube;

    if (demod_am_get_mode() == DEMOD_MODE_WFM) {
        return (uint8_t)(s_scan_pico < SCAN_WFM_RUIDO_DB);
    }
    /*
     * El suelo sale de un sitio o de otro, pero la cuenta es LA MISMA: con
     * el rango hay un solo suelo movil y con el banco uno por memoria. Se
     * coge la direccion y se sigue igual, que es mejor que dos copias de
     * tres lineas que un dia se separan. Ver s_rng_piso.
     */
    {
        float *piso = s_rng_on ? &s_rng_piso : &s_scan_piso[c];

        if (!s_rng_on && c >= BANCO_MEM_N) { return 0U; }

        /* Baja de golpe y sube a cuentagotas, los dos con el mismo minimo:
         * el suelo no puede quedar por encima de lo mas callado que se acaba
         * de oir aqui. */
        sube = sube_ok ? (*piso + SCAN_PISO_SUBE_DB) : *piso;
        *piso = (s_scan_valle < sube) ? s_scan_valle : sube;

        return (uint8_t)(s_scan_pico >= (*piso + SCAN_MARGEN_DB));
    }
}

static void scan_ventana(uint32_t espera_ms)
{
    s_scan_ms    = g_msticks;
    s_scan_espera = espera_ms;
    s_scan_pico  = (demod_am_get_mode() == DEMOD_MODE_WFM) ? 9999.0f : -9999.0f;
    s_scan_valle = 9999.0f;
}

static void mem_scan_para(void)
{
    uint8_t era_rango = s_rng_on;

    if (!s_scan_on) { return; }
    s_scan_on      = 0U;
    s_rng_on       = 0U;
    s_scan_para_en = 0U;
    /* Y se apaga la medida del nivel, que solo existe para esto: ver
     * demod_am_set_nivel_medir(), y por que importa apagarla. */
    demod_am_set_nivel_medir(0U);
    /*
     * Y AQUI ES DONDE SE GUARDA MEM.CSV, no en cada hallazgo.
     *
     * Escribir el fichero son dos borrados de 4 kB por un bus a patadas,
     * y un barrido de tramo puede encontrar algo cada pocos segundos: a
     * cada hallazgo serian parones continuos y desgaste de la flash para
     * nada, porque hasta que no se para el barrido nadie va a mirar la
     * lista. Al parar se escribe una vez y ya esta.
     *
     * LA RADIO SE QUEDA DONDE ESTE: no se recupera la frecuencia de antes
     * de entrar en el escaner. Si el barrido se para es porque has tocado
     * MEM mientras se oia algo, o sea que lo que quieres es quedarte ahi.
     *
     * El buffer se lo presta s_gsv_buf, igual que mem_graba(): ver el
     * contrato en banco.h.
     */
    if (era_rango) {
        (void)banco_guarda((char *)s_gsv_buf, (uint32_t)sizeof s_gsv_buf);
    }
    mem_boton_repinta();
    mem_rejilla_repinta();
}

static void mem_scan_salta(void)
{
    uint8_t sig;
    uint8_t rehace;

    if (s_rng_on) { rng_scan_salta(); return; }

    sig = banco_siguiente(s_scan_banco, s_scan_mem);
    if (sig == 0xFFU) { mem_scan_para(); return; }
    s_scan_mem     = sig;
    s_scan_para_en = 0U;
    /* Primero se pone la memoria y DESPUES se abre la ventana, que asi
     * empieza a contar cuando la cadena ya ha cambiado y no antes. Ver
     * scan_ventana(), que ademas necesita saber el modo que ha quedado. */
    rehace = mem_recupera(s_scan_banco, sig);
    /* Lo que tarde en poder contestar depende de si la cadena se ha
     * rehecho, y de si lo que ha entrado es FM ancha: ver SCAN_MIRA_MS. */
    scan_ventana(!rehace ? SCAN_ESPERA_MS
                 : ((demod_am_get_mode() == DEMOD_MODE_WFM)
                    ? SCAN_ESPERA_WFM_MS : SCAN_ESPERA_MODO_MS));
    /* El numero del boton cambia en CADA salto, asi que esto si va por
     * salto. Es la ventana del boton y nada mas -72x32 px-, o sea del
     * orden de 2.400 accesos al bus frente a los 89.771 de la cabecera
     * entera. Ver ui_top_draw_mem(). */
    mem_boton_repinta();
    mem_rejilla_repinta();
}

static void mem_scan_conmuta(uint8_t bk)
{
    if (s_scan_on && s_scan_banco == bk) { mem_scan_para(); return; }
    /*
     * LA CELDA SCAN DEL BANCO SCANNER HACE LO DE SIEMPRE - 11/11/2026,
     * dicho por el dueno. Aqui hubo un rato en que ese banco, si tenia un
     * tramo fijado, barria EL TRAMO en vez de sus memorias. Era un atajo
     * y estaba mal: esa celda barre memorias en los cuatro bancos, y el
     * tramo se arranca desde su propio modo. Dos cosas distintas con el
     * mismo boton es como se pierde la cuenta de lo que hace cada cual.
     */
    if (banco_usadas(bk) == 0U) {
        /* Un banco vacio no se barre, y se dice con el pip de "no ha
         * podido ser" en vez de no hacer nada: un boton que no responde se
         * vive como que no ha entrado el toque. */
        bip_error();
        return;
    }
    s_scan_on      = 1U;
    /*
     * Y SE ENCIENDE LA MEDIDA DEL NIVEL, que fuera del barrido no se mide.
     *
     * Son 256 multiplicaciones y un logaritmo por bloque, y dejarlas
     * corriendo siempre le quitaba el margen que le queda a STANAG cuando
     * esta enganchado a una señal: la radio se saturaba y dejaba de
     * responder. Lo vio el usuario. Ver demod_am_set_nivel_medir().
     */
    demod_am_set_nivel_medir(1U);
    s_scan_banco   = bk;
    s_scan_mem     = 0xFFU;
    /* Cada barrido aprende sus propios suelos: los de la vez anterior eran
     * de otra hora y otro automatico de ganancia. 9999 = sin visitar, que
     * en la primera visita deja el suelo en el valle de esa visita. */
    {
        uint8_t i;

        for (i = 0U; i < BANCO_MEM_N; i++) { s_scan_piso[i] = 9999.0f; }
    }
    mem_scan_salta();
    /*
     * Y SE VUELVE A LA VENTANA PRINCIPAL - 26/10/2026, pedido por el
     * usuario. Mientras busca, lo que se quiere delante es el espectro y
     * la cascada, no la rejilla del banco: ver el salto y lo que cuesta
     * en mem_recupera(). Se cierra DESPUES del primer salto para no
     * repintar la rejilla que esta a punto de desaparecer.
     *
     * Por donde va lo dice el boton MEM de la cabecera, que pone el
     * numero de la memoria; y tocarlo para el barrido. Ver
     * ui_top_state_t.mem_scan.
     */
    menu_screen_close();
}

static void mem_scan_poll(void)
{
    uint8_t hay;

    if (!s_scan_on) { return; }

    /*
     * Y SE PARA SOLO EN LOS MODOS DE PANEL ENTERO - 29/10/2026, tarde,
     * revision a fondo de HFDL.
     *
     * No habia ni una llamada a mem_scan_para() en ningun camino de
     * cambio de modo: los unicos frenos eran tocar MEM, reconmutar el
     * mismo banco y quedarse el banco sin memorias. Asi que arrancabas el
     * barrido, elegias HFDL en la rejilla de modos y el barrido seguia
     * vivo, saltando de memoria cada SCAN_ESPERA_MS + SCAN_MIRA_MS. El
     * dial se iba de la frecuencia DOS VECES Y MEDIA POR SEGUNDO, y una
     * rafaga de HFDL dura un segundo. Lo mismo al reves: desde HFDL se
     * puede abrir el banco y arrancarlo desde dentro.
     *
     * La guarda va AQUI y no en los sitios que cambian de modo, y es
     * deliberado: en el latido no hay forma de rodearla. Poniendola en
     * los cambios de modo habria que acordarse en cada camino nuevo, que
     * es exactamente como se escapo esto.
     *
     * Barrer no tiene sentido en estos ocho: todos viven en un canal
     * fijo, el panel ocupa la pantalla entera y el nivel que mira el
     * barrido -demod_am_set_nivel_medir()- cuesta 256 multiplicaciones y
     * un logaritmo por bloque, que es lo que ya ahogo a la radio en
     * STANAG.
     */
    if (modo_panel_entero()) { mem_scan_para(); return; }
    /* Un rato de gracia para que el silenciador decida: la cadena acaba de
     * cambiar de frecuencia y de modo, y el silencio de entrada de FM ancha
     * son doscientos bloques. Preguntar antes es preguntar al transitorio. */
    {
        uint32_t t = (uint32_t)(g_msticks - s_scan_ms);

        if (t < s_scan_espera) { return; }

        /* El cuarto de segundo de mirar: se apunta lo mejor que se vea, en
         * cada vuelta del bucle. Una voz tiene pausas, asi que preguntar
         * una sola vez al final se perderia media emisora. */
        scan_mira();
        /* El tramo mira menos: ver SCAN_RNG_MIRA_MS y la medida del
         * asentamiento que hay delante. */
        if (t < (uint32_t)(s_scan_espera
                           + (s_rng_on ? SCAN_RNG_MIRA_MS : SCAN_MIRA_MS))) {
            return;
        }
    }

    /*
     * EL TRAMO SE QUEDA UN RATO FIJO Y SIGUE, haya o no haya todavia
     * alguien hablando. Ver SCAN_RNG_PARADA_MS: con la regla del banco
     * -seguir cuando el canal lleve callado de seguido- una portadora
     * permanente para el barrido para siempre.
     *
     * Va ANTES de medir a proposito: lo que decida la ventana de ahora ya
     * no cambia nada, el rato se ha cumplido y toca saltar.
     */
    if (s_rng_on && s_scan_para_en
        && (uint32_t)(g_msticks - s_scan_oida_ms) >= SCAN_RNG_PARADA_MS) {
        mem_scan_salta();
        return;
    }

    /* Vigilando, el suelo no puede subir: ver mem_hay_senal(). */
    hay = mem_hay_senal(s_scan_mem, (uint8_t)(s_scan_para_en ? 0U : 1U));

    if (hay) {
        if (!s_scan_para_en) {
            /* Recien encontrada: se queda y lo dice. El rotulo de la
             * rejilla pasa de "barriendo" a "escuchando", y por eso se
             * repinta aqui: la rejilla solo se repintaba en los saltos, y
             * quedarse ya no es un salto. */
            s_scan_para_en = 1U;
            /* Y si lo que corre es el barrido del tramo, aqui es donde se
             * apunta: una vez por emisora, no una por ventana. Ver
             * rng_apunta(). */
            if (s_rng_on) { rng_apunta(); }
            /* El reloj de la parada empieza AQUI, en el instante de
             * pararse, y no se vuelve a tocar: ver SCAN_RNG_PARADA_MS y
             * el comentario de mas abajo. En el banco esta misma variable
             * se pone al dia en cada ventana y significa otra cosa. */
            s_scan_oida_ms = g_msticks;
            bip_listo();
            mem_rejilla_repinta();
        }
        /*
         * En el banco, esto es "la ultima vez que se oyo algo" y se pone
         * al dia en cada ventana: de ahi sale el hueco entre dos frases.
         * En el tramo es "CUANDO ME PARE" y NO se toca mas, porque lo que
         * se cuenta es el rato que lleva parado, no el silencio. Es la
         * misma variable con dos sentidos, y se dice aqui para que nadie
         * la ponga al dia en el tramo sin querer.
         */
        if (!s_rng_on) { s_scan_oida_ms = g_msticks; }
        /* Y se sigue vigilando el mismo canal: otra ventana aqui mismo,
         * sin espera, que la cadena no se ha movido. */
        scan_ventana(0U);
        return;
    }

    if (s_scan_para_en) {
        /*
         * Callado, pero no por eso libre todavia: un hueco entre dos
         * frases dura menos que SCAN_VUELVE_MS, y lo que se mide es
         * cuanto lleva callado DE SEGUIDO. Mientras no llegue, se sigue
         * vigilando aqui.
         */
        if ((uint32_t)(g_msticks - s_scan_oida_ms) < SCAN_VUELVE_MS) {
            scan_ventana(0U);
            return;
        }
        /* Ya esta libre: el barrido sigue por donde iba. Sin pip, que
         * esto no es un hallazgo. */
    }
    /* El suelo de esta memoria ya lo ha puesto al dia mem_hay_senal()
     * antes de decidir: ver alli por que va en ese orden. */
    mem_scan_salta();
}

