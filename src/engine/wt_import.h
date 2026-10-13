/* Custom wavetable bank from a sample (prototype; notes/wavetable-custom.md). Portable C, no libc/libgcc, no division
 * operator, no writable globals: builds 12 frames in wt_layout.h's format (9 band-limited levels per frame) from
 * any int16 sample, e.g. the synth sampler's trimmed window (grain_source.h).
 *
 *   1. Period: autocorrelation on a 4x decimated centre segment (lags 20..2048 samples = 2.2 kHz..21.5 Hz),
 *      smallest lag within 1/8 of the best peak (avoids octave-down errors), refined at full rate with AMDF and a
 *      parabola. Checked first: a Serum-style table (one single-cycle frame per 2048 samples from the start of the
 *      buffer) is recognised by its seams: on the 2048 grid each frame's end continues into its own start about as
 *      well as into the next frame (wti_serum). Unpitched or too short: fixed 2048-sample slices.
 *   2. Per frame (12 evenly spaced over the window): one period, cut where the signal is flat and repeats best
 *      (wti_cut), resampled to 2048 points (linear), the loop seam closed by a ramp, 2048-point fixed-point FFT, DC and harmonics above min(256, 0.45 P) removed, the linear
 *      interpolation's sinc^2 droop undone (P <= 1536), phase rotated so harmonic 1 is a sine (smooth morphs).
 *   3. Levels: inverse FFT of harmonics 1..256 >> L at size 1 << level_bits[L], as gen_wt_data.py; the whole frame
 *      (all levels) peak-normalized to 0.9 FS, like gen_wt_data.py. Band-limited to degree H at 8 points per
 *      harmonic, so by Bernstein's inequality adjacent samples differ by at most 2 pi / 8 * peak = 23,160 < 32767
 *      (wt.c's 16-bit interpolation deltas stay valid).
 * Work in steps (wti_step): the period first, then one frame per call, so a UI hook can spread it over frames.
 */
#ifndef WT_IMPORT_H
#define WT_IMPORT_H
#include <stdint.h>
#include "dsp.h"
#include "sine_table.h"
#include "wt_layout.h"

#define WTI_N 2048
#define WTI_LOG2N 11
#define WTI_H 256
#define WTI_FIXED (2048u << 16)                 /* fallback period: Serum-style 2048-sample frames */

struct wti_work {                               /* ~34.5 KB of scratch RAM (FREE region) */
    int32_t re[WTI_N], im[WTI_N];               /* FFT buffers; period detection reuses them */
    int32_t cr[WTI_H + 1], ci[WTI_H + 1];       /* the current frame's spectrum, harmonic k at [k] */
    int32_t lv[FRAME_STRIDE];                   /* all 9 levels before quantisation */
};

struct wti_job {
    const int16_t *x;                           /* window start: x[0 .. len - 1] readable */
    uint32_t len;
    uint32_t align;                             /* x's offset from the start of the buffer (2048 grid) */
    int16_t *dst;                               /* FRAMES * FRAME_STRIDE samples */
    uint32_t p_q16;                             /* period, samples Q16 (stage >= 1) */
    uint32_t span;                              /* distance between frame starts */
    int32_t stage;                              /* 0 period, 1..FRAMES frame stage - 1, FRAMES + 1 done, < 0 failed */
    int32_t pitched;                            /* 2 Serum-style grid, 1 detected period, 0 fixed slices */
};

/* uncorrected 1 / sinc^2(i / 128), Q14, i = 0..64: linear interpolation's droop at harmonic/period = i / 128 */
static const uint16_t wti_invsinc2[65] = {
    16384, 16387, 16397, 16414, 16437, 16466, 16503, 16546, 16596, 16653, 16717, 16788, 16866, 16951, 17044, 17145,
    17253, 17369, 17493, 17625, 17766, 17915, 18074, 18241, 18418, 18605, 18801, 19009, 19226, 19455, 19696, 19948,
    20213, 20491, 20782, 21086, 21406, 21740, 22091, 22458, 22842, 23244, 23665, 24105, 24567, 25050, 25556, 26086,
    26641, 27223, 27833, 28472, 29143, 29847, 30586, 31361, 32176, 33032, 33932, 34879, 35875, 36925, 38030, 39196,
    40426,
};

