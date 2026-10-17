/* CHORD v2 (notes/chord2.md): the chord model and the state shared by the engine (hold.c, chord2_impl.h) and the
 * screens (ui.c, ui_chord2.h). Device code: no division, no floats, no libc. Included by both objects, so every
 * function here is static (C2_FN) and the state is one global, `c2`, defined in hold.c.
 *
 * A chord on a key = root (semitones from the key) + quality + TOP (a ladder: each step puts the next chord tone on
 * top, across octaves) + VOICING (bipolar: drops left, ups right), or a learned chord kept as played (Q_CUSTOM, TOP
 * and VOICING then transform the notes as played) until TYPE is turned. Q_NONE (0, so zeroed memory means "plays its note") = no chord.
 * The qualities are listed in the order the TYPE knob scans them (Model:Cycles style, notes/chord2.md "TYPE scan"):
 * the everyday triads and sevenths first, then sixths, the open (no third) group, the colour chords, the altered ones;
 * turning left past the first one (two detents: a wall) unmaps the key.
 */
#ifndef CHORD2_H
#define CHORD2_H
#include <stdint.h>

#define C2_FN static __attribute__((unused))
#define C2_MAX 4                    /* chord tones (6 synth voices); + 1 optional bass note */
#define C2_NOTES (C2_MAX + 1)
#define C2_KEYS 24
#define C2_MAPS 8
#define C2_SLOTS 8
#define C2_VOICES 6                 /* TE's poly voices (VoiceCtl.cpp:178) */
#define C2_REL_OFS 36               /* relative note index = key + interval + 36 (hold_impl.h REL_OFS) */
#define C2_IV_MAX 35                /* intervals from the key stay within +-35 (the player's relative range) */

enum { Q_NONE, Q_MAJ, Q_MIN, Q_MAJ7, Q_7, Q_MIN7, Q_HDIM, Q_DIM, Q_DIM7, Q_6, Q_MIN6, Q_MINMAJ7, Q_SUS4, Q_SUS2, Q_5,
       Q_7SUS4, Q_9SUS4, Q_ADD9, Q_69, Q_MAJ9, Q_9, Q_MIN9, Q_MIN11, Q_13, Q_7S11, Q_7B9, Q_7S9, Q_7B13, Q_AUG,
       /* the WIDE group (phase 2): two-note intervals, named by their real distance (c2_dyad_name); appended so the
        * file's quality codes stay; the TYPE scan reaches them LEFT of NOTE (c2_scan_q) */
       Q_P8, Q_M10, Q_MIN10, Q_MIN7D, Q_MAJ7D, Q_P12, Q_M9D, Q_M13D, Q_TT,
       Q_COUNT, Q_DYAD = 0x7D, Q_CUSTOM = 0x7E };
#define Q_FIRST_WIDE Q_P8

struct c2_quality {
    int8_t iv[C2_MAX];              /* close position, ascending, from the root (rootless voicings start above it) */
    uint8_t n;
    uint8_t rootless;
    char name[6];                   /* symbol: Real Book ('-' minor, '^' maj7, '*' dim, '$' half-dim, '&' flat) */
    char tag[5];                    /* on a key: <= 3 glyphs */
    char longname[13];              /* spelled out, for the big title when it fits */
};

static const struct c2_quality c2_q[Q_COUNT] = {
    { { 0 }, 0, 0, "", "", "" },                                /* Q_NONE */
    { { 0, 4, 7 }, 3, 0, "", "", "MAJ" },
    { { 0, 3, 7 }, 3, 0, "-", "-", "MIN" },
    { { 0, 4, 7, 11 }, 4, 0, "^7", "^7", "MAJ7" },
    { { 0, 4, 7, 10 }, 4, 0, "7", "7", "7" },
    { { 0, 3, 7, 10 }, 4, 0, "-7", "-7", "MIN7" },
    { { 0, 3, 6, 10 }, 4, 0, "$7", "$", "MIN7&5" },
    { { 0, 3, 6 }, 3, 0, "*", "*", "DIM" },
    { { 0, 3, 6, 9 }, 4, 0, "*7", "*7", "DIM7" },
    { { 0, 4, 7, 9 }, 4, 0, "6", "6", "6" },
    { { 0, 3, 7, 9 }, 4, 0, "-6", "-6", "MIN6" },
    { { 0, 3, 7, 11 }, 4, 0, "-^7", "-^", "MIN^7" },
    { { 0, 5, 7 }, 3, 0, "SUS4", "S4", "SUS4" },
    { { 0, 2, 7 }, 3, 0, "SUS2", "S2", "SUS2" },
    { { 0, 7 }, 2, 0, "5", "5", "5" },
    { { 0, 5, 7, 10 }, 4, 0, "7SUS4", "7S4", "7SUS4" },
    { { 5, 7, 10, 14 }, 4, 1, "9SUS4", "9S4", "9SUS4" },
    { { 0, 4, 7, 14 }, 4, 0, "ADD9", "(9)", "ADD9" },
    { { 0, 4, 9, 14 }, 4, 0, "6/9", "69", "6/9" },
    { { 4, 7, 11, 14 }, 4, 1, "^9", "^9", "MAJ9" },
    { { 4, 7, 10, 14 }, 4, 1, "9", "9", "9" },
    { { 3, 7, 10, 14 }, 4, 1, "-9", "-9", "MIN9" },
    { { 3, 10, 14, 17 }, 4, 1, "-11", "-11", "MIN11" },
    { { 4, 10, 14, 21 }, 4, 1, "13", "13", "13" },
    { { 0, 4, 10, 18 }, 4, 0, "7#11", "#11", "7#11" },
    { { 4, 7, 10, 13 }, 4, 1, "7&9", "7&9", "7&9" },
    { { 0, 4, 10, 15 }, 4, 0, "7#9", "7#9", "7#9" },
    { { 4, 10, 14, 20 }, 4, 1, "7&13", "&13", "7&13" },
    { { 0, 4, 8 }, 3, 0, "+", "+", "AUG" },
    /* WIDE: P perfect, M major, m minor ('~' draws a lower-case m: never in a chord symbol), TT tritone; the
     * title spells them out. Root + 7th: the left-hand shells. */
    { { 0, 12 }, 2, 0, "P8", "P8", "OCTAVE" },
    { { 0, 16 }, 2, 0, "M10", "M10", "MAJOR 10TH" },
    { { 0, 15 }, 2, 0, "~10", "~10", "MINOR 10TH" },
    { { 0, 10 }, 2, 0, "~7", "~7", "MINOR 7TH" },
    { { 0, 11 }, 2, 0, "M7", "M7", "MAJOR 7TH" },
    { { 0, 19 }, 2, 0, "P12", "P12", "PERFECT 12TH" },
    { { 0, 14 }, 2, 0, "M9", "M9", "MAJOR 9TH" },
    { { 0, 21 }, 2, 0, "M13", "M13", "MAJOR 13TH" },
    { { 0, 6 }, 2, 0, "TT", "TT", "TRITONE" },
};

