/*
 * memset() FOR THE APM32 BUILD - 14/11/2026.
 *
 * newlib-nano's memset() (nano.specs) is built for size: one byte per loop, three to four cycles a byte. Profiled on
 * the radio while the Settings page opened, it was 15 % of the drawing time: gfx2_render() clears its 6.4 kB band
 * before each of the 80 bands of a page, about 30 ms a page with the audio interrupt running. This one writes four
 * bytes at a time, sixteen per turn once aligned, and returns the same thing. Defined here, it replaces the library's
 * at link time for every caller. memcpy() is left alone: nano's already copies words.
 *
 * Built with -fno-tree-loop-distribute-patterns (see avisos.mk): without it GCC may recognise the byte loops below as
 * a memset and call memset() from inside memset().
 *
 * The GD32 build keeps the library's: about 100 bytes of flash it does not have.
 */
#ifdef CHIP_APM32F407
#include <stddef.h>
#include <stdint.h>

typedef uint32_t __attribute__((may_alias)) palabra_t;

void *memset(void *dst, int c, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    uint32_t v = (uint32_t)(uint8_t)c;

    v |= v << 8;
    v |= v << 16;
    while ((n != 0U) && (((uintptr_t)p & 3U) != 0U)) { *p++ = (uint8_t)c; n--; }
    {
        palabra_t *w = (palabra_t *)(void *)p;

        while (n >= 16U) { w[0] = v; w[1] = v; w[2] = v; w[3] = v; w += 4; n -= 16U; }
        while (n >= 4U)  { *w++ = v; n -= 4U; }
        p = (uint8_t *)(void *)w;
    }
    while (n != 0U) { *p++ = (uint8_t)c; n--; }
    return dst;
}
#else
typedef int memrapido_vacio_t;   /* ISO C: a translation unit is not empty */
#endif
