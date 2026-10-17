/* CHORD v2 engine (notes/chord2.md), included by hold_impl.h after CHORD1's code (device: hold.c; host: chord2_test.c
 * with stand-ins). Feature bit FEAT_CHORD2 (with FEAT_CHORD): off = CHORD1 exactly, and every new hook passes
 * through to TE.
 *
 * Threads: everything here runs in TE's KeyHandler thread (note keys, the synth controller's press / release /
 * SHIFT-key / encoder slots: FUN_019b6c74 and FUN_019b6c0c dispatch them from the same loop), except c2_gui_tick
 * (the file, from the frame hook in the GUI thread, ui.c). The GUI writes `d` only while loading the file once
 * (`loading` set: the keys play plain notes meanwhile); every other write of `d` is ours, here.
 *
 * Hooks (build_slot.py --chord; addresses notes/chord2.md "MVP as built"):
 *   c2_press    synth controller press slot +0x14 (0x019119A8 -> 0x01A51058): SHIFT ends the edit freeze; with the page
 *               open SPLIT / LIFT / DROP are ours, T1..T4 close the page and go on to TE
 *   c2_release  release slot +0x24 (0x019119B8 -> 0x01A5056C, which always returns 0): SPLIT up, SHIFT up = learn ends
 *   c2_knob     encoder slot +0x2C (0x019119C0 -> 0x01A505BC): page open = ours (knob 0..3, SHIFT = 4..7), else TE's
 *   hold_shiftkey (shift slot +0x1C, CHORD1's): SHIFT+SPLIT opens / closes the page; on the page SHIFT+LIFT = undo
 *   chord_keydown / hold_keyup (CHORD1's note-on / note-off CALLs): maps, the voice budget, HOLD chords-only, learn.
 */
#include "chord2.h"
#include "hook_steps.h"

#define FEAT_CHORD2 4u                 /* CHORD v2: maps, the page; off: CHORD1 */
#define C2_ON() ((FEATURES & (FEAT_CHORD | FEAT_CHORD2)) == (FEAT_CHORD | FEAT_CHORD2))

#define KEY_SHIFT 0x26
#define KEY_T1 6
#define KEY_T4 9
#define KEY_LIFT 0xA
#define KEY_DROP 0xB
#define KEY_SPLIT 0xC

/* ---- the state, with the factory maps (static, so they are there from the first key event) ---- */
#define FC(k, r, qq) [k] = { (r), (qq), 0, 0, { 0 }, 0, 0, 0, 0 }
#define FT(k, r, qq, t) [k] = { (r), (qq), 0, (t), { 0 }, 0, 0, 0, 0 }
#define SH { 0, Q_MAJ, 0, 0, { 0 }, 0, 0, 0, 0 }
struct c2_state c2 RAM = {
    .d = {
        .maps = {
            /* 1 DIATONIC C: C major sevenths on the white keys, black keys plain */
            [0] = {
                FC(0, 0, Q_MAJ7), FC(2, 0, Q_7), FC(4, 0, Q_MIN7), FC(6, 0, Q_HDIM), FC(7, 0, Q_MAJ7),
                FC(9, 0, Q_MIN7), FC(11, 0, Q_MIN7), FC(12, 0, Q_MAJ7), FC(14, 0, Q_7), FC(16, 0, Q_MIN7),
                FC(18, 0, Q_HDIM), FC(19, 0, Q_MAJ7), FC(21, 0, Q_MIN7), FC(23, 0, Q_MIN7),
            },
            /* 2 II-V-I in F, Bb, Eb, C on groups of three white keys; D D# E = the minor ii-V-i to C minor. TOPs set so
             * each group voice-leads (6 semitones of movement over the two changes; SMOOTH is phase 2) */
            [1] = {
                FT(0, 2, Q_MIN7, 2), FT(2, 5, Q_7, 0), FT(4, -4, Q_MAJ7, 2),
                FT(6, 1, Q_MIN7, 0), FT(7, 5, Q_7, -2), FT(9, -4, Q_MAJ7, 0),
                FT(11, 1, Q_MIN7, -1), FT(12, 5, Q_7, -3), FT(14, -4, Q_MAJ7, -1),
                FT(16, 5, Q_MIN7, -4), FT(18, -4, Q_7, -2), FT(19, 0, Q_MAJ7, -4),
                FT(21, 0, Q_HDIM, -4), FT(22, 4, Q_7B9, -7), FT(23, -4, Q_MIN6, -3),
            },
            /* 3 NEO SOUL: m9 / maj9 / 13 / 7#9 colours around Eb major / C minor on the lower octave (flat roots on the
             * black keys); the upper octave plays plain notes for a melody over a held pad */
            [2] = {
                FC(0, 0, Q_MIN9), FC(2, 0, Q_7S9), FC(3, 0, Q_MAJ9), FC(5, 0, Q_13), FC(7, 0, Q_MIN9),
                FC(8, 0, Q_MAJ9), FC(9, 0, Q_MIN7), FC(10, 0, Q_MAJ9), FC(11, 0, Q_HDIM),
            },
            /* 4 POP: C major triads on the white keys, borrowed chords on the black ones */
            [3] = {
                FC(0, 0, Q_MAJ), FC(2, 0, Q_MAJ), FC(4, 0, Q_MIN), FC(6, 0, Q_DIM), FC(7, 0, Q_MAJ), FC(9, 0, Q_MIN),
                FC(11, 0, Q_MIN), FC(12, 0, Q_MAJ), FC(14, 0, Q_MAJ), FC(16, 0, Q_MIN), FC(18, 0, Q_DIM),
                FC(19, 0, Q_MAJ), FC(21, 0, Q_MIN), FC(23, 0, Q_MIN), FC(1, -1, Q_MIN), FC(3, 0, Q_MAJ),
                FC(5, 0, Q_MAJ), FC(8, 1, Q_MAJ), FC(10, 0, Q_MAJ), FC(13, -1, Q_MIN), FC(15, 0, Q_MAJ),
                FC(17, 0, Q_MAJ), FC(20, 1, Q_MAJ), FC(22, 0, Q_MAJ),
            },
        },
        .set = { { 0, 0, 0, 0, "DIATONIC C" }, { 0, 1, 1, 0, "II-V-I" }, { 1, 1, 1, 0, "NEO SOUL" }, { 0, 0, 1, 0, "POP" } },
        .shape = { SH, SH, SH, SH, SH, SH, SH, SH },
    },
    .sel = 7, .turned = -1, .last_key = -1, .file_slot = -1,
};
#undef FC
#undef FT
#undef SH

