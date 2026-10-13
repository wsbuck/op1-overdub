/* Smooth phase bend ("warp") shared by wt.c and the screen (src/slot/ui.c), so the display matches the sound.
 *
 * phase' = phase + k sin(2 pi phase) / (2 pi), k = 0 .. 0.9 from the knob. The wave leans (one half plays
 * faster, the other slower) with no corner in the phase curve, so the result stays band-limited: the
 * local speed-up is at most 1 + k, and wt raises its band-limit level by k octaves (>= log2(1 + k)).
 * A two-segment bend was tried first: its corner aliased at -15 dB at C8 (LOG.md).
 * Phases are uint32 (2^32 = one cycle).
 */
#ifndef WARP_H
#define WARP_H
#include <stdint.h>
#include "sine_table.h"

struct warp {
    int32_t depth;      /* k / (2 pi) * 2^17, multiplies a Q15 sine into phase units */
    int32_t level_q8;   /* k octaves, Q8 */
};

static inline struct warp warp_setup(int32_t knob)
{
    struct warp w;
    w.depth = (knob * 18779) >> 15;                       /* 0.9 / (2 pi) * 2^17 = 18779 */
    w.level_q8 = (knob * 230) >> 15;                      /* 0.9 * 256 */
    return w;
}

static inline uint32_t warp_phase(const struct warp *w, uint32_t ph)
{
    uint32_t i = ph >> 24;
    int32_t f = (int32_t)((ph >> 9) & 0x7FFF);
    int32_t s = sine256[i] + (((sine256[i + 1] - sine256[i]) * f) >> 15);
    return ph + (uint32_t)(s * w->depth);
}

#endif