/* atan(2^-i) in uint32 turns, CORDIC */
static const uint32_t wti_atan[24] = {
    0x20000000u, 0x12e4051eu, 0x9fb385bu, 0x51111d4u, 0x28b0d43u, 0x145d7e1u, 0xa2f61eu, 0x517c55u,
    0x28be53u, 0x145f2fu, 0xa2f98u, 0x517ccu, 0x28be6u, 0x145f3u, 0xa2fau, 0x517du,
    0x28beu, 0x145fu, 0xa30u, 0x518u, 0x28cu, 0x146u, 0xa3u, 0x51u,
};

/* sin of a uint32 turn, Q31 (sine256 interpolated: ~3e-5 error) */
static inline int32_t wti_sin(uint32_t a)
{
    uint32_t i = a >> 24;
    int32_t f = (int32_t)((a >> 9) & 0x7FFF);
    return (sine256[i] + (((sine256[i + 1] - sine256[i]) * f) >> 15)) << 16;
}
#define wti_cos(a) wti_sin((a) + 0x40000000u)

static inline int32_t wti_abs(int32_t v)
{
    return v < 0 ? -v : v;
}

/* Angle of (x, y) in uint32 turns (CORDIC vectoring; |x|, |y| < 2^29). */
static inline uint32_t wti_angle(int32_t x, int32_t y)
{
    uint32_t a = 0;
    if (x < 0) {                                /* rotate by 180 degrees into the right half plane */
        x = -x;
        y = -y;
        a = 0x80000000u;
    }
    for (int i = 0; i < 24; i++) {
        int32_t nx;
        if (y > 0) {
            nx = x + (y >> i);
            y -= x >> i;
            a += wti_atan[i];
        } else {
            nx = x - (y >> i);
            y += x >> i;
            a -= wti_atan[i];
        }
        x = nx;
    }
    return a;
}

/* In-place radix-2 complex FFT of size 1 << log2n. Forward: e^-i, scaled by 1/2 per stage (output = 1/N sum).
 * Inverse: e^+i, unscaled (caller keeps sum |input| < 2^30). */
static inline void wti_fft(int32_t *re, int32_t *im, int log2n, int inverse)
{
    const int n = 1 << log2n;
    for (int i = 1, j = 0; i < n; i++) {        /* bit reversal */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            int32_t t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int lg = 1; lg <= log2n; lg++) {
        const int len = 1 << lg, half = len >> 1;
        const uint32_t step = 1u << (32 - lg);  /* one turn / len */
        for (int k = 0; k < half; k++) {
            int32_t c = wti_cos(step * (uint32_t)k), s = wti_sin(step * (uint32_t)k);
            if (!inverse)
                s = -s;
            for (int i = k; i < n; i += len) {
                int j = i + half;
                int32_t tr = qmul(re[j], c) - qmul(im[j], s);
                int32_t ti = qmul(re[j], s) + qmul(im[j], c);
                if (inverse) {
                    re[j] = re[i] - tr;
                    im[j] = im[i] - ti;
                    re[i] += tr;
                    im[i] += ti;
                } else {
                    re[j] = (re[i] - tr) >> 1;
                    im[j] = (im[i] - ti) >> 1;
                    re[i] = (re[i] + tr) >> 1;
                    im[i] = (im[i] + ti) >> 1;
                }
            }
        }
    }
}

