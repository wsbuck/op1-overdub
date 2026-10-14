/* wt.c's band-limited table oscillator core, as a header (notes/harmonic-synth.md: share it, don't fork it). harm.c
 * uses it now; wt.c keeps its own copy until it is next touched (identical arithmetic: level choice, 8x oversampled
 * linear interpolation, level crossfade).
 *
 * A frame is wt_layout.h's 9 levels; level L holds harmonics 1 .. 256 >> L in 1 << level_bits[L] samples.
 */
#ifndef WT_OSC_H
#define WT_OSC_H
#include <stdint.h>
#include "wt_layout.h"

/* One level of one frame: table, shift = 32 - log2(size), mask = size - 1. */
struct wo_look {
    const int16_t *t;
    int shift;
    uint32_t mask;
};

static inline struct wo_look wo_level(const int16_t *frame, int level)
{
    struct wo_look l;
    l.t = frame + level_off[level];
    l.shift = 32 - level_bits[level];
    l.mask = (1u << level_bits[level]) - 1;
    return l;
}

/* (d * f) >> 15 for 16-bit d and f: one-cycle fractional multiply on Blackfin. Interpolation deltas fit in 16 bits
 * for frames peaking at <= 0.9 FS (wt_levels.h). */
#ifdef __bfin__
#define wo_mul16(d, f) __builtin_bfin_mult_fr1x16((short)(d), (short)(f))
#else
#define wo_mul16(d, f) (((int32_t)(short)(d) * (f)) >> 15)
#endif

/* Sample at phase ph, linear interpolation, Q15 range. */
static inline int32_t wo_read(const struct wo_look *A, uint32_t ph)
{
    uint32_t i0 = ph >> A->shift, i1 = (i0 + 1) & A->mask;
    int32_t f = (int32_t)((ph >> (A->shift - 15)) & 0x7FFF);
    return A->t[i0] + wo_mul16(A->t[i1] - A->t[i0], f);
}

/* Level for increment inc: x = log2(inc) - 22 (one octave below the alias limit log2(inc) - 23), plus extra (Q8, e.g.
 * warp's speed-up), in Q8. Returns the lower level and the Q8 weight of the next one. Pitch increments are cycles per
 * sample, so this is right at any sample rate. */
static inline int wo_pick_level(uint32_t inc, int32_t extra, int32_t *w)
{
    int msb = 31;
    while (msb > 0 && !(inc >> msb))
        msb--;
    int32_t frac = (int32_t)(((inc << (31 - msb)) >> 23) & 0xFF);   /* mantissa ~ log2 fraction */
    int32_t x = ((msb - 22) << 8) + frac + extra;
    if (x <= 0) {
        *w = 0;
        return 0;
    }
    if (x >= (LEVELS - 1) << 8) {
        *w = 0;
        return LEVELS - 1;
    }
    *w = x & 0xFF;
    return x >> 8;
}

/* One oscillator sample: levels lk[0] / lk[1] blended by w (Q8). Q15 out. */
static inline int32_t wo_osc(const struct wo_look *lk, int32_t w, uint32_t ph)
{
    int32_t s = wo_read(&lk[0], ph);
    if (w)
        s += ((wo_read(&lk[1], ph) - s) * w) >> 8;
    return s;
}

#endif
