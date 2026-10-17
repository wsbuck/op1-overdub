/* Host harness for the CHORD v2 mockups (design/chord2/render.py draws its output; design only).
 * Stand-ins for ui.c's primitives with the same signatures and meaning (as src/slot/test/screens_main.c); every
 * segment is printed as "x0 y0 x1 y1 width colour" (Q22.10 SVG units; width -1 = a 1-panel-pixel fast line), and the
 * AA / fast line counts go to stderr.
 *   chord2_preview SCENE      draw one screen
 *   chord2_preview model      model checks (recognition, voicings, fill, voice leading); exit 1 on failure
 *   chord2_preview list       the scene names
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

#include "chord2_ui.h"
#include "chord2_factory.h"

static struct c2_ui U;

static void base(void)
{
    memset(&U, 0, sizeof U);
    U.mode = 2;
    U.map = 1;
    U.map_name = "DIATONIC C";
    U.sel = -1;
    U.turned = -1;
    U.playing = -1;
    c2_fill(U.keys, 0, S_MAJOR, 1);
    c2_clear(&U.shape);
    U.shape.q = Q_MIN7;
}

static void factory(int i)
{
    static const char *names[4] = { "DIATONIC C", "II-V-I", "NEO SOUL", "POP" };
    U.map = i + 1;
    U.map_name = names[i];
    if (i == 1)
        c2_load(U.keys, c2_f_251, sizeof c2_f_251 / sizeof c2_f_251[0]);
    else if (i == 2)
        c2_load(U.keys, c2_f_neo, sizeof c2_f_neo / sizeof c2_f_neo[0]);
    else if (i == 3)
        c2_load(U.keys, c2_f_pop, sizeof c2_f_pop / sizeof c2_f_pop[0]);
    U.smooth = c2_factory_set[i].smooth;
    U.bass = c2_factory_set[i].bass;
    U.flats = c2_factory_set[i].flats;
}

static void sound(int key)
{
    U.sounding |= 1u << key;
    U.sel = key;
    U.playing = key;
}

/* ---- scenes ---- */
static void s_map_play(void *r)              /* playing DIATONIC C: G4 held */
{
    base();
    sound(14);
    U.locked = 1;
    c2_page_map(r, &U);
}
static void s_map_edit(void *r)              /* editing C4's chord: white turned two steps: E on top -> G on top */
{
    base();
    U.sel = 7;
    U.locked = 1;
    U.keys[7].top = 2;
    U.turned = 2;
    c2_page_map(r, &U);
}
static void s_map_voicing(void *r)           /* orange: DROP 2 */
{
    base();
    U.sel = 7;
    U.locked = 1;
    U.keys[7].fam = F_DROP2;
    U.turned = 3;
    c2_page_map(r, &U);
}
static void s_map_shift(void *r)             /* SHIFT held: the map layer on the knobs */
{
    base();
    factory(1);
    U.sel = 7;
    U.shift = 1;
    U.strum = 30;
    U.turned = 0;
    c2_page_map(r, &U);
}
static void s_learn(void *r)                 /* SHIFT + A C E G learned into C4 (the edited key) */
{
    base();
    U.sel = 7;
    U.learning = 1;
    U.learn_n = 4;
    int8_t iv[4] = { -3, 0, 4, 7 };
    memcpy(U.learn_iv, iv, 4);
    U.sounding = (1u << 4) | (1u << 7) | (1u << 11) | (1u << 14);
    c2_page_map(r, &U);
}
static void s_learned(void *r)               /* after SHIFT is let go: C4 = A-7 as played */
{
    base();
    U.sel = 7;
    struct c2_chord *c = &U.keys[7];
    c->q = Q_CUSTOM;
    c->n = 4;
    int8_t iv[4] = { -3, 0, 4, 7 };
    memcpy(c->iv, iv, 4);
    U.flash = "C4 = A-7  SHIFT+LIFT UNDOES";
    c2_page_map(r, &U);
}
static void s_frozen(void *r)                /* C4 edited (frozen by a knob turn) while G7 is played */
{
    base();
    U.sel = 7;
    U.locked = 1;
    U.turned = 3;
    U.keys[7].fam = F_SPREAD;
    U.sounding = 1u << 14;
    U.playing = 14;
    c2_page_map(r, &U);
}
static void s_slash(void *r)                 /* learned B D G on C4: G/B, kept as a slash bass after TYPE -> ^7 */
{
    base();
    U.sel = 7;
    struct c2_chord *c = &U.keys[7];
    c->q = Q_CUSTOM;
    c->n = 3;
    int8_t iv[3] = { 4, 7, 12 };            /* E? no: B D G from C4 = -1 2 7 */
    iv[0] = -1;
    iv[1] = 2;
    iv[2] = 7;
    memcpy(c->iv, iv, 3);
    c2_structure(c, Q_MAJ7);
    U.turned = 1;
    U.locked = 1;
    c2_page_map(r, &U);
}
static void s_251(void *r)                   /* factory II-V-I, Bb group: F7 sounding, SMOOTH */
{
    base();
    factory(1);
    sound(7);
    c2_page_map(r, &U);
}
static void s_neo(void *r)                   /* factory NEO SOUL with HOLD, strum, bass */
{
    base();
    factory(2);
    U.hold = 1;
    U.strum = 20;
    sound(3);
    c2_page_map(r, &U);
}
static void s_pop(void *r)
{
    base();
    factory(3);
    sound(9);
    c2_page_map(r, &U);
}
static struct c2_chord preview[24];
static void s_fill(void *r)                  /* WRITE: F major sevenths into empty MAP 5 */
{
    base();
    U.fill_src = SRC_SCALE;
    U.fill_tonic = 5;
    U.fill_scale = S_MAJOR;
    U.fill_type = 1;
    U.fill_slot = 5;
    U.flats = 1;
    U.turned = 1;
    c2_fill(preview, 5, S_MAJOR, 1);
    c2_page_fill(r, &U, preview);
}
static void s_fill_confirm(void *r)          /* WRITE: factory NEO SOUL over MAP 1, confirm */
{
    base();
    U.fill_src = SRC_FACTORY;
    U.fill_tonic = 2;
    U.fill_slot = 1;
    U.fill_confirm = 1;
    U.flats = 1;
    U.turned = 0;
    c2_load(preview, c2_f_neo, sizeof c2_f_neo / sizeof c2_f_neo[0]);
    c2_page_fill(r, &U, preview);
}
static void s_copy(void *r)                  /* WRITE: a copy of MAP 2 up 2 semitones into MAP 6 */
{
    base();
    U.fill_src = SRC_MAP;
    U.fill_scale = 2;
    U.fill_tonic = 2;
    U.fill_slot = 6;
    U.flats = 1;
    U.turned = 1;
    c2_load(preview, c2_f_251, sizeof c2_f_251 / sizeof c2_f_251[0]);
    for (int k = 0; k < 24; k++)
        if (preview[k].q != Q_NONE)
            preview[k].root += 2;
    c2_page_fill(r, &U, preview);
}
static void s_shape(void *r)                 /* SHAPE mode: -7 shape, drop 2, played on C4 */
{
    base();
    U.mode = 1;
    U.shape.q = Q_MIN7;
    U.shape.fam = F_DROP2;
    U.sel = 7;
    c2_page_shape(r, &U);
}
static void s_unmapped(void *r)              /* an unmapped key edited: plays its note */
{
    base();
    U.sel = 8;
    c2_page_map(r, &U);
}
static void s_cleared(void *r)               /* SPLIT + key cleared F4: undo offered */
{
    base();
    c2_clear(&U.keys[12]);
    U.sel = 12;
    U.flash = "F4 CLEARED  SHIFT+LIFT UNDOES";
    c2_page_map(r, &U);
}
static void s_badge(void *r)                 /* our engine screens: the badge (no engine drawing here) */
{
    base();
    U.hold = 1;
    c2_badge(r, &U, "G7");
}
static void s_chip(void *r)                  /* TE screens: the chip, top right */
{
    base();
    c2_chip(r, &U, "B&^7", 0);
}
static void s_chip_idle(void *r)             /* TE screens, nothing sounding: the mode, dim */
{
    base();
    U.map = 3;
    c2_chip(r, &U, 0, 0);
}
static void s_badge_seq(void *r)             /* the arp is on: chords bypassed, said so */
{
    base();
    U.seq = 1;
    c2_badge(r, &U, "G7");
}

