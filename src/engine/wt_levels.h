/* The level-building half of wt_import.h's wti_frame, split out so that other builders can feed it a spectrum
 * (harm_build.h: the drawn harmonics). wti_frame does the same steps inline today; when wt_import.h is next touched it
 * can call these (identical arithmetic: the inverse FFT per level and the 0.9 FS quantisation).
 *
 *   1. Put harmonic k's complex amplitude in w->cr[k], w->ci[k] (k = 1 .. WTI_H; sine phase is cr = 0, ci = -a).
 *   2. wti_levels(w, kmax, sh): the inverse FFT of harmonics 1 .. min(256 >> L, kmax) at size 1 << level_bits[L], for
 *      the 9 levels, into w->lv (int32, wt_layout.h's offsets). Inputs are shifted right by sh first: keep
 *      2 sum |cr| + |ci| < 2^30. Returns the peak |sample| over all levels.
 *   3. wti_store(w, denom, dst): dst = lv * 29490 / denom, clamped to +-32767. denom = the peak gives wt_import.h's
 *      per-frame normalisation (peak 0.9 FS); a larger denom plays quieter. Band-limited to degree H at 8 points per
 *      harmonic, a frame whose peak is <= 0.9 FS has adjacent-sample deltas <= 2 pi / 8 * 29490 = 23,161 (Bernstein),
 *      so wt.c's 16-bit interpolation deltas stay valid.
 */
#ifndef WT_LEVELS_H
#define WT_LEVELS_H
#include <stdint.h>
#include "wt_import.h"

static inline int32_t wti_levels(struct wti_work *w, int kmax, int sh)
{
    int32_t *re = w->re, *im = w->im, peak = 0;
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
    return peak;
}

static inline void wti_store(const struct wti_work *w, int32_t denom, int16_t *dst)
{
    if (denom <= 0) {
        for (int j = 0; j < FRAME_STRIDE; j++)
            dst[j] = 0;
        return;
    }
    int ls = 0;
    while ((denom << ls) < (1 << 28))
        ls++;
    int32_t g = (int32_t)udiv(29490u << 17, (uint32_t)(denom << ls) >> 14);     /* 29490 * 2^31 / denom' */
    for (int j = 0; j < FRAME_STRIDE; j++) {
        int32_t v = qmul(w->lv[j] << ls, g);
        dst[j] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    }
}

#endif
