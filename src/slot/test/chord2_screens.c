/* Host gallery of the CHORD v2 screens (ui_chord2.h, the code ui.c draws on the device), with the engine
 * (hold_impl.h, chord2_impl.h) driven through its real hooks to set each scene up. chord2_screens.py renders the output.
 *   chord2_screens list | SCENE | check
 * A scene prints one "x0 y0 x1 y1 width colour" per segment (Q22.10 SVG units; width -1 = a 1-panel-pixel fast line);
 * the AA / fast line counts go to stderr. `check` runs the frame hook's page-closing rules. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef int32_t fp32;
#define FP(x) ((fp32)(x) << 10)
enum { RED = 2, OFFRED = 3, OFFGREY = 4, DARKGREEN = 5, PURPLEGREY = 8, LIGHTBLUE = 9, DARKBLUE = 10, GREY = 12, ORANGE = 13,
       WHITE = 14, BLUE = 15, GREEN = 16, LIGHTGREEN = 18 };

static int n_aa, n_fast;
static void line_fp(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, fp32 w, int colour)
{
    (void)r;
    n_aa++;
    printf("%d %d %d %d %d %d\n", x0, y0, x1, y1, w, colour);
}
static void line_fast(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, int colour)
{
    (void)r;
    n_fast++;
    printf("%d %d %d %d -1 %d\n", x0, y0, x1, y1, colour);
}
static void vline_fast(void *r, fp32 x, fp32 y0, fp32 y1, int colour) { line_fast(r, x, y0, x, y1, colour); }

/* ---- TE stand-ins for the engine ---- */
static int32_t play_mode, slot, octave[8];
static uint8_t key_down[24], locked, shift_down;
#define PLAY_MODE play_mode
#define SLOT slot
#define OCTAVE(s) octave[s]
#define KEY_DOWN key_down
#define VOICECTL_LOCKED locked
#define SHIFT_DOWN shift_down
static uint32_t features = 7;
#define FEATURES features
static uint32_t host_time = 1000000, tempo_spb = 22050u << 14;   /* c2_time units (1.28 us); 120 BPM */
static uint32_t c2_time(void) { return host_time; }
#define TEMPO_SPB tempo_spb
void te_make_note(int32_t *n, int key, int vel) { n[0] = key + 0x35 + 12 * OCTAVE(SLOT); n[1] = vel; }
void te_note_off(int32_t *n) { (void)n; }
void te_note_on(int32_t *n, int flags) { (void)n; (void)flags; }
int te_synth_shiftkey(void *self, int key) { (void)self; (void)key; return 0; }
int te_synth_press(void *self, int key) { (void)self; (void)key; return 0; }
int te_synth_release(void *self, int key) { (void)self; (void)key; return 0; }
int te_synth_knob(void *self, int knob, int16_t *delta) { (void)self; (void)knob; (void)delta; return 0; }
int te_yaffs_open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -1; }
int te_yaffs_read(int fd, void *buf, unsigned n) { (void)fd; (void)buf; (void)n; return -1; }
int te_yaffs_write(int fd, const void *buf, unsigned n) { (void)fd; (void)buf; (void)n; return -1; }
int te_yaffs_close(int fd) { (void)fd; return 0; }
int te_yaffs_mkdir(const char *path, int mode) { (void)path; (void)mode; return -1; }
#define HOLD_HOST_TEST
#include "../hold_impl.h"
#include "../ui_chord2.h"

static void dn(int k)
{
    int32_t n[5];
    key_down[k] = 1;
    te_make_note(n, k, 0x7F);
    chord_keydown(n);
}
static void up(int k)
{
    int32_t n[5];
    key_down[k] = 0;
    te_make_note(n, k, 0x40);
    hold_keyup(n);
}
static void knob(int k, int detents)
{
    for (int i = 0; i < (detents < 0 ? -detents : detents); i++) {
        int16_t d = detents < 0 ? -1024 : 1024;
        c2_knob(0, k, &d);
    }
}
static void tick(int n)
{
    for (int i = 0; i < n; i++)
        c2.clock++;
}
static void shift(int on)
{
    if (on) {
        c2_press(0, KEY_SHIFT);
        shift_down = 1;
    } else {
        c2_release(0, KEY_SHIFT);
        shift_down = 0;
    }
}
static void base(int mode)
{
    c2.clock = 500;
    c2.d.mode[0] = (uint8_t)mode;
    c2.page = 1;
    c2.dg.frames = 4321;
    c2.dg.load = C2_LOAD_A;
    c2.dg.load_seq = 17;
    c2.dg.saves = 3;
    c2.dg.save_err = C2_SAVE_OK;
    c2.dg.ctl_synth = 4000;
}
static void view(struct c2_view *v)
{
    v->slot = slot;
    v->octave = octave[slot];
    v->hold = hold_on != 0;
    v->held = hold_keys;
    v->shift = shift_down;
    v->now = c2.clock;
}
static void page(void)
{
    struct c2_view v;
    view(&v);
    c2_draw_page(0, &c2, &v);
}

