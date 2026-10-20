/* Harmonic engine screen and table publisher (notes/harmonic-synth.md "Screen"), for inclusion into src/slot/ui.c after
 * its drawing primitives (line_fp, line_fast, vline_fast, text, text_fast, sin_t, the colour enum, RAM, cycles()).
 * src/slot/test/screens_preview.py renders it on the host with stand-ins for those.
 *
 * harm_tick(k, u): UI thread, every frame on this screen. Publishes unison and warp from the stored knobs, and when the
 *   drawing changed, builds the table (harm_build.h, ~1.7 M cycles = ~4.3 ms) into the frame the engine is neither
 *   playing nor fading from, then publishes it (harm.h). Rate-limited: at most one build every HARM_BUILD_GAP screen
 *   frames (~0.13 s), so a knob held turning costs ~3 % of the UI thread, not 4.3 ms every frame; the edits in between
 *   coalesce into the next build, and the last published table keeps playing meanwhile. The first build (nothing
 *   published yet) is immediate. Before the first tick the engine plays a sine.
 * draw_harm(r, k, u): k = the stored knobs (all 8; not the live ones: TE's LFO would scramble them), u = the edit
 *   state (harm_edit.h).
 *   bottom: 24 bars, height = level (4 px per 3 dB step); off bars are a dark-blue stub; ticks under them (every
 *     bar dark blue, the even ones longer, every 4th grey).
 *   blue: the cursor's column (bracket and underline) and its number (H6) top left.
 *   green: the cursor bar's fill and its level (-9 DB, OFF) next to the number.
 *   white: bars PAINT wrote in the last ~0.5 s, fading to grey.
 *   top right: one cycle of the published table (level 3, through warp as the engine plays it) in blue, unison's
 *     detuned ghost in orange behind it.
 *   SHIFT: TILT as a blue curve of the effective levels over the bars, EXTEND as a green ramp of faded bars past 24,
 *     STAMP's name in white; the value of a SHIFT knob or unison replaces the readout for ~1.5 s after a turn.
 */
#ifndef UI_HARM_H
#define UI_HARM_H
#include "harm_build.h"
#include "harm_edit.h"
#include "warp.h"
#include "ui_hookdiag.h"

#define HU_X0 10                            /* bar 0 left edge */
#define HU_PITCH 12
#define HU_W 7                              /* bar width */
#define HU_BASE 128                         /* bar bottom */
#define HU_STEP 4                           /* px per level */
#define HU_WX 184                           /* waveform panel: x, width, centre y, amplitude */
#define HU_WW 128
#define HU_WY 34
#define HU_WA 22
#define HU_SHOW 45                          /* frames a value readout stays (~1.5 s) */
#define HARM_BUILD_GAP 4                    /* screen frames between table builds at least */
#ifndef HARM_RAM
#define HARM_RAM (&harm_ram)
#endif
#ifndef HARM_WORK
#define HARM_WORK(h) ((struct wti_work *)((h) + 1))     /* the builder's scratch follows harm_ram (engines.json "ram") */
#endif

static void harm_tick(const int16_t *k, struct harm_ui *u)
{
    struct harm_ram *h = HARM_RAM;
    if (!h)
        return;
    h->uni = harm_uni(k) * HARM_UNI_SCALE;
    h->warp = harm_warp(k) * HARM_UNI_SCALE;
    uint32_t sig = harm_sig(k);
    if (sig == h->sig && h->front)
        return;
    if (h->front && u->frame - u->build_frame < HARM_BUILD_GAP)
        return;                             /* coalesce: the next build picks up every edit made meanwhile */
    u->build_frame = u->frame;
    int target = 1;
    while (target == h->front || target == h->prev)
        target++;
    uint32_t t0 = cycles();
    harm_build(k, h->frame[target - 1], HARM_WORK(h));
    h->build_cycles = cycles() - t0;
    h->builds++;
    h->prev = h->front;
    __asm__ volatile("" ::: "memory");      /* the frame is complete before the engine can see it */
    h->front = target;
    h->sig = sig;
}

/* A filled bar from x0 to x1 (SVG units), y0 (top) to y1, as 1-px vertical runs (cheap on device). */
static void hu_bar(void *r, fp32 x0, fp32 x1, fp32 y0, fp32 y1, int colour)
{
    for (fp32 x = x0; x <= x1; x += 862)    /* 1 / 1.1875 SVG units: one panel row */
        vline_fast(r, x, y0, y1, colour);
}