C2_FN int c2_is_wide(int q) { return q >= Q_FIRST_WIDE && q < Q_COUNT; }

/* The TYPE scan (Model:Cycles style): position 0 = NOTE (the key plays its note), 1..28 = the chord list in enum
 * order (unchanged since the MVP, so learned detent counts still hold), -1..-9 = the WIDE group, LEFT of NOTE:
 * octave, the 10ths, the 7th shells, the 12th, then the compound 9th, 13th and the tritone. NOTE sits between with a wall on each
 * side, so a spin either way stops before it. */
#define C2_SCAN_MIN (Q_FIRST_WIDE - Q_COUNT)     /* -9 */
#define C2_SCAN_MAX (Q_FIRST_WIDE - 1)           /* 28 = AUG */
C2_FN int c2_scan_pos(int q)
{
    if (c2_is_wide(q))
        return Q_FIRST_WIDE - 1 - q;             /* Q_P8 -> -1 ... Q_TT -> -7 */
    return q > Q_NONE && q < Q_FIRST_WIDE ? q : 0;
}
C2_FN int c2_scan_q(int pos)
{
    if (pos < 0)
        return pos < C2_SCAN_MIN ? Q_TT : Q_FIRST_WIDE - 1 - pos;
    return pos > C2_SCAN_MAX ? Q_AUG : pos;
}

/* VOICING (orange) is bipolar, -4..+4; 0 = the base voicing (close for a scanned chord, exactly as played for a
 * learned one). Left (drops) moves notes below the top DOWN an octave and never changes the top note: DROP 2, DROP 3,
 * DROP 2+4, SPREAD (DROP 2+4, then the lowest down again). Right mirrors it (ups) and never changes the bass note:
 * UP 2 (the 2nd from the bottom up an octave), UP 3, UP 2+4, SPREAD UP. A triad has no 2+4 (skipped), its SPREAD is
 * DROP 2+3 / UP 2+3. A two-note chord keeps its bass and moves its upper note: -2..+2 octaves. */
#define C2_VOI_MIN -4
#define C2_VOI_MAX 4
static const char *const c2_voi_name[9] = { "SPREAD", "DROP 2+4", "DROP 3", "DROP 2", "CLOSE", "UP 2", "UP 3",
                                            "UP 2+4", "SPREAD UP" };
static const char *const c2_voi_dyad[5] = { "-2 OCT", "-1 OCT", "CLOSE", "+1 OCT", "+2 OCT" };

/* Is VOICING position v defined for a chord of n (upper) notes? */
C2_FN int c2_voi_ok(int n, int v)
{
    if (v == 0)
        return 1;
    if (v < C2_VOI_MIN || v > C2_VOI_MAX || n < 2)
        return 0;
    if (n == 2)
        return v >= -2 && v <= 2;
    if (n == 3)
        return v != 3 && v != -3;
    return 1;
}

/* The VOICING name for position v of an n-note chord (base: "CLOSE" or "AS PLAYED"). */
C2_FN const char *c2_voi_label(int n, int v, const char *base)
{
    if (v == 0)
        return base;
    if (n == 2)
        return v >= -2 && v <= 2 ? c2_voi_dyad[v + 2] : base;
    return v >= C2_VOI_MIN && v <= C2_VOI_MAX ? c2_voi_name[v + 4] : base;
}

/* 12 bytes; the file stores these fields in this order (chord2_impl.h c2_put_chord). */
struct c2_chord {
    int8_t root;                    /* semitones from the key */
    uint8_t q;                      /* Q_*, Q_CUSTOM (learned, iv[] as played) or Q_NONE (the key plays its note) */
    int8_t voi;                     /* VOICING -4..4 (0 = close / as played); file v1 stored a family 0..3 here */
    int8_t top;                     /* ladder: 0 = root position (learned: as played); +1 = next chord tone on top */
    int8_t iv[C2_MAX];              /* Q_CUSTOM: from the key, ascending */
    uint8_t n;
    uint8_t has_slash;              /* slash bass kept from a learned chord whose lowest note is not its root */
    int8_t slash;                   /* semitones from the key */
    uint8_t flags;                  /* C2_ANCHOR: a TOP turned with SMOOTH on (SMOOTH plays this TOP exactly) */
};
#define C2_ANCHOR 1

/* Per map settings: strum = a signed index into c2_strum_ms (the whole spread; + up: low to high, - down), 0 off. */
struct c2_set {
    uint8_t bass, smooth, flats;
    int8_t strum;
    char name[12];
};

/* Everything that persists (the file, chord2_impl.h), and what undo snapshots. */
struct c2_data {
    struct c2_chord maps[C2_MAPS][C2_KEYS];
    struct c2_set set[C2_MAPS];
    struct c2_chord shape[C2_SLOTS];      /* SHAPE mode: one chord on every key, per synth slot */
    uint8_t mode[C2_SLOTS];               /* per synth slot: 0 OFF, 1 SHAPE, 2..9 MAP 1..8 */
};
#define C2_MODE_COUNT (2 + C2_MAPS)

