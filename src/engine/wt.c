/* Wavetable engine: 8 banks x 12 frames, smooth frame morph, band-limited levels, warp, unison.
 *
 * Knobs (engine page, 0..0x7FFF, read once per block):
 *   [0] blue   position: morphs through the bank's 12 frames
 *   [1] green  bank: analog, pwm, vowel, organ, sync, fold, bell, digital (gen_wt_data.py); fully clockwise
 *              (>= 31744) SAMPLE, built from the sampler slot's sample by the screen (wt_user.h)
 *   [2] white  warp: smooth phase bend, the wave leans (warp.h)
 *   [3] orange unison: fades in a second oscillator, detuned up to +30 cents
 *
 * Each frame is stored at 9 levels with harmonics 1..256>>L (gen_wt_data.py). The level is chosen
 * from the block's pitch so that no harmonic passes Nyquist, crossfading between two adjacent levels
 * so a glide never steps; warp raises the level by its speed-up. Lookups interpolate linearly on 8x
 * oversampled levels. No filter: that is the FX slot's job (the hidden filter FX is enabled in the DB).
 * A ~10 Hz DC blocker follows the oscillators: warp makes the halves of the cycle unequal, which shifts
 * the wave's mean.
 * Blue's position is smoothed per sample (see POS_SH below): the knob arrives once per block.
 * State: phases at bytes 0/8 and the DC blocker at 16/20, all reset by TE's note-on (engine.h); the smoothed
 * position at 4/24, valid once 12 holds POS_TAG.
 */
#include "engine.h"
#include "dsp.h"
#include "warp.h"

#include "wt_layout.h"
#include "wt_user.h"

extern const int16_t engine_data[];

struct state {
    uint32_t phase1;    /*  0: reset to 0x80000000 on note-on */
    int32_t pos;        /*  4: frame position where the next block starts, Q22 frames */
    uint32_t phase2;    /*  8: reset to 0x80000000 on note-on */
    uint32_t pos_tag;   /* 12: POS_TAG once pos is valid (voice RAM starts dirty) */
    int32_t dc_x;       /* 16: DC blocker input history, cleared on note-on */
    int32_t dc_y;       /* 20: DC blocker output history, cleared on note-on */
    int32_t pos_mid;    /* 24: the position smoother's first pole, Q22 frames */
    uint32_t spare7;
};

/* One level of one frame: table, shift = 32 - log2(size), mask = size - 1. */
struct look {
    const int16_t *t;
    int shift;
    uint32_t mask;
};

