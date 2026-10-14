/* Host preview harness for the screens in src/slot/ui_harm.h and src/slot/ui_fm4.h (screens_preview.py draws its
 * output). Stand-ins for ui.c's primitives with the same signatures and meaning; every segment is printed as
 * "x0 y0 x1 y1 width colour" (Q22.10 SVG units; width -1 = a 1-panel-pixel fast line).
 *   screens_main harm|fm4 K0..K7 [name=value ...]
 * harm: cursor=N last=KNOB stamp=N trail=FIRST:LAST (PAINT just wrote bars FIRST..LAST, LAST most recent) frame=N
 * fm4:  page=0..4 alg_shown=1 last=KNOB play=FRAMES (a note held that long, the gears animated over those
 *       frames; 0 = still)
 * both: badges=1 (HOLD and CHORD as ui.c draws them, to check overlaps), delta=N (the hook's raw-delta corner)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "font.h"
#include "dsp.h"
#include "sine_table.h"

#define RAM
typedef int32_t fp32;                     /* Q22.10 */
#define FP(x) ((fp32)(x) << 10)
enum { RED = 2, OFFRED = 3, OFFGREY = 4, DARKGREEN = 5, PURPLEGREY = 8, LIGHTBLUE = 9, DARKBLUE = 10, GREY = 12, ORANGE = 13,
       WHITE = 14, BLUE = 15, GREEN = 16, LIGHTGREEN = 18 };
static inline uint32_t cycles(void) { return 0; }

static int n_aa, n_fast, quiet;           /* quiet: frames before the one shown (animation) */
static void line_fp(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, fp32 w, int colour)
{
    (void)r;
    if (quiet)
        return;
    n_aa++;
    printf("%d %d %d %d %d %d\n", x0, y0, x1, y1, w, colour);
}
static void line_fast(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, int colour)
{
    (void)r;
    if (quiet)
        return;
    n_fast++;
    printf("%d %d %d %d -1 %d\n", x0, y0, x1, y1, colour);
}
static void vline_fast(void *r, fp32 x, fp32 y0, fp32 y1, int colour)
{
    line_fast(r, x, y0, x, y1, colour);
}
/* as ui.c */
static void text_any(void *r, const char *s, fp32 x, fp32 y, fp32 step, int colour, int fast)
{
    fp32 w = step >> 2 > FP(1) ? step >> 2 : FP(1);
    for (; *s; s++, x += 6 * step) {
        int c = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
        if (c < FONT_FIRST || c > FONT_LAST)
            continue;
        for (const uint8_t *g = font_seg + font_off[c - FONT_FIRST]; *g != 0xFF; g += 2) {
            fp32 x0 = x + (g[0] >> 4) * step, y0 = y + (g[0] & 15) * step;
            fp32 x1 = x + (g[1] >> 4) * step, y1 = y + (g[1] & 15) * step;
            if (fast)
                line_fast(r, x0, y0, x1, y1, colour);
            else
                line_fp(r, x0, y0, x1, y1, w, colour);
        }
    }
}
#define text(r, s, x, y, step, colour) text_any(r, s, x, y, step, colour, 0)
#define text_fast(r, s, x, y, step, colour) text_any(r, s, x, y, step, colour, 1)
static int32_t sin_t(uint32_t turn)
{
    uint32_t i = turn >> 24;
    int32_t f = (int32_t)((turn >> 9) & 0x7FFF);
    return sine256[i] + (((sine256[i + 1] - sine256[i]) * f) >> 15);
}
#define cos_t(t) sin_t((t) + 0x40000000u)

/* ui.c's HOLD and CHORD badges (outline only), to see what they cover */
static void badges(void *r)
{
    line_fp(r, FP(130), FP(5), FP(170), FP(5), FP(1), RED);
    line_fp(r, FP(170), FP(5), FP(170), FP(20), FP(1), RED);
    line_fp(r, FP(170), FP(20), FP(130), FP(20), FP(1), RED);
    line_fp(r, FP(130), FP(20), FP(130), FP(5), FP(1), RED);
    text(r, "HOLD", FP(133) + (FP(1) >> 1), FP(8), FP(3) >> 1, RED);
    line_fp(r, FP(130), FP(24), FP(180), FP(24), FP(1), RED);
    line_fp(r, FP(180), FP(24), FP(180), FP(39), FP(1), RED);
    line_fp(r, FP(180), FP(39), FP(130), FP(39), FP(1), RED);
    line_fp(r, FP(130), FP(39), FP(130), FP(24), FP(1), RED);
    text(r, "CHORD", FP(133) + (FP(1) >> 1), FP(27), FP(3) >> 1, RED);
    for (int s = 0; s <= 16; s++)
        vline_fast(r, FP(186) + s * FP(3), FP(24), (0x54A >> (s % 12)) & 1 ? FP(31) : FP(35), GREY);
}

