/* Host tests for the harm and fm4 edit functions (src/slot/harm_edit.h, fm4_edit.h), the harm table builder
 * (src/engine/harm_build.h) and fm4's knob decoding (src/engine/fm4.h).
 *   cc -std=gnu99 -O1 -Wall -Wextra -Werror -I src/slot -I src/engine src/slot/test/synth_edit_test.c -lm -o /tmp/se && /tmp/se
 * Prints FAIL lines and a summary; exits non-zero if anything failed. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "harm_build.h"
#include "harm_edit.h"
#include "fm4_edit.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                           printf("\n"); } } while (0)

static void levels_of(const int16_t *k, int *lv)
{
    for (int i = 0; i < HARM_N; i++)
        lv[i] = harm_level(k, i);
}

static void test_harm_pack(void)
{
    int16_t k[8] = { 0 };
    for (int i = 0; i < HARM_N; i++)
        harm_set_level(k, i, (i * 7 + 3) & 15);
    for (int i = 0; i < HARM_N; i++)
        CHECK(harm_level(k, i) == ((i * 7 + 3) & 15), "pack: level %d", i);
    for (int t = -16; t <= 15; t++)
        for (int e = 0; e <= 15; e++) {
            k[1] = harm_k1(t, e);
            CHECK(harm_tilt(k) == t && harm_extend(k) == e, "pack: tilt %d extend %d", t, e);
        }
    k[0] = harm_k0(63, 17);
    CHECK(harm_uni(k) == 63 && harm_warp(k) == 17 && ((uint16_t)k[0] >> 12) == HARM_VERSION, "pack: k0");
    /* every level 15 packs to -1 (0xFFFF): negative knob values must survive (range rows -32768..32767) */
    for (int i = 0; i < HARM_N; i++)
        harm_set_level(k, i, 15);
    CHECK(k[2] == -1 && k[7] == -1, "pack: full levels are -1");
}