/* signed decimal (|v| < 1000) into p, '+' first if plus and v > 0; returns the end */
static char *hu_num(char *p, int32_t v, int plus)
{
    if (v < 0) {
        *p++ = '-';
        v = -v;
    } else if (plus && v) {
        *p++ = '+';
    }
    int hd = 0, td = 0;
    while (v >= 100) {
        v -= 100;
        hd++;
    }
    while (v >= 10) {
        v -= 10;
        td++;
    }
    if (hd)
        *p++ = (char)('0' + hd);
    if (hd || td)
        *p++ = (char)('0' + td);
    *p++ = (char)('0' + v);
    *p = 0;
    return p;
}

static char *hu_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

/* One cycle of frame f (level 3, 256 samples) at phase ph, through warp */
static int32_t hu_wave(const int16_t *f, const struct warp *wp, uint32_t ph)
{
    if (wp->depth)
        ph = warp_phase(wp, ph);
    return f ? f[level_off[3] + (ph >> 24)] : (sin_t(ph) * 19661) >> 15;     /* sine before the first build */
}

static void draw_harm(void *r, const int16_t *k, struct harm_ui *u)
{
    u->frame++;
    harm_tick(k, u);
    int cur = harm_clamp(u->cursor, 0, HARM_N - 1);
    int tilt = harm_tilt(k), ext = harm_extend(k);

    /* ---- top right: one cycle of what plays ---- */
    const struct harm_ram *h = HARM_RAM;
    const int16_t *f = h && h->front >= 1 && h->front <= HARM_FRAMES ? h->frame[h->front - 1] : 0;
    struct warp wp = warp_setup(harm_warp(k) * HARM_UNI_SCALE);
    int uni = harm_uni(k);
    line_fast(r, FP(HU_WX), FP(HU_WY), FP(HU_WX + HU_WW), FP(HU_WY), DARKBLUE);
    for (int pass = uni ? 0 : 1; pass < 2; pass++) {
        /* orange: unison's second oscillator, shifted along by its detune (as wt's ghost) */
        uint32_t shift = pass ? 0 : (uint32_t)(0x06000000u + (uint32_t)uni * 0x00300000u);
        fp32 px = 0, py = 0;
        for (int i = 0; i <= 32; i++) {
            uint32_t ph = i == 32 ? 0xFFFFFFFFu : (uint32_t)i << 27;
            fp32 qx = FP(HU_WX) + i * (FP(HU_WW) >> 5);
            fp32 qy = FP(HU_WY) - ((hu_wave(f, &wp, ph + shift) * (FP(HU_WA) >> 5)) >> 10);
            if (i) {
                if (pass)
                    line_fp(r, px, py, qx, qy, FP(2), BLUE);
                else
                    line_fast(r, px, py, qx, qy, uni < 16 ? OFFRED : ORANGE);
            }
            px = qx;
            py = qy;
        }
    }

    /* ---- readout, top left: cursor harmonic and level, or the value of the knob just turned ---- */
    char buf[24], *p = buf;
    int recent = u->last >= 3 && u->frame - u->last_frame < HU_SHOW;
    if (recent && u->last == 6) {
        text(r, harm_stamp_name[u->stamp], FP(8), FP(8), FP(2), WHITE);
    } else if (recent) {
        int colour = u->last == 3 || u->last == 7 ? ORANGE : u->last == 4 ? BLUE : GREEN;
        if (u->last == 3) {
            p = hu_num(hu_str(p, "UNISON "), uni, 0);
        } else if (u->last == 7) {
            p = hu_num(hu_str(p, "WARP "), harm_warp(k), 0);
        } else if (u->last == 4) {
            /* 0.75 dB per octave per step, in quarter dB */
            int32_t q = tilt < 0 ? -3 * tilt : 3 * tilt;
            p = hu_str(p, tilt < 0 ? "TILT -" : tilt ? "TILT +" : "TILT ");
            p = hu_num(p, q >> 2, 0);
            if (q & 3)
                p = hu_str(p, (q & 3) == 1 ? ".25" : (q & 3) == 2 ? ".5" : ".75");
            hu_str(p, " DB/OCT");
        } else {
            p = ext ? hu_num(hu_str(p, "EXTEND "), ext, 0) : hu_str(p, "EXTEND OFF");
        }
        text(r, buf, FP(8), FP(8), FP(3) >> 1, colour);
    } else {
        p = hu_num(hu_str(p, "H"), cur + 1, 0);
        text(r, buf, FP(8), FP(8), FP(2), BLUE);
        fp32 dx = FP(8 + 12 * (p - buf) + 8);
        int v = harm_level(k, cur);
        p = buf;
        if (v)
            hu_str(hu_num(p, (v - HARM_MAXLEVEL) * 3, 0), " DB");
        else
            hu_str(p, "OFF");
        text(r, buf, dx, FP(11), FP(3) >> 1, GREEN);
    }

    /* ---- bars ---- */
    const fp32 base = FP(HU_BASE);
    for (int i = 0; i < HARM_N; i++) {
        fp32 x0 = FP(HU_X0 + HU_PITCH * i), x1 = x0 + FP(HU_W);
        int v = harm_level(k, i);
        int colour = GREY;
        uint32_t age = u->painted[i] ? u->frame + 1 - u->painted[i] : 1000;
        if (age < 6)
            colour = WHITE;
        else if (age < 11)
            colour = 17;                    /* lightpurplegrey */
        else if (age < 16)
            colour = PURPLEGREY;
        if (i == cur)
            colour = GREEN;
        if (v)
            hu_bar(r, x0, x1, base - FP(HU_STEP * v), base, colour);
        else
            line_fast(r, x0, base - FP(1), x1, base - FP(1), i == cur ? GREEN : DARKBLUE);
        /* ticks: every bar dark blue, even ones longer, every 4th grey */
        int t = (i & 1) ? ((i & 3) == 3 ? 5 : 3) : 2;
        vline_fast(r, x0 + FP(HU_W) / 2, base + FP(3), base + FP(3 + t), (i & 3) == 3 ? GREY : DARKBLUE);
    }
    /* blue cursor: a bracket over the column and a thick underline */
    {
        fp32 x0 = FP(HU_X0 + HU_PITCH * cur) - FP(2), x1 = x0 + FP(HU_W + 4);
        fp32 top = base - FP(HU_STEP * HARM_MAXLEVEL) - FP(4);
        line_fp(r, x0, top, x1, top, FP(1), BLUE);
        line_fp(r, x0, top, x0, top + FP(4), FP(1), BLUE);
        line_fp(r, x1, top, x1, top + FP(4), FP(1), BLUE);
        line_fp(r, x0, base + FP(2), x1, base + FP(2), FP(2), BLUE);
    }

    /* SHIFT blue: TILT as a cap on every drawn bar at its effective level (level + tilt * 0.75 dB * log2 n) */
    if (tilt) {
        for (int i = 0; i < HARM_N; i++) {
            int v = harm_level(k, i);
            if (!v)
                continue;
            /* 0.75 dB = a quarter of a level step: v + tilt * log2(n) / 4, Q16 */
            int32_t lvq = (v << 16) + ((harm_log2((uint32_t)i + 1) * tilt) >> 2);
            if (lvq < 0)
                lvq = 0;
            if (lvq > (HARM_MAXLEVEL + 3) << 16)
                lvq = (HARM_MAXLEVEL + 3) << 16;
            fp32 x0 = FP(HU_X0 + HU_PITCH * i) - FP(1), y = base - ((lvq >> 6) * HU_STEP);
            line_fast(r, x0, y, x0 + FP(HU_W + 2), y, BLUE);
            line_fast(r, x0, y - 862, x0 + FP(HU_W + 2), y - 862, BLUE);
        }
    }

    /* SHIFT green: EXTEND as faded bars past 24 (the mean of bars 13..24, one repeat per bar, 3 px each) */
    if (ext) {
        int32_t sum = 0;
        for (int i = 12; i < HARM_N; i++)
            sum += harm_level(k, i);
        int32_t lvl = (sum * 683) >> 13;    /* / 12 */
        for (int j = 0; j < 5; j++) {
            lvl -= (16 - ext) >> 1;         /* (16 - ext) * 1.5 dB per repeat = (16 - ext) / 2 levels */
            if (lvl <= 0)
                break;
            fp32 x = FP(HU_X0 + HU_PITCH * HARM_N + 1 + 4 * j);
            hu_bar(r, x, x + FP(2), base - FP(HU_STEP * lvl), base, j < 2 ? GREEN : DARKGREEN);
        }
    }
    hook_delta_diag(r, FP(8), FP(32), 0);   /* TE's raw encoder delta, under the readout (--debug-ui only) */
}

#endif
