/* Wavetable engine: 8 banks x 12 frames, smooth frame morph, band-limited levels, warp, unison.
 *
 * Knobs (engine page, 0..0x7FFF, read once per block):
 *   [0] blue   position: morphs through the bank's 12 frames
 *   [1] green  bank: analog, pwm, vowel, organ, sync, fold, bell, digital (gen_wt_data.py)
 *   [2] white  warp: smooth phase bend, the wave leans (warp.h)
 *   [3] orange unison: fades in a second oscillator, detuned up to +30 cents
 *
 * Each frame is stored at 9 levels with harmonics 1..256>>L (gen_wt_data.py). The level is chosen
 * from the block's pitch so that no harmonic passes Nyquist, crossfading between two adjacent levels
 * so a glide never steps; warp raises the level by its speed-up. Lookups interpolate linearly on 8x
 * oversampled levels. No filter: that is the FX slot's job (the hidden filter FX is enabled in the DB).
 * A ~10 Hz DC blocker follows the oscillators: warp makes the halves of the cycle unequal, which shifts
 * the wave's mean.
 * State: phases at bytes 0/8 and the DC blocker at 16/20, all reset by TE's note-on (engine.h).
 */
#include "engine.h"
#include "dsp.h"
#include "warp.h"

#include "wt_layout.h"

extern const int16_t engine_data[];

struct state {
    uint32_t phase1;    /*  0: reset to 0x80000000 on note-on */
    uint32_t spare1;
    uint32_t phase2;    /*  8: reset to 0x80000000 on note-on */
    uint32_t spare3;
    int32_t dc_x;       /* 16: DC blocker input history, cleared on note-on */
    int32_t dc_y;       /* 20: DC blocker output history, cleared on note-on */
    uint32_t spare6, spare7;
};

/* One level of one frame: table, shift = 32 - log2(size), mask = size - 1. */
struct look {
    const int16_t *t;
    int shift;
    uint32_t mask;
};

static inline struct look look_at(int bank, int frame, int level)
{
    struct look l;
    l.t = engine_data + (bank * FRAMES + frame) * FRAME_STRIDE + level_off[level];
    l.shift = 32 - level_bits[level];
    l.mask = (1u << level_bits[level]) - 1;
    return l;
}

/* (d * f) >> 15 for 16-bit d and f: one-cycle fractional multiply on Blackfin. Adjacent samples in the
 * tables differ by at most 18,516 (gen_wt_data.py output), so interpolation deltas fit in 16 bits. */
#ifdef __bfin__
#define mul16(d, f) __builtin_bfin_mult_fr1x16((short)(d), (short)(f))
#else
#define mul16(d, f) (((int32_t)(short)(d) * (f)) >> 15)
#endif

/* Sample at phase ph from frames A and B of one level, blended by ff (Q15), Q15 range: linear
 * interpolation on the 8x oversampled levels (Hermite cost twice as much for -37 vs -40 dB worst
 * aliasing), frames blended after. */
static inline int32_t lookup(const struct look *A, const struct look *B, int32_t ff, uint32_t ph)
{
    uint32_t i0 = ph >> A->shift, i1 = (i0 + 1) & A->mask;
    int32_t f = (int32_t)((ph >> (A->shift - 15)) & 0x7FFF);
    int32_t a = A->t[i0] + mul16(A->t[i1] - A->t[i0], f);
    int32_t b = B->t[i0] + mul16(B->t[i1] - B->t[i0], f);
    return a + (((b - a) * ff) >> 15);
}

/* Level for increment inc: x = log2(inc) - 22 (one octave below the alias limit log2(inc) - 23),
 * plus extra (Q8, warp's speed-up), in Q8. Returns the lower level and the Q8 weight of the next one. */
static inline int pick_level(uint32_t inc, int32_t extra, int32_t *w)
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

/* One oscillator sample: frames A/B blended by ff (Q15), levels L/L+1 blended by w (Q8). Q15 out. */
static inline int32_t osc(const struct look *lk, int32_t ff, int32_t w, uint32_t ph)
{
    int32_t s = lookup(&lk[0], &lk[1], ff, ph);
    if (w)
        s += ((lookup(&lk[2], &lk[3], ff, ph) - s) * w) >> 8;
    return s;
}

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct state *s = st;
    int32_t pos = clamp_knob(params[0]);
    int bank = clamp_knob(params[1]) >> 12;
    int32_t warp_knob = clamp_knob(params[2]);
    int32_t uni = clamp_knob(params[3]);

    /* Frame position: frames fr, fr + 1 with weight ff (Q15). */
    int32_t fpos = pos * (FRAMES - 1);
    int fr = fpos >> 15;
    int32_t ff = fpos & 0x7FFF;
    if (fr >= FRAMES - 1) {
        fr = FRAMES - 2;
        ff = 0x7FFF;
    }

    /* Unison: osc 2 gain g2 (Q15) fades in over the first quarter, detune up to +30 cents (Q12). */
    int32_t g2 = uni >= 8192 ? 32767 : uni << 2;
    int32_t det12 = (((uni * uni) >> 15) * 72) >> 15;
    int32_t norm = 0x7FFFFFFF - (g2 << 15);               /* ~1 / (1 + g2), Q31: 1 → 0.5 */

    struct warp wp = warp_setup(warp_knob);

    int32_t w;
    int lvl = pick_level((uint32_t)pitch[0], warp_knob ? wp.level_q8 : 0, &w);
    int lvl2 = lvl + 1 < LEVELS ? lvl + 1 : lvl;
    struct look lk[4] = {
        look_at(bank, fr, lvl), look_at(bank, fr + 1, lvl),
        look_at(bank, fr, lvl2), look_at(bank, fr + 1, lvl2),
    };

    uint32_t p1 = s->phase1, p2 = s->phase2;
    int32_t dc_x = s->dc_x, dc_y = s->dc_y;

    for (int i = 0; i < ENGINE_BLOCK; i++) {
        uint32_t inc = (uint32_t)pitch[i];
        int32_t v = osc(lk, ff, w, warp_knob ? warp_phase(&wp, p1) : p1);
        if (g2) {
            v += (osc(lk, ff, w, warp_knob ? warp_phase(&wp, p2) : p2) * g2) >> 15;
            p2 += inc + ((inc >> 10) * (uint32_t)det12 >> 2);
        }
        int32_t x = qmul(norm, v << 9);                   /* Q15 → Q24, unison-normalized */
        dc_y = x - dc_x + qmul(2144424002, dc_y);         /* y = x - x1 + R y1, R = 1 - 2 pi 10 / 44100 */
        dc_x = x;
        out[i] = soft_knee(dc_y >> 10);                   /* Q24 → Q15 at -6 dB; linear below 0.75 FS */
        p1 += inc;
    }
    s->phase1 = p1;
    s->phase2 = p2;
    s->dc_x = dc_x;
    s->dc_y = dc_y;
}
