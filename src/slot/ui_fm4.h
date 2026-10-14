/* fm4 screen (notes/fm-operator.md "Screen"), for inclusion into src/slot/ui.c after its drawing primitives (line_fp,
 * line_fast, vline_fast, text, text_fast, sin_t, the colour enum, RAM). src/slot/test/screens_preview.py renders it on
 * the host with stand-ins.
 *
 * draw_fm4(r, k, u): k = the sound's 8 knobs, u = the edit state (fm4_edit.h; its page is MAIN without the hook).
 *   left: the four operator boxes laid out by the algorithm, modulators above what they modulate, carriers on a rail
 *     to the output. Each box: its letter, ratio, a level bar and a small envelope glyph.
 *   blue: INDEX: the modulation lines get thicker and brighter.
 *   green: ALGORITHM: the layout; its number (ALG 4) shows for ~1.5 s after a change, top right.
 *   white: TIME: the envelope glyphs stretch (x0.25 .. x4).
 *   orange: FEEDBACK: a loop on D, growing.
 *   right: one cycle of the output at the current settings (static: envelopes at full), in blue.
 *   v0 (plain knobs): the SHIFT knobs' ratios are coloured by knob (B blue, C green, D white); DETUNE shows its value in
 *     orange for ~1.5 s after a change.
 *   operator pages (hook): the selected box is white with underlines in the knob colours (blue ratio, green fine,
 *     white level, orange envelope), and its values are spelled out on top.
 */
#ifndef UI_FM4_H
#define UI_FM4_H
#include "fm4.h"
#include "fm4_edit.h"

#define FU_BW 36                            /* box */
#define FU_BH 20
#define FU_COL 24                           /* half-column pitch */
#define FU_ROW 30
#define FU_Y0 104                           /* row 0 box top */
#define FU_RAIL 134                         /* output rail */
#define FU_GX 8                             /* graph area x 8 .. 200 */
#define FU_GW 192
#define FU_CX 212                           /* output cycle: x, width, centre y, amplitude */
#define FU_CW 100
#define FU_CY 84
#define FU_CA 40
#define FU_SHOW 45

/* (half column, row) of A, B, C, D per algorithm */
static const uint8_t fu_layout[FM4_ALGS][FM4_OPS][2] = {
    { { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 } },
    { { 1, 0 }, { 1, 1 }, { 0, 2 }, { 2, 2 } },
    { { 1, 0 }, { 0, 1 }, { 0, 2 }, { 2, 1 } },
    { { 1, 0 }, { 0, 1 }, { 2, 1 }, { 2, 2 } },
    { { 0, 0 }, { 0, 1 }, { 2, 0 }, { 2, 1 } },
    { { 2, 0 }, { 0, 1 }, { 2, 1 }, { 4, 1 } },
    { { 0, 0 }, { 2, 0 }, { 2, 1 }, { 2, 2 } },
    { { 0, 0 }, { 2, 0 }, { 1, 1 }, { 1, 2 } },
    { { 0, 0 }, { 2, 0 }, { 4, 0 }, { 2, 1 } },
    { { 0, 0 }, { 2, 0 }, { 4, 0 }, { 4, 1 } },
    { { 0, 0 }, { 2, 0 }, { 4, 0 }, { 6, 0 } },
};
static const uint8_t fu_width[FM4_ALGS] = { 0, 2, 2, 2, 2, 4, 2, 2, 4, 4, 6 };   /* widest half column */

static int16_t fu_prev[8] RAM = { 0 };      /* knobs at the last frame (v0: what changed) */
static int32_t fu_detune_frame RAM = -1000;