/* the engines' shared RAM: the builder's scratch right after harm_ram, as on device */
#include "harm.h"
#include "wt_import.h"
static struct {
    struct harm_ram h;
    struct wti_work w;
} host_ram;
#define HARM_RAM (&host_ram.h)
#define HARM_WORK(h) (&host_ram.w)
#include "sprobe.h"
static struct kh_diag host_diag;          /* the knob hook's record (delta=N shows the raw-delta corner) */
#define HOOK_DIAG (&host_diag)
#include "ui_harm.h"
static uint8_t host_voices[8 * 4096];     /* fm4's voice_ram slots (fm4.c struct fm4_voice) */
#define FM4_VOICES host_voices
#include "ui_fm4.h"

/* One fm4 voice playing for f frames (30 per second) since note-on: envelopes as fm4.c runs them */
static void fake_voice(const int16_t *k, int f)
{
    struct fm4_patch p;
    fm4_decode(k, &p);
    struct fm4_voice *v = (struct fm4_voice *)host_voices;
    v->owner = OWNER_FM4;
    v->blocks += 23;
    double t = f / 30.0;
    for (int op = 0; op < FM4_OPS; op++) {
        int sh = p.env[op];
        double tau = pow(2.0, (fm4_env_tau[sh] + p.time_q16) / 65536.0);
        double s0 = fm4_env_start[sh], s1 = fm4_env_target[sh];
        double e = sh ? s1 + (s0 - s1) * exp(-t / tau) : 32767;
        v->env[op] = (int32_t)(e * 32768);
    }
}

static int opt(int argc, char **argv, const char *name, int dflt)
{
    size_t n = strlen(name);
    for (int i = 10; i < argc; i++)
        if (!strncmp(argv[i], name, n) && argv[i][n] == '=')
            return atoi(argv[i] + n + 1);
    return dflt;
}

/* harm_tick's rate limit: a knob turned every frame for 61 frames, then left; prints the builds and whether the last
 * edit was published within HARM_BUILD_GAP frames. */
static int ratelimit(void)
{
    static struct harm_ui u;
    harm_ui_reset(&u);
    int16_t k[8] = { harm_k0(0, 0), 0, 0x0F, 0, 0, 0, 0, 0 };
    harm_tick(k, &u);                       /* first build: immediate */
    uint32_t first = host_ram.h.builds;
    for (int f = 0; f < 61; f++) {
        u.frame++;
        harm_set_level(k, 1 + (f % 20), f & 15);
        harm_tick(k, &u);
    }
    uint32_t turning = host_ram.h.builds - first;
    int settled = -1;
    for (int f = 0; f < 10; f++) {
        u.frame++;
        harm_tick(k, &u);
        if (settled < 0 && host_ram.h.sig == harm_sig(k))
            settled = f + 1;
    }
    printf("ratelimit first %u turning %u of 61 frames, settled after %d frames, builds after %u\n", first, turning,
           settled, host_ram.h.builds - first - turning);
    return !(first == 1 && turning <= 61 / HARM_BUILD_GAP + 1 && turning >= 61 / HARM_BUILD_GAP - 1 && settled >= 1 &&
             settled <= HARM_BUILD_GAP && host_ram.h.builds - first - turning <= 1);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "ratelimit"))
        return ratelimit();
    if (argc < 10)
        return 2;
    int16_t k[8];
    for (int i = 0; i < 8; i++)
        k[i] = (int16_t)atoi(argv[2 + i]);
    void *r = 0;
    host_diag.delta = opt(argc, argv, "delta", 0);
    host_diag.dmin = host_diag.delta < 0 ? -host_diag.delta : host_diag.delta;
    host_diag.calls = host_diag.delta != 0;
    if (!strcmp(argv[1], "harm")) {
        static struct harm_ui u;
        harm_ui_reset(&u);
        u.frame = (uint32_t)opt(argc, argv, "frame", 100);
        u.cursor = opt(argc, argv, "cursor", 0);
        u.last = opt(argc, argv, "last", -1);
        u.last_frame = u.frame;
        u.stamp = opt(argc, argv, "stamp", 0);
        for (int i = 10; i < argc; i++)
            if (!strncmp(argv[i], "trail=", 6)) {
                int a = atoi(argv[i] + 6), b = atoi(strchr(argv[i], ':') + 1);
                for (int j = a; j <= b; j++)
                    u.painted[j] = u.frame + 1 - (uint32_t)(2 * (b - j));
            }
        draw_harm(r, k, &u);
    } else {
        static struct fm4_ui u;
        fm4_ui_reset(&u);
        u.frame = 100;
        u.page = opt(argc, argv, "page", 0);
        u.last = opt(argc, argv, "last", -1);
        u.last_frame = u.frame;
        u.alg_shown = opt(argc, argv, "alg_shown", 0) ? -1 : (k[1] < 0 ? 0 : (k[1] * 11) >> 15);
        for (int i = 0; i < 8; i++)
            fu_prev[i] = k[i];
        int play = opt(argc, argv, "play", 0);
        quiet = 1;
        for (int f = 0; f < play; f++) {
            fake_voice(k, f);
            u.frame--;                      /* keep the readouts' timing as without animation */
            draw_fm4(r, k, &u);
        }
        if (play)
            fake_voice(k, play);
        quiet = 0;
        draw_fm4(r, k, &u);
    }
    if (opt(argc, argv, "badges", 0))
        badges(r);
    fprintf(stderr, "aa %d fast %d\n", n_aa, n_fast);
    return 0;
}