/* Period of x[0 .. len - 1] in samples, Q16, or 0 if unpitched/too short. Uses w->re and w->im. */
static inline uint32_t wti_period(const int16_t *x, uint32_t len, struct wti_work *w)
{
    int16_t *d = (int16_t *)w->re;                          /* 16-bit: 16 x 16 MACs */
    int32_t *r = w->im;
    uint32_t nd = len >> 2 < 1024 ? len >> 2 : 1024;      /* decimated samples (4096 = 93 ms at most) */
    int maxlag = nd >> 1 < 512 ? (int)(nd >> 1) : 512;
    int W = (int)nd - maxlag;
    if (maxlag < 16)
        return 0;
    const int16_t *c = x + ((len - (nd << 2)) >> 1);       /* centre segment */
    for (uint32_t i = 0; i < nd; i++)                       /* 4x box decimation, 10 bits: sums stay < 2^30 */
        d[i] = (int16_t)((c[4 * i] + c[4 * i + 1] + c[4 * i + 2] + c[4 * i + 3]) >> 7);
    for (int t = 0; t <= maxlag; t++) {
        int32_t s = 0;
        for (int i = 0; i < W; i++)
            s += d[i] * d[i + t];
        r[t] = s;
    }
    int32_t r0 = r[0], best = 0;
    if (r0 < 4 * W)                                         /* silence (about -50 dBFS) */
        return 0;
    for (int t = 5; t < maxlag; t++)
        if (r[t] >= r[t - 1] && r[t] > r[t + 1] && r[t] > best)
            best = r[t];
    if ((best >> 4) * 10 < (r0 >> 4) * 3)                   /* weaker than 0.3 r0: unpitched */
        return 0;
    int tau = 0;
    for (int t = 5; t < maxlag; t++)
        if (r[t] >= r[t - 1] && r[t] > r[t + 1] && r[t] >= best - (best >> 3)) {
            tau = t;
            break;
        }
    /* refine at full rate: AMDF over +-5 samples around 4 tau */
    int32_t lo = 4 * tau - 5, hi = 4 * tau + 5;
    if (lo < 2)
        lo = 2;
    int32_t m = (int32_t)len - hi - 2;
    if (m > 2048)
        m = 2048;
    if (m < 64)
        return 0;
    const int16_t *a = x + ((len - (uint32_t)(m + hi)) >> 1);
    int32_t dmin = 0x7FFFFFFF, lmin = lo;
    for (int32_t l = lo; l <= hi; l++) {
        int32_t s = 0;
        for (int32_t i = 0; i < m; i++)
            s += wti_abs(a[i] - a[i + l]) >> 2;             /* 2048 * 2^14 < 2^31 */
        r[l - lo] = s;
        if (s < dmin) {
            dmin = s;
            lmin = l;
        }
    }
    int32_t frac = 0;                                       /* parabola through the minimum, Q16, |frac| <= 0.5 */
    if (lmin > lo && lmin < hi) {
        int32_t dm = r[lmin - 1 - lo], d0 = r[lmin - lo], dp = r[lmin + 1 - lo];
        int32_t num = dm - dp, den = 2 * (dm - 2 * d0 + dp);
        while (wti_abs(num) >= 1 << 15 || den >= 1 << 15) {
            num >>= 1;
            den >>= 1;
        }
        if (den > 0) {
            frac = (int32_t)udiv((uint32_t)wti_abs(num) << 16, (uint32_t)den);
            if (frac > 0x8000)
                frac = 0x8000;
            if (num < 0)
                frac = -frac;
        }
    }
    uint32_t p = ((uint32_t)lmin << 16) + (uint32_t)frac;
    if (p > WTI_FIXED - (WTI_FIXED >> 8) && p < WTI_FIXED + (WTI_FIXED >> 8))
        p = WTI_FIXED;                                      /* Serum-style table: exactly 2048 */
    return p;
}

/* Serum-style table test: up to 32 frames on the 2048 grid (grid offset g = first frame start in x). Extrapolating
 * each frame's last two samples, a table frame continues into its own first sample about as well as into the next
 * frame's first sample; a continuous recording continues only into the next sample (the next frame), and its own
 * first sample is 2047 samples back, unrelated. Noise is ruled out by requiring the inside (second differences at 4
 * points per frame) to be smooth against the signal level. 1 if it looks like a table. */
static inline int wti_serum(const int16_t *x, uint32_t len, uint32_t g)
{
    int32_t wrap = 0, next = 0, in = 0, lvl = 0, n = 0;
    for (uint32_t s = g; s + 2049 <= len && n < 32; s += 2048, n++) {
        const int16_t *f = x + s;
        int32_t ext = 2 * f[2047] - f[2046];               /* the frame's next sample, extrapolated */
        wrap += wti_abs(f[0] - ext) >> 2;
        next += wti_abs(f[2048] - ext) >> 2;
        for (int q = 1; q <= 4; q++) {
            const int16_t *c = f + q * 409;
            in += wti_abs(c[1] - 2 * c[0] + c[-1]) >> 4;
            lvl += wti_abs(c[0]) >> 4;
        }
    }
    if (n < 2)
        return 0;
    return wrap <= 2 * next + (lvl >> 6) && 4 * in <= lvl && lvl > 16 * n;
}