/* ---- scenes ---- */
static void s_map_play(void)              /* DIATONIC C, G4 held: the played view */
{
    base(2);
    dn(14);
    page();
}
static void s_map_type_scan(void)         /* C4 held, green turned: C^7 -> C6 -> C (scan strip, heard live) */
{
    base(2);
    dn(7);
    knob(1, -2);
    page();
}
static void s_map_top(void)
{
    base(2);
    dn(7);
    up(7);
    tick(80);
    knob(2, 2);
    page();
}
static void s_map_voicing(void)
{
    base(2);
    dn(7);
    up(7);
    tick(80);
    knob(3, -1);
    page();
}
static void s_pinned_playing(void)        /* C4 pinned (SPLIT tapped) while G7 is played */
{
    base(2);
    dn(7);
    up(7);
    knob(3, 1);
    c2_press(0, KEY_SPLIT);
    c2_release(0, KEY_SPLIT);
    tick(80);
    dn(2);
    page();
}
static void s_learn(void)                 /* C4 edited, SHIFT + A C E G: named live, red */
{
    base(2);
    dn(7);
    up(7);
    tick(80);
    shift(1);
    dn(4); dn(7); dn(11); dn(14);
    page();
}
static void s_learned(void)
{
    base(2);
    dn(7);
    up(7);
    tick(80);
    shift(1);
    dn(4); dn(7); dn(11); dn(14);
    shift(0);
    up(4); up(7); up(11); up(14);
    page();
}
static void s_learned_slash(void)         /* B D G learned on C4 = G/B; TYPE turned keeps the bass */
{
    base(2);
    dn(7);
    up(7);
    tick(80);
    shift(1);
    dn(6); dn(9); dn(14);
    shift(0);
    up(6); up(9); up(14);
    tick(80);
    knob(1, 2);
    page();
}
static void s_map_shift(void)             /* SHIFT held: MODE, BASS, the diagnostics */
{
    base(2);
    c2.dg.knobs = 12;
    c2.dg.knob_delta = 1024;
    c2.dg.split = 2;
    c2.dg.lift = 1;
    c2.dg.shift_up = 5;
    c2.dg.shift_dn = 5;
    c2.dg.learn_starts = 3;
    c2.dg.learn_ok = 2;
    c2.dg.learn_cancels = 1;
    c2.dg.learn_end = C2_LE_UP;
    c2.dg.combo_key = KEY_SPLIT;
    shift_down = 1;
    page();
}
static void s_unpinned(void)              /* SPLIT tapped again: UNPINNED, the open padlock */
{
    base(2);
    dn(7);
    up(7);
    c2_press(0, KEY_SPLIT);
    c2_release(0, KEY_SPLIT);
    tick(80);
    c2_press(0, KEY_SPLIT);
    c2_release(0, KEY_SPLIT);
    page();
}
static void s_notes_offkb(void)           /* D5 (top key), TOP up: notes past the keyboard's right edge, counted */
{
    base(2);
    dn(21);
    up(21);
    tick(80);
    knob(2, 3);
    page();
}
static void s_write_factory(void)         /* NEO SOUL, SHIFT+DROP: WRITE opens on FACTORY -> MAP 3 */
{
    base(4);
    hold_shiftkey(0, KEY_DROP);
    page();
}
static void s_write_dorian(void)          /* WRITE: D dorian 7ths into MAP 5 (the preview on the keys) */
{
    base(2);
    hold_shiftkey(0, KEY_DROP);
    knob(0, 1);
    knob(1, 2);
    knob(2, S_DORIAN);
    knob(3, 4);
    page();
}
static void s_write_copy(void)            /* WRITE: a copy of II-V-I a tone up into MAP 6 */
{
    base(3);
    hold_shiftkey(0, KEY_DROP);
    knob(0, 3);
    knob(1, 2);
    knob(3, 4);
    page();
}
static void s_write_overwrite(void)       /* WRITE: triads over a used map: OVERWRITE? */
{
    base(2);
    hold_shiftkey(0, KEY_DROP);
    knob(0, 2);
    knob(1, 7);
    c2_press(0, KEY_DROP);
    page();
}
static void s_played(void)                /* the played-notes view: a C^7 pad and a melody note, what sounds lit */
{
    base(2);
    dn(7);
    dn(16);
    page();
}
static void s_played_hold(void)           /* NEO SOUL + BASS latched, a melody over it: every sounding note */
{
    base(4);
    hold_shiftkey(0, 7);
    dn(0);
    up(0);
    dn(19);
    page();
}
static void s_map_after(void)             /* 0.5 s after the last key-up: the map view again */
{
    base(2);
    dn(14);
    up(14);
    tick(C2_PLAYED_SHOW + 1);
    page();
}
static void s_voicing_up(void)            /* VOICING right: UP 2 (the bass kept), the strip */
{
    base(2);
    dn(7);
    up(7);
    tick(80);
    knob(3, 1);
    page();
}
static void s_voicing_spread(void)        /* VOICING all the way left: SPREAD */
{
    base(2);
    dn(7);
    up(7);
    tick(80);
    knob(3, -4);
    page();
}
static void s_learned_10th(void)          /* the user's C3 + E4 learned on C4: C MAJOR 10TH, VOICING moves the E */
{
    base(5);
    dn(7);
    up(7);
    tick(80);
    shift(1);
    octave[0] = -1;
    dn(7);                                /* C3 */
    octave[0] = 0;
    dn(11);                               /* E4 */
    shift(0);
    up(11);
    key_down[7] = 0;
    tick(80);
    page();
}
static void s_learned_wide(void)          /* C3 G3 E4 learned: the quality name, "PLAYED 2 OCT" */
{
    base(5);
    dn(7);
    up(7);
    tick(80);
    shift(1);
    octave[0] = -1;
    dn(7);
    dn(14);
    octave[0] = 0;
    dn(11);
    shift(0);
    up(11);
    key_down[7] = key_down[14] = 0;
    tick(80);
    page();
}
static void s_type_wide(void)             /* TYPE left of NOTE: the WIDE group, M10 */
{
    base(2);
    dn(8);
    up(8);
    tick(80);
    knob(1, -2);
    page();
}
static void s_map_wide(void)              /* a map of wide intervals: the key labels */
{
    base(6);
    static const uint8_t q[7] = { Q_P8, Q_M10, Q_MIN10, Q_P12, Q_M9D, Q_M13D, Q_TT };
    static const uint8_t k[7] = { 0, 2, 4, 5, 7, 9, 11 };
    for (int i = 0; i < 7; i++) {
        c2.d.maps[4][k[i]].q = q[i];
        c2.d.maps[4][k[i] + 12].q = q[i];
    }
    dn(14);
    up(14);
    tick(80);
    page();
}
static void s_strum_shift(void)           /* SHIFT layer with STRUM (up 30 ms), SMOOTH on; the SEQ diagnostic line */
{
    base(3);
    c2.seq_live = 8;
    c2.d.set[1].strum = 3;
    c2.dg.tick_rate = 689;
    c2.dg.tick_gap = 1140;
    c2.dg.strum_sent = 412;
    c2.owner = 1;
    shift_down = 1;
    page();
}
static void s_strum_header(void)          /* the header: strum, SMOOTH, BASS, HOLD */
{
    base(4);
    c2.seq_live = 8;
    c2.d.set[2].strum = -4;
    hold_shiftkey(0, 7);
    dn(5);
    up(5);
    tick(80);
    page();
}
static void s_cleared(void)
{
    base(2);
    c2_press(0, KEY_SPLIT);
    dn(16);
    up(16);
    c2_release(0, KEY_SPLIT);
    page();
}
static void s_map_251(void)
{
    base(3);
    dn(2);
    page();
}
static void s_map_neo(void)               /* NEO SOUL, BASS, HOLD, a chord latched */
{
    base(4);
    hold_shiftkey(0, 7);
    dn(3);
    up(3);
    page();
}
static void s_map_pop(void)
{
    base(5);
    dn(9);
    page();
}
static void s_unmapped(void)              /* C#4 has no chord: NOTE; green gives it one */
{
    base(2);
    dn(8);
    up(8);
    page();
}
static void s_shape(void)                 /* SHAPE: the slot's shape on every key, lit from the edited key */
{
    base(1);
    dn(7);
    up(7);
    tick(80);
    knob(1, Q_MIN7 - Q_MAJ);              /* MAJ -> MIN7 */
    knob(3, 1);
    page();
}
static void s_off(void)
{
    base(0);
    page();
}
static void s_badge(void)                 /* our engine screen: map number and the last chord */
{
    struct c2_view v;
    base(2);
    c2.page = 0;
    dn(2);
    view(&v);
    c2_draw_badge(0, &c2, &v);
}
static void s_chip(void)                  /* TE's screens: the sounding chord, top right */
{
    struct c2_view v;
    base(3);
    c2.page = 0;
    dn(4);
    view(&v);
    c2_draw_chip(0, &c2, &v);
}
static void s_chip_idle(void)             /* silent: the slot's mode, dim */
{
    struct c2_view v;
    base(4);
    c2.page = 0;
    view(&v);
    c2_draw_chip(0, &c2, &v);
}

