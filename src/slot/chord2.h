/* CHORD v2 (notes/chord2.md): the chord model and the state shared by the engine (hold.c, chord2_impl.h) and the
 * screens (ui.c, ui_chord2.h). Device code: no division, no floats, no libc. Included by both objects, so every
 * function here is static (C2_FN) and the state is one global, `c2`, defined in hold.c.
 *
 * A chord on a key = root (semitones from the key) + quality + TOP (a ladder: each step puts the next chord tone on
 * top, across octaves) + voicing family (close, drop 2, drop 3, spread), or a learned chord kept as played
 * (Q_CUSTOM) until TYPE is turned. Q_NONE (0, so zeroed memory means "plays its note") = no chord.
 * The qualities are listed in the order the TYPE knob scans them (Model:Cycles style, notes/chord2.md "TYPE scan"):
 * bright major colours, suspended, dominant, altered, minor, diminished; turning left past the first one unmaps the key.
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
#define C2_IV_MAX 35                /* intervals from the key stay within +-35 (the player's relative range) */

enum { Q_NONE, Q_5, Q_MAJ, Q_6, Q_MAJ7, Q_69, Q_ADD9, Q_MAJ9, Q_SUS2, Q_SUS4, Q_7SUS4, Q_9SUS4,
       Q_7, Q_9, Q_13, Q_7S11, Q_7B9, Q_7S9, Q_7B13, Q_AUG,
       Q_MIN, Q_MIN6, Q_MINMAJ7, Q_MIN7, Q_MIN9, Q_MIN11, Q_HDIM, Q_DIM, Q_DIM7,
       Q_COUNT, Q_CUSTOM = 0x7E };

struct c2_quality {
    int8_t iv[C2_MAX];              /* close position, ascending, from the root (rootless voicings start above it) */
    uint8_t n;
    uint8_t rootless;
    char name[6];                   /* symbol: Real Book ('-' minor, '^' maj7, '*' dim, '$' half-dim, '&' flat) */
    char tag[5];                    /* on a key: <= 3 glyphs */
    char longname[8];               /* spelled out, for the big title when it fits */
};

static const struct c2_quality c2_q[Q_COUNT] = {
    { { 0 }, 0, 0, "", "", "" },                                /* Q_NONE */
    { { 0, 7 }, 2, 0, "5", "5", "5" },
    { { 0, 4, 7 }, 3, 0, "", "", "MAJ" },
    { { 0, 4, 7, 9 }, 4, 0, "6", "6", "6" },
    { { 0, 4, 7, 11 }, 4, 0, "^7", "^7", "MAJ7" },
    { { 0, 4, 9, 14 }, 4, 0, "6/9", "69", "6/9" },
    { { 0, 4, 7, 14 }, 4, 0, "ADD9", "(9)", "ADD9" },
    { { 4, 7, 11, 14 }, 4, 1, "^9", "^9", "MAJ9" },
    { { 0, 2, 7 }, 3, 0, "SUS2", "S2", "SUS2" },
    { { 0, 5, 7 }, 3, 0, "SUS4", "S4", "SUS4" },
    { { 0, 5, 7, 10 }, 4, 0, "7SUS4", "7S4", "7SUS4" },
    { { 5, 7, 10, 14 }, 4, 1, "9SUS4", "9S4", "9SUS4" },
    { { 0, 4, 7, 10 }, 4, 0, "7", "7", "7" },
    { { 4, 7, 10, 14 }, 4, 1, "9", "9", "9" },
    { { 4, 10, 14, 21 }, 4, 1, "13", "13", "13" },
    { { 0, 4, 10, 18 }, 4, 0, "7#11", "#11", "7#11" },
    { { 4, 7, 10, 13 }, 4, 1, "7&9", "7&9", "7&9" },
    { { 0, 4, 10, 15 }, 4, 0, "7#9", "7#9", "7#9" },
    { { 4, 10, 14, 20 }, 4, 1, "7&13", "&13", "7&13" },
    { { 0, 4, 8 }, 3, 0, "+", "+", "AUG" },
    { { 0, 3, 7 }, 3, 0, "-", "-", "MIN" },
    { { 0, 3, 7, 9 }, 4, 0, "-6", "-6", "MIN6" },
    { { 0, 3, 7, 11 }, 4, 0, "-^7", "-^", "MIN^7" },
    { { 0, 3, 7, 10 }, 4, 0, "-7", "-7", "MIN7" },
    { { 3, 7, 10, 14 }, 4, 1, "-9", "-9", "MIN9" },
    { { 3, 10, 14, 17 }, 4, 1, "-11", "-11", "MIN11" },
    { { 0, 3, 6, 10 }, 4, 0, "$7", "$", "MIN7&5" },
    { { 0, 3, 6 }, 3, 0, "*", "*", "DIM" },
    { { 0, 3, 6, 9 }, 4, 0, "*7", "*7", "DIM7" },
};