/* Diagnostic counters, shown on the CHORD page while SHIFT is held (the morning test doubles as CHORDPROBE). */
struct c2_diag {
    uint32_t frames;                /* frame hook calls (GUI thread, after TE's render) */
    uint32_t fb_frames;             /* page or badge drawn from our engine screen instead (no frame hook) */
    uint32_t ctl_synth;             /* frames on which the current controller was the synth one */
    uint32_t knobs;                 /* encoder turns our wrapper swallowed (page open) */
    int32_t knob_delta, knob_dmin;  /* last raw delta, the smallest |delta| seen */
    uint32_t split, lift, drop;     /* SPLIT / LIFT / DROP presses our press wrapper saw */
    uint32_t shift_up;              /* SHIFT releases our release wrapper saw */
    uint32_t sheds;                 /* notes or chords the voice budget let go */
    uint32_t shift_dn;              /* SHIFT presses our press wrapper saw */
    uint32_t purges;                /* tracked keys released because their key-up could no longer come */
    int32_t voices;                 /* sounding notes we track (after the last key event) */
    int32_t load;                   /* C2_LOAD_* */
    int32_t load_seq;
    int32_t load_ver;               /* the loaded file's version (1 = migrated) */
    uint32_t saves;
    int32_t save_err;               /* C2_SAVE_*: the last save's result */
    int32_t mkdir_ret;
    /* learn (the third diagnostic line, LEARN ...): why a learn started, ended or never started */
    uint32_t learn_starts;          /* first SHIFT + note of a learn */
    uint32_t learn_ok;              /* written (LEARNED / MAX) */
    uint32_t learn_cancels;         /* a SHIFT combo (LIFT, DROP, SPLIT, T2) during a learn */
    uint32_t learn_blocked;         /* SHIFT + note that did not learn (after a combo, or MAP off the page) */
    uint32_t learn_end;             /* the last end / block reason: C2_LE_* */
    int32_t combo_key;              /* the last key that made this SHIFT press a combo (0x100 + knob), -1 none */
    /* the sequencer-thread tick (STRUM): the fourth diagnostic line, SEQ ... */
    uint32_t tick_rate;             /* ticks in the last ~1 s window */
    uint32_t tick_gap;              /* the longest gap between two ticks in that window (c2_time units) */
    uint32_t strum_sent;            /* note-ons the tick sent */
    uint32_t strum_forced;          /* ... of which sent by the stale guard (due never reached) */
    uint32_t takeovers;             /* times the KeyHandler took the notes back from a dead tick */
    uint32_t smooth_resets;         /* SMOOTH starting over (2 bars of silence, HOLD off) */
};
/* learn end reasons: '-' none, U SHIFT up (release slot), N a plain note (fallback), P a key press without SHIFT
 * (fallback), C cancelled by a combo, E nothing to write (no notes, no slot), B blocked (SHIFT already used for a
 * combo), M blocked (MAP mode with the page closed: SHIFT + notes just play) */
enum { C2_LE_NONE = '-', C2_LE_UP = 'U', C2_LE_NOTE = 'N', C2_LE_PRESS = 'P', C2_LE_CANCEL = 'C', C2_LE_EMPTY = 'E',
       C2_LE_BLOCKED = 'B', C2_LE_MAP = 'M' };
enum { C2_LOAD_PENDING, C2_LOAD_NONE, C2_LOAD_A, C2_LOAD_B, C2_LOAD_BAD };
enum { C2_SAVE_NONE, C2_SAVE_OK, C2_SAVE_OPEN, C2_SAVE_WRITE, C2_SAVE_CLOSE, C2_SAVE_TORN };

enum { MSG_NONE, MSG_CLEARED, MSG_COPIED, MSG_PASTED, MSG_UNDONE, MSG_LEARNED, MSG_MAX4, MSG_EMPTY_CLIP, MSG_LATER,
       MSG_TURN_GREEN, MSG_NOTHING_UNDO, MSG_MODE, MSG_BASS, MSG_REDONE, MSG_PINNED, MSG_UNPINNED, MSG_WALL, MSG_LEARN_CANCEL, MSG_GREEN_FIRST,
       MSG_FACTORY_ASK, MSG_FACTORY_DONE, MSG_FACTORY_NOMAP, MSG_STRUM, MSG_SMOOTH, MSG_NO_TICK, MSG_WRITTEN,
       MSG_OVERWRITE, MSG_WRITE_CANCEL, MSG_VOI_END, MSG_VOI_HINT, MSG_STRUM_WALL, MSG_SMOOTH_RESTART, MSG_SHIFT_UP,
       MSG_ANCHOR, MSG_COUNT };

/* STRUM's whole spread by detent: fine steps where a strum is a flam, coarse where it is a roll. */
#define C2_STRUM_MAX 19
static const uint16_t c2_strum_ms[C2_STRUM_MAX + 1] = { 0, 5, 10, 15, 20, 25, 30, 35, 40, 50, 60, 70, 80, 90, 100, 120,
                                                        150, 200, 250, 300 };
C2_FN int c2_strum_spread(int st) { st = st < 0 ? -st : st; return c2_strum_ms[st > C2_STRUM_MAX ? C2_STRUM_MAX : st]; }

/* WRITE (SHIFT+DROP): what to write into a map. */
enum { W_FACTORY, W_SEVENTHS, W_TRIADS, W_COPY, W_EMPTY, W_LAST, W_COUNT };   /* W_LAST: the map the last WRITE replaced */

#define C2_LEARN_MAX 8
#define C2_NOTES_SHOW 30            /* frames (~1 s) the keyboard keeps the notes view after a chord knob turn */
#define C2_SCAN_SHOW 128            /* frames (~4 s) the TYPE scan strip stays after a green turn */
#define C2_GROUP 64                 /* frames (~2 s): turns of the same knob within this are one undo gesture */
#define C2_FLASH 72                 /* frames a message shows */
#define C2_TAP 15                   /* frames (~0.5 s): a SPLIT held longer does not pin */
#define C2_ARM 72                   /* frames (~2.4 s) OVERWRITE? waits for DROP */
#define C2_WR_IDLE 320              /* frames (~10 s) without a WRITE knob or key: WRITE closes */
#define C2_PLAYED_SHOW 15           /* frames (~0.5 s) the played-notes view stays after the last key-up */

/* The whole state. Writers: the KeyHandler thread (keys, knobs: every edit) and the GUI thread (the file load,
 * once, while `loading` is set; closing the page). Readers: both (the GUI draws from it, the save serialises it,
 * checked against `gen`). */
