#ifndef DECODER_ARENA_H
#define DECODER_ARENA_H
/*
 * APM32: fast-RAM (TCM) buffers of decoders that never run at the same time, overlaid in one union.
 *
 * NAVTEX and APRS (AX.25) each keep four matched-filter delay lines (mark and space, I and Q). Each is one
 * entry of k_demod_modes, so only one of them is ever selected; together they cost 4,608 bytes of TCM, and
 * overlaid they cost the larger of the two, 3,072.
 *
 * WHY AN OWNER. The mode change does not stop every decoder before starting the new one: it walks the
 * decoders in order, NAVTEX before APRS, and calls start or stop on each. Going from APRS to NAVTEX,
 * navtex_start() runs while ax25_process() is still being called from the audio interrupt, and the
 * running sums of the matched filters would never recover from the samples the other decoder wrote over
 * them. So each start takes the arena (decoder_arena_take()) before clearing its part, and each process
 * function returns early unless it owns the arena (decoder_arena_mine()). The owner is one byte written by
 * the main loop and read by the interrupt.
 *
 * On GD32 nothing is overlaid: the buffers stay in their own files and the two calls compile to nothing.
 */
#include <stdint.h>

#define DECODER_ARENA_NAVTEX 1U
#define DECODER_ARENA_AX25   2U

#ifdef CHIP_APM32F407

#define DECODER_ARENA_NAVTEX_SPB 192U   /* SPB_MAX of navtex.c, checked there */
#define DECODER_ARENA_AX25_SPB    96U   /* SPB_MAX of ax25.c, checked there */

typedef union {
    struct {
        float mi[DECODER_ARENA_NAVTEX_SPB], mq[DECODER_ARENA_NAVTEX_SPB];
        float ei[DECODER_ARENA_NAVTEX_SPB], eq[DECODER_ARENA_NAVTEX_SPB];
    } navtex;
    struct {
        float mi[DECODER_ARENA_AX25_SPB], mq[DECODER_ARENA_AX25_SPB];
        float ei[DECODER_ARENA_AX25_SPB], eq[DECODER_ARENA_AX25_SPB];
    } ax25;
} decoder_arena_t;

extern decoder_arena_t g_decoder_arena;
extern volatile uint8_t g_decoder_arena_owner;

#define decoder_arena_take(who) do { g_decoder_arena_owner = (uint8_t)(who); } while (0)
#define decoder_arena_mine(who) (g_decoder_arena_owner == (uint8_t)(who))

#else

#define decoder_arena_take(who) do { } while (0)
#define decoder_arena_mine(who) (1)

#endif

#endif /* DECODER_ARENA_H */
