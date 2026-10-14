/* fm4: what an encoder turn does once the encoder hook is confirmed (notes/fm-operator.md "Controls"). A pure function
 * of the UI state, the turn and the sound's 8 knob values. Without the hook, fm4 runs in its v0 form (fm4.h): 8 plain
 * knobs that TE applies itself, and this file is not used.
 *
 *   if (fm4_edit(&fm4_ui, knob, *delta, knobs) == FM4_DONE) *delta = 0;   then TE's own apply + store, as always
 *
 * knob = TE's knob index with type-3 rows (4..7 = SHIFT + blue/green/white/orange).
 *   SHIFT blue (4)  PAGE: MAIN, A, B, C, D (UI state only). Entering an operator page turns a v0 sound into the packed
 *                   form first (fm4_to_packed: its ratios, level 15, the v0 envelopes; detune is dropped).
 *   MAIN page       blue INDEX, green ALGORITHM, white TIME, orange FEEDBACK: FM4_PASS (TE applies them, plain values;
 *                   TE's LFO can target them). SHIFT green/white/orange: FM4_PASS on a v0 sound (its ratio and detune
 *                   knobs), ignored on a packed one.
 *   operator page   blue COARSE ratio (32 steps), green FINE (8), white LEVEL (0..15), orange ENVELOPE shape (8), of that
 *                   operator, written into knob 4 + op. SHIFT green/white/orange ignored.
 * TE's deltas become steps through a per-knob accumulator: FM4_DELTA_SHIFT = log2(delta units per step), 0 until the
 * knob probe reports TE's delta per detent.
 */
#ifndef FM4_EDIT_H
#define FM4_EDIT_H
#include <stdint.h>
#include "fm4.h"

#ifndef FM4_DELTA_SHIFT
#define FM4_DELTA_SHIFT 0
#endif
#define FM4_PASS 0                          /* TE applies the turn */
#define FM4_DONE 1                          /* handled here: give TE delta 0 */

struct fm4_ui {
    uint32_t frame;                         /* screen frames (the screen counts them) */
    int32_t page;                           /* 0 MAIN, 1..4 operator A..D */
    int32_t acc[8];
    int32_t last;                           /* knob of the last turn, -1 none */
    uint32_t last_frame;
    int32_t alg_shown;                      /* the screen: algorithm whose number was last shown, -1 none */
    uint32_t alg_frame;
};

static inline void fm4_ui_reset(struct fm4_ui *u)
{
    uint8_t *p = (uint8_t *)u;
    for (unsigned i = 0; i < sizeof *u; i++)
        p[i] = 0;
    u->last = -1;
    u->alg_shown = -1;
}

static inline int fm4_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline int32_t fm4_steps(struct fm4_ui *u, int knob, int32_t delta)
{
    int32_t a = u->acc[knob] + delta, st = a >= 0 ? a >> FM4_DELTA_SHIFT : -((-a) >> FM4_DELTA_SHIFT);
    u->acc[knob] = a - st * (1 << FM4_DELTA_SHIFT);
    return st;
}

/* v0 knobs 4..7 -> the packed form (no-op if already packed) */
static inline void fm4_to_packed(int16_t *k)
{
    if (fm4_is_packed(k))
        return;
    struct fm4_patch p;
    fm4_decode(k, &p);
    for (int i = 0; i < FM4_OPS; i++)
        k[4 + i] = fm4_pack(p.coarse[i], 0, p.level[i], p.env[i]);
}

static inline int fm4_edit(struct fm4_ui *u, int knob, int32_t delta, int16_t *k)
{
    if ((uint32_t)knob > 7)
        return FM4_PASS;
    int page = fm4_clamp(u->page, 0, FM4_OPS);
    if (knob < 4 && !page)
        return FM4_PASS;
    if (knob > 4 && !page && !fm4_is_packed(k))
        return FM4_PASS;
    int32_t st = fm4_steps(u, knob, delta);
    if (!st)
        return FM4_DONE;
    if (knob == 4) {
        u->page = fm4_clamp(page + st, 0, FM4_OPS);
        if (u->page)
            fm4_to_packed(k);
    } else if (knob < 4) {
        int op = page - 1;
        uint16_t w = (uint16_t)k[4 + op];
        int c = (w >> 10) & 31, f = (w >> 7) & 7, l = (w >> 3) & 15, e = w & 7;
        if (knob == 0)
            c = fm4_clamp(c + st, 0, FM4_COARSES - 1);
        else if (knob == 1)
            f = fm4_clamp(f + st, 0, FM4_FINES - 1);
        else if (knob == 2)
            l = fm4_clamp(l + st, 0, 15);
        else
            e = fm4_clamp(e + st, 0, FM4_ENVS - 1);
        k[4 + op] = fm4_pack(c, f, l, e);
    } else {
        return FM4_DONE;                    /* SHIFT green/white/orange on a packed sound or an operator page */
    }
    u->last = knob;
    u->last_frame = u->frame;
    return FM4_DONE;
}

#endif