/* Linear interpolation of x at Q16 position pos. */
static inline int32_t wti_at(const int16_t *x, uint32_t pos)
{
    uint32_t i = pos >> 16;
    int32_t f = (int32_t)((pos >> 1) & 0x7FFF);
    return x[i] + (((x[i + 1] - x[i]) * f) >> 15);
}

/* One frame: the period starting at x[0] (x[0 .. P + 1] readable) into dst[FRAME_STRIDE]. seam: close the loop
 * with a ramp towards x[P] (the next cycle of a recording); 0 for a table frame, which wraps onto its own x[0]. */
static inline void wti_frame(const int16_t *x, uint32_t p_q16, int seam, int16_t *dst, struct wti_work *w)
{
    int32_t *re = w->re, *im = w->im;
    const uint32_t step = (p_q16 + 1024) >> 11;
    const int32_t y0 = x[0], e = seam ? wti_at(x, p_q16) : y0;
    uint32_t pos = 0;
    for (int j = 0; j < WTI_N; j++, pos += step) {          /* resample to 2048, Q24, seam closed by a ramp */
        re[j] = (wti_at(x, pos) << 9) - (((e - y0) * j) >> 2);
        im[j] = 0;
    }
    wti_fft(re, im, WTI_LOG2N, 0);

    /* harmonics kept: 1 .. kmax, below 0.45 x the source's Nyquist (P / 2) and 256 */
    int32_t pint = (int32_t)(p_q16 >> 16);
    int kmax = (pint * 29) >> 6;
    if (kmax > WTI_H)
        kmax = WTI_H;
    /* sinc^2 droop of the linear interpolation (when it interpolates: P < 2048; above 1536 it is < 0.8 dB and the
     * images fold back into the band, so it is left alone) */
    uint32_t m = pint <= 1536 ? udiv(1u << 31, p_q16 >> 8) : 0;     /* 2^39 / P_q16: k * m >> 16 = 128 k / P */
    int32_t sum = 0, top = 0;
    for (int k = 1; k <= WTI_H; k++) {
        int32_t cr = 0, ci = 0;
        if (k <= kmax) {
            cr = re[k];
            ci = im[k];
            if (m) {
                uint32_t u = (uint32_t)k * m;
                uint32_t i = u >> 16, f = (u >> 8) & 0xFF;
                int32_t g = wti_invsinc2[i] + ((((int32_t)wti_invsinc2[i + 1] - wti_invsinc2[i]) * (int32_t)f) >> 8);
                cr = qmul(cr, g << 15) << 2;
                ci = qmul(ci, g << 15) << 2;
            }
        }
        w->cr[k] = cr;
        w->ci[k] = ci;
        int32_t mag = wti_abs(cr) + wti_abs(ci);
        sum += mag;
        if (mag > top)
            top = mag;
    }
    /* phase: harmonic 1 to sine phase (angle -1/4 turn), all harmonics rotated with it, so neighbouring frames
     * line up and the morph does not cancel. Skipped if harmonic 1 is too weak to have a reliable phase. */
    if (wti_abs(w->cr[1]) + wti_abs(w->ci[1]) > top >> 5) {
        uint32_t th = 0xC0000000u - wti_angle(w->cr[1], w->ci[1]);
        for (int k = 1; k <= kmax; k++) {
            uint32_t a = th * (uint32_t)k;
            int32_t c = wti_cos(a), s = wti_sin(a), cr = w->cr[k], ci = w->ci[k];
            w->cr[k] = qmul(cr, c) - qmul(ci, s);
            w->ci[k] = qmul(cr, s) + qmul(ci, c);
        }
    }
    int sh = 0;                                             /* inverse FFT headroom: 2 sum |c| < 2^30 */
    while ((sum >> sh) > (1 << 28))
        sh++;

    /* levels */
    int32_t peak = 0;
    for (int L = 0; L < LEVELS; L++) {
        const int lb = level_bits[L], size = 1 << lb;
        int h = WTI_H >> L;
        if (h > kmax)
            h = kmax;
        for (int j = 0; j < size; j++)
            re[j] = im[j] = 0;
        for (int k = 1; k <= h; k++) {
            re[k] = re[size - k] = w->cr[k] >> sh;
            im[k] = w->ci[k] >> sh;
            im[size - k] = -im[k];
        }
        wti_fft(re, im, lb, 1);
        int32_t *o = w->lv + level_off[L];
        for (int j = 0; j < size; j++) {
            o[j] = re[j];
            if (wti_abs(re[j]) > peak)
                peak = wti_abs(re[j]);
        }
    }
    /* normalise the whole frame to 0.9 FS (29490), as gen_wt_data.py */
    if (!peak) {
        for (int j = 0; j < FRAME_STRIDE; j++)
            dst[j] = 0;
        return;
    }
    int ls = 0;
    while ((peak << ls) < (1 << 28))
        ls++;
    int32_t g = (int32_t)udiv(29490u << 17, (uint32_t)(peak << ls) >> 14);      /* 29490 * 2^31 / peak' */
    for (int j = 0; j < FRAME_STRIDE; j++) {
        int32_t v = qmul(w->lv[j] << ls, g);
        dst[j] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    }
}

