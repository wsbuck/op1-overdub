/* TE's raw encoder delta -> edit steps, for engines whose turns the knob hook handles (src/slot/knobhook.c).
 *
 * TE's delta per detent and its acceleration at the hook are not known yet (notes/knob-hook.md). So that the screens
 * behave whatever they turn out to be, every hook call with a nonzero delta is at least one step, and a larger delta
 * than the smallest one seen since boot (dmin, the delta of one slow detent once any slow turn happened) gives more,
 * in powers of two up to cap: |delta| >= 2 dmin -> 2, >= 4 dmin -> 4, ... cap 1 = exactly one step per call (cursor,
 * PAINT, STAMP, page: never skip). Only shifts and compares (no runtime division on the Blackfin).
 * The edit functions (harm_edit.h, fm4_edit.h) then take the steps as their delta (their *_DELTA_SHIFT stays 0).
 */
#ifndef HOOK_STEPS_H
#define HOOK_STEPS_H
#include <stdint.h>

static inline int32_t hook_steps(int32_t *dmin, int32_t delta, int32_t cap)
{
    if (!delta)
        return 0;
    int32_t m = delta < 0 ? -delta : delta;
    if (*dmin <= 0 || m < *dmin)
        *dmin = m;
    int32_t st = 1, t = *dmin << 1;
    while (st < cap && m >= t && t > 0) {
        st <<= 1;
        t <<= 1;
    }
    if (st > cap)
        st = cap;
    return delta < 0 ? -st : st;
}

#endif
