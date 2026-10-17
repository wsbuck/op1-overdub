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
 *   c2_press    synth controller press slot +0x14 (0x019119A8 -> 0x01A51058), presses without SHIFT: with the page
 *               open SPLIT / LIFT / DROP are ours, T1..T4 close the page and go on to TE; closed, all of them are TE's
 *   c2_release  release slot +0x24 (0x019119B8 -> 0x01A5056C, which always returns 0): SPLIT up, SHIFT up = learn ends
 *   c2_knob     encoder slot +0x2C (0x019119C0 -> 0x01A505BC): page open = ours (knob 0..3, SHIFT = 4..7), else TE's
 *   hold_shiftkey (shift slot +0x1C, CHORD1's): every press while SHIFT is down, note keys included (FUN_019b6c74
 *               calls it before TE's global SHIFT handler FUN_019ac9b8, which then plays the note: FUN_019ac0b0);
 *               SHIFT+SPLIT opens / closes the page; on the page SHIFT+LIFT = undo
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
/* The factory maps 1..4 (5..8 empty): the boot state, and what SHIFT+DROP, DROP restores (c2_factory). */
#define C2_FACTORY_MAPS \
            /* 1 DIATONIC C: C major sevenths on the white keys, black keys plain; TOPs keep each octave's chords in \
             * one register (tops A4..B4 on the lower keys, F5..G5 on the upper) instead of a root-position ladder */ \
            [0] = { \
                FT(0, 0, Q_MAJ7, 2), FT(2, 0, Q_7, 2), FT(4, 0, Q_MIN7, 1), FT(6, 0, Q_HDIM, 0), FT(7, 0, Q_MAJ7, 0), \
                FT(9, 0, Q_MIN7, -1), FT(11, 0, Q_MIN7, -1), FT(12, 0, Q_MAJ7, 1), FT(14, 0, Q_7, 1), \
                FT(16, 0, Q_MIN7, 0), FT(18, 0, Q_HDIM, -1), FT(19, 0, Q_MAJ7, -1), FT(21, 0, Q_MIN7, -2), \
                FT(23, 0, Q_MIN7, -2), \
            }, \
            /* 2 II-V-I in F, Bb, Eb, C on groups of three white keys; D D# E = the minor ii-V-i to C minor. TOPs set so \
             * each group voice-leads (6 semitones of movement over the two changes; SMOOTH is phase 2) */ \
            [1] = { \
                FT(0, 2, Q_MIN7, 2), FT(2, 5, Q_7, 0), FT(4, -4, Q_MAJ7, 2), \
                FT(6, 1, Q_MIN7, 0), FT(7, 5, Q_7, -2), FT(9, -4, Q_MAJ7, 0), \
                FT(11, 1, Q_MIN7, -1), FT(12, 5, Q_7, -3), FT(14, -4, Q_MAJ7, -1), \
                FT(16, 5, Q_MIN7, -4), FT(18, -4, Q_7, -2), FT(19, 0, Q_MAJ7, -4), \
                FT(21, 0, Q_HDIM, -4), FT(22, 4, Q_7B9, -7), FT(23, -4, Q_MIN6, -3), \
            }, \
            /* 3 NEO SOUL: m9 / maj9 / 13 / 7#9 colours around Eb major / C minor on the lower octave (flat roots on the \
             * black keys); the upper octave plays plain notes for a melody over a held pad */ \
            [2] = { \
                FC(0, 0, Q_MIN9), FC(2, 0, Q_7S9), FC(3, 0, Q_MAJ9), FC(5, 0, Q_13), FC(7, 0, Q_MIN9), \
                FC(8, 0, Q_MAJ9), FC(9, 0, Q_MIN7), FC(10, 0, Q_MAJ9), FC(11, 0, Q_HDIM), \
            }, \
            /* 4 POP: C major triads on the white keys, borrowed chords on the black ones */ \
            [3] = { \
                FC(0, 0, Q_MAJ), FC(2, 0, Q_MAJ), FC(4, 0, Q_MIN), FC(6, 0, Q_DIM), FC(7, 0, Q_MAJ), FC(9, 0, Q_MIN), \
                FC(11, 0, Q_MIN), FC(12, 0, Q_MAJ), FC(14, 0, Q_MAJ), FC(16, 0, Q_MIN), FC(18, 0, Q_DIM), \
                FC(19, 0, Q_MAJ), FC(21, 0, Q_MIN), FC(23, 0, Q_MIN), FC(1, -1, Q_MIN), FC(3, 0, Q_MAJ), \
                FC(5, 0, Q_MAJ), FC(8, 1, Q_MAJ), FC(10, 0, Q_MAJ), FC(13, -1, Q_MIN), FC(15, 0, Q_MAJ), \
                FC(17, 0, Q_MAJ), FC(20, 1, Q_MAJ), FC(22, 0, Q_MAJ), \
            },
#define C2_FACTORY_SET { { 0, 0, 0, 0, "DIATONIC C" }, { 0, 1, 1, 0, "II-V-I" }, { 1, 1, 1, 0, "NEO SOUL" }, { 0, 0, 1, 0, "POP" } }
#define C2_FACTORY_N 4
static const struct c2_chord c2_factory_maps[C2_FACTORY_N][C2_KEYS] = { C2_FACTORY_MAPS };
static const struct c2_set c2_factory_set[C2_FACTORY_N] = C2_FACTORY_SET;
struct c2_state c2 RAM = {
    .d = {
        .maps = { C2_FACTORY_MAPS },
        .set = C2_FACTORY_SET,
        .shape = { SH, SH, SH, SH, SH, SH, SH, SH },
    },
    .sel = 7, .turned = -1, .last_key = -1, .last_tap = -1, .file_slot = -1, .wr_last_map = -1, .smooth_key = -1,
    .dg = { .learn_end = C2_LE_NONE, .combo_key = -1 },
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

static void c2_zero(void *dst, int n)
{
    volatile uint8_t *d = dst;         /* volatile: GCC must not turn this into a memset call */
    for (int i = 0; i < n; i++)
        d[i] = 0;
}

static int c2_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

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

/* Undo: the whole state before an edit gesture (turns of one knob within C2_GROUP frames are one gesture; `knob` is
 * the knob + 1, 0 for any other gesture). */
static void c2_snapshot(int knob)
{
    c2.gen++;                          /* an edit starts: a save running now is retried (and waits 2 s) */
    c2.edit_frame = c2_now();
    if (knob && c2.gesture_knob == (uint32_t)knob && c2_now() - c2.knob_frame <= C2_GROUP)
        return;                        /* the same knob again within ~2 s: one gesture */
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

static void c2_revoice_all(void);

/* ---- WRITE (SHIFT+DROP on the page): a whole map from a factory map, diatonic 7ths or triads in a key and scale, a
 * transposed copy of a map, or empty. It opens on "FACTORY -> this map" (maps 5..8: EMPTY), so SHIFT+DROP, DROP is
 * still the NIGHT2 factory restore; turning a knob picks something else, and then a map that is not empty needs DROP
 * twice (OVERWRITE?). One undo gesture. The keys audition the preview while WRITE is open. ---- */
static int c2_in_range(const struct c2_chord *c, int bass);

static int c2_map_empty(int m)
{
    int used = 0;
    for (int k = 0; k < C2_KEYS; k++)
        used |= c2.d.maps[m][k].q != Q_NONE;
    return !used;
}

static int c2_wr_armed(void) { return c2.wr_arm && c2_now() + 1 - c2.wr_arm <= C2_ARM; }

/* A chord transposed by t semitones (same key), kept inside +-35 by octaves. */
static void c2_transpose(struct c2_chord *c, int t)
{
    if (c->q == Q_NONE)
        return;
    if (c->q == Q_CUSTOM) {
        int lo = 99, hi = -99;
        for (int i = 0; i < c->n && i < C2_MAX; i++) {
            c->iv[i] = (int8_t)(c->iv[i] + t);
            lo = c->iv[i] < lo ? c->iv[i] : lo;
            hi = c->iv[i] > hi ? c->iv[i] : hi;
        }
        c->slash = (int8_t)(c->slash + t);
        int sh = hi > 23 ? -12 : lo < -23 ? 12 : 0;
        for (int i = 0; i < c->n && i < C2_MAX; i++)
            c->iv[i] = (int8_t)(c->iv[i] + sh);
        c->slash = (int8_t)(c->slash + sh);
        return;
    }
    int r = c->root + t, sh = r > 11 ? -12 : r < -11 ? 12 : 0;
    c->root = (int8_t)(r + sh);
    c->slash = (int8_t)(c->slash + t + sh);
    for (int g = 0; g < 4 && !c2_in_range(c, 1); g++) {
        int8_t iv[C2_NOTES];
        int bi, n = c2_notes(c, 1, iv, &bi);
        int d = n && iv[n - 1] > C2_IV_MAX ? -12 : 12;
        c->root = (int8_t)(c->root + d);
        c->slash = (int8_t)(c->slash + d);
    }
}

static void c2_wr_name(char *nm, const char *a, const char *b, int c)   /* "D DORIAN 7": 11 characters at most */
{
    char t[24], *p = c2_puts(t, a);
    if (b[0]) {
        *p++ = ' ';
        p = c2_puts(p, b);
    }
    if (c) {
        *p++ = ' ';
        *p++ = (char)c;
        *p = 0;
    }
    int i = 0;
    for (; i < 11 && t[i]; i++)
        nm[i] = t[i];
    for (; i < 12; i++)
        nm[i] = 0;
}

/* The preview from the knobs: wr_map / wr_set. */
static void c2_wr_build(void)
{
    int to = c2.wr_to, from = c2.wr_from;
    struct c2_set *st = &c2.wr_set;
    if (from == W_FACTORY) {
        int f = c2.wr_fac;
        c2_copy(c2.wr_map, c2_factory_maps[f], sizeof c2.wr_map);
        c2_copy(st, &c2_factory_set[f], sizeof *st);
    } else if (from == W_SEVENTHS || from == W_TRIADS) {
        c2_fill(c2.wr_map, c2.wr_key, c2.wr_scale, from == W_SEVENTHS);
        c2_copy(st, &c2.d.set[to], sizeof *st);   /* the target's BASS / SMOOTH / STRUM stay */
        st->flats = (uint8_t)c2_scale_flats(c2.wr_key, c2.wr_scale);
        c2_wr_name(st->name, (st->flats ? c2_pc_flat : c2_pc_sharp)[c2.wr_key], c2_scale_short[c2.wr_scale],
                   from == W_SEVENTHS ? '7' : '3');
    } else if (from == W_COPY) {
        int src = c2.wr_src, t = c2.wr_shift;
        c2_copy(c2.wr_map, c2.d.maps[src], sizeof c2.wr_map);
        c2_copy(st, &c2.d.set[src], sizeof *st);
        for (int k = 0; k < C2_KEYS; k++)
            c2_transpose(&c2.wr_map[k], t);
        char base[13], sfx[5], *p = sfx;
        for (int i = 0; i < 12; i++)
            base[i] = st->name[i];
        base[12] = 0;
        if (!base[0])
            c2_put2(c2_puts(base, "MAP "), src + 1);
        if (t) {
            *p++ = t > 0 ? '+' : '-';
            p = c2_put2(p, t > 0 ? t : -t);
        }
        *p = 0;
        int room = 11 - (t ? (int)(p - sfx) + 1 : 0);
        if (room < (int)sizeof base - 1)
            base[room] = 0;
        c2_wr_name(st->name, base, sfx, 0);
    } else if (from == W_LAST && c2.wr_last_map >= 0) {
        c2_copy(c2.wr_map, c2.wr_last, sizeof c2.wr_map);
        c2_copy(st, &c2.wr_last_set, sizeof *st);
    } else {
        c2_zero(c2.wr_map, sizeof c2.wr_map);
        c2_zero(st, sizeof *st);
    }
}

static void c2_wr_close(void)
{
    c2.wr_open = 0;
    c2.wr_arm = 0;
}

static void c2_wr_open_page(void)
{
    int m = c2_mode_map(c2_mode());
    if (m < 0 || c2.loading) {
        c2_flash(MSG_FACTORY_NOMAP, 0);
        return;
    }
    c2.wr_open = 1;
    c2.wr_to = m;
    c2.wr_from = m < C2_FACTORY_N ? W_FACTORY : W_EMPTY;
    c2.wr_fac = m < C2_FACTORY_N ? m : 0;
    c2.wr_src = m;
    c2.wr_shift = 0;
    c2.wr_touched = 0;
    c2.wr_arm = 0;
    c2.wr_open_frame = c2.wr_frame = c2_now() + 1;
    c2.msg = MSG_NONE;
    c2_wr_build();
}

static void c2_wr_knob(int knob, int steps)
{
    if (knob == 0)                     /* OLD MAP n only once a WRITE replaced one */
        c2.wr_from = c2_clampi(c2.wr_from + steps, 0, c2.wr_last_map >= 0 ? W_COUNT - 1 : W_COUNT - 2);
    else if (knob == 1) {
        if (c2.wr_from == W_FACTORY)
            c2.wr_fac = c2_clampi(c2.wr_fac + steps, 0, C2_FACTORY_N - 1);
        else if (c2.wr_from == W_COPY)
            c2.wr_shift = c2_clampi(c2.wr_shift + steps, -11, 11);
        else if (c2.wr_from != W_EMPTY)
            c2.wr_key = c2_mod12(c2.wr_key + steps);  /* the tonic goes round */
    } else if (knob == 2) {
        if (c2.wr_from == W_COPY)
            c2.wr_src = c2_clampi(c2.wr_src + steps, 0, C2_MAPS - 1);
        else if (c2.wr_from == W_SEVENTHS || c2.wr_from == W_TRIADS)
            c2.wr_scale = c2_clampi(c2.wr_scale + steps, 0, S_COUNT - 1);
    } else if (knob == 3)
        c2.wr_to = c2_clampi(c2.wr_to + steps, 0, C2_MAPS - 1);
    c2.wr_touched = 1;
    c2.wr_arm = 0;
    c2.wr_frame = c2_now();
    c2_wr_build();
}

/* DROP on the WRITE page. */
static void c2_wr_drop(void)
{
    /* SHIFT+DROP, DROP: the plain restore, already asked; later, or after a knob, a used map asks again */
    int to = c2.wr_to, plain = !c2.wr_touched && c2_now() + 1 - c2.wr_open_frame <= C2_ARM;
    if (c2.loading)
        return;
    c2.wr_frame = c2_now();
    if (!plain && !c2_map_empty(to) && !c2_wr_armed()) {
        c2.wr_arm = c2_now() + 1;
        c2_flash(MSG_OVERWRITE, to);
        return;
    }
    c2_snapshot(0);
    if (!c2_map_empty(to)) {           /* what it replaces stays reachable: FROM OLD MAP n (beyond the one undo) */
        c2_copy(c2.wr_last, c2.d.maps[to], sizeof c2.wr_last);
        c2_copy(&c2.wr_last_set, &c2.d.set[to], sizeof c2.wr_last_set);
        c2.wr_last_map = to;
    }
    c2_copy(c2.d.maps[to], c2.wr_map, sizeof c2.d.maps[to]);
    c2_copy(&c2.d.set[to], &c2.wr_set, sizeof c2.d.set[to]);
    /* the slot keeps playing its own map (mid-set): writing elsewhere only says where it went */
    c2_touch();
    c2_wr_close();
    c2.smooth_n = 0;
    c2_revoice_all();
    if (plain)
        c2_flash(MSG_FACTORY_DONE, to);
    else
        c2_flash(MSG_WRITTEN, to);
}

/* TE's note key codes (FUN_019c316c, table 0x018E0A80: 0x0D..0x0F, 0x16..0x20, 0x27..0x2A, 0x30..0x35). With SHIFT
 * down they reach the SHIFT slot first, then TE's handler plays them: they are a learn's notes, not combos. */
static int c2_note_code(int key)
{
    return (key >= 0x0D && key <= 0x0F) || (key >= 0x16 && key <= 0x20) || (key >= 0x27 && key <= 0x2A) ||
           (key >= 0x30 && key <= 0x35);
}

/* This SHIFT press did a combo (`key`: the key code, 0x100 + knob): it no longer learns. */
static void c2_combo(int key)
{
    c2.shift_used = 1;
    c2.dg.combo_key = key;
}

/* ---- STRUM: our notes through the sequencer thread ----
 * TE's voice control has no lock, and the only fine timer is TE's sequencer thread (FUN_01a4318c: it pends on its
 * semaphore, then runs the sequencer step FUN_01a0d2c0; our c2_seq_tick wraps that CALL at 0x01A431AA, hold_impl.h).
 * So while the slot's map strums, NO note of ours is sent from the KeyHandler: the KeyHandler only keeps the wanted
 * state (rel_ref, plus a due time and an onset count per note) and the tick sends the difference, offs first, then
 * the ons that are due. The KeyHandler pre-empts the tick (priority 26 over 27), so the tick always sees a whole key
 * event; a tick pre-empted half way just finishes the difference on its next run. Nothing is queued, so nothing can
 * be sent out of order: a note released before its strum turn never sounds, and every tick converges on rel_ref.
 * owner: 0 the KeyHandler sends (the MVP path, every map without STRUM); 1 the tick sends; 2 the tick sends until
 * nothing is pending, then hands back (0). The KeyHandler only hands over when the GUI has seen the tick run for 4
 * frames in a row; if it then stops for 3 frames, the KeyHandler takes the notes back at its next event and sends the
 * difference itself (dg.takeovers). */
#define C2_TU_MS 781                   /* c2_time units per ms (512 cycles at 400 MHz) */
#define C2_STALE_TICKS 250             /* a wanted note waits at most this many ticks (~360 ms), whatever the clock says */
#define C2_LIVE 4                      /* GUI frames in a row the tick must have run before strums go through it */
#define C2_IDLE 3                      /* GUI frames without a tick before the KeyHandler takes over */

static int c2_strum_map(void)
{
    int m = c2_mode_map(c2_mode());
    return m >= 0 ? c2.d.set[m].strum : 0;
}

/* KeyHandler: note r (relative) on or off; rel_ref is already updated by the caller. delay: c2_time units. */
static void c2_out(int r, int on, uint32_t delay)
{
    if (!c2.owner) {
        rel_note(r - REL_OFS, on);
        return;
    }
    if (on) {
        c2.due[r] = c2_time() + delay;
        c2.onset[r]++;                   /* a new onset: a note still on is retriggered */
    } else
        c2.off_due[r] = c2_time() + delay;
}

/* KeyHandler takes the notes back from a tick that stopped: send the difference itself. */
static void c2_take_back(void)
{
    for (int r = 0; r < 96; r++) {
        int want = c2.rel_ref[r] != 0, was = c2.on[r];
        if (was && !want)
            rel_note(r - REL_OFS, 0);
        else if (!was && want)
            rel_note(r - REL_OFS, 1);
        else if (!was && !want && c2.raw_off[r])
            rel_note(r - REL_OFS, 0);
        c2.on[r] = 0;
        c2.raw_off[r] = 0;
    }
}

/* At every KeyHandler event: hand the notes to the tick while the map strums, back when it stops. */
static void c2_owner_update(void)
{
    uint32_t o = c2.owner;
    int want = c2_strum_map() != 0 && c2.seq_live >= C2_LIVE && !c2.loading;
    if (o && c2.seq_idle >= C2_IDLE) {
        c2.owner = 0;
        c2_take_back();
        c2.dg.takeovers++;
        return;
    }
    if (want && !o) {
        for (int r = 0; r < 96; r++) {
            c2.on[r] = c2.rel_ref[r] != 0;
            c2.seen[r] = c2.onset[r];
            c2.wait[r] = 0;
            c2.raw_off[r] = 0;
        }
        c2.seq_reset = 0;
        c2.owner = 1;
    } else if (want && o == 2)
        c2.owner = 1;
    else if (!want && o == 1)
        c2.owner = 2;
}

/* The sequencer thread, every tick (hold_impl.h c2_seq_tick, after TE's step): send what rel_ref wants. */
static void c2_seq_service(void)
{
    uint32_t now = c2_time();
    c2.seq_ticks++;
    uint32_t gap = now - c2.tick_last;
    if (c2.tick_n && gap > c2.tick_gap)
        c2.tick_gap = gap;
    c2.tick_last = now;
    c2.tick_n++;
    if (now - c2.tick_t0 >= 1000u * C2_TU_MS) {
        c2.dg.tick_rate = c2.tick_n;
        c2.dg.tick_gap = c2.tick_gap;
        c2.tick_n = 0;
        c2.tick_gap = 0;
        c2.tick_t0 = now;
    }
    uint32_t o = c2.owner;
    if (!o || VOICECTL_LOCKED)
        return;                        /* a preset is loading: everything waits for the next tick */
    if (c2.seq_reset) {                /* a play-mode switch: TE dropped the voices; forget what we sent */
        for (int r = 0; r < 96; r++)
            c2.on[r] = 0;
        c2.seq_reset = 0;
    }
    int pending = 0;
    for (int r = 0; r < 96; r++) {     /* offs first, so an onset never takes a 7th voice */
        if (!c2.on[r])
            continue;
        if (c2.onset[r] == c2.seen[r]) {
            if (c2.rel_ref[r])
                continue;
            int32_t left = (int32_t)(c2.off_due[r] - now);
            if (left > 0 && left <= 400 * C2_TU_MS && c2.wait[r] < C2_STALE_TICKS) {
                c2.wait[r]++;          /* a legato strum: it hands its voice over when its successor starts */
                pending = 1;
                continue;
            }
        }
        rel_note(r - REL_OFS, 0);
        c2.on[r] = 0;
        c2.wait[r] = 0;
    }
    for (int r = 0; r < 96; r++)
        if (c2.raw_off[r]) {
            c2.raw_off[r] = 0;
            if (!c2.on[r] && !c2.rel_ref[r])
                rel_note(r - REL_OFS, 0);
        }
    int sounding = 0;
    for (int r = 0; r < 96; r++)
        sounding += c2.on[r];
    for (int r = 0; r < 96; r++) {
        if (!c2.rel_ref[r] || c2.on[r])
            continue;
        int32_t until = (int32_t)(c2.due[r] - now);
        int stale = until > 400 * C2_TU_MS || c2.wait[r] >= C2_STALE_TICKS;
        if (until > 0 && !stale) {
            c2.wait[r]++;
            pending = 1;
            continue;
        }
        for (int h = 0; h < 96 && sounding >= C2_VOICES; h++)   /* the cap holds: a voice still handing over goes */
            if (c2.on[h] && !c2.rel_ref[h]) {
                rel_note(h - REL_OFS, 0);
                c2.on[h] = 0;
                c2.wait[h] = 0;
                sounding--;
            }
        sounding++;
        c2.seen[r] = c2.onset[r];
        rel_note(r - REL_OFS, 1);
        c2.on[r] = 1;
        c2.wait[r] = 0;
        c2.dg.strum_sent++;
        if (until > 0)
            c2.dg.strum_forced++;
    }
    if (o == 2 && !pending)
        c2.owner = 0;                  /* nothing left to strum: the KeyHandler sends again */
}

/* GUI, every frame: is the tick running? */
static void c2_seq_watch(void)
{
    uint32_t t = c2.seq_ticks;
    if (t != c2.seq_seen) {
        c2.seq_seen = t;
        c2.seq_idle = 0;
        if (c2.seq_live < 8)
            c2.seq_live++;
    } else {
        if (c2.seq_idle < 255)
            c2.seq_idle++;
        if (c2.seq_idle >= C2_IDLE)
            c2.seq_live = 0;
    }
}

/* ---- playing: our own voice tracking, by key ---- */

static int c2_voice_count(void)
{
    int n = 0;
    for (int r = 0; r < 96; r++)
        n += c2.rel_ref[r] != 0;
    return n;
}

/* The played-notes view (page): what sounds now, kept as it was when the last note stops. */
static void c2_view_update(void)
{
    uint32_t w[3] = { 0, 0, 0 }, roots = 0;
    for (int r = 0; r < 96; r++)
        if (c2.rel_ref[r])
            w[r >> 5] |= 1u << (r & 31);
    if (!(w[0] | w[1] | w[2]))
        return;
    int mode = c2_mode(), s = c2_slot(), m = c2_mode_map(mode);
    for (int k = 0; k < C2_KEYS; k++) {
        if (c2.key_kind[k] != 2)
            continue;
        const struct c2_chord *c = c2.wr_open ? &c2.wr_map[k] : m >= 0 ? &c2.d.maps[m][k] :
                                   mode == 1 && s >= 0 ? &c2.d.shape[s] : 0;
        int rt;
        if (c && c2_shown(c, &rt) != Q_NONE)
            roots |= 1u << c2_mod12(k + rt);
    }
    c2.view_rel[0] = w[0];
    c2.view_rel[1] = w[1];
    c2.view_rel[2] = w[2];
    c2.view_roots = roots;
}

static void c2_release_key(int key)
{
    int n = c2.key_cnt[key];
    for (int i = 0; i < n; i++) {
        int r = c2.key_rel[key][i];
        if (r < 96 && c2.rel_ref[r] && --c2.rel_ref[r] == 0)
            c2_out(r, 0, 0);
    }
    c2.key_cnt[key] = 0;
    c2.key_kind[key] = 0;
    c2.key_bass[key] = -1;
    if (c2.sounding && !(c2.sounding &= ~(1u << key)))
        c2.silent_since = c2_time();   /* the last chord stopped: SMOOTH's silence starts */
    c2.dg.voices = c2_voice_count();
    c2_view_update();
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
    c2.keys_held = 0;
    c2.smooth_n = 0;
    if (c2.owner)
        c2.seq_reset = 1;              /* the tick forgets what it sent (TE dropped the voices) */
}

/* SMOOTH: 2 bars at TE's tempo (samples per beat, Q14; pingpong's valid range, else 120 BPM) in c2_time units:
 * 8 beats x spb samples x 17.72 units per sample (400 MHz / 512 / 44100). */
static uint32_t c2_two_bars(void)
{
    uint32_t spb = (uint32_t)TEMPO_SPB >> 14;
    if (spb < 11980 || spb > 66300)
        spb = 22050;
    return spb * 142;
}

static int8_t c2_kn_upper[C2_MAX] RAM; /* c2_key_notes: the upper notes it chose (key-relative), for SMOOTH */
static int c2_kn_un RAM;
static int c2_kn_top RAM;              /* ... and the TOP */

/* The relative notes key `key` plays now (key + interval + REL_OFS, deduplicated, ascending, inside the note range);
 * *chord = 1 for a chord, 0 for its plain note; *bass = index of the BASS note or -1. smooth: a new chord (SMOOTH may
 * pick its TOP); 0 for a re-voice (an edit auditions the chord's own TOP). WRITE open: the preview plays. */
static int c2_key_notes(int key, uint8_t *rel, int *chord, int *bass, int smooth)
{
    int mode = c2_mode(), s = c2_slot(), m = c2_mode_map(mode);
    const struct c2_chord *c = m >= 0 ? &c2.d.maps[m][key] : mode == 1 && s >= 0 ? &c2.d.shape[s] : 0;
    int bassf = m >= 0 && c2.d.set[m].bass, sm = smooth && m >= 0 && c2.d.set[m].smooth;
    if (c2.wr_open && m >= 0) {
        c = &c2.wr_map[key];
        bassf = c2.wr_set.bass;
        sm = 0;
    }
    int8_t iv[C2_NOTES];
    int b = -1, n = 0;
    c2_kn_un = 0;
    if (c && c->q != Q_NONE) {
        int top = c->top;
        if (sm && c2.smooth_n > 0 && c2.smooth_map == m && !(c->flags & C2_ANCHOR))
            top = c2_smooth(c, key, c2.smooth_prev, c2.smooth_n);   /* an anchored chord plays its own TOP */
        c2_kn_top = top;
        n = c2_notes_t(c, bassf, top, iv, &b);
        int8_t u[C2_MAX];
        int nu = c2_upper(c, top, u);
        for (int i = 0; i < nu; i++)
            c2_kn_upper[i] = (int8_t)(u[i] + key);
        c2_kn_un = nu;
    }
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
        c2_out(r, 0, 0);
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
    static const uint8_t order_chord[3] = { 0, 1, 2 }, order_plain[3] = { 1, 0, 2 }, order_learn[3] = { 0, 2, 1 };
    const uint8_t *order = chord ? order_chord : c2.learning ? order_learn : order_plain;   /* learn: the pad goes */
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
    uint8_t need[C2_NOTES], pos[C2_NOTES];
    int nn = 0;
    for (int i = 0; i < n && i < C2_NOTES; i++)
        if (c2.rel_ref[rel[i]]++ == 0) {
            pos[nn] = (uint8_t)i;
            need[nn++] = rel[i];
        }
    c2_track(key, rel, n, chord, bass);
    c2.key_age[key] = ++c2.age;
    uint8_t gone[2 * C2_NOTES + 6];    /* notes the victims sound now (a legato strum hands their voices over) */
    int ng = 0;
    if (c2.owner) {                    /* a hand-over still running is cut short: its notes stop at the next tick, so
                                        * the budget (which only counts rel_ref) holds */
        uint32_t t = c2_time();
        for (int r = 0; r < 96; r++)
            c2.off_due[r] = t;
    }
    for (int k = 0; k < C2_KEYS && c2.owner; k++)
        if (victims >> k & 1)
            for (int i = 0; i < c2.key_cnt[k] && ng < (int)sizeof gone; i++)
                if (c2.on[c2.key_rel[k][i]])   /* only what really sounds hands a voice over */
                    gone[ng++] = c2.key_rel[k][i];
    c2_let_go(victims);
    /* STRUM: the whole spread is c2_strum_ms[|strum|]; note i of n (from the bottom up, or the top down) starts at
     * spread x i / (n - 1) (Q8 reciprocals: no division); a BASS note always starts at once (with a down strum's
     * top note, as a left hand would); common tones keep sounding */
    static const uint16_t recip8[C2_NOTES] = { 0, 256, 128, 85, 64 };
    int st = chord && c2.owner ? c2_strum_map() : 0;
    uint32_t step = 0;
    if (st && n > 1) {
        uint32_t spread = (uint32_t)c2_strum_spread(st) * C2_TU_MS;
        step = (spread * recip8[n - 1]) >> 8;
    }
    uint32_t when[C2_NOTES];
    int nw = 0;
    for (int i = 0; i < nn; i++) {
        int at = st < 0 ? n - 1 - pos[i] : pos[i];
        if (bass >= 0 && pos[i] == bass)
            at = 0;
        c2_out(need[i], 1, step * (uint32_t)at);
        if (step)
            when[nw++] = step * (uint32_t)at;
    }
    /* a legato change under STRUM: each note let go stops when the next new note starts, one for one in the strum's
     * direction (the sum never passes the budget: offs go first in the tick), so the pad never thins out mid-roll */
    if (nw && ng) {
        for (int i = 1; i < nw; i++)   /* onsets in time order */
            for (int j = i; j > 0 && when[j - 1] > when[j]; j--) {
                uint32_t t = when[j];
                when[j] = when[j - 1];
                when[j - 1] = t;
            }
        for (int i = 1; i < ng; i++)   /* released notes from the strum's start (low first up, high first down) */
            for (int j = i; j > 0 && (st > 0 ? gone[j - 1] > gone[j] : gone[j - 1] < gone[j]); j--) {
                uint8_t t = gone[j];
                gone[j] = gone[j - 1];
                gone[j - 1] = t;
            }
        uint32_t now = c2_time();
        for (int i = 0, w = 0; i < ng; i++)
            if (!c2.rel_ref[gone[i]])
                c2.off_due[gone[i]] = now + when[w < nw ? w++ : nw - 1];
    }
    c2.dg.voices = c2_voice_count();
    c2_view_update();
}

/* A sounding key whose chord was edited (live audition): the notes it keeps are not retriggered, the ones it loses go
 * off, the new ones on, inside the voice budget. */
static void c2_revoice(int key)
{
    if (!c2.key_cnt[key] || !(KEY_DOWN[key] || (hold_keys >> key & 1)))
        return;
    uint8_t rel[C2_NOTES], old[C2_NOTES];
    int chord, bass, n = c2_key_notes(key, rel, &chord, &bass, 0), on = c2.key_cnt[key];
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
            c2_out(old[i], 0, 0);
    c2_let_go(c2_budget(key, rel, 0, 0, chord));
    c2_track(key, rel, n, chord, bass);
    if (chord && c2_kn_un >= 2) {     /* SMOOTH leads on from what sounds now (an edit auditioned) */
        for (int i = 0; i < c2_kn_un; i++)
            c2.smooth_prev[i] = c2_kn_upper[i];
        c2.smooth_n = c2_kn_un;
        c2.smooth_key = key;
        c2.smooth_top = c2_kn_top;
    }
    for (int i = 0; i < nn; i++)
        c2_out(need[i], 1, 0);
    c2.dg.voices = c2_voice_count();
    c2_view_update();
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
        if (c2.key_cnt[k] && !KEY_DOWN[k] && !(hold_keys >> k & 1)) {
            c2_release_key(k);
            c2.dg.purges++;
        }
        if (!KEY_DOWN[k])
            c2.chord_down &= ~(1u << k);
    }
}

/* ---- learn: SHIFT + notes; written when SHIFT is let go ---- */

static void c2_learn_note(int key, int32_t note)
{
    if (!c2.learning) {
        c2.learning = 1;
        c2.dg.learn_starts++;
        c2.learn_frame = c2_now();
        c2.learn_n = 0;
        c2.learn_over = 0;
        c2.learn_page = (int32_t)c2.page;
        int32_t t[5];
        if (c2.page && !c2.pinned && c2.last_tap >= 0 && c2.last_tap < C2_KEYS)
            c2.sel = c2.last_tap;      /* a learn goes to the edited key: the last key tapped, unless pinned */
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
    *over = n > C2_NOTES || c2.learn_over;
    if (n > C2_NOTES) {                /* the bass and the top 4: the melody note is kept, inner notes go */
        iv[0] = v[0];
        for (int i = 1; i < C2_NOTES; i++)
            iv[i] = v[n - C2_NOTES + i];
        return C2_NOTES;
    }
    for (int i = 0; i < n; i++)
        iv[i] = v[i];
    return n;
}

static void c2_learn_commit(int why)
{
    if (!c2.learning || c2.loading)
        return;
    c2.learning = 0;
    int8_t iv[C2_NOTES];
    int over, n = c2_learn_take(iv, &over), s = c2_slot();
    if (!n || s < 0) {
        c2.dg.learn_end = C2_LE_EMPTY;
        return;
    }
    c2.dg.learn_end = (uint32_t)why;
    c2.dg.learn_ok++;
    c2_snapshot(0);
    struct c2_chord nc;
    c2_clear(&nc);
    nc.q = Q_CUSTOM;
    int first = 0;
    if (n == C2_NOTES) {               /* 5 notes: the lowest is kept as the bass under the four */
        nc.has_slash = 1;
        nc.slash = iv[0];
        first = 1;
    }
    nc.n = (uint8_t)(n - first);
    for (int i = first; i < n; i++)
        nc.iv[i - first] = iv[i];
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
    c2_owner_update();
    c2_purge_stale();
    if (hold_keys & bit) {             /* a latched key pressed again: release it first, so it retriggers */
        hold_keys &= ~bit;
        c2_release_key(key);
    } else if (c2.key_cnt[key])
        c2_release_key(key);           /* its key-up never came */
    c2.dead &= ~bit;
    if (c2.page && c2.split_down) {    /* SPLIT + key: clear that key's chord, play nothing */
        int was = c2.sel;
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
        if (c2.pinned)
            c2.sel = was;              /* pinned: the cleared key does not take the edit */
        else
            c2.last_tap = key;
        return;
    }
    uint8_t rel[C2_NOTES];
    int chord = 0, bass = -1, n;
    if (!SHIFT_DOWN)
        c2.shift_used = 0;             /* also without the press hook: a plain key ends any SHIFT combo */
    int learn = SHIFT_DOWN && !c2.shift_used && !c2.wr_open && (c2.page || c2_mode() <= 1);
    if (SHIFT_DOWN && !learn) {        /* SHIFT + note in MAP off the page, or after a combo: just play */
        if (!c2.learning) {
            c2.dg.learn_blocked++;
            c2.dg.learn_end = c2.shift_used ? C2_LE_BLOCKED : C2_LE_MAP;
        }
        if (!c2.shift_used)
            c2_combo(key + 0x200);     /* MAP off the page: this SHIFT press stays a plain one */
    }
    if (learn) {                       /* learn: SHIFT + notes, which sound as played */
        c2_learn_note(key, note[0]);
        rel[0] = (uint8_t)(key + REL_OFS);
        c2_play(key, rel, 1, 0, -1);
        return;
    }
    if (c2.learning)
        c2_learn_commit(C2_LE_NOTE);   /* SHIFT's release was not seen: the first plain note ends the learn */
    int m = c2_mode_map(c2_mode());
    if (m >= 0 && (c2.smooth_map != m || (!c2.sounding && c2_time() - c2.silent_since > c2_two_bars()))) {
        if (c2.smooth_n)
            c2.dg.smooth_resets++;
        c2.smooth_n = 0;               /* SMOOTH starts over: 2 bars of silence, or another map */
        c2.smooth_map = m;
    }
    n = c2_key_notes(key, rel, &chord, &bass, 1);
    if (c2.wr_open)
        c2.wr_frame = c2_now();        /* auditioning keeps WRITE open */
    if (c2.page && !c2.wr_open) {      /* the edited key = the last key pressed, unless pinned */
        c2.last_tap = key;
        if (!c2.pinned)
            c2.sel = key;
    }
    if (!n)
        return;
    c2_play(key, rel, n, chord, bass);
    c2.keys_held |= bit;
    if (chord) {
        c2.chord_down |= bit;
        if (c2_kn_un >= 2 && m >= 0) {   /* SMOOTH leads on from what this chord played */
            for (int i = 0; i < c2_kn_un; i++)
                c2.smooth_prev[i] = c2_kn_upper[i];
            c2.smooth_n = c2_kn_un;
            c2.smooth_key = key;
            c2.smooth_top = c2_kn_top;
        }
    }
}

static void c2_keyup(int32_t *note)
{
    int key = synth_mode() ? note_key(note) : -1;
    if (key < 0) {
        te_note_off(note);
        return;
    }
    uint32_t bit = 1u << key;
    c2_owner_update();
    c2.chord_down &= ~bit;
    c2.keys_held &= ~bit;
    c2.release_frame = c2_now();
    if (c2.dead & bit) {
        c2.dead &= ~bit;
        return;
    }
    if (!c2.key_cnt[key]) {            /* not ours: release like stock, unless one of our chords holds that pitch */
        if (!c2.rel_ref[key + REL_OFS]) {
            if (c2.owner)
                c2.raw_off[key + REL_OFS] = 1;   /* strumming: only the tick talks to TE's voices */
            else
                te_note_off(note);
        }
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
static int c2_learn_cancel(void)
{
    if (!c2.learning)
        return 0;
    c2.learning = 0;
    c2.dg.learn_cancels++;
    c2.dg.learn_end = C2_LE_CANCEL;
    c2_flash(MSG_LEARN_CANCEL, 0);
    return 1;
}

static int c2_shiftkey(int key)
{
    c2.wr_arm = 0;                     /* any SHIFT combo drops a pending OVERWRITE? */
    if (c2_note_code(key))
        return 0;                      /* SHIFT + a note: TE plays it next, and c2_keydown learns it */
    if ((key == KEY_LIFT || key == KEY_DROP || key == KEY_SPLIT) && c2_learn_cancel())
        return 1;                      /* a combo during a learn cancels it and does nothing else */
    if (key == KEY_SPLIT) {
        c2.page = !c2.page;
        c2_wr_close();
        c2.pinned = 0;                 /* the pin ends when the page closes (and a page opens unpinned) */
        c2.split_down = 0;
        c2.turned = -1;
        c2.msg = MSG_NONE;
        if (c2.page && c2_strum_map() && c2.seq_live < C2_LIVE)
            c2_flash(MSG_NO_TICK, 1);  /* a strumming map with no sequencer tick: it plays at once */
        return 1;
    }
    if (!c2.page)
        return 0;
    if (key == KEY_LIFT) {
        if (c2.loading)
            return 1;
        c2_wr_close();
        c2_undo();
        c2_revoice_all();
        return 1;
    }
    if (key == KEY_DROP) {             /* WRITE: opens on the factory restore; SHIFT+DROP again closes it */
        if (c2.wr_open) {
            c2_wr_close();
            c2_flash(MSG_WRITE_CANCEL, 0);
        } else
            c2_wr_open_page();
        return 1;
    }
    return 0;
}

static int c2_press_key(int key)
{
    if (c2.learning && !SHIFT_DOWN)
        c2_learn_commit(C2_LE_PRESS);  /* SHIFT was let go where our release slot didn't see it */
    if (key == KEY_SPLIT)
        c2.dg.split++;
    else if (key == KEY_LIFT)
        c2.dg.lift++;
    else if (key == KEY_DROP)
        c2.dg.drop++;
    if (key != KEY_SHIFT && key != KEY_DROP)
        c2.wr_arm = 0;                 /* OVERWRITE? waits for DROP only */
    if (key == KEY_SHIFT) {
        c2.dg.shift_dn++;
        c2.shift_used = 0;             /* the edited key moves only at the first learn note (c2_learn_note) */
        return 0;
    }
    if (!c2.page)
        return 0;
    if (c2.wr_open && (key == KEY_SPLIT || key == KEY_LIFT || key == KEY_DROP)) {
        if (key == KEY_DROP)
            c2_wr_drop();              /* WRITE: DROP writes (OVERWRITE? needs it twice) */
        else if (key == KEY_SPLIT) {
            c2_wr_close();             /* SPLIT cancels */
            c2_flash(MSG_WRITE_CANCEL, 0);
        }
        return 1;
    }
    if (key == KEY_SPLIT) {
        c2.split_down = 1;
        c2.split_used = 0;
        c2.split_frame = c2_now();
        return 1;
    }
    if (key == KEY_LIFT || key == KEY_DROP) {
        /* LIFT copies the edited key; DROP pastes onto the last key tapped (the edited key unless pinned: pin a
         * chord, LIFT, then tap and DROP to copy it around) */
        int was = c2.sel;
        if (key == KEY_DROP && c2.last_tap >= 0 && c2.last_tap < C2_KEYS)
            c2.sel = c2.last_tap;
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
        c2.sel = was;                  /* the paste never moves the edited key */
        return 1;
    }
    if (key >= KEY_T1 && key <= KEY_T4) {
        c2.page = 0;                   /* T1..T4 close the page and go on to TE's page */
        c2_wr_close();
        c2.pinned = 0;
    }
    return 0;
}

static void c2_release_key_ev(int key)
{
    if (key == KEY_SPLIT) {
        if (c2.split_down && !c2.split_used && c2.page && c2_now() - c2.split_frame <= C2_TAP) {
            /* SPLIT tapped alone (short: a SPLIT held to clear, then let go unused, does nothing): pin / unpin */
            c2.pinned = !c2.pinned;
            c2_flash(c2.pinned ? MSG_PINNED : MSG_UNPINNED, c2.sel);
        }
        c2.split_down = 0;
    }
    else if (key == KEY_SHIFT) {
        c2.dg.shift_up++;
        c2_learn_commit(C2_LE_UP);
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


/* VOICING after a TYPE change: the nearest position toward CLOSE that exists for the new chord. */
static void c2_voi_fix(struct c2_chord *c)
{
    for (int g = 0; g < 9 && c->voi && !c2_voi_applies(c, c->voi); g++)
        c->voi = (int8_t)(c->voi > 0 ? c->voi - 1 : c->voi + 1);
}

/* Edit chord c with knob 0..3 by `steps` (the plain layer). Returns 1 if it changed. */
static int c2_edit_chord(struct c2_chord *c, int knob, int steps, int bass)
{
    struct c2_chord old;
    c2_copy(&old, c, sizeof old);
    if (knob == 1) {                   /* TYPE: scan the ordered list; NOTE between the chords and the WIDE group */
        int q = c->q == Q_CUSTOM ? c2_custom_q(c) : c->q;
        if (c->q == Q_CUSTOM)
            c2_structure(c, q);
        int pos = c2_scan_pos(q), to = c2_clampi(pos + steps, C2_SCAN_MIN, C2_SCAN_MAX);
        if (pos && (pos > 0 ? to <= 0 : to >= 0)) {   /* a wall each side of NOTE: rest there ~0.3 s, then one more */
            uint32_t since = c2_now() + 1 - c2.wall_frame;
            if (!c2.wall_frame || since < 10 || since > 64) {
                if (!c2.wall_frame || since > 64)
                    c2.wall_frame = c2_now() + 1;
                to = pos > 0 ? 1 : -1;
                c2_flash(MSG_WALL, 0);
            } else {
                c2.wall_frame = 0;
                to = 0;
            }
        } else
            c2.wall_frame = 0;
        q = c2_scan_q(to);
        if (q == Q_NONE) {
            c2_clear(c);
        } else {
            if (old.q == Q_NONE) {
                c2_clear(c);
                c->root = 0;
            }
            c->q = (uint8_t)q;
            int nt = c2_q[q].n, lim = c2_is_wide(q) ? 2 : 2 * nt;
            c->top = (int8_t)c2_clampi(c->top, -lim, lim);
            c2_voi_fix(c);
            for (int g = 0; g < 4 && !c2_in_range(c, bass); g++)
                c->top = (int8_t)(c->top > 0 ? c->top - 1 : c->top < 0 ? c->top + 1 : 0);   /* a wide one fits */
        }
    } else if (c->q == Q_NONE) {
        c2_flash(MSG_TURN_GREEN, 0);
        return 0;
    } else if (knob == 0) {
        if (c->q == Q_CUSTOM) {        /* ROOT on a learned chord: transpose it */
            int n = c2_voi_n(c);
            for (int i = 0; i < n; i++)
                c->iv[i] = (int8_t)(c->iv[i] + steps);
            c->slash = (int8_t)(c->slash + steps);
        } else {
            int r = c2_clampi(c->root + steps, -11, 11);
            if (c->has_slash)
                c->slash = (int8_t)(c->slash + (r - c->root));
            c->root = (int8_t)r;
        }
    } else if (knob == 2) {            /* TOP: a ladder from the voicing as built (learned: as played) */
        int n = c2_voi_n(c), lim = c->q != Q_CUSTOM && c2_is_wide(c->q) ? 2 : 2 * n;
        int m = c2_mode_map(c2_mode());
        if (m >= 0 && c2.d.set[m].smooth) {   /* SMOOTH on: start from the TOP it played, and anchor the chord */
            if (!(c->flags & C2_ANCHOR) && c2.smooth_n > 0 && c2.smooth_map == m && c2.smooth_key == c2.sel)
                c->top = (int8_t)c2.smooth_top;
            c->flags |= C2_ANCHOR;
            c2_flash(MSG_ANCHOR, 0);
        } else
            c->flags &= (uint8_t)~C2_ANCHOR;   /* a TOP set without SMOOTH: no anchor */
        c->top = (int8_t)c2_clampi(c->top + steps, -lim, lim);
    } else {                           /* VOICING: bipolar; positions that don't exist (or don't fit) are skipped */
        int dir = steps > 0 ? 1 : -1, v = c->voi;
        if (dir > 0 && !v && !c2.voi_hint) {
            c2.voi_hint = 1;           /* NIGHT2 had the drops on the right: say it once */
            c2_flash(MSG_VOI_HINT, 0);
        }
        for (int s = steps; s != 0; s -= dir) {
            int cand = v + dir;
            for (; cand >= C2_VOI_MIN && cand <= C2_VOI_MAX; cand += dir) {
                c->voi = (int8_t)cand;
                if (c2_voi_applies(c, cand) && c2_in_range(c, bass))
                    break;
            }
            if (cand < C2_VOI_MIN || cand > C2_VOI_MAX) {
                c2_flash(MSG_VOI_END, dir);
                break;
            }
            v = cand;
        }
        c->voi = (int8_t)v;
    }
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
        c2_combo(0x100 + knob);
    if (c2.wr_open) {                  /* WRITE: the plain knobs pick what to write; SHIFT + knobs do nothing */
        if (knob < 4)
            c2_wr_knob(knob, steps);
        else
            c2_flash(MSG_SHIFT_UP, 0); /* SHIFT still held after SHIFT+DROP: the knobs need it let go */
        c2.turned = knob;
        c2.knob_frame = c2_now();
        return;
    }
    c2_owner_update();
    int s = c2_slot();
    c2.turned = knob;
    if (knob >= 4) {                   /* SHIFT layer: the map's settings */
        if (s < 0)
            return;
        int m = c2_mode_map(c2_mode());
        if (knob == 4) {
            int md = c2_clampi(c2.d.mode[s] + steps, 0, C2_MODE_COUNT - 1);
            if (md != c2.d.mode[s]) {
                c2_snapshot(knob + 1);
                c2.d.mode[s] = (uint8_t)md;
                c2_touch();
            }
            c2_flash(MSG_MODE, md);
        } else if (m < 0) {
            c2_flash(MSG_TURN_GREEN, 0);
        } else if (knob == 5) {        /* STRUM: down 300 ms .. OFF .. up 300 ms (c2_strum_ms); a wall at OFF */
            int was = c2.d.set[m].strum, v = c2_clampi(was + steps, -C2_STRUM_MAX, C2_STRUM_MAX);
            if (was && (was > 0 ? v <= 0 : v >= 0)) {
                v = 0;                 /* a spin from up to down stops at OFF ... */
                c2.strum_wall = c2_now() + 1;
            } else if (!was && v && c2.strum_wall && c2_now() + 1 - c2.strum_wall < 10)
                v = 0;                 /* ... and rests there ~0.3 s before it goes on */
            if (v != c2.d.set[m].strum) {
                c2_snapshot(knob + 1);
                c2.d.set[m].strum = (int8_t)v;
                c2_touch();
            }
            c2_flash(v && c2.seq_live < C2_LIVE ? MSG_NO_TICK : MSG_STRUM, v);
            c2.knob_frame = c2_now();
            c2.turned = knob;
            return;
        } else if (knob == 6) {        /* SMOOTH on (right) / off (left); right again: start the chain over */
            if (steps > 0 && c2.d.set[m].smooth) {
                c2.smooth_n = 0;
                c2_flash(MSG_SMOOTH_RESTART, 0);
                c2.knob_frame = c2_now();
                return;
            }
            if ((steps > 0) != (c2.d.set[m].smooth != 0)) {
                c2_snapshot(knob + 1);
                c2.d.set[m].smooth = steps > 0;
                c2.smooth_n = 0;
                c2_touch();
            }
            c2_flash(MSG_SMOOTH, c2.d.set[m].smooth);
        } else if ((steps > 0) != (c2.d.set[m].bass != 0)) {
            c2_snapshot(knob + 1);
            c2.d.set[m].bass = steps > 0;
            c2_touch();
            c2_revoice_all();
            c2_flash(MSG_BASS, steps > 0);
        }
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
        c2_snapshot(knob + 1);                /* only a real change replaces the undo snapshot */
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
#define C2_FILE_VERSION 2              /* 2: VOICING bipolar (-4..4) where 1 had a family 0..3; 1 is migrated */
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
    *p++ = (uint8_t)c->voi;
    *p++ = (uint8_t)c->top;
    for (int i = 0; i < C2_MAX; i++)
        *p++ = (uint8_t)c->iv[i];
    *p++ = c->n;
    *p++ = c->has_slash;
    *p++ = (uint8_t)c->slash;
    *p++ = c->flags;
    return p;
}

static const uint8_t *c2_get_chord(const uint8_t *p, struct c2_chord *c)
{
    c->root = (int8_t)*p++;
    c->q = *p++;
    c->voi = (int8_t)*p++;
    c->top = (int8_t)*p++;
    for (int i = 0; i < C2_MAX; i++)
        c->iv[i] = (int8_t)*p++;
    c->n = *p++;
    c->has_slash = *p++;
    c->slash = (int8_t)*p++;
    c->flags = *p++;
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
    c->voi = (int8_t)c2_clampi(c->voi, C2_VOI_MIN, C2_VOI_MAX);
    c->top = (int8_t)c2_clampi(c->top, -8, 8);
    for (int i = 0; i < C2_MAX; i++)
        c->iv[i] = (int8_t)c2_clampi(c->iv[i], -C2_IV_MAX, C2_IV_MAX);
    if (c->q != Q_CUSTOM)
        c->n = 0;
    c->has_slash = c->has_slash != 0;
    c->flags &= C2_ANCHOR;
    c->slash = (int8_t)c2_clampi(c->slash, -60, 60);
    if (c->n > C2_MAX)
        c->n = C2_MAX;
    c2_voi_fix(c);
    if (!c2_in_range(c, 1)) {
        c->top = 0;
        c->voi = 0;
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
        s->strum = (int8_t)c2_clampi(s->strum, -C2_STRUM_MAX, C2_STRUM_MAX);
        int end = 0;
        for (int i = 0; i < 12; i++) {
            char ch = s->name[i];
            end |= ch == 0 || i == 11;
            if (end)
                s->name[i] = 0;
            else if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-' || ch == '#' ||
                       ch == '&' || ch == '+'))
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
        *p++ = (uint8_t)d->set[m].strum;
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

/* The file's version if buf[0..n) is a valid file (1 or 2), else 0; *seq its sequence number. */
static int c2_check(const uint8_t *buf, int n, uint32_t *seq)
{
    static const char magic[8] = { 'O', 'V', 'D', 'B', 'C', 'H', 'R', 'D' };
    if (n != C2_FILE)
        return 0;
    for (int i = 0; i < 8; i++)
        if (buf[i] != (uint8_t)magic[i])
            return 0;
    uint32_t ver = c2_get16(buf + 8);
    if ((ver != 1 && ver != C2_FILE_VERSION) || c2_get16(buf + 10) != C2_HDR || c2_get32(buf + 16) != C2_PAYLOAD)
        return 0;
    if (c2_crc32(buf + C2_HDR, C2_PAYLOAD) != c2_get32(buf + 20))
        return 0;
    *seq = c2_get32(buf + 12);
    return (int)ver;
}

/* Version 1 stored a VOICING family 0..3 (CLOSE, DROP 2, DROP 3, SPREAD) where version 2 has the bipolar position:
 * each lands where it sounds the same. 4+ notes: 0 -1 -2 -3 (v1 SPREAD = DROP 2+4); triads: 0 -1 -1 -4 (v1's triad
 * DROP 3 played DROP 2, its SPREAD was DROP 2+3); a power chord: any v1 drop (the lower note down an octave) = +1 OCT
 * (the same interval, an octave higher). Learned chords ignored the family: CLOSE (= as played). */
static void c2_migrate_v1(struct c2_chord *c)
{
    static const int8_t four[4] = { 0, -1, -2, -3 }, three[4] = { 0, -1, -1, -4 };
    int f = (uint8_t)c->voi;
    f = f < 4 ? f : 0;
    if (c->q == Q_NONE || c->q == Q_CUSTOM || c->q >= Q_FIRST_WIDE)
        c->voi = 0;
    else if (c2_q[c->q].n >= 4)
        c->voi = four[f];
    else if (c2_q[c->q].n == 3)
        c->voi = three[f];
    else
        c->voi = (int8_t)(f != 0);
}

static void c2_parse(const uint8_t *buf, struct c2_data *d, int ver)
{
    const uint8_t *p = buf + C2_HDR;
    for (int m = 0; m < C2_MAPS; m++)
        for (int k = 0; k < C2_KEYS; k++)
            p = c2_get_chord(p, &d->maps[m][k]);
    if (ver == 1)                      /* the test outside the loops (src/slot/test/loopcc.py) */
        for (int i = 0; i < C2_MAPS * C2_KEYS; i++)
            c2_migrate_v1(&d->maps[0][0] + i);
    for (int m = 0; m < C2_MAPS; m++) {
        d->set[m].bass = *p++;
        d->set[m].smooth = *p++;
        d->set[m].flats = *p++;
        d->set[m].strum = (int8_t)*p++;
        for (int i = 0; i < 12; i++)
            d->set[m].name[i] = (char)*p++;
    }
    for (int s = 0; s < C2_SLOTS; s++)
        p = c2_get_chord(p, &d->shape[s]);
    if (ver == 1)
        for (int s = 0; s < C2_SLOTS; s++)
            c2_migrate_v1(&d->shape[s]);
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
    int ver = n >= 0 ? c2_check(c2_buf, n, &seq) : 0;
    if (ver) {
        c2_parse(c2_buf, &c2.d, ver);
        c2.dg.load_ver = ver;
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
    c2_seq_watch();
    uint32_t now = c2_now();
    if (c2.wr_open && now + 1 - c2.wr_frame > C2_WR_IDLE)
        c2_wr_close();                 /* WRITE left alone ~10 s: closed, so a later DROP pastes as usual */
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
