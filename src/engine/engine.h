/* Portable engine contract (OP-1 fw 246 iter slot; also buildable for Daisy Seed / host tests).
 * Per call: one voice, ENGINE_BLOCK mono Q15 samples at 44.1 kHz.
 *   state  : ENGINE_STATE_BYTES of per-voice storage, kept between calls. TE's note-on (iter slot +0x1C)
 *            writes 0x80000000 to bytes 0..3 and 8..11 and clears 16..23, so treat those as reset on note-on.
 *   params : int16[4], engine-page knobs 0..0x7FFF; [0] blue, [1] green, [2] white, [3] orange.
 *   pitch  : int32[ENGINE_BLOCK] per-sample phase increments, 2^32 = one cycle.
 * No globals with initial values, no libc, no division (no .data/.bss, no libgcc). */
#ifndef ENGINE_H
#define ENGINE_H
#include <stdint.h>

#define ENGINE_BLOCK 128
#define ENGINE_STATE_BYTES 32

void engine_render(void *state, int16_t *out, const int16_t *params, const int32_t *pitch);

#endif
