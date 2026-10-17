/* Host gallery of the CHORD v2 screens (ui_chord2.h, the code ui.c draws on the device), with the engine
 * (hold_impl.h, chord2_impl.h) driven through its real hooks to set each scene up. chord2_screens.py renders the output.
 *   chord2_screens list | SCENE | check
 * A scene prints one "x0 y0 x1 y1 width colour" per segment (Q22.10 SVG units; width -1 = a 1-panel-pixel fast line);
 * the AA / fast line counts go to stderr. `check` runs the frame hook's page-closing rules. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
static void s_map_play(void)              /* DIATONIC C, G4 held */
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
    knob(3, 1);
    page();
}
static void s_frozen_playing(void)        /* C4 edited (frozen) while G7 is played */
{
    base(2);
    dn(7);
    up(7);
    knob(3, 1);
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
    shift_down = 1;
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
    { "map_voicing", s_map_voicing }, { "frozen_playing", s_frozen_playing }, { "learn", s_learn },
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