struct c2_state {
    struct c2_data d;
    struct c2_data undo;            /* the state before the last edit gesture (undo swaps the two) */
    uint32_t undo_ok;
    struct c2_chord clip;           /* LIFT copies, DROP pastes */
    volatile uint32_t gen;          /* bumped at the start and the end of every edit of d (the save checks it) */
    uint32_t edit_frame;            /* frame of the last edit (save 2 s after) */
    uint32_t knob_frame;            /* frame of the last knob turn (undo grouping, the scan strip) */
    volatile uint32_t loading;      /* GUI copying a loaded file into d: the keys play plain meanwhile */
    uint32_t loaded;
    uint32_t load_tries;            /* loads that found no file at all (the disk may not be up yet) */
    uint32_t learn_frame;           /* when the learn started */
    /* page */
    volatile uint32_t page;         /* CHORD page open */
    int32_t sel;                    /* edited key 0..23 */
    int32_t turned;                 /* last knob turned 0..7, -1 none */
    uint32_t split_down;            /* SPLIT held (page open): the next note key clears instead of playing */
    uint32_t msg, msg_frame;        /* a message and when it was set */
    int32_t msg_arg;
    /* learn (SHIFT + notes) */
    uint32_t learning;
    int32_t learn_n, learn_over, learn_base;  /* notes, more than C2_LEARN_MAX, note number of the target key */
    int32_t learn_note[C2_LEARN_MAX];         /* note numbers as played */
    int32_t learn_key[C2_LEARN_MAX];          /* their keys (still down at commit = kept) */
    int32_t learn_page;                        /* started with the page open */
    /* playing (our voice tracking, by key) */
    uint8_t key_cnt[C2_KEYS];       /* relative notes this key sounds (0 = none tracked) */
    uint8_t key_kind[C2_KEYS];      /* 0 none, 1 plain note, 2 chord */
    int8_t key_bass[C2_KEYS];       /* index of its BASS note in key_rel, -1 none */
    uint8_t key_rel[C2_KEYS][C2_NOTES];
    uint32_t key_age[C2_KEYS];
    uint32_t age;
    uint32_t dead;                  /* keys down whose key-up must be ignored (shed by the budget, SPLIT-cleared) */
    uint32_t chord_down;            /* chord keys physically down (HOLD gestures) */
    uint8_t rel_ref[96];            /* sounding notes by relative index (key + interval + 36), reference counted */
    int32_t last_key;               /* last chord key played (badge, chip) */
    uint32_t last_frame;
    uint32_t sounding;              /* keys sounding a chord (bitmask, for the screens) */
    int32_t last_slot;              /* slot the last chord was played in */
    /* file */
    int32_t file_slot;              /* 0 A, 1 B: the file loaded or written last, -1 none */
    uint32_t file_seq;
    uint32_t saved_gen;
    uint32_t save_tries;
    uint32_t save_frame;
    uint32_t fail_gen;              /* gen the last failed save tried */
    int32_t last_tap;               /* the last key pressed on the page (the edited key, unless pinned) */
    uint32_t pinned;                /* SPLIT tapped alone: the edited key stays until SPLIT is tapped again or the page
                                     * closes; unpinned, the edited key is always the last key pressed */
    uint32_t split_used;            /* a key was cleared while SPLIT was down (so its release does not pin) */
    uint32_t split_frame;           /* clock when SPLIT went down: only a short tap pins */
    uint32_t shift_used;            /* this SHIFT press already did a combo (SHIFT+T2, +SPLIT, +knob ...): no learn */
    uint32_t wall_frame;            /* TYPE at the first quality: the frame the wall was hit (a 2nd detent = NOTE) */
    uint32_t redo;                  /* the last undo was itself undone: the next one says REDONE */
    uint32_t gesture_knob;          /* the undo snapshot was taken for a knob gesture */
    uint32_t restore_frame;         /* SHIFT+DROP armed RESTORE FACTORY? at this clock (0 = not armed) */
    int32_t restore_map;            /* ... for this map */
    volatile uint32_t clock;        /* frames: the frame hook's (or our engine screen's, without it) */
    uint32_t ui_frame;              /* clock when ui_draw (our engine screen) last drew */
    uint32_t ui_hook_seen;          /* dg.frames when ui_draw last ran (frame hook alive if it moved) */
    struct c2_diag dg;
    /* played-notes view (page): what sounds, kept until the next change that leaves something sounding */
    uint32_t view_rel[3];           /* bit r: relative note r (key + interval + REL_OFS) */
    uint32_t view_roots;            /* pitch classes (key units, mod 12) that are a sounding chord's root */
    uint32_t keys_held;             /* keys physically down that we track */
    uint32_t release_frame;         /* clock of the last key-up */
    /* SMOOTH */
    int8_t smooth_prev[C2_MAX];     /* the last chord's upper notes, key-relative (key + interval) */
    int32_t smooth_n;               /* 0 = start over (the chord's own TOP) */
    int32_t smooth_map;
    uint32_t silent_since;          /* c2_time when the last chord stopped sounding */
    /* STRUM: our notes go through the sequencer thread while a map strums (chord2_impl.h "STRUM") */
    volatile uint32_t owner;        /* 0 the KeyHandler sends our notes, 1 the tick does, 2 the tick hands them back */
    volatile uint32_t due[96];      /* c2_time a wanted note may start */
    volatile uint32_t off_due[96];  /* c2_time a released note may stop (a legato strum hands voices over one for one) */
    volatile uint8_t onset[96];     /* bumped at each new onset (a retrigger) */
    volatile uint8_t raw_off[96];   /* an untracked key-up the tick must send */
    volatile uint32_t seq_reset;    /* play mode switched: the tick forgets what it sent */
    uint8_t on[96];                 /* sent and sounding (tick-owned while owner != 0) */
    uint8_t seen[96];               /* onset at the last onset sent */
    uint8_t wait[96];               /* ticks a wanted note has waited (the stale guard) */
    volatile uint32_t seq_ticks;    /* tick count (the GUI checks it moves) */
    uint32_t seq_seen, seq_live, seq_idle;   /* GUI: frames the tick moved / did not */
    uint32_t tick_t0, tick_n, tick_last, tick_gap;   /* the tick's own 1 s window */
    /* WRITE */
    uint32_t wr_open;
    int32_t wr_from, wr_key, wr_scale, wr_src, wr_shift, wr_fac, wr_to;   /* knobs: FROM, KEY / TRANSPOSE / WHICH, SCALE / MAP, TO */
    uint32_t wr_touched;            /* a knob turned since SHIFT+DROP (the plain factory restore needs no 2nd DROP) */
    uint32_t wr_open_frame;         /* clock when WRITE opened (the plain restore: within C2_ARM of it) */
    uint32_t wr_frame;              /* clock of the last WRITE knob / key (idle ~10 s: WRITE closes) */
    int32_t smooth_key, smooth_top; /* the key SMOOTH last voiced and the TOP it played (the TOP readout) */
    uint32_t voi_hint;              /* the first VOICING turn right since boot said where the drops went */
    uint32_t strum_wall;            /* clock when a STRUM turn stopped at OFF (a rest, then it crosses) */
    uint32_t wr_arm;                /* OVERWRITE? armed at this clock + 1 (0 = not) */
    struct c2_chord wr_map[C2_KEYS];   /* the preview (keys audition it) */
    struct c2_chord wr_last[C2_KEYS];  /* what the last WRITE replaced (FROM: OLD MAP n) */
    struct c2_set wr_last_set;
    int32_t wr_last_map;               /* its map, -1 none */
    struct c2_set wr_set;
};