enum { F_CLOSE, F_DROP2, F_DROP3, F_SPREAD, F_COUNT };
static const char *const c2_fam_name[F_COUNT] = { "CLOSE", "DROP 2", "DROP 3", "SPREAD" };

/* 12 bytes; the file stores these fields in this order (chord2_impl.h c2_put_chord). */
struct c2_chord {
    int8_t root;                    /* semitones from the key */
    uint8_t q;                      /* Q_*, Q_CUSTOM (learned, iv[] as played) or Q_NONE (the key plays its note) */
    uint8_t fam;                    /* F_* */
    int8_t top;                     /* ladder: 0 = root position; +1 = next chord tone on top; n steps = +1 octave */
    int8_t iv[C2_MAX];              /* Q_CUSTOM: from the key, ascending */
    uint8_t n;
    uint8_t has_slash;              /* slash bass kept from a learned chord whose lowest note is not its root */
    int8_t slash;                   /* semitones from the key */
    uint8_t pad;
};

/* Per map settings (MVP: BASS only is editable; smooth / strum are stored for phase 2). */
struct c2_set {
    uint8_t bass, smooth, flats, strum;
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
    int32_t voices;                 /* sounding notes we track (after the last key event) */
    int32_t load;                   /* C2_LOAD_* */
    int32_t load_seq;
    uint32_t saves;
    int32_t save_err;               /* C2_SAVE_*: the last save's result */
    int32_t mkdir_ret;
};
enum { C2_LOAD_PENDING, C2_LOAD_NONE, C2_LOAD_A, C2_LOAD_B, C2_LOAD_BAD };
enum { C2_SAVE_NONE, C2_SAVE_OK, C2_SAVE_OPEN, C2_SAVE_WRITE, C2_SAVE_CLOSE, C2_SAVE_TORN };

enum { MSG_NONE, MSG_CLEARED, MSG_COPIED, MSG_PASTED, MSG_UNDONE, MSG_LEARNED, MSG_MAX4, MSG_EMPTY_CLIP, MSG_LATER,
       MSG_TURN_GREEN, MSG_NOTHING_UNDO, MSG_MODE, MSG_BASS, MSG_COUNT };

#define C2_LEARN_MAX 8
#define C2_FREEZE 64                /* frames (~2 s) the edited key stays fixed after a knob turn */
#define C2_FLASH 72                 /* frames a message shows */

/* The whole state. Writers: the KeyHandler thread (keys, knobs: every edit) and the GUI thread (the file load,
 * once, while `loading` is set; closing the page). Readers: both (the GUI draws from it, the save serialises it,
 * checked against `gen`). */
