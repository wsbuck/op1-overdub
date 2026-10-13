/* Wavetable data layout shared by wt.c and the screen (src/slot/ui.c). Must match gen_wt_data.py:
 * bank b, frame f, level L at sample offset (b * FRAMES + f) * FRAME_STRIDE + level_off[L];
 * level L holds harmonics 1 .. 256 >> L in 1 << level_bits[L] samples (8x oversampled). */
#ifndef WT_LAYOUT_H
#define WT_LAYOUT_H
#include <stdint.h>

#define BANKS 8
#define FRAMES 12
#define LEVELS 9
#define FRAME_STRIDE 4128
static const uint16_t level_off[LEVELS] = { 0, 2048, 3072, 3584, 3840, 3968, 4032, 4064, 4096 };
static const uint8_t level_bits[LEVELS] = { 11, 10, 9, 8, 7, 6, 5, 5, 5 };  /* log2 of level size */

#endif