/* ---- the model ---- */

C2_FN int c2_mod12(int x)           /* no % : no libgcc */
{
    while (x < 0)
        x += 12;
    while (x >= 12)
        x -= 12;
    return x;
}

static const char *const c2_pc_sharp[12] = { "C", "C#", "D", "E&", "E", "F", "F#", "G", "A&", "A", "B&", "B" };
static const char *const c2_pc_flat[12] = { "C", "D&", "D", "E&", "E", "F", "G&", "G", "A&", "A", "B&", "B" };

/* The OP-1's 24 keys start on F (FUN_01a85014: note = key + 0x35 + 12 * octave). */
C2_FN int c2_key_pc(int key) { return c2_mod12(key + 5); }

C2_FN void c2_sort(int8_t *v, int n)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            int8_t t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
}

C2_FN int c2_mask(const int8_t *iv, int n, int base)
{
    int m = 0;
    for (int i = 0; i < n; i++)
        m |= 1 << c2_mod12(iv[i] + base);
    return m;
}

C2_FN int c2_popcount12(int m)
{
    int c = 0;
    for (int i = 0; i < 12; i++)
        c += (m >> i) & 1;
    return c;
}

/* TOP: each step puts the next chord tone on top (the lowest note up an octave until a new note is on top; negative
 * steps move the top note down). A WIDE interval moves whole by octaves instead, so it stays the interval it names. */
C2_FN void c2_ladder(int8_t *out, int n, int top, int wide)
{
    if (n < 2)
        return;
    if (wide) {
        for (int i = 0; i < n; i++)
            out[i] = (int8_t)(out[i] + 12 * top);
        return;
    }
    for (; top > 0; top--) {
        int t0 = out[n - 1];
        for (int g = 0; g < n && out[n - 1] == t0; g++) {
            out[0] += 12;
            c2_sort(out, n);
        }
    }
    for (; top < 0; top++) {
        out[n - 1] -= 12;
        c2_sort(out, n);
    }
}

/* VOICING position v on ascending notes out[n] (chord2.h c2_voi_name): drops keep the top note, ups keep the bass, a
 * two-note chord moves its upper note by octaves (and must keep it above the bass). Returns 0 if v does not apply
 * (out unchanged). */
C2_FN int c2_voicing(int8_t *out, int n, int v)
{
    if (v == 0)
        return 1;
    if (!c2_voi_ok(n, v))
        return 0;
    if (n == 2) {
        int u = out[1] + 12 * v;
        if (u <= out[0])
            return 0;
        out[1] = (int8_t)u;
        return 1;
    }
    if (v < 0) {
        if (v == -1)
            out[n - 2] -= 12;
        else if (v == -2)
            out[n - 3] -= 12;
        else if (n == 3) {         /* a triad's SPREAD: DROP 2+3 (the MVP's triad SPREAD) */
            out[1] -= 12;
            out[0] -= 12;
        } else {
            out[n - 2] -= 12;      /* DROP 2+4 */
            out[n - 4] -= 12;
            if (v == -4) {         /* SPREAD: and the lowest down again */
                c2_sort(out, n);
                out[0] -= 12;
            }
        }
    } else {
        if (v == 1)
            out[1] += 12;
        else if (v == 2)
            out[2] += 12;
        else if (n == 3) {         /* SPREAD UP of a triad: UP 2+3 */
            out[1] += 12;
            out[2] += 12;
        } else {
            out[1] += 12;          /* UP 2+4 */
            out[3] += 12;
            if (v == 4) {          /* SPREAD UP: and the highest up again */
                c2_sort(out, n);
                out[n - 1] += 12;
            }
        }
    }
    c2_sort(out, n);
    return 1;
}

/* A structured quality voiced: TOP, then VOICING. Intervals from the root, ascending. */
C2_FN int c2_voice(int q, int voi, int top, int8_t *out)
{
    int n = c2_q[q].n;
    for (int i = 0; i < n; i++)
        out[i] = c2_q[q].iv[i];
    c2_ladder(out, n, top, c2_is_wide(q));
    c2_voicing(out, n, voi);
    return n;
}

C2_FN int c2_valid_q(int q) { return q == Q_CUSTOM || (q > Q_NONE && q < Q_COUNT); }

/* The chord's upper notes (no added bass), from the key, ascending, with TOP = `top`: a learned chord starts from
 * exactly what was played, and TOP / VOICING transform that (0 / 0 = as played). Returns the count. */
C2_FN int c2_upper_v(const struct c2_chord *c, int top, int voi, int8_t *out)
{
    if (c->q == Q_NONE || !c2_valid_q(c->q))
        return 0;
    if (c->q == Q_CUSTOM) {
        if (c->n < 1 || c->n > C2_MAX)
            return 0;
        int n = c->n;
        for (int i = 0; i < n; i++)
            out[i] = c->iv[i];
        c2_sort(out, n);
        /* a learned interval (two pitch classes or fewer) or a voicing wider than an octave moves whole by octaves:
         * TOP keeps what was played (the same as a WIDE quality) */
        int wide = c2_popcount12(c2_mask(out, n, 0)) <= 2 || out[n - 1] - out[0] > 12;
        c2_ladder(out, n, top, wide);
        c2_voicing(out, n, voi);
        return n;
    }
    int n = c2_voice(c->q, voi, top, out);
    for (int i = 0; i < n; i++)
        out[i] = (int8_t)(out[i] + c->root);
    return n;
}

C2_FN int c2_upper(const struct c2_chord *c, int top, int8_t *out) { return c2_upper_v(c, top, c->voi, out); }

/* The upper-note count VOICING works on (a learned chord: its notes as played; a scanned one: its quality's). */
C2_FN int c2_voi_n(const struct c2_chord *c)
{
    if (c->q == Q_CUSTOM)
        return c->n < 1 ? 1 : c->n > C2_MAX ? C2_MAX : c->n;
    return c->q > Q_NONE && c->q < Q_COUNT ? c2_q[c->q].n : 0;
}

/* Does VOICING v apply to c (its note count; a two-note chord's upper note stays above its bass)? */
C2_FN int c2_voi_applies(const struct c2_chord *c, int v)
{
    int8_t u[C2_NOTES];
    int n = c2_voi_n(c);
    if (!c2_voi_ok(n, v))
        return 0;
    if (n != 2 || v == 0)
        return 1;
    return c2_upper_v(c, c->top, 0, u) == 2 && c2_voicing(u, 2, v);
}

