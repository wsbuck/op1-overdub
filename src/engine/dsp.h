/* Shared fixed-point DSP helpers for engines (header-only, no libc/libgcc).
 * Audio Q24 (1.0 = 2^24); Q31 coefficients; filter g/k in Q28 while computing coefficients. */
#ifndef DSP_H
#define DSP_H
#include <stdint.h>

#define ONE 0x01000000          /* 1.0 in Q24 */

/* Q31 multiply: (a * b) >> 31. GCC 4.3 would call libgcc __muldi3 for a 64-bit product. */
#ifdef __bfin__
#define qmul(a, b) __builtin_bfin_mult_fr1x32x32((a), (b))
#else
static inline int32_t qmul(int32_t a, int32_t b)
{
    int64_t p = ((int64_t)a * b) >> 31;
    return p > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)p;
}
#endif

static inline int32_t clamp_knob(int16_t v)
{
    return v < 0 ? 0 : v;
}

/* n / d, d != 0. Restoring division; for a few calls per block only. */
static inline uint32_t udiv(uint32_t n, uint32_t d)
{
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    return q;
}

/* 1.0 / D in Q31 for D in Q28, D > 1.0 and D < 2^31: floor(2^59 / D). */
static inline int32_t recip_q31(uint32_t d)
{
    uint32_t q = 0, r = 1u << 28;
    for (int i = 0; i < 31; i++) {
        r <<= 1;
        q <<= 1;
        if (r >= d) {
            r -= d;
            q |= 1;
        }
    }
    return (int32_t)q;
}

/* floor(sqrt(v)) */
static inline uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* Trapezoidal (zero-delay feedback) state-variable low-pass, Simper / Cytomic 2013. */
struct svf {
    int32_t a1, a2, a3;         /* Q31 */
};

/* g = tan(pi fc / fs) and k = 1/Q, both Q28; g (g + k) must stay below 7 (Q28 in int32). */
static inline struct svf svf_coefs(int32_t g, int32_t k)
{
    struct svf f;
    int32_t ggk = qmul(g, g + k) << 3;                   /* g (g + k), Q28 */
    f.a1 = recip_q31((uint32_t)((1 << 28) + ggk));       /* 1 / (1 + g (g + k)) */
    f.a2 = qmul(g, f.a1) << 3;                           /* g a1 */
    f.a3 = qmul(g, f.a2) << 3;                           /* g a2 */
    return f;
}

/* One sample: v0 in, returns the low-pass output; ic1/ic2 are the filter state. */
static inline int32_t svf_lp(const struct svf *f, int32_t v0, int32_t *ic1, int32_t *ic2)
{
    int32_t v3 = v0 - *ic2;
    int32_t v1 = qmul(f->a1, *ic1) + qmul(f->a2, v3);
    int32_t v2 = *ic2 + qmul(f->a2, *ic1) + qmul(f->a3, v3);
    *ic1 = 2 * v1 - *ic1;
    *ic2 = 2 * v2 - *ic2;
    return v2;
}

/* Cutoff knob (0..0x7FFF) -> g (Q28) from a VA_CUTOFF_POINTS table, linear interpolation. */
static inline int32_t cutoff_g(const int32_t *table, int32_t knob)
{
    int32_t idx = knob >> 9, frac = knob & 511;
    return table[idx] + (((table[idx + 1] - table[idx]) >> 9) * frac);
}

/* x in Q15 (any range) -> Q15 soft clip: x - 4x^3/27 on |x| <= 1.5, +-1 beyond. */
/* Linear up to KNEE_T (0.75 FS), then a quadratic knee reaching full scale with zero slope at
 * x = KNEE_T + 2 (32767 - KNEE_T): transparent at normal levels (a pure sine stays pure), soft only near the top. */
#define KNEE_T 24576
static inline int16_t soft_knee(int32_t x)
{
    int32_t ax = x < 0 ? -x : x;
    if (ax > KNEE_T) {
        int32_t d = ax - KNEE_T;
        ax = d >= 2 * (32767 - KNEE_T) ? 32767 : KNEE_T + d - ((d * d) >> 15);   /* d^2 / (4 * 8191) ~ d^2 >> 15 */
    }
    return (int16_t)(x < 0 ? -ax : ax);
}

static inline int16_t soft_clip(int32_t x)
{
    if (x >= 49152)
        return 32767;
    if (x <= -49152)
        return -32767;
    uint32_t ax = (uint32_t)(x < 0 ? -x : x);
    int32_t x2 = (int32_t)((ax * ax) >> 15);
    int32_t x3 = (x2 * (x >> 1)) >> 14;
    return (int16_t)(x - ((x3 * 4855) >> 15));
}

#endif
