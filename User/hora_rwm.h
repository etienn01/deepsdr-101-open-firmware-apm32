/* -----------------------------------------------------------------------
 * RWM - 4996 / 9996 / 14996 kHz, Moscu. 13/11/2026.
 *
 * Pedido por el dueño del proyecto, y avisado por un usuario que tiene un
 * programa de PC para esto.
 *
 * *** LO PRIMERO, PORQUE CAMBIA LO QUE ESTE FICHERO PUEDE PROMETER: RWM NO
 * MANDA LA HORA DEL DIA. *** Lo dice su propia norma -GOST 8.323-2016- y lo
 * confirma el programa del usuario, que no descifra ninguna hora: lo que
 * hace es coger el borde de minuto y encajarle la fase del reloj que YA
 * TIENE el ordenador, y si el error pasa de treinta segundos se rinde y
 * manda a NTP. La hora del dia sale de internet, no de la radio.
 *
 * Asi que RWM no es como las seis de dcf77.c -ni como el RDS-: de aquellas
 * sale hh:mm y fecha, y de esta sale UN BORDE. Lo que da, y lo da muy bien:
 *
 *   el borde de segundo   con los trenes de 1 Hz y de 10 Hz
 *   el borde de MINUTO    con el pulso largo de 500 ms
 *   el DUT1 y el dUT1     con los pulsos doblados del tren de 1 Hz
 *
 * Con eso la radio puede ANCLAR el minuto y el segundo -que es lo que hace
 * reloj_anclar() para el RDS y para las cinco horarias- conservando la hora
 * y la fecha que ya tengas. No mas, y se dice en la pantalla.
 *
 * EL CICLO, QUE SE REPITE CADA MEDIA HORA:
 *
 *   :00 y :30   7:55   portadora continua, sin modular
 *   :08 y :38   1:00   transmisor apagado
 *   :09 y :39   0:55   identificacion en Morse, "RWM RWM RWM..."
 *   :10 y :40   9:55   tren de 1 Hz. El pulso del minuto son 500 ms, los
 *                      demas 100, y se doblan para codificar el DUT1
 *   :20 y :50   9:55   tren de 10 Hz, pulsos de 20 ms. 40 ms en el segundo
 *                      y 500 ms en el minuto
 *
 * POR QUE AQUI NO HAY PASO BANDA NI TONO DE AUDIO. El programa del PC se
 * come un filtro por FFT alrededor de 2000 Hz en cada ventana porque recibe
 * en banda lateral y la portadora le llega como un tono. La radio no tiene
 * ese problema: RWM es A1A -portadora conmutada- y en AM la conmutacion ES
 * la envolvente, que es justo lo que demod_am.c ya le entrega a los
 * decodificadores horarios. El trabajo caro no hay que hacerlo.
 *
 * COMO SE ALIMENTA. Igual que las seis de dcf77.c: rwm_feed() con la
 * envolvente promediada por bloques, desde la interrupcion. La tabla de
 * emisoras pide `sub` muestras por bloque; con 8 salen 3.000 por segundo,
 * cada una promedio de 32 crudas, o sea 24 dB de ruido menos de regalo. A
 * esa tasa un pulso de 20 ms son 60 muestras y el del minuto 1.500.
 * ----------------------------------------------------------------------- */
#ifndef HORA_RWM_H
#define HORA_RWM_H

#include <stdint.h>

/* En que tramo del ciclo cree que esta. Se deduce del RITMO de los pulsos,
 * no del reloj: el programa del PC lo saca de `dt.minute % 30`, o sea del
 * reloj que viene a corregir, y eso aqui no vale porque el reloj de la radio
 * puede estar en cualquier sitio. */
typedef enum {
    RWM_BUSCA = 0,    /* nada reconocible todavia */
    /*
     * NO HAY ESTADO DE "PORTADORA". Una portadora encendida y quieta no
     * tiene contraste: el suelo sube hasta ella y la razon pico/suelo se va
     * a uno, que es lo mismo que ve este modulo cuando no hay nada.
     * Separar "portadora" de "nada" pide una referencia ABSOLUTA -el
     * medidor de señal- y eso lo tiene la pantalla, no este fichero. Se
     * intento al reves y sim/rwm.c lo tumbo.
     */
    RWM_CALLADO,      /* ni portadora ni pulsos: :08-:09 o :38-:39 */
    RWM_MORSE,        /* manipulacion irregular: la identificacion */
    RWM_1HZ,          /* tren de un pulso por segundo: :10-:20 o :40-:50 */
    RWM_10HZ          /* tren de diez por segundo: :20-:30 o :50-:00 */
} rwm_estado_t;

#define RWM_DUT_SIN_MEDIR  ((int8_t)127)

typedef struct {
    uint8_t  estado;        /* rwm_estado_t */
    uint8_t  nivel;         /* 0..99, cuanto sobresale del suelo de ruido */
    uint8_t  marca_hay;     /* 1 = se ha visto un borde de minuto */
    uint32_t marca_hace_ms; /* cuanto hace del ultimo borde de minuto */
    uint32_t segundo;       /* segundos contados desde ese borde, 0..59 */
    uint32_t pulsos;        /* cuantos pulsos validos lleva */
    int8_t   dut1;          /* DUT1 en decimas de segundo, -8..+8 */
    int8_t   ddut1;         /* dUT1 en unidades de 0,02 s, -4..+4 */
} rwm_info_t;

/* fs_hz = a cuantas muestras por segundo se va a llamar a rwm_feed(). */
void rwm_init(float fs_hz);
void rwm_feed(float muestra);
void rwm_info(rwm_info_t *out);

/* Los milisegundos transcurridos desde el ultimo borde de minuto, para que
 * quien aplique la hora pueda restarlos igual que hace con el RDS. 0 si no
 * hay ninguno. Ver reloj_anclar(). */
uint32_t rwm_atraso_ms(void);

#endif /* HORA_RWM_H */
