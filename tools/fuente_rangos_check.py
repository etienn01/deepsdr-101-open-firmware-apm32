#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
QUE LAS TABLAS DE RANGOS DE LAS FUENTES SIGAN COMPARTIDAS.

  python3 tools/fuente_rangos_check.py User/fonts_data.c

POR QUE EXISTE - 13/11/2026.

Una tabla gfx2_range_t no describe pixeles: describe QUE caracteres lleva la
fuente y en que orden. Las cuatro fuentes de interfaz se generaron del mismo
juego de caracteres, asi que sus cuatro tablas eran IDENTICAS rango a rango,
y las dos de numeros igual. Eran 76 rangos de 6 bytes repetidos: 456 bytes de
flash en un chip donde quedaban 388 libres.

Ahora vive una de cada y las demas apuntan a ella.

Y HACE FALTA VIGILARLO POR UN MOTIVO CONCRETO: la cabecera de fonts_data.c
dice "NO EDITAR A MANO: se regenera con tools/gen_assets.py", y ese guion no
esta en el arbol. O sea que el fichero ya solo se mantiene a mano, pero el
dia que alguien recupere el generador y lo pase, volvera a emitir las cuatro
tablas por separado y los 456 bytes se iran sin que nada chille.

QUE COMPRUEBA, en este orden:

  1. que no hay dos tablas de rangos con el MISMO contenido. Si las hay, son
     bytes repetidos y se dice cuantos.
  2. que toda tabla declarada la usa algun descriptor de fuente, y que todo
     descriptor apunta a una tabla que existe.
"""
import re
import sys


def tablas(txt):
    """nombre -> lista de (lo, hi, first)."""
    out = {}
    for m in re.finditer(r'const gfx2_range_t (\w+)\[(\d+)\] = \{(.*?)\n\};',
                         txt, re.S):
        nombre, n, cuerpo = m.group(1), int(m.group(2)), m.group(3)
        t = re.findall(
            r'\{\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(\d+)\s*\}',
            cuerpo)
        if len(t) != n:
            print('MAL: %s dice [%d] y tiene %d rangos escritos'
                  % (nombre, n, len(t)))
            return None
        out[nombre] = [(int(a, 0), int(b, 0), int(c, 0)) for a, b, c in t]
    return out


def usos(txt):
    """Las tablas que nombra algun `const gfx2_font_t ... = { ..., X, ...}`."""
    vistos = []
    for m in re.finditer(r'const gfx2_font_t \w+ = \{(.*?)\};', txt, re.S):
        for n in re.findall(r'(\w+_ranges)', m.group(1)):
            vistos.append(n)
    return vistos


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    txt = open(sys.argv[1], encoding='utf-8', errors='replace').read()

    print('LAS TABLAS DE RANGOS DE LAS FUENTES')
    print('-----------------------------------')
    t = tablas(txt)
    if t is None:
        return 1
    if not t:
        print('MAL: no encuentro ninguna tabla de rangos en', sys.argv[1])
        return 1

    fallos = 0

    # 1. ninguna repetida
    grupos = {}
    for k, v in t.items():
        grupos.setdefault(tuple(v), []).append(k)
    for v, ks in sorted(grupos.items(), key=lambda x: x[1][0]):
        if len(ks) > 1:
            desperdicio = (len(ks) - 1) * len(v) * 6
            print('  MAL: %s tienen EL MISMO contenido (%d rangos).'
                  % (', '.join(sorted(ks)), len(v)))
            print('       Son %d bytes de flash repetidos. Deja una y que las'
                  % desperdicio)
            print('       demas apunten a ella en su descriptor.')
            fallos += 1
        else:
            print('  %-22s %2d rangos   unica  ok' % (ks[0], len(v)))

    # 2. y las que hay se usan, y las que se usan existen
    u = usos(txt)
    for k in sorted(t):
        if k not in u:
            print('  MAL: %s esta declarada y no la usa ningun descriptor' % k)
            fallos += 1
    for k in sorted(set(u)):
        if k not in t:
            print('  MAL: un descriptor apunta a %s, que no existe aqui' % k)
            fallos += 1

    print()
    print('  %d tabla(s), %d referencia(s) desde descriptores'
          % (len(t), len(u)))
    if fallos:
        print('RANGOS: %d problema(s)' % fallos)
        return 1
    print('RANGOS: ninguna repetida, y todas se usan')
    return 0


if __name__ == '__main__':
    sys.exit(main())
