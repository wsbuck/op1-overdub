/* HOLD: keyboard notes keep sounding after the keys are released, until hold is turned off.
 * Toggle: SHIFT+T2 (key 7) on the synth screen. Synth play mode only; sequencers and MIDI are untouched.
 * Design and addresses: notes/hold.md (fw 246).
 *
 * CHORD (build_slot.py --chord, needs --hold): hold SHIFT and play up to 4 notes to learn a shape (the first is
 * the root); the first plain note ends learning and from then on every key plays the shape on that key. Learning a
 * single note turns chord mode off. Chord notes are reference counted by relative index (key + interval), so
 * overlapping chords don't cut shared notes. Hook: the keyboard note-on CALL at 0x019AC290 (FUN_019ac0b0 ->
 * FUN_01a85750) calls chord_keydown. Design: notes/seq-chord.md.
 *
 * Hooks (installed by tools/build_slot.py --hold):
 *   - the keyboard key-up CALL at 0x019AC3A6 (FUN_019ac2f8 -> FUN_01a85840 note-off) calls hold_keyup;
 *   - the synth controller's shift-key slot (vtable 0x01911994 +0x1C, pointer 0x019119B0 -> 0x01A50904)
 *     points at hold_shiftkey.
 * Held notes are kept as physical key indices (0..23) and released by rebuilding the note the way TE's
 * own key-up does (FUN_01a85014(&n, key, 0x40)), so octave and slot changes, which transpose the active
 * notes, are handled the same as stock. Both hooks run in the KeyHandler thread.
 */
#include <stdint.h>

extern void te_note_off(int32_t *note);                     /* FUN_01a85840 (VoiceCtl.cpp:814) */
extern void te_note_on(int32_t *note, int flags);           /* FUN_01a85750 (VoiceCtl.cpp:776) */
extern void te_make_note(int32_t *note, int key, int vel);  /* FUN_01a85014 */
extern int te_synth_shiftkey(void *self, int key);          /* FUN_01a50904 */

void hold_keyup(int32_t *note);
void chord_keydown(int32_t *note);
int hold_shiftkey(void *self, int key);

#include "hold_impl.h"
