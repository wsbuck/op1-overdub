/* Host test of CHORD v2 (src/slot/chord2.h, chord2_impl.h through hold_impl.h) with stand-ins for TE: notes, keys,
 * the synth controller's slots and an in-memory yaffs.
 *   cc -std=gnu99 -Wall -Wextra -I src/slot src/slot/test/chord2_test.c -o /tmp/c2t && /tmp/c2t
 * Also run in the Blackfin simulator by chord2_check.py (device compiler, same checks). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int32_t play_mode, slot, octave[8];
static uint8_t key_down[24], locked, shift_down;
static int sounding[128], nons, noffs, bad_range, bad_off;
static int te_press_calls, te_release_calls, te_knob_calls, te_shift_calls, last_te_key, last_te_knob;
static int16_t last_te_delta;

#define PLAY_MODE play_mode
#define SLOT slot
#define OCTAVE(s) octave[s]
#define KEY_DOWN key_down
#define VOICECTL_LOCKED locked
#define SHIFT_DOWN shift_down
static uint32_t features = 7;
#define FEATURES features

void te_make_note(int32_t *n, int key, int vel) { n[0] = key + 0x35 + 12 * OCTAVE(SLOT); n[1] = vel; }
void te_note_off(int32_t *n)
{
    noffs++;
    if (n[0] >= 0 && n[0] < 128 && sounding[n[0]])
        sounding[n[0]] = 0;          /* TE: a note-off removes the note (it is in the active list once) */
    else
        bad_off++;
}
void te_note_on(int32_t *n, int flags)
{
    (void)flags;
    if (n[0] < 0 || n[0] > 127) {
        bad_range++;                 /* VoiceCtl asserts on these */
        return;
    }
    nons++;
    sounding[n[0]] = 1;              /* a note already active is ignored by TE (no second entry) */
}
int te_synth_shiftkey(void *self, int key) { (void)self; last_te_key = key; te_shift_calls++; return 0; }
int te_synth_press(void *self, int key) { (void)self; last_te_key = key; te_press_calls++; return 0; }
int te_synth_release(void *self, int key) { (void)self; last_te_key = key; te_release_calls++; return 0; }
int te_synth_knob(void *self, int knob, int16_t *delta)
{
    (void)self;
    te_knob_calls++;
    last_te_knob = knob;
    last_te_delta = *delta;
    return 0;
}

/* ---- in-memory yaffs: two files, a directory flag, failure injection ---- */
static uint8_t fs_data[2][4096];
static int fs_len[2] = { -1, -1 }, fs_dir, fs_fail_open, fs_short_write, fs_calls, fs_open_fd[4], fs_pos[4], fs_wr[4];
static int fs_index(const char *p)
{
    if (!strcmp(p, "/yaffs2/user/overdub/chorda.bin"))
        return 0;
    if (!strcmp(p, "/yaffs2/user/overdub/chordb.bin"))
        return 1;
    return -2;
}
int te_yaffs_open(const char *path, int flags, int mode)
{
    (void)mode;
    fs_calls++;
    int i = fs_index(path);
    if (i < 0 || fs_fail_open)
        return -1;
    if (flags == 0x241) {
        if (!fs_dir)
            return -1;               /* no directory: create fails */
        fs_len[i] = 0;
    } else if (fs_len[i] < 0)
        return -1;
    for (int fd = 0; fd < 4; fd++)
        if (!fs_open_fd[fd]) {
            fs_open_fd[fd] = i + 1;
            fs_pos[fd] = 0;
            fs_wr[fd] = flags == 0x241;
            return fd + 3;
        }
    return -1;
}
int te_yaffs_read(int fd, void *buf, unsigned n)
{
    fs_calls++;
    fd -= 3;
    int i = fs_open_fd[fd] - 1, left = fs_len[i] - fs_pos[fd];
    if ((int)n > left)
        n = (unsigned)left;
    memcpy(buf, fs_data[i] + fs_pos[fd], n);
    fs_pos[fd] += (int)n;
    return (int)n;
}
int te_yaffs_write(int fd, const void *buf, unsigned n)
{
    fs_calls++;
    fd -= 3;
    int i = fs_open_fd[fd] - 1;
    if (fs_short_write)
        n = n / 2;
    memcpy(fs_data[i] + fs_len[i], buf, n);
    fs_len[i] += (int)n;
    return (int)n;
}
int te_yaffs_close(int fd)
{
    fs_calls++;
    fs_open_fd[fd - 3] = 0;
    return 0;
}
int te_yaffs_mkdir(const char *path, int mode)
{
    (void)mode;
    fs_calls++;
    if (strcmp(path, "/yaffs2/user/overdub") || fs_dir)
        return -1;
    fs_dir = 1;
    return 0;
}

#define HOLD_HOST_TEST
#include "../hold_impl.h"

static int fails, passes;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } else passes++; } while (0)

static struct c2_state c2_boot;          /* the state as the image loads it */

/* A note key as the device delivers it (FUN_019b6c74): without SHIFT the synth controller's press slot (+0x14,
 * c2_press) first, then TE's global press plays it; with SHIFT down the SHIFT slot (+0x1C, hold_shiftkey) first,
 * then TE's global SHIFT handler FUN_019ac9b8 plays it (FUN_019ac0b0: s_note_ons set, the note-on CALL). A release
 * always goes to the release slot (+0x24, c2_release) first, then the global release (the note-off CALL). The key
 * codes are TE's note codes (FUN_019c316c); the code -> key order is not modelled (ascending). */
static const uint8_t note_code[24] = { 0x0D, 0x0E, 0x0F, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
                                       0x1F, 0x20, 0x27, 0x28, 0x29, 0x2A, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35 };
static int slot_calls;                   /* note presses that went through a controller slot first */
static void key_dn(int key)
{
    int32_t n[5];
    int code = note_code[key];
    int taken = shift_down ? hold_shiftkey(0, code) : c2_press(0, code);
    slot_calls++;
    if (taken)
        return;                          /* a slot that returns 1 ends the press: no note (never for a note key) */
    key_down[key] = 1;
    te_make_note(n, key, 0x7F);
    chord_keydown(n);
}
static void key_up(int key)
{
    int32_t n[5];
    c2_release(0, note_code[key]);
    key_down[key] = 0;
    te_make_note(n, key, 0x40);
    hold_keyup(n);
}
static void shift(int down)
{
    if (down) {
        c2_press(0, KEY_SHIFT);      /* press slot sees SHIFT before SHIFT_DOWN is set */
        shift_down = 1;
    } else {
        c2_release(0, KEY_SHIFT);    /* release slot sees it before the global handler clears it */
        shift_down = 0;
    }
}
static int shift_key(int key)        /* SHIFT + a non-note key: the shift slot */
{
    shift_down = 1;
    int r = hold_shiftkey(0, key);
    shift_down = 0;
    return r;
}
static void knob(int k, int detents)
{
    for (int i = 0; i < (detents < 0 ? -detents : detents); i++) {
        int16_t d = detents < 0 ? -1024 : 1024;
        c2_knob(0, k, &d);
    }
}
static void frames(int n)
{
    for (int i = 0; i < n; i++) {
        c2.clock++;
        c2.dg.frames++;
        c2_gui_tick();
    }
}
static int count_sounding(void)
{
    int c = 0;
    for (int i = 0; i < 128; i++)
        c += sounding[i];
    return c;
}
static int is_on(int note) { return note >= 0 && note < 128 && sounding[note]; }
#define N0 0x35                          /* key 0 at octave 0: F3 */

static void fresh(uint32_t mask)
{
    memcpy(&c2, &c2_boot, sizeof c2);
    for (int k = 0; k < 24; k++)
        c2.d.maps[0][k].top = 0;          /* the play tests count notes of DIATONIC C in close root position */
    features = mask;
    hold_on = hold_keys = 0;
    chord_on = chord_learning = 0;
    chord_n = 0;
    chord_reset();
    memset(key_down, 0, sizeof key_down);
    memset(sounding, 0, sizeof sounding);
    memset(octave, 0, sizeof octave);
    nons = noffs = bad_range = bad_off = 0;
    te_press_calls = te_release_calls = te_knob_calls = te_shift_calls = 0;
    shift_down = 0;
    play_mode = 0;
    slot = 0;
    locked = 0;
    synth_mode();
}
static void all_up(void)
{
    for (int k = 0; k < 24; k++)
        if (key_down[k])
            key_up(k);
    if (hold_on)
        hold_shiftkey(0, 7);
}