struct scene { const char *name; void (*fn)(void); };
static const struct scene scenes[] = {
    { "map_play", s_map_play }, { "map_type_scan", s_map_type_scan }, { "map_top", s_map_top },
    { "map_voicing", s_map_voicing }, { "pinned_playing", s_pinned_playing },
    { "unpinned", s_unpinned }, { "write_factory", s_write_factory }, { "write_dorian", s_write_dorian },
    { "write_copy", s_write_copy }, { "write_overwrite", s_write_overwrite }, { "played", s_played },
    { "played_hold", s_played_hold }, { "map_after", s_map_after }, { "voicing_up", s_voicing_up },
    { "voicing_spread", s_voicing_spread }, { "learned_10th", s_learned_10th }, { "learned_wide", s_learned_wide },
    { "type_wide", s_type_wide }, { "map_wide", s_map_wide }, { "strum_shift", s_strum_shift },
    { "strum_header", s_strum_header },
    { "notes_offkb", s_notes_offkb }, { "learn", s_learn },
    { "learned", s_learned }, { "learned_slash", s_learned_slash }, { "map_shift", s_map_shift },
    { "cleared", s_cleared }, { "map_251", s_map_251 }, { "map_neo", s_map_neo }, { "map_pop", s_map_pop },
    { "unmapped", s_unmapped }, { "shape", s_shape }, { "off", s_off }, { "badge", s_badge }, { "chip", s_chip },
    { "chip_idle", s_chip_idle },
};
#define N_SCENES (int)(sizeof scenes / sizeof scenes[0])

