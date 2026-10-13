/* Two-oscillator virtual analog: 2 × (saw → pulse morph) → ZDF state-variable low-pass → soft clip.
 *
 * Knobs (engine page, 0..0x7FFF, read once per block):
 *   [0] blue   wave: saw → square over the first half, then square → 10% pulse
 *   [1] green  osc 2 detune: 0 → +1 semitone, squared for fine control near 0
 *   [2] white  cutoff: 20 Hz → 14 kHz, exponential (va_tables.h)
 *   [3] orange resonance: Q 0.5 → 10, input level reduced up to -12 dB to keep the peak in range
 *
 * Oscillators are PolyBLEP saws; a pulse is saw(t) - saw(t + pw). The filter is the trapezoidal
 * (zero-delay feedback) SVF (Simper, Cytomic 2013), stable under modulation at any setting.
 *
 * Fixed point: audio Q24 (1.0 = 2^24, headroom for resonant peaks), filter coefficients Q31,
 * g and k Q28 while computing coefficients. Q31 products use the Blackfin builtin (GCC 4.3 would
 * call libgcc __muldi3 for a 64-bit multiply); divisions are done by hand once per block.
 *
 * State: TE's note-on writes 0x80000000 to bytes 0..3 and 8..11 and clears 16..23 (engine.h), so
 * the phases and filter state below reset on every note, which is what we want.
 */
#include "engine.h"
#include "dsp.h"
#include "va_tables.h"

struct state {
    uint32_t phase1;    /*  0: reset to 0x80000000 on note-on */
    uint32_t spare1;
    uint32_t phase2;    /*  8: reset to 0x80000000 on note-on */
    uint32_t spare3;
    int32_t ic1;        /* 16: SVF state, cleared on note-on */
    int32_t ic2;        /* 20: SVF state, cleared on note-on */
    uint32_t spare6, spare7;
};

/* PolyBLEP saw in Q24 at phase ph. d = inc >> 16 for this block, inv = 2^30 / d (0 = no correction). */
static inline int32_t saw(uint32_t ph, uint32_t d, uint32_t inv)
{
    int32_t v = (int32_t)(ph >> 7) - ONE;
    uint32_t t = ph >> 16, u = ~ph >> 16;
    if (t < d) {                         /* just after the wrap: add (1 - t/dt)^2 */
        int32_t y = 32768 - (int32_t)((t * inv) >> 15);
        if (y > 0)
            v += (y * y) >> 6;
    } else if (u < d) {                  /* just before the wrap: subtract (1 - (1-t)/dt)^2 */
        int32_t y = 32768 - (int32_t)((u * inv) >> 15);
        if (y > 0)
            v -= (y * y) >> 6;
    }
    return v;
}

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct state *s = st;
    int32_t wave = clamp_knob(params[0]);
    int32_t det = clamp_knob(params[1]);
    int32_t cut = clamp_knob(params[2]);
    int32_t res = clamp_knob(params[3]);

    /* Wave: o = saw(t) - mix * saw(t + pw). */
    int32_t mix;
    uint32_t pw;
    if (wave < 16384) {
        mix = wave << 17;
        pw = 0x80000000u;
    } else {
        mix = 0x7FFFFFFF;
        pw = 0x80000000u - (uint32_t)(wave - 16384) * 104858u;   /* 0.5 → 0.1 cycle */
    }

    /* Detune: inc2 = inc * (1 + det12 / 4096), det12 ≤ 243 (one semitone = 0.0595). */
    int32_t det12 = (((det * det) >> 15) * 244) >> 15;

    /* Filter coefficients. g from the table, k = 2 - 1.9 res (Q 0.5 → 10). */
    struct svf f = svf_coefs(cutoff_g(va_cutoff_g, cut), (2 << 28) - res * 15565);
    int32_t comp = 0x7FFFFFFF - res * 49153;             /* input gain 1 → 0.25 (Q31) */

    /* PolyBLEP widths from the block's first increment (pitch changes little within 128 samples). */
    uint32_t inc0 = (uint32_t)pitch[0];
    uint32_t inc0b = inc0 + ((inc0 >> 10) * (uint32_t)det12 >> 2);
    uint32_t d1 = inc0 >> 16, d2 = inc0b >> 16;
    uint32_t inv1 = d1 ? udiv(1u << 30, d1) : 0, inv2 = d2 ? udiv(1u << 30, d2) : 0;
    if (!inv1) d1 = 0;
    if (!inv2) d2 = 0;

    uint32_t p1 = s->phase1, p2 = s->phase2;
    int32_t ic1 = s->ic1, ic2 = s->ic2;

    for (int i = 0; i < ENGINE_BLOCK; i++) {
        uint32_t inc = (uint32_t)pitch[i];
        uint32_t inc2 = inc + ((inc >> 10) * (uint32_t)det12 >> 2);

        int32_t o1 = saw(p1, d1, inv1) - qmul(mix, saw(p1 + pw, d1, inv1));
        int32_t o2 = saw(p2, d2, inv2) - qmul(mix, saw(p2 + pw, d2, inv2));
        int32_t v0 = qmul(comp, (o1 + o2) >> 1);

        out[i] = soft_clip(svf_lp(&f, v0, &ic1, &ic2) >> 10);   /* Q24 → Q15 at -6 dB */
        p1 += inc;
        p2 += inc2;
    }
    s->phase1 = p1;
    s->phase2 = p2;
    s->ic1 = ic1;
    s->ic2 = ic2;
}
