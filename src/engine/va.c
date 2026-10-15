/* Two-oscillator virtual analog: 2 × (sine → saw → square → pulse) + sub square + white noise → soft knee.
 * No filter: the filter FX does that (notes/fx-slot.md).
 *
 * Knobs (engine page, 0..0x7FFF, read once per block):
 *   [0] blue   wave: sine → saw over the first third, saw → square over the second, square → 10% pulse over the last
 *   [1] green  osc 2 detune: 0 → +1 semitone, squared for fine control near 0
 *   [2] white  noise level: 0 → as loud as the oscillators, squared for fine control near 0
 *   [3] orange sub: square one octave below osc 1, 0 → as loud as the oscillators
 * The oscillators' level follows the wave (wave_gain: saw, square and pulse about equally loud, the pulse below the
 * knee); the sum is scaled by 1 / (1 + (sub + noise) / 2) so turning up sub or noise doesn't push the output into the
 * knee.
 *
 * Oscillators are PolyBLEP saws; a pulse is saw(t) - saw(t + pw); the sine is the interpolated 256-point table, in
 * phase with the saw's fundamental so the sine → saw morph doesn't cancel. The sub's phase is osc 1's halved, with
 * the top bit toggled on each wrap of osc 1, so it stays locked an octave down.
 *
 * Fixed point: audio Q24 (1.0 = 2^24), levels Q31. Q31 products use the Blackfin builtin (GCC 4.3 would call libgcc
 * __muldi3 for a 64-bit multiply); divisions are done by hand once per block.
 *
 * State: TE's note-on writes 0x80000000 to bytes 0..3 and 8..11 and clears 16..23 (engine.h), so the phases and the
 * sub's toggle reset on every note. The noise generator (bytes 4..7) is never reset, so notes don't repeat it.
 */
#include "engine.h"
#include "dsp.h"
#include "sine_table.h"

#define THIRD 10923

/* Oscillator level per wave knob (k = 0, 1024, .. 32768; Q15), so saw, square and pulse sit at about the same
 * loudness and below the soft knee: sine -> saw 1; saw -> square 1 / sqrt(1 + a + a^2) (the square's rms is the
 * saw's x sqrt 3: -4.8 dB at the square); square -> pulse 1 / sqrt(12 d (1 - d)) (duty d: the saw's rms), capped so
 * the pulse's peak 2 (1 - d) stays at 1.4 (0.78 at 10 %: -1.8 dB re the saw, peak 0.7 FS). */
static const uint16_t wave_gain[33] = {
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32253, 30682, 29116, 27595, 26141,
    24771, 23488, 22295, 21189, 20165, 19218, 18942, 19068, 19309, 19677, 20196, 20900, 21846, 23124, 24896, 26593,
    25486,
};

struct state {
    uint32_t phase1;    /*  0: reset to 0x80000000 on note-on */
    uint32_t rng;       /*  4: noise LCG, never reset */
    uint32_t phase2;    /*  8: reset to 0x80000000 on note-on */
    uint32_t spare3;
    uint32_t sub_top;   /* 16: sub phase top bit, cleared on note-on */
    uint32_t spare5, spare6, spare7;
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

/* Sine in Q24, in phase with saw(): the saw's fundamental is -sin(2 pi t), so read the table half a turn on. */
static inline int32_t sine(uint32_t ph)
{
    ph ^= 0x80000000u;
    uint32_t i = ph >> 24, f = (ph >> 8) & 0xFFFF;
    return ((int32_t)sine256[i] << 9) + (((sine256[i + 1] - sine256[i]) * (int32_t)f) >> 7);
}

/* One oscillator: mode 0 sine → saw by a, 1 saw → square by a, 2 pulse of width pw. */
static inline int32_t osc(uint32_t ph, uint32_t d, uint32_t inv, int mode, int32_t a, uint32_t pw)
{
    int32_t v = saw(ph, d, inv);
    if (mode == 0) {
        int32_t sn = sine(ph);
        return sn + qmul(a, v - sn);
    }
    return v - qmul(a, saw(ph + pw, d, inv));
}

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct state *s = st;
    int32_t wave = clamp_knob(params[0]);
    int32_t det = clamp_knob(params[1]);
    int32_t noise = clamp_knob(params[2]);
    int32_t sub = clamp_knob(params[3]);