static const struct { const char *name; void (*fn)(void *); } scenes[] = {
    { "map_play", s_map_play }, { "map_edit_top", s_map_edit }, { "map_edit_voicing", s_map_voicing },
    { "frozen_playing", s_frozen }, { "learned_slash", s_slash }, { "write_copy", s_copy },
    { "chip_idle", s_chip_idle }, { "badge_seq", s_badge_seq },
    { "map_shift", s_map_shift }, { "learn", s_learn }, { "learned", s_learned }, { "map_251", s_251 },
    { "map_neo", s_neo }, { "map_pop", s_pop }, { "write_scale", s_fill }, { "write_confirm", s_fill_confirm },
    { "shape", s_shape }, { "unmapped", s_unmapped }, { "cleared", s_cleared }, { "badge", s_badge },
    { "chip", s_chip },
};
#define N_SCENES (int)(sizeof scenes / sizeof scenes[0])

/* ---- model checks ---- */
static int fails;
static void check(int ok, const char *what)
{
    if (!ok)
        fails++;
    fprintf(stderr, "%s %s\n", ok ? "pass" : "FAIL", what);
}

static int model(void)
{
    char b[16];
    int root;
    /* every quality, family, TOP and root: the played notes always get a name with the same notes; in close root
     * position the root is kept unless the chord has an enharmonic twin (6 = -7, -6 = $7, *7, +, rootless voicings) */
    int total = 0, named = 0, close = 0, close_same = 0;
    for (int q = 0; q < Q_COUNT; q++)
        for (int f = 0; f < F_COUNT; f++)
            for (int top = -3; top <= 4; top++)
                for (int rt = -5; rt <= 6; rt++) {
                    struct c2_chord c = { (int8_t)rt, (uint8_t)q, (uint8_t)f, (int8_t)top, { 0 }, 0, 0, 0 };
                    int8_t iv[C2_NOTES];
                    int n = c2_notes(&c, 0, iv);
                    int got = c2_recognise(iv, n, &root);
                    total++;
                    named += got != Q_CUSTOM && c2_mask(c2_q[got].iv, n, root) == c2_mask(iv, n, 0);
                    int twin = q == Q_6 || q == Q_MIN6 || q == Q_DIM7 || q == Q_AUG || q == Q_69 || c2_q[q].rootless;
                    if (f == F_CLOSE && top == 0 && !twin) {
                        close++;
                        close_same += got == q && c2_mod12(root - rt) == 0;
                    }
                }
    fprintf(stderr, "named %d/%d, close root position kept %d/%d\n", named, total, close_same, close);
    check(named == total, "every voiced chord gets a name with its notes");
    check(close_same == close, "close root position: root and quality kept");
    int8_t am7[4] = { -3, 0, 4, 7 };
    check(c2_recognise(am7, 4, &root) == Q_MIN7 && c2_mod12(root + 3) == 0, "A C E G = A-7");
    int8_t c6[4] = { 0, 4, 7, 9 };
    check(c2_recognise(c6, 4, &root) == Q_6 && root == 0, "C E G A with C in the bass = C6");
    struct c2_chord c = { 0, Q_MAJ7, F_CLOSE, 0, { 0 }, 0, 0, 0 };
    c2_name(b, &c, 7, 0, 0);
    check(!strcmp(b, "C^7"), "key C, ^7 -> C^7");
    struct c2_chord h = { 0, Q_HDIM, F_CLOSE, 0, { 0 }, 0, 0, 0 };
    c2_name(b, &h, 6, 0, 1);
    check(!strcmp(b, "B$"), "key B, half-dim tag -> B$");
    int8_t iv[C2_NOTES];
    int n;
    struct c2_chord d2 = { 0, Q_MAJ7, F_DROP2, 0, { 0 }, 0, 0, 0 };
    c2_notes(&d2, 0, iv);
    check(iv[0] == -5 && iv[1] == 0 && iv[2] == 4 && iv[3] == 11, "C^7 drop 2 = G C E B");
    /* TOP ladder: each step puts the next chord tone on top; n steps = an octave */
    int ok = 1, prev_top = -99;
    for (int t = -4; t <= 8; t++) {
        struct c2_chord l = { 0, Q_MAJ7, F_CLOSE, (int8_t)t, { 0 }, 0, 0, 0 };
        int n = c2_notes(&l, 0, iv);
        ok &= iv[n - 1] > prev_top;
        prev_top = iv[n - 1];
    }
    check(ok, "TOP ladder: the top note rises with every step");
    struct c2_chord l4 = { 0, Q_MAJ7, F_CLOSE, 4, { 0 }, 0, 0, 0 };
    c2_notes(&l4, 0, iv);
    check(iv[0] == 12 && iv[3] == 23, "TOP +4 on a 4-note chord = one octave up");
    ok = 1;                                 /* VOICING never moves the top note */
    for (int q = 0; q < Q_COUNT; q++)
        for (int t = -3; t <= 5; t++) {
            struct c2_chord a = { 0, (uint8_t)q, F_CLOSE, (int8_t)t, { 0 }, 0, 0, 0 };
            int na = c2_notes(&a, 0, iv), top = iv[na - 1];
            for (int f = 1; f < F_COUNT; f++) {
                a.fam = (uint8_t)f;
                na = c2_notes(&a, 0, iv);
                ok &= iv[na - 1] == top;
            }
        }
    check(ok, "every VOICING keeps the top note TOP chose");
    struct c2_chord gb = { 0, Q_CUSTOM, 0, 0, { -1, 2, 7 }, 3, 0, 0 };
    c2_name(b, &gb, 7, 0, 0);
    check(!strcmp(b, "G/B"), "learned B D G on C = G/B");
    c2_structure(&gb, Q_MAJ7);
    c2_name(b, &gb, 7, 0, 0);
    n = c2_notes(&gb, 0, iv);
    check(!strcmp(b, "G^7/B") && iv[0] == -1 - 12 * (iv[1] < -1), "TYPE on G/B keeps the B bass: G^7/B");
    struct c2_chord t1 = { 0, Q_MAJ, F_CLOSE, 1, { 0 }, 0, 0, 0 };
    c2_notes(&t1, 0, iv);
    check(iv[0] == 4 && iv[1] == 7 && iv[2] == 12, "C, TOP +1 = E G C");
    /* bass: the root under the chord */
    struct c2_chord m9 = { 0, Q_MIN9, F_CLOSE, 0, { 0 }, 0, 0, 0 };
    n = c2_notes(&m9, 1, iv);
    check(n == 5 && iv[0] == -12 && iv[1] == 3, "C-9 rootless + BASS -1 = C2 under Eb G Bb D");
    /* fill: C major sevenths on F..E: F^7 G7 A-7 B$7 C^7 D-7 E-7, black keys plain */
    struct c2_chord map[24];
    c2_fill(map, 0, S_MAJOR, 1);
    check(map[0].q == Q_MAJ7 && map[2].q == Q_7 && map[4].q == Q_MIN7 && map[6].q == Q_HDIM && map[7].q == Q_MAJ7 &&
          map[9].q == Q_MIN7 && map[11].q == Q_MIN7 && map[1].q == Q_NONE, "fill C major 7ths");
    c2_fill(map, 5, S_MAJOR, 1);
    check(map[5].q == Q_MAJ7 && map[11].q == Q_HDIM && map[4].q == Q_MIN7, "fill F major: Bb^7 on A#, A-7 on A, E$7 on E");
    /* SMOOTH: G-7 (F key) -> C7 (G key) -> F^7 (A key): each change moves <= 4 semitones in total */
    struct c2_chord g = { 2, Q_MIN7, 0, 0, { 0 }, 0, 0, 0 }, c7 = { 5, Q_7, 0, 0, { 0 }, 0, 0, 0 }, f = { -4, Q_MAJ7, 0, 0, { 0 }, 0, 0, 0 };
    int8_t prev[C2_NOTES], t[C2_MAX];
    n = c2_notes(&g, 0, prev);
    int worst = 0;
    const struct c2_chord *seq[2] = { &c7, &f };
    int keys[2] = { 2, 4 };
    for (int s = 0; s < 2; s++) {
        int top = c2_smooth(seq[s], keys[s], prev, 4, 7);
        n = c2_voice(&c2_q[seq[s]->q], seq[s]->fam, top, t);
        int moved = 0;
        for (int i = 0; i < n; i++) {
            t[i] += keys[s] + seq[s]->root;
            int dd = t[i] - prev[i];
            moved += dd < 0 ? -dd : dd;
            prev[i] = t[i];
        }
        worst = moved > worst ? moved : worst;
    }
    check(worst <= 4, "SMOOTH ii-V-I in F: each change moves <= 4 semitones in total");
    check(c2_smooth(&c7, 2, prev, 0, 7) == c7.top, "SMOOTH after silence: the chord's own TOP (deterministic)");
    /* every factory chord: <= 4 tones (+ bass), intervals within +-35 of the key */
    const struct c2_entry *f3[3] = { c2_f_251, c2_f_neo, c2_f_pop };
    int nf[3] = { sizeof c2_f_251 / sizeof c2_f_251[0], sizeof c2_f_neo / sizeof c2_f_neo[0],
                  sizeof c2_f_pop / sizeof c2_f_pop[0] };
    ok = 1;
    for (int m = 0; m < 3; m++) {
        c2_load(map, f3[m], nf[m]);
        for (int k = 0; k < 24; k++) {
            n = c2_notes(&map[k], c2_factory_set[m + 1].bass, iv);
            for (int i = 0; i < n; i++)
                ok &= n <= C2_NOTES && iv[i] >= -35 && iv[i] <= 35;
        }
    }
    check(ok, "factory maps: <= 4 tones + bass, intervals within +-35");
    return fails ? 1 : 0;
}