static char *fu_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *fu_dec(char *p, uint32_t v)
{
    uint32_t t = 0;
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

/* Output at phase step n of a 256-step cycle, steady (envelopes at full), Q15. s: D's feedback history. */
static int32_t fu_sample(const struct fm4_patch *p, uint32_t n, int32_t *s1, int32_t *s2, int32_t norm)
{
    unsigned car = fm4_car[p->alg];
    uint32_t y[FM4_OPS];
    int32_t acc = 0;
    for (int op = FM4_OPS - 1; op >= 0; op--) {
        uint32_t ph = (n << 8) * (p->ratio[op] >> 0);    /* (n / 256 cycles) x ratio (Q16) in 2^32 turns */
        uint32_t m = 0;
        for (int j = op + 1; j < FM4_OPS; j++)
            if (fm4_mods[p->alg][op] >> j & 1)
                m += y[j];
        if (op == FM4_OPS - 1)
            m += (uint32_t)((*s1 + *s2) * ((p->fb * 14603) >> 15));
        int32_t s = sin_t(ph + m);
        if (op == FM4_OPS - 1) {
            *s2 = *s1;
            *s1 = s;
        }
        int32_t lv = fm4_level[p->level[op]];
        if (car >> op & 1) {
            acc += (s * ((lv * norm) >> 15)) >> 15;
            y[op] = 0;
        } else {
            int32_t d = (lv * p->index) >> 15;
            y[op] = (uint32_t)s * (uint32_t)((d * (8 * 20861 >> 3)) >> 12);
        }
    }
    return acc;
}

static void fu_box(void *r, fp32 x, fp32 y, int colour, fp32 w)
{
    if (w) {
        line_fp(r, x, y, x + FP(FU_BW), y, w, colour);
        line_fp(r, x + FP(FU_BW), y, x + FP(FU_BW), y + FP(FU_BH), w, colour);
        line_fp(r, x + FP(FU_BW), y + FP(FU_BH), x, y + FP(FU_BH), w, colour);
        line_fp(r, x, y + FP(FU_BH), x, y, w, colour);
    } else {
        line_fast(r, x, y, x + FP(FU_BW), y, colour);
        line_fast(r, x + FP(FU_BW), y, x + FP(FU_BW), y + FP(FU_BH), colour);
        line_fast(r, x + FP(FU_BW), y + FP(FU_BH), x, y + FP(FU_BH), colour);
        line_fast(r, x, y + FP(FU_BH), x, y, colour);
    }
}

/* Envelope glyph for shape sh, 10 x 5 units at (x, y) bottom-left, stretched by TIME (log2 scale, Q16) */
static void fu_env(void *r, fp32 x, fp32 y, int sh, int32_t time_q16, int colour)
{
    fp32 w = FP(5) + (fp32)((time_q16 + (2 << 16)) >> 6) * 5 / 4;      /* 5 .. 15 units */
    fp32 hi = y - FP(5), mid = y - (FP(5) >> 1);
    fp32 a = fm4_env_start[sh] ? hi : y;
    fp32 b = fm4_env_target[sh] == 32767 ? hi : fm4_env_target[sh] ? mid : y;
    if (!sh) {
        line_fast(r, x, hi, x + w, hi, colour);
        return;
    }
    fp32 knee = sh >= 1 && sh <= 4 ? x + (w >> (5 - sh)) : sh == 5 ? x + (w >> 2) : x + (w >> 1);
    line_fast(r, x, a, knee, b, colour);
    line_fast(r, knee, b, x + w, b, colour);
}

static void draw_fm4(void *r, const int16_t *k, struct fm4_ui *u)
{
    struct fm4_patch p;
    fm4_decode(k, &p);
    u->frame++;
    int page = u->page >= 0 && u->page <= FM4_OPS ? u->page : 0;
    if (p.alg != u->alg_shown) {
        u->alg_shown = p.alg;
        u->alg_frame = u->frame;
    }
    if (!p.packed && k[7] != fu_prev[7])
        fu_detune_frame = (int32_t)u->frame;
    for (int i = 0; i < 8; i++)
        fu_prev[i] = k[i];

    /* box positions: the layout centred in the graph area */
    fp32 bx[FM4_OPS], by[FM4_OPS];
    fp32 left = FP(FU_GX) + ((FP(FU_GW) - FP(FU_COL) * fu_width[p.alg] - FP(FU_BW)) >> 1);
    for (int op = 0; op < FM4_OPS; op++) {
        bx[op] = left + FP(FU_COL) * fu_layout[p.alg][op][0];
        by[op] = FP(FU_Y0) - FP(FU_ROW) * fu_layout[p.alg][op][1];
    }
    unsigned car = fm4_car[p.alg];

    /* blue: modulation lines, thicker and brighter with INDEX */
    fp32 mw = FP(1) + ((p.index * 3) >> 5);          /* 1 .. 4 units */
    int mc = p.index < 2048 ? DARKBLUE : p.index < 12000 ? LIGHTBLUE : BLUE;
    for (int op = 0; op < FM4_OPS; op++)
        for (int j = op + 1; j < FM4_OPS; j++)
            if (fm4_mods[p.alg][op] >> j & 1)
                line_fp(r, bx[j] + FP(FU_BW / 2), by[j] + FP(FU_BH), bx[op] + FP(FU_BW / 2), by[op], mw,
                        p.level[j] ? mc : DARKBLUE);
    /* carriers to the output rail, the rail to the cycle panel */
    fp32 rail_l = FP(400);
    for (int op = 0; op < FM4_OPS; op++)
        if (car >> op & 1) {
            fp32 cx = bx[op] + FP(FU_BW / 2);
            line_fast(r, cx, by[op] + FP(FU_BH), cx, FP(FU_RAIL), GREY);
            rail_l = cx < rail_l ? cx : rail_l;
        }
    line_fast(r, rail_l, FP(FU_RAIL), FP(FU_CX - 4), FP(FU_RAIL), GREY);
    line_fast(r, FP(FU_CX - 4), FP(FU_RAIL), FP(FU_CX - 4), FP(FU_CY), GREY);

    /* orange: feedback loop on D's right edge, growing with FEEDBACK */
    if (p.fb > 0) {
        int32_t rad = FP(3) + ((p.fb * 7) >> 5);     /* 3 .. 10 units */
        fp32 cx = bx[3] + FP(FU_BW) + rad - FP(1), cy = by[3] + FP(FU_BH / 2);
        fp32 px = 0, py = 0;
        for (int i = 0; i <= 8; i++) {
            uint32_t t = 0xC0000000u + (uint32_t)i * 0x1B000000u;      /* 3/4 of a turn, from the top round */
            fp32 qx = cx + ((rad >> 5) * cos_t(t) >> 10), qy = cy - ((rad >> 5) * sin_t(t) >> 10);
            if (i)
                line_fp(r, px, py, qx, qy, FP(3) >> 1, p.fb < 4096 ? OFFRED : ORANGE);
            px = qx;
            py = qy;
        }
        line_fp(r, px, py, px - FP(3), py - FP(2), FP(3) >> 1, p.fb < 4096 ? OFFRED : ORANGE);   /* arrow head */
        line_fp(r, px, py, px - FP(3), py + FP(2), FP(3) >> 1, p.fb < 4096 ? OFFRED : ORANGE);
    }

    /* boxes */
    static const char *const letter[FM4_OPS] = { "A", "B", "C", "D" };
    for (int op = 0; op < FM4_OPS; op++) {
        fp32 x = bx[op], y = by[op];
        int sel = page == op + 1, on = p.level[op] != 0;
        fu_box(r, x, y, sel ? WHITE : on ? GREY : DARKBLUE, sel ? FP(1) : 0);
        text_fast(r, letter[op], x - FP(6), y + FP(1), FP(1), sel ? WHITE : DARKBLUE);
        char buf[8];
        int long_text = fm4_ratio_text(buf, p.ratio[op]) - buf > 2;
        int rc = on ? WHITE : GREY;
        if (!p.packed && op)
            rc = op == 1 ? BLUE : op == 2 ? GREEN : WHITE;             /* v0: the SHIFT knob that sets it */
        text(r, buf, x + FP(3), y + FP(3), long_text ? FP(1) : FP(3) >> 1, sel ? WHITE : rc);
        /* level bar: 0..15 over 22 units */
        fp32 lw = (p.level[op] * FP(22) * 4369) >> 16;                /* level / 15 x 22 */
        for (fp32 yy = y + FP(15); yy <= y + FP(17); yy += 862)
            if (p.level[op])
                line_fast(r, x + FP(3), yy, x + FP(3) + lw, yy, sel ? WHITE : car >> op & 1 ? GREY : PURPLEGREY);
        fu_env(r, x + FP(26), y + FP(17), p.env[op], p.time_q16, sel ? ORANGE : GREY);
        if (sel) {                                   /* knob-coloured underlines: ratio, fine, level, envelope */
            line_fp(r, x + FP(3), y + FP(FU_BH + 3), x + FP(13), y + FP(FU_BH + 3), FP(2), BLUE);
            line_fp(r, x + FP(14), y + FP(FU_BH + 3), x + FP(20), y + FP(FU_BH + 3), FP(2), GREEN);
            line_fp(r, x + FP(3), y + FP(13), x + FP(25), y + FP(13), FP(1), WHITE);
            line_fp(r, x + FP(26), y + FP(FU_BH + 3), x + FP(34), y + FP(FU_BH + 3), FP(2), ORANGE);
        }
    }

    /* right: one cycle of the output */
    {
        int32_t lsum = 0, s1 = 0, s2 = 0;
        for (int op = 0; op < FM4_OPS; op++)
            if (car >> op & 1)
                lsum += fm4_level[p.level[op]];
        int32_t norm = lsum > 32767 ? (int32_t)udiv(32767u << 15, (uint32_t)lsum) : 32767;
        for (uint32_t n = 0; n < 256; n++)            /* one cycle to settle the feedback */
            fu_sample(&p, n, &s1, &s2, norm);
        line_fast(r, FP(FU_CX), FP(FU_CY), FP(FU_CX + FU_CW), FP(FU_CY), DARKBLUE);
        fp32 px = 0, py = 0;
        for (uint32_t n = 0; n <= 256; n++) {
            int32_t v = fu_sample(&p, n, &s1, &s2, norm);
            if (n & 7)
                continue;
            fp32 qx = FP(FU_CX) + (fp32)(n * (FP(FU_CW) >> 8)), qy = FP(FU_CY) - (((FP(FU_CA) >> 5) * v) >> 10);
            if (n)
                line_fp(r, px, py, qx, qy, FP(3) >> 1, BLUE);
            px = qx;
            py = qy;
        }
    }

    /* top right: algorithm number, a v0 detune, or the operator page's values */
    char buf[32], *q = buf;
    if (page) {
        int op = page - 1;
        q = fu_str(q, letter[op]);
        q = fu_str(q, " ");
        q = fm4_ratio_text(q, p.ratio[op]);
        text(r, buf, FP(FU_CX), FP(8), FP(2), WHITE);
        q = fu_str(buf, "LVL ");
        q = fu_dec(q, p.level[op]);
        q = fu_str(q, " ");
        fu_str(q, fm4_env_name[p.env[op]]);
        text_fast(r, buf, FP(FU_CX), FP(26), FP(1), WHITE);
    } else if (u->frame - u->alg_frame < FU_SHOW) {
        q = fu_str(q, "ALG ");
        fu_dec(q, (uint32_t)p.alg + 1);
        text(r, buf, FP(FU_CX), FP(8), FP(2), GREEN);
    } else if (!p.packed && (int32_t)u->frame - fu_detune_frame < FU_SHOW) {
        /* detune: up to 1 % = 17 cents on B, C (down), D (half) */
        q = fu_str(q, "DETUNE ");
        q = fu_dec(q, (uint32_t)((p.detune * 17 + 16384) >> 15));
        fu_str(q, " C");
        text(r, buf, FP(FU_CX), FP(8), FP(3) >> 1, ORANGE);
    }
}

#endif
