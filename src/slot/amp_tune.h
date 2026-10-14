/* The amp screen's tuner (src/slot/ui.c): pitch of TE IN, computed on the UI thread. Design: notes/amp.md.
 * Portable C, no division operator, no writable globals (the caller keeps struct amp_tuner).
 *
 * Input: AMP_TUNE_BLOCKS consecutive 64-sample blocks of TE IN (44.1 kHz, 18.9 ms), captured by the caller.
 * YIN (de Cheveigne and Kawahara 2002) on the block decimated 2x (22.05 kHz, 416 samples), scaled to a 9-bit peak: difference
 * function over a 96-sample window for lags 8..310 (2.76 kHz .. 71 Hz: drop D to high E's 24th fret), cumulative-mean normalised;
 * the first lag under 0.15 (descending to its local minimum), else unpitched (a weaker fallback gave wrong notes);
 * then the difference function at the full rate around twice that lag, and a parabola through its minimum, refine it. Silence (peak under -50 dBFS) is unpitched.
 * Display: the median of the last three readings; the note changes only when two readings agree; cents are smoothed;
 * the reading is held for AMP_TUNE_HOLD updates after the signal goes, then cleared.
 */
#ifndef AMP_TUNE_H
#define AMP_TUNE_H
#include <stdint.h>
#include "dsp.h"

#define AMP_TUNE_BLOCKS 13
#define AMP_TUNE_N (AMP_TUNE_BLOCKS * 64)
#define AMP_TUNE_W 96
#define AMP_TUNE_LAG0 8
#define AMP_TUNE_LAG1 310
#define AMP_TUNE_SILENT 104                 /* -50 dBFS peak */
#define AMP_TUNE_HOLD 6

/* log2(1 + i / 64), Q16, i = 0..64 */
static const uint16_t amp_log2_tab[65] = {
    0, 1466, 2909, 4331, 5732, 7112, 8473, 9814, 11136, 12440, 13727, 14996, 16248, 17484, 18704, 19909, 21098,
    22272, 23433, 24579, 25711, 26830, 27936, 29029, 30109, 31178, 32234, 33279, 34312, 35334, 36346, 37346, 38336, 39316,
    40286, 41246, 42196, 43137, 44068, 44990, 45904, 46809, 47705, 48593, 49472, 50344, 51207, 52063, 52911, 53751, 54584,
    55410, 56229, 57040, 57845, 58643, 59434, 60219, 60997, 61769, 62534, 63294, 64047, 64794, 65535 };

/* log2(v) in Q16 for v >= 1 */
static inline int32_t amp_log2(uint32_t v)
{
    int n = 31;
    while (!(v >> n))
        n--;
    uint32_t m = n >= 22 ? v >> (n - 22) : v << (22 - n);   /* 1.22 fixed: 2^22 .. 2^23 - 1 */
    uint32_t i = (m >> 16) & 63, f = m & 0xFFFF;
    return (n << 16) + amp_log2_tab[i] + (int32_t)(((amp_log2_tab[i + 1] - amp_log2_tab[i]) * f) >> 16);
}

struct amp_tune_work {                     /* scratch (2 KB: kept off the UI thread's stack) */
    int16_t y[AMP_TUNE_N / 2];
    uint32_t d[AMP_TUNE_LAG1 + 2];
};

