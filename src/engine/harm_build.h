/* Harmonic engine table builder (UI thread): the 24 drawn levels, tilt and extend -> one band-limited single-cycle
 * frame in wt_layout.h's format (9 levels, harmonics 1 .. 256 >> L), through wt_levels.h's inverse FFTs.
 *
 * Amplitudes, all in the log2 domain (Q16 octaves of amplitude below 2^20):
 *   level v (1..15)  (15 - v) * 3 dB
 *   tilt t           -t * 0.75 dB per octave of the harmonic number (t > 0 brighter)
 *   extend e (1..15) harmonics 25..256 repeat bars 13..24 (12 harmonics, so odd/even patterns keep their parity),
 *                    each repeat (16 - e) * 1.5 dB quieter than the one below; 0 = nothing above 24
 * The loudest harmonic is set to 2^20 (so 256 of them stay inside the inverse FFT's 2^30 headroom); harmonics below
 * 2^20 / 2^16 are dropped. Sine phase for every harmonic: the textbook shapes on screen, and harmonic 1 lines up.
 * Loudness: a fixed gain from the RMS, not peak normalisation, so lowering one bar does not raise the others: a single
 * harmonic peaks at 0.6 FS; a drawing whose peak would pass 0.9 FS is scaled down to 0.9 FS (wt.c's interpolation
 * bound). Cost: 9 inverse FFTs (sizes 2048 .. 32), ~1.6 M CYCLES (harm_check.py measures it).
 */
#ifndef HARM_BUILD_H
#define HARM_BUILD_H
#include <stdint.h>
#include "harm.h"
#include "wt_levels.h"

#define HARM_DB3 32657                      /* 3 dB in Q16 octaves: 3 / 6.0206 * 65536 */
#define HARM_TILT_STEP 8164                 /* 0.75 dB */
#define HARM_EXT_STEP 16329                 /* 1.5 dB */

/* log2(1 + i / 32), Q16 */
static const uint16_t harm_log2_tab[33] = {
    0, 2909, 5732, 8473, 11136, 13727, 16248, 18704, 21098, 23433, 25711, 27936, 30109, 32234, 34312, 36346, 38336,
    40286, 42196, 44068, 45904, 47705, 49472, 51207, 52911, 54584, 56229, 57845, 59434, 60997, 62534, 64047, 65535,
};

/* 2^(i / 32), Q15 */
static const uint16_t harm_exp2_tab[33] = {
    32768, 33486, 34219, 34968, 35734, 36516, 37316, 38133, 38968, 39821, 40693, 41584, 42495, 43425, 44376, 45348,
    46341, 47356, 48393, 49452, 50535, 51642, 52773, 53928, 55109, 56316, 57549, 58809, 60097, 61413, 62757, 64132,
    65535,
};

/* log2(k) in Q16 for k >= 1 */
static inline int32_t harm_log2(uint32_t k)
{
    int msb = 0;
    while (k >> (msb + 1))
        msb++;
    uint32_t m = (k << (16 - msb)) & 0xFFFF;   /* fraction, Q16 (k < 2^16) */
    uint32_t i = m >> 11, f = m & 0x7FF;
    return (msb << 16) + harm_log2_tab[i] + (int32_t)(((harm_log2_tab[i + 1] - harm_log2_tab[i]) * f) >> 11);
}

/* 2^20 * 2^(-a / 65536) for a >= 0 (Q16 octaves), 0 beyond 16 octaves */
static inline int32_t harm_amp(int32_t a)
{
    if (a < 0)
        a = 0;
    if (a >= 16 << 16)
        return 0;
    int32_t y = (20 << 16) - a;
    int n = y >> 16;
    uint32_t f = (uint32_t)y & 0xFFFF, i = f >> 11, fr = f & 0x7FF;
    uint32_t m = harm_exp2_tab[i] + (((harm_exp2_tab[i + 1] - harm_exp2_tab[i]) * fr) >> 11);   /* Q15, 1 .. 2 */
    return (int32_t)(n >= 15 ? m << (n - 15) : m >> (15 - n));
}

#define HARM_OFF 0x40000000                 /* attenuation of a harmonic that is off */

/* Attenuation (Q16 octaves, may be negative after tilt) of every harmonic 1..WTI_H into att[], HARM_OFF = off.
 * Returns the highest harmonic present. */
static inline int harm_atten(const int16_t *k, int32_t *att)
{
    int tilt = harm_tilt(k), ext = harm_extend(k), top = 0;
    for (int h = 1; h <= WTI_H; h++)
        att[h] = HARM_OFF;
    for (int i = 0; i < HARM_N; i++) {
        int v = harm_level(k, i);
        if (v) {
            att[i + 1] = (HARM_MAXLEVEL - v) * HARM_DB3;
            top = i + 1;
        }
    }
    if (ext) {
        int32_t fade = (16 - ext) * HARM_EXT_STEP;
        for (int r = 1, h = HARM_N + 1; h <= WTI_H; r++)
            for (int j = 0; j < 12 && h <= WTI_H; j++, h++)
                if (att[HARM_N - 11 + j] != HARM_OFF) {
                    att[h] = att[HARM_N - 11 + j] + r * fade;
                    top = h;
                }
    }
    if (tilt) {
        for (int h = 2; h <= top; h++)
            if (att[h] != HARM_OFF)
                att[h] -= ((harm_log2((uint32_t)h) >> 6) * (tilt * HARM_TILT_STEP)) >> 10;
    }
    return top;
}

/* Build the frame for knobs k into dst (FRAME_STRIDE samples). w: scratch (struct wti_work, ~34 KB). */
static inline void harm_build(const int16_t *k, int16_t *dst, struct wti_work *w)
{
    int32_t *att = w->re;                       /* scratch until the spectrum is set */
    int top = harm_atten(k, att);
    int32_t lo = 0x7FFFFFFF;
    for (int h = 1; h <= top; h++)
        if (att[h] < lo)
            lo = att[h];
    uint32_t sq = 0;                            /* sum of (a >> 9)^2 < 2^30 */
    int32_t sum = 0;
    for (int h = 0; h <= WTI_H; h++) {
        int32_t a = h && h <= top && att[h] != HARM_OFF ? harm_amp(att[h] - lo) : 0;
        if (a < 16)
            a = 0;
        w->cr[h] = 0;
        w->ci[h] = -a;                          /* sine phase */
        sq += (uint32_t)((a >> 9) * (a >> 9));
        sum += a;
    }
    int sh = 0;                                 /* 2 sum |c| < 2^30 (true for <= 256 harmonics at <= 2^20) */
    while ((sum >> sh) > (1 << 28))
        sh++;
    int32_t peak = wti_levels(w, top, sh);
    int32_t rms = (int32_t)(isqrt32(sq) << 9) >> sh;        /* sqrt(sum a^2), in the shifted units */
    /* single harmonic of amplitude a: IFFT peak 2a; want 0.6 FS = 29490 * 2a / denom, i.e. denom = 3 sqrt(sum a^2) */
    int32_t denom = 3 * rms > peak ? 3 * rms : peak;
    wti_store(w, denom, dst);
}

#endif