static void test_harm_edit(void)
{
    struct harm_ui u;
    harm_ui_reset(&u);
    int16_t k[8] = { harm_k0(0, 0), harm_k1(0, 0), 0, 0, 0, 0, 0, 0 };
    int lv[HARM_N];

    /* cursor: clamps, saves nothing */
    CHECK(!harm_edit(&u, 0, -5, k) && u.cursor == 0, "cursor clamps at 1");
    CHECK(!harm_edit(&u, 0, 30, k) && u.cursor == 23, "cursor clamps at 24 (%d)", u.cursor);
    harm_edit(&u, 0, -20, k);
    CHECK(u.cursor == 3, "cursor 4 (%d)", u.cursor);

    /* level */
    CHECK(harm_edit(&u, 1, 9, k) && harm_level(k, 3) == 9, "level 9 at H4");
    harm_edit(&u, 1, 100, k);
    CHECK(harm_level(k, 3) == 15, "level clamps at 15");
    harm_edit(&u, 1, -100, k);
    CHECK(harm_level(k, 3) == 0, "level clamps at 0");
    harm_edit(&u, 1, 12, k);
    levels_of(k, lv);
    for (int i = 0; i < HARM_N; i++)
        CHECK(lv[i] == (i == 3 ? 12 : 0), "level edit touches only the cursor bar (%d: %d)", i, lv[i]);

    /* paint: brush = the bar it starts on; writes every bar entered; stops at the ends */
    u.frame = 50;
    CHECK(harm_edit(&u, 2, 4, k), "paint right changes knobs");
    CHECK(u.cursor == 7, "paint moved the cursor to H8 (%d)", u.cursor);
    for (int i = 4; i <= 7; i++)
        CHECK(harm_level(k, i) == 12 && u.painted[i] == 51, "paint wrote H%d", i + 1);
    CHECK(harm_level(k, 8) == 0 && u.painted[8] == 0, "paint stopped at H8");
    harm_edit(&u, 1, -12, k);              /* H8 off, then paint left with an off brush: erase */
    harm_edit(&u, 2, -2, k);
    CHECK(u.cursor == 5 && harm_level(k, 6) == 0 && harm_level(k, 5) == 0 && harm_level(k, 4) == 12,
          "paint with level 0 erases (cursor %d, %d %d %d)", u.cursor, harm_level(k, 6), harm_level(k, 5),
          harm_level(k, 4));
    harm_edit(&u, 2, -40, k);
    CHECK(u.cursor == 0, "paint stops at H1");

    /* macros: clamps, and the drawing is untouched */
    int16_t before[8];
    memcpy(before, k, sizeof before);
    harm_edit(&u, 3, 70, k);
    harm_edit(&u, 7, -3, k);
    harm_edit(&u, 4, -40, k);
    harm_edit(&u, 5, 9, k);
    CHECK(harm_uni(k) == 63 && harm_warp(k) == 0 && harm_tilt(k) == -16 && harm_extend(k) == 9, "macros clamp");
    CHECK(!memcmp(before + 2, k + 2, 12), "macros leave the drawing alone");
    harm_edit(&u, 4, 100, k);
    CHECK(harm_tilt(k) == 15, "tilt clamps at +15");

    /* stamps with one undo */
    int16_t own[8];
    memcpy(own, k, sizeof own);
    harm_edit(&u, 6, 1, k);
    levels_of(k, lv);
    CHECK(u.stamp == 1 && !memcmp(lv, (int[]){ 15, 13, 12, 11, 10, 10, 9, 9, 9, 8, 8, 8, 8, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6,
                                               6 }, sizeof lv), "stamp 1 is the saw");
    harm_edit(&u, 6, 4, k);
    levels_of(k, lv);
    CHECK(u.stamp == 5 && lv[0] == 15 && lv[1] == 15 && lv[2] == 0 && lv[3] == 15 && lv[4] == 0, "stamp 5 organ");
    harm_edit(&u, 6, 10, k);
    levels_of(k, lv);
    CHECK(u.stamp == HARM_STAMPS && !lv[0] && !lv[23], "stamp clamps at CLEAR");
    harm_edit(&u, 6, -100, k);
    CHECK(u.stamp == 0 && !memcmp(own + 2, k + 2, 12), "back to 0 restores the drawing");
    harm_edit(&u, 6, 2, k);                /* square, then a level edit (H2 0 -> 1) keeps it */
    harm_edit(&u, 0, 1, k);
    CHECK(u.stamp == 2, "moving the cursor keeps the stamp open");
    harm_edit(&u, 1, 1, k);
    CHECK(u.stamp == 0 && harm_level(k, 1) == 1 && harm_level(k, 2) == 12, "a level edit keeps the stamped drawing");
    harm_edit(&u, 6, -1, k);
    CHECK(u.stamp == 0 && harm_level(k, 2) == 12, "no undo after the stamp was kept");

    /* accumulator: with a delta shift, small deltas add up */
    harm_ui_reset(&u);
    memset(k, 0, sizeof k);
    for (int i = 0; i < 3; i++)
        harm_edit(&u, 0, 1, k);
    CHECK(u.cursor == 3, "deltas of 1 are steps at HARM_DELTA_SHIFT 0 (%d)", u.cursor);
    CHECK(harm_edit(&u, 9, 1, k) == 0 && harm_edit(&u, -1, 1, k) == 0, "bad knob index ignored");
    CHECK(((uint16_t)k[0] >> 12) == HARM_VERSION, "every edit writes the format version");
}

/* DFT magnitude of x[0..n) at bin h */
static double dft(const int16_t *x, int n, int h)
{
    double re = 0, im = 0;
    for (int j = 0; j < n; j++) {
        re += x[j] * cos(2 * M_PI * h * j / n);
        im -= x[j] * sin(2 * M_PI * h * j / n);
    }
    return 2 * sqrt(re * re + im * im) / n;
}

