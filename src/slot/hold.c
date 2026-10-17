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
 * CHORD v2 (FEAT_CHORD2, notes/chord2.md; chord2_impl.h): per-slot modes OFF / SHAPE / MAP 1..8, the CHORD page
 * (SHIFT+SPLIT), learn, undo, the voice budget, HOLD chords-only, the file. Extra hooks: the synth controller's press,
 * release and encoder slots (c2_press, c2_release, c2_knob) and the frame hook (ui.c c2_frame -> c2_gui_tick).
 * Each hook checks the feature mask (slot.S _feature_mask, hold_impl.h FEAT_*): a hook left out at install time
 * behaves as stock.
 * Held notes are kept as physical key indices (0..23) and released by rebuilding the note the way TE's
 * own key-up does (FUN_01a85014(&n, key, 0x40)), so octave and slot changes, which transpose the active
 * notes, are handled the same as stock. Both hooks run in the KeyHandler thread.
 */
#include <stdint.h>

extern void te_note_off(int32_t *note);                     /* FUN_01a85840 (VoiceCtl.cpp:814) */
extern void te_note_on(int32_t *note, int flags);           /* FUN_01a85750 (VoiceCtl.cpp:776) */
extern void te_make_note(int32_t *note, int key, int vel);  /* FUN_01a85014 */
extern int te_synth_shiftkey(void *self, int key);          /* FUN_01a50904 */
extern int te_synth_press(void *self, int key);             /* FUN_01a51058: synth controller press slot +0x14 */
extern int te_synth_release(void *self, int key);           /* FUN_01a5056c: release slot +0x24 (returns 0) */
extern int te_synth_knob(void *self, int knob, int16_t *delta);   /* FUN_01a505bc: encoder slot +0x2C */
extern int te_yaffs_open(const char *path, int flags, int mode);  /* FUN_01a997f4: fd, -1 on errors */
extern int te_yaffs_read(int fd, void *buf, unsigned n);    /* FUN_01a99c90: bytes read, -1 on errors */
extern int te_yaffs_write(int fd, const void *buf, unsigned n);   /* FUN_01a99f84: bytes written */
extern int te_yaffs_close(int fd);                          /* FUN_01a99908: 0 */
extern int te_yaffs_mkdir(const char *path, int mode);      /* FUN_01a9a886: 0, -1 if it exists */
extern const volatile uint32_t feature_mask;                /* slot.S: FEAT_HOLD | FEAT_CHORD | FEAT_CHORD2 (apply.py) */

void hold_keyup(int32_t *note);
void chord_keydown(int32_t *note);
int hold_shiftkey(void *self, int key);
int c2_press(void *self, int key);
int c2_release(void *self, int key);
int c2_knob(void *self, int knob, int16_t *delta);
void c2_gui_tick(void);

#include "hold_impl.h"
