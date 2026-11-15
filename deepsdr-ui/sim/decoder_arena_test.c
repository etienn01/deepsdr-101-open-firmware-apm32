/*
 * The APM32 decoder arena (User/decoder_arena.h): NAVTEX and APRS share their matched-filter buffers.
 *
 * main.c changes mode by walking the decoders in order, NAVTEX before APRS, so going from APRS to NAVTEX
 * navtex_start() runs while APRS is still on and the audio interrupt still calls ax25_process(). This plays
 * that sequence and checks that only the owner of the arena writes to it. Built with CHIP_APM32F407, the
 * only configuration where the buffers are shared.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "decoder_arena.h"
#include "navtex.h"
#include "ax25.h"

static float s_audio[480];
static int s_fails;

static void check(int good, const char *what)
{
    printf("  %-62s %s\n", what, good ? "ok" : "FALLO");
    if (!good) { s_fails++; }
}

int main(void)
{
    static decoder_arena_t before;
    unsigned i;

    printf("decoder arena (APM32): NAVTEX and APRS\n\n");
    for (i = 0; i < 480; i++) {
        s_audio[i] = 0.3f * sinf(0.7f * (float)i) + 0.2f * sinf(0.13f * (float)i);
    }
    ax25_start(12000.0f);
    ax25_process(s_audio, 480);
    check(ax25_activo() && decoder_arena_mine(DECODER_ARENA_AX25), "APRS on and owning the arena");

    navtex_start(12000.0f);
    check(navtex_activo() && ax25_activo(), "both on, as between navtex_start() and ax25_stop()");
    memcpy(&before, &g_decoder_arena, sizeof before);
    ax25_process(s_audio, 480);
    check(memcmp(&before, &g_decoder_arena, sizeof before) == 0, "ax25_process() leaves the arena alone");
    navtex_process(s_audio, 480);
    check(memcmp(&before, &g_decoder_arena, sizeof before) != 0, "navtex_process() writes it (so the test sees writes)");
    ax25_stop();

    ax25_start(12000.0f);
    memcpy(&before, &g_decoder_arena, sizeof before);
    navtex_process(s_audio, 480);
    check(memcmp(&before, &g_decoder_arena, sizeof before) == 0, "after ax25_start(), NAVTEX leaves it alone");

    if (s_fails) {
        printf("\nHAY FALLOS (%d)\n", s_fails);
        return 1;
    }
    printf("\nTODO CORRECTO (0 fallos)\n");
    return 0;
}
