/*
 * A WORD-AT-A-TIME memcpy() - 12/11/2026.
 *
 * The build links newlib-nano (-specs=nano.specs), whose memcpy() is the size-optimised stub: one byte per loop,
 * about four cycles per byte. Profiled on the radio by sampling the program counter over SWD (DWT_PCSR, 20 000
 * samples on the main screen), it was the biggest single item: 17.9 % of all CPU time, ahead of every DSP routine.
 * Most of it is the spectrum: compon() lays the grid template (800 pixels, 1600 bytes) on every row of every band,
 * some 300 kB per frame.
 *
 * Defining memcpy() here replaces the library's for the whole firmware, the C library's own callers included: the
 * linker takes an object file's definition before it looks in libc_nano.a.
 *
 * How: bytes until the destination is word aligned, then words, four per loop, then the last bytes. A source that
 * is not word aligned is read with unaligned word loads, which the Cortex-M4 does in hardware on normal memory
 * (SCB->CCR.UNALIGN_TRP is never set in this firmware); the spectrum's halfword arrays hit that case at every odd
 * starting column. Nothing here is meant for peripheral registers, where unaligned accesses fault; nobody copies
 * from those with memcpy().
 *
 * Checked on the host against the system's memcpy() for every length from 0 to 300 bytes and every source and
 * destination alignment from 0 to 7, with guard bytes on both sides.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Word accesses through these types are allowed to alias anything, and the second one may be unaligned. */
typedef uint32_t __attribute__((may_alias)) palabra_t;
typedef struct { uint32_t v; } __attribute__((packed, may_alias)) palabra_suelta_t;

/* Without this, GCC can recognise the byte loops below as a copy and turn them into a call to memcpy(): itself. */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (n >= 8U) {
        palabra_t *dw;

        while (((uintptr_t)d & 3U) != 0U) {
            *d++ = *s++;
            n--;
        }
        dw = (palabra_t *)(void *)d;

        if (((uintptr_t)s & 3U) == 0U) {
            const palabra_t *sw = (const palabra_t *)(const void *)s;

            while (n >= 16U) {
                uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];

                dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
                sw += 4; dw += 4; n -= 16U;
            }
            while (n >= 4U) {
                *dw++ = *sw++;
                n -= 4U;
            }
            s = (const uint8_t *)sw;
        } else {
            const palabra_suelta_t *sw = (const palabra_suelta_t *)(const void *)s;

            while (n >= 16U) {
                uint32_t a = sw[0].v, b = sw[1].v, c = sw[2].v, e = sw[3].v;

                dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
                sw += 4; dw += 4; n -= 16U;
            }
            while (n >= 4U) {
                *dw++ = (sw++)->v;
                n -= 4U;
            }
            s = (const uint8_t *)sw;
        }
        d = (uint8_t *)dw;
    }
    while (n != 0U) {
        *d++ = *s++;
        n--;
    }
    return dst;
}