/* Intervals from the key, as the player plays them, ascending. bass 1 (the map's BASS) adds the root under the chord as
 * a 5th voice; bass 0 adds it only for a rootless quality (MAJ9, MIN11 ... keep their root, as on the Cycles); bass -1
 * neither (the raw voicing, for recognition). *bass_at = the index of that added root (shed first), else -1. A slash
 * bass (structured, or a learned chord's 5th note) is part of the chord. `top`: the TOP to use (SMOOTH picks one;
 * c2_notes passes the chord's own). Returns the count (0 = plays its note). Defensive: anything out of range (a torn
 * read while the GUI loads the file) gives 0. */
C2_FN int c2_notes_t(const struct c2_chord *c, int bass, int top, int8_t *out, int *bass_at)
{
    *bass_at = -1;
    int n = c2_upper(c, top, out);
    if (!n)
        return 0;
    int b = 0, add = 0;
    if (c->has_slash) {            /* G/B: B under the chord; a learned 5-note voicing: its bass */
        b = c->slash;
        add = 1;
    } else if (c->q != Q_CUSTOM && bass > 0) {   /* BASS: the root an octave under (learned chords: their own) */
        b = c->root - 12;
        add = 1;
    } else if (c->q != Q_CUSTOM && bass == 0 && c2_q[c->q].rootless) {
        b = c->root;               /* a rootless colour chord gets its root right under it */
        add = 1;
    }
    if (add) {
        int guard = 0;
        while (b >= out[0] && guard++ < 8)
            b -= 12;
        for (int i = n; i > 0; i--)
            out[i] = out[i - 1];
        out[0] = (int8_t)b;
        *bass_at = c->has_slash ? -1 : 0;   /* a slash bass is part of the chord, never shed alone */
        n++;
    }
    return n;
}

C2_FN int c2_notes(const struct c2_chord *c, int bass, int8_t *out, int *bass_at)
{
    return c2_notes_t(c, bass, c->top, out, bass_at);
}

/* Name a set of played notes (intervals from the key): the best (root, quality). Compares pitch-class sets, so
 * doubled notes don't matter. Prefers the root in the bass, then a voicing with the root, then the earlier quality.
 * The WIDE intervals are named by their distance instead (c2_dyad). Returns the quality or Q_CUSTOM. */
C2_FN int c2_recognise(const int8_t *iv, int n, int *root_semis)
{
    if (n < 1)
        return Q_CUSTOM;
    int want = c2_mask(iv, n, 0), best = -1, best_q = Q_CUSTOM;
    int low = iv[0];
    for (int i = 1; i < n; i++)
        low = iv[i] < low ? iv[i] : low;
    for (int q = 1; q < Q_FIRST_WIDE; q++) {
        if (c2_popcount12(c2_mask(c2_q[q].iv, c2_q[q].n, 0)) != c2_popcount12(want))
            continue;
        for (int r = 0; r < 12; r++) {
            if (c2_mask(c2_q[q].iv, c2_q[q].n, r) != want)
                continue;
            int s = (r == c2_mod12(low) ? 8 : 0) + (c2_q[q].rootless ? 0 : 4);   /* ties: the earlier quality */
            if (s > best) {
                best = s;
                best_q = q;
                *root_semis = r;
            }
        }
    }
    return best_q;
}

/* Two pitch classes or fewer (a dyad, with octave doublings, or one note in octaves): named as an interval by its
 * real distance, so C3 + E4 is a major 10th, not "C". *root = the lowest note; *dist = from it to the lowest note of
 * the other pitch class (one pitch class: to the highest note; a single note: 0). Returns 1 for a dyad. */
C2_FN int c2_dyad(const int8_t *iv, int n, int *root, int *dist)
{
    if (n < 1 || c2_popcount12(c2_mask(iv, n, 0)) > 2)
        return 0;
    int lo = iv[0], hi = iv[0];
    for (int i = 1; i < n; i++) {
        lo = iv[i] < lo ? iv[i] : lo;
        hi = iv[i] > hi ? iv[i] : hi;
    }
    int other = 127;
    for (int i = 0; i < n; i++)
        if (c2_mod12(iv[i] - lo) != 0 && iv[i] < other)
            other = iv[i];
    *root = lo;
    *dist = other == 127 ? hi - lo : other - lo;
    return 1;
}

/* The WIDE quality (or 5) a distance is, Q_CUSTOM if none. */
C2_FN int c2_dyad_q(int dist)
{
    if (dist == 7)
        return Q_5;
    for (int q = Q_FIRST_WIDE; q < Q_COUNT; q++)
        if (c2_q[q].iv[1] == dist)
            return q;
    return Q_CUSTOM;
}

