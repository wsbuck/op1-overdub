/* Screens for our engines (ids >= 15), drawn instead of iter's dials on the shared iter screen (0x14).
 *
 * Called from _ui_update (slot.S) once per display frame, after TE has cleared the frame, only when the
 * current synth slot holds one of our engines. Immediate mode on TE's raster (notes/screen.md):
 *   te_line  FUN_01a24a4c(raster, Q22.10 point a, b, Q22.10 width, const uint8_t rgba[4] *)  AGG thick line
 * Landscape SVG space, 320 wide, y down, scaled by TE to the panel. Safe area x 4..316, y 2..140.
 * Text is our stroke font (font.h), drawn with te_line so it scales with the panel.
 * No libc/libgcc: no division, no floats, no writable globals (this links into the slot block).
 *
 * Design: notes/screen.md "UI redesign" (TE-style: dark structure, live elements in the knob colours,
 * iter's dials, sampler's HOLD badge). wavetable: waterfall of the bank's frames; grain: source overview,
 * playhead, spray, live grains (sampler or built-in bank); va: oscillator shape and filter response;
 * inprobe (a test engine): the input buffer it reads, live, with numbers to report.
 * sprobe (a test engine): the 8 stored knob values, what the encoder hook (knobhook.c) wrote, and the render params.
 * amp: a small combo amp in an oblique view (ui_amp.h): drive rumbles and sparks, tone, gate meter, cab speakers, tuner.
 * harm (ui_harm.h): the 24 drawn harmonic bars and the table it builds; fm4 (ui_fm4.h): operators as a gear train.
 * Both draw from the 8 stored knobs (never the LFO-modulated live ones) and the encoder hook's edit state (knobhook.c).
 * drum_ui_draw: our drum engines on the dbox screen they borrow, by kind: the drum probe (a test drum engine: what TE
 * handed the engine) and fmdrum (ui_fmdrum.h: the selected key's FM drum, its envelopes, the 24 keys).
 */
#include <stdint.h>
#include "font.h"
#include "dsp.h"
#include "warp.h"
#include "wt_layout.h"
#include "wt_user.h"
#include "wt_import.h"
#include "modal_state.h"
#include "grain_layout.h"
#include "grain_state.h"
#include "grain_source.h"
#include "ensemble.h"
#include "pingpong.h"
#include "crush.h"
#include "fxpub.h"
#include "inprobe.h"
#include "drumprobe.h"
#include "sprobe.h"
#include "amp.h"
#include "amp_tune.h"

/* Writable state lives in .data (the slot block is loaded into RAM); .bss is refused by the linker. */
#ifdef __bfin__
#define RAM __attribute__((section(".data")))
static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c));
    return c;
}
#else
#define RAM
static inline uint32_t cycles(void) { return 0; }
#endif

typedef int32_t fp32;                     /* Q22.10 */
#define FP(x) ((fp32)(x) << 10)

extern void *te_raster(void);
extern void te_line(void *raster, const fp32 *a, const fp32 *b, fp32 width, const uint8_t *rgba);
/* Live knobs come from the render trampoline (tramp.S copies each call's params). An earlier version also
 * popped TE's DSP->GUI mailbox (FUN_0196749c) but passed the record in R0; TE's function takes it in R1, so
 * it wrote 28 bytes through a stale register: memory corruption and, once, a misaligned-write exception
 * (LOG.md 2026-10-08). Removed. */
extern const int16_t wt_data[] __attribute__((weak));         /* wt engine's tables (build_slot.py: --defsym _wt_data) */
extern uint8_t voice_ram[];               /* engines' per-voice RAM (FREE region 0, 0x00810000) */
extern uint32_t hold_on;                  /* hold.c */
extern uint32_t chord_on, chord_learning; /* hold.c: CHORD mode and its learned shape */
extern int32_t chord_n, chord_iv[];
extern const uint8_t ui_kind[];           /* screen kind per engine id 15.. (build_slot.py, from engines.json) */
enum { KIND_VA = 1, KIND_WAVETABLE = 2, KIND_MODAL = 3, KIND_GRAIN = 4, KIND_INPROBE = 5, KIND_SPROBE = 6, KIND_AMP = 7,
       KIND_HARM = 8, KIND_FM4 = 9 };   /* = UI_KINDS in build_slot.py */
extern const int16_t grain_data[] __attribute__((weak));        /* granular engine: overview[GRAIN_OVERVIEW], then the bank */
/* Our FX (fx.json; build_slot.py generates fx_kind/fx_count). Weak: an image without one of them still links. */
extern const uint8_t fx_kind[];           /* screen kind per FX id 8.. */
extern const int32_t fx_count;
enum { FX_ENSEMBLE = 1, FX_PINGPONG = 2, FX_DISTORTION = 3, FX_CRUSH = 4 };   /* = FX_UI_KINDS in build_slot.py */
extern int32_t distortion_curve(const int16_t *p, int32_t x) __attribute__((weak));
extern const char *distortion_mode_name(const int16_t *p) __attribute__((weak));
extern struct ens ensemble_state[2] __attribute__((weak));
extern struct pp pingpong_state[2] __attribute__((weak));
extern struct crush crush_state[2] __attribute__((weak));   /* crush.c: gain reduction and smoothed knobs */
/* The knobs each FX used last (fxpub.h): after TE's LFO, which the stored knobs never show */
extern struct fx_pub ensemble_pub[2] __attribute__((weak)), pingpong_pub[2] __attribute__((weak)),
    distortion_pub[2] __attribute__((weak)), crush_pub[2] __attribute__((weak));
extern struct dp_diag drumprobe_diag __attribute__((weak));     /* drumprobe.c, only in builds with the drum probe */
/* Our drum engines (build_slot.py): ids drum_first .. + drum_count, screen kind per id (DRUM_KINDS) */
extern const int32_t drum_first, drum_count;
extern const uint8_t drum_kind[];
enum { DRUM_PROBE = 1, DRUM_FM = 2 };                          /* = DRUM_KINDS in build_slot.py */
extern uint32_t dp_type_hits[2] __attribute__((weak));           /* drumprobe.c: TE's loader / saver took our type */
extern struct sp_ram sprobe_ram __attribute__((weak));          /* sprobe engine's RAM (engines.json "ram") */
extern struct kh_diag knobhook_diag __attribute__((weak));      /* knobhook.c, only in builds with a knob hook */
extern struct amp_ram amp_ram __attribute__((weak));            /* amp engine's RAM (engines.json "ram") */
/* TE's retained SVG renderer (notes/svg-route.md; call conventions checked at TE's call sites). */
extern int te_yaffs_open(const char *path, int flags, int mode);     /* FUN_01a997f4: fd, -1 if missing */
extern int te_yaffs_close(int fd);                                    /* FUN_01a99908 */
extern void *te_svg_load(const char *name);   /* FUN_019fb458: /yaffs2/content/display/<name>; asserts on errors */
extern void *te_node_find(void *node, const char *id, int must);      /* FUN_019eb8ac: must = 0 -> 0 if missing */
extern void te_node_matrix(void *node, const int32_t *m);             /* FUN_019ead58: local matrix, Q10 */
extern void te_svg_render(void *screen);      /* FUN_01a3b4c8: renders [screen] (the doc) onto the raster */
extern uint8_t *te_svg_stroke(void *screen, const char *id);  /* FUN_01a3b854: the node's line style, rgba first */
extern uint8_t *te_svg_fill(void *screen, const char *id);    /* FUN_01a3b96c: the node's fill rgba */

#ifndef PALETTE                           /* overridable for the host preview (src/slot/test/preview.py) */
#define PALETTE ((const uint8_t *)0x01BAAF50)
#define KNOBS(slot) ((const int16_t *)(0xFF8013C4 + (slot) * 16))
#define PROF ((volatile uint32_t *)0x00800000)     /* tramp.S: max, sum, calls, live knobs[4] (int16), count */
#define TE_MATRIX ((const int32_t *)0x01DE5E68)   /* raster transform, Q22.10 column-major */
/* FX knobs by edit mode (FUN_01a28da0): synth 0xFF801444 + slot*16, drum 0xFF8015C4 + slot*16, master 0xFF8016C4 */
#define FX_KNOBS(mode, slot) ((const int16_t *)((mode) == 3 ? 0xFF8016C4 : \
                                                 ((mode) == 1 ? 0xFF8015C4 : 0xFF801444) + (slot) * 16))
#endif
/* TE palette (colors.svg): structure in dark shades, live elements in the knob colours (notes/screen.md). */
enum { RED = 2, OFFRED = 3, OFFGREY = 4, DARKGREEN = 5, PURPLEGREY = 8, LIGHTBLUE = 9, DARKBLUE = 10, GREY = 12, ORANGE = 13,
       WHITE = 14, BLUE = 15, GREEN = 16, LIGHTGREEN = 18 };

static void line_fp(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, fp32 w, int colour)
{
    fp32 a[2] = { x0, y0 }, b[2] = { x1, y1 };
    te_line(r, a, b, w, PALETTE + 4 * colour);
}

/* Vertical SVG line written straight into the frame, 1 px, no anti-aliasing. TE's raster maps SVG space with
 * the matrix at 0x01DE5E68 (column-major Q22.10: px = m0 x + m3 y + m6, row = m1 x + m4 y + m7). On the
 * rotated panels m0 = m4 = 0, so a vertical SVG line is one run of pixels in a panel row: a few cycles per
 * pixel instead of an AGG line (~20k cycles each on device; LOG.md 2026-10-08). Other transforms fall back
 * to te_line. Raster object: [0] back buffer (uint16 RGB565), [1] px per row, [2] row stride bytes, [3] rows.
 * Palette entries are RGBA (te_line swaps to BGR for its blender, which packs red from byte 0). */
static void line_fp(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, fp32 w, int colour);

/* The raster's buffer must be one of TE's two framebuffers (0x02000000, 0x03000000; up to 0x29C00 B each,
 * notes/screen.md) and the geometry sane, or the fast paths fall back to te_line: a wrong pointer here would
 * write into random memory. */
static int raster_ok(const int32_t *ras)
{
    uint32_t b = (uint32_t)ras[0];
    int32_t width = ras[1], stride = ras[2], rows = ras[3];
    if (width <= 0 || width > 400 || rows <= 0 || rows > 480 || stride < width * 2 || stride > 1024)
        return 0;
    uint32_t end = b + (uint32_t)(rows * stride);
    return (b >= 0x02000000u && end <= 0x02040000u) || (b >= 0x03000000u && end <= 0x03040000u);
}