    /* Wave: mode and amount (Q31) within its third; pulse width 0.5 → 0.1 cycle over the last. */
    int mode = wave < THIRD ? 0 : wave < 2 * THIRD ? 1 : 2;
    int32_t a = 0x7FFFFFFF;
    uint32_t pw = 0x80000000u;
    if (mode < 2) {
        int32_t w3 = (wave - mode * THIRD) * 3;
        a = w3 >= 32767 ? 0x7FFFFFFF : w3 << 16;
    } else {
        pw = 0x80000000u - (uint32_t)(wave - 2 * THIRD) * 157309u;
    }

    int32_t wi = wave >> 10, wf = wave & 1023;
    int32_t lw = (wave_gain[wi] + (((wave_gain[wi + 1] - wave_gain[wi]) * wf) >> 10)) << 16;   /* Q31 */

    /* Detune: inc2 = inc * (1 + det12 / 4096), det12 ≤ 243 (one semitone = 0.0595). */
    int32_t det12 = (((det * det) >> 15) * 244) >> 15;

    /* Levels (Q31) and the output scale 1 / (1 + (sub + noise) / 2). */
    int32_t sub_l = sub << 16, noise_l = ((noise * noise) >> 15) << 16;
    uint32_t den = (1u << 28) + (((uint32_t)sub_l >> 3) >> 1) + (((uint32_t)noise_l >> 3) >> 1);
    int32_t norm = den > (1u << 28) ? recip_q31(den) : 0x7FFFFFFF;

    /* PolyBLEP widths from the block's first increment (pitch changes little within 128 samples). The sub runs at
     * half osc 1's rate: half the width, twice the inverse. */
    uint32_t inc0 = (uint32_t)pitch[0];
    uint32_t inc0b = inc0 + ((inc0 >> 10) * (uint32_t)det12 >> 2);
    uint32_t d1 = inc0 >> 16, d2 = inc0b >> 16;
    uint32_t inv1 = d1 ? udiv(1u << 30, d1) : 0, inv2 = d2 ? udiv(1u << 30, d2) : 0;
    if (!inv1) d1 = 0;
    if (!inv2) d2 = 0;
    uint32_t ds = d1 >> 1, invs = ds ? inv1 << 1 : 0;

    uint32_t p1 = s->phase1, p2 = s->phase2, top = s->sub_top, rng = s->rng;

    for (int i = 0; i < ENGINE_BLOCK; i++) {
        uint32_t inc = (uint32_t)pitch[i];
        uint32_t inc2 = inc + ((inc >> 10) * (uint32_t)det12 >> 2);

        int32_t v = qmul(lw, (osc(p1, d1, inv1, mode, a, pw) + osc(p2, d2, inv2, mode, a, pw)) >> 1);
        if (sub_l) {
            uint32_t ps = (p1 >> 1) | top;
            v += qmul(sub_l, saw(ps, ds, invs) - saw(ps + 0x80000000u, ds, invs));
        }
        if (noise_l) {
            rng = rng * 1664525u + 1013904223u;
            v += qmul(noise_l, (int32_t)rng >> 7);
        }
        out[i] = soft_knee(qmul(norm, v) >> 10);                 /* Q24 → Q15 at -6 dB */

        uint32_t np = p1 + inc;
        if (np < p1)
            top ^= 0x80000000u;
        p1 = np;
        p2 += inc2;
    }
    s->phase1 = p1;
    s->phase2 = p2;
    s->sub_top = top;
    s->rng = rng;
}