static struct wti_work work;
static int16_t frame[FRAME_STRIDE];

static void test_harm_build(void)
{
    int16_t k[8] = { harm_k0(0, 0), harm_k1(0, 0), 0, 0, 0, 0, 0, 0 };
    /* every stamp, plus tilt and extend extremes: band limits, peak, interpolation deltas, levels */
    for (int s = 0; s < HARM_STAMPS + 3; s++) {
        int tilt = s == HARM_STAMPS ? 15 : s == HARM_STAMPS + 1 ? -16 : 0, ext = s >= HARM_STAMPS ? 15 : 0;
        const uint8_t *lv = harm_stamp_lv[s < HARM_STAMPS ? s : 0];
        for (int i = 0; i < HARM_N; i++)
            harm_set_level(k, i, s == HARM_STAMPS + 2 ? 15 : lv[i]);
        k[1] = harm_k1(tilt, ext);
        harm_build(k, frame, &work);
        int peak = 0, dmax = 0;
        double leak = 0;
        for (int L = 0; L < LEVELS; L++) {
            const int16_t *x = frame + level_off[L];
            int n = 1 << level_bits[L], hmax = WTI_H >> L;
            for (int j = 0; j < n; j++) {
                int a = x[j] < 0 ? -x[j] : x[j], d = x[(j + 1) & (n - 1)] - x[j];
                peak = a > peak ? a : peak;
                dmax = (d < 0 ? -d : d) > dmax ? (d < 0 ? -d : d) : dmax;
            }
            for (int h = hmax + 1; h < n / 2; h += (L ? 1 : 7))
                leak = fmax(leak, dft(x, n, h));
        }
        CHECK(peak <= 29495, "build %d: peak %d <= 0.9 FS", s, peak);
        CHECK(dmax < 32767, "build %d: interpolation delta %d", s, dmax);
        CHECK(20 * log10(leak / 29490 + 1e-12) < -80, "build %d: above the level's limit %.1f dB", s,
              20 * log10(leak / 29490 + 1e-12));
        if (s == HARM_STAMPS + 2 || s == 5)
            continue;
        /* levels on the 2048-point level: -3 dB per step re the loudest, within 0.1 dB */
        double ref = 0, worst = 0;
        int top = 0;
        for (int i = 0; i < HARM_N; i++)
            if (lv[i] > top)
                top = lv[i];
        for (int i = 0; i < HARM_N; i++)
            if (lv[i] == top && !ref)
                ref = dft(frame, 2048, i + 1);
        for (int i = 0; i < HARM_N && !tilt && !ext; i++) {
            double got = dft(frame, 2048, i + 1);
            if (lv[i])
                worst = fmax(worst, fabs(20 * log10(got / ref) - 3.0 * (lv[i] - top)));
            else
                CHECK(got < ref * 1e-4, "build %d: H%d off (%.1f dB)", s, i + 1, 20 * log10(got / ref + 1e-12));
        }
        CHECK(worst < 0.1, "build %d: levels within %.3f dB", s, worst);
    }
    /* RMS rule: one sine peaks at 0.6 FS; a saw has the same RMS; lowering one bar does not raise the others */
    memset(k + 2, 0, 12);
    harm_set_level(k, 0, 15);
    k[1] = 0;
    harm_build(k, frame, &work);
    double rs = 0, rsaw = 0;
    int pk = 0;
    for (int j = 0; j < 2048; j++) {
        rs += (double)frame[j] * frame[j];
        pk = abs(frame[j]) > pk ? abs(frame[j]) : pk;
    }
    CHECK(abs(pk - 19660) < 20, "one sine peaks at 0.6 FS (%d)", pk);
    for (int i = 0; i < HARM_N; i++)
        harm_set_level(k, i, harm_stamp_lv[0][i]);
    harm_build(k, frame, &work);
    double h1 = dft(frame, 2048, 1);
    for (int j = 0; j < 2048; j++)
        rsaw += (double)frame[j] * frame[j];
    CHECK(fabs(10 * log10(rsaw / rs)) < 0.2, "saw RMS = sine RMS (%.2f dB)", 10 * log10(rsaw / rs));
    harm_set_level(k, 5, 1);
    harm_build(k, frame, &work);
    CHECK(fabs(dft(frame, 2048, 1) / h1 - 1) < 0.02, "lowering H6 changes H1 by %.3f dB",
          20 * log10(dft(frame, 2048, 1) / h1));
    /* tilt: -12 steps = -9 dB per octave: H8 falls 27 dB more than drawn */
    double a1 = dft(frame, 2048, 1), a8 = dft(frame, 2048, 8);
    k[1] = harm_k1(-12, 0);
    harm_build(k, frame, &work);
    double t = 20 * log10(dft(frame, 2048, 8) / dft(frame, 2048, 1)) - 20 * log10(a8 / a1);
    CHECK(fabs(t + 27) < 0.2, "tilt -12: H8 %.2f dB re untilted (expect -27)", t);
    /* extend: H25..36 repeat H13..24, (16 - e) * 1.5 dB down */
    k[1] = harm_k1(0, 12);
    harm_build(k, frame, &work);
    double e1 = 20 * log10(dft(frame, 2048, 25) / dft(frame, 2048, 13)), e2 = 20 * log10(dft(frame, 2048, 37) /
                                                                                       dft(frame, 2048, 13));
    CHECK(fabs(e1 + 6) < 0.2 && fabs(e2 + 12) < 0.2, "extend 12: H25 %.2f dB, H37 %.2f dB re H13 (-6, -12)", e1, e2);
    /* nothing drawn: silence */
    memset(k + 2, 0, 12);
    k[1] = harm_k1(5, 15);
    harm_build(k, frame, &work);
    int any = 0;
    for (int j = 0; j < FRAME_STRIDE; j++)
        any |= frame[j];
    CHECK(!any, "no bars: a silent frame");
}