/* ---- the model ---- */
static void t_model(void)
{
    int root = 0;
    char b[24];
    int total = 0, named = 0, close = 0, close_same = 0;
    for (int q = 1; q < Q_COUNT; q++)
        for (int f = 0; f < F_COUNT; f++)
            for (int top = -3; top <= 4; top++)
                for (int rt = -5; rt <= 6; rt++) {
                    struct c2_chord c = { (int8_t)rt, (uint8_t)q, (uint8_t)f, (int8_t)top, { 0 }, 0, 0, 0, 0 };
                    int8_t iv[C2_NOTES];
                    int bi, n = c2_notes(&c, -1, iv, &bi);
                    int got = c2_recognise(iv, n, &root);
                    total++;
                    named += got != Q_CUSTOM && c2_mask(c2_q[got].iv, c2_q[got].n, root) == c2_mask(iv, n, 0);
                    int twin = q == Q_6 || q == Q_MIN6 || q == Q_DIM7 || q == Q_AUG || q == Q_69 || c2_q[q].rootless ||
                               q == Q_SUS2 || q == Q_SUS4;
                    if (f == F_CLOSE && top == 0 && !twin) {
                        close++;
                        close_same += got == q && c2_mod12(root - rt) == 0;
                    }
                }
    CHECK(named == total, "every voiced chord gets a name with its notes (%d/%d)", named, total);
    CHECK(close_same == close, "close root position keeps root and quality (%d/%d)", close_same, close);
    int8_t am7[4] = { -3, 0, 4, 7 };
    CHECK(c2_recognise(am7, 4, &root) == Q_MIN7 && c2_mod12(root + 3) == 0, "A C E G = A-7");
    int8_t c6[4] = { 0, 4, 7, 9 };
    CHECK(c2_recognise(c6, 4, &root) == Q_6 && root == 0, "C E G A over C = C6");
    int8_t pw[3] = { 0, 7, 12 };
    CHECK(c2_recognise(pw, 3, &root) == Q_5 || 1, "");
    CHECK(c2_recognise(pw, 3, &root) == Q_5 && root == 0, "C G C = C5");
    struct c2_chord c = { 0, Q_MAJ7, F_CLOSE, 0, { 0 }, 0, 0, 0, 0 };
    c2_name(b, &c, 7, 0, 0);
    CHECK(!strcmp(b, "C^7"), "C^7 symbol (%s)", b);
    c2_name(b, &c, 7, 0, 2);
    CHECK(!strcmp(b, "C MAJ7"), "C^7 spelled out (%s)", b);
    struct c2_chord h = { 0, Q_HDIM, F_CLOSE, 0, { 0 }, 0, 0, 0, 0 };
    c2_name(b, &h, 6, 0, 1);
    CHECK(!strcmp(b, "B$"), "key B half-dim tag (%s)", b);
    struct c2_chord a7 = { 0, Q_MIN7, F_CLOSE, 0, { 0 }, 0, 0, 0, 0 };
    c2_name(b, &a7, 4, 0, 1);
    CHECK(!strcmp(b, "A-7"), "A-7 tag (%s)", b);
    int8_t iv[C2_NOTES];
    int bi;
    struct c2_chord d2 = { 0, Q_MAJ7, F_DROP2, 0, { 0 }, 0, 0, 0, 0 };
    c2_notes(&d2, 0, iv, &bi);
    CHECK(iv[0] == -5 && iv[1] == 0 && iv[2] == 4 && iv[3] == 11, "C^7 drop 2 = G C E B");
    int ok = 1;
    for (int q = 1; q < Q_COUNT; q++)
        for (int t = -3; t <= 5; t++) {
            struct c2_chord x = { 0, (uint8_t)q, F_CLOSE, (int8_t)t, { 0 }, 0, 0, 0, 0 };
            int na = c2_notes(&x, 0, iv, &bi), top = iv[na - 1];
            for (int f = 1; f < F_COUNT; f++) {
                x.fam = (uint8_t)f;
                na = c2_notes(&x, 0, iv, &bi);
                ok &= iv[na - 1] == top;
            }
        }
    CHECK(ok, "every VOICING keeps the top note TOP chose");
    ok = 1;
    for (int q = 1; q < Q_COUNT; q++) {
        int prev = -99, nt = c2_q[q].n;
        for (int t = -nt; t <= 2 * nt; t++) {
            struct c2_chord l = { 0, (uint8_t)q, F_CLOSE, (int8_t)t, { 0 }, 0, 0, 0, 0 };
            int n = c2_notes(&l, 0, iv, &bi);
            if (iv[n - 1] <= prev)
                printf("  ladder %s t %d top %d prev %d\n", c2_q[q].longname, t, iv[n - 1], prev);
            ok &= iv[n - 1] > prev;
            prev = iv[n - 1];
        }
    }
    CHECK(ok, "TOP ladder: the top note rises with every step, every quality");
    struct c2_chord gb = { 0, Q_CUSTOM, 0, 0, { -1, 2, 7 }, 3, 0, 0, 0 };
    c2_name(b, &gb, 7, 0, 0);
    CHECK(!strcmp(b, "G/B"), "learned B D G on C = G/B (%s)", b);
    c2_structure(&gb, Q_MAJ7);
    c2_name(b, &gb, 7, 0, 0);
    int n = c2_notes(&gb, 0, iv, &bi);
    CHECK(!strcmp(b, "G^7/B") && c2_mod12(iv[0] + 1) == 0 && bi == -1, "TYPE on G/B keeps the B bass: G^7/B (%s)", b);
    struct c2_chord m9 = { 0, Q_MIN9, F_CLOSE, 0, { 0 }, 0, 0, 0, 0 };
    n = c2_notes(&m9, 1, iv, &bi);
    CHECK(n == 5 && iv[0] == -12 && iv[1] == 3 && bi == 0, "C-9 rootless + BASS = C under Eb G Bb D");
    /* scan order: every quality once, neighbours share notes more often than not */
    int share = 0;
    for (int q = Q_MAJ; q < Q_COUNT - 1; q++)
        share += c2_popcount12(c2_mask(c2_q[q].iv, c2_q[q].n, 0) & c2_mask(c2_q[q + 1].iv, c2_q[q + 1].n, 0)) >= 2;
    for (int q = Q_MAJ; q < Q_COUNT - 1; q++)
        if (c2_popcount12(c2_mask(c2_q[q].iv, c2_q[q].n, 0) & c2_mask(c2_q[q + 1].iv, c2_q[q + 1].n, 0)) < 2)
            printf("  scan pair %s %s shares < 2\n", c2_q[q].longname, c2_q[q + 1].longname);
    CHECK(share >= Q_COUNT - 4, "TYPE scan: neighbours share at least 2 notes (%d of %d)", share, Q_COUNT - 3);
    ok = 1;
    for (int q = 1; q < Q_COUNT; q++) {
        char t[24];
        struct c2_chord x = { 0, (uint8_t)q, 0, 0, { 0 }, 0, 0, 0, 0 };
        c2_name(t, &x, 1, 0, 1);
        ok &= strlen(t) <= 5;        /* root (2) + tag (<= 3) fits a key */
    }
    CHECK(ok, "every key tag fits 5 glyphs");
    uint8_t crc_in[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    CHECK(c2_crc32(crc_in, 9) == 0xCBF43926u, "CRC-32 check value");
}

/* ---- factory maps ---- */
static void t_factory(void)
{
    char b[24];
    fresh(7);
    int ok = 1;
    for (int m = 0; m < 4; m++)
        for (int k = 0; k < 24; k++) {
            int8_t iv[C2_NOTES];
            int bi, n = c2_notes(&c2.d.maps[m][k], 1, iv, &bi);
            for (int i = 0; i < n; i++)
                ok &= iv[i] >= -C2_IV_MAX && iv[i] <= C2_IV_MAX;
        }
    CHECK(ok, "factory maps: intervals within +-35 even with BASS");
    c2_name(b, &c2.d.maps[0][7], 7, 0, 0);
    CHECK(!strcmp(b, "C^7"), "DIATONIC C: C4 = C^7 (%s)", b);
    c2_name(b, &c2.d.maps[0][6], 6, 0, 0);
    CHECK(!strcmp(b, "B$7"), "DIATONIC C: B = B$7 (%s)", b);
    c2_name(b, &c2.d.maps[1][0], 0, 1, 0);
    char b2[24], b3[24];
    c2_name(b2, &c2.d.maps[1][2], 2, 1, 0);
    c2_name(b3, &c2.d.maps[1][4], 4, 1, 0);
    CHECK(!strcmp(b, "G-7") && !strcmp(b2, "C7") && !strcmp(b3, "F^7"), "II-V-I: F G A = G-7 C7 F^7 (%s %s %s)", b, b2, b3);
    c2_name(b, &c2.d.maps[1][22], 22, 1, 0);
    CHECK(!strcmp(b, "G7&9"), "II-V-I: D# = G7b9 (%s)", b);
    c2_name(b, &c2.d.maps[2][3], 3, 1, 0);
    CHECK(!strcmp(b, "A&^9") && c2.d.set[2].bass, "NEO SOUL: Ab3 = Ab^9, BASS on (%s)", b);
    c2_name(b, &c2.d.maps[3][1], 1, 1, 0);
    CHECK(!strcmp(b, "F-"), "POP: F# = Fm (%s)", b);
    CHECK(c2.d.maps[0][1].q == Q_NONE && c2.d.maps[4][7].q == Q_NONE, "black keys of DIATONIC C and MAP 5 plain");
    for (int s = 0; s < 8; s++)
        ok &= c2.d.mode[s] == 0;
    CHECK(ok, "image default: every slot OFF");
}

/* ---- playing ---- */
static void t_play(void)
{
    fresh(7);
    key_dn(7);
    CHECK(count_sounding() == 1 && is_on(N0 + 7), "slot OFF: a key plays its note");
    key_up(7);
    CHECK(count_sounding() == 0, "... and releases");
    c2.d.mode[0] = 2;                     /* MAP 1 DIATONIC C */
    key_dn(7);
    CHECK(count_sounding() == 4 && is_on(N0 + 7) && is_on(N0 + 11) && is_on(N0 + 14) && is_on(N0 + 18),
          "MAP 1: C4 plays C E G B");
    key_dn(8);
    CHECK(count_sounding() == 5 && is_on(N0 + 8), "an unmapped black key plays its note with the chord");
    key_up(8);
    CHECK(count_sounding() == 4, "... and its release leaves the chord");
    key_up(7);
    CHECK(count_sounding() == 0, "chord released");
    /* plain note under a chord that holds the same pitch: its key-up must not cut the chord's note */
    key_dn(4);                            /* A-7: A C E G */
    key_dn(1);                            /* F#: plain */
    key_up(1);
    CHECK(count_sounding() == 4, "plain key-up does not cut the chord");
    key_up(4);
    /* slot 1 stays OFF while slot 0 is MAP 1 */
    slot = 1;
    key_dn(7);
    CHECK(count_sounding() == 1, "slot 2 OFF: plain while slot 1 is MAP 1");
    key_up(7);
    slot = 0;
    CHECK(count_sounding() == 0 && bad_range == 0 && bad_off == 0, "clean (no out-of-range, no stray note-offs)");
    /* octave at the top: notes above 127 are skipped and nothing hangs */
    octave[0] = 4;
    key_dn(23);
    key_up(23);
    CHECK(bad_range == 0 && count_sounding() == 0, "top octave: notes above 127 skipped");
    octave[0] = 0;
}

static void t_budget(void)
{
    fresh(7);
    c2.d.mode[0] = 2;
    key_dn(7);                            /* C^7: 4 */
    key_dn(4);                            /* A-7: A C E G (shares C E G): 5 voices */
    CHECK(count_sounding() == 5, "overlapping chords share notes: 5 voices");
    key_dn(2);                            /* G7: G B D F: 7 distinct with the others -> the oldest chord goes */
    CHECK(count_sounding() <= 6 && is_on(N0 + 2) && is_on(N0 + 6) && is_on(N0 + 9) && is_on(N0 + 12),
          "budget: the new chord sounds whole (%d voices)", count_sounding());
    CHECK(!c2.key_cnt[7] && !c2.key_cnt[4] && (c2.dead >> 7 & 1) && (c2.dead >> 4 & 1),
          "budget: whole chords let go, oldest first, until the new one fits");
    key_up(7);
    CHECK(count_sounding() <= 6 && is_on(N0 + 2), "the shed key's key-up releases nothing");
    key_up(4);
    key_up(2);
    CHECK(count_sounding() == 0, "all released");
    /* plain notes go before chords */
    key_dn(7);                            /* chord */
    key_dn(1);
    key_dn(3);                            /* two plain: 6 voices */
    key_dn(13);                           /* one more plain: shed the oldest plain (F#) */
    CHECK(count_sounding() == 6 && c2.key_cnt[7] && !c2.key_cnt[1], "budget: the oldest plain note goes before a chord");
    all_up();
    CHECK(count_sounding() == 0, "all released");
    /* BASS sheds first */
    c2.d.mode[0] = 4;                     /* NEO SOUL, BASS on */
    key_dn(0);                            /* F-9 rootless + bass: 5 */
    CHECK(count_sounding() == 5, "NEO SOUL F: 4 tones + bass");
    key_dn(1);                            /* plain F#: 6 */
    key_dn(13);                           /* plain: 7 -> the bass goes, nothing else */
    CHECK(count_sounding() == 6 && c2.key_cnt[0] == 5 && !c2.key_cnt[1] && c2.key_cnt[13],
          "budget: a melody note sheds the oldest plain note before the pad's BASS");
    all_up();
    /* fuzz: voices never above 6, no half chords, nothing hangs */
    uint32_t rnd = 12345;
    int worst = 0, half = 0;
    for (int it = 0; it < 20000; it++) {
        rnd = rnd * 1103515245u + 12345u;
        int k = (int)(rnd >> 16) % 24, op = (int)(rnd >> 8) & 15;
        if (op < 6) {
            if (!key_down[k])
                key_dn(k);
        } else if (op < 12) {
            if (key_down[k])
                key_up(k);
        } else if (op == 12) {
            hold_shiftkey(0, 7);
        } else if (op == 13) {
            c2.d.mode[0] = (uint8_t)((rnd >> 20) % C2_MODE_COUNT);
        } else if (op == 14) {
            int no = (int)((rnd >> 21) % 5) - 2, sh = 12 * (no - octave[0]);
            int tmp[128] = { 0 };             /* TE transposes the active notes on an octave change */
            for (int i = 0; i < 128; i++)
                if (sounding[i] && i + sh >= 0 && i + sh < 128)
                    tmp[i + sh] = 1;
            memcpy(sounding, tmp, sizeof tmp);
            octave[0] = no;
        } else
            c2.page = 1, knob((rnd >> 22) & 3, ((rnd >> 24) & 1) ? 1 : -1), c2.page = 0;
        if (count_sounding() > worst)
            worst = count_sounding();
        static int told;
        if (!told && count_sounding() > 6) {
            told = 1;
            printf("  diverge at %d op %d key %d: TE %d ours %d mode %d hold %d\n", it, op, k, count_sounding(),
                   c2_voice_count(), c2.d.mode[0], (int)hold_on);
        }
        for (int kk = 0; kk < 24; kk++)    /* a tracked chord key's notes all sound (none half-stolen) */
            if (c2.key_kind[kk] == 2)
                for (int i = 0; i < c2.key_cnt[kk]; i++) {
                    int32_t nt[5];
                    te_make_note(nt, c2.key_rel[kk][i] - REL_OFS, 0x7F);
                    half += !is_on(nt[0]) && octave[0] == 0;
                }
    }
    all_up();
    octave[0] = 0;
    CHECK(worst <= 6, "fuzz: never more than 6 voices (%d)", worst);
    CHECK(bad_range == 0 && bad_off == 0, "fuzz: no note out of range, no note-off twice");
    CHECK(count_sounding() == 0 && c2_voice_count() == 0, "fuzz: everything released at the end (%d)", count_sounding());
    CHECK(half == 0, "fuzz: every tracked chord sounds whole (%d misses)", half);
}

static void t_hold(void)
{
    fresh(7);
    c2.d.mode[0] = 2;
    hold_shiftkey(0, 7);
    CHECK(hold_on, "HOLD on");
    key_dn(7);
    key_up(7);
    CHECK(count_sounding() == 4, "HOLD latches the chord");
    key_dn(1);
    key_up(1);
    CHECK(count_sounding() == 4, "HOLD: an unmapped key plays live over the pad");
    int before = nons;
    key_dn(4);                            /* A-7 shares C E G with C^7 */
    key_up(4);
    CHECK(count_sounding() == 4 && is_on(N0 + 4) && !is_on(N0 + 18) && nons - before == 1,
          "a new chord replaces the latched one; only A is new (common tones not retriggered)");
    key_dn(9);                            /* D-7 (D F A C) while... */
    key_dn(12);                           /* F^7 (F A C E) pressed while D is down: one gesture, both sound */
    CHECK(c2.key_cnt[9] && c2.key_cnt[12] && !c2.key_cnt[4] && count_sounding() == 5, "keys pressed together form one gesture");
    key_up(9);
    key_up(12);
    before = nons;
    key_dn(11);                           /* re-pressing a latched chord retriggers it */
    CHECK(nons > before, "re-pressing a latched chord retriggers it");
    key_up(11);
    hold_shiftkey(0, 7);
    CHECK(!hold_on && count_sounding() == 0, "HOLD off releases everything");
    /* slot OFF: HOLD latches plain notes as HOLD1 */
    c2.d.mode[0] = 0;
    hold_shiftkey(0, 7);
    key_dn(3);
    key_up(3);
    CHECK(count_sounding() == 1, "slot OFF: HOLD latches plain notes");
    hold_shiftkey(0, 7);
    CHECK(count_sounding() == 0, "... released by HOLD off");
}

static void t_page(void)
{
    char b[24];
    fresh(7);
    c2.d.mode[0] = 2;
    CHECK(shift_key(KEY_SPLIT) == 1 && c2.page && te_shift_calls == 0, "SHIFT+SPLIT opens the page");
    int16_t d = 1024;
    c2.page = 0;
    c2_knob(0, 2, &d);
    CHECK(te_knob_calls == 1 && last_te_knob == 2 && last_te_delta == 1024, "page closed: knobs go to TE unchanged");
    c2.page = 1;
    c2_knob(0, 2, &d);
    CHECK(te_knob_calls == 1 && c2.dg.knobs == 1, "page open: knobs are swallowed");
    /* edited key = last key pressed */
    key_dn(4);
    key_up(4);
    CHECK(c2.sel == 4, "the last key pressed is edited");
    /* TYPE scan with live audition on a held key */
    key_dn(7);                            /* C^7 sounding */
    int before = nons;
    knob(1, 1);                           /* green right: 7 (C E G Bb) */
    CHECK(c2.d.maps[0][7].q == Q_7 && is_on(N0 + 17) && !is_on(N0 + 18) && count_sounding() == 4 && nons - before == 1,
          "TYPE right on a held C^7: C7 heard at once, only the changed note retriggers");
    knob(1, -3);                          /* MAJ7, MIN, MAJ */
    CHECK(c2.d.maps[0][7].q == Q_MAJ && count_sounding() == 3, "TYPE scans down to MAJ (3 notes)");
    knob(1, -1);
    CHECK(c2.d.maps[0][7].q == Q_MAJ, "a wall before NOTE: one detent stays on MAJ");
    knob(1, -1);
    CHECK(c2.d.maps[0][7].q == Q_MAJ, "a fast second detent stays too (a spin can't fall through)");
    frames(12);
    knob(1, -1);
    CHECK(c2.d.maps[0][7].q == Q_NONE && count_sounding() == 1, "after resting at the wall, one more: the key plays its note");
    knob(1, 2);
    CHECK(c2.d.maps[0][7].q == Q_MIN, "and up again from the note: MAJ, MIN");
    frames(40);
    knob(1, 100);
    CHECK(c2.d.maps[0][7].q == Q_COUNT - 1, "TYPE stops at the end of the list");
    key_up(7);
    /* no freeze: right after a knob turn, the next key pressed is the edited one */
    key_dn(9);
    key_up(9);
    CHECK(c2.sel == 9, "no freeze: a key pressed right after a knob turn takes the edit at once");
    knob(2, 1);
    key_dn(7);
    key_up(7);
    CHECK(c2.sel == 7, "... and again, with no wait");
    knob(2, -1);
    /* ROOT, TOP, VOICING */
    knob(0, 2);
    c2_name(b, &c2.d.maps[0][7], 7, 0, 0);
    knob(0, -2);
    knob(1, -100);
    frames(12);
    knob(1, -1);                          /* through the wall to NOTE */
    knob(1, Q_MAJ7);                      /* NOTE -> MAJ7 */
    CHECK(c2.d.maps[0][7].q == Q_MAJ7, "TYPE to ^7");
    knob(2, 2);
    int8_t iv[C2_NOTES];
    int bi, n = c2_notes(&c2.d.maps[0][7], 0, iv, &bi);
    CHECK(iv[n - 1] == 16, "TOP +2: E on top (%d)", iv[n - 1]);
    knob(3, 1);
    int n2 = c2_notes(&c2.d.maps[0][7], 0, iv, &bi);
    CHECK(n2 == 4 && iv[n2 - 1] == 16 && c2.d.maps[0][7].fam == F_DROP2, "VOICING drop 2 keeps E on top");
    knob(2, -100);
    knob(2, 100);
    CHECK(c2_in_range(&c2.d.maps[0][7], 1), "TOP clamps inside the range");
    /* clear, copy, paste, undo */
    c2_press(0, KEY_SPLIT);
    key_dn(4);
    CHECK(c2.d.maps[0][4].q == Q_NONE && count_sounding() == 0 && c2.msg == MSG_CLEARED, "SPLIT + A: cleared, no sound");
    key_up(4);
    c2_release(0, KEY_SPLIT);
    CHECK(count_sounding() == 0 && bad_off == 0, "the cleared key's key-up does nothing");
    shift_key(KEY_LIFT);
    CHECK(c2.d.maps[0][4].q == Q_MIN7 && c2.msg == MSG_UNDONE, "SHIFT+LIFT undoes the clear");
    shift_key(KEY_LIFT);
    CHECK(c2.d.maps[0][4].q == Q_NONE, "SHIFT+LIFT again redoes it");
    shift_key(KEY_LIFT);
    key_dn(2);
    key_up(2);
    te_press_calls = 0;
    c2_press(0, KEY_LIFT);
    key_dn(3);
    key_up(3);
    int tp = te_press_calls;
    c2_press(0, KEY_DROP);
    CHECK(c2.d.maps[0][3].q == Q_7 && c2.msg == MSG_PASTED, "tap G, LIFT, tap G#, DROP: G7's chord pasted on G#");
    CHECK(te_press_calls == tp && tp == 1, "page open: LIFT / DROP / SPLIT never reach TE (only the note keys do)");
    /* SPLIT tapped alone pins the edited key: knobs, learn, LIFT, DROP, SPLIT+key keep to it until SPLIT again */
    key_dn(7);
    key_up(7);
    c2_press(0, KEY_SPLIT);
    c2_release(0, KEY_SPLIT);
    CHECK(c2.pinned && c2.sel == 7 && c2.msg == MSG_PINNED, "SPLIT tapped alone pins the edited key");
    frames(400);
    key_dn(9);
    key_up(9);
    shift(1);
    shift(0);
    CHECK(c2.sel == 7 && c2.pinned, "pinned: playing and SHIFT keep the edited key, however long");
    key_dn(4);                            /* A-7 sounding */
    key_dn(13);                           /* a melody over it */
    CHECK(c2.sel == 7, "pinned: chords and plain notes played keep the edited key");
    key_up(13);
    key_up(4);
    struct c2_chord pin_was = c2.d.maps[0][7];
    knob(3, -1);
    CHECK(c2.sel == 7 && memcmp(&pin_was, &c2.d.maps[0][7], sizeof pin_was), "pinned: the knobs edit the pinned key");
    c2_press(0, KEY_LIFT);
    key_dn(12);
    key_up(12);
    c2_press(0, KEY_DROP);
    CHECK(c2.sel == 7 && c2.pinned && !memcmp(&c2.clip, &c2.d.maps[0][7], sizeof c2.clip) &&
          !memcmp(&c2.d.maps[0][12], &c2.d.maps[0][7], sizeof c2.clip) && c2.msg == MSG_PASTED,
          "pinned: LIFT copies the pinned key, DROP pastes onto the key tapped, the pin stays");
    shift_key(KEY_LIFT);                  /* undo the paste */
    CHECK(c2.d.maps[0][12].q == Q_MAJ7, "... undone");
    c2_press(0, KEY_SPLIT);
    key_dn(16);
    key_up(16);
    c2_release(0, KEY_SPLIT);
    CHECK(c2.pinned && c2.sel == 7 && c2.d.maps[0][16].q == Q_NONE, "pinned: SPLIT + key clears that key, the pin stays");
    shift_key(KEY_LIFT);                  /* undo the clear */
    c2_press(0, KEY_SPLIT);
    c2_release(0, KEY_SPLIT);
    CHECK(!c2.pinned && c2.msg == MSG_UNPINNED, "SPLIT again unpins (UNPINNED)");
    c2_press(0, KEY_SPLIT);
    frames(C2_TAP + 5);
    c2_release(0, KEY_SPLIT);
    CHECK(!c2.pinned, "SPLIT held ~1 s and let go unused: no pin");
    key_dn(11);
    key_up(11);
    CHECK(c2.sel == 11, "unpinned: the next key pressed is edited again");
    /* the pin ends when the page closes, by SHIFT+SPLIT or a T key; the page reopens unpinned */
    c2_press(0, KEY_SPLIT);
    c2_release(0, KEY_SPLIT);
    CHECK(c2.pinned, "pinned again");
    shift_key(KEY_SPLIT);
    CHECK(!c2.page && !c2.pinned, "SHIFT+SPLIT closes the page and unpins");
    shift_key(KEY_SPLIT);
    CHECK(c2.page && !c2.pinned, "the page reopens unpinned");
    c2_press(0, KEY_SPLIT);
    c2_release(0, KEY_SPLIT);
    c2_press(0, 6);
    CHECK(!c2.page && !c2.pinned, "T1 closes the page and unpins");
    shift_key(KEY_SPLIT);
    /* a learn goes to the last key tapped (no wait after a knob turn) */
    knob(2, 1);
    key_dn(11);
    key_up(11);
    CHECK(c2.sel == 11, "a tap right after a knob turn takes the edit");
    shift(1);
    CHECK(c2.sel == 11, "SHIFT alone keeps the edited key");
    key_dn(14);
    CHECK(c2.sel == 11 && c2.learning, "the learn goes to the key tapped");
    shift(0);
    key_up(14);
    shift_key(KEY_LIFT);
    shift_key(KEY_LIFT);
    shift_key(KEY_LIFT);
    CHECK(c2.msg == MSG_UNDONE, "undo says UNDONE");
    shift_key(KEY_LIFT);
    CHECK(c2.msg == MSG_REDONE, "undo again says REDONE");
    /* unpinned, a plain note is the last key pressed too: it takes the edit (pin to keep a pad's chord) */
    key_dn(4);                            /* A-7 sounding (edited) */
    key_dn(13);                           /* F#4: plain */
    CHECK(c2.sel == 13, "unpinned: a plain key over a sounding chord is the edited key (the last pressed)");
    key_up(13);
    key_up(4);
    /* a rootless colour chord keeps its root without BASS */
    struct c2_chord m9 = { 0, Q_MIN9, 0, 0, { 0 }, 0, 0, 0, 0 };
    int8_t r9[C2_NOTES];
    int rb, rn = c2_notes(&m9, 0, r9, &rb);
    CHECK(rn == 5 && rb == 0 && c2_mod12(r9[0]) == 0, "MIN9 without BASS: its root under it, sheddable");
    /* T1..T4 close the page and go on */
    te_press_calls = 0;
    c2_press(0, 6);
    CHECK(!c2.page && te_press_calls == 1 && last_te_key == 6, "T1 closes the page and goes on to TE");
    c2_press(0, KEY_LIFT);
    CHECK(te_press_calls == 2, "page closed: LIFT is TE's");
    /* mode per slot: SHIFT + blue */
    c2.page = 1;
    knob(4, -1);
    CHECK(c2.d.mode[0] == 1, "SHIFT+blue left: MAP 1 -> SHAPE");
    knob(4, 3);
    CHECK(c2.d.mode[0] == 4, "SHIFT+blue: -> MAP 3");
    knob(7, 1);
    CHECK(c2.d.set[2].bass == 1, "SHIFT+orange: BASS on for MAP 3");
    slot = 2;
    knob(4, 1);
    CHECK(c2.d.mode[2] == 1 && c2.d.mode[0] == 4, "slot 3 has its own mode");
    slot = 0;
    c2.page = 0;
    all_up();
}

static void t_learn(void)
{
    char b[24];
    fresh(7);
    c2.d.mode[0] = 2;
    c2.page = 1;
    key_dn(7);
    key_up(7);                            /* edit C4 */
    shift(1);
    key_dn(4); key_dn(7); key_dn(11); key_dn(14);   /* A C E G */
    CHECK(c2.learning && count_sounding() == 4, "learning: notes sound as played");
    shift(0);
    c2_name(b, &c2.d.maps[0][7], 7, 0, 0);
    CHECK(!c2.learning && c2.d.maps[0][7].q == Q_CUSTOM && c2.msg == MSG_LEARNED,
          "SHIFT up: C4 = the chord as played (%s)", b);
    CHECK(!strcmp(b, "A-7"), "learned A C E G is named A-7 (%s)", b);
    all_up();
    /* notes still held are kept; a wrong note lifted before SHIFT goes is dropped */
    shift(1);
    key_dn(6); key_dn(9); key_dn(14); key_dn(15);   /* B D G G# */
    key_up(15);
    shift(0);
    c2_name(b, &c2.d.maps[0][7], 7, 0, 0);
    CHECK(!strcmp(b, "G/B"), "held notes only: B D G = G/B (%s)", b);
    all_up();
    knob(1, 2);
    c2_name(b, &c2.d.maps[0][7], 7, 0, 0);
    CHECK(c2.d.maps[0][7].has_slash && strstr(b, "/B") != 0, "TYPE on a learned slash chord keeps the bass (%s)", b);
    /* none held when SHIFT goes: all notes played */
    shift(1);
    key_dn(0); key_up(0); key_dn(4); key_up(4); key_dn(7); key_up(7);
    shift(0);
    c2_name(b, &c2.d.maps[0][7], 7, 0, 0);
    CHECK(!strcmp(b, "F"), "none held: all notes played = F (%s)", b);
    /* more than 4: the lowest 4, MAX 4 */
    shift(1);
    for (int k = 0; k < 6; k++)
        key_dn(k * 2);
    shift(0);
    CHECK(c2.d.maps[0][7].n == 4 && c2.msg == MSG_MAX4 && c2.d.maps[0][7].has_slash && c2.d.maps[0][7].slash == -7 &&
          c2.d.maps[0][7].iv[0] == -3 && c2.d.maps[0][7].iv[3] == 3, "6 notes: the bass and the top 4 kept, MAX shown");
    int8_t lv[C2_NOTES];
    int lb, ln = c2_notes(&c2.d.maps[0][7], 0, lv, &lb);
    CHECK(ln == 5 && lv[0] == -7, "a learned 5-note voicing plays 5 notes, its bass included");
    all_up();
    /* one note maps a single note */
    shift(1);
    key_dn(2);
    shift(0);
    CHECK(c2.d.maps[0][7].q == Q_CUSTOM && c2.d.maps[0][7].n == 1, "one note maps that note");
    all_up();
    /* a SHIFT used for a combo (SHIFT+T2) does not learn the next note */
    struct c2_chord keep = c2.d.maps[0][7];
    shift(1);
    hold_shiftkey(0, 7);
    key_dn(4);
    shift(0);
    all_up();
    CHECK(!memcmp(&keep, &c2.d.maps[0][7], sizeof keep), "SHIFT+T2 then a note under the same SHIFT: no learn");
    /* SHIFT+LIFT during a learn cancels the learn and does not undo */
    struct c2_chord keep2 = c2.d.maps[0][7];
    int undo_before = (int)c2.undo_ok;
    shift(1);
    key_dn(0);
    key_dn(4);
    hold_shiftkey(0, KEY_LIFT);
    shift(0);
    all_up();
    CHECK(!c2.learning && c2.msg == MSG_LEARN_CANCEL && !memcmp(&keep2, &c2.d.maps[0][7], sizeof keep2) &&
          (int)c2.undo_ok == undo_before, "SHIFT+LIFT during a learn cancels it (nothing written, nothing undone)");
    /* knobs during a learn do nothing */
    shift(1);
    key_dn(4);
    int mode_before = c2.d.mode[0];
    knob(4, 1);
    shift(0);
    all_up();
    CHECK(c2.d.mode[0] == mode_before, "SHIFT+blue during a learn does not change MODE");
    /* SHIFT pressed and released without notes: nothing */
    struct c2_chord was = c2.d.maps[0][7];
    shift(1);
    shift(0);
    CHECK(!memcmp(&was, &c2.d.maps[0][7], sizeof was), "SHIFT alone changes nothing");
    /* knob turns under SHIFT never learn */
    shift(1);
    knob(4, 1);
    shift(0);
    CHECK(!c2.learning, "knob turns under SHIFT never learn");
    knob(4, -1);
    /* off the page in MAP: SHIFT + notes just play (no learn) */
    c2.page = 0;
    shift(1);
    key_dn(7);
    shift(0);
    all_up();
    CHECK(c2.d.mode[0] == 2 && !c2.learning, "off the page in MAP: SHIFT + a note plays, the mode stays");
    /* off the page from OFF: CHORD1's gesture */
    c2.d.mode[0] = 0;
    shift(1);
    key_dn(0); key_dn(4); key_dn(7);
    shift(0);
    all_up();
    CHECK(c2.d.mode[0] == 1 && c2.d.shape[0].q == Q_CUSTOM && c2.d.shape[0].n == 3, "off the page: SHIFT + notes = SHAPE");
    key_dn(2);
    CHECK(count_sounding() == 3 && is_on(N0 + 2) && is_on(N0 + 6) && is_on(N0 + 9), "SHAPE: G plays G B D");
    key_up(2);
    shift(1);
    key_dn(3);
    shift(0);
    key_up(3);
    CHECK(c2.d.mode[0] == 0, "off the page: SHIFT + one note = OFF");
    /* SHIFT's release not seen (fallback): the first plain note ends the learn */
    c2.d.mode[0] = 2;
    c2.page = 1;
    key_dn(1);
    key_up(1);                            /* a plain key: no SHIFT combo pending */
    c2.sel = 7;
    c2.last_tap = 7;
    shift_down = 1;
    key_dn(4); key_dn(7);
    shift_down = 0;
    key_dn(9);
    CHECK(!c2.learning && c2.d.maps[0][7].q == Q_CUSTOM && c2.d.maps[0][7].n == 2, "fallback: a plain note ends the learn");
    all_up();
    CHECK(count_sounding() == 0, "learn: everything released");
}

/* The device's learn sequence (NIGHT1, test image 84: "LEARNED" never appeared), every key through the controller
 * slots as FUN_019b6c74 delivers it. SHIFT + a note reaches the SHIFT slot (hold_shiftkey) before TE plays the note:
 * NIGHT1 counted that as a SHIFT combo, so the first chord note already ended any chance of a learn. */
static void t_learn_device(void)
{
    char b[24];
    fresh(7);
    c2.d.mode[0] = 2;
    shift(1);                             /* SHIFT + SPLIT: the page opens (a combo) */
    CHECK(shift_key(KEY_SPLIT) == 1 && c2.page, "device: SHIFT+SPLIT opens the page");
    shift_down = 1;
    c2_release(0, KEY_SPLIT);
    shift(0);
    CHECK(c2.dg.combo_key == KEY_SPLIT && c2.dg.learn_starts == 0, "device: the opening SHIFT+SPLIT is a combo");
    key_dn(7);                            /* tap the key to learn into */
    key_up(7);
    CHECK(c2.sel == 7 && c2.last_tap == 7, "device: the tapped key is edited");
    int calls = te_shift_calls;
    shift(1);
    CHECK(!c2.shift_used, "device: a new SHIFT press is not a combo yet");
    key_dn(4); key_dn(7); key_dn(11); key_dn(14);   /* A C E G, each through the SHIFT slot first */
    CHECK(te_shift_calls == calls + 4, "device: SHIFT + each note went to TE's SHIFT slot too (it ignores notes)");
    CHECK(c2.learning && !c2.shift_used && c2.dg.learn_starts == 1 && c2.dg.learn_blocked == 0 && count_sounding() == 4,
          "device: SHIFT + notes through the SHIFT slot learn (not a combo), the notes sound");
    shift(0);
    c2_name(b, &c2.d.maps[0][7], 7, 0, 0);
    CHECK(!c2.learning && c2.msg == MSG_LEARNED && !strcmp(b, "A-7") && c2.dg.learn_ok == 1 && c2.dg.learn_end == C2_LE_UP,
          "device: SHIFT up (release slot) writes the chord: LEARNED A-7, END:U (%s)", b);
    CHECK(c2.dg.shift_up == 2 && c2.dg.shift_dn == 2, "device: UP / DN count the SHIFT releases / presses");
    all_up();
    CHECK(count_sounding() == 0, "device: the learn's notes released");
    key_dn(7);
    CHECK(count_sounding() == 4 && is_on(N0 + 4) && is_on(N0 + 14), "device: the key now plays the learned A-7");
    key_up(7);
    /* the tapped key is not part of the chord: a 3-note learn on another key */
    key_dn(2);
    key_up(2);
    shift(1);
    key_dn(9); key_dn(13); key_dn(16);    /* D F# A over G: the key itself not played */
    shift(0);
    all_up();
    c2_name(b, &c2.d.maps[0][2], 2, 0, 0);
    CHECK(c2.d.maps[0][2].q == Q_CUSTOM && c2.d.maps[0][2].n == 3 && !strcmp(b, "D"),
          "device: tap G, SHIFT + D F# A: G's key plays D (the tapped key is only the target) (%s)", b);
    /* the combo rules still hold: SHIFT+T2 (HOLD) then a note under the same SHIFT does not learn */
    struct c2_chord keep = c2.d.maps[0][2];
    shift(1);
    shift_key(KEY_T2);
    shift_down = 1;
    key_dn(4);
    shift(0);
    all_up();
    shift_key(KEY_T2);                    /* HOLD off again */
    CHECK(!memcmp(&keep, &c2.d.maps[0][2], sizeof keep) && c2.dg.learn_blocked == 1 && c2.dg.learn_end == C2_LE_BLOCKED &&
          c2.dg.combo_key == KEY_T2, "device: SHIFT+T2 then a note: no learn, B:1 END:B CB:07");
    /* a combo during a learn cancels it */
    shift(1);
    key_dn(0);
    key_dn(4);
    shift_key(KEY_LIFT);
    shift_down = 1;
    shift(0);
    all_up();
    CHECK(c2.dg.learn_cancels == 1 && c2.dg.learn_end == C2_LE_CANCEL && !memcmp(&keep, &c2.d.maps[0][2], sizeof keep),
          "device: SHIFT+LIFT during a learn cancels it, X:1 END:C");
    /* the octave keys under SHIFT are combos (not notes) */
    shift(1);
    hold_shiftkey(0, 0x24);
    CHECK(c2.shift_used && c2.dg.combo_key == 0x24, "device: SHIFT + an octave key is a combo");
    shift(0);
    /* MAP with the page closed: SHIFT + notes play, M */
    shift_key(KEY_SPLIT);
    shift(1);
    key_dn(5);
    shift(0);
    all_up();
    CHECK(!c2.page && c2.dg.learn_end == C2_LE_MAP && c2.d.mode[0] == 2, "device: page closed in MAP: no learn, END:M");
    /* every TE note code is a note to us, and nothing else is */
    int codes = 0, wrong = 0;
    for (int code = 0; code < 0x40; code++) {
        int note = 0;
        for (int k = 0; k < 24; k++)
            note |= note_code[k] == code;
        codes += c2_note_code(code);
        wrong += c2_note_code(code) != note;
    }
    CHECK(codes == 24 && !wrong && !c2_note_code(KEY_SHIFT) && !c2_note_code(KEY_SPLIT) && !c2_note_code(KEY_T2),
          "the 24 note codes are TE's (FUN_019c316c), SHIFT / SPLIT / T keys are not");
}

/* SHIFT+DROP: RESTORE FACTORY? and DROP again (with or without SHIFT) restores the map; anything else, or ~2.4 s,
 * drops the question; one undo gesture. */
static void t_factory_restore(void)
{
    fresh(7);
    memcpy(&c2, &c2_boot, sizeof c2);    /* the real factory TOPs */
    c2.d.mode[0] = 4;                     /* MAP 3 NEO SOUL */
    shift_key(KEY_SPLIT);
    struct c2_chord was0 = c2.d.maps[2][0];
    key_dn(0);
    key_up(0);
    knob(1, 3);
    knob(7, -1);                          /* BASS off */
    c2_press(0, KEY_SPLIT);
    key_dn(3);
    key_up(3);
    c2_release(0, KEY_SPLIT);
    CHECK(memcmp(&was0, &c2.d.maps[2][0], sizeof was0) && c2.d.maps[2][3].q == Q_NONE && !c2.d.set[2].bass, "NEO SOUL edited");
    /* SHIFT+DROP asks; a key in between drops the question */
    shift_key(KEY_DROP);
    CHECK(c2.msg == MSG_FACTORY_ASK && c2_restore_armed(), "SHIFT+DROP: RESTORE FACTORY?");
    key_dn(5);
    key_up(5);
    c2_press(0, KEY_DROP);
    int same = !memcmp(c2.d.maps[2], c2_factory_maps[2], sizeof c2.d.maps[2]);
    CHECK(!same && !c2_restore_armed(), "a key between SHIFT+DROP and DROP: no restore (DROP pastes as usual)");
    /* a knob between: no restore */
    shift_key(KEY_DROP);
    knob(2, 0);
    knob(0, 1);
    knob(0, -1);
    c2_press(0, KEY_LIFT);
    CHECK(!c2_restore_armed(), "a knob or LIFT drops the question");
    /* too late: no restore */
    shift_key(KEY_DROP);
    frames(C2_ARM + 2);
    c2_press(0, KEY_DROP);
    CHECK(memcmp(c2.d.maps[2], c2_factory_maps[2], sizeof c2.d.maps[2]), "DROP after ~2.4 s: no restore");
    /* SHIFT+DROP, then DROP: restored, map settings too, sounding chords revoiced */
    struct c2_chord edited[24];
    memcpy(edited, c2.d.maps[2], sizeof edited);
    key_dn(7);                            /* a NEO SOUL chord held */
    shift_key(KEY_DROP);
    c2_press(0, KEY_DROP);
    CHECK(!memcmp(c2.d.maps[2], c2_factory_maps[2], sizeof c2.d.maps[2]) && c2.d.set[2].bass == 1 &&
          !strcmp(c2.d.set[2].name, "NEO SOUL") && c2.msg == MSG_FACTORY_DONE, "SHIFT+DROP, DROP: NEO SOUL is factory again");
    CHECK(c2.d.maps[0][7].q == c2_boot.d.maps[0][7].q, "other maps untouched");
    key_up(7);
    CHECK(count_sounding() == 0 && bad_off == 0, "restore under a held chord: nothing left sounding");
    shift_key(KEY_LIFT);
    CHECK(!memcmp(c2.d.maps[2], edited, sizeof edited) && !c2.d.set[2].bass, "SHIFT+LIFT undoes the restore");
    shift_key(KEY_LIFT);                  /* redo */
    /* SHIFT+DROP twice under one SHIFT also confirms */
    knob(1, 1);
    shift(1);
    shift_down = 1;
    hold_shiftkey(0, KEY_DROP);
    hold_shiftkey(0, KEY_DROP);
    shift(0);
    CHECK(memcmp(c2.d.maps[2], c2_factory_maps[2], sizeof c2.d.maps[2]) && c2_restore_armed(),
          "SHIFT+DROP twice: only asks (a plain DROP confirms)");
    c2_press(0, KEY_DROP);
    CHECK(!memcmp(c2.d.maps[2], c2_factory_maps[2], sizeof c2.d.maps[2]), "... then DROP restores");
    /* an empty map (5..8) restores to empty; SHAPE / OFF: nothing, a hint */
    c2.d.mode[0] = 6;                     /* MAP 5 */
    key_dn(4);
    key_up(4);
    knob(1, 2);
    shift_key(KEY_DROP);
    c2_press(0, KEY_DROP);
    int empty = 1;
    for (int k = 0; k < 24; k++)
        empty &= c2.d.maps[4][k].q == Q_NONE;
    CHECK(empty, "MAP 5 restores to empty");
    c2.d.mode[0] = 1;
    struct c2_data before;
    memcpy(&before, &c2.d, sizeof before);
    shift_key(KEY_DROP);
    c2_press(0, KEY_DROP);
    CHECK(c2.msg != MSG_FACTORY_DONE && !memcmp(c2.d.maps, before.maps, sizeof before.maps), "SHAPE: SHIFT+DROP restores nothing");
    shift_key(KEY_DROP);
    CHECK(c2.msg == MSG_FACTORY_NOMAP, "SHAPE: SHIFT+DROP says pick a map");
    /* page closed: SHIFT+DROP and DROP are TE's */
    c2.d.mode[0] = 4;
    shift_key(KEY_SPLIT);
    int sc = te_shift_calls, pc = te_press_calls;
    hold_shiftkey(0, KEY_DROP);
    c2_press(0, KEY_DROP);
    CHECK(te_shift_calls == sc + 1 && te_press_calls == pc + 1 && !c2_restore_armed(), "page closed: SHIFT+DROP, DROP go to TE");
    all_up();
}

/* ---- the file ---- */
static void fs_reset(void)
{
    fs_len[0] = fs_len[1] = -1;
    fs_dir = fs_fail_open = fs_short_write = 0;
}
static void boot(void)                    /* a power cycle: the image's state, then the first frames */
{
    fresh(7);
    frames(C2_LOAD_FRAME + 256 * 4 + 1);   /* with no file at all the load retries 3 times */
}
static void t_file(void)
{
    fs_reset();
    boot();
    CHECK(c2.loaded && c2.dg.load == C2_LOAD_NONE && c2.d.maps[0][7].q == Q_MAJ7, "no file: factory maps, NONE");
    fs_calls = 0;
    fresh(7);
    frames(10);
    CHECK(fs_calls == 0, "no disk access in the first frames after boot");
    frames(C2_LOAD_FRAME + 256 * 4);
    c2.d.mode[0] = 4;
    c2.d.maps[2][5].q = Q_7;
    c2_touch();
    frames(10);
    CHECK(c2.dg.saves == 0, "not saved right after the edit");
    frames(C2_SAVE_DELAY);
    CHECK(c2.dg.saves == 1 && fs_len[0] == C2_FILE && c2.dg.mkdir_ret == 0, "saved ~2 s after the edit (file A)");
    c2.d.maps[2][6].q = Q_9;
    c2_touch();
    frames(C2_SAVE_DELAY + 1);
    CHECK(c2.dg.saves == 2 && fs_len[1] == C2_FILE, "the next save goes to B");
    boot();
    CHECK(c2.dg.load == C2_LOAD_B && c2.d.mode[0] == 4 && c2.d.maps[2][5].q == Q_7 && c2.d.maps[2][6].q == Q_9,
          "power cycle: the newest (B) loads, slot mode comes back");
    /* corrupt B: A is used */
    fs_data[1][100] ^= 0x55;
    boot();
    CHECK(c2.dg.load == C2_LOAD_A && c2.d.maps[2][5].q == Q_7 && c2.d.maps[2][6].q != Q_9, "bad CRC on B: A loads");
    /* the next save must not overwrite A (the only good one) */
    c2.d.maps[2][7].q = Q_13;
    c2_touch();
    frames(C2_SAVE_DELAY + 1);
    CHECK(fs_len[1] == C2_FILE && c2.file_slot == 1, "after loading A the save goes to B");
    boot();
    CHECK(c2.dg.load == C2_LOAD_B && c2.d.maps[2][7].q == Q_13, "and B is the newest again");
    /* both bad: factory */
    fs_data[0][30] ^= 1;
    fs_data[1][30] ^= 1;
    boot();
    CHECK(c2.dg.load == C2_LOAD_BAD && c2.d.maps[0][7].q == Q_MAJ7 && c2.d.mode[0] == 0, "both bad: factory maps, BAD");
    /* short, long, old version, wrong magic */
    fs_reset();
    fs_dir = 1;
    c2_serialize(fs_data[0], &c2_boot.d, 5);
    fs_len[0] = C2_FILE - 1;
    boot();
    CHECK(c2.dg.load == C2_LOAD_BAD, "short file rejected");
    fs_len[0] = C2_FILE + 3;
    boot();
    CHECK(c2.dg.load == C2_LOAD_BAD, "long file rejected");
    fs_len[0] = C2_FILE;
    boot();
    CHECK(c2.dg.load == C2_LOAD_A && c2.dg.load_seq == 5, "a good file loads");
    fs_data[0][8] = 0;                    /* version 0 */
    boot();
    CHECK(c2.dg.load == C2_LOAD_BAD, "old version rejected");
    c2_serialize(fs_data[0], &c2_boot.d, 5);
    fs_data[0][2] = 'X';
    boot();
    CHECK(c2.dg.load == C2_LOAD_BAD, "wrong magic rejected");
    /* sequence numbers wrap */
    fs_reset();
    fs_dir = 1;
    struct c2_data a = c2_boot.d, bb = c2_boot.d;
    a.mode[1] = 3;
    bb.mode[1] = 5;
    c2_serialize(fs_data[0], &a, 0xFFFFFFFFu);
    c2_serialize(fs_data[1], &bb, 1u);
    fs_len[0] = fs_len[1] = C2_FILE;
    boot();
    CHECK(c2.dg.load == C2_LOAD_B && c2.d.mode[1] == 5, "sequence wrap: 1 is newer than 0xFFFFFFFF");
    /* random contents with a valid CRC: everything clamped, every key plays inside the range */
    uint32_t rnd = 99;
    int ok = 1;
    for (int it = 0; it < 200; it++) {
        for (int i = C2_HDR; i < C2_FILE; i++) {
            rnd = rnd * 1664525u + 1013904223u;
            fs_data[0][i] = (uint8_t)(rnd >> 24);
        }
        c2_put32(fs_data[0] + 20, c2_crc32(fs_data[0] + C2_HDR, C2_PAYLOAD));
        c2_put32(fs_data[0] + 12, 1000u + (uint32_t)it);
        memcpy(fs_data[0], "OVDBCHRD", 8);
        c2_put16(fs_data[0] + 8, 1);
        c2_put16(fs_data[0] + 10, C2_HDR);
        c2_put32(fs_data[0] + 16, C2_PAYLOAD);
        fs_len[0] = C2_FILE;
        boot();
        ok &= c2.dg.load == C2_LOAD_A;
        for (int s = 0; s < 8; s++)
            ok &= c2.d.mode[s] < C2_MODE_COUNT;
        for (int m = 0; m < 8; m++) {
            for (int k = 0; k < 24; k++)
                ok &= c2_in_range(&c2.d.maps[m][k], 1) && (c2.d.maps[m][k].q == Q_NONE || c2_valid_q(c2.d.maps[m][k].q));
            ok &= c2.d.set[m].name[11] == 0;
        }
        c2.d.mode[0] = (uint8_t)(2 + (it & 7));
        for (int k = 0; k < 24; k++) {
            key_dn(k);
            char b[32];
            c2_name(b, &c2.d.maps[it & 7][k], k, it & 1, 0);
            ok &= strlen(b) < 16;
        }
        all_up();
        ok &= count_sounding() == 0 && bad_range == 0;
    }
    CHECK(ok, "random file contents (valid CRC): clamped, playable, nothing hangs");
    /* write failures */
    fs_reset();
    boot();
    fs_fail_open = 1;
    c2_touch();
    frames(C2_SAVE_DELAY + 1);
    CHECK(c2.dg.save_err == C2_SAVE_OPEN && c2.dg.saves == 0, "open fails: error shown, no crash");
    int calls = fs_calls;
    frames(100);
    CHECK(fs_calls == calls, "after a failure: wait before retrying");
    frames(256 * 10);
    int tries = (int)c2.save_tries;
    frames(256 * 4);
    CHECK(tries == 8 && (int)c2.save_tries == 8, "gives up after 8 tries until the next edit (%d)", tries);
    fs_fail_open = 0;
    c2_touch();
    frames(C2_SAVE_DELAY + 1);
    CHECK(c2.dg.save_err == C2_SAVE_OK, "the next edit saves again");
    fs_short_write = 1;
    c2_touch();
    frames(C2_SAVE_DELAY + 1);
    CHECK(c2.dg.save_err == C2_SAVE_WRITE, "a short write is an error");
    fs_short_write = 0;
    int good_slot = c2.file_slot;
    frames(300);
    CHECK(c2.dg.save_err == C2_SAVE_OK && c2.file_slot != good_slot, "a retry writes the other file");
    /* the disk not readable at boot: the newest file must survive the first save */
    fs_reset();
    fs_dir = 1;
    struct c2_data a10 = c2_boot.d;
    a10.mode[3] = 7;
    c2_serialize(fs_data[0], &a10, 10);
    c2_serialize(fs_data[1], &c2_boot.d, 9);
    fs_len[0] = fs_len[1] = C2_FILE;
    fs_fail_open = 1;
    boot();
    CHECK(c2.loaded && c2.dg.load == C2_LOAD_NONE, "opens fail at boot: retried, then factory maps");
    fs_fail_open = 0;
    c2.d.mode[0] = 3;
    c2_touch();
    frames(C2_SAVE_DELAY + 1);
    uint32_t sq;
    CHECK(c2.dg.save_err == C2_SAVE_OK && c2_check(fs_data[0], fs_len[0], &sq) && sq == 10 &&
          c2_check(fs_data[1], fs_len[1], &sq) && sq == 11, "the save goes over the older file (A seq 10 kept, B seq 11)");
    /* notes near the top or bottom of the range are left out (an octave key could push them past 0..127) */
    fresh(7);
    c2.d.mode[0] = 2;
    octave[0] = 4;
    key_dn(23);
    int top_ok = 1;
    for (int i = 116; i < 128; i++)
        top_ok &= !sounding[i];
    key_up(23);
    octave[0] = 0;
    CHECK(top_ok && count_sounding() == 0, "no chord note above 115");
    /* no save while learning */
    fs_reset();
    fs_dir = 1;
    boot();
    c2_touch();
    c2.learning = 1;
    c2.learn_frame = c2.clock;
    frames(C2_SAVE_DELAY * 2);
    CHECK(c2.dg.saves == 0, "no save while learning");
    c2.learning = 0;
    frames(1);
    CHECK(c2.dg.saves == 1, "saved after");
}

/* ---- the feature mask: chord2 off = CHORD1 and stock slots ---- */
static void t_mask(void)
{
    for (int mask = 0; mask < 8; mask++) {
        fresh((uint32_t)mask);
        c2.d.mode[0] = 2;
        fs_reset();
        fs_calls = 0;
        int on = (mask & 6) == 6;
        int16_t d = -2048;
        c2.page = 1;
        c2_knob(0, 3, &d);
        int r = c2_press(0, KEY_LIFT);
        c2_release(0, KEY_SPLIT);
        frames(C2_LOAD_FRAME + 2);
        if (!on) {
            CHECK(te_knob_calls == 1 && last_te_knob == 3 && last_te_delta == -2048 && te_press_calls == 1 && r == 0 &&
                  te_release_calls == 1 && fs_calls == 0, "mask %d: every new hook passes to TE, no disk access", mask);
            c2.page = 0;
            key_dn(7);
            CHECK(count_sounding() == 1, "mask %d: MAP data ignored, a key plays its note", mask);
            key_up(7);
        } else
            CHECK(te_knob_calls == 0 && te_press_calls == 0 && te_release_calls == 1, "mask 7: the page takes knobs and LIFT");
    }
}

int main(void)
{
    memcpy(&c2_boot, &c2, sizeof c2);
    t_model();
    t_factory();
    t_play();
    t_budget();
    t_hold();
    t_page();
    t_learn();
    t_learn_device();
    t_factory_restore();
    t_file();
    t_mask();
    printf("\n%d checks, %d failed\n", passes + fails, fails);
    printf(fails ? "FAILED\n" : "all passed\n");
    return fails != 0;
}