static int check(void)
{
    int fails = 0;
#define EXPECT(c, msg) do { if (!(c)) { printf("FAIL %s\n", msg); fails++; } else printf("PASS %s\n", msg); } while (0)
    c2.page = 1;
    c2.dg.ctl_synth = 0;
    c2_frame_state(&c2, 0, 0);
    EXPECT(c2.page, "controller check never succeeded: the page stays open (the guess is not trusted)");
    c2_frame_state(&c2, 1, 0);
    EXPECT(c2.page && c2.dg.ctl_synth == 1, "synth controller current: the page stays open");
    c2_frame_state(&c2, 0, 0);
    EXPECT(!c2.page, "another controller once the check worked: the page closes");
    c2.page = 1;
    c2_frame_state(&c2, 1, 1);
    EXPECT(!c2.page, "drum play mode: the page closes");
    uint32_t f = c2.dg.frames, k = c2.clock;
    c2_frame_state(&c2, 1, 0);
    EXPECT(c2.dg.frames == f + 1 && c2.clock == k + 1, "one frame = one tick");
    /* the notes view: a chord knob turn, ~1 s, then the map view again; after a learn; SHAPE always */
    struct c2_view v;
    base(2);
    dn(7);
    up(7);
    tick(80);
    view(&v);
    EXPECT(!c2u_notes_view(&c2, &v), "a key played: the map view");
    knob(2, 1);
    view(&v);
    EXPECT(c2u_notes_view(&c2, &v), "a chord knob turned: the notes view");
    tick(C2_NOTES_SHOW + 1);
    view(&v);
    EXPECT(!c2u_notes_view(&c2, &v), "~1 s after the last turn: the map view again");
    knob(7, 1);
    view(&v);
    EXPECT(!c2u_notes_view(&c2, &v), "a SHIFT knob (map setting): stays the map view");
    shift(1);
    dn(4); dn(7);
    view(&v);
    EXPECT(c2u_notes_view(&c2, &v), "during a learn: the notes view");
    shift(0);
    up(4); up(7);
    view(&v);
    EXPECT(c2u_notes_view(&c2, &v) && c2.msg == MSG_LEARNED, "after a learn: the notes view");
    tick(C2_NOTES_SHOW + 20);
    view(&v);
    EXPECT(!c2u_notes_view(&c2, &v), "then the map view");
    /* the played-notes view: while keys are held and ~0.5 s after, then the map view (a HOLD pad included) */
    uint8_t col[24];
    int below, above;
    base(2);
    c2_clear(&c2.d.maps[0][7]);           /* the learn above wrote C4: C^7 again */
    c2.d.maps[0][7].q = Q_MAJ7;
    c2.d.set[0].bass = 0;                 /* BASS was turned on above */
    tick(80);
    view(&v);
    EXPECT(!c2u_played_view(&c2, &v), "nothing played yet: the map view");
    dn(7);
    dn(1);
    view(&v);
    EXPECT(c2u_played_view(&c2, &v), "keys held: the played-notes view");
    c2u_played_cols(&c2, col, &below, &above);
    EXPECT(col[7] == BLUE && col[11] == LIGHTBLUE && col[14] == LIGHTBLUE && col[18] == WHITE && col[1] == LIGHTBLUE &&
           !col[0] && !below && !above, "C^7 + F#: C (root) blue, B (top) white, E G F# light blue");
    up(1);
    up(7);
    tick(C2_PLAYED_SHOW - 2);
    view(&v);
    EXPECT(c2u_played_view(&c2, &v), "just released: still the played view (~0.5 s)");
    tick(4);
    view(&v);
    EXPECT(!c2u_played_view(&c2, &v), "0.5 s after the last key-up: the map view");
    hold_shiftkey(0, 7);
    dn(4);
    up(4);
    tick(C2_PLAYED_SHOW + 1);
    view(&v);
    EXPECT(!c2u_played_view(&c2, &v) && c2.sounding, "HOLD: the pad sounds on, the map view comes back after 0.5 s");
    hold_shiftkey(0, 7);
    octave[0] = 2;
    dn(23);
    c2u_played_cols(&c2, col, &below, &above);
    EXPECT(above >= 1 && col[23], "a chord past the top key: the arrow counts what is above");
    up(23);
    octave[0] = 0;
    int8_t iv[4] = { -9, -2, 0, 30 };
    c2u_notecols(col, iv, 4, 7, 0, 0, &below, &above);
    EXPECT(below == 1 && above == 1 && col[5] == LIGHTBLUE && col[7] == BLUE && !col[0], "off-keyboard notes are counted per side");
    int8_t iv2[3] = { 0, 4, 7 };
    c2u_notecols(col, iv2, 3, 0, 0, 0, &below, &above);
    EXPECT(col[0] == BLUE && col[4] == LIGHTBLUE && col[7] == WHITE && !below && !above, "root blue, top white, others light blue");
    /* every scene draws inside the safe area and the panel */
    printf(fails ? "FAILED\n" : "all passed\n");
    return fails != 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "list")) {
        for (int i = 0; i < N_SCENES; i++)
            printf("%s\n", scenes[i].name);
        return 0;
    }
    if (!strcmp(argv[1], "check"))
        return check();
    for (int i = 0; i < N_SCENES; i++)
        if (!strcmp(argv[1], scenes[i].name)) {
            scenes[i].fn();
            fprintf(stderr, "%d AA lines, %d fast lines\n", n_aa, n_fast);
            return 0;
        }
    fprintf(stderr, "unknown scene %s\n", argv[1]);
    return 2;
}