C2_FN char *c2_puts(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

C2_FN char *c2_put2(char *p, int v)          /* 0..99, no division */
{
    int t = 0;
    while (v >= 10) {
        v -= 10;
        t++;
    }
    if (t)
        *p++ = (char)('0' + t);
    *p++ = (char)('0' + v);
    *p = 0;
    return p;
}

/* Interval names by distance (semitones): P perfect, M major, m minor ('~', a lower-case m), A augmented, TT the
 * tritone, 5 the power fifth; P8 the octave. Key tag "M10", title "MAJOR 10TH". */
static const uint8_t c2_iv_num[12] = { 1, 2, 2, 3, 3, 4, 4, 5, 6, 6, 7, 7 };
static const char c2_iv_qual[12] = { 'P', '~', 'M', '~', 'M', 'P', 'A', 'P', '~', 'M', '~', 'M' };
C2_FN int c2_iv_split(int d, int *num)
{
    int s = d < 0 ? 0 : d, o = 0;
    while (s >= 12) {
        s -= 12;
        o++;
    }
    *num = c2_iv_num[s] + 7 * o;
    return s;
}

C2_FN char *c2_dyad_tag(char *p, int d)
{
    int num, s = c2_iv_split(d, &num);
    if (d <= 0)
        return c2_puts(p, "!");    /* one note: a dot under its name */
    if (d == 6)
        return c2_puts(p, "TT");
    if (d == 7)
        return c2_puts(p, "5");
    *p++ = c2_iv_qual[s];
    return c2_put2(p, num);
}

C2_FN char *c2_dyad_long(char *p, int d)
{
    int num, s = c2_iv_split(d, &num);
    if (d <= 0)
        return c2_puts(p, "NOTE");
    if (s == 0)
        return c2_puts(p, d == 12 ? "OCTAVE" : d == 24 ? "2 OCTAVES" : "3 OCTAVES");
    if (d == 6)
        return c2_puts(p, "TRITONE");
    char qc = c2_iv_qual[s];
    p = c2_puts(p, qc == 'P' ? "PERFECT " : qc == 'M' ? "MAJOR " : qc == '~' ? "MINOR " : "AUG ");
    p = c2_put2(p, num);
    int u = num, teen = num >= 11 && num <= 13;
    while (u >= 10)
        u -= 10;
    return c2_puts(p, teen ? "TH" : u == 1 ? "ST" : u == 2 ? "ND" : u == 3 ? "RD" : "TH");
}

/* The (root, quality) to show for a chord: a learned one is recognised; two pitch classes or fewer (and every
 * two-note chord) = Q_DYAD with *dist (c2_dyad). *root = semitones from the key. */
C2_FN int c2_shown2(const struct c2_chord *c, int *root, int *dist)
{
    int q = c->q;
    *root = c->root;
    *dist = 0;
    if (q == Q_NONE || !c2_valid_q(q))
        return Q_NONE;
    int8_t v[C2_NOTES];
    int bi, n = c2_notes(c, -1, v, &bi);
    if (!n)
        return Q_NONE;
    if ((q == Q_CUSTOM || c2_q[q].n == 2) && c2_dyad(v, n, root, dist))
        return Q_DYAD;
    if (q == Q_CUSTOM) {
        int8_t u[C2_NOTES];
        int nu = c2_upper(c, c->top, u);   /* the four upper notes; a learned 5th-note bass shows as "/B" */
        q = c2_recognise(u, nu, root);
        if (q == Q_CUSTOM)
            *root = v[0];
    }
    return q;
}

C2_FN int c2_shown(const struct c2_chord *c, int *root)
{
    int d;
    return c2_shown2(c, root, &d);
}

/* "F#-7" (style 0: symbol), "F#MIN7" (style 2: spelled out) or the key tag (style 1, no slash). Intervals: "CM10" /
 * "C MAJOR 10TH". */
C2_FN char *c2_name(char *p, const struct c2_chord *c, int key, int flats, int style)
{
    int root, dist;
    int q = c2_shown2(c, &root, &dist);
    *p = 0;
    if (q == Q_NONE)
        return p;
    p = c2_puts(p, (flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(key + root)]);
    if (q == Q_DYAD) {
        if (style == 2)
            *p++ = ' ';
        return style == 2 ? c2_dyad_long(p, dist) : c2_dyad_tag(p, dist);
    }
    if (style == 2 && q != Q_CUSTOM && c2_q[q].longname[0] >= 'A')
        *p++ = ' ';                /* "C MAJ7", "A MIN7", but "G7", "C6/9" */
    p = c2_puts(p, q == Q_CUSTOM ? "?" : style == 1 ? c2_q[q].tag : style == 2 ? c2_q[q].longname : c2_q[q].name);
    if (style == 1)
        return p;
    int low = 0, has = 0;
    if (c->q == Q_CUSTOM) {
        int8_t v[C2_NOTES];
        int bi, n = c2_notes(c, -1, v, &bi);
        low = n ? v[0] : 0;
        has = q != Q_CUSTOM;
    } else if (c->has_slash) {
        low = c->slash;
        has = 1;
    }
    if (has && c2_mod12(low - root) != 0) {
        *p++ = '/';
        p = c2_puts(p, (flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(key + low)]);
    }
    return p;
}

/* The span of a chord's notes (lowest to highest), for "PLAYED 2 OCT". */
C2_FN int c2_span(const struct c2_chord *c, int bass)
{
    int8_t v[C2_NOTES];
    int bi, n = c2_notes(c, bass, v, &bi);
    return n ? v[n - 1] - v[0] : 0;
}

C2_FN void c2_clear(struct c2_chord *c)
{
    c->root = 0;
    c->q = Q_NONE;
    c->voi = 0;
    c->top = 0;
    for (int i = 0; i < C2_MAX; i++)
        c->iv[i] = 0;
    c->n = 0;
    c->has_slash = 0;
    c->slash = 0;
    c->flags = 0;
}

/* TYPE turned on a learned chord: it becomes structured (recognised root and quality, or `q`), keeping a learned
 * bass that is not the root as the slash bass, so G/B stays G^7/B. TOP and VOICING start again from close. */
C2_FN void c2_structure(struct c2_chord *c, int q)
{
    int8_t v[C2_NOTES];
    int bi, n = c2_notes(c, -1, v, &bi);
    if (!n) {
        c2_clear(c);
        return;
    }
    int root = 0, low = v[0], dist;
    int8_t u[C2_NOTES];
    int nu = c2_upper(c, c->top, u);
    int got = c2_dyad(v, n, &root, &dist) ? c2_dyad_q(dist) : c2_recognise(u, nu, &root);
    if (got == Q_CUSTOM)
        root = low;
    root = low + c2_mod12(root - low);              /* the root's pitch at or above the bass */
    while (root > 11)
        root -= 12;
    while (root < -11)
        root += 12;
    c->q = (uint8_t)(q ? q : got == Q_CUSTOM ? Q_MAJ : got);
    c->root = (int8_t)root;
    c->voi = 0;
    c->top = 0;
    c->has_slash = !c2_is_wide(c->q) && c->q != Q_5 && c2_mod12(low - root) != 0;
    c->slash = (int8_t)low;
    c->n = 0;
}

/* The quality TYPE starts from on a learned chord: the recognised one (an interval: its WIDE quality), else MAJ. */
C2_FN int c2_custom_q(const struct c2_chord *c)
{
    int root, dist;
    int q = c2_shown2(c, &root, &dist);
    if (q == Q_DYAD)
        q = c2_dyad_q(dist);
    return q > Q_NONE && q < Q_COUNT ? q : Q_MAJ;
}

/* ---- SMOOTH (per map): voice leading. The TOP (in the chord's own VOICING) whose upper notes move least from the
 * previous chord's, near the register the chord's own TOP sets. prev[pn]: the previous chord's upper notes in
 * key-relative units (key + interval). Deterministic: the same previous chord gives the same choice, and after a
 * reset (pn = 0) the chord's own TOP plays, so a phrase starts where the player set it. ---- */
C2_FN int c2_iabs(int x) { return x < 0 ? -x : x; }

/* How far chord a (+ ka) moves to b: voice by voice (both ascending) when they have as many notes, else each note to
 * the nearest of the other, both ways. */
C2_FN int c2_move(const int8_t *a, int na, int ka, const int8_t *b, int nb)
{
    int cost = 0;
    if (na == nb) {
        for (int i = 0; i < na; i++)
            cost += c2_iabs(a[i] + ka - b[i]);
        return 2 * cost;
    }
    for (int i = 0; i < na; i++) {
        int best = 99;
        for (int j = 0; j < nb; j++) {
            int d = c2_iabs(a[i] + ka - b[j]);
            best = d < best ? d : best;
        }
        cost += best;
    }
    for (int j = 0; j < nb; j++) {
        int best = 99;
        for (int i = 0; i < na; i++) {
            int d = c2_iabs(a[i] + ka - b[j]);
            best = d < best ? d : best;
        }
        cost += best;
    }
    return cost;
}

C2_FN int c2_smooth(const struct c2_chord *c, int key, const int8_t *prev, int pn)
{
    int8_t own[C2_MAX], v[C2_MAX];
    int n0 = c2_upper(c, c->top, own);
    if (!pn || n0 < 2)
        return c->top;
    int centre = 0;
    for (int i = 0; i < n0; i++)
        centre += own[i];
    int span = c->q != Q_CUSTOM && c2_is_wide(c->q) ? 1 : n0, best = 1 << 30, best_t = c->top;
    for (int t = c->top - span; t <= c->top + span; t++) {
        int n = c2_upper(c, t, v), sum = 0, ok = n == n0;
        for (int i = 0; i < n; i++) {
            sum += v[i];
            ok &= v[i] >= -C2_IV_MAX && v[i] <= C2_IV_MAX;
        }
        if (!ok || c2_iabs(sum - centre) > 12 * n)
            continue;                  /* never more than an octave from the chord's own register */
        int cost = 4 * c2_move(v, n, key, prev, pn) + c2_iabs(sum - centre) + c2_iabs(t - c->top);
        if (cost < best) {
            best = cost;
            best_t = t;
        }
    }
    return best_t;
}

/* ---- WRITE: a diatonic map from a key and a scale ---- */
enum { S_MAJOR, S_MINOR, S_DORIAN, S_PHRYG, S_LYDIAN, S_MIXO, S_LOCRIAN, S_HARM, S_MELODIC, S_COUNT };
static const uint8_t c2_scale[S_COUNT][7] = {
    { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 3, 5, 7, 9, 10 }, { 0, 1, 3, 5, 7, 8, 10 },
    { 0, 2, 4, 6, 7, 9, 11 }, { 0, 2, 4, 5, 7, 9, 10 }, { 0, 1, 3, 5, 6, 8, 10 }, { 0, 2, 3, 5, 7, 8, 11 },
    { 0, 2, 3, 5, 7, 9, 11 },
};
static const char *const c2_scale_name[S_COUNT] = { "MAJOR", "MINOR", "DORIAN", "PHRYGIAN", "LYDIAN", "MIXOLYDIAN",
                                                    "LOCRIAN", "HARM MINOR", "MEL MINOR" };
static const char *const c2_scale_short[S_COUNT] = { "MAJOR", "MINOR", "DORIAN", "PHRYG", "LYDIAN", "MIXO", "LOCRI",
                                                     "HARMIN", "MELMIN" };
static const uint8_t c2_scale_parent[S_COUNT] = { 0, 9, 2, 4, 5, 7, 11, 9, 9 };   /* degree of its parent major */

/* Spell the key with flats? (its parent major key has flats: F Bb Eb Ab Db Gb). tonic: pitch class, C = 0. */
C2_FN int c2_scale_flats(int tonic, int scale)
{
    int pm = c2_mod12(tonic - c2_scale_parent[scale]);
    return pm == 5 || pm == 10 || pm == 3 || pm == 8 || pm == 1 || pm == 6;
}

C2_FN int c2_quality_of(const int8_t *iv, int n)   /* iv from the root, close */
{
    int m = c2_mask(iv, n, 0);
    for (int q = 1; q < Q_FIRST_WIDE; q++)
        if (!c2_q[q].rootless && c2_q[q].n == n && c2_mask(c2_q[q].iv, n, 0) == m)
            return q;
    return Q_CUSTOM;
}

/* The TOP that keeps a chord's top note nearest `target` (key-relative), ties to the smaller |TOP|. */
C2_FN int c2_top_near(const struct c2_chord *c, int key, int target)
{
    int8_t v[C2_MAX];
    int best = 1 << 30, best_t = 0;
    for (int a = 0; a <= 8; a++) {
        int t = a & 1 ? (a + 1) >> 1 : -(a >> 1);   /* 0, 1, -1, 2, -2 ... */
        int n = c2_upper(c, t, v), ok = n > 0;
        for (int i = 0; i < n; i++)
            ok &= v[i] >= -C2_IV_MAX && v[i] <= C2_IV_MAX;
        if (!ok)
            continue;
        int d = c2_iabs(key + v[n - 1] - target);
        if (d < best) {
            best = d;
            best_t = t;
        }
    }
    return best_t;
}

/* Every key whose pitch is in the scale gets the diatonic chord stacked in thirds from it (triads or sevenths);
 * the others play their note. TOPs keep each octave's chords in one register, as DIATONIC C (tops near A4 on the
 * lower keys, F#5 on the upper). */
C2_FN void c2_fill(struct c2_chord *map, int tonic, int scale, int sevenths)
{
    for (int k = 0; k < C2_KEYS; k++) {
        struct c2_chord *c = &map[k];
        c2_clear(c);
        int pc = c2_key_pc(k), deg = -1;
        for (int d = 0; d < 7; d++)
            if (c2_mod12(tonic + c2_scale[scale][d]) == pc)
                deg = d;
        if (deg < 0)
            continue;
        int8_t iv[4];
        int n = sevenths ? 4 : 3;
        for (int i = 0; i < n; i++) {
            int d = deg + 2 * i, up = 0;
            while (d >= 7) {
                d -= 7;
                up += 12;
            }
            iv[i] = (int8_t)(c2_scale[scale][d] + up - c2_scale[scale][deg]);
        }
        c->q = (uint8_t)c2_quality_of(iv, n);
        if (c->q == Q_CUSTOM) {
            for (int i = 0; i < n; i++)
                c->iv[i] = iv[i];
            c->n = (uint8_t)n;
        }
        c->top = (int8_t)c2_top_near(c, k, k < 12 ? 17 : 26);
    }
}

C2_FN uint32_t c2_crc32(const uint8_t *p, int n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

C2_FN int c2_mode_map(int mode) { return mode >= 2 && mode < C2_MODE_COUNT ? mode - 2 : -1; }

#endif
