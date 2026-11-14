#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
QUE EL BOTON "APLICAR" DE LA HORA ESCRIBA EL RTC ANTES DE ANCLAR.

  python3 tools/hora_aplica_check.py User/main.c

POR QUE EXISTE - 13/11/2026.

El mismo probador que cazo el fallo de los segundos en ALS162 lo volvio a
cazar en RDS: la hora se ensena con segundos, pero al pulsar Aplicar el
reloj se quedaba con el HH:MM anterior.

Y era el ORDEN de dos llamadas. Las dos escriben el RTC:

  rtc_pon_fecha_hora(dd, mo, aa, hh, mi, 0U)   la fecha, y la hora del borde
                                               de minuto CON LOS SEGUNDOS A
                                               CERO
  reloj_anclar(minutos, atraso_ms)             el desfase de verdad, y
                                               termina escribiendo el RTC
                                               con SU SEGUNDO

Asi que la que va detras manda. Puesta la fecha primero, el anclaje la
conserva -rtc_pon_hora() lee el RTC y solo cambia h:m:s- y el reloj queda
clavado. Al reves, el anclaje dura lo que tarda la linea siguiente en
machacarlo.

La rama de las cinco emisoras horarias lo lleva escrito desde el 29/10/2026
-el orden importa, y la fecha va primero- y ademas AFIRMABA que la rama del
RDS tenia el mismo cuidado. No lo tenia, y nadie lo miraba. Un comentario no es
una comprobacion.

QUE COMPRUEBA: que dentro del manejador de UIH_HIT_APLICAR, ninguna llamada
a reloj_anclar() tenga una llamada a rtc_pon_fecha_hora() DETRAS dentro de
su misma rama. No cuenta cuantas ramas hay ni en que orden: una rama que no
escriba fecha -RWM no trae fecha- pasa sin mas.
"""
import re
import sys

RTC = 'rtc_pon_fecha_hora('
ANC = 'reloj_anclar('


def sin_comentarios(txt):
    """Los dos tipos de comentario, sustituidos por espacios para que las
    posiciones no se muevan: asi los numeros de linea siguen valiendo."""
    def blanquea(m):
        return re.sub(r'[^\n]', ' ', m.group(0))
    txt = re.sub(r'/\*.*?\*/', blanquea, txt, flags=re.S)
    txt = re.sub(r'//[^\n]*', blanquea, txt)
    return txt


def bloque_aplicar(txt):
    """El cuerpo del `else if (k == UIH_HIT_APLICAR) {`, con sus llaves."""
    m = re.search(r'\bk\s*==\s*UIH_HIT_APLICAR\b[^\{]*\{', txt)
    if not m:
        return None, None
    ini = txt.index('{', m.start())
    d = 0
    for j in range(ini, len(txt)):
        if txt[j] == '{':
            d += 1
        elif txt[j] == '}':
            d -= 1
            if d == 0:
                return ini, j
    return None, None


def hasta_cerrar_la_rama(txt, desde, fin):
    """Desde `desde`, hasta que se cierra la llave que CONTIENE esa
    posicion. Es la rama del if/else donde vive la llamada."""
    d = 0
    for j in range(desde, fin + 1):
        if txt[j] == '{':
            d += 1
        elif txt[j] == '}':
            if d == 0:
                return j
            d -= 1
    return fin


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    ruta = sys.argv[1]
    crudo = open(ruta, encoding='utf-8', errors='replace').read()
    txt = sin_comentarios(crudo)

    ini, fin = bloque_aplicar(txt)
    print('EL ORDEN DE "APLICAR" EN LA PANTALLA DE LA HORA')
    print('----------------------------------------------')
    if ini is None:
        print('MAL: no encuentro el manejador de UIH_HIT_APLICAR en', ruta)
        return 1

    def linea_de(pos):
        return crudo.count('\n', 0, pos) + 1

    anclajes = []
    p = txt.find(ANC, ini)
    while p != -1 and p < fin:
        anclajes.append(p)
        p = txt.find(ANC, p + 1)

    if not anclajes:
        print('MAL: ninguna llamada a reloj_anclar() dentro de Aplicar.')
        print('     O se ha quitado el anclaje, o este guion ya no sabe')
        print('     donde mirar. Las dos cosas hay que verlas.')
        return 1

    fallos = 0
    for a in anclajes:
        cierre = hasta_cerrar_la_rama(txt, a, fin)
        detras = txt.find(RTC, a, cierre)
        rama = txt.rfind(ANC, ini, a)
        delante = txt.rfind(RTC, rama if rama != -1 and rama < a else ini, a)
        print('  reloj_anclar() en la linea %d' % linea_de(a))
        if detras != -1:
            print('    MAL: rtc_pon_fecha_hora() en la linea %d, DETRAS del'
                  % linea_de(detras))
            print('         anclaje y en su misma rama: le machaca los')
            print('         segundos y el reloj se queda en el minuto')
            print('         anterior. La fecha va DELANTE.')
            fallos += 1
        elif delante != -1:
            print('    la fecha va delante, en la linea %d   ok'
                  % linea_de(delante))
        else:
            print('    esta rama no escribe fecha   ok')

    print()
    if fallos:
        print('APLICAR: %d rama(s) con el orden al reves' % fallos)
        return 1
    print('APLICAR: el orden es correcto en las %d rama(s)' % len(anclajes))
    return 0


if __name__ == '__main__':
    sys.exit(main())