/* Pitch of x[0 .. AMP_TUNE_N - 1] as a MIDI note in Q16 (69 << 16 = A4 = 440 Hz), or -1 if unpitched or silent. */
static inline int32_t amp_tune_pitch(const int16_t *x, struct amp_tune_work *w)
{
    int16_t *y = w->y;
    uint32_t *d = w->d;
    int32_t peak = 0;
    for (int i = 0; i < AMP_TUNE_N / 2; i++) {
        int32_t v = (x[2 * i] + x[2 * i + 1]) >> 1;
        y[i] = (int16_t)v;
        v = v < 0 ? -v : v;
        peak = v > peak ? v : peak;
    }
    if (peak < AMP_TUNE_SILENT)
        return -1;
    int sh = 0, up = 0;
    while ((peak >> sh) >= 512)            /* peak 256..511: squared differences x 96 stay below 2^28 */
        sh++;
    while ((peak << up) < 256)
        up++;
    int32_t mean = 0;
    for (int i = 0; i < AMP_TUNE_N / 2; i++)
        mean += y[i];
    mean = ((mean >> 7) * 315) >> 10;      /* / 416 (approximately): only to keep a DC offset from eating bits */
    for (int i = 0; i < AMP_TUNE_N / 2; i++)
        y[i] = (int16_t)(((y[i] - mean) << up) >> sh);

    uint32_t sum = 0;
    int first = -1;
    for (int t = 1; t <= AMP_TUNE_LAG1 + 1; t++) {
        uint32_t acc = 0;
        for (int j = 0; j < AMP_TUNE_W; j++) {
            int32_t e = y[j] - y[j + t];
            acc += (uint32_t)(e * e);
        }
        d[t] = acc;
        sum += acc >> 8;
        if (t < AMP_TUNE_LAG0 || t > AMP_TUNE_LAG1)
            continue;
        uint32_t c = udiv((acc >> 8) * (uint32_t)t, (sum >> 12) + 1);   /* cumulative mean normalised, Q12 */
        if (first < 0 && c < 614)          /* 0.15 */
            first = t;
    }
    int t = first;
    if (t < 0)
        return -1;
    while (t < AMP_TUNE_LAG1 && d[t + 1] < d[t])
        t++;
    /* refine at the full rate: the difference function over 2 x 96 samples at lags 2t - 2 .. 2t + 2, its minimum, and a
     * parabola through it and its neighbours: offset = (a - c) / (2 (a - 2 b + c)), Q8 */
    uint32_t e[5];
    int k = 0;
    for (int i = 0; i < 5; i++) {
        int lag = 2 * t - 2 + i;
        uint32_t acc = 0;
        for (int j = 0; j < 2 * AMP_TUNE_W; j++) {
            int32_t v = ((x[j] - x[j + lag]) << up) >> (sh + 1);
            acc += (uint32_t)(v * v);
        }
        e[i] = acc;
        if (acc < e[k])
            k = i;
    }
    k = k < 1 ? 1 : k > 3 ? 3 : k;
    int32_t a = (int32_t)(e[k - 1] >> 4), b = (int32_t)(e[k] >> 4), c = (int32_t)(e[k + 1] >> 4);
    int32_t den = a - 2 * b + c, num = a - c, off = 0;
    if (den > 0) {
        uint32_t q = udiv((uint32_t)(num < 0 ? -num : num) << 7, (uint32_t)den);
        off = q > 128 ? 128 : (int32_t)q;
        off = num < 0 ? -off : off;
    }
    uint32_t p_q8 = ((uint32_t)(2 * t - 2 + k) << 7) + (uint32_t)(off >> 1);   /* period at 22.05 kHz, Q8 */
    /* midi = 69 + 12 log2(22050 / (440 P)) */
    int32_t l = amp_log2(p_q8) - (8 << 16);
    return (69 << 16) + 12 * (370090 - l);              /* 370090 = log2(22050 / 440) Q16 */
}

struct amp_tuner {
    int32_t hist[3];                       /* the last readings (MIDI Q16, -1 none) */
    int32_t note;                          /* shown note (-1 none) */
    int32_t cents;                         /* shown cents, Q8 */
    int32_t pending;                       /* a different note seen once */
    int32_t age;                           /* updates since the last pitched reading */
};

static inline void amp_tune_reset(struct amp_tuner *t)
{
    t->hist[0] = t->hist[1] = t->hist[2] = -1;
    t->note = t->pending = -1;
    t->cents = 0;
    t->age = AMP_TUNE_HOLD;
}

/* One reading (amp_tune_pitch, or -1) into the display state. */
static inline void amp_tune_update(struct amp_tuner *t, int32_t m)
{
    t->hist[2] = t->hist[1];
    t->hist[1] = t->hist[0];
    t->hist[0] = m;
    if (m < 0) {
        if (++t->age >= AMP_TUNE_HOLD) {
            t->note = t->pending = -1;
            t->age = AMP_TUNE_HOLD;
        }
        return;
    }
    t->age = 0;
    int32_t a = t->hist[0], b = t->hist[1] < 0 ? a : t->hist[1], c = t->hist[2] < 0 ? b : t->hist[2];
    int32_t med = a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
    int32_t n = (med + 32768) >> 16;
    int32_t cents = (((med - (n << 16)) >> 4) * 100) >> 4;      /* Q8 */
    if (n != t->note) {
        if (t->note >= 0 && n != t->pending) {
            t->pending = n;                                     /* wait for a second reading */
            return;
        }
        t->note = n;
        t->cents = cents;
    } else {
        t->cents += (cents - t->cents) >> 1;
    }
    t->pending = -1;
}

#endif