/* dump a map as text for notes/chord2.md */
static void dump(const char *which)
{
    struct c2_chord map[24];
    int flats = 1;
    if (!strcmp(which, "251"))
        c2_load(map, c2_f_251, sizeof c2_f_251 / sizeof c2_f_251[0]);
    else if (!strcmp(which, "neo"))
        c2_load(map, c2_f_neo, sizeof c2_f_neo / sizeof c2_f_neo[0]);
    else if (!strcmp(which, "pop"))
        c2_load(map, c2_f_pop, sizeof c2_f_pop / sizeof c2_f_pop[0]);
    else {
        c2_fill(map, 0, S_MAJOR, 1);
        flats = 0;
    }
    for (int k = 0; k < 24; k++) {
        char b[16];
        if (map[k].q == Q_NONE)
            continue;
        c2_name(b, &map[k], k, flats, 0);
        printf("%s%d=%s ", (flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(k)], k < 7 ? 3 : k < 19 ? 4 : 5, b);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "list")) {
        for (int i = 0; i < N_SCENES; i++)
            printf("%s\n", scenes[i].name);
        return 0;
    }
    if (!strcmp(argv[1], "model"))
        return model();
    if (!strcmp(argv[1], "dump")) {
        dump(argc > 2 ? argv[2] : "diatonic");
        return 0;
    }
    for (int i = 0; i < N_SCENES; i++)
        if (!strcmp(argv[1], scenes[i].name)) {
            scenes[i].fn(0);
            fprintf(stderr, "%d AA lines, %d fast lines\n", n_aa, n_fast);
            return 0;
        }
    fprintf(stderr, "unknown scene %s\n", argv[1]);
    return 2;
}