/* Where to cut a recorded cycle: the offset in [0, P) (x[0 .. avail - 1] readable) where the signal is flattest and
 * one period later most alike, so the loop seam (closed by a ramp in wti_frame) never lands on an edge. */
static inline uint32_t wti_cut(const int16_t *x, uint32_t p, uint32_t avail)
{
    uint32_t best = 0, n = avail > 2 * p + 3 ? p : (avail > p + 3 ? avail - p - 3 : 0);
    int32_t bv = 0x7FFFFFFF;
    for (uint32_t i = 0; i < n; i++) {
        int32_t v = wti_abs(x[i] - x[i + p]) + wti_abs(x[i + 1] - x[i]);
        if (v < bv) {
            bv = v;
            best = i;
        }
    }
    return best;
}

static inline void wti_begin(struct wti_job *j, const int16_t *x, uint32_t len, uint32_t align, int16_t *dst)
{
    j->x = x;
    j->len = len;
    j->align = align;
    j->dst = dst;
    j->stage = 0;
    j->p_q16 = 0;
    j->span = 0;
    j->pitched = 0;
}

/* One step: the period (stage 0), then one frame per call. Returns 1 while more steps remain, 0 when the bank is
 * complete, -1 if the window is too short for one period. */
static inline int wti_step(struct wti_job *j, struct wti_work *w)
{
    if (j->stage < 0)
        return -1;
    if (j->stage == 0) {
        uint32_t g = (2048 - (j->align & 2047)) & 2047;    /* first grid point in x */
        uint32_t p = 0;
        if (wti_serum(j->x, j->len, g))
            j->pitched = 2;
        else
            j->pitched = (p = wti_period(j->x, j->len, w)) != 0;
        j->p_q16 = p ? p : WTI_FIXED;
        uint32_t need = (j->p_q16 >> 16) + 3;
        if (j->len < need) {
            j->stage = -1;
            return -1;
        }
        j->span = udiv(j->len - need, FRAMES - 1);
        j->stage = 1;
        return 1;
    }
    if (j->stage > FRAMES)
        return 0;
    int f = j->stage - 1;
    uint32_t start = j->span * (uint32_t)f;
    if (j->p_q16 == WTI_FIXED) {                            /* 2048-sample frames: snap to the buffer's 2048 grid */
        uint32_t g = (2048 - (j->align & 2047)) & 2047;
        uint32_t s = start < g ? g : ((start - g + 1024) & ~2047u) + g;
        while (s + 2051 > j->len && s >= 2048 + g)
            s -= 2048;
        if (s + 2051 <= j->len)
            start = s;
    }
    if (j->pitched == 1)
        start += wti_cut(j->x + start, j->p_q16 >> 16, j->len - start);
    wti_frame(j->x + start, j->p_q16, j->pitched != 2, j->dst + f * FRAME_STRIDE, w);
    j->stage++;
    return j->stage <= FRAMES;
}

#endif
