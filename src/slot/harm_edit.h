/* Harmonic engine: what an encoder turn does (notes/harmonic-synth.md "Controls"). A pure function of the UI state,
 * the turn and the sound's 8 knob values; the encoder hook (knobhook.c, once confirmed on device) calls it for the
 * harm engine's id, then hands TE delta 0 so that TE's own code stores the knobs in the patch:
 *
 *   harm_edit(&harm_ui, knob, *delta, knobs);  *delta = 0;  te_knob_apply(knob, delta, id, knobs);
 *
 * knob = TE's knob index with type-3 rows: 0..3 plain blue/green/white/orange, 4..7 the same with SHIFT.
 *   0 blue    CURSOR 1..24 (UI state only, not saved)
 *   1 green   LEVEL of the cursor harmonic, 0 (off) .. 15
 *   2 white   PAINT: moves the cursor one bar per step and writes the level it started on (the brush) into every bar
 *             it enters; bars it wrote light up white on screen for a moment
 *   3 orange  UNISON 0..63
 *   4 SHIFT blue    TILT -16..15 (0.75 dB/octave per step, at build time; the drawing is unchanged)
 *   5 SHIFT green   EXTEND 0..15 (repeat bars 13..24 above harmonic 24, 0 = off)
 *   6 SHIFT white   STAMP: steps through SAW, SQUARE, TRIANGLE, EVEN, ORGAN, CLEAR, each replacing the drawing; back to
 *                   0 restores the drawing from before the first stamp (one undo, RAM). A LEVEL or PAINT edit keeps the
 *                   stamped drawing (the undo is dropped).
 *   7 SHIFT orange  WARP 0..63
 * TE's deltas become steps through a per-knob accumulator: HARM_DELTA_SHIFT = log2(delta units per step), 0 until the
 * knob probe reports what TE's delta is per detent (and whether its acceleration reaches the hook).
 * Returns 1 if the knob values changed.
 */
#ifndef HARM_EDIT_H
#define HARM_EDIT_H
#include <stdint.h>
#include "harm.h"

#ifndef HARM_DELTA_SHIFT
#define HARM_DELTA_SHIFT 0
#endif
#define HARM_STAMPS 6

static const char *const harm_stamp_name[HARM_STAMPS + 1] = { "OWN", "SAW", "SQUARE", "TRIANGLE", "EVEN", "ORGAN",
                                                              "CLEAR" };
/* levels: 1/n (saw), odd 1/n (square), odd 1/n^2 (triangle), 1 + even 2/n, drawbars 16' 8' 4' (H1 H2 H4), none */
static const uint8_t harm_stamp_lv[HARM_STAMPS][HARM_N] = {
    { 15, 13, 12, 11, 10, 10, 9, 9, 9, 8, 8, 8, 8, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6 },
    { 15, 0, 12, 0, 10, 0, 9, 0, 9, 0, 8, 0, 8, 0, 7, 0, 7, 0, 6, 0, 6, 0, 6, 0 },
    { 15, 0, 9, 0, 6, 0, 4, 0, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 15, 15, 0, 13, 0, 12, 0, 11, 0, 10, 0, 10, 0, 9, 0, 9, 0, 9, 0, 8, 0, 8, 0, 8 },
    { 15, 15, 0, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0 },
};

struct harm_ui {
    uint32_t frame;                         /* screen frames (the screen counts them; the hook reads them) */
    int32_t cursor;                         /* 0..23 */
    int32_t acc[8];                         /* TE delta not yet turned into a step, per knob */
    uint32_t painted[HARM_N];               /* frame + 1 of the last PAINT write per bar, 0 = never */
    int32_t stamp;                          /* 0: the user's drawing; 1..HARM_STAMPS: that stamp, undo holds the drawing */
    int16_t undo[6];                        /* knobs 2..7 before the first stamp */
    int32_t last;                           /* knob of the last turn that did something, -1 none */
    uint32_t last_frame;                    /* its frame */
    uint32_t build_frame;                   /* the screen: frame of the last table build (ui_harm.h harm_tick) */
};

static inline int harm_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline int32_t harm_steps(struct harm_ui *u, int knob, int32_t delta)
{
    int32_t a = u->acc[knob] + delta, st = a >= 0 ? a >> HARM_DELTA_SHIFT : -((-a) >> HARM_DELTA_SHIFT);
    u->acc[knob] = a - st * (1 << HARM_DELTA_SHIFT);
    return st;
}

static inline void harm_ui_reset(struct harm_ui *u)
{
    uint8_t *p = (uint8_t *)u;
    for (unsigned i = 0; i < sizeof *u; i++)
        p[i] = 0;
    u->last = -1;
}

static inline int harm_edit(struct harm_ui *u, int knob, int32_t delta, int16_t *k)
{
    if ((uint32_t)knob > 7)
        return 0;
    int32_t st = harm_steps(u, knob, delta);
    if (!st)
        return 0;
    int16_t old[8];
    for (int i = 0; i < 8; i++)
        old[i] = k[i];
    int uni = harm_uni(k), warp = harm_warp(k), tilt = harm_tilt(k), ext = harm_extend(k);
    u->cursor = harm_clamp(u->cursor, 0, HARM_N - 1);
    switch (knob) {
    case 0:
        u->cursor = harm_clamp(u->cursor + st, 0, HARM_N - 1);
        break;
    case 1:
        u->stamp = 0;
        harm_set_level(k, u->cursor, harm_clamp(harm_level(k, u->cursor) + st, 0, HARM_MAXLEVEL));
        break;
    case 2: {
        int brush = harm_level(k, u->cursor), dir = st > 0 ? 1 : -1;
        u->stamp = 0;
        for (int n = st > 0 ? st : -st; n > 0 && u->cursor + dir >= 0 && u->cursor + dir < HARM_N; n--) {
            u->cursor += dir;
            harm_set_level(k, u->cursor, brush);
            u->painted[u->cursor] = u->frame + 1;
        }
        break;
    }
    case 3:
        uni = harm_clamp(uni + st, 0, 63);
        break;
    case 4:
        tilt = harm_clamp(tilt + st, -16, 15);
        break;
    case 5:
        ext = harm_clamp(ext + st, 0, 15);
        break;
    case 6: {
        int s = harm_clamp(u->stamp + st, 0, HARM_STAMPS);
        if (s == u->stamp)
            break;
        if (!u->stamp)
            for (int i = 0; i < 6; i++)
                u->undo[i] = k[2 + i];
        if (!s)
            for (int i = 0; i < 6; i++)
                k[2 + i] = u->undo[i];
        else
            for (int i = 0; i < HARM_N; i++)
                harm_set_level(k, i, harm_stamp_lv[s - 1][i]);
        u->stamp = s;
        break;
    }
    default:
        warp = harm_clamp(warp + st, 0, 63);
        break;
    }
    k[0] = harm_k0(uni, warp);
    k[1] = harm_k1(tilt, ext);
    u->last = knob;
    u->last_frame = u->frame;
    for (int i = 0; i < 8; i++)
        if (k[i] != old[i])
            return 1;
    return 0;
}

#endif