static void vline_fast(void *r, fp32 x, fp32 y0, fp32 y1, int colour)
{
    const int32_t *m = TE_MATRIX;
    const int32_t *ras = (const int32_t *)r;
    int32_t width = ras[1], stride = ras[2], rows = ras[3];
    if (m[0] != 0 || m[4] != 0 || !raster_ok(ras)) {
        line_fp(r, x, y0, x, y1, FP(1) >> 1, colour);
        return;
    }
    int32_t row = (((m[1] * (x >> 2)) >> 8) + m[7]) >> 10;
    int32_t p0 = (((m[3] * (y0 >> 2)) >> 8) + m[6]) >> 10, p1 = (((m[3] * (y1 >> 2)) >> 8) + m[6]) >> 10;
    if (p0 > p1) {
        int32_t t = p0;
        p0 = p1;
        p1 = t;
    }
    if (row < 0 || row >= rows || p1 < 0 || p0 >= width)
        return;
    if (p0 < 0)
        p0 = 0;
    if (p1 >= width)
        p1 = width - 1;
    const uint8_t *c = PALETTE + 4 * colour;
    uint16_t px = (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
    uint16_t *dst = (uint16_t *)((uint8_t *)(uintptr_t)ras[0] + row * stride) + p0;
    for (int32_t i = p0; i <= p1; i++)
        *dst++ = px;
}

/* Any thin line written straight into the frame: endpoints through TE's matrix, then a 1-px Bresenham line in
 * panel coordinates. A few hundred cycles instead of ~20k for an AGG line; used for dim/background detail
 * (waterfall frames, dial rings, seams, hidden edges). Same raster checks and fallback as vline_fast. */
static void line_fast(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, int colour)
{
    const int32_t *m = TE_MATRIX;
    const int32_t *ras = (const int32_t *)r;
    int32_t width = ras[1], stride = ras[2], rows = ras[3];
    if (!raster_ok(ras)) {
        line_fp(r, x0, y0, x1, y1, FP(1) >> 1, colour);
        return;
    }
    int32_t ax = x0 >> 2, ay = y0 >> 2, bx = x1 >> 2, by = y1 >> 2;              /* Q8, products fit */
    int32_t c0 = (((m[0] * ax + m[3] * ay) >> 8) + m[6]) >> 10, r0 = (((m[1] * ax + m[4] * ay) >> 8) + m[7]) >> 10;
    int32_t c1 = (((m[0] * bx + m[3] * by) >> 8) + m[6]) >> 10, r1 = (((m[1] * bx + m[4] * by) >> 8) + m[7]) >> 10;
    const uint8_t *c = PALETTE + 4 * colour;
    uint16_t px = (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
    uint8_t *buf = (uint8_t *)(uintptr_t)ras[0];
    int32_t dc = c1 > c0 ? c1 - c0 : c0 - c1, dr = r1 > r0 ? r1 - r0 : r0 - r1;
    int32_t sc = c0 < c1 ? 1 : -1, sr = r0 < r1 ? 1 : -1, err = dc - dr;
    for (int n = 0; n < 1024; n++) {
        if ((uint32_t)c0 < (uint32_t)width && (uint32_t)r0 < (uint32_t)rows)
            ((uint16_t *)(buf + r0 * stride))[c0] = px;
        if (c0 == c1 && r0 == r1)
            break;
        int32_t e2 = 2 * err;
        if (e2 > -dr) {
            err -= dr;
            c0 += sc;
        }
        if (e2 < dc) {
            err += dc;
            r0 += sr;
        }
    }
}

/* Draw s with its top-left at (x, y) (Q22.10); `step` = grid step (glyph 4 x 6 steps, advance 6).
 * text(): anti-aliased (titles, HOLD). text_fast(): 1-px strokes for small labels, crisp and ~free. */
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

/* sin and cos of `turn` (uint32, 2^32 = one turn), Q15. */
static int32_t sin_t(uint32_t turn)
{
    uint32_t i = turn >> 24;
    int32_t f = (int32_t)((turn >> 9) & 0x7FFF);
    return sine256[i] + (((sine256[i + 1] - sine256[i]) * f) >> 15);
}
#define cos_t(t) sin_t((t) + 0x40000000u)

/* ---- shared chrome: title and HOLD badge. No knob dials: every knob drives part of the picture instead,
 * in its knob colour (blue, green, white, orange), as on TE's own screens. ---- */

/* Title, top left, in the colour of the knob that selects what it names (TE: ftwo, eq). */
static void title(void *r, const char *s, int colour)
{
    text(r, s, FP(8), FP(7), FP(2), colour);
}

/* sampler.svg's boxed HOLD, between the title and the dials. */
static void hold_badge(void *r)
{
    line_fp(r, FP(130), FP(5), FP(170), FP(5), FP(1), RED);
    line_fp(r, FP(170), FP(5), FP(170), FP(20), FP(1), RED);
    line_fp(r, FP(170), FP(20), FP(130), FP(20), FP(1), RED);
    line_fp(r, FP(130), FP(20), FP(130), FP(5), FP(1), RED);
    text(r, "HOLD", FP(133) + (FP(1) >> 1), FP(8), FP(3) >> 1, RED);
}

/* CHORD: a boxed badge (LEARN while SHIFT-learning) under HOLD's, and the shape on a mini keyboard: one tick per
 * semitone (black keys short and dark), the chord's notes as dots under the ticks, the root white. */
static void chord_badge(void *r)
{
    int learning = chord_learning;
    int colour = learning ? ORANGE : RED;
    line_fp(r, FP(130), FP(24), FP(180), FP(24), FP(1), colour);
    line_fp(r, FP(180), FP(24), FP(180), FP(39), FP(1), colour);
    line_fp(r, FP(180), FP(39), FP(130), FP(39), FP(1), colour);
    line_fp(r, FP(130), FP(39), FP(130), FP(24), FP(1), colour);
    text(r, learning ? "LEARN" : "CHORD", FP(133) + (FP(1) >> 1), FP(27), FP(3) >> 1, colour);
    int n = chord_n, lo = 0, hi = 0;
    if (n > 4)
        n = 4;
    for (int i = 0; i < n; i++) {
        lo = chord_iv[i] < lo ? chord_iv[i] : lo;
        hi = chord_iv[i] > hi ? chord_iv[i] : hi;
    }
    if (hi - lo < 12)                       /* show at least an octave */
        hi = lo + 12;
    int span = hi - lo;
    fp32 step = span <= 24 ? FP(3) : (fp32)udiv(FP(72), span);
    fp32 x0 = FP(186);
    int pc = lo;                            /* pitch class of the lowest tick (no % or / here: no libgcc) */
    while (pc < 0)
        pc += 12;
    while (pc >= 12)
        pc -= 12;
    for (int s = 0; s <= span; s++, pc = pc == 11 ? 0 : pc + 1) {
        int black = (0x54A >> pc) & 1;      /* C# D# F# G# A# */
        vline_fast(r, x0 + s * step, FP(24), black ? FP(31) : FP(35), black ? DARKBLUE : GREY);
    }
    for (int i = 0; i < n; i++) {
        fp32 x = x0 + (chord_iv[i] - lo) * step;
        line_fp(r, x, FP(37), x, FP(39) + (FP(1) >> 1), FP(2), i ? colour : WHITE);
    }
}

/* ---- wavetable: waterfall of the bank's frames, current frame bright ---- */


#define WT_POINTS 65                        /* 64 segments at shift 0 */
#define WT_LEVEL 3                          /* 256-sample level: 32 harmonics */
#define WT_WIDTH 200
#define WT_AMP 18
#define WT_X0 24                            /* front frame left edge, centre line */
#define WT_Y0 122
#define WT_DX (FP(13) >> 1)                 /* 6.5 per frame towards the back */
#define WT_DY FP(5)

/* Sample of frame f (or the blend of f and f + 1 by ff, Q15) at phase ph, Q15 range. */
static int32_t wt_sample(const int16_t *base, int f, int32_t ff, uint32_t ph)
{
    const int16_t *a = base + f * FRAME_STRIDE + level_off[WT_LEVEL];
    uint32_t i = ph >> (32 - 8);
    int32_t s = a[i];
    if (ff) {
        const int16_t *b = a + FRAME_STRIDE;
        s += ((b[i] - s) * ff) >> 15;
    }
    return s;
}

/* One frame as a polyline of (WT_POINTS - 1) >> shift segments at depth `depth` (Q15 frames from the
 * front): 1-px fast lines when width is 0, else anti-aliased. Returns the right end (x, y). */
static void wt_frame(void *r, const int16_t *bank, int f, int32_t ff, const struct warp *wp, int32_t depth, int shift,
                     fp32 width, int colour, fp32 *end_x, fp32 *end_y)
{
    fp32 ox = FP(WT_X0) + ((WT_DX >> 5) * depth >> 10), oy = FP(WT_Y0) - ((WT_DY >> 5) * depth >> 10);
    const uint32_t step = (0xFFFFFFFFu / (WT_POINTS - 1)) << shift;
    const fp32 dx = (FP(WT_WIDTH) / (WT_POINTS - 1)) << shift;
    const int points = ((WT_POINTS - 1) >> shift) + 1;
    fp32 px = 0, py = 0;
    for (int i = 0; i < points; i++) {
        uint32_t ph = i == points - 1 ? 0xFFFFFFFFu : step * (uint32_t)i;
        int32_t s = wt_sample(bank, f, ff, wp->depth ? warp_phase(wp, ph) : ph);
        fp32 qx = ox + (i == points - 1 ? FP(WT_WIDTH) : dx * i);
        fp32 qy = oy - ((s * FP(WT_AMP)) >> 15);
        if (i) {
            if (!width)
                line_fast(r, px, py, qx, qy, colour);
            else
                line_fp(r, px, py, qx, qy, width, colour);
        }
        px = qx;
        py = qy;
    }
    if (end_x) {
        *end_x = ox + FP(WT_WIDTH);
        *end_y = oy;
    }
}

/* SAMPLE bank (wt_user.h): built here, on the UI thread, from the sampler slot's trimmed window, one wti_step
 * (<= ~3 M cycles) per screen frame, about 13 frames per bank. Rebuilt once the source has been unchanged for 10
 * frames and differs from the last build; a change during a build abandons it. The engine keeps playing the
 * published bank meanwhile, and keeps it if the sampler goes away. */
static struct wti_job wtu_job RAM = { 0, 0, 0, 0, 0, 0, 0, 0 };
static uint32_t wtu_frame RAM = 0;

static void wt_user_tick(void)
{
    struct wt_user *u = &wt_ram;
    struct wti_work *work = (struct wti_work *)(u + 1);
    struct grain_src src = grain_source(0, 0);
    uint32_t sig = 0;
    if (src.slot >= 0) {
        /* signature: slot, window, the sampler's peak table over the window, 64 samples */
        sig = 0x9E3779B9u ^ ((uint32_t)src.slot << 28) ^ (src.start << 3) ^ (src.len * 2654435761u);
        uint32_t a = src.start >> 8, b = (src.start + src.len) >> 8;
        int32_t sum = 0;
        for (uint32_t i = a; i <= b && i < 1034; i++)
            sum += src.peaks[i];
        sig = sig * 31u + (uint32_t)sum;
        uint32_t step = src.len >> 6;
        for (uint32_t i = 0; i < 64; i++)
            sig = sig * 31u + (uint16_t)src.audio[src.start + step * i];
        if (!sig)
            sig = 1;
    }
    if (sig != u->sig) {                        /* changed: wait for it to settle (abandons a build) */
        u->sig = sig;
        u->stable = 0;
        u->building = 0;
        return;
    }
    if (u->building) {
        int r = wti_step(&wtu_job, work);
        if (r == 1)
            return;
        if (r == 0) {
            u->pitched = wtu_job.pitched;
            u->period = wtu_job.p_q16;
            u->slot = src.slot;
            __asm__ volatile("" ::: "memory");  /* every frame written before the engine can see it */
            u->front = u->building;
        }
        u->built_sig = sig;
        u->building = 0;
        return;
    }
    if (!sig || sig == u->built_sig || ++u->stable < 10)
        return;
    int target = u->front == 1 ? 2 : 1;         /* never the bank the engine plays */
    wti_begin(&wtu_job, src.audio + src.start, src.len, src.start, u->bank[target - 1]);
    u->building = target;
}

/* Bank name, shown for ~1.5 s when green changes the bank (and on first show), then hidden */
static const char *const bank_name[BANKS + 1] = { "ANALOG", "PWM", "VOWEL", "ORGAN", "SYNC", "FOLD", "BELL", "DIGITAL",
                                                  "SAMPLE" };
static int32_t wtu_named RAM = -1;
static uint32_t wtu_name_until RAM = 0;

static void draw_wt(void *r, const int16_t *k)
{
    int32_t pos = k[0] < 0 ? 0 : k[0], uni = k[3] < 0 ? 0 : k[3];
    int bank_i = wt_bank_of(k[1]);
    const int16_t *bank = wt_bank_base(wt_data, bank_i);
    struct warp wp = warp_setup(k[2] < 0 ? 0 : k[2]);

    wt_user_tick();
    wtu_frame++;
    /* green: 9 bank pips (the current one lit); the 9th is SAMPLE: blinks while building, off-red while there is
     * no sample to play (SAMPLE then plays bank 0) */
    for (int i = 0; i <= BANKS; i++) {
        fp32 x = FP(174) + FP(11) * i;
        int colour = i == bank_i ? GREEN : GREY;
        if (i == BANKS && wt_ram.building && (wtu_frame & 8))
            colour = i == bank_i ? DARKGREEN : DARKBLUE;
        else if (i == BANKS && i == bank_i && !wt_ram.front)
            colour = OFFRED;
        if (i == bank_i)
            line_fp(r, x, FP(10), x + FP(7), FP(10), FP(2), colour);
        else
            line_fast(r, x, FP(10), x + FP(7), FP(10), colour);
    }
    if (bank_i != wtu_named) {
        wtu_named = bank_i;
        wtu_name_until = wtu_frame + 45;
    }
    if ((int32_t)(wtu_name_until - wtu_frame) > 0)
        title(r, bank_i == BANKS && !wt_ram.front ? "NO SAMPLE" : bank_name[bank_i], GREEN);
    /* white: warp glyph, one sine cycle through the engine's own phase bend (warp.h), x 276..312 */
    {
        fp32 gx = FP(276), gy = FP(28), px = 0, py = 0;
        for (int i = 0; i <= 18; i++) {
            uint32_t ph = i == 18 ? 0xFFFFFFFFu : (0xFFFFFFFFu / 18) * (uint32_t)i;
            int32_t sv = sin_t(wp.depth ? warp_phase(&wp, ph) : ph);
            fp32 qx = gx + FP(2) * i, qy = gy - ((FP(7) >> 5) * sv >> 10);
            if (i)
                line_fp(r, px, py, qx, qy, FP(3) >> 1, WHITE);
            px = qx;
            py = qy;
        }
    }

    /* fm/st.svg depth hierarchy: back frames dark blue, the two around the position grey, current bright. */
    int32_t fpos = pos * (FRAMES - 1);
    int fr = fpos >> 15;
    int32_t ff = fpos & 0x7FFF;
    if (fr >= FRAMES - 1) {
        fr = FRAMES - 2;
        ff = 0x7FFF;
    }
    for (int f = FRAMES - 1; f >= 0; f--)
        wt_frame(r, bank, f, 0, &wp, f << 15, 2, 0, f == fr || f == fr + 1 ? GREY : DARKBLUE, 0, 0);
    /* orange: unison as a detuned ghost of the current frame, fading in (off-red, then orange) and spreading
     * sideways with the detune (as the engine: gain over the first quarter, detune growing with the rest) */
    if (uni > 0) {
        int32_t spread = FP(2) + ((FP(10) >> 5) * ((uni * uni) >> 15) >> 10);
        /* depth offset in Q15 frames: spread / 6.5 units per frame = spread (Q10) * 32768 / 6656 ~ * 315 >> 6 */
        wt_frame(r, bank, fr, ff, &wp, fpos + ((spread * 315) >> 6), 1, FP(1), uni < 8192 ? OFFRED : ORANGE, 0, 0);
    }
    fp32 ex, ey;
    wt_frame(r, bank, fr, ff, &wp, fpos, 1, FP(2), BLUE, &ex, &ey);
    line_fp(r, ex + FP(2), ey, ex + FP(9), ey, FP(2), BLUE);  /* position handle (st.svg) */
}

/* ---- va: oscillator shape with its detune ghost; sub and noise ---- */

#define VA_THIRD 10923                                        /* as va.c */

/* va.c's oscillator without the PolyBLEP, Q12 (1.0 = 4096): sine → saw → square → pulse */
static int32_t va_osc(uint32_t ph, int32_t wave)
{
    int32_t sw = (int32_t)(ph >> 19) - 4096;
    if (wave < VA_THIRD) {
        int32_t sn = sin_t(ph ^ 0x80000000u) >> 3, a = wave * 3;
        return sn + ((a * (sw - sn)) >> 15);
    }
    if (wave < 2 * VA_THIRD) {
        int32_t a = (wave - VA_THIRD) * 3;
        return sw - ((a * ((int32_t)((ph + 0x80000000u) >> 19) - 4096)) >> 15);
    }
    uint32_t pw = 0x80000000u - (uint32_t)(wave - 2 * VA_THIRD) * 157309u;
    return sw - ((int32_t)((ph + pw) >> 19) - 4096);
}

/* Two periods from note-on phase (0x80000000) + `phase`, x0..x0+w, one segment per unit */
static void va_shape(void *r, int32_t wave, uint32_t phase, fp32 x0, fp32 w, fp32 cy, fp32 amp, fp32 width,
                     int colour)
{
    const int n = 136;
    fp32 step = (w * 241) >> 15, px = 0, py = 0;              /* w / 136 without a runtime division */
    for (int i = 0; i <= n; i++) {
        uint32_t ph = 0x80000000u + phase + (uint32_t)i * (0xFFFFFFFFu / (n / 2));
        fp32 qx = x0 + step * i, qy = cy - (((amp >> 4) * va_osc(ph, wave)) >> 8);
        if (i) {
            if (width)
                line_fp(r, px, py, qx, qy, width, colour);
            else
                line_fast(r, px, py, qx, qy, colour);
        }
        px = qx;
        py = qy;
    }
}

static uint32_t va_frame RAM = 0;     /* screen frames, for the noise */

static void draw_va(void *r, const int16_t *k)
{
    int32_t wave = k[0] < 0 ? 0 : k[0], det = k[1] < 0 ? 0 : k[1];
    int32_t noise = k[2] < 0 ? 0 : k[2], sub = k[3] < 0 ? 0 : k[3];

    /* blue: wave shape, green: detune ghost, white: noise, orange: sub */

    /* left panel: oscillator (pd.svg axes), osc 2 as a green ghost offset by the detune */
    const fp32 ox = FP(10), ow = FP(136), ocy = FP(90), oamp = FP(18);
    line_fast(r, ox, ocy, ox + ow, ocy, DARKBLUE);
    line_fast(r, ox, FP(62), ox, FP(118), DARKBLUE);
    if (det >= 330)
        va_shape(r, wave, (uint32_t)(2621 + ((det * 13107) >> 15)) << 17, ox, ow, ocy, oamp, 0, GREEN);
    va_shape(r, wave, 0, ox, ow, ocy, oamp, FP(2), BLUE);

    /* right panel: the sub's square (orange) over four of the oscillator's periods (faint), so it reads as an octave
     * down; noise below. Height = level (as va.c: sub linear, noise squared). */
    const fp32 rx = FP(166), rw = FP(146), scy = FP(70), ncy = FP(112);
    line_fast(r, rx, FP(48), rx, FP(132), DARKBLUE);
    va_shape(r, wave, 0, rx, rw / 2, scy, FP(8), 0, DARKBLUE);
    va_shape(r, wave, 0, rx + rw / 2, rw / 2, scy, FP(8), 0, DARKBLUE);
    fp32 sa = ((FP(14) >> 5) * sub) >> 10, sy = scy + sa;         /* starts low, as va.c's sub at note-on */
    for (int i = 1; i <= 4; i++) {
        fp32 xa = rx + rw * (i - 1) / 4, xb = rx + rw * i / 4, ny = i & 1 ? scy - sa : scy + sa;
        line_fp(r, xa, sy, xb, sy, FP(2), ORANGE);
        if (i < 4)
            line_fp(r, xb, sy, xb, ny, FP(2), ORANGE);
        sy = ny;
    }

    va_frame++;
    fp32 na = ((FP(14) >> 5) * ((noise * noise) >> 15)) >> 10;
    fp32 nstep = (rw * 683) >> 16, px = 0, py = 0;                /* rw / 96 */
    for (int i = 0; i <= 96; i++) {
        int32_t h = (int32_t)(((va_frame >> 1) * 97 + i + 1) * 2654435761u) >> 16;   /* -32768..32767 */
        fp32 qx = rx + nstep * i, qy = ncy - (((na >> 5) * h) >> 10);
        if (i)
            line_fast(r, px, py, qx, qy, WHITE);
        px = qx;
        py = qy;
    }
}

/* ---- grain: source overview (dim) with the active region bright, playhead, spray, live grains ---- */

/* 2^(y / 65536) * 65536 (as grain.c's exp2_q16), y < 15 * 65536 */
static uint32_t exp2_q16_ui(int32_t y)
{
    static const uint16_t tab[33] = { 32768, 33486, 34219, 34968, 35734, 36516, 37316, 38133, 38968, 39821, 40693,
        41584, 42495, 43425, 44376, 45348, 46341, 47356, 48393, 49452, 50535, 51642, 52773, 53928, 55109, 56316,
        57549, 58809, 60097, 61413, 62757, 64132, 65535 };
    int n = y >> 16;
    int32_t f = y & 0xFFFF, i = f >> 11, fr = f & 0x7FF;
    uint32_t m = tab[i] + (((tab[i + 1] - tab[i]) * fr) >> 11);
    return (m << n) << 1;
}


static uint32_t grain_frame RAM = 0;     /* screen frames, for the idle spray pop-outs */

static void draw_grain(void *r, const int16_t *k)
{
    int32_t pos = k[0] < 0 ? 0 : k[0], size = k[1] < 0 ? 0 : k[1], dens = k[2] < 0 ? 0 : k[2];
    int32_t spray = k[3] < 0 ? 0 : k[3];
    struct grain_src src = grain_source(grain_data + GRAIN_OVERVIEW, GRAIN_TOTAL);
    int bank_src = (pos * GRAIN_SOURCES) >> 15;
    if (bank_src >= GRAIN_SOURCES)
        bank_src = GRAIN_SOURCES - 1;
    /* white: density as a row of ticks, 1 .. 16 (2 .. 80 grains per second), top right */
    for (int i = 0, n = 1 + ((dens * 15) >> 15); i < n; i++)
        line_fast(r, FP(304) - FP(8) * i, FP(8), FP(304) - FP(8) * i, FP(18), WHITE);

    /* overview: 128 bars across x 8..312, centre y 90, +-36. Bank: its own peaks. Sampler: TE's peak table
     * (one per 256 samples) over the trimmed window. Colour: inside the spray window blue, inside the current
     * bank sound (or the whole sampler window) grey, else dark blue (sampler.svg / drum2.svg). */
    const fp32 x0 = FP(8), width = FP(304), cy = FP(90);
    fp32 px = x0 + (((width >> 10) * pos) >> 5);
    fp32 sw = ((width >> 10) * ((spray * 1029) >> 15)) >> 5;       /* +-0.3 s of the 9.6 s bank */
    if (src.slot >= 0)
        sw = (fp32)(udiv(((uint32_t)sw >> 4) * (GRAIN_TOTAL >> 6), src.len >> 6) << 4);   /* same 0.3 s */
    fp32 seg_lo = x0 + (width / GRAIN_SOURCES) * bank_src, seg_hi = seg_lo + width / GRAIN_SOURCES;
    /* green: grain size as a shaded band around the playhead, as wide as one grain on this scale (10 .. 500 ms;
     * at least 6 units so short grains stay visible); the bars inside it are drawn green */
    uint32_t glen = (441u * exp2_q16_ui((size * 11559) >> 10)) >> 16;         /* samples, as grain.c */
    fp32 half = (fp32)(udiv((glen >> 2) * (uint32_t)(width >> 10), src.len >> 2) << 9);
    if (half < FP(3))
        half = FP(3);
    for (fp32 x = px - half; x <= px + half; x += FP(1) >> 1)
        vline_fast(r, x, cy - FP(40), cy + FP(40), DARKGREEN);
    for (int i = 0; i < 128; i++) {
        int32_t h;
        if (src.slot < 0) {
            int32_t a = grain_data[2 * i], b = grain_data[2 * i + 1];
            h = a > b ? a : b;
        } else {
            uint32_t s0 = src.start + (uint32_t)(((src.len >> 7) * (uint32_t)i)), s1 = s0 + (src.len >> 7);
            h = 0;
            for (uint32_t pi = s0 >> 8; pi <= (s1 >> 8) && pi < 1034; pi++)
                if (src.peaks[pi] > h)
                    h = src.peaks[pi];
        }
        fp32 x = x0 + ((width >> 7) * i), hh = (h * FP(36)) >> 15;
        if (hh < FP(1) >> 1)
            hh = FP(1) >> 1;
        int colour = DARKBLUE;
        if (x >= px - half && x <= px + half)
            colour = GREEN;
        else if (src.slot >= 0 || (x >= seg_lo && x < seg_hi))
            colour = GREY;
        vline_fast(r, x, cy - hh, cy + hh, colour);
    }
    if (src.slot < 0)
        for (int i = 0; i <= GRAIN_SOURCES; i++)                    /* ruler ticks at the sound seams */
            vline_fast(r, x0 + (width / GRAIN_SOURCES) * i, FP(131), FP(135), GREY);

    /* playhead with a downward cap */
    line_fp(r, px, FP(48), px, FP(132), FP(3) >> 1, BLUE);
    line_fp(r, px - FP(3), FP(45), px + FP(3), FP(45), FP(1), BLUE);
    line_fp(r, px - FP(3), FP(45), px, FP(49), FP(1), BLUE);
    line_fp(r, px + FP(3), FP(45), px, FP(49), FP(1), BLUE);

    /* live grains: 2-px bright runs lighting up the audio they read (rendering voices only). White while a grain
     * reads at the playhead (it drifts forward by up to one grain); orange when the spray popped it out elsewhere */
    int live = 0;
    for (int vi = 0; vi < 6; vi++) {
        struct grain_voice *gv = (struct grain_voice *)(voice_ram + vi * VOICE_RAM_BYTES);
        if (gv->owner != OWNER_GRAIN || gv->peak <= 0)
            continue;
        gv->peak = 0;
        for (int gi = 0; gi < GRAINS; gi++) {
            const struct grain *g = &gv->g[gi];
            if (!g->wstep)
                continue;
            uint32_t sample = g->pos >> 12;
            if (sample < src.start || sample >= src.start + src.len)
                continue;
            /* x of the sample inside the window: (sample - start) / len * width */
            uint32_t rel = sample - src.start;
            fp32 gx = x0 + (fp32)(udiv((rel >> 4) * (uint32_t)(width >> 10), src.len >> 4) << 10);
            int32_t amp = sin_t(g->wph >> 1);                          /* window, Q15 */
            fp32 hh = ((FP(30) >> 5) * amp) >> 10;
            if (hh < FP(3))
                hh = FP(3);
            int colour = g->mul > 20000 ? LIGHTBLUE : WHITE;         /* shimmer grains light blue */
            if (gx < px - half - FP(2) || gx > px + 3 * half + FP(2))
                colour = ORANGE;                                       /* sprayed */
            live = 1;
            vline_fast(r, gx, cy - hh, cy + hh, colour);
            vline_fast(r, gx + (FP(1) * 7 >> 3), cy - hh, cy + hh, colour);
        }
    }
    /* orange: no notes sounding, so show where the spray would throw grains: three pop-outs inside +-spray,
     * moving to new random spots about four times a second */
    grain_frame++;
    if (!live && sw > FP(1)) {
        for (uint32_t j = 0; j < 3; j++) {
            uint32_t h = ((grain_frame >> 3) * 3 + j + 1) * 2654435761u;
            fp32 gx = px - sw + (fp32)((((uint32_t)(2 * sw) >> 4) * (h >> 16)) >> 12);
            int bi = ((gx - x0) * 431) >> 20;                          /* / (304 / 128 units): no runtime division */
            int32_t hgt = bi < 0 || bi > 127 ? 0 : src.slot < 0 ? (grain_data[2 * bi] > grain_data[2 * bi + 1] ? grain_data[2 * bi] : grain_data[2 * bi + 1]) : 16384;
            fp32 hh = (hgt * FP(36)) >> 15;
            if (hh < FP(4))
                hh = FP(4);
            vline_fast(r, gx, cy - hh, cy + hh, ORANGE);
            vline_fast(r, gx + (FP(1) * 7 >> 3), cy - hh, cy + hh, ORANGE);
        }
    }
}

/* ---- number text (inprobe, diagnostics) ---- */

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_dec(char *p, uint32_t u)
{
    static const uint32_t pow10[] = { 1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1 };
    int started = 0;
    for (int i = 0; i < 10; i++) {
        int d = 0;
        while (u >= pow10[i]) {
            u -= pow10[i];
            d++;
        }
        if (d || started || i == 9) {
            *p++ = (char)('0' + d);
            started = 1;
        }
    }
    *p = 0;
    return p;
}

static char *put_hex(char *p, uint32_t u, int digits)
{
    for (int i = digits - 1; i >= 0; i--) {
        uint32_t d = (u >> (4 * i)) & 15;
        *p++ = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    *p = 0;
    return p;
}

/* ---- inprobe: the input buffer the engine reads (src/engine/inprobe.h; notes/input.md), live ---- */

static const char *const inp_names[INP_SOURCES] = { "SILENCE", "TONE", "TE IN", "RAW", "RAW OTHER", "ALT" };
static uint32_t inp_frames RAM = 0;       /* screen frames */
static uint32_t inp_sum RAM = 0;          /* checksum of the last frame's block */
static uint32_t inp_changed RAM = 0;      /* one bit per frame, newest in bit 0: block changed since the frame before */
static int32_t inp_hold RAM = -96 * 256;  /* held peak, dBFS Q8 */
static uint32_t inp_renders RAM = 0;      /* render count (tramp.S) at the last frame */

/* 20 log10(p / 32768) in Q8 (dBFS) for p >= 1: log2 from the leading bit plus a linear mantissa (< 0.5 dB off) */
static int32_t dbfs_q8(int32_t p)
{
    if (p < 1)
        return -96 * 256;
    int n = 0;
    while (n < 15 && (p >> (n + 1)))
        n++;
    int32_t f = (p << (15 - n)) & 0x7FFF;
    return ((((n - 15) << 15) + f) * 1541) >> 15;                 /* 6.0206 dB per octave, Q8 */
}

static char *put_db(char *p, int32_t q8)
{
    if (q8 < 0) {
        *p++ = '-';
        q8 = -q8;
    }
    p = put_dec(p, (uint32_t)(q8 + 128) >> 8);
    return put_str(p, "DB");
}

static void draw_inprobe(void *r, const int16_t *k)
{
    char buf[48], *p;
    int db = inp_pos(k[0], INP_DB_MAX + 1), src = inp_pos(k[1], INP_SOURCES);
    int ch = inp_pos(k[2], INP_CHANNELS), sh = inp_pos(k[3], INP_SHIFTS);
    int32_t g = inp_gain_q10[db];
    int16_t x[INP_N];
    uint32_t sel = te_rx_sel & 1, raw = inp_fetch(src, ch, sh, x);
    int stereo = inp_words(src) != 0;

    /* green: the source's name and where it is read (int32 L/R or int16 mono; short of the HOLD/CHORD badges) */
    title(r, inp_names[src], GREEN);
    p = buf;
    if (src == INP_TE)
        p = put_str(p, "FF800CB4 I16 X64");
    else if (src == INP_RAW || src == INP_RAW_OTHER)
        p = put_str(put_str(p, (sel ^ (src == INP_RAW_OTHER)) ? "FF800570" : "FF800370"), " LR");
    else if (src == INP_ALT)
        p = put_str(p, "01AB2E2C LR");
    else if (src == INP_TONE)
        p = put_str(p, "ENGINE SINE");
    if (src == INP_RAW || src == INP_RAW_OTHER)
        put_dec(put_str(p, " SEL "), sel);
    text_fast(r, buf, FP(8), FP(24), FP(1), GREY);

    /* live: frame counter, and whether the block changed since the last frame (LIVE / STILL; below the CHORD
     * badge's keyboard), and in how many of the last 32 frames */
    uint32_t sum = raw;
    int32_t peak = 0;
    for (int i = 0; i < INP_N; i++) {
        int32_t a = x[i] < 0 ? -x[i] : x[i];
        peak = a > peak ? a : peak;
        sum = sum * 31 + (uint16_t)x[i];
    }
    inp_frames++;
    inp_changed = (inp_changed << 1) | (sum != inp_sum);
    inp_sum = sum;
    int n = 0;
    for (uint32_t b = inp_changed; b; b >>= 1)
        n += b & 1;
    put_dec(put_str(buf, "F "), inp_frames);
    text_fast(r, buf, FP(236), FP(7), FP(3) >> 1, WHITE);
    int changed = inp_changed & 1;
    text(r, changed ? "LIVE" : "STILL", FP(236), FP(36), FP(3) >> 1, changed ? GREEN : RED);
    put_str(put_dec(buf, n), "/32");
    text_fast(r, buf, FP(286), FP(38), FP(1), changed ? GREEN : RED);

    /* the block after the gain (what the engine plays, before the knee), 64 samples across, full scale +-30 */
    const fp32 x0 = FP(8), cy = FP(80), amp = FP(30), step = FP(304) >> 6;
    line_fast(r, x0, cy, x0 + FP(304), cy, DARKBLUE);
    line_fast(r, x0, cy - amp, x0 + FP(304), cy - amp, DARKBLUE);
    line_fast(r, x0, cy + amp, x0 + FP(304), cy + amp, DARKBLUE);
    fp32 px = 0, py = 0;
    for (int i = 0; i < INP_N; i++) {
        int32_t v = inp_sat16((x[i] * g) >> 10);
        fp32 qx = x0 + step * i, qy = cy - (((amp >> 5) * v) >> 10);
        if (i)
            line_fp(r, px, py, qx, qy, FP(3) >> 1, GREEN);
        px = qx;
        py = qy;
    }

    /* peak meter of the source (before the gain): -84 .. 0 dBFS over x 8..248, held peak in red */
    int32_t d = dbfs_q8(peak);
    inp_hold = d > inp_hold - 64 ? d : inp_hold - 64;             /* falls 0.25 dB per frame */
    const fp32 mx = FP(8), mw = FP(240), my = FP(120);
    line_fast(r, mx, my, mx + mw, my, DARKBLUE);
    fp32 len = ((d + 84 * 256) * 731) >> 6;                       /* 240 units per 84 dB: 240 * 1024 / 21504 */
    if (len > 0)
        line_fp(r, mx, my, mx + (len < mw ? len : mw), my, FP(4), WHITE);
    fp32 hl = ((inp_hold + 84 * 256) * 731) >> 6;
    if (hl > 0 && hl <= mw)
        line_fp(r, mx + hl, my - FP(3), mx + hl, my + FP(3), FP(1), RED);
    put_db(put_str(buf, "PK "), d);
    text_fast(r, buf, FP(256), FP(117), FP(1), WHITE);

    /* numbers: gain (blue), channel (white), shift (orange), the first raw word, render calls since the last frame */
    p = put_str(buf, "+");
    put_str(put_dec(p, db), "DB");
    text_fast(r, buf, FP(8), FP(130), FP(1), BLUE);
    text_fast(r, stereo ? (ch == 0 ? "L+R" : ch == 1 ? "L" : "R") : "-", FP(48), FP(130), FP(1), WHITE);
    put_dec(put_str(buf, "SH "), stereo ? (uint32_t)sh : 0);
    text_fast(r, buf, FP(76), FP(130), FP(1), ORANGE);
    put_hex(put_str(buf, "W0 "), raw, stereo ? 8 : 4);
    text_fast(r, buf, FP(116), FP(130), FP(1), GREY);
    uint32_t renders = PROF[5];
    put_dec(put_str(buf, "R "), renders - inp_renders);
    inp_renders = renders;
    text_fast(r, buf, FP(236), FP(130), FP(1), GREY);
}

/* ---- drum probe (src/slot/drumprobe.c, notes/drum-engine.md): what TE hands a drum engine, to report ---- */

static uint32_t dp_frames RAM = 0;        /* screen frames */
static uint32_t dp_renders RAM = 0;       /* render calls at the last frame */

static char *put_sdec(char *p, int32_t v)
{
    if (v < 0) {
        *p++ = '-';
        v = -v;
    }
    return put_dec(p, (uint32_t)v);
}

/* drum_ui_draw (below) for a drum slot holding the probe (id). */
static void draw_drumprobe(int slot, int id)
{
    static const uint8_t knob_colour[8] = { BLUE, GREEN, WHITE, ORANGE, LIGHTBLUE, LIGHTGREEN, GREY, ORANGE };
    void *r = te_raster();
    struct dp_diag *d = &drumprobe_diag;
    char buf[64], *p;
    if (!d)
        return;
    slot &= 7;
    int edit = te_drum_edit;
    edit = edit < 0 ? 0 : edit >= DP_KEYS ? DP_KEYS - 1 : edit;
    const int16_t *ek = te_drum_params + (slot * DP_KEYS + edit) * 8;
    dp_frames++;

    title(r, "DRUM PROBE", WHITE);
    put_dec(put_str(buf, "F "), dp_frames);
    text_fast(r, buf, FP(236), FP(7), FP(3) >> 1, WHITE);

    /* the last note-on as the engine saw it: key 0..23, velocity (TE's << 8 undone), voice, note-on/off counts */
    p = put_dec(put_str(buf, "KEY "), (uint32_t)(d->key < 0 ? 0 : d->key));
    p = put_dec(put_str(p, " VEL "), (uint32_t)d->vel >> 8);
    p = put_dec(put_str(p, " V "), (uint32_t)(d->voice < 0 ? 0 : d->voice));
    p = put_dec(put_str(p, " ON "), d->noteons);
    put_dec(put_str(p, " OFF "), d->noteoffs);
    text_fast(r, buf, FP(8), FP(24), FP(1), WHITE);
    /* the key the knobs edit, slot, id, slot inits, range row copied; render calls since the last frame, worst
     * render (cycles), TE's pitch ratio for the last note */
    p = put_dec(put_str(buf, "EDIT "), (uint32_t)edit);
    p = put_dec(put_str(p, " SLOT "), (uint32_t)slot);
    p = put_dec(put_str(p, " ID "), (uint32_t)id);
    p = put_dec(put_str(p, " IN "), (uint32_t)d->inits);
    put_dec(put_str(p, " ROW "), (uint32_t)d->row_copied);
    text_fast(r, buf, FP(8), FP(34), FP(1), GREY);
    p = put_dec(put_str(buf, "R "), d->renders - dp_renders);
    put_dec(put_str(p, " CYC "), d->cyc_max);
    text_fast(r, buf, FP(8), FP(44), FP(1), GREY);
    dp_renders = d->renders;
    /* save/load: TE's per-key loader (LD) and saver (SV) took the dbox path for our type, and a checksum of the
     * slot's 24 x 8 values (compare before saving and after reloading) */
    uint32_t sum = 0;
    const int16_t *sk = te_drum_params + slot * DP_KEYS * 8;
    for (int i = 0; i < DP_KEYS * 8; i++)
        sum = sum * 31 + (uint16_t)sk[i];
    const uint32_t *hits = dp_type_hits;
    p = put_dec(put_str(buf, "LD "), hits ? hits[0] : 0);
    p = put_dec(put_str(p, " SV "), hits ? hits[1] : 0);
    put_hex(put_str(p, " SUM "), (sum ^ sum >> 16) & 0xFFFF, 4);
    text_fast(r, buf, FP(124), FP(44), FP(1), WHITE);

    /* per knob: the edit key's value (what the encoder wrote), the newest voice's rendered value (after the LFO),
     * and dbox's range row 9 that TE's encoder clamps with (min max scale) */
    for (int i = 0; i < 8; i++) {
        const int16_t *rg = ranges + 9 * 32 + 4 * i;
        p = put_dec(put_str(buf, "K"), (uint32_t)i);
        p = put_sdec(put_str(p, " "), ek[i]);
        p = put_sdec(put_str(p, " / "), d->live[i]);
        p = put_sdec(put_str(p, "  "), rg[0]);
        p = put_sdec(put_str(p, ".."), rg[1]);
        put_sdec(put_str(p, " S "), rg[2]);
        text_fast(r, buf, FP(8), FP(56 + 10 * i), FP(1), knob_colour[i]);
    }

    /* voices: one bar per voice, its level (top right) */
    for (int v = 0; v < DP_VOICES; v++) {
        fp32 x = FP(256 + 5 * v), h = (d->level[v] * 22) >> 5;      /* 22 units at full scale */
        line_fast(r, x, FP(50), x + FP(3), FP(50), DARKBLUE);
        if (h > FP(1) >> 1)
            line_fp(r, x + (FP(3) >> 1), FP(50), x + (FP(3) >> 1), FP(50) - h, FP(3), GREEN);
    }
}

/* ---- knob probe (src/engine/sprobe.c, src/slot/knobhook.c, notes/knob-hook.md): the 8 stored knobs, the hook ---- */

static uint32_t sp_frames RAM = 0;
static uint32_t sp_renders RAM = 0;

static void draw_sprobe(void *r, int slot)
{
    static const uint8_t knob_colour[8] = { BLUE, GREEN, WHITE, ORANGE, LIGHTBLUE, LIGHTGREEN, GREY, ORANGE };
    const int16_t *k = KNOBS(slot);
    const struct kh_diag *h = &knobhook_diag;
    const struct sp_ram *e = &sprobe_ram;
    char buf[64], *p;
    sp_frames++;
    title(r, "KNOB PROBE", WHITE);
    put_dec(put_str(buf, "F "), sp_frames);
    text_fast(r, buf, FP(236), FP(7), FP(3) >> 1, WHITE);

    /* the hook since boot: calls, last knob and delta, slot; render calls since the last frame (below the badges) */
    uint32_t calls = h ? h->calls : 0;
    p = put_dec(put_str(buf, "HOOK "), calls);
    p = put_sdec(put_str(p, " KNOB "), h ? h->knob : -1);
    p = put_sdec(put_str(p, " D "), h ? h->delta : 0);
    put_sdec(put_str(p, " SLOT "), h ? h->slot : -1);
    text_fast(r, buf, FP(8), FP(46), FP(1), WHITE);
    uint32_t renders = e ? e->renders : 0;
    put_dec(put_str(buf, "R "), renders - sp_renders);
    sp_renders = renders;
    text_fast(r, buf, FP(250), FP(46), FP(1), GREY);
    /* what the hook last wrote into knobs 6, 7; NOW: they still hold it (SAME), or not (DIFF: clamped or rewritten),
     * or nothing written since boot (-) */
    int same = h && calls && k[SP_K6] == h->w6 && k[SP_K7] == h->w7;
    p = put_hex(put_str(buf, "WROTE "), (uint16_t)(h ? h->w6 : 0), 4);
    put_hex(put_str(p, " "), (uint16_t)(h ? h->w7 : 0), 4);
    text_fast(r, buf, FP(8), FP(57), FP(1), GREY);
    text_fast(r, !calls ? "NOW -" : same ? "NOW SAME" : "NOW DIFF", FP(166), FP(57), FP(1),
              !calls ? GREY : same ? GREEN : RED);
    /* from the stored values alone (survives a reboot): the sound's edit count in K6, and K7 = f(K6) */
    int empty = k[SP_K6] == 0 && k[SP_K7] == 0, pair = k[SP_K6] < 0 && k[SP_K7] == sp_k7(k[SP_K6]);
    p = put_dec(put_str(buf, "EDITS "), sp_edits(k[SP_K6]));
    put_dec(put_str(p, " LAST K"), (uint32_t)k[SP_K6] & 7);
    text_fast(r, buf, FP(8), FP(68), FP(1), GREY);
    text_fast(r, pair ? "PAIR OK" : empty ? "PAIR -" : "PAIR BAD", FP(166), FP(68), FP(1),
              pair ? GREEN : empty ? GREY : RED);

    /* per knob (K0..K3 left, SHIFT's K4..K7 right): stored value in hex and signed decimal, then L = the last render
     * call's param (after TE's LFO; white when it differs from the stored value) */
    for (int i = 0; i < 8; i++) {
        fp32 x = FP(i < 4 ? 8 : 166), y = FP(84 + 13 * (i & 3));
        p = put_dec(put_str(buf, "K"), (uint32_t)i);
        p = put_hex(put_str(p, " "), (uint16_t)k[i], 4);
        put_sdec(put_str(p, " "), k[i]);
        text_fast(r, buf, x, y, FP(1), knob_colour[i]);
        p = put_str(buf, "L ");
        if (e)
            put_sdec(p, e->live[i]);
        else
            put_str(p, "-");
        text_fast(r, buf, x + FP(98), y, FP(1), e && e->live[i] != k[i] ? WHITE : GREY);
    }
}

/* ---- amp: a small combo in an oblique view, the tuner (src/slot/ui_amp.h) ---- */
#include "ui_amp.h"

static uint32_t last_renders RAM = 1;     /* render count at the last frame (live knobs) */

#ifdef UI_DEBUG
/* ---- diagnostics (PROF1): engine DSP load and UI cost from CYCLES, and the live-knob record ---- */

static uint32_t pct(uint32_t part, uint32_t whole)
{
    whole >>= 8;
    return whole ? udiv((part >> 8) * 100, whole) : 0;
}

static uint32_t last_frame RAM = 1, ui_cost RAM = 1, live_count RAM = 1;

static void diagnostics(void *r, uint32_t now, const int16_t *k, int live)
{
    char buf[40], *p;
    uint32_t elapsed = now - last_frame;
    uint32_t max = PROF[0], sum = PROF[1], calls = PROF[2];
    PROF[0] = 0;
    PROF[1] = 0;
    PROF[2] = 0;
    last_frame = now;
    live_count += live;

    /* Per render call: average and worst cycles (K). A % of the frame was misleading: render calls from
     * two contexts overlap in time, so their wall-clock sum can exceed the frame (LOG.md 2026-10-08). */
    p = put_str(buf, "R ");
    p = put_dec(p, calls ? udiv(sum, calls) >> 10 : 0);
    p = put_str(p, "K MAX ");
    p = put_dec(p, max >> 10);
    p = put_str(p, "K N ");
    put_dec(p, calls);
    text(r, buf, FP(8), FP(22), FP(1), ORANGE);

    p = put_str(buf, "UI ");
    p = put_dec(p, pct(ui_cost, elapsed));
    p = put_str(p, "% LIVE ");
    put_dec(p, live_count & 1023);                              /* frames with live (rendered) knobs */
    text(r, buf, FP(8), FP(30), FP(1), ORANGE);

    p = put_str(buf, "K ");
    for (int i = 0; i < 4; i++) {
        p = put_dec(p, (uint32_t)(uint16_t)k[i]);
        p = put_str(p, " ");
    }
    p = put_str(p, "F ");
    put_dec(p, elapsed >> 10);
    text(r, buf, FP(8), FP(38), FP(1), ORANGE);
}

#endif

/* ---- our FX (fx.json): drawn instead of the filter FX screen they borrow (slot.S _fx_ui_update) ---- */

static int32_t clamp_i(int32_t x, int32_t lo, int32_t hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static int32_t knob_pos(int16_t k)
{
    return k < 0 ? 0 : k;
}

/* Orange = mix: a vertical bar on the right, dark track, orange fill from the bottom. */
static void mix_bar(void *r, int16_t mix)
{
    vline_fast(r, FP(306), FP(30), FP(130), DARKBLUE);
    fp32 top = FP(130) - (fp32)((knob_pos(mix) * 100) >> 5);   /* 100 units at 32768 (Q22.10: x 1024 / 32768) */
    if (top < FP(129))
        line_fp(r, FP(306), FP(130), FP(306), top, FP(4), ORANGE);
}

/* LFO rate of the ensemble in Hz, Q16: 0.1 * 2^(log2(80) k / 32768), 0.1 .. 8 Hz (ensemble.c rate_inc). */
static uint32_t ens_rate_q16(int32_t k)
{
    k = (k * 25891) >> 15;
    return ((exp2_q16_ui(k << 4) >> 4) * 6554u) >> 12;
}

/* Triangle like ensemble.c's tri(), from a Q16 turn: -32768 at 0, +32766 at a half turn. */
static int32_t tri16(uint32_t u)
{
    int32_t x = u & 0xFFFF;
    return x < 32768 ? 2 * x - 32768 : 2 * (65535 - x) - 32768;
}

#define ENS_X0 16
#define ENS_W 274                           /* the last second, oldest left */
#define ENS_CY 82
#define ENS_AMP 42

/* One tap's delay sweep over the last second: exact triangle (breakpoints only), phase offset `off` (Q16 turns). */
static void ens_sweep(void *r, uint32_t now, uint32_t cyc, uint32_t off, int32_t amp, int mirror, int colour,
                      fp32 width, int fast)
{
    if (!cyc)
        cyc = 1;
    uint32_t u1 = now + off + (40u << 16), u0 = u1 - cyc;          /* unwrapped Q16 turns */
    uint32_t u = u0;
    fp32 xp = 0, yp = 0;
    for (int n = 0; n < 64; n++) {
        int32_t t = tri16(u);
        fp32 x = FP(ENS_X0) + (fp32)udiv((u - u0) * (uint32_t)ENS_W, cyc) * 1024;   /* cyc <= 524288: no overflow */
        fp32 y = FP(ENS_CY) + (fp32)(((mirror ? t : -t) * amp) >> 5);  /* amp units at t = 32768 */
        if (n) {
            if (fast)
                line_fast(r, xp, yp, x, y, colour);
            else
                line_fp(r, xp, yp, x, y, width, colour);
        }
        if (u == u1)
            break;
        xp = x;
        yp = y;
        uint32_t next = (u & ~0x7FFFu) + 0x8000u;                 /* next half-turn breakpoint */
        u = next < u1 ? next : u1;
    }
}

static void draw_ensemble(void *r, const int16_t *k, int stereo)
{
    int32_t ch = knob_pos(k[2]), ens = ens_amount(ch);               /* white: character (ensemble.h) */
    title(r, ens ? "ENSEMBLE" : ch >= ENS_DUAL_HI ? "DUAL" : "CHORUS", WHITE);
    const struct ens *st = ensemble_state ? &ensemble_state[stereo] : 0;
    uint32_t now = st ? st->lfo >> 16 : 0;
    uint32_t cyc = ens_rate_q16(knob_pos(k[0]));                  /* turns per second, Q16 */
    int32_t amp = (knob_pos(k[1]) * ENS_AMP) >> 15;
    line_fast(r, FP(ENS_X0), FP(ENS_CY), FP(ENS_X0 + ENS_W), FP(ENS_CY), DARKBLUE);
    for (fp32 x = FP(ENS_X0); x < FP(ENS_X0 + ENS_W); x += FP(8)) {   /* green: depth guides */
        line_fast(r, x, FP(ENS_CY - amp), x + FP(4), FP(ENS_CY - amp), GREEN);
        line_fast(r, x, FP(ENS_CY + amp), x + FP(4), FP(ENS_CY + amp), GREEN);
    }
    line_fp(r, FP(ENS_X0 + ENS_W + 4), FP(ENS_CY - amp), FP(ENS_X0 + ENS_W + 4), FP(ENS_CY + amp), FP(1), GREEN);
    if (ens >= 2048) {                      /* white: the two extra taps, brighter with more ensemble */
        int c = ens < 16384 ? GREY : WHITE;
        ens_sweep(r, now, cyc, 0x5555, amp, 0, c, FP(1), 1);
        ens_sweep(r, now, cyc, 0xAAAA, amp, 0, c, FP(1), 1);
    }
    if (stereo)                             /* master: R in anti-phase (DUAL), or L's sweep with the wet inverted (CE) */
        ens_sweep(r, now, cyc, 0, amp, 1, DARKBLUE, FP(1), 1);
    ens_sweep(r, now, cyc, 0, amp, 0, BLUE, FP(2), 0);           /* blue: the sweep, at its rate */
    mix_bar(r, k[3]);
}

static const char *const pp_div_names[PP_DIVS] = { "1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.",
    "1/4", "1/2T", "1/4.", "1/2" };

/* The pingpong delay the screen shows, whole samples: the DSP's (glided) delay once it runs, else blue's target at
 * 120 BPM (pingpong.h). */
static uint32_t pp_delay(const int16_t *k, int stereo)
{
    const struct pp *st = pingpong_state ? &pingpong_state[stereo] : 0;
    if (st && st->d > 0)
        return (uint32_t)st->d >> 15;
    int div, fold;
    return (uint32_t)pp_target(knob_pos(k[0]), PP_SPB_DEF, &div, &fold) >> 15;
}

/* The time readout, top left in blue: the division played ("1/8.", "1/8T", "1/4") or the free time ("330MS"); when
 * synced, the tempo in grey top right ("120BPM", "133.3BPM"), as the DSP reads it, and under the division what blue
 * asked for when it was too long for the line ("1/2 TOO LONG": the half or quarter plays). */
static void pp_readout(void *r, const int16_t *k, const struct pp *st)
{
    int div, fold;
    int32_t spb = st && st->spb ? st->spb : PP_SPB_DEF;
    int32_t dt = st && st->dt ? st->dt : pp_target(knob_pos(k[0]), spb, &div, &fold);
    if (st && st->dt) {
        div = st->div;
        fold = st->fold;
    }
    char buf[16], *p;
    if (div >= 0 && div < PP_DIVS) {
        text(r, pp_div_names[div], FP(8), FP(7), FP(2), BLUE);
        if (fold > 0 && div + 3 * fold < PP_DIVS) {                 /* slow tempo: what blue asked for, in grey */
            put_str(put_str(buf, pp_div_names[div + 3 * fold]), " TOO LONG");
            text_fast(r, buf, FP(8), FP(23), FP(1), GREY);
        }
        uint32_t q = (uint32_t)spb >> 10;                           /* samples per beat, Q4 */
        uint32_t t = udiv(423360000u + (q >> 1), q);                /* BPM x 10 = 60 x 44100 x 10 x 16 / q */
        uint32_t whole = udiv(t, 10);
        p = put_dec(buf, whole);
        if (t - whole * 10) {
            *p++ = '.';
            p = put_dec(p, t - whole * 10);
        }
        put_str(p, "BPM");
        int n = (int)(p - buf) + 3;
        text_fast(r, buf, FP(312) - FP(12 * n - 4), FP(7), FP(2), GREY);   /* 1-px lines: cheap, secondary */
    } else {
        put_str(put_dec(buf, udiv(((uint32_t)dt >> 15) * 10 + 220, 441)), "MS");
        text(r, buf, FP(8), FP(7), FP(2), BLUE);
    }
}

#define PP_X0 20
#define PP_W 270                            /* 2.7 s */
#define PP_CY 82
#define PP_H 48

static void draw_pingpong(void *r, const int16_t *k, int stereo)
{
    title(r, stereo ? "PING PONG" : "DELAY", BLUE);
    /* time: the delay played (pingpong.h), in 1/100 units of the 2.7 s window: 1 unit = 10 ms */
    uint32_t t10 = udiv(pp_delay(k, stereo) << 4, 441);              /* Q4 units */
    int32_t g = (knob_pos(k[1]) * 31130) >> 15;                      /* feedback, Q15 (0.95 max) */
    int32_t mix = knob_pos(k[3]);
    line_fast(r, FP(PP_X0), FP(PP_CY), FP(PP_X0 + PP_W), FP(PP_CY), DARKBLUE);
    for (int i = 0; i <= 27; i += 5)                                  /* ruler: 0.5 s */
        vline_fast(r, FP(PP_X0 + i * 10), FP(PP_CY - 2), FP(PP_CY + 2), DARKBLUE);
    int32_t dry = ((32767 - mix) * PP_H) >> 15;
    if (dry > 0)
        line_fp(r, FP(PP_X0), FP(PP_CY), FP(PP_X0), FP(PP_CY - dry), FP(2), GREY);
    int32_t a = 32767;                      /* echo level, Q15 */
    fp32 px = 0, py = 0;
    for (int e = 1; e <= 64 && a > 300; e++) {
        uint32_t xq = (uint32_t)e * t10;    /* Q4 units */
        if (xq >= (uint32_t)PP_W << 4)
            break;
        fp32 x = FP(PP_X0) + (fp32)(xq << 6);
        int32_t h = (int32_t)(((a * mix) >> 15) * PP_H) >> 15;
        int up = !stereo || (e & 1);        /* L echoes up, R down */
        fp32 y = up ? FP(PP_CY - h) : FP(PP_CY + h);
        if (e <= 24)
            line_fp(r, x, FP(PP_CY), x, y == FP(PP_CY) ? y - FP(1) : y, FP(2), BLUE);
        else
            vline_fast(r, x, up ? y : FP(PP_CY), up ? FP(PP_CY) : y, BLUE);
        fp32 ty = FP(PP_CY - h);            /* green: the decay envelope over the echo tops */
        if (e > 1 && e <= 24)
            line_fp(r, px, py, x, ty, FP(1), GREEN);
        px = x;
        py = ty;
        a = (a * g) >> 15;
    }
    /* white: tone glyph, top right: a spectrum tilt, dark (falling) .. flat .. bright (rising) */
    int32_t tilt = ((knob_pos(k[2]) - 16384) * 10) >> 14;             /* -10 .. +10 units */
    line_fp(r, FP(250), FP(18) - FP(tilt), FP(290), FP(18) + FP(tilt), FP(2), WHITE);
    vline_fast(r, FP(270), FP(8), FP(28), DARKBLUE);
    mix_bar(r, k[3]);
}

#define DS_CX 160
#define DS_CY 74
#define DS_HX 64                            /* box half width / height */
#define DS_HY 56

static void draw_distortion(void *r, const int16_t *k)
{
    title(r, distortion_mode_name ? distortion_mode_name(k) : "DISTORT", WHITE);
    line_fast(r, FP(DS_CX - DS_HX), FP(DS_CY), FP(DS_CX + DS_HX), FP(DS_CY), DARKBLUE);
    vline_fast(r, FP(DS_CX), FP(DS_CY - DS_HY), FP(DS_CY + DS_HY), DARKBLUE);
    for (int i = -DS_HX; i < DS_HX; i += 8)                          /* dry: the unity diagonal, dashed */
        line_fast(r, FP(DS_CX + i), FP(DS_CY) - FP(i) * DS_HY / DS_HX, FP(DS_CX + i + 4),
                  FP(DS_CY) - FP(i + 4) * DS_HY / DS_HX, GREY);
    /* blue: drive, how hard the input hits the curve: a bar under the box */
    fp32 dw = (fp32)((knob_pos(k[0]) * 2 * DS_HX) >> 5);             /* 0 .. 2 * DS_HX units */
    line_fast(r, FP(DS_CX - DS_HX), FP(DS_CY + DS_HY + 8), FP(DS_CX + DS_HX), FP(DS_CY + DS_HY + 8), DARKBLUE);
    if (dw > FP(1))
        line_fp(r, FP(DS_CX - DS_HX), FP(DS_CY + DS_HY + 8), FP(DS_CX - DS_HX) + dw, FP(DS_CY + DS_HY + 8), FP(3),
                BLUE);
    /* green: the resonant filter before the shaper, as a small response glyph left of the box (log frequency,
     * 20 Hz .. 20 kHz): flat in the middle (16384 +- 2048), low-pass 6 kHz -> 150 Hz with rising resonance below,
     * band-pass focus 350 Hz -> 4 kHz above (distortion.c) */
    {
        int32_t g = knob_pos(k[1]);
        fp32 x0 = FP(14), x1 = FP(82), by = FP(122);
        if (g < 14336) {
            int32_t frac = 9557 + ((g * 39995) >> 15);                    /* Q15 log(fc / 20 Hz) / log(1000) */
            fp32 fx = x0 + (fp32)((frac * 68) >> 5);
            fp32 bump = (fp32)(((14336 - g) * 18725) >> 15);              /* resonance: up to 8 units at 0 */
            fp32 px[5] = { x0, fx - FP(5), fx, fx + FP(12), x1 };
            fp32 py[5] = { by, by, by - bump, by + FP(14), by + FP(14) };
            for (int i = 0; i < 4; i++) {
                fp32 a = px[i] < x0 ? x0 : px[i] > x1 ? x1 : px[i], b = px[i + 1] < x0 ? x0 : px[i + 1] > x1 ? x1 : px[i + 1];
                if (b > a)
                    line_fp(r, a, py[i], b, py[i + 1], FP(2), GREEN);
            }
        } else if (g > 18432) {
            int32_t frac = 13579 + (((g - 18432) * 26418) >> 15);         /* 350 Hz .. 4 kHz */
            fp32 fx = x0 + (fp32)((frac * 68) >> 5);
            fp32 px[5] = { x0, fx - FP(12), fx, fx + FP(12), x1 };
            fp32 py[5] = { by + FP(8), by + FP(8), by - FP(6), by + FP(8), by + FP(8) };
            for (int i = 0; i < 4; i++) {
                fp32 a = px[i] < x0 ? x0 : px[i] > x1 ? x1 : px[i], b = px[i + 1] < x0 ? x0 : px[i + 1] > x1 ? x1 : px[i + 1];
                if (b > a)
                    line_fp(r, a, py[i], b, py[i + 1], FP(2), GREEN);
            }
        } else {
            line_fp(r, x0, by, x1, by, FP(2), GREEN);
        }
        line_fast(r, x0, by + FP(16), x1, by + FP(16), DARKBLUE);
    }
    /* white: the transfer curve (drive, character and auto gain), 64 segments */
    if (distortion_curve) {
        fp32 xp = 0, yp = 0;
        for (int i = 0; i <= 64; i++) {
            int32_t xin = i * 1024 - 32768;
            int32_t y = distortion_curve(k, xin > 32767 ? 32767 : xin);
            fp32 x = FP(DS_CX - DS_HX) + FP(i * 2 * DS_HX) / 64;
            fp32 yy = FP(DS_CY) - (fp32)((y * DS_HY) >> 5);           /* DS_HY units at 32768 */
            if (i)
                line_fp(r, xp, yp, x, yy, FP(2), WHITE);
            xp = x;
            yp = yy;
        }
    }
    mix_bar(r, k[3]);
}

/* ---- SVG screens (TE's retained renderer, notes/svg-route.md): our own documents in content/display/, loaded on
 * first show (never at boot), only after checking the file exists (TE's loader asserts on a missing file). Nodes
 * are found once with must = 0; a missing id leaves its pointer 0 and the code skips it. Per frame: group matrices
 * and the visible flag (byte +0x50), then one render; our line overlay goes on top. ---- */

#define NODE_VISIBLE 0x50

struct svgdoc {
    void *doc;                              /* first word: TE's render takes a "screen" whose word 0 is the doc */
    int32_t state;                          /* 0 not tried, 1 loaded, -1 missing (draw the line version) */
};

static int svg_open(struct svgdoc *d, const char *path, const char *name)
{
    if (!d->state) {
        int fd = te_yaffs_open(path, 0, 0x100);
        d->state = -1;
        if (fd >= 0) {
            te_yaffs_close(fd);
            d->doc = te_svg_load(name);
            d->state = d->doc ? 1 : -1;
        }
    }
    return d->state > 0;
}

static void show(void *node, int on)
{
    if (node)
        ((volatile uint8_t *)node)[NODE_VISIBLE] = on ? 1 : 0;
}

/* Matrix {a, b, 0, c, d, 0, e, f, 1} (Q10, row vector: x' = a x + c y + e, y' = b x + d y + f). */
static void set_matrix(void *node, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f)
{
    if (!node)
        return;
    int32_t m[9] = { a, b, 0, c, d, 0, e, f, 1024 };
    te_node_matrix(node, m);
}

/* Rotation by `turn` (uint32 = one turn) about (px, py) (whole units), optionally followed by a translation. */
static void set_rotation(void *node, uint32_t turn, int32_t px, int32_t py)
{
    int32_t c = cos_t(turn) >> 5, sn = sin_t(turn) >> 5;        /* Q10 */
    set_matrix(node, c, sn, -sn, c, px * (1024 - c) + sn * py, py * (1024 - c) - sn * px);
}

/* Rotation by `turn` and uniform scale `sc` (Q10) about (px, py) (whole units). */
static void set_rot_scale(void *node, uint32_t turn, int32_t sc, int32_t px, int32_t py)
{
    int32_t c = ((cos_t(turn) >> 5) * sc) >> 10, sn = ((sin_t(turn) >> 5) * sc) >> 10;    /* Q10 */
    set_matrix(node, c, sn, -sn, c, px * (1024 - c) + sn * py, py * (1024 - c) - sn * px);
}

/* Vertical scale `sy` (Q10) about y = cy10 (Q10 units). */
static void set_vscale(void *node, int32_t sy, int32_t cy10)
{
    set_matrix(node, 1024, 0, 0, sy, 0, cy10 - ((sy * cy10) >> 10));
}

/* Render a document; svg_cost() (--svg-cost builds) then shows the kilocycles since the render started (render +
 * whatever overlay was drawn in between), right-aligned in the bottom corner. */
static uint32_t svg_t0 RAM = 0;

static void svg_render(void *doc)
{
    struct { void *doc; } screen = { doc };
    svg_t0 = cycles();
    te_svg_render(&screen);
}

static void svg_cost(void *r)
{
#ifdef SVG_COST
    char buf[12];
    uint32_t kc = (cycles() - svg_t0) >> 10, n = 0;
    do {
        buf[10 - n++] = '0' + kc % 10;
        kc /= 10;
    } while (kc && n < 9);
    buf[11] = 0;
    text_fast(r, buf + 11 - n, FP(316) - FP(6) * n, FP(132), FP(1), GREY);
#else
    (void)r;
#endif
}

/* A cached fill or stroke colour of a node (TE's style getters assert on a missing id: checked with find first),
 * with its colour from the file. */
struct recol {
    uint8_t *p;
    uint8_t orig[3];
};

static void recol_init(struct recol *c, void *doc, const char *id, int stroke)
{
    struct { void *doc; } screen = { doc };
    c->p = te_node_find(doc, id, 0) ? (stroke ? te_svg_stroke(&screen, id) : te_svg_fill(&screen, id)) : 0;
    for (int i = 0; i < 3; i++)
        c->orig[i] = c->p ? c->p[i] : 0;
}

/* Colour 0xRRGGBB scaled by v (0 .. 256). */
static void recol_set(struct recol *c, uint32_t rgb, int32_t v)
{
    if (!c->p)
        return;
    c->p[0] = (uint8_t)((((rgb >> 16) & 255) * v) >> 8);
    c->p[1] = (uint8_t)((((rgb >> 8) & 255) * v) >> 8);
    c->p[2] = (uint8_t)(((rgb & 255) * v) >> 8);
}

static void recol_fade(struct recol *c, int32_t v)
{
    recol_set(c, (uint32_t)c->orig[0] << 16 | (uint32_t)c->orig[1] << 8 | c->orig[2], v);
}

/* Affine maps (Q10; e, f in Q10 units), x' = a x + c y + e, y' = b x + d y + f. */
struct aff {
    int32_t a, b, c, d, e, f;
};

/* (No struct assignment or by-value structs: GCC turns those into memcpy, and there is no libc.) */
static void aff_mul(struct aff *m, const struct aff *m1, const struct aff *m2)     /* m2 first, then m1 */
{
    m->a = (m1->a * m2->a + m1->c * m2->b) >> 10;
    m->b = (m1->b * m2->a + m1->d * m2->b) >> 10;
    m->c = (m1->a * m2->c + m1->c * m2->d) >> 10;
    m->d = (m1->b * m2->c + m1->d * m2->d) >> 10;
    m->e = ((m1->a * m2->e + m1->c * m2->f) >> 10) + m1->e;
    m->f = ((m1->b * m2->e + m1->d * m2->f) >> 10) + m1->f;
}

static void aff_rot(struct aff *m, uint32_t turn, fp32 px, fp32 py)    /* about (px, py), Q10 units */
{
    int32_t c = cos_t(turn) >> 5, sn = sin_t(turn) >> 5;
    m->a = c;
    m->b = sn;
    m->c = -sn;
    m->d = c;
    m->e = px - ((c * px - sn * py) >> 10);
    m->f = py - ((sn * px + c * py) >> 10);
}

static void set_aff(void *node, const struct aff *m)
{
    set_matrix(node, m->a, m->b, m->c, m->d, m->e, m->f);
}

static void aff_line(void *r, const struct aff *m, fp32 x0, fp32 y0, fp32 x1, fp32 y1, fp32 w, int colour)
{
    line_fp(r, ((m->a * x0 + m->c * y0) >> 10) + m->e, ((m->b * x0 + m->d * y0) >> 10) + m->f,
            ((m->a * x1 + m->c * y1) >> 10) + m->e, ((m->b * x1 + m->d * y1) >> 10) + m->f, w, colour);
}

/* Deterministic noise, Q15 -1 .. 1, a new value `rate_q8` / 256 times per second (30 fps frame clock). */
static int32_t jit(uint32_t frame, uint32_t seed, uint32_t rate_q8)
{
    uint32_t n = (frame * ((rate_q8 * 8738u) >> 10)) >> 16;
    uint32_t x = n * 0x9E3779B1u ^ seed * 0x85EBCA77u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return (int32_t)(int16_t)(x >> 16);
}

/* ensemble: the choir (design/fx/choir.svg; ids and behaviour in its header). */
enum { C_S, C_HEAD, C_HALO, C_CONE, C_MOUTH, C_SHUT, C_R, C_KINDS };
static const char *const choir_ids[C_KINDS][3] = {
    { "s1", "s2", "s3" }, { "head1", "head2", "head3" }, { 0, "halo2", "halo3" }, { 0, "cone2", "cone3" },
    { "mouth1", "mouth2", "mouth3" }, { "shut1", "shut2", "shut3" }, { "r1", "r2", "r3" } };
static const char *const note_ids[6] = { "n1", "n2", "n3", "n4", "n5", "n6" };
/* The backing singers fade in from black with white: these fills and strokes are scaled toward black. Their colour
 * values are unique in choir.svg (TE may share one style entry between elements of equal colour). */
/* hair2 / hair3 are groups of one-subpath leaves (notes/svg-route.md "Path quirks"): recolour the leaves */
static const char *const fade_fill[2][8] = { { "robe2", "hair2_1", "hair2_2", "face2", "book2", "cheek2", "mo2", "cone2" },
                                             { "robe3", "hair3_1", "hair3_2", "face3", "book3", "cheek3", "mo3", "cone3" } };
static const char *const fade_stroke[2][5] = { { "robe2", "hair2_1", "hair2_2", "face2", "shut2" },
                                               { "robe3", "hair3_1", "hair3_2", "face3", "shut3" } };
#define FADES (2 * (8 + 5))
static uint8_t *fade_rgba[FADES] RAM = { 0 };
static uint8_t fade_orig[FADES][3] RAM = { { 0 } };
static int32_t fade_level RAM = -1;
static struct svgdoc choir RAM = { 0, 0 };
static void *choir_node[C_KINDS][3] RAM = { { 0 } };
static void *choir_note[6] RAM = { 0 };
static uint32_t choir_frame RAM = 0;

static int choir_ready(void)
{
    if (choir.state)
        return choir.state > 0;
    if (!svg_open(&choir, "/yaffs2/content/display/choir.svg", "choir.svg"))
        return 0;
    for (int k = 0; k < C_KINDS; k++)
        for (int i = 0; i < 3; i++)
            if (choir_ids[k][i])
                choir_node[k][i] = te_node_find(choir.doc, choir_ids[k][i], 0);
    for (int j = 0; j < 6; j++)
        choir_note[j] = te_node_find(choir.doc, note_ids[j], 0);
    struct { void *doc; } screen = { choir.doc };
    int n = 0;
    for (int i = 0; i < 2; i++) {                   /* the getters assert on a missing id: check with find first */
        for (int j = 0; j < 8; j++, n++)
            fade_rgba[n] = te_node_find(choir.doc, fade_fill[i][j], 0) ? te_svg_fill(&screen, fade_fill[i][j]) : 0;
        for (int j = 0; j < 5; j++, n++)
            fade_rgba[n] = te_node_find(choir.doc, fade_stroke[i][j], 0) ? te_svg_stroke(&screen, fade_stroke[i][j]) : 0;
    }
    for (n = 0; n < FADES; n++)
        for (int c = 0; c < 3; c++)
            fade_orig[n][c] = fade_rgba[n] ? fade_rgba[n][c] : 0;
    return 1;
}

/* Screen-side motion phases, clocked by the 6.5 Hz shimmer phase (a steady audio clock): the sway follows the blue
 * rate exactly up to 1 Hz, then at 0.35x of the excess (8 Hz -> ~3.5 Hz on screen), so it keeps speeding up over
 * the whole knob without becoming a blur; the star flick runs at a steady 3 Hz. */
struct view { uint32_t last_vib, sway, flick; };
static struct view choir_view RAM = { 0, 0, 0 };

static void choir_motion(int32_t rate_k, uint32_t vib)
{
    uint32_t dv = vib - choir_view.last_vib;
    choir_view.last_vib = vib;
    if (dv > 0x40000000u)                                           /* > a quarter turn: a stall or a jump */
        dv = 0x40000000u;
    uint32_t hz = ens_rate_q16(rate_k);                             /* Q16 */
    uint32_t vhz = hz <= 65536 ? hz : 65536 + (((hz - 65536) * 90) >> 8);
    uint32_t ratio = ((vhz >> 4) * 10082u) >> 12;                   /* vhz / 6.5, Q16 */
    choir_view.sway += (dv >> 16) * ratio;
    choir_view.flick += (dv >> 16) * 30247u;                        /* 3 / 6.5, Q16 */
}

static void draw_choir(void *r, const int16_t *k, int stereo)
{
    static const int32_t xs[3] = { 160, 90, 230 };
    const struct ens *st = ensemble_state ? &ensemble_state[stereo] : 0;
    choir_motion(knob_pos(k[0]), st ? st->vib : 0);
    uint32_t lfo = choir_view.sway, vib = choir_view.flick;
    int32_t depth = knob_pos(k[1]), ch = knob_pos(k[2]), mix = knob_pos(k[3]);
    /* white = character: CE a solo; DUAL two silhouettes behind (a second line); the ensemble fades them in fully */
    int32_t ens = ens_amount(ch), fade = (ens_dual(ch) >> 9) + ((ens * 3) >> 9);   /* 0, 64 (DUAL), .. 255 */
    int backing = fade > 0, open = mix >= 1638;                     /* singers 2/3 on stage; mouths open (5 %) */
    if (fade != fade_level) {                       /* white: the backing singers fade in from black */
        fade_level = fade;
        for (int n = 0; n < FADES; n++)
            if (fade_rgba[n])
                for (int c = 0; c < 3; c++)
                    fade_rgba[n][c] = (uint8_t)((fade_orig[n][c] * fade) >> 8);
    }
    choir_frame++;
    for (int i = 0; i < 3; i++) {
        int on = i == 0 || backing;
        uint32_t off = (uint32_t)i * 0x55555555u;                    /* 120 degrees apart */
        int32_t t = tri16((lfo + off) >> 16);                       /* -32768 .. 32766 */
        int32_t sway = (depth * t) >> 15;                           /* Q15 of 9 degrees */
        set_rotation(choir_node[C_S][i], (uint32_t)(sway * 3277), xs[i], 124);       /* 9 deg = 0.025 turn */
        set_rotation(choir_node[C_HEAD][i], (uint32_t)(((sway * 19661) >> 15) * 3277), xs[i], 84);
        set_vscale(choir_node[C_MOUTH][i], 307 + ((mix * 717) >> 15), FP(78) + (FP(1) >> 1));
        show(choir_node[C_S][i], on);
        show(choir_node[C_CONE][i], on);
        show(choir_node[C_MOUTH][i], open);
        show(choir_node[C_SHUT][i], !open);
        /* green stars: grow with depth, flick +-20 degrees at the 6.5 Hz shimmer */
        set_rot_scale(choir_node[C_HALO][i], (vib >> 31) ? 0x0E38E38Eu : 0xF1C71C72u, 614 + ((depth * 922) >> 15),
                      xs[i], 72);
        show(choir_node[C_HALO][i], on && depth > 0 && ens > 0);
        if (stereo) {                                               /* master: reflections lean in anti-phase */
            int32_t tr = tri16((lfo + off + 0x80000000u) >> 16);
            set_rotation(choir_node[C_R][i], (uint32_t)(-((depth * tr) >> 15) * 3277), xs[i], 124);
        }
        show(choir_node[C_R][i], stereo && on);
    }
    int notes = (mix * 6 + 16384) >> 15;                            /* round(6 mix) */
    for (int j = 0; j < 6; j++) {
        uint32_t u = (choir_frame * 1311u + (uint32_t)j * 10923u) & 0xFFFF;   /* rise: ~0.6 per s at 30 fps */
        int32_t wob = sin_t(u << 18) >> 4;                          /* +-2 units, Q10 */
        set_matrix(choir_note[j], 1024, 0, 0, 1024, wob, -(int32_t)((34u * u) >> 6));
        show(choir_note[j], j < notes);
    }
    svg_render(choir.doc);
    svg_cost(r);
}

/* log2(x) in Q10 for x >= 1: the leading bit plus a linear mantissa (< 0.09 octave off; for pictures). */
static int32_t log2_q10(uint32_t x)
{
    int32_t n = 0;
    while (n < 31 && (x >> (n + 1)))
        n++;
    return (n << 10) + (int32_t)((n >= 10 ? x >> (n - 10) : x << (10 - n)) & 1023);
}

/* pingpong: table tennis (design/fx/pingpong.svg, made by design/fx/gen_pingpong.py; ids and behaviour in its
 * header). The ball's flights are clocked by the delay line's write index (audio time); one flight per delay, or a
 * whole multiple of it of at least 0.25 s. */
enum { P_TOP, P_LEGL, P_LEGR, P_PADL, P_PADR, P_BATL, P_BATR, P_BALL, P_SHADE, P_GLINT, P_GLOW, P_SHADOW, P_TABLE,
       P_WALL, P_KINDS };
static const char *const pp_ids[P_KINDS] = { "tabletop", "legL", "legR", "padL", "padR", "batL", "batR", "ball",
    "ballshade", "ballglint", "glow", "shadow", "table", "wall" };
#define PIPS 11
#define PP_PIVOT_X 19                                               /* the paddles turn about their handle ends: */
#define PP_PIVOT_Y 111                                              /* (19, 111) and (301, 111) (pingpong.svg) */
static struct svgdoc tennis RAM = { 0, 0 };
static void *pp_node[P_KINDS] RAM = { 0 };
static struct recol pp_ball RAM = { 0, { 0 } };
static struct recol pp_pip[PIPS][2] RAM = { { { 0, { 0 } } } };
static int32_t pp_lit RAM = -1, pp_tone RAM = -1;
struct pp_view { uint32_t last_w, fpos, flights, glow; };
static struct pp_view pp_view[2] RAM = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
#define DEG 11930465u                                               /* one degree in uint32 turns */
#define HIT 5292                                                    /* 0.12 s: the swing after a hit */

static int tennis_ready(void)
{
    if (tennis.state)
        return tennis.state > 0;
    if (!svg_open(&tennis, "/yaffs2/content/display/pingpong.svg", "pingpong.svg"))
        return 0;
    if (!te_node_find(tennis.doc, "padL", 0)) {                     /* an older pingpong.svg (players) left on the */
        tennis.state = -1;                                          /* disk: the line screen */
        return 0;
    }
    for (int i = 0; i < P_KINDS; i++)
        pp_node[i] = te_node_find(tennis.doc, pp_ids[i], 0);
    recol_init(&pp_ball, tennis.doc, "ballbody", 0);
    char id[6];                                                     /* (no array initialiser: memcpy) */
    id[0] = 'p', id[1] = 'i', id[2] = 'p', id[5] = 0;
    for (int j = 0; j < PIPS; j++) {
        id[3] = j < 9 ? '1' + j : '1';
        id[4] = j < 9 ? 0 : '0' + j - 9;
        recol_init(&pp_pip[j][0], tennis.doc, id, 0);
        recol_init(&pp_pip[j][1], tennis.doc, id, 1);
    }
    return 1;
}

static void translate(void *node, fp32 x, fp32 y)
{
    set_matrix(node, 1024, 0, 0, 1024, x, y);
}

/* One echo arch from (x0, y0) to (x1, y1), peak h above the chord (Q10), in n segments. */
static void arch(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, fp32 h, int n, fp32 w, int colour)
{
    fp32 px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        int32_t t = (int32_t)udiv((uint32_t)i << 10, (uint32_t)n), tt = (t * (1024 - t)) >> 10;
        fp32 x = x0 + (((x1 - x0) * t) >> 10), y = y0 + (((y1 - y0) * t) >> 10) - ((4 * h * tt) >> 10);
        if (w)
            line_fp(r, px, py, x, y, w, colour);
        else
            line_fast(r, px, py, x, y, colour);
        px = x;
        py = y;
    }
}

static void draw_tennis(void *r, const int16_t *k, int stereo)
{
    int32_t g = knob_pos(k[1]), tone = knob_pos(k[2]), mix = knob_pos(k[3]);
    const struct pp *st = pingpong_state ? &pingpong_state[stereo] : 0;
    struct pp_view *v = &pp_view[stereo];
    uint32_t w = st ? st->w : 0, dw = w - v->last_w;
    v->last_w = w;
    if (!dw || dw > 44100)                                          /* FX not running: a 30 fps frame of time */
        dw = 1470;
    uint32_t d = pp_delay(k, stereo);
    if (d < 441)
        d = 441;
    uint32_t period = d;
    while (period < 11025)                                          /* at least 0.25 s per flight */
        period += d;
    v->fpos += dw;
    while (v->fpos >= period) {
        v->fpos -= period;
        v->flights++;
    }
    uint32_t since = v->fpos;
    int32_t ph = (int32_t)udiv(v->fpos << 10, period);              /* 0 .. 1023 */
    int right = stereo && (v->flights & 1);                         /* the hitter: R on odd flights (stereo) */

    /* blue: the table length follows the delay played, log scale: 20 ms .. 1.48 s = half 40 .. 104 */
    int32_t oct = log2_q10(d) - log2_q10(PP_D_MIN);                 /* 0 .. 6.21 octaves, Q10 */
    fp32 half = FP(40) + clamp_i((oct * 10551) >> 10, 0, FP(64)), dx = FP(104) - half;
    int32_t sx = (half * 10082) >> 20;                              /* half / 104, Q10 */
    set_matrix(pp_node[P_TOP], sx, 0, 0, 1024, 160 * (1024 - sx), 0);
    translate(pp_node[P_LEGL], dx, 0);
    translate(pp_node[P_LEGR], -dx, 0);
    translate(pp_node[P_PADL], dx, 0);
    translate(pp_node[P_PADR], -dx, 0);
    translate(pp_node[P_WALL], -dx, 0);
    show(pp_node[P_TABLE], stereo);
    show(pp_node[P_PADR], stereo);
    show(pp_node[P_WALL], !stereo);

    /* bats: the hitter follows through (24 deg, back to -8 over 0.12 s, then 0), the receiver cocks back (16 deg) over
     * the last 40 % of the flight */
    int32_t hit = since < HIT ? -24 * (int32_t)DEG + 72143 * (int32_t)since : since < 8820 ? 8 * (int32_t)DEG : 0;
    int32_t recv = ph > 614 ? 465579 * (ph - 614) : 0;              /* up to 16 deg: the wide blade stays on screen */
    int32_t al = right ? recv : hit, ar = right ? hit : recv;
    set_rotation(pp_node[P_BATL], (uint32_t)-al, PP_PIVOT_X, PP_PIVOT_Y);
    set_rotation(pp_node[P_BATR], (uint32_t)ar, 320 - PP_PIVOT_X, PP_PIVOT_Y);
    int swoosh = since < HIT;

    /* the ball along the newest arch */
    fp32 xl = FP(158) - half, xr = stereo ? FP(162) + half : FP(155) + half;
    fp32 y0 = FP(70), y1 = stereo ? FP(70) : FP(74);
    int32_t t = (stereo ? right : (int)(v->flights & 1)) ? 1024 - ph : ph, tt = (t * (1024 - t)) >> 10;
    fp32 bx = xl + (((xr - xl) * t) >> 10), by = y0 + (((y1 - y0) * t) >> 10) - ((4 * FP(42) * tt) >> 10);
    translate(pp_node[P_BALL], bx, by);
    int32_t ssx = 1024 - (((FP(70) - by) * 10486) >> 20);           /* 1 - h / 100: smaller as the ball flies up */
    set_matrix(pp_node[P_SHADOW], ssx < 307 ? 307 : ssx, 0, 0, 1024, bx, stereo ? FP(100) : FP(131));

    /* white: the ball's look */
    int band = tone < 8192 ? 0 : tone < 14746 ? 1 : 2;
    if (band != pp_tone) {
        pp_tone = band;
        recol_set(&pp_ball, band == 0 ? 0x7C7990 : band == 1 ? 0xAEB1DC : 0xFFFFFF, 256);
    }
    show(pp_node[P_SHADE], band < 2);
    show(pp_node[P_GLINT], band == 2);
    v->glow += dw * 97391u;                                         /* one turn per second */
    show(pp_node[P_GLOW], tone > 18022);
    if (tone > 18022)
        set_rot_scale(pp_node[P_GLOW], v->glow, 358 + ((665 * ((tone - 16384) >> 4)) >> 10), 0, 0);

    /* orange: the scoreboard */
    int32_t lit = (mix * PIPS + 16384) >> 15;
    if (lit != pp_lit) {
        pp_lit = lit;
        for (int j = 0; j < PIPS; j++)
            for (int c = 0; c < 2; c++)
                recol_set(&pp_pip[j][c], j < lit ? 0xFF423F : 0x4E2832, 256);
    }
    svg_render(tennis.doc);

    /* green: the echo arches, oldest first, the newest on top */
    int32_t gq = (g * 31130) >> 15, lv[8], n = 0;
    for (int32_t a = 32768; a > 1311 && n < 8; a = (a * gq) >> 15)
        lv[n++] = a;
    for (int j = n - 1; j >= 0; j--) {
        fp32 h = FP(6) + ((36 * lv[j]) >> 5);
        if (j == 0)
            arch(r, xl, y0, xr, y1, h, 14, FP(2), LIGHTGREEN);
        else if (j <= 5)
            arch(r, xl, y0, xr, y1, h, 8, FP(1) + 205, GREEN);
        else
            arch(r, xl, y0, xr, y1, h, 16, 0, DARKGREEN);
    }
    if (swoosh) {                                                   /* a swoosh in front of the hitter's paddle */
        fp32 hx = right ? xr : xl;
        int32_t side = right ? -1 : 1;
        for (int32_t rr = 10; rr <= 16; rr += 3) {
            fp32 qx = 0, qy = 0;
            for (int i = 0; i < 4; i++) {
                uint32_t a = (10 + 20 * (uint32_t)i) * DEG;
                fp32 x = hx + side * ((rr * cos_t(a)) >> 5), y = FP(70) + ((rr * sin_t(a)) >> 5);
                if (i)
                    line_fast(r, qx, qy, x, y, LIGHTBLUE);
                qx = x;
                qy = y;
            }
        }
    }
    pp_readout(r, k, st);
    svg_cost(r);
}

/* distortion: the two pedals (design/fx/pedals.svg; ids and behaviour in its header). */
#include "wah_table.h"
enum { D_PEDAL, D_PLATE, D_S1, D_S2, D_S3, D_S4, D_GLOW, D_P1, D_P2, D_P3, D_P4, D_TTOP, D_TSIDE, D_TEND, D_KINDS };
static const char *const dist_ids[D_KINDS] = { "pedal", "plate", "s1", "s2", "s3", "s4", "glow", "p1", "p2", "p3", "p4",
                                               "ttop", "tside", "tend" };
/* Faded with mix toward black; the controls stay unfaded. Each with the styles it has (pedals.svg): TE's style
 * getters assert (Screen.cpp linestyle_id) when asked for a stroke or fill the element does not have. */
enum { FF = 1, FS = 2 };
static const struct { const char *id; uint8_t has; } dist_fade_ids[] = {
    { "shadow", FF }, { "cableO", FS }, { "plugL_1", FF | FS }, { "plugL_2", FF | FS }, { "plugR_1", FF | FS },
    { "plugR_2", FF | FS }, { "collar", FF | FS }, { "jackL", FF | FS }, { "jackR", FF | FS }, { "hilite", FS }, { "lip", FS },
    { "grooves", FF },
    { "screw", FF | FS }, { "slot", FS }, { "wshadow", FF }, { "wroofL", FF | FS }, { "wroofR", FF | FS },
    { "wend", FF | FS }, { "wside", FF | FS }, { "wbrk", FF | FS }, { "rubber", FS }, { "chrome", FF | FS },
    { "shine", FS }, { "wjackI", FS }, { "wjackO", FF | FS }, { "cableI", FS }, { "patch", FS }, { "plugW", FF | FS },
    { "tendf", FF | FS } };
#define DIST_FADES (sizeof dist_fade_ids / sizeof dist_fade_ids[0])
enum { R_BODYF, R_BODYS, R_PLATEB, R_PLAQUE, R_LEDF, R_LEDS, R_RIBS, R_RAYS, R_SPECIAL = R_RAYS + 5 };   /* rays_1..5 */
static struct svgdoc pedals RAM = { 0, 0 };
static void *dist_node[D_KINDS] RAM = { 0 };
static struct recol dist_fade[DIST_FADES][2] RAM = { { { 0, { 0 } } } };
static struct recol dist_rc[R_SPECIAL] RAM = { { 0, { 0 } } };
static int32_t dist_look RAM = -1;
static uint32_t dist_frame RAM = 0;
/* Per mode (TUBE MUFF FUZZ CLIP FOLD CRUSH): body, plaque rim, LED rays. */
static const uint32_t mode_body[6] = { 0xFFB3B3, 0xE6E7FF, 0x5F4270, 0xFF423F, 0x00475B, 0xAEB1DC };
static const uint32_t mode_trim[6] = { 0x010000, 0xFF3A5D, 0x010000, 0x010000, 0x010000, 0x010000 };
static const uint32_t mode_rays[6] = { 0xFF3A5D, 0xFF3A5D, 0xFF90A7, 0xFFFFFF, 0xFF90A7, 0xFF3A5D };
static const int16_t spark_xy[4][2] = { { 113, 21 }, { 207, 23 }, { 213, 77 }, { 107, 76 } };

static int pedals_ready(void)
{
    if (pedals.state)
        return pedals.state > 0;
    if (!svg_open(&pedals, "/yaffs2/content/display/pedals.svg", "pedals.svg"))
        return 0;
    for (int i = 0; i < D_KINDS; i++)
        dist_node[i] = te_node_find(pedals.doc, dist_ids[i], 0);
    for (unsigned i = 0; i < DIST_FADES; i++)
        for (int c = 0; c < 2; c++)
            if (dist_fade_ids[i].has & (c ? FS : FF))
                recol_init(&dist_fade[i][c], pedals.doc, dist_fade_ids[i].id, c);
    recol_init(&dist_rc[R_BODYF], pedals.doc, "body", 0);
    recol_init(&dist_rc[R_BODYS], pedals.doc, "body", 1);
    recol_init(&dist_rc[R_PLATEB], pedals.doc, "plateb", 0);
    recol_init(&dist_rc[R_PLAQUE], pedals.doc, "plaque", 1);
    recol_init(&dist_rc[R_LEDF], pedals.doc, "led", 0);
    recol_init(&dist_rc[R_LEDS], pedals.doc, "led", 1);
    static const char *const rays[5] = { "rays_1", "rays_2", "rays_3", "rays_4", "rays_5" };  /* one subpath each */
    for (int i = 0; i < 5; i++)
        recol_init(&dist_rc[R_RAYS + i], pedals.doc, rays[i], 1);
    recol_init(&dist_rc[R_RIBS], pedals.doc, "ribs", 0);
    return 1;
}

static int mode_index(const char *s)
{
    static const char two[6][2] = { { 'T', 'U' }, { 'M', 'U' }, { 'F', 'U' }, { 'C', 'L' }, { 'F', 'O' }, { 'C', 'R' } };
    for (int i = 0; i < 6; i++)
        if (s[0] == two[i][0] && s[1] == two[i][1])
            return i;
    return 0;
}

static void draw_pedals(void *r, const int16_t *k)
{
    int32_t d = knob_pos(k[0]), g = knob_pos(k[1]), m = knob_pos(k[3]);
    const char *word = distortion_mode_name ? distortion_mode_name(k) : "TUBE";
    int mode = mode_index(word);
    uint32_t f = ++dist_frame, rate = 2048 + (uint32_t)((4096 * d) >> 15);   /* shake: 8 + 16 d Hz, Q8 */
    int32_t d15 = (d + ((d * d) >> 15)) >> 1;                       /* ~ d^1.5 */
    int32_t d115 = (d * (27853 + ((4915 * d) >> 15))) >> 15;        /* ~ d^1.15 */

    /* blue: the box shakes, the foot plate rattles, sparks fly */
    struct aff shake = { 1024, 0, 0, 1024, (((3277 * d115) >> 15) * jit(f, 1, rate)) >> 15,
                         (((2048 * d115) >> 15) * jit(f, 2, rate)) >> 15 };
    uint32_t th = (uint32_t)((((d15 * 655) >> 15) * jit(f, 7, rate)));
    struct aff rot, pm;
    aff_rot(&rot, th, FP(160), FP(138));
    aff_mul(&pm, &rot, &shake);
    set_aff(dist_node[D_PEDAL], &pm);
    translate(dist_node[D_PLATE], (((2662 * d115) >> 15) * jit(f, 3, rate)) >> 15,
              (((1843 * d115) >> 15) * jit(f, 4, rate)) >> 15);
    static const int32_t spark_at[4] = { 23593, 26214, 28836, 31130 };
    for (int i = 0; i < 4; i++) {
        int on = d > spark_at[i] && jit(f, 10 + i, 15 << 8) > -11469;
        show(dist_node[D_S1 + i], on);
        if (on)
            set_rot_scale(dist_node[D_S1 + i], (uint32_t)(9102 * jit(f, 20 + i, 15 << 8)), 819 + ((410 * d) >> 15),
                          spark_xy[i][0], spark_xy[i][1]);
    }
    int glow = m > 0 && d > 983;
    show(dist_node[D_GLOW], glow);
    if (glow) {
        int32_t sc = 563 + ((768 * d) >> 15);
        sc = (sc * (1024 + ((((61 * d) >> 15) * jit(f, 5, 20 << 8)) >> 15))) >> 10;
        set_matrix(dist_node[D_GLOW], sc, 0, 0, sc, 160 * (1024 - sc), (97 * (1024 - sc)) >> 1);
    }
    for (int i = 0; i < 4; i++)                                     /* knob pointers: -135 .. +135 degrees */
        set_rotation(dist_node[D_P1 + i], 0xA0000000u + (uint32_t)knob_pos(k[i]) * 98304u, 127 + 22 * i, 34);

    /* green: the wah treadle rocks, heel down (low-pass) .. level .. toe down */
    int32_t a = g < 14336 ? -(((14336 - g) * 74899) >> 15) : g > 18432 ? ((g - 18432) * 74904) >> 15 : 0;
    a = a < -32768 ? -32768 : a > 32768 ? 32768 : a;
    int32_t pos = (a + 32768) * WAH_STEPS, i0 = pos >> 16, fr = (pos & 0xFFFF) >> 6;
    if (i0 >= WAH_STEPS) {
        i0 = WAH_STEPS - 1;
        fr = 1024;
    }
    int32_t wm[18];
    for (int j = 0; j < 18; j++)
        wm[j] = wah_table[i0][j] + (((wah_table[i0 + 1][j] - wah_table[i0][j]) * fr) >> 10);
    for (int j = 0; j < 3; j++)
        set_matrix(dist_node[D_TTOP + j], wm[6 * j], wm[6 * j + 1], wm[6 * j + 2], wm[6 * j + 3], wm[6 * j + 4],
                   wm[6 * j + 5]);

    /* white: body colour per mode; orange: both pedals fade in from black */
    int32_t v = 38 + ((218 * m) >> 15);                             /* 0.15 + 0.85 m, of 256 */
    int32_t look = v | mode << 9 | (m > 0) << 12;
    if (look != dist_look) {
        dist_look = look;
        for (unsigned i = 0; i < DIST_FADES; i++)
            for (int c = 0; c < 2; c++)
                recol_fade(&dist_fade[i][c], v);
        recol_set(&dist_rc[R_BODYF], mode_body[mode], v);
        recol_set(&dist_rc[R_BODYS], mode_body[mode], v);
        recol_set(&dist_rc[R_PLATEB], mode_body[mode], v);
        recol_set(&dist_rc[R_PLAQUE], mode_trim[mode], v);
        recol_set(&dist_rc[R_LEDF], m > 0 ? 0xFF3A5D : 0x4E2832, m > 0 ? v : 256);
        recol_set(&dist_rc[R_LEDS], m > 0 ? 0xFF3A5D : 0x4E2832, m > 0 ? v : 256);
        for (int i = 0; i < 5; i++)
            recol_set(&dist_rc[R_RAYS + i], mode_rays[mode], 256);
        recol_set(&dist_rc[R_RIBS], 0x00ED95, 102 + ((154 * v) >> 8));
    }
    svg_render(pedals.doc);

    /* the mode word on the plaque, shaking with the box (the static group "dist" is translate(-64, 0)) */
    pm.e -= FP(64);
    int len = 0;
    while (word[len])
        len++;
    fp32 step = FP(1) + 819, x = FP(160) - (((6 * len - 2) * step) >> 1), y = FP(59) + 102;
    int colour = v >= 154 ? WHITE : v >= 90 ? PURPLEGREY : GREY;
    for (const char *s = word; *s; s++, x += 6 * step)
        for (const uint8_t *gl = font_seg + font_off[*s - FONT_FIRST]; *gl != 0xFF; gl += 2)
            aff_line(r, &pm, x + (gl[0] >> 4) * step, y + (gl[0] & 15) * step, x + (gl[1] >> 4) * step,
                     y + (gl[1] & 15) * step, FP(1) + 512, colour);
    svg_cost(r);
}

/* crush: the line fallback when content/display/crush3d.svg is missing (draw_pyramid below). Four bars in the knob
 * colours (RATE, BITS, SQUASH, GRIT) and the compressor's gain reduction as a white meter on the right; the hold rate
 * and bit depth below the title. The DSP's view (smoothed knobs, rate, bits, gain reduction, flips) is crush_state
 * (crush.h, notes/fx-crush.md "Screen"). */
static void draw_crush(void *r, const int16_t *k, int stereo)
{
    const struct crush *st = crush_state ? &crush_state[stereo] : 0;
    char buf[24], *p = buf;
    title(r, "CRUSH", WHITE);
    if (st) {
        p = put_dec(p, (uint32_t)clamp_i(st->rate_hz, 0, 99999));
        p = put_str(p, "HZ ");
        p = put_dec(p, (uint32_t)clamp_i(st->bits_q12, 0, 16 << 12) >> 12);
        p = put_str(p, st->mu > 16384 ? "B MU" : "B");
        text(r, buf, FP(8), FP(30), FP(1), GREY);
    }
    for (int i = 0; i < 4; i++) {
        fp32 y = FP(56 + 20 * i);
        int32_t v = knob_pos(k[i]);
        line_fast(r, FP(16), y, FP(256), y, DARKBLUE);
        fp32 w = (fp32)((v * 240) >> 5);                     /* 240 units at 32768 */
        if (w > FP(1))
            line_fp(r, FP(16), y, FP(16) + w, y, FP(3), i == 0 ? BLUE : i == 1 ? GREEN : i == 2 ? WHITE : ORANGE);
    }
    vline_fast(r, FP(290), FP(40), FP(130), DARKBLUE);
    int32_t g = st ? clamp_i(st->gr, 0, 32767) : 0;          /* 0 .. 32767 = 0 .. 30 dB */
    fp32 top = FP(40) + (fp32)((g * 90) >> 5);               /* reduction grows down from the top */
    if (top > FP(41))
        line_fp(r, FP(290), FP(40), FP(290), top, FP(4), WHITE);
}

/* crush: the 3D pyramid (design/fx/crush3d.svg, published as content/display/crush3d.svg; its header documents every
 * id, threshold, matrix and recolour; gen_crush3d.py runtime_state() is the reference, followed here). Inputs from
 * the DSP (crush_state): the smoothed knobs, the real bit depth (bits_q12) and mu-law share, the compressor's gain
 * reduction (the pump); the frame counter is the clock (30 fps). Knobs Q15, matrices Q10. */
enum { P_SUN, P_FARG, P_PYR, P_SMOOTH, P_CMU, P_CAP1, P_CAP2, P_PILE, P_CH1, P_CH2, P_CHR, P_SINGLE };
static const char *const pyr_single[P_SINGLE] = { "sun", "farg", "pyr", "smooth", "cmu", "cap1", "cap2", "pile", "ch1",
                                                  "ch2", "chr" };
enum { PA_VOX, PA_C = 4, PA_H = 8, PA_HW = 11, PA_HF = 14, PA_K = 17, PA_G = 23, PA_GS = 26, PA_D = 29, PA_DS = 33,
       PA_N = 37 };
static const char *const pyr_multi[PA_N] = {
    "vox1", "vox2", "vox3", "vox4", "c1", "c2", "c3", "c4", "h1", "h2", "h3", "hw1", "hw2", "hw3", "hf1", "hf2", "hf3",
    "k1", "k2", "k3", "k4", "k5", "k6", "g1", "g2", "g3", "gs1", "gs2", "gs3", "d1", "d2", "d3", "d4",
    "ds1", "ds2", "ds3", "ds4" };
/* Recoloured styles: (id, stroke?, role). Roles index the palette rows below. Every id is a leaf with that style
 * (TE's style getters assert on a group): the SVG splits long paths into parts a, b, .. (AGG's cell limit), and
 * crush_screen_check checks this table against the file. */
enum { RL_L, RL_R, RL_RS, RL_T, RL_CORE, RL_COURSE, RL_SUN, RL_FARL, RL_FARR, RL_SHADOW, RL_HORIZON, RL_SAND, RL_N };
static const struct { const char *id; uint8_t stroke, role; } pyr_rc_ids[] = {
#include "crush3d_ids.h"
};
#define PYR_RC (sizeof pyr_rc_ids / sizeof pyr_rc_ids[0])
/* BITS posterise rows: A nb >= 9, B 5..9, C 2..5, D < 2 (colors.svg values) */
#define CP_PURPLEGREY 0xAEB1DC
#define CP_LPG 0xE6E7FF
#define CP_WHITE 0xFFFFFF
#define CP_GREY 0x7C7990
#define CP_OFFGREY 0x353238
#define CP_BLACK 0x010000
#define CP_DARKBLUE 0x383572
#define CP_OFFBLUE 0x1D2040
#define CP_GREEN 0x00ED95
static const uint32_t pyr_pal[4][RL_N] = {     /* neutral family; green only as BITS's rim; white only for SQUASH */
    { CP_PURPLEGREY, CP_GREY, CP_GREY, CP_LPG, CP_OFFGREY, CP_DARKBLUE, CP_DARKBLUE, CP_DARKBLUE, CP_OFFBLUE, CP_OFFBLUE,
      CP_GREY, CP_OFFGREY },
    { CP_LPG, CP_OFFGREY, CP_OFFGREY, CP_LPG, CP_BLACK, CP_BLACK, CP_GREY, CP_DARKBLUE, CP_DARKBLUE, CP_BLACK, CP_GREY,
      CP_GREY },
    { CP_LPG, CP_OFFGREY, CP_OFFGREY, CP_LPG, CP_OFFGREY, CP_BLACK, CP_GREY, CP_OFFGREY, CP_OFFGREY, CP_BLACK, CP_LPG,
      CP_BLACK },                                   /* the shade face stays offgrey: black only at 1 bit, with its rim */
    { CP_LPG, CP_BLACK, CP_GREEN, CP_LPG, CP_OFFGREY, CP_BLACK, CP_GREY, CP_OFFGREY, CP_OFFGREY, CP_BLACK, CP_LPG,
      CP_BLACK } };
/* Falling debris (header table, Q10): fall DX DY, ground shadow start GX GY and landing LX LY; 1 / period (Q16). */
static const int32_t pyr_deb[4][6] = {
    { -31406, 43612, 122295, 109568, 90889, 107796 }, { 5734, 41441, 219224, 102400, 224963, 110848 },
    { -31406, 22979, 99215, 111268, 67809, 109496 }, { 5734, 66191, 191539, 104448, 197263, 112896 } };
static const uint32_t pyr_deb_rate[4] = { 2185, 1867, 1630, 1447 };  /* 1 / (30 fps x (1 + 0.17 i) s), Q16 */
static const int16_t pyr_crack[6] = { 3277, 8192, 16384, 19661, 24576, 29491 };    /* o >= .1 .25 .5 .6 .75 .9 */
static const int16_t pyr_debris_on[4] = { 16384, 20480, 24576, 28672 };            /* nd = ceil((o - .5) / .5 * 4) */
static const int16_t pyr_rate_lvl[4] = { 4096, 10240, 18432, 25600 };              /* voxel levels: RATE detents */
static struct svgdoc pyramid RAM = { 0, 0 };
static void *pyr_node[P_SINGLE] RAM = { 0 };
static void *pyr_multi_node[PA_N] RAM = { 0 };
static struct recol pyr_rc[PYR_RC] RAM = { { 0, { 0 } } };
static int32_t pyr_look RAM = -1;
static uint32_t pyr_frame RAM = 0;
static uint32_t pyr_deb_start[4] RAM = { 0 };     /* frame a debris fall started, 0 = idle */
/* The value readout: the knob turned last (raw change >= 512), until which frame, per screen context. */
static int32_t pyr_ctx RAM = -1;
static int16_t pyr_ref[4] RAM = { 0 };
static int32_t pyr_shown RAM = 0;
static uint32_t pyr_until RAM = 0;
static uint32_t pyr_flips[2] RAM = { 0 };         /* crush_state flips at the last frame, per instance */

static int pyramid_ready(void)
{
    if (pyramid.state)
        return pyramid.state > 0;
    if (!svg_open(&pyramid, "/yaffs2/content/display/crush3d.svg", "crush3d.svg"))
        return 0;
    if (!te_node_find(pyramid.doc, "cells4", 0)) {                 /* not the cell-budgeted file (notes/screen.md): an */
        pyramid.state = -1;                                         /* older crush3d.svg could overflow AGG: lines */
        return 0;
    }
    for (int i = 0; i < P_SINGLE; i++)
        pyr_node[i] = te_node_find(pyramid.doc, pyr_single[i], 0);
    for (int i = 0; i < PA_N; i++)
        pyr_multi_node[i] = te_node_find(pyramid.doc, pyr_multi[i], 0);
    for (unsigned i = 0; i < PYR_RC; i++)
        recol_init(&pyr_rc[i], pyramid.doc, pyr_rc_ids[i].id, pyr_rc_ids[i].stroke);
    return 1;
}

/* "RATE 11.0KHZ", "BITS 8 MU-LAW" / "BITS 7.5 MU 50%", "SQUASH 4.0:1", "JITTER 40%" / "FLIP 25%": the knob's value as
 * the DSP has it (at most 15 characters, drawn at title size: draw_pyramid). */
static char *put_frac(char *p, uint32_t v, uint32_t div)     /* v / div with log10(div) decimals, v already rounded */
{
    uint32_t w = udiv(v, div), f = v - w * div;
    p = put_dec(p, w);
    *p++ = '.';
    if (div == 100 && f < 10)
        *p++ = '0';
    return put_dec(p, f);
}

static void crush_readout(char *p, int knob, const struct crush *st, const int32_t *k)
{
    if (knob == 0) {
        uint32_t hz = st ? (uint32_t)clamp_i(st->rate_hz, 0, 99999) : 44100;
        p = put_str(p, "RATE ");
        if (hz >= 10000)
            put_str(put_frac(p, udiv(hz + 50, 100), 10), "KHZ");          /* 11.0KHZ */
        else if (hz >= 1000)
            put_str(put_frac(p, udiv(hz + 5, 10), 100), "KHZ");           /* 5.51KHZ */
        else
            put_str(put_dec(p, hz), "HZ");
    } else if (knob == 1) {
        int32_t nb = st ? clamp_i(st->bits_q12, 4096, 16 << 12) : 16 << 12, w = (nb + 2048) >> 12, f = nb - (w << 12);
        p = put_str(p, "BITS ");
        if (f >= -204 && f <= 204)                                         /* on a whole bit (the plateaus) */
            p = put_dec(p, (uint32_t)w);
        else
            p = put_frac(p, (uint32_t)((nb * 10 + 2048) >> 12), 10);
        if (st && st->mu >= 32768)
            put_str(p, " MU-LAW");                                      /* the full mu-law plateau (detent 16) */
        else if (st && st->mu > 327)                                    /* morphing: how far, 1 .. 99 % */
            put_str(put_dec(put_str(p, " MU "), (uint32_t)((clamp_i(st->mu, 0, 32768) * 100 + 16384) >> 15)), "%");
    } else if (knob == 2) {
        int32_t sq = k[2], c = (31130 * ((sq * (65536 - sq)) >> 15)) >> 15;   /* crush.c's slope 1 - 1 / ratio */
        p = put_str(p, "SQUASH ");
        if (!sq)
            put_str(p, "OFF");
        else {
            uint32_t t = udiv(327680u + (uint32_t)((32768 - c) >> 1), (uint32_t)(32768 - c));   /* ratio x 10 */
            put_str(t >= 100 ? put_dec(p, udiv(t + 5, 10)) : put_frac(p, t, 10), ":1");
        }
    } else if (k[3] == 0)
        put_str(p, "GRIT OFF");
    else if (k[3] <= 16384)                                             /* lower half: jitter amount */
        put_str(put_dec(put_str(p, "JITTER "), (uint32_t)((k[3] * 200 + 16384) >> 15)), "%");
    else                                                                /* upper half: flips (jitter at full) */
        put_str(put_dec(put_str(p, "FLIP "), (uint32_t)(((k[3] - 16384) * 200 + 16384) >> 15)), "%");
}

static void draw_pyramid(void *r, const int16_t *kraw, int running, int stereo, int ctx)
{
    const struct crush *st = crush_state ? &crush_state[stereo] : 0;
    /* every input clamped to what crush.c publishes (crush_fuzz_check.py: no state makes degenerate geometry). The
     * knobs: crush's smoothed ones (after TE's LFO) while it runs, else the stored ones (kraw: FX off, other sound) */
    int32_t k[4];
    for (int i = 0; i < 4; i++)
        k[i] = st && running ? clamp_i(st->k[i], 0, 32767) : knob_pos(kraw[i]);
    int32_t rr = k[0], s = k[2], o = k[3];
    /* bit depth (Q12) and mu-law as the DSP has them (without the DSP state: pristine) */
    int32_t nb = st ? clamp_i(st->bits_q12, 4096, 16 << 12) : 16 << 12;
    int mu = st && st->mu > 8192;
    int32_t gr = st ? clamp_i(st->gr, 0, 32767) : 0;
    int live = !st || st->level > 33;                            /* something is playing (> -60 dBFS) */
    uint32_t df = st ? st->flips - pyr_flips[stereo] : 0;        /* flips since the last frame (the DSP counts flip
                                                                  * events on its clock; they only act on sound) */
    if (!live)
        df = 0;
    if (st)
        pyr_flips[stereo] = st->flips;
    uint32_t f = ++pyr_frame;

    /* the readout: a knob moved >= 512 raw shows its value ~1.5 s (not on entering the screen or another sound) */
    if (ctx != pyr_ctx) {
        pyr_ctx = ctx;
        pyr_until = f;
        for (int i = 0; i < 4; i++)
            pyr_ref[i] = kraw[i];
    }
    for (int i = 0; i < 4; i++) {
        int32_t d = kraw[i] - pyr_ref[i];
        if (d >= 512 || d <= -512) {
            pyr_ref[i] = kraw[i];
            pyr_shown = i;
            pyr_until = f + 45;
        }
    }

    /* blue RATE: one voxel level replaces the smooth pyramid, from RATE detents 4 / 10 / 18 / 25 */
    int lvl = 0;
    for (int i = 0; i < 4; i++)
        lvl += rr >= pyr_rate_lvl[i];
    show(pyr_node[P_SMOOTH], lvl == 0);
    for (int i = 0; i < 4; i++)
        show(pyr_multi_node[PA_VOX + i], i + 1 == lvl);
    /* green BITS: courses, mu-law courses, missing stones, collapsed top, rubble */
    int tiers = nb >= 12 << 12 ? 4 : nb >= 8 << 12 ? 3 : nb >= 4 << 12 ? 2 : nb >= 2 << 12 ? 1 : 0;
    for (int i = 0; i < 4; i++)
        show(pyr_multi_node[PA_C + i], !mu && i + 1 <= tiers);
    show(pyr_node[P_CMU], mu);
    for (int i = 0; i < 3; i++) {
        int on = i == 0 ? nb < 8 << 12 : nb <= (i == 1 ? 5 : 3) << 12;
        show(pyr_multi_node[PA_H + i], on);
        show(pyr_multi_node[PA_HW + i], on);
        show(pyr_multi_node[PA_HF + i], on);
    }
    show(pyr_node[P_CAP1], nb <= 3 << 12);
    show(pyr_node[P_PILE], nb <= 3 << 12);
    show(pyr_node[P_CAP2], nb <= 6144);
    int row = nb >= 9 << 12 ? 0 : nb >= 5 << 12 ? 1 : nb >= 2 << 12 ? 2 : 3;
    if (row != pyr_look) {
        pyr_look = row;
        for (unsigned i = 0; i < PYR_RC; i++)
            recol_set(&pyr_rc[i], pyr_pal[row][pyr_rc_ids[i].role], 256);
    }

    /* orange GRIT: the whole pyramid trembles with the clock jitter while something plays */
    int32_t dx = o > 1638 && live ? ((512 + ((1536 * o) >> 15)) * jit(f, 1, (10 << 8) + ((7680 * o) >> 15))) >> 15 : 0;
    int32_t dy = o > 9830 && live ? (((614 * o) >> 15) * jit(f, 2, (14 << 8) + ((7680 * o) >> 15))) >> 15 : 0;
    translate(pyr_node[P_PYR], dx, dy);

    /* orange GRIT: cracks and notches show the amount (static); the DSP's bit flips shed debris (a fall starts on a
     * frame with flips) and shift XOR slices (more flips, more slices) */
    for (int i = 0; i < 6; i++)
        show(pyr_multi_node[PA_K + i], o >= pyr_crack[i]);
    show(pyr_node[P_CH1], o >= 16384);
    show(pyr_node[P_CH2], o >= 24576);
    show(pyr_node[P_CHR], o >= 24576);
    int spawned = 0;
    for (int i = 0; i < 4; i++) {
        int slot = o > pyr_debris_on[i];
        uint32_t u = pyr_deb_start[i] ? (f - pyr_deb_start[i]) * pyr_deb_rate[i] : 0;   /* Q16 */
        if (pyr_deb_start[i] && (u > 81920 || !slot))
            pyr_deb_start[i] = 0;                                   /* fell and rested a quarter period: idle */
        if (!pyr_deb_start[i] && slot && df && !spawned) {
            pyr_deb_start[i] = f;
            u = 0;
            spawned = 1;
        }
        int on = pyr_deb_start[i] && u <= 65536;
        show(pyr_multi_node[PA_D + i], on);
        show(pyr_multi_node[PA_DS + i], on);
        if (!on)
            continue;
        int32_t u10 = (int32_t)(u >> 6), u2 = (u10 * u10) >> 10;
        const int32_t *d = pyr_deb[i];
        translate(pyr_multi_node[PA_D + i], (d[0] * u10) >> 10, (d[1] * u2) >> 10);
        int32_t sc = 358 + ((666 * u10) >> 10);
        int32_t X = d[2] + (((d[4] - d[2]) * u10) >> 10), Y = d[3] + (((d[5] - d[3]) * u10) >> 10);
        set_matrix(pyr_multi_node[PA_DS + i], sc, 0, 0, sc, X - ((sc * d[2]) >> 10), Y - ((sc * d[3]) >> 10));
    }
    int ns = !df ? 0 : df < 3 ? 1 : df < 7 ? 2 : 3;
    if (nb <= 3 << 12 && o > 19661 && ns > 2)
        ns = 2;                                                     /* the busiest state: at most 2 slices */
    int start = (int)(((jit(f, 10, 7 << 8) + 32768) * 3) >> 16);    /* 0 .. 2 */
    int32_t amp = 4096 + ((14336 * o) >> 15);                        /* 4 + 14 o, Q10 */
    for (int i = 0; i < 3; i++) {
        int j = i - start < 0 ? i - start + 3 : i - start;
        show(pyr_multi_node[PA_G + i], j < ns);
        int32_t n = jit(f, 20 + i, (uint32_t)(7 + 9 * i) << 8), an = n < 0 ? -n : n;
        int32_t a = (amp * (13107 + ((19661 * an) >> 15))) >> 15;
        translate(pyr_multi_node[PA_GS + i], n >= 0 ? a : -a, 0);
    }
    /* white SQUASH: the sun is its indicator, white, shrinking and dimming with the gain reduction (it pumps with the
     * audio); with SQUASH off it takes the BITS row's colour. Idle parallax: the far pyramids and the sun drift
     * against the big one (16 s orbit). */
    int32_t w = sin_t(f * 8947849u);                                        /* sin(2 pi t / 16), Q15 */
    translate(pyr_node[P_FARG], -((3 * w) >> 5), 0);
    int sq = s >= 655;
    int32_t ks = sq ? 1024 - ((461 * gr) >> 15) : 1024;                    /* 1 - 0.45 gr, Q10 */
    set_matrix(pyr_node[P_SUN], ks, 0, 0, ks, 58 * (1024 - ks) - ((3 * w) >> 6), 38 * (1024 - ks));
    for (unsigned i = 0; i < PYR_RC; i++)
        if (pyr_rc_ids[i].role == RL_SUN)
            recol_set(&pyr_rc[i], sq ? CP_WHITE : pyr_pal[row][RL_SUN], sq ? 256 - ((141 * gr) >> 15) : 256);
#ifdef CRUSH_STAGED
    /* Device diagnosis (build.py --staged-screens): on the first open after boot the layers come up one by one, every
     * 3 s, labelled; a stage that stops the OP-1 (agg.cpp:339) names the culprit layer. */
    static const char *const stage_name[5] = { "STAGE 0 SKY", "STAGE 1 PYRAMID", "STAGE 2 RATE", "STAGE 3 BITS",
                                                "STAGE 4 GRIT" };
    static uint32_t first RAM = 0;
    if (!first)
        first = f;
    uint32_t stage = (f - first) / 90;                              /* 3 s per stage */
    show(pyr_node[P_PYR], stage >= 1);      /* CRUSH6 hid it at stage 0 and never showed it again: always set it */
    if (stage < 5) {
        if (stage < 2) {
            show(pyr_node[P_SMOOTH], 1);
            for (int i = 0; i < 4; i++)
                show(pyr_multi_node[PA_VOX + i], 0);
        }
        if (stage < 3) {
            show(pyr_node[P_CMU], 0);
            for (int i = 0; i < 9; i++)
                show(pyr_multi_node[PA_H + i], 0);                     /* h, hw, hf */
            show(pyr_node[P_CAP1], 0);
            show(pyr_node[P_CAP2], 0);
            show(pyr_node[P_PILE], 0);
        }
        if (stage < 4) {
            for (int i = 0; i < 6; i++)
                show(pyr_multi_node[PA_K + i], 0);
            for (int i = 0; i < 3; i++)
                show(pyr_multi_node[PA_G + i], 0);
            for (int i = 0; i < 8; i++)
                show(pyr_multi_node[PA_D + i], 0);                     /* d1..4, ds1..4 */
            show(pyr_node[P_CH1], 0);
            show(pyr_node[P_CH2], 0);
            show(pyr_node[P_CHR], 0);
        }
        svg_render(pyramid.doc);
        text(r, stage_name[stage], FP(8), FP(7), FP(2), WHITE);
        svg_cost(r);
        return;
    }
#endif
    svg_render(pyramid.doc);
    if ((int32_t)(pyr_until - f) > 0) {                             /* the value readout, title position */
        char buf[28];
        crush_readout(buf, pyr_shown, st, k);
        /* title size 2 (12 high: y 7 .. 19, above the sun at 24 .. 52; 15 characters end at x 184, above the apex) */
        text(r, buf, FP(8), FP(7), FP(2),
             pyr_shown == 0 ? BLUE : pyr_shown == 1 ? GREEN : pyr_shown == 2 ? WHITE : ORANGE);
    }
    svg_cost(r);
}

#ifdef CRUSH_TEST_CARD
/* Device ground truth for TE's SVG route (CRUSH9; build.py --test-card): on the first open of the crush screen after boot, crushcard.svg
 * (design/fx/gen_card.py: 18 numbered cases) for 360 frames (~12 s), each case's expected result beside it in 1-px
 * grey lines (not through TE's parser). Case 17 is a group the C scales to half about its centre, case 18 a grey
 * square the C recolours green. Returns 1 while the card is up. */
#include "crushcard.h"

#define CARD_FRAMES 360
static struct svgdoc card RAM = { 0, 0 };
static uint32_t card_frame RAM = 0;

static int draw_card(void *r)
{
    if (card_frame >= CARD_FRAMES)
        return 0;
    if (!svg_open(&card, "/yaffs2/content/display/crushcard.svg", "crushcard.svg")) {
        card_frame = CARD_FRAMES;
        return 0;
    }
    if (!card_frame++) {
        set_matrix(te_node_find(card.doc, "k17", 0), 512, 0, 0, 512, CARD_GM_CX * 512, CARD_GM_CY * 512);
        struct recol g;
        recol_init(&g, card.doc, "k18", 0);
        recol_set(&g, CP_GREEN, 256);
    }
    svg_render(card.doc);
    for (int i = 0; i < CARD_SEGS; i++)                             /* SVG units x 4 -> Q10 */
        line_fast(r, (fp32)card_seg[i][0] << 8, (fp32)card_seg[i][1] << 8, (fp32)card_seg[i][2] << 8,
                  (fp32)card_seg[i][3] << 8, GREY);
    for (int i = 0; i < CARD_N; i++) {
        char buf[4];
        int n = i + 1, j = 0;
        if (n >= 10)                                                /* 1 .. 18: no division (no libgcc) */
            buf[j++] = '1', n -= 10;
        buf[j++] = (char)('0' + n);
        buf[j] = 0;
        text_fast(r, buf, FP(card_num[i][0]), FP(card_num[i][1]), FP(1), WHITE);
    }
    return 1;
}
#else
#define draw_card(r) 0
#endif

/* The knobs the FX used (fxpub.h) instead of the stored ones, so the screen moves with TE's LFO: TE modulates only its
 * per-block copy 0xFF801724 that process gets (notes/fx-slot.md "LFO"). Trusted while the FX runs, i.e. its block
 * count moved since this screen's frame before (~23 blocks per 30 fps frame); else (FX off, the screen just opened
 * after a gap, nothing published) the stored knobs. Knobs 4..7 (no LFO reaches them) stay the stored ones. */
static uint32_t fx_frame RAM = 0;
static uint32_t fx_seen_n[4][2] RAM = { { 0 } }, fx_seen_f[4][2] RAM = { { 0 } };
static const int16_t *fx_live(int kind, int stereo, const int16_t *stored, int16_t *buf)
{
    struct fx_pub *const pubs[4] = { ensemble_pub, pingpong_pub, distortion_pub, crush_pub };
    const struct fx_pub *u = kind >= 1 && kind <= 4 ? pubs[kind - 1] : 0;
    if (!u)
        return stored;
    u += stereo;
    uint32_t n = u->blocks, f = ++fx_frame;
    int run = n != fx_seen_n[kind - 1][stereo] && fx_seen_f[kind - 1][stereo] == f - 1;
    fx_seen_n[kind - 1][stereo] = n;
    fx_seen_f[kind - 1][stereo] = f;
    if (!run)
        return stored;
    for (int i = 0; i < 8; i++)
        buf[i] = i < 4 ? u->k[i] : stored[i];
    return buf;
}

/* FX screen: id = our FX id (8..), mode = edit mode (0 synth, 1 drum, 3 master), slot = sound slot. */
void fx_ui_draw(int id, int mode, int slot)
{
    void *r = te_raster();
    const int16_t *stored = FX_KNOBS(mode, slot);
    int kind = id >= 8 && id < 8 + fx_count ? fx_kind[id - 8] : 0;
    int16_t live[8];
    const int16_t *k = fx_live(kind, mode == 3, stored, live);  /* drawing; the crush readout fires on stored turns */
    if (kind == FX_ENSEMBLE) {
        if (choir_ready())
            draw_choir(r, k, mode == 3);
        else
            draw_ensemble(r, k, mode == 3);
    }
    else if (kind == FX_PINGPONG) {
        if (tennis_ready())
            draw_tennis(r, k, mode == 3);
        else
            draw_pingpong(r, k, mode == 3);
    }
    else if (kind == FX_DISTORTION) {
        if (pedals_ready())
            draw_pedals(r, k);
        else
            draw_distortion(r, k);
    }
    else if (kind == FX_CRUSH) {
        if (draw_card(r))
            (void)0;
        else if (pyramid_ready())
            draw_pyramid(r, stored, k != stored, mode == 3, mode << 4 | slot);
        else
            draw_crush(r, k, mode == 3);
    }
}

/* ---- harm and fm4 (notes/harm-fm4.md): screens in ui_harm.h / ui_fm4.h; their edit state lives in knobhook.c (the
 * encoder hook writes it, the screens read it). Weak: builds without the hook still link (a local state then).
 * Here, after the SVG helpers: fm4 draws through the SVG route when content/display/fm4.svg exists. ---- */
#define UI_HAS_SVG                          /* ui_fm4.h draws its gears from design/synth/fm4.svg (SVG helpers above) */
#include "ui_harm.h"
#include "ui_fm4.h"
#include "ui_fmdrum.h"

/* From _drum_ui_update (slot.S) once per frame while the dbox screen shows a drum slot holding one of our drum
 * engines (id). */
void drum_ui_draw(int slot, int id)
{
    int kind = id >= drum_first && id < drum_first + drum_count ? drum_kind[id - drum_first] : 0;
    if (kind == DRUM_FM)
        draw_fmdrum(slot);
    else if (kind == DRUM_PROBE)
        draw_drumprobe(slot, id);
}
extern struct harm_ui harm_ui __attribute__((weak));
extern struct fm4_ui fm4_ui __attribute__((weak));
static struct harm_ui harm_ui_local RAM = { .last = -1 };
static struct fm4_ui fm4_ui_local RAM = { .last = -1, .alg_shown = -1 };

void ui_draw(int engine, int slot)
{
    uint32_t t0 = cycles();
    void *r = te_raster();
    /* Live (LFO-modulated) knobs: the render trampoline copies each call's params; use them while voices
     * render (the count moved since the last frame), else the stored knobs. */
    uint32_t renders = PROF[5];
    int16_t kl[4];
    const int16_t *k = KNOBS(slot);
    int live = renders != last_renders;
    if (live) {
        const int16_t *p = (const int16_t *)&PROF[3];
        for (int i = 0; i < 4; i++)
            kl[i] = p[i];
        k = kl;
    }
    last_renders = renders;
    int kind = engine >= 15 && engine < 15 + 16 ? ui_kind[engine - 15] : 0;
    if (kind == KIND_WAVETABLE)
        draw_wt(r, k);
    else if (kind == KIND_GRAIN)
        draw_grain(r, k);
    else if (kind == KIND_VA)
        draw_va(r, k);
    else if (kind == KIND_INPROBE)
        draw_inprobe(r, k);
    else if (kind == KIND_SPROBE)
        draw_sprobe(r, slot);
    else if (kind == KIND_HARM)             /* the stored knobs, all 8 (packed; never the live copy) */
        draw_harm(r, KNOBS(slot), &harm_ui ? &harm_ui : &harm_ui_local);
    else if (kind == KIND_FM4)
        draw_fm4(r, KNOBS(slot), &fm4_ui ? &fm4_ui : &fm4_ui_local);
    else if (kind == KIND_AMP)
        draw_amp(r, k);
    if (hold_on)
        hold_badge(r);
    if (chord_on || chord_learning)
        chord_badge(r);
#ifdef UI_DEBUG
    diagnostics(r, t0, k, live);
#else
    (void)live;
    (void)t0;
#endif
#ifdef UI_DEBUG
    ui_cost = cycles() - t0;
#endif
}
