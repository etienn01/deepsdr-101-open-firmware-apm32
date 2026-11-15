/*
 * The storage behind decoder_arena.h. APM32 only; on GD32 this file is empty.
 */
#include "decoder_arena.h"

#ifdef CHIP_APM32F407
/* .tcmram is zeroed at boot (see the linker script); each decoder clears its own part when it starts.
 * Not on a Mac host: Mach-O section names need a segment, and the simulator bench builds this file. */
#if defined(__GNUC__) && !defined(__APPLE__)
decoder_arena_t g_decoder_arena __attribute__((section(".tcmram")));
#else
decoder_arena_t g_decoder_arena;
#endif
volatile uint8_t g_decoder_arena_owner;
#else
typedef int decoder_arena_empty_t;   /* ISO C wants something in a translation unit */
#endif