static void test_fm4(void)
{
    struct fm4_patch p;
    int16_t k[8] = { 16384, 0, 16384, 0, 2048, 3072, 1024, 0 };    /* v0 */
    fm4_decode(k, &p);
    CHECK(!p.packed && p.ratio[0] == 65536 && p.ratio[1] == 2 * 65536 && p.ratio[2] == 3 * 65536 &&
          p.ratio[3] == 65536, "v0 ratios 1 2 3 1 (%u %u %u %u)", p.ratio[0], p.ratio[1], p.ratio[2], p.ratio[3]);
    CHECK(p.env[0] == 0 && p.env[1] == FM4_ENV_V0 && p.level[3] == 15 && p.index == 8192, "v0 envelopes, levels, index");
    for (int a = 0; a < FM4_ALGS; a++) {
        k[1] = (int16_t)((a * 32768 + 16384) / 11);
        CHECK(fm4_alg_of(k[1]) == a, "algorithm %d from its knob", a + 1);
    }
    CHECK(fm4_alg_of(32767) == 10 && fm4_alg_of(0) == 0 && fm4_alg_of(-5) == 0, "algorithm ends");
    for (int c = 0; c < FM4_COARSES; c++)
        for (int f = 0; f < FM4_FINES; f++) {
            int16_t q[8] = { 0, 0, 0, 0, fm4_pack(c, f, 9, 3), fm4_pack(31, 7, 15, 7), fm4_pack(0, 0, 0, 0),
                             fm4_pack(1, 0, 15, 0) };
            fm4_decode(q, &p);
            CHECK(p.packed && p.coarse[0] == c && p.fine[0] == f && p.level[0] == 9 && p.env[0] == 3 &&
                  p.coarse[1] == 31 && p.level[2] == 0, "pack round trip %d %d", c, f);
        }
    int16_t q[8] = { 0, 0, 0, 0, fm4_pack(13, 6, 0, 0), fm4_pack(0, 7, 0, 0), fm4_pack(2, 5, 0, 0), fm4_pack(1, 0, 0, 0) };
    fm4_decode(q, &p);
    CHECK(p.ratio[0] == 13 * 65536 + 32768 && p.ratio[1] == 46341 && p.ratio[2] == 2 * 65536 + 26870,
          "fine: 13 + 0.5, 0.5 x sqrt 2, 2 + 0.41 (%u %u %u)", p.ratio[0], p.ratio[1], p.ratio[2]);
    char buf[16];
    fm4_ratio_text(buf, p.ratio[0]);
    CHECK(!strcmp(buf, "13.5"), "ratio text %s", buf);
    fm4_ratio_text(buf, p.ratio[1]);
    CHECK(!strcmp(buf, "0.71"), "ratio text %s", buf);
    fm4_ratio_text(buf, 65536);
    CHECK(!strcmp(buf, "1"), "ratio text %s", buf);
    fm4_ratio_text(buf, 2 * 65536 + 26870);
    CHECK(!strcmp(buf, "2.41"), "ratio text %s", buf);

    /* edit: MAIN passes plain knobs to TE; SHIFT blue pages; operator pages pack */
    struct fm4_ui u;
    fm4_ui_reset(&u);
    int16_t v0[8] = { 16384, 6000, 16384, 0, 2048, 3072, 1024, 9000 };
    memcpy(k, v0, sizeof k);
    CHECK(fm4_edit(&u, 0, 3, k) == FM4_PASS && fm4_edit(&u, 3, -1, k) == FM4_PASS, "MAIN: plain knobs pass");
    CHECK(fm4_edit(&u, 5, 1, k) == FM4_PASS && fm4_edit(&u, 7, 1, k) == FM4_PASS, "MAIN, v0 sound: SHIFT knobs pass");
    CHECK(!memcmp(k, v0, sizeof k), "passing leaves the knobs to TE");
    CHECK(fm4_edit(&u, 4, 2, k) == FM4_DONE && u.page == 2, "SHIFT blue: page B");
    CHECK(fm4_is_packed(k), "entering an operator page packs the sound");
    fm4_decode(k, &p);
    CHECK(p.ratio[1] == 2 * 65536 && p.ratio[2] == 3 * 65536 && p.ratio[3] == 65536 && p.env[1] == FM4_ENV_V0 &&
          p.env[0] == 0, "packing keeps the v0 ratios and envelopes");
    fm4_edit(&u, 0, 3, k);                 /* B coarse 2 -> 5 */
    fm4_edit(&u, 1, 6, k);                 /* fine +0.5 */
    fm4_edit(&u, 2, -6, k);                /* level 9 */
    fm4_edit(&u, 3, 10, k);                /* envelope clamps at 7 */
    fm4_decode(k, &p);
    CHECK(p.ratio[1] == 5 * 65536 + 32768 && p.level[1] == 9 && p.env[1] == 7 && p.ratio[0] == 65536,
          "page B edits B only (%u %d %d)", p.ratio[1], p.level[1], p.env[1]);
    CHECK(fm4_edit(&u, 6, 1, k) == FM4_DONE, "operator page: SHIFT white ignored");
    fm4_edit(&u, 4, -10, k);
    CHECK(u.page == 0 && fm4_edit(&u, 1, 1, k) == FM4_PASS, "back to MAIN");
    CHECK(fm4_edit(&u, 5, 1, k) == FM4_DONE, "MAIN, packed sound: SHIFT green ignored");
    fm4_edit(&u, 4, 9, k);
    CHECK(u.page == 4, "page clamps at D");
    fm4_edit(&u, 0, -40, k);
    fm4_decode(k, &p);
    CHECK(p.coarse[3] == 0 && p.ratio[3] == 32768, "D coarse clamps at 0.5");
}

int main(void)
{
    test_harm_pack();
    test_harm_edit();
    test_harm_build();
    test_fm4();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
