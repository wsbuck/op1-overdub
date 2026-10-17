/* HOLD and CHORD implementation, included by hold.c (device) and test/hold_test.c (host, with stand-ins).
 * See hold.c for the design. */
#ifndef HOLD_HOST_TEST
#define PLAY_MODE (*(volatile int32_t *)0x01DE4C40)         /* 0 synth, 1 drum, 2 game */
#define SLOT (*(volatile int32_t *)0xFF8016D4)
#define OCTAVE(s) (*(volatile int32_t *)(0x01DFE4E0 + (s) * 4))
#define KEY_DOWN ((volatile uint8_t *)0x01AAD488)           /* s_note_ons[24] */
#define VOICECTL_LOCKED (*(volatile uint8_t *)0x01DFE730)   /* note-offs are dropped while locked */
#define SHIFT_DOWN (*(volatile uint8_t *)0x01DD7F78)        /* set/cleared by the global key handler */
#define RAM __attribute__((section(".data")))
#define FEATURES feature_mask                               /* slot.S, set by tools/apply.py */
#else
#define RAM
#endif

#define FEAT_HOLD 1u                   /* SHIFT+T2 toggles hold; off: SHIFT+T2 goes to TE's handler as in stock */
#define FEAT_CHORD 2u                  /* SHIFT + notes learn a chord; off: notes play as stock (no learning) */
/* FEAT_CHORD2 4u (chord2_impl.h): CHORD v2, needs FEAT_CHORD; off: CHORD1 exactly */

#define KEY_T2 7
#define CHORD_MAX 4                    /* notes in a shape (6 voices) */
#define IV_MAX 35                      /* intervals are clamped to +-35 semitones */
#define REL_OFS 36                     /* relative note index = key + interval + REL_OFS: 0 .. 94 */

uint32_t hold_on RAM = 0;
uint32_t hold_keys RAM = 0;            /* bit k: key k released while holding */

/* CHORD state (ui.c reads chord_on, chord_learning, chord_n, chord_iv). */
uint32_t chord_on RAM = 0;
uint32_t chord_learning RAM = 0;
int32_t chord_n RAM = 0;
int32_t chord_iv[CHORD_MAX] RAM = {0, 0, 0, 0};             /* semitones from the root; [0] = 0 */
static int32_t learn_root RAM = 0;                          /* note number of the first learn note */
static int8_t key_n[24] RAM = {0};                          /* per key: shape size it was played with (0 = plain) */
static int8_t key_iv[24][CHORD_MAX] RAM = {{0}};
static uint8_t rel_ref[96] RAM = {0};                       /* sounding chord notes, by relative index */
static int32_t last_mode RAM = 0;

/* One chord note on/off by relative index (key + interval; TE builds the note number from it with the current
 * octave, like its own key-up, so octave changes between on and off still match). Out-of-range notes are skipped:
 * VoiceCtl asserts on notes outside 0..127. */
static void rel_note(int rel, int on)
{
    int32_t note[5];
    te_make_note(note, rel, on ? 0x7F : 0x40);
    if (note[0] < 0 || note[0] > 127)
        return;
    if (on)
        te_note_on(note, 0);
    else
        te_note_off(note);
}

/* Release whatever key `key` is sounding: its chord (reference counted) or the plain note `note` (or rebuilt). */
static void key_release(int key, int32_t *note)
{
    int n = key_n[key];
    if (!n) {
        int32_t built[5];
        if (!note) {
            te_make_note(built, key, 0x40);
            note = built;
        }
        te_note_off(note);
        return;
    }
    key_n[key] = 0;
    for (int i = 0; i < n; i++) {
        int r = key + key_iv[key][i] + REL_OFS;
        if (rel_ref[r] && --rel_ref[r] == 0)
            rel_note(r - REL_OFS, 0);
    }
}

static void chord_reset(void)
{
    for (int k = 0; k < 24; k++)
        key_n[k] = 0;
    for (int r = 0; r < 96; r++)
        rel_ref[r] = 0;
}

static void release_held(void)
{
    if (VOICECTL_LOCKED)
        return;                        /* a preset is loading; keys stay held until hold is toggled again */
    for (int key = 0; key < 24; key++) {
        if (!(hold_keys & (1u << key)) || KEY_DOWN[key])
            continue;
        key_release(key, 0);
    }
    hold_keys = 0;
}

/* The key index of a keyboard note (inverse of FUN_01a85014 in synth mode), or -1. */
static int note_key(const int32_t *note)
{
    int key = note[0] - 0x35 - 12 * OCTAVE(SLOT);
    return key >= 0 && key < 24 ? key : -1;
}

static void c2_reset(void);

/* Play mode switches clear TE's key state without note-offs (FUN_019ac3c8): forget ours too. */
static int synth_mode(void)
{
    int32_t mode = PLAY_MODE;
    if (mode != last_mode) {
        last_mode = mode;
        chord_reset();
        c2_reset();
        hold_keys = 0;
    }
    return mode == 0;
}