struct c2_state {
    struct c2_data d;
    struct c2_data undo;            /* the state before the last edit gesture (undo swaps the two) */
    uint32_t undo_ok;
    struct c2_chord clip;           /* LIFT copies, DROP pastes */
    uint32_t gen;                   /* bumped by every edit of d */
    uint32_t edit_frame;            /* frame of the last edit (save 2 s after) */
    uint32_t knob_frame;            /* frame of the last knob turn (freeze, undo grouping) */
    volatile uint32_t loading;      /* GUI copying a loaded file into d: the keys play plain meanwhile */
    uint32_t loaded;
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
    uint32_t gesture_knob;          /* the undo snapshot was taken for a knob gesture */
    volatile uint32_t clock;        /* frames: the frame hook's (or our engine screen's, without it) */
    uint32_t ui_frame;              /* clock when ui_draw (our engine screen) last drew */
    uint32_t ui_hook_seen;          /* dg.frames when ui_draw last ran (frame hook alive if it moved) */
    struct c2_diag dg;
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

/* Voice a quality: TOP steps of inversion on the close voicing (each moves the lowest note up an octave; negative
 * steps move the top note down), then the family's drop on the result: drops only move notes below the top, so
 * VOICING never changes the top note TOP set. Intervals from the root, ascending. Triads: DROP 3 = DROP 2. */
C2_FN int c2_voice(const struct c2_quality *q, int fam, int top, int8_t *out)
{
    int n = q->n;
    for (int i = 0; i < n; i++)
        out[i] = q->iv[i];
    if (n < 2)
        return n;
    for (; top > 0; top--) {        /* the lowest note up an octave until a new note is on top (wide chords) */
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
    if ((fam == F_DROP3 && n == 3) || (fam > F_DROP2 && n == 2))
        fam = F_DROP2;             /* triads: DROP 3 = DROP 2; a power chord: anything wider = DROP 2 */
    if (fam == F_DROP2)
        out[n - 2] -= 12;
    else if (fam == F_DROP3)
        out[n - 3] -= 12;
    else if (fam == F_SPREAD) {
        if (n == 4) {
            out[2] -= 12;           /* drop 2 + 4 */
            out[0] -= 12;
        } else {
            out[1] -= 12;           /* triad: drop 2, then the lowest down too: wide, top kept */
            out[0] -= 12;
        }
    }
    c2_sort(out, n);
    return n;
}

C2_FN int c2_valid_q(int q) { return q == Q_CUSTOM || (q > Q_NONE && q < Q_COUNT); }

/* Intervals from the key, as the player plays them, ascending; bass adds the root an octave under the chord's root
 * (and under its lowest note) as a 5th voice (*bass_at = its index, else -1). Returns the count (0 = plays its note).
 * Defensive: anything out of range (a torn read while the GUI loads the file) gives 0. */
C2_FN int c2_notes(const struct c2_chord *c, int bass, int8_t *out, int *bass_at)
{
    int n = 0;
    *bass_at = -1;
    if (c->q == Q_NONE || !c2_valid_q(c->q))
        return 0;
    if (c->q == Q_CUSTOM) {
        if (c->n < 1 || c->n > C2_MAX)
            return 0;
        for (int i = 0; i < c->n; i++)
            out[i] = c->iv[i];
        n = c->n;
        c2_sort(out, n);
        return n;
    }
    n = c2_voice(&c2_q[c->q], c->fam < F_COUNT ? c->fam : F_CLOSE, c->top, out);
    for (int i = 0; i < n; i++)
        out[i] = (int8_t)(out[i] + c->root);
    int b = 0, add = 0;
    if (c->has_slash) {            /* G/B: B under the chord */
        b = c->slash;
        add = 1;
    } else if (bass) {
        b = c->root - 12;
        add = 1;
    }
    if (add && n) {
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

/* Name a set of played notes (intervals from the key): the best (root, quality). Compares pitch-class sets, so
 * doubled notes don't matter. Prefers the root in the bass, then a voicing with the root, then the earlier quality.
 * Returns the quality or Q_CUSTOM. */
C2_FN int c2_recognise(const int8_t *iv, int n, int *root_semis)
{
    if (n < 1)
        return Q_CUSTOM;
    int want = c2_mask(iv, n, 0), best = -1, best_q = Q_CUSTOM;
    int low = iv[0];
    for (int i = 1; i < n; i++)
        low = iv[i] < low ? iv[i] : low;
    for (int q = 1; q < Q_COUNT; q++) {
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

C2_FN char *c2_puts(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

/* The (root, quality) to show for a chord: a learned one is recognised; *root = semitones from the key. */
C2_FN int c2_shown(const struct c2_chord *c, int *root)
{
    int q = c->q;
    *root = c->root;
    if (q == Q_CUSTOM) {
        if (c->n < 1 || c->n > C2_MAX)
            return Q_NONE;
        int8_t v[C2_MAX];
        for (int i = 0; i < c->n; i++)
            v[i] = c->iv[i];
        c2_sort(v, c->n);
        q = c2_recognise(v, c->n, root);
        if (q == Q_CUSTOM)
            *root = v[0];
    }
    return c2_valid_q(q) ? q : Q_NONE;
}

/* "F#-7" (style 0: symbol), "F#MIN7" (style 2: spelled out) or the key tag (style 1, no slash). */
C2_FN char *c2_name(char *p, const struct c2_chord *c, int key, int flats, int style)
{
    int root;
    int q = c2_shown(c, &root);
    *p = 0;
    if (q == Q_NONE)
        return p;
    p = c2_puts(p, (flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(key + root)]);
    if (style == 2 && q != Q_CUSTOM && c2_q[q].longname[0] >= 'A')
        *p++ = ' ';                /* "C MAJ7", "A MIN7", but "G7", "C6/9" */
    p = c2_puts(p, q == Q_CUSTOM ? "?" : style == 1 ? c2_q[q].tag : style == 2 ? c2_q[q].longname : c2_q[q].name);
    if (style == 1)
        return p;
    int low = 0, has = 0;
    if (c->q == Q_CUSTOM) {
        low = c->iv[0];
        for (int i = 1; i < c->n; i++)
            low = c->iv[i] < low ? c->iv[i] : low;
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

C2_FN void c2_clear(struct c2_chord *c)
{
    c->root = 0;
    c->q = Q_NONE;
    c->fam = F_CLOSE;
    c->top = 0;
    for (int i = 0; i < C2_MAX; i++)
        c->iv[i] = 0;
    c->n = 0;
    c->has_slash = 0;
    c->slash = 0;
    c->pad = 0;
}

/* TYPE turned on a learned chord: it becomes structured (recognised root and quality, or `q`), keeping a learned
 * bass that is not the root as the slash bass, so G/B stays G^7/B. */
C2_FN void c2_structure(struct c2_chord *c, int q)
{
    int root = 0, low = c->iv[0];
    for (int i = 1; i < c->n; i++)
        low = c->iv[i] < low ? c->iv[i] : low;
    int8_t v[C2_MAX];
    for (int i = 0; i < c->n; i++)
        v[i] = c->iv[i];
    c2_sort(v, c->n);
    int got = c2_recognise(v, c->n, &root);
    if (got == Q_CUSTOM)
        root = low;
    root = low + c2_mod12(root - low);              /* the root's pitch at or above the bass */
    while (root > 11)
        root -= 12;
    while (root < -11)
        root += 12;
    c->q = (uint8_t)(q ? q : got == Q_CUSTOM ? Q_MAJ : got);
    c->root = (int8_t)root;
    c->fam = F_CLOSE;
    c->top = 0;
    c->has_slash = c2_mod12(low - root) != 0;
    c->slash = (int8_t)low;
    c->n = 0;
}

/* The quality TYPE starts from on a learned chord: the recognised one (else MAJ). */
C2_FN int c2_custom_q(const struct c2_chord *c)
{
    int root;
    int q = c2_shown(c, &root);
    return q > Q_NONE && q < Q_COUNT ? q : Q_MAJ;
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
