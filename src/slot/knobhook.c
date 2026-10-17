/* Encoder hook for synth engines that store their own data in the 8 knob values (notes/knob-hook.md). Users: the
 * knob probe sprobe (a test engine; src/engine/sprobe.c, packing in sprobe.h), harm (harm_edit.h), fm4 (fm4_edit.h)
 * and va (SHIFT + orange = the sub's shape in knob 7, va.h).
 *
 * TE's synth engine-page encoder handler (mode 0, 0x01A50610) does
 *   CALL FUN_01a1dfec(knob R0, int16 *delta R1, engine id R2, [SP + 12] knobs = 0xFF8013C4 + slot * 16)   0x01A5063E
 *   CALL FUN_01a00d14(patch, knobs)   writes the 8 knobs into the patch's "knobs" (then the patch's +0x34 hook)
 * build_slot.py points the first CALL at entry.inc _knob_hook: other engines go to FUN_01a1dfec untouched; the hooked
 * ids come here (through FX_STEREO_ENTRY, which hands on the 4th argument), with hook_kind[id - 15] (build_slot.py
 * HOOK_KINDS) saying which engine it is. Runs in TE's encoder (UI) thread, as the screens do.
 *   sprobe: knobs 0..5 still go through TE's own function (type 3: each against its own row); knobs 6..7 are ours.
 *           Then the hook rewrites knobs 6..7 with the packed edit count.
 *   harm:   harm_edit() changes the stored knobs; TE gets delta 0 (nothing more to apply), then stores them.
 *   fm4:    fm4_edit(): MAIN-page plain knobs (and v0 SHIFT knobs) pass to TE with TE's delta; page changes and
 *           operator-page edits are ours, TE gets delta 0.
 *   va:     knob 7 (SHIFT + orange) steps the sub's shape one per call, clamped to 0..VA_SUB_SHAPES-1, then TE gets
 *           delta 0 (va's rows are type 0: TE's fine-adjust remap to knob 3 with delta 0 changes nothing). Every other
 *           knob goes to TE untouched: the plain knobs and SHIFT + blue / green / white (TE's fine adjust) as before.
 * In every case TE's FUN_01a00d14 then stores all 8 knobs as it always does.
 * TE's delta per detent is not known yet: hook_steps.h turns it into edit steps (one per call for cursor-like
 * controls), and knobhook_diag records the last raw delta for the screens' corner readout.
 * The knobs are the slot's stored values (0xFF8013C4 + slot * 16). TE's LFO never writes them: all six of its
 * FUN_01a1dfec calls (0x019C9D44.. LFO modules) pass the voice params copy 0xFF801724 (notes/knob-hook.md).
 */
#include <stdint.h>
#include "sprobe.h"
#include "harm_edit.h"
#include "fm4_edit.h"
#include "hook_steps.h"
#include "va.h"

#define RAM __attribute__((section(".data")))

enum { HOOK_SPROBE = 1, HOOK_HARM = 2, HOOK_FM4 = 3, HOOK_VA = 4 };    /* = HOOK_KINDS in build_slot.py */

extern void te_knob_apply(int32_t knob, int16_t *delta, int32_t id, int16_t *knobs);   /* FUN_01a1dfec */
extern int16_t te_knobs[];                 /* 0xFF8013C4: synth knobs, int16[8] per slot */
extern const uint8_t hook_kind[];          /* per engine id 15.. (build_slot.py) */

struct kh_diag knobhook_diag RAM = { 0, -1, 0, 0, 0, -1, 0 };
/* The hooked engines' edit state, shared with their screens (ui.c, ui_harm.h, ui_fm4.h). */
struct harm_ui harm_ui RAM = { .last = -1 };
struct fm4_ui fm4_ui RAM = { .last = -1, .alg_shown = -1 };

/* steps per call at most, per knob (hook_steps.h): 1 where a skipped value would be confusing */
static const uint8_t harm_cap[8] = {
    1,  /* 0 CURSOR */
    4,  /* 1 LEVEL 0..15 */
    1,  /* 2 PAINT: one bar per call */
    4,  /* 3 UNISON 0..63 */
    2,  /* 4 TILT -16..15 */
    1,  /* 5 EXTEND 0..15 */
    1,  /* 6 STAMP */
    4,  /* 7 WARP 0..63 */
};
static const uint8_t fm4_op_cap[4] = { 2, 1, 2, 1 };   /* operator page: COARSE (32), FINE (8), LEVEL (16), ENV (8) */

static void sprobe_turn(int32_t knob, int16_t *delta, int32_t id, int16_t *knobs)
{
    struct kh_diag *d = &knobhook_diag;
    if ((uint32_t)knob < SP_K6)
        te_knob_apply(knob, delta, id, knobs);
    int16_t k6 = sp_k6(sp_edits(knobs[SP_K6]) + 1, knob);
    knobs[SP_K6] = k6;
    knobs[SP_K7] = sp_k7(k6);
    d->w6 = k6;
    d->w7 = knobs[SP_K7];
}

void knobhook_turn(int32_t knob, int16_t *delta, int32_t id, int16_t *knobs)
{
    struct kh_diag *d = &knobhook_diag;
    d->calls++;
    d->knob = knob;
    d->delta = *delta;
    d->slot = (int32_t)(((uintptr_t)knobs - (uintptr_t)te_knobs) >> 4);
    int kind = (uint32_t)(id - 15) < 47 ? hook_kind[id - 15] : 0;
    if ((kind == HOOK_HARM || kind == HOOK_FM4) && (uint32_t)knob > 7)
        kind = 0;                           /* not a knob of ours: TE's call as for any engine */
    if (kind == HOOK_HARM) {
        int32_t st = hook_steps(&d->dmin, *delta, harm_cap[knob]);
        harm_edit(&harm_ui, knob, st, knobs);
        *delta = 0;
        te_knob_apply(knob, delta, id, knobs);
    } else if (kind == HOOK_FM4) {
        int page = fm4_ui.page;
        int32_t cap = knob == 4 ? 1 : knob < 4 && page >= 1 && page <= FM4_OPS ? fm4_op_cap[knob] : 1;
        int32_t st = hook_steps(&d->dmin, *delta, cap);
        if (fm4_edit(&fm4_ui, knob, st, knobs) == FM4_DONE)
            *delta = 0;                     /* else TE applies its own delta to a plain knob, as for any engine */
        te_knob_apply(knob, delta, id, knobs);
    } else if (kind == HOOK_VA && knob == VA_SUB_KNOB) {
        int32_t v = va_sub_shape(knobs[VA_SUB_KNOB]) + hook_steps(&d->dmin, *delta, 1);
        knobs[VA_SUB_KNOB] = (int16_t)(v < 0 ? 0 : v >= VA_SUB_SHAPES ? VA_SUB_SHAPES - 1 : v);
        *delta = 0;
        te_knob_apply(knob, delta, id, knobs);
    } else if (kind == HOOK_SPROBE) {
        if (*delta)
            hook_steps(&d->dmin, *delta, 1);    /* only to record dmin */
        sprobe_turn(knob, delta, id, knobs);
    } else {
        te_knob_apply(knob, delta, id, knobs);
    }
}