/* Chord keys whose key-up can no longer come: TE's key-up returns early when s_note_ons[key] is 0
 * (0x019AC37C), and a play-mode switch clears s_note_ons while a key is held. Release them, or their notes'
 * references would stay counted and later chords sharing those notes would play without them. */
static void purge_stale(void)
{
    for (int k = 0; k < 24; k++)
        if (key_n[k] && !KEY_DOWN[k] && !(hold_keys & (1u << k)))
            key_release(k, 0);
}

#include "chord2_impl.h"

/* Replaces the keyboard's note-on call (FUN_019ac0b0 -> FUN_01a85750(note, 0)). */
void chord_keydown(int32_t *note)
{
    if (C2_ON()) {
        c2_keydown(note);
        return;
    }
    int key = synth_mode() ? note_key(note) : -1;
    if (key < 0) {
        te_note_on(note, 0);
        return;
    }
    purge_stale();
    if (hold_keys & (1u << key)) {     /* a held key pressed again: release it first, so it retriggers */
        hold_keys &= ~(1u << key);
        key_release(key, 0);
    } else if (key_n[key]) {
        key_release(key, 0);           /* key-up never came (e.g. a sequencer took it): don't leak references */
    }
    if (SHIFT_DOWN && (FEATURES & FEAT_CHORD)) {   /* learn: SHIFT + notes; the first is the root */
        if (!chord_learning) {
            chord_learning = 1;
            learn_root = note[0];
            chord_n = 1;
            chord_iv[0] = 0;
        } else if (chord_n < CHORD_MAX) {
            int32_t iv = note[0] - learn_root;
            iv = iv > IV_MAX ? IV_MAX : iv < -IV_MAX ? -IV_MAX : iv;
            int dup = 0;
            for (int i = 0; i < chord_n; i++)
                dup |= chord_iv[i] == iv;
            if (!dup)
                chord_iv[chord_n++] = iv;
        }
        te_note_on(note, 0);           /* learn notes sound as played */
        return;
    }
    if (chord_learning) {              /* the first plain note ends learning; one note = chord mode off */
        chord_learning = 0;
        chord_on = chord_n >= 2;
    }
    if (!chord_on) {
        te_note_on(note, 0);
        return;
    }
    key_n[key] = chord_n;
    for (int i = 0; i < chord_n; i++) {
        key_iv[key][i] = chord_iv[i];
        int r = key + chord_iv[i] + REL_OFS;
        if (rel_ref[r]++ == 0)
            rel_note(r - REL_OFS, 1);
    }
}

/* Replaces the keyboard's note-off call: note = {note, velocity, flag, ...} from FUN_01a85014. */
void hold_keyup(int32_t *note)
{
    if (C2_ON()) {
        c2_keyup(note);
        return;
    }
    int key = synth_mode() ? note_key(note) : -1;
    if (key < 0) {
        te_note_off(note);
        return;
    }
    if (hold_on) {
        hold_keys |= 1u << key;
        return;
    }
    key_release(key, note);
}

/* Synth screen SHIFT+key handler: SHIFT+T2 toggles hold, everything else goes to TE's handler. */
int hold_shiftkey(void *self, int key)
{
    if (C2_ON() && !c2_note_code(key)) {   /* SHIFT + a note key comes here too (before TE plays it): not a combo */
        c2_combo(key);                 /* a SHIFT combo: this SHIFT press no longer learns */
        if (key == KEY_T2 && c2_learn_cancel())
            return 1;                  /* ... and one during a learn cancels the learn, nothing else */
    }
    if (key == KEY_T2 && (FEATURES & FEAT_HOLD)) {
        hold_on = !hold_on;
        if (!hold_on) {
            if (C2_ON())
                c2_release_held();
            else
                release_held();
        }
        return 1;
    }
    if (C2_ON() && c2_shiftkey(key))
        return 1;
    return te_synth_shiftkey(self, key);
}

/* CHORD v2 hooks on the synth controller's press, release and encoder slots: with FEAT_CHORD2 off (or FEAT_CHORD),
 * each is TE's own function, called with the same arguments. */
int c2_press(void *self, int key)
{
    if (C2_ON() && c2_press_key(key))
        return 1;
    return te_synth_press(self, key);
}

int c2_release(void *self, int key)
{
    int r = te_synth_release(self, key);
    if (C2_ON())
        c2_release_key_ev(key);
    return r;
}

int c2_knob(void *self, int knob, int16_t *delta)
{
    if (!C2_ON() || !c2.page)
        return te_synth_knob(self, knob, delta);
    c2.dg.knobs++;
    c2.dg.knob_delta = *delta;
    int32_t steps = hook_steps(&c2.dg.knob_dmin, *delta, 1);
    if (steps)
        c2_knob_turn(knob, steps);
    return 0;
}
