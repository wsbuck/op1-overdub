/* The knob hook's raw delta, small and grey in a corner of the hooked engines' screens (ui_harm.h, ui_fm4.h), so the
 * user can report TE's delta per detent and its acceleration (notes/knob-hook.md): "D<last delta> M<smallest |delta|
 * since boot>". Nothing until the first hook call. For inclusion after ui.c's primitives (text_fast, FP).
 * HOOK_DIAG: the hook's record (knobhook.c knobhook_diag; weak in ui.c, so 0 in builds without the hook).
 */
#ifndef UI_HOOKDIAG_H
#define UI_HOOKDIAG_H
#include "sprobe.h"

#ifndef HOOK_DIAG
#define HOOK_DIAG (&knobhook_diag)
#endif

/* signed decimal, any int32 > INT32_MIN, without division; returns the end */
static char *hd_num(char *p, int32_t v)
{
    static const int32_t pow10[9] = { 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1 };
    if (v < 0) {
        *p++ = '-';
        v = -v;
    }
    int started = 0;
    for (int i = 0; i < 9; i++) {
        int dgt = 0;
        while (v >= pow10[i]) {
            v -= pow10[i];
            dgt++;
        }
        if (dgt || started || i == 8) {
            *p++ = (char)('0' + dgt);
            started = 1;
        }
    }
    *p = 0;
    return p;
}

/* top y; left edge x, or right edge x if right */
static void hook_delta_diag(void *r, fp32 x, fp32 y, int right)
{
    const struct kh_diag *d = HOOK_DIAG;
    if (!d || !d->calls)
        return;
    char buf[32], *p = buf;
    *p++ = 'D';
    p = hd_num(p, d->delta);
    *p++ = ' ';
    *p++ = 'M';
    p = hd_num(p, d->dmin);
    text_fast(r, buf, right ? x - (fp32)(p - buf) * FP(6) : x, y, FP(1), GREY);
}

#endif