/* ---- small helpers (struct copies go through c2_copy: GCC would call memcpy, which the slot block lacks) ---- */
static uint32_t c2_now(void) { return c2.clock; }

static int c2_slot(void)
{
    int32_t s = SLOT;
    return s >= 0 && s < C2_SLOTS ? (int)s : -1;
}

static int c2_mode(void)
{
    int s = c2_slot();
    if (s < 0 || c2.loading)
        return 0;
    int m = c2.d.mode[s];
    return m < C2_MODE_COUNT ? m : 0;
}

static void c2_copy(void *dst, const void *src, int n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

static void c2_flash(int msg, int arg)
{
    c2.msg = (uint32_t)msg;
    c2.msg_arg = arg;
    c2.msg_frame = c2_now();
}

static void c2_touch(void)
{
    c2.gen++;
    c2.edit_frame = c2_now();
}

/* Undo: the whole state before an edit gesture (knob turns within C2_FREEZE frames are one gesture). */
static void c2_snapshot(int knob)
{
    c2.gen++;                          /* an edit starts: a save running now is retried (and waits 2 s) */
    c2.edit_frame = c2_now();
    if (knob && c2.gesture_knob && c2_now() - c2.knob_frame <= C2_FREEZE)
        return;
    c2_copy(&c2.undo, &c2.d, sizeof c2.d);
    c2.undo_ok = 1;
    c2.redo = 0;
    c2.gesture_knob = (uint32_t)knob;
}

static void c2_undo(void)
{
    c2.gen++;
    c2.edit_frame = c2_now();
    if (!c2.undo_ok) {
        c2_flash(MSG_NOTHING_UNDO, 0);
        return;
    }
    uint8_t *a = (uint8_t *)&c2.d, *b = (uint8_t *)&c2.undo;
    for (unsigned i = 0; i < sizeof c2.d; i++) {
        uint8_t t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
    c2.gesture_knob = 0;
    c2_touch();
    c2_flash(c2.redo ? MSG_REDONE : MSG_UNDONE, 0);
    c2.redo = !c2.redo;
}

/* The chord the page edits: the edited key's in MAP mode, the slot's shape in SHAPE mode; 0 when OFF. */
static struct c2_chord *c2_target(void)
{
    int mode = c2_mode(), s = c2_slot(), m = c2_mode_map(mode);
    if (m >= 0 && c2.sel >= 0 && c2.sel < C2_KEYS)
        return &c2.d.maps[m][c2.sel];
    if (mode == 1 && s >= 0)
        return &c2.d.shape[s];
    return 0;
}

static int c2_frozen(void) { return c2.pinned || (c2.knob_frame && c2_now() - c2.knob_frame <= C2_FREEZE); }

/* ---- playing: our own voice tracking, by key ---- */

static int c2_voice_count(void)
{
    int n = 0;
    for (int r = 0; r < 96; r++)
        n += c2.rel_ref[r] != 0;
    return n;
}

static void c2_release_key(int key)
{
    int n = c2.key_cnt[key];
    for (int i = 0; i < n; i++) {
        int r = c2.key_rel[key][i];
        if (r < 96 && c2.rel_ref[r] && --c2.rel_ref[r] == 0)
            rel_note(r - REL_OFS, 0);
    }
    c2.key_cnt[key] = 0;
    c2.key_kind[key] = 0;
    c2.key_bass[key] = -1;
    c2.sounding &= ~(1u << key);
    c2.dg.voices = c2_voice_count();
}

static void c2_reset(void)
{
    for (int k = 0; k < C2_KEYS; k++) {
        c2.key_cnt[k] = 0;
        c2.key_kind[k] = 0;
        c2.key_bass[k] = -1;
    }
    for (int r = 0; r < 96; r++)
        c2.rel_ref[r] = 0;
    c2.dead = c2.chord_down = c2.sounding = 0;
    c2.learning = 0;
    c2.split_down = 0;
}

/* The relative notes key `key` plays now (key + interval + REL_OFS, deduplicated, ascending, inside the note range);
 * *chord = 1 for a chord, 0 for its plain note; *bass = index of the BASS note or -1. */
static int c2_key_notes(int key, uint8_t *rel, int *chord, int *bass)
{
    int mode = c2_mode(), s = c2_slot(), m = c2_mode_map(mode);
    const struct c2_chord *c = m >= 0 ? &c2.d.maps[m][key] : mode == 1 && s >= 0 ? &c2.d.shape[s] : 0;
    int8_t iv[C2_NOTES];
    int b = -1, n = c ? c2_notes(c, m >= 0 && c2.d.set[m].bass, iv, &b) : 0;
    *chord = n > 0;
    if (!n) {
        iv[0] = 0;
        n = 1;
        b = -1;
    }
    int out = 0;
    *bass = -1;
    for (int i = 0; i < n; i++) {
        int r = key + iv[i] + REL_OFS, dup = 0;
        if (r < 0 || r >= 96)
            continue;
        int32_t note[5];
        te_make_note(note, r - REL_OFS, 0x7F);
        if (note[0] < 12 || note[0] > 115)
            continue;                  /* an octave key can move a sounding note by 12: keep it inside 0..127 */
        for (int j = 0; j < out; j++)
            dup |= rel[j] == r;
        if (dup)
            continue;
        if (i == b)
            *bass = out;
        rel[out++] = (uint8_t)r;
    }
    return out;
}

/* Voices sounding if `victims` were released and `rel` added. */
static int c2_count_after(uint32_t victims, const uint8_t *rel, int n)
{
    uint8_t ref[96];
    for (int r = 0; r < 96; r++)
        ref[r] = c2.rel_ref[r];
    for (int k = 0; k < C2_KEYS; k++)
        if (victims >> k & 1)
            for (int i = 0; i < c2.key_cnt[k]; i++)
                if (ref[c2.key_rel[k][i]])
                    ref[c2.key_rel[k][i]]--;
    for (int i = 0; i < n; i++)
        ref[rel[i]]++;
    int c = 0;
    for (int r = 0; r < 96; r++)
        c += ref[r] != 0;
    return c;
}

/* Drop one key's BASS note (the first thing the budget sheds). */
static void c2_shed_bass(int k)
{
    int b = c2.key_bass[k], n = c2.key_cnt[k];
    if (b < 0 || b >= n)
        return;
    int r = c2.key_rel[k][b];
    if (c2.rel_ref[r] && --c2.rel_ref[r] == 0)
        rel_note(r - REL_OFS, 0);
    for (int i = b; i < n - 1; i++)
        c2.key_rel[k][i] = c2.key_rel[k][i + 1];
    c2.key_cnt[k] = (uint8_t)(n - 1);
    c2.key_bass[k] = -1;
    c2.dg.sheds++;
}

/* The voice budget: the keys to let go (whole) so that `rel` fits in C2_VOICES with what sounds; BASS notes are
 * shed on the spot (first). Order: the oldest BASS voice, the oldest plain note, the oldest whole chord. */
static uint32_t c2_budget(int key, const uint8_t *rel, int n, uint32_t victims, int chord)
{
    /* a chord sheds the oldest BASS note first; a plain (melody) note sheds the oldest plain note before the BASS */
    static const uint8_t order_chord[3] = { 0, 1, 2 }, order_plain[3] = { 1, 0, 2 };
    const uint8_t *order = chord ? order_chord : order_plain;
    for (int guard = 0; guard < 2 * C2_KEYS && c2_count_after(victims, rel, n) > C2_VOICES; guard++) {
        int pick = -1, kind = 0;
        uint32_t best = 0;
        for (int i = 0; i < 3 && pick < 0; i++) {
            int pass = order[i];
            for (int k = 0; k < C2_KEYS; k++) {
                if (k == key || (victims >> k & 1) || !c2.key_cnt[k])
                    continue;
                int ok = pass == 0 ? c2.key_bass[k] >= 0 : pass == 1 ? c2.key_kind[k] == 1 : c2.key_kind[k] == 2;
                if (ok && (pick < 0 || c2.key_age[k] - best > 0x80000000u)) {
                    pick = k;
                    best = c2.key_age[k];
                    kind = pass;
                }
            }
        }
        if (pick < 0)
            break;
        if (kind == 0)
            c2_shed_bass(pick);
        else {
            victims |= 1u << pick;
            c2.dg.sheds++;
        }
    }
    return victims;
}

static void c2_let_go(uint32_t victims)
{
    for (int k = 0; k < C2_KEYS; k++)
        if (victims >> k & 1) {
            if (KEY_DOWN[k])
                c2.dead |= 1u << k;                /* its key-up must not release anything */
            hold_keys &= ~(1u << k);
            c2_release_key(k);
        }
}

static void c2_track(int key, const uint8_t *rel, int n, int chord, int bass)
{
    for (int i = 0; i < n && i < C2_NOTES; i++)
        c2.key_rel[key][i] = rel[i];
    c2.key_cnt[key] = (uint8_t)(n < C2_NOTES ? n : C2_NOTES);
    c2.key_kind[key] = (uint8_t)(chord ? 2 : 1);
    c2.key_bass[key] = (int8_t)bass;
    if (chord) {
        c2.sounding |= 1u << key;
        c2.last_key = key;
        c2.last_slot = c2_slot();
        c2.last_frame = c2_now();
    } else
        c2.sounding &= ~(1u << key);
}

/* Play `key` as rel[0..n): the voice budget first (whole chords, never half chords), and a HOLD gesture replaces the
 * latched chords. Order: references up for the new notes, the victims released (their notes off unless the new
 * chord shares them: common tones are not retriggered and never stolen), then the new notes on. */
static void c2_play(int key, const uint8_t *rel, int n, int chord, int bass)
{
    uint32_t bit = 1u << key, victims = 0;
    if (chord && hold_on && !(c2.chord_down & ~bit))
        victims |= hold_keys;                      /* a new HOLD gesture replaces the latched chords */
    victims = c2_budget(key, rel, n, victims & ~bit, chord);
    uint8_t need[C2_NOTES];
    int nn = 0;
    for (int i = 0; i < n && i < C2_NOTES; i++)
        if (c2.rel_ref[rel[i]]++ == 0)
            need[nn++] = rel[i];
    c2_track(key, rel, n, chord, bass);
    c2.key_age[key] = ++c2.age;
    c2_let_go(victims);
    for (int i = 0; i < nn; i++)
        rel_note(need[i] - REL_OFS, 1);
    c2.dg.voices = c2_voice_count();
}

/* A sounding key whose chord was edited (live audition): the notes it keeps are not retriggered, the ones it loses go
 * off, the new ones on, inside the voice budget. */
static void c2_revoice(int key)
{
    if (!c2.key_cnt[key] || !(KEY_DOWN[key] || (hold_keys >> key & 1)))
        return;
    uint8_t rel[C2_NOTES], old[C2_NOTES];
    int chord, bass, n = c2_key_notes(key, rel, &chord, &bass), on = c2.key_cnt[key];
    for (int i = 0; i < on; i++) {
        old[i] = c2.key_rel[key][i];
        if (c2.rel_ref[old[i]])
            c2.rel_ref[old[i]]--;                  /* off below, unless kept */
    }
    c2.key_cnt[key] = 0;
    uint8_t need[C2_NOTES];
    int nn = 0;
    for (int j = 0; j < n; j++) {             /* our references first: a victim can't turn off what we keep */
        int had = 0;
        for (int i = 0; i < on; i++)
            had |= rel[j] == old[i];
        if (c2.rel_ref[rel[j]]++ == 0 && !had)
            need[nn++] = rel[j];
    }
    for (int i = 0; i < on; i++)       /* before the budget's victims: a shared note goes off exactly once */
        if (!c2.rel_ref[old[i]])
            rel_note(old[i] - REL_OFS, 0);
    c2_let_go(c2_budget(key, rel, 0, 0, chord));
    c2_track(key, rel, n, chord, bass);
    for (int i = 0; i < nn; i++)
        rel_note(need[i] - REL_OFS, 1);
    c2.dg.voices = c2_voice_count();
}

static void c2_revoice_all(void)
{
    for (int k = 0; k < C2_KEYS; k++)
        if (c2.key_kind[k])
            c2_revoice(k);
}

static void c2_release_held(void)
{
    if (VOICECTL_LOCKED)
        return;                        /* a preset is loading; keys stay held until hold is toggled again */
    for (int key = 0; key < C2_KEYS; key++)
        if ((hold_keys >> key & 1) && !KEY_DOWN[key])
            c2_release_key(key);
    hold_keys = 0;
}

/* Tracked keys whose key-up can no longer come (a play-mode switch cleared s_note_ons): release them. */
static void c2_purge_stale(void)
{
    for (int k = 0; k < C2_KEYS; k++)
    {
        if (c2.key_cnt[k] && !KEY_DOWN[k] && !(hold_keys >> k & 1))
            c2_release_key(k);
        if (!KEY_DOWN[k])
            c2.chord_down &= ~(1u << k);
    }
}

/* ---- learn: SHIFT + notes; written when SHIFT is let go ---- */

static void c2_learn_note(int key, int32_t note)
{
    if (!c2.learning) {
        c2.learning = 1;
        c2.learn_frame = c2_now();
        c2.learn_n = 0;
        c2.learn_over = 0;
        c2.learn_page = (int32_t)c2.page;
        int32_t t[5];
        if (c2.page && c2.sel >= 0 && c2.sel < C2_KEYS) {
            te_make_note(t, c2.sel, 0x7F);
            c2.learn_base = t[0];
        } else
            c2.learn_base = note;      /* off the page (CHORD1's gesture): the first note is the root */
    }
    for (int i = 0; i < c2.learn_n; i++)
        if (c2.learn_note[i] == note)
            return;
    if (c2.learn_n < C2_LEARN_MAX) {
        c2.learn_note[c2.learn_n] = note;
        c2.learn_key[c2.learn_n] = key;
        c2.learn_n++;
    } else
        c2.learn_over = 1;
}

/* The learned chord's intervals from the target key: the notes still held, or all if none; deduplicated, ascending,
 * the lowest C2_MAX kept. Returns the count; *over = more than C2_MAX distinct notes. */
static int c2_learn_take(int8_t *iv, int *over)
{
    int held = 0, n = 0;
    int8_t v[C2_LEARN_MAX];
    for (int i = 0; i < c2.learn_n; i++)
        held |= KEY_DOWN[c2.learn_key[i]] != 0;
    for (int i = 0; i < c2.learn_n; i++) {
        if (held && !KEY_DOWN[c2.learn_key[i]])
            continue;
        int32_t d = c2.learn_note[i] - c2.learn_base;
        d = d > C2_IV_MAX ? C2_IV_MAX : d < -C2_IV_MAX ? -C2_IV_MAX : d;
        int dup = 0;
        for (int j = 0; j < n; j++)
            dup |= v[j] == d;
        if (!dup)
            v[n++] = (int8_t)d;
    }
    c2_sort(v, n);
    *over = n > C2_MAX || c2.learn_over;
    if (n > C2_MAX) {                  /* the bass and the top 3: the melody note is kept, inner notes go */
        iv[0] = v[0];
        for (int i = 1; i < C2_MAX; i++)
            iv[i] = v[n - C2_MAX + i];
        return C2_MAX;
    }
    for (int i = 0; i < n; i++)
        iv[i] = v[i];
    return n;
}

static void c2_learn_commit(void)
{
    if (!c2.learning || c2.loading)
        return;
    c2.learning = 0;
    int8_t iv[C2_MAX];
    int over, n = c2_learn_take(iv, &over), s = c2_slot();
    if (!n || s < 0)
        return;
    c2_snapshot(0);
    struct c2_chord nc;
    c2_clear(&nc);
    nc.q = Q_CUSTOM;
    nc.n = (uint8_t)n;
    for (int i = 0; i < n; i++)
        nc.iv[i] = iv[i];
    if (!c2.learn_page) {              /* CHORD1's gesture: >= 2 notes = SHAPE, 1 note = OFF */
        if (n == 1)
            c2.d.mode[s] = 0;
        else {
            c2_copy(&c2.d.shape[s], &nc, sizeof nc);
            c2.d.mode[s] = 1;
        }
    } else {
        int m = c2_mode_map(c2_mode());
        if (m >= 0 && c2.sel >= 0 && c2.sel < C2_KEYS)
            c2_copy(&c2.d.maps[m][c2.sel], &nc, sizeof nc);
        else {
            c2_copy(&c2.d.shape[s], &nc, sizeof nc);
            c2.d.mode[s] = 1;
        }
    }
    c2_touch();
    c2_flash(over ? MSG_MAX4 : MSG_LEARNED, n);
}

/* ---- the note hooks ---- */

static void c2_keydown(int32_t *note)
{
    int key = synth_mode() ? note_key(note) : -1;
    if (key < 0) {
        te_note_on(note, 0);
        return;
    }
    uint32_t bit = 1u << key;
    c2_purge_stale();
    if (hold_keys & bit) {             /* a latched key pressed again: release it first, so it retriggers */
        hold_keys &= ~bit;
        c2_release_key(key);
    } else if (c2.key_cnt[key])
        c2_release_key(key);           /* its key-up never came */
    c2.dead &= ~bit;
    if (c2.page && c2.split_down) {    /* SPLIT + key: clear that key's chord, play nothing */
        c2.sel = key;
        struct c2_chord *t = c2.loading ? 0 : c2_target();
        if (t) {
            c2_snapshot(0);
            c2_clear(t);
            c2_touch();
            c2_revoice_all();
        }
        c2_flash(MSG_CLEARED, key);
        c2.split_used = 1;
        c2.dead |= bit;
        return;
    }
    uint8_t rel[C2_NOTES];
    int chord = 0, bass = -1, n;
    if (!SHIFT_DOWN)
        c2.shift_used = 0;             /* also without the press hook: a plain key ends any SHIFT combo */
    int learn = SHIFT_DOWN && !c2.shift_used && (c2.page || c2_mode() <= 1);
    if (SHIFT_DOWN && !learn)
        c2.shift_used = 1;             /* SHIFT + note in MAP off the page, or after a combo: just play */
    if (learn) {                       /* learn: SHIFT + notes, which sound as played */
        c2_learn_note(key, note[0]);
        rel[0] = (uint8_t)(key + REL_OFS);
        c2_play(key, rel, 1, 0, -1);
        return;
    }
    if (c2.learning)
        c2_learn_commit();             /* SHIFT's release was not seen: the first plain note ends the learn */
    if (c2.page) {
        c2.last_tap = key;
        if (!c2_frozen())
            c2.sel = key;
    }
    n = c2_key_notes(key, rel, &chord, &bass);
    if (!n)
        return;
    c2_play(key, rel, n, chord, bass);
    if (chord)
        c2.chord_down |= bit;
}

static void c2_keyup(int32_t *note)
{
    int key = synth_mode() ? note_key(note) : -1;
    if (key < 0) {
        te_note_off(note);
        return;
    }
    uint32_t bit = 1u << key;
    c2.chord_down &= ~bit;
    if (c2.dead & bit) {
        c2.dead &= ~bit;
        return;
    }
    if (!c2.key_cnt[key]) {            /* not ours: release like stock, unless one of our chords holds that pitch */
        if (!c2.rel_ref[key + REL_OFS])
            te_note_off(note);
        return;
    }
    if (hold_on && (c2.key_kind[key] == 2 || c2_mode() == 0)) {
        hold_keys |= bit;              /* HOLD latches chords (everything when the slot is OFF, as HOLD1) */
        return;
    }
    c2_release_key(key);
}

/* ---- the page: keys ---- */

/* SHIFT + key on the synth screen (CHORD1's shift slot): 1 = handled. */
static int c2_shiftkey(int key)
{
    c2.shift_used = 1;                 /* any SHIFT + key combo: this SHIFT press no longer learns */
    if (key == KEY_SPLIT) {
        c2.page = !c2.page;
        c2.split_down = 0;
        c2.turned = -1;
        c2.msg = MSG_NONE;
        return 1;
    }
    if (!c2.page)
        return 0;
    if (key == KEY_LIFT) {
        if (c2.loading)
            return 1;
        c2_undo();
        c2_revoice_all();
        return 1;
    }
    if (key == KEY_DROP) {
        c2_flash(MSG_LATER, 0);        /* WRITE (scale fill, copy, factory) is phase 2 */
        return 1;
    }
    return 0;
}

static int c2_press_key(int key)
{
    if (c2.learning && !SHIFT_DOWN)
        c2_learn_commit();             /* SHIFT was let go where our release slot didn't see it */
    if (key == KEY_SPLIT)
        c2.dg.split++;
    else if (key == KEY_LIFT)
        c2.dg.lift++;
    else if (key == KEY_DROP)
        c2.dg.drop++;
    if (key == KEY_SHIFT) {
        c2.dg.shift_dn++;
        c2.shift_used = 0;
        c2.knob_frame = 0;             /* SHIFT ends the freeze, so a learn goes to the last key tapped */
        if (c2.page && !c2.pinned && c2.last_tap >= 0 && c2.last_tap < C2_KEYS)
            c2.sel = c2.last_tap;
        return 0;
    }
    if (!c2.page)
        return 0;
    if (key == KEY_SPLIT) {
        c2.split_down = 1;
        c2.split_used = 0;
        return 1;
    }
    if (key == KEY_DROP && !c2.pinned && c2.last_tap >= 0 && c2.last_tap < C2_KEYS)
        c2.sel = c2.last_tap;          /* LIFT on one key, tap another, DROP: pasted where you tapped */
    if (key == KEY_LIFT || key == KEY_DROP) {
        struct c2_chord *t = c2.loading ? 0 : c2_target();
        if (!t)
            c2_flash(MSG_TURN_GREEN, 0);
        else if (key == KEY_LIFT) {
            c2_copy(&c2.clip, t, sizeof *t);
            c2_flash(MSG_COPIED, c2.sel);
        } else if (c2.clip.q == Q_NONE)
            c2_flash(MSG_EMPTY_CLIP, 0);
        else {
            c2_snapshot(0);
            c2_copy(t, &c2.clip, sizeof *t);
            c2_touch();
            c2_revoice_all();
            c2_flash(MSG_PASTED, c2.sel);
        }
        return 1;
    }
    if (key >= KEY_T1 && key <= KEY_T4)
        c2.page = 0;                   /* T1..T4 close the page and go on to TE's page */
    return 0;
}

static void c2_release_key_ev(int key)
{
    if (key == KEY_SPLIT) {
        if (c2.split_down && !c2.split_used && c2.page) {   /* SPLIT tapped alone: pin / unpin the edited key */
            c2.pinned = !c2.pinned;
            c2_flash(c2.pinned ? MSG_PINNED : MSG_UNPINNED, c2.sel);
        }
        c2.split_down = 0;
    }
    else if (key == KEY_SHIFT) {
        c2.dg.shift_up++;
        c2_learn_commit();
    }
}

/* ---- the page: knobs ---- */

static int c2_in_range(const struct c2_chord *c, int bass)
{
    int8_t iv[C2_NOTES];
    int b, n = c2_notes(c, bass, iv, &b);
    for (int i = 0; i < n; i++)
        if (iv[i] < -C2_IV_MAX || iv[i] > C2_IV_MAX)
            return 0;
    return 1;
}

static int c2_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Edit chord c with knob 0..3 by `steps` (the plain layer). Returns 1 if it changed. */
static int c2_edit_chord(struct c2_chord *c, int knob, int steps, int bass)
{
    struct c2_chord old;
    c2_copy(&old, c, sizeof old);
    if (knob == 1) {                   /* TYPE: scan the ordered list; left past the first = the key's own note */
        int q = c->q == Q_CUSTOM ? c2_custom_q(c) : c->q;
        if (c->q == Q_CUSTOM)
            c2_structure(c, q);
        int to = c2_clampi(q + steps, Q_NONE, Q_COUNT - 1);
        if (to == Q_NONE && q > Q_NONE) {          /* a wall before NOTE: a second detent left within ~1 s */
            if (!c2.wall_frame || c2_now() + 1 - c2.wall_frame > 32) {
                c2.wall_frame = c2_now() + 1;
                to = Q_NONE + 1;
            } else
                c2.wall_frame = 0;
        } else
            c2.wall_frame = 0;
        q = to;
        if (q == Q_NONE) {
            c2_clear(c);
        } else {
            if (old.q == Q_NONE) {
                c2_clear(c);
                c->root = 0;
            }
            c->q = (uint8_t)q;
            int nt = c2_q[q].n;
            c->top = (int8_t)c2_clampi(c->top, -2 * nt, 2 * nt);
        }
    } else if (c->q == Q_NONE) {
        c2_flash(MSG_TURN_GREEN, 0);
        return 0;
    } else if (c->q == Q_CUSTOM) {
        int n = c->n < 1 ? 1 : c->n > C2_MAX ? C2_MAX : c->n;
        if (knob == 0) {               /* ROOT on a learned chord: transpose it */
            for (int i = 0; i < n; i++)
                c->iv[i] = (int8_t)(c->iv[i] + steps);
        } else if (knob == 2) {        /* TOP: the lowest note up an octave, or the highest down */
            for (int s = steps; s != 0; s += s > 0 ? -1 : 1) {
                int lo = 0, hi = 0;
                for (int i = 1; i < n; i++) {
                    lo = c->iv[i] < c->iv[lo] ? i : lo;
                    hi = c->iv[i] > c->iv[hi] ? i : hi;
                }
                if (s > 0)
                    c->iv[lo] = (int8_t)(c->iv[lo] + 12);
                else
                    c->iv[hi] = (int8_t)(c->iv[hi] - 12);
            }
        } else {                       /* VOICING: a recognised chord becomes structured first */
            int root;
            int8_t v[C2_MAX];
            for (int i = 0; i < n; i++)
                v[i] = c->iv[i];
            c2_sort(v, n);
            if (c2_recognise(v, n, &root) == Q_CUSTOM)
                return 0;
            c2_structure(c, 0);
            c->fam = (uint8_t)c2_clampi(c->fam + steps, 0, F_COUNT - 1);
        }
    } else if (knob == 0) {
        int r = c2_clampi(c->root + steps, -11, 11);
        if (c->has_slash)
            c->slash = (int8_t)(c->slash + (r - c->root));
        c->root = (int8_t)r;
    } else if (knob == 2) {
        int nt = c2_q[c->q].n;
        c->top = (int8_t)c2_clampi(c->top + steps, -2 * nt, 2 * nt);
    } else
        c->fam = (uint8_t)c2_clampi(c->fam + steps, 0, F_COUNT - 1);
    if (!c2_in_range(c, bass)) {
        c2_copy(c, &old, sizeof old);
        return 0;
    }
    int same = 1;
    const uint8_t *a = (const uint8_t *)c, *b = (const uint8_t *)&old;
    for (unsigned i = 0; i < sizeof *c; i++)
        same &= a[i] == b[i];
    return !same;
}

static void c2_knob_turn(int knob, int steps)
{
    if (c2.loading || c2.learning || knob < 0 || knob > 7)
        return;                        /* knob turns during a learn do nothing (MODE can't change mid-learn) */
    if (knob >= 4)
        c2.shift_used = 1;
    int s = c2_slot();
    c2.turned = knob;
    if (knob >= 4) {                   /* SHIFT layer: the map's settings */
        if (s < 0)
            return;
        if (knob == 4) {
            int m = c2_clampi(c2.d.mode[s] + steps, 0, C2_MODE_COUNT - 1);
            if (m != c2.d.mode[s]) {
                c2_snapshot(1);
                c2.d.mode[s] = (uint8_t)m;
                c2_touch();
            }
            c2_flash(MSG_MODE, m);
        } else if (knob == 7) {
            int m = c2_mode_map(c2_mode());
            if (m < 0) {
                c2_flash(MSG_TURN_GREEN, 0);
            } else if ((steps > 0) != (c2.d.set[m].bass != 0)) {
                c2_snapshot(1);
                c2.d.set[m].bass = steps > 0;
                c2_touch();
                c2_revoice_all();
                c2_flash(MSG_BASS, steps > 0);
            }
        } else
            c2_flash(MSG_LATER, knob);  /* STRUM, SMOOTH: phase 2/3 */
        c2.knob_frame = c2_now();
        return;
    }
    struct c2_chord *t = c2_target();
    if (!t) {
        c2_flash(MSG_TURN_GREEN, 0);
        return;
    }
    int m = c2_mode_map(c2_mode());
    struct c2_chord e;
    c2_copy(&e, t, sizeof e);
    if (c2_edit_chord(&e, knob, steps, m >= 0 && c2.d.set[m].bass)) {
        c2_snapshot(1);                /* only a real change replaces the undo snapshot */
        c2_copy(t, &e, sizeof e);
        c2_touch();
        if (m >= 0)
            c2_revoice(c2.sel);
        else
            c2_revoice_all();
    }
    c2.knob_frame = c2_now();
}

/* ---- the file (GUI thread: c2_gui_tick from ui.c's frame hook) ----
 * /yaffs2/user/overdub/chorda.bin and chordb.bin, written alternately; the newer valid one wins. Header (24 bytes,
 * little endian): "OVDBCHRD", u16 version, u16 header size, u32 sequence, u32 payload size, u32 CRC-32 of the
 * payload. Payload: 8 maps x 24 chords x 12 bytes, 8 map settings x 16, 8 shapes x 12, 8 slot modes.
 * Missing, short, long, wrong magic / version / size / CRC: not used (the other file, else the factory maps).
 * Every value read is clamped (c2_sanitize). Nothing here can assert: TE's yaffs calls return -1 on errors. */
#define C2_FILE_VERSION 1
#define C2_HDR 24
#define C2_PAYLOAD (C2_MAPS * C2_KEYS * 12 + C2_MAPS * 16 + C2_SLOTS * 12 + C2_SLOTS)
#define C2_FILE (C2_HDR + C2_PAYLOAD)
#define C2_LOAD_FRAME 150              /* ~5 s after boot: TE is up, the disk mounted (TE reads it from boot) */
#define C2_SAVE_DELAY 64               /* frames after the last edit */
#define C2_O_RDONLY 0
#define C2_O_WRITE 0x241               /* WRONLY | CREAT | TRUNC, as TE's own writes (0x019AD3C4, 0x0199FD68) */
static const char c2_dir[] = "/yaffs2/user/overdub";
static const char *const c2_path[2] = { "/yaffs2/user/overdub/chorda.bin", "/yaffs2/user/overdub/chordb.bin" };
static uint8_t c2_buf[C2_FILE + 16] RAM;

static void c2_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void c2_put32(uint8_t *p, uint32_t v) { c2_put16(p, v); c2_put16(p + 2, v >> 16); }
static uint32_t c2_get16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t c2_get32(const uint8_t *p) { return c2_get16(p) | c2_get16(p + 2) << 16; }

static uint8_t *c2_put_chord(uint8_t *p, const struct c2_chord *c)
{
    *p++ = (uint8_t)c->root;
    *p++ = c->q;
    *p++ = c->fam;
    *p++ = (uint8_t)c->top;
    for (int i = 0; i < C2_MAX; i++)
        *p++ = (uint8_t)c->iv[i];
    *p++ = c->n;
    *p++ = c->has_slash;
    *p++ = (uint8_t)c->slash;
    *p++ = 0;
    return p;
}

static const uint8_t *c2_get_chord(const uint8_t *p, struct c2_chord *c)
{
    c->root = (int8_t)*p++;
    c->q = *p++;
    c->fam = *p++;
    c->top = (int8_t)*p++;
    for (int i = 0; i < C2_MAX; i++)
        c->iv[i] = (int8_t)*p++;
    c->n = *p++;
    c->has_slash = *p++;
    c->slash = (int8_t)*p++;
    c->pad = 0;
    p++;
    return p;
}

/* Everything read is clamped to what the engine and the screens accept. */
static void c2_sanitize_chord(struct c2_chord *c)
{
    if (!c2_valid_q(c->q) || (c->q == Q_CUSTOM && (c->n < 1 || c->n > C2_MAX))) {
        c2_clear(c);
        return;
    }
    c->root = (int8_t)c2_clampi(c->root, -11, 11);
    c->fam = (uint8_t)(c->fam < F_COUNT ? c->fam : 0);
    c->top = (int8_t)c2_clampi(c->top, -8, 8);
    for (int i = 0; i < C2_MAX; i++)
        c->iv[i] = (int8_t)c2_clampi(c->iv[i], -C2_IV_MAX, C2_IV_MAX);
    if (c->q != Q_CUSTOM)
        c->n = 0;
    c->has_slash = c->has_slash != 0 && c->q != Q_CUSTOM;
    c->slash = (int8_t)c2_clampi(c->slash, -60, 60);
    if (!c2_in_range(c, 1)) {
        c->top = 0;
        c->fam = F_CLOSE;
        if (!c2_in_range(c, 1))
            c2_clear(c);
    }
}

static void c2_sanitize(struct c2_data *d)
{
    for (int m = 0; m < C2_MAPS; m++) {
        for (int k = 0; k < C2_KEYS; k++)
            c2_sanitize_chord(&d->maps[m][k]);
        struct c2_set *s = &d->set[m];
        s->bass = s->bass != 0;
        s->smooth = s->smooth != 0;
        s->flats = s->flats != 0;
        s->strum = 0;
        int end = 0;
        for (int i = 0; i < 12; i++) {
            char ch = s->name[i];
            end |= ch == 0 || i == 11;
            if (end)
                s->name[i] = 0;
            else if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-' || ch == '#'))
                s->name[i] = '?';
        }
    }
    for (int s = 0; s < C2_SLOTS; s++) {
        c2_sanitize_chord(&d->shape[s]);
        if (d->mode[s] >= C2_MODE_COUNT)
            d->mode[s] = 0;
    }
}

static int c2_serialize(uint8_t *buf, const struct c2_data *d, uint32_t seq)
{
    uint8_t *p = buf + C2_HDR;
    for (int m = 0; m < C2_MAPS; m++)
        for (int k = 0; k < C2_KEYS; k++)
            p = c2_put_chord(p, &d->maps[m][k]);
    for (int m = 0; m < C2_MAPS; m++) {
        *p++ = d->set[m].bass;
        *p++ = d->set[m].smooth;
        *p++ = d->set[m].flats;
        *p++ = d->set[m].strum;
        for (int i = 0; i < 12; i++)
            *p++ = (uint8_t)d->set[m].name[i];
    }
    for (int s = 0; s < C2_SLOTS; s++)
        p = c2_put_chord(p, &d->shape[s]);
    for (int s = 0; s < C2_SLOTS; s++)
        *p++ = d->mode[s];
    static const char magic[8] = { 'O', 'V', 'D', 'B', 'C', 'H', 'R', 'D' };
    for (int i = 0; i < 8; i++)
        buf[i] = (uint8_t)magic[i];
    c2_put16(buf + 8, C2_FILE_VERSION);
    c2_put16(buf + 10, C2_HDR);
    c2_put32(buf + 12, seq);
    c2_put32(buf + 16, C2_PAYLOAD);
    c2_put32(buf + 20, c2_crc32(buf + C2_HDR, C2_PAYLOAD));
    return (int)(p - buf);
}

/* 1 if buf[0..n) is a valid file; *seq its sequence number. */
static int c2_check(const uint8_t *buf, int n, uint32_t *seq)
{
    static const char magic[8] = { 'O', 'V', 'D', 'B', 'C', 'H', 'R', 'D' };
    if (n != C2_FILE)
        return 0;
    for (int i = 0; i < 8; i++)
        if (buf[i] != (uint8_t)magic[i])
            return 0;
    if (c2_get16(buf + 8) != C2_FILE_VERSION || c2_get16(buf + 10) != C2_HDR || c2_get32(buf + 16) != C2_PAYLOAD)
        return 0;
    if (c2_crc32(buf + C2_HDR, C2_PAYLOAD) != c2_get32(buf + 20))
        return 0;
    *seq = c2_get32(buf + 12);
    return 1;
}

static void c2_parse(const uint8_t *buf, struct c2_data *d)
{
    const uint8_t *p = buf + C2_HDR;
    for (int m = 0; m < C2_MAPS; m++)
        for (int k = 0; k < C2_KEYS; k++)
            p = c2_get_chord(p, &d->maps[m][k]);
    for (int m = 0; m < C2_MAPS; m++) {
        d->set[m].bass = *p++;
        d->set[m].smooth = *p++;
        d->set[m].flats = *p++;
        d->set[m].strum = *p++;
        for (int i = 0; i < 12; i++)
            d->set[m].name[i] = (char)*p++;
    }
    for (int s = 0; s < C2_SLOTS; s++)
        p = c2_get_chord(p, &d->shape[s]);
    for (int s = 0; s < C2_SLOTS; s++)
        d->mode[s] = *p++;
    c2_sanitize(d);
}

/* Read file i into c2_buf: its length, or -1 if it can't be opened. */
static int c2_read(int i)
{
    int fd = te_yaffs_open(c2_path[i], C2_O_RDONLY, 0x100);
    if (fd < 0)
        return -1;
    int n = te_yaffs_read(fd, c2_buf, sizeof c2_buf);
    te_yaffs_close(fd);
    return n;
}

/* The newest valid file: its index (or -1) and its sequence number; *seen = some file could be opened. */
static int c2_newest(uint32_t *seq_out, int *seen)
{
    uint32_t seq[2] = { 0, 0 };
    int ok[2];
    *seen = 0;
    for (int i = 0; i < 2; i++) {
        int n = c2_read(i);
        *seen |= n >= 0;
        ok[i] = n >= 0 && c2_check(c2_buf, n, &seq[i]);
    }
    int pick = ok[0] && ok[1] ? ((int32_t)(seq[1] - seq[0]) > 0) : ok[1] ? 1 : ok[0] ? 0 : -1;
    *seq_out = pick >= 0 ? seq[pick] : 0;
    return pick;
}

static void c2_load(void)
{
    uint32_t seq;
    int seen, pick = c2_newest(&seq, &seen);
    if (!seen && ++c2.load_tries < 4)
        return;                        /* nothing opened: maybe the disk isn't up yet; try again in ~8 s */
    c2.loading = 1;                    /* the keys play plain notes; edits wait */
    int n = pick >= 0 ? c2_read(pick) : -1;
    if (n >= 0 && c2_check(c2_buf, n, &seq)) {
        c2_parse(c2_buf, &c2.d);
        c2.file_slot = pick;
        c2.file_seq = seq;
        c2.dg.load = pick ? C2_LOAD_B : C2_LOAD_A;
        c2.dg.load_seq = (int32_t)seq;
    } else {
        c2.dg.load = seen ? C2_LOAD_BAD : C2_LOAD_NONE;   /* factory maps (already in d) */
        c2.file_slot = pick;
        c2.file_seq = seq;
    }
    c2.undo_ok = 0;
    c2.saved_gen = c2.gen;
    c2.loading = 0;
    c2.loaded = 1;
}

static void c2_save(void)
{
    uint32_t g = c2.gen;
    uint32_t seq = c2.file_seq + 1;
    int n = c2_serialize(c2_buf, &c2.d, seq);
    if (c2.gen != g) {                 /* edited while we copied: try again next frame */
        c2.dg.save_err = C2_SAVE_TORN;
        return;
    }
    uint32_t disk_seq;                 /* what is on the disk now decides: never write over the newest good file */
    int seen, newest = c2_newest(&disk_seq, &seen);
    if (newest >= 0 && (int32_t)(disk_seq - c2.file_seq) > 0)
        seq = disk_seq + 1;
    if (newest >= 0)
        c2.file_slot = newest;
    n = c2_serialize(c2_buf, &c2.d, seq);   /* c2_newest used the buffer */
    if (c2.gen != g) {
        c2.dg.save_err = C2_SAVE_TORN;
        return;
    }
    if (!c2.dg.saves && !c2.save_tries)
        c2.dg.mkdir_ret = te_yaffs_mkdir(c2_dir, 0);   /* -1 when it exists already */
    c2.save_tries++;
    c2.save_frame = c2_now();
    int slot = c2.file_slot == 0 ? 1 : 0;              /* never overwrite the newest good file */
    int fd = te_yaffs_open(c2_path[slot], C2_O_WRITE, 0x180);
    if (fd < 0) {
        c2.dg.save_err = C2_SAVE_OPEN;
        c2.fail_gen = g;
        return;
    }
    int w = te_yaffs_write(fd, c2_buf, (unsigned)n);
    int cl = te_yaffs_close(fd);
    if (w != n || cl != 0) {
        c2.dg.save_err = w != n ? C2_SAVE_WRITE : C2_SAVE_CLOSE;
        c2.fail_gen = g;
        return;
    }
    c2.file_slot = slot;
    c2.file_seq = seq;
    c2.saved_gen = g;
    c2.save_tries = 0;
    c2.dg.saves++;
    c2.dg.save_err = C2_SAVE_OK;
}

/* Once per frame from the frame hook (GUI thread, between TE's render and the flush; TE holds no yaffs lock here). */
void c2_gui_tick(void)
{
    if (!C2_ON())
        return;
    uint32_t now = c2_now();
    if (!c2.loaded) {
        if (now >= C2_LOAD_FRAME + 256 * c2.load_tries)
            c2_load();
        return;
    }
    if (c2.gen == c2.saved_gen || (c2.learning && now - c2.learn_frame < 256) || now - c2.edit_frame < C2_SAVE_DELAY)
        return;
    if (c2.save_tries >= 8) {          /* the disk refuses: stop until the next edit */
        if (c2.gen == c2.fail_gen)
            return;
        c2.save_tries = 0;
    }
    if (c2.save_tries && now - c2.save_frame < 256)
        return;
    c2_save();
}
