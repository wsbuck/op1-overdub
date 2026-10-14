/* Encoder hook for synth engines that store their own data in the 8 knob values (notes/knob-hook.md). First user: the
 * knob probe sprobe (a test engine; src/engine/sprobe.c, packing in sprobe.h).
 *
 * TE's synth engine-page encoder handler (mode 0, 0x01A50610) does
 *   CALL FUN_01a1dfec(knob R0, int16 *delta R1, engine id R2, [SP + 12] knobs = 0xFF8013C4 + slot * 16)   0x01A5063E
 *   CALL FUN_01a00d14(patch, knobs)   writes the 8 knobs into the patch's "knobs" (then the patch's +0x34 hook)
 * build_slot.py points the first CALL at slot.S _knob_hook: other engines go to FUN_01a1dfec untouched; the probe's id
 * comes here (through FX_STEREO_ENTRY, which hands on the 4th argument). Knobs 0..5 still go through TE's own
 * function (type 3: each against its own row); knobs 6..7 are ours. Then the hook rewrites knobs 6..7 with the
 * packed edit count, and TE's FUN_01a00d14 stores all 8 as it always does. Runs in TE's encoder (UI) thread.
 */
#include <stdint.h>
#include "sprobe.h"

#define RAM __attribute__((section(".data")))

extern void te_knob_apply(int32_t knob, int16_t *delta, int32_t id, int16_t *knobs);   /* FUN_01a1dfec */
extern int16_t te_knobs[];                 /* 0xFF8013C4: synth knobs, int16[8] per slot */

struct kh_diag knobhook_diag RAM = { 0, -1, 0, 0, 0, -1 };

void knobhook_turn(int32_t knob, int16_t *delta, int32_t id, int16_t *knobs)
{
    struct kh_diag *d = &knobhook_diag;
    d->calls++;
    d->knob = knob;
    d->delta = *delta;
    d->slot = (int32_t)(((uintptr_t)knobs - (uintptr_t)te_knobs) >> 4);
    if ((uint32_t)knob < SP_K6)
        te_knob_apply(knob, delta, id, knobs);
    int16_t k6 = sp_k6(sp_edits(knobs[SP_K6]) + 1, knob);
    knobs[SP_K6] = k6;
    knobs[SP_K7] = sp_k7(k6);
    d->w6 = k6;
    d->w7 = knobs[SP_K7];
}
