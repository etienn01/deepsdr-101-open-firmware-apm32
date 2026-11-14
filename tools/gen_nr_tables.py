#!/usr/bin/env python3
"""Generates User/nr_ss_tables.h: the twiddle, Hann window and bit-reversal tables of the noise reduction's
128-point FFT (User/nr_ss.c) as constants in flash, so the APM32 build does not spend 1,280 bytes of CCM
computing them at boot (see nr_ss_init()).

The values are the ones nr_ss_init() computes ON THE RADIO, bit for bit. That is not the same as evaluating its
C source in plain float32: arm-none-eabi-gcc -O2 contracts two multiply-adds into fused instructions (one
rounding instead of two), read from the disassembly of nr_ss_init() and sinf_approx_local():

  - the Bhaskara denominator 5*pi*pi - (4x)*(pi - x) is a vfms (fused multiply-subtract);
  - the window's argument 2*pi*t + pi/2 (cosf_approx_local() inlined) is a vfma.

Without them 7 cosines, 4 sines and 28 window values come out one unit in the last place off. Every other
operation is a plain float32 one in the source order; the window's final 0.5 - 0.5*c is also a vfms, but 0.5*c
is exact, so fused or not gives the same result. Each operation below is evaluated exactly (fractions) and rounded
once to float32, round to nearest even, which is what the Cortex-M4 FPU does (FPSCR left at its reset value).

Usage: python3 tools/gen_nr_tables.py > User/nr_ss_tables.h
"""
import struct
from fractions import Fraction

N = 128  # NR_SS_NSTFFT


def f32(x):
    """Round a Python float to the nearest float32 (once: used only on values that are already exact)."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def round32(q):
    """Exact rational -> float32, round to nearest, ties to even (no overflow/underflow in these tables)."""
    assert q != 0
    a = abs(q)
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    assert -126 <= e <= 127, 'out of the normal float32 range'
    scale = Fraction(2) ** (e - 23)
    m = a / scale
    n = m.numerator // m.denominator
    r = m - n
    if r > Fraction(1, 2) or (r == Fraction(1, 2) and n % 2 == 1):
        n += 1
    v = float(n * scale)
    return -v if q < 0 else v


# Each float32 operation: exact result, one rounding. A zero result takes its sign from IEEE double arithmetic on
# the same (float32-valued) operands, which follows the same rules.
def add(a, b):
    q = Fraction(a) + Fraction(b)
    return round32(q) if q != 0 else a + b


def sub(a, b):
    return add(a, -b)


def mul(a, b):
    q = Fraction(a) * Fraction(b)
    return round32(q) if q != 0 else a * b


def div(a, b):
    q = Fraction(a) / Fraction(b)
    return round32(q) if q != 0 else a / b


def fma(a, b, c):
    """a*b + c with a single rounding (vfma; vfms is fma(-a, b, c))."""
    q = Fraction(a) * Fraction(b) + Fraction(c)
    return round32(q) if q != 0 else a * b + c   # a*b is exact in double: same signed zero


PI = f32(3.14159265358979)
TWO_PI = mul(2.0, PI)                  # 2.0f * pi, folded by the compiler (exact)
HALF_PI = div(PI, 2.0)                 # 3.14159265358979f / 2.0f, folded (exact)
FIVE_PI_PI = mul(mul(5.0, PI), PI)     # 5.0f * pi * pi, folded left to right
assert struct.pack('<f', FIVE_PI_PI) == struct.pack('<I', 0x42456461), 'the constant in nr_ss.o is 0x42456461'


def sinf_approx_local(x):
    """User/nr_ss.c's Bhaskara-I sine, as compiled: the denominator is fused (vfms)."""
    while x > PI:
        x = sub(x, TWO_PI)
    while x < -PI:
        x = add(x, TWO_PI)
    negate = x < 0.0
    if negate:
        x = -x
    pmx = sub(PI, x)
    den = fma(-pmx, mul(4.0, x), FIVE_PI_PI)       # 5*pi*pi - (4x)*(pi - x), one rounding
    num = mul(mul(16.0, x), pmx)                   # (16x)*(pi - x)
    r = div(num, den)
    return -r if negate else r


def cosf_approx_local(x):
    return sinf_approx_local(add(x, HALF_PI))


def tables():
    cos_t, sin_t, win, bitrev = [], [], [], []
    for n in range(N // 2):
        angle = div(mul(mul(-2.0, PI), float(n)), float(N))   # -2.0f * pi * (float)n / (float)N
        cos_t.append(cosf_approx_local(angle))
        sin_t.append(sinf_approx_local(angle))
    for n in range(N):
        t = div(float(n), float(N))                           # (float)n / (float)N, t = n/N as the original
        arg = fma(t, TWO_PI, HALF_PI)                         # 2.0f * pi * t + pi/2, fused (vfma)
        win.append(fma(-0.5, sinf_approx_local(arg), 0.5))  # 0.5f - 0.5f * cos, vfms
    bits = N.bit_length() - 1
    for n in range(N):
        v, r = n, 0
        for _ in range(bits):
            r = (r << 1) | (v & 1)
            v >>= 1
        bitrev.append(r)
    return cos_t, sin_t, win, bitrev


def hexf(v):
    """Exact float literal: the IEEE bits, as a hexadecimal float that C reads back bit for bit."""
    return float(v).hex() + 'f'


def main():
    cos_t, sin_t, win, bitrev = tables()

    def rows(vals, per_line, fmt):
        return '\n'.join('    ' + ', '.join(fmt(v) for v in vals[i:i + per_line]) + ','
                         for i in range(0, len(vals), per_line))

    print('/* Generated by tools/gen_nr_tables.py - do not edit. See nr_ss_init() in nr_ss.c. */')
    print('#ifndef NR_SS_TABLES_H')
    print('#define NR_SS_TABLES_H')
    print('#include <stdint.h>')
    print('#define NR_SS_TABLES_SIZE %dU' % N)
    print('static const float k_nr_twiddle_cos[%d] = {\n%s\n};' % (N // 2, rows(cos_t, 4, hexf)))
    print('static const float k_nr_twiddle_sin[%d] = {\n%s\n};' % (N // 2, rows(sin_t, 4, hexf)))
    print('static const float k_nr_win[%d] = {\n%s\n};' % (N, rows(win, 4, hexf)))
    print('static const uint8_t k_nr_bitrev[%d] = {\n%s\n};' % (N, rows(bitrev, 16, str)))
    print('#endif /* NR_SS_TABLES_H */')


if __name__ == '__main__':
    main()