static inline struct look look_at(const int16_t *base, int frame, int level)
{
    struct look l;
    l.t = base + frame * FRAME_STRIDE + level_off[level];
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

/* Blue's position, smoothed: TE hands render the knob once per block (encoder detents of 1024 raw = 0.34 frame, or
 * TE's LFO, which modulates the voice's knob copy), and stepping the frame blend there clicks: bell's frames differ by
 * up to ~2000 LSB per detent. Two cascaded one-pole smoothers per block (each 1/4 of the way, ~3.5 blocks = 5 ms at
 * 88.2 kHz; 50 % of a step after 5-6 blocks, 90 % after ~15), the second ramped linearly across the block, so the
 * position moves every sample and its slope has no steps. Bell, one detent (wt_check): splatter above 3 kHz -27 dB re
 * the frame change for a hard step, -52 dB for a one-block ramp, -67 dB smoothed (notes/LOG.md WTPOS).
 * P: Q22 frames (frame index above bit 22, its Q15 weight in bits 7..21), so a per-sample increment dP lands exactly
 * on P + 128 dP. */
#define POS_TAG 0x57A9E001u
#define POS_SH 2                                          /* each pole moves 1 / 4 of the way per block */

/* d / 2^sh rounded towards 0 (an arithmetic shift alone would round -1 >> 2 to -1 and never settle) */
static inline int32_t shr0(int32_t d, int sh)
{
    return (d < 0 ? d + (1 << sh) - 1 : d) >> sh;
}

/* Samples k = 0 .. n - 1 with k * a <= x (a > 0, x >= 0, n <= 128): min(n, x / a + 1), without a division. */
static inline int samples_within(int32_t x, int32_t a, int n)
{
    if ((n - 1) * a <= x)
        return n;
    int k = 0;                                            /* largest k < n with k a <= x, bit by bit */
    for (int b = 64; b; b >>= 1)
        if (k + b < n && (k + b) * a <= x)
            k += b;
    return k + 1;
}

struct osc_run {
    struct look lk[4];                                    /* frames fr, fr + 1 at levels lvl, lvl2 */
    int32_t w, g2, det12, norm, warp_knob;
    const struct warp *wp;
    uint32_t p1, p2;                                      /* oscillator phases */
    int32_t dc_x, dc_y;                                   /* DC blocker */
};

static inline void frame_pair(struct look *lk, const int16_t *base, int fr, int lvl, int lvl2)
{
    lk[0] = look_at(base, fr, lvl);
    lk[1] = look_at(base, fr + 1, lvl);
    lk[2] = look_at(base, fr, lvl2);
    lk[3] = look_at(base, fr + 1, lvl2);
}

/* n samples from frame pair r->lk, frame weight F (Q22) moving by dP per sample when ramp (else fixed). */
static inline __attribute__((always_inline)) void run(struct osc_run *r, int16_t *out, const int32_t *pitch, int n,
                                                       int32_t F, int32_t dP, int ramp)
{
    const struct look *lk = r->lk;
    const int32_t w = r->w, g2 = r->g2, det12 = r->det12, norm = r->norm, warp_knob = r->warp_knob;
    uint32_t p1 = r->p1, p2 = r->p2;
    int32_t dc_x = r->dc_x, dc_y = r->dc_y;
    int32_t ff = F >> 7;
    for (int i = 0; i < n; i++) {
        uint32_t inc = (uint32_t)pitch[i];
        if (ramp) {
            ff = F >> 7;
            F += dP;
        }
        int32_t v = osc(lk, ff, w, warp_knob ? warp_phase(r->wp, p1) : p1);
        if (g2) {
            v += (osc(lk, ff, w, warp_knob ? warp_phase(r->wp, p2) : p2) * g2) >> 15;
            p2 += inc + ((inc >> 10) * (uint32_t)det12 >> 2);
        }
        int32_t x = qmul(norm, v << 9);                   /* Q15 → Q24, unison-normalized */
        dc_y = x - dc_x + qmul(2144424002, dc_y);         /* y = x - x1 + R y1, R = 1 - 2 pi 10 / 44100 */
        dc_x = x;
        out[i] = soft_knee(dc_y >> 10);                   /* Q24 → Q15 at -6 dB; linear below 0.75 FS */
        p1 += inc;
    }
    r->p1 = p1;
    r->p2 = p2;
    r->dc_x = dc_x;
    r->dc_y = dc_y;
}

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct state *s = st;
    const int16_t *base = wt_bank_base(engine_data, wt_bank_of(params[1]));   /* SAMPLE: read once per block */
    int32_t warp_knob = clamp_knob(params[2]);
    int32_t uni = clamp_knob(params[3]);

    /* Position: the knob is the target; pos_mid follows it, P follows pos_mid with a ramp across the block. A note-on
     * (TE's reset of phase1 and the DC blocker) starts at the target. The knob tops out at 32767 x 11 < 11 << 15, so
     * frame + 1 <= 11 always. */
    int32_t target = clamp_knob(params[0]) * (FRAMES - 1) << 7;
    int32_t P = s->pos, mid = s->pos_mid;
    if (s->pos_tag != POS_TAG || (s->phase1 == 0x80000000u && !s->dc_x && !s->dc_y))
        P = mid = target;
    int32_t dm = shr0(target - mid, POS_SH);
    mid = dm ? mid + dm : target;                         /* settles within 2^-20 frame */
    int32_t dP = shr0(mid - P, 7 + POS_SH);               /* per sample: 1/4 of the way over the block */
    if (!dP)
        P = mid;                                          /* within 2^-13 frame: <= 3 LSB on the sharpest bank */
    s->pos = P + dP * ENGINE_BLOCK;
    s->pos_mid = mid;
    s->pos_tag = POS_TAG;

    /* Unison: osc 2 gain g2 (Q15) fades in over the first quarter, detune up to +30 cents (Q12). */
    int32_t g2 = uni >= 8192 ? 32767 : uni << 2;
    int32_t det12 = (((uni * uni) >> 15) * 72) >> 15;
    int32_t norm = 0x7FFFFFFF - (g2 << 15);               /* ~1 / (1 + g2), Q31: 1 → 0.5 */

    struct warp wp = warp_setup(warp_knob);

    int32_t w;
    int lvl = pick_level((uint32_t)pitch[0], warp_knob ? wp.level_q8 : 0, &w);
    int lvl2 = lvl + 1 < LEVELS ? lvl + 1 : lvl;

    struct osc_run r = { { { 0, 0, 0 } }, w, g2, det12, norm, warp_knob, &wp, s->phase1, s->phase2, s->dc_x, s->dc_y };
    if (!dP) {                                            /* steady: the block with one frame pair, as before */
        frame_pair(r.lk, base, P >> 22, lvl, lvl2);
        run(&r, out, pitch, ENGINE_BLOCK, P & 0x3FFFFF, 0, 0);
    } else {
        /* Ramping: the block in segments of one frame pair, split where P crosses a frame (at most 11 times), so
         * the pair stays fixed inside the loop and the ramp costs one add per sample. */
        for (int i = 0; i < ENGINE_BLOCK;) {
            int32_t F = P & 0x3FFFFF;                     /* Q22 weight of frame (P >> 22) + 1 */
            int n = dP > 0 ? samples_within(0x3FFFFF - F, dP, ENGINE_BLOCK - i)   /* while P < next frame */
                           : samples_within(F, -dP, ENGINE_BLOCK - i);            /* while P >= this frame */
            frame_pair(r.lk, base, P >> 22, lvl, lvl2);
            run(&r, out + i, pitch + i, n, F, dP, 1);
            P += dP * n;
            i += n;
        }
    }
    s->phase1 = r.p1;
    s->phase2 = r.p2;
    s->dc_x = r.dc_x;
    s->dc_y = r.dc_y;
}
