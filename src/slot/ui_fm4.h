/* fm4 screen (notes/fm-operator.md "Screen"; gear design 2026-10-14), for inclusion into src/slot/ui.c after its drawing
 * primitives (line_fp, line_fast, vline_fast, text, text_fast, sin_t, cos_t, udiv, the colour enum, RAM, voice_ram).
 * src/slot/test/screens_preview.py renders it on the host with stand-ins.
 *
 * draw_fm4(r, k, u): k = the sound's 8 stored knobs, u = the edit state (fm4_edit.h).
 *   Each operator is a gear; the algorithm is the gear train. Carriers sit on the bottom row and drive the output
 *   shaft (grey) to the output cycle panel on the right; each modulator meshes with the gear it modulates (one that
 *   modulates several meshes with the nearest and drives the others through a blue belt).
 *   size: the ratio (log scale, 0.5 small .. 48 large); speed: the ratio too (capped at 4x so the teeth don't strobe),
 *     times the operator's envelope, read from the voices that rendered since the last frame (fm4.c blocks); with no
 *     note sounding the gears stand still. Meshing gears turn opposite ways.
 *   blue (INDEX) x a modulator's level = tooth bite: barely touching .. meshed deep, the modulator gear brighter blue,
 *     and the driven gear jitters as it turns (its angle + bite x sin of the modulator's angle: phase modulation).
 *   carriers: level = brightness (dark .. white, a double rim at the top levels).
 *   green: ALGORITHM rearranges the train; ALG n shows for ~1.5 s after a change, top right.
 *   white: TIME (the envelopes run faster or slower); TIME xN shows for ~1.5 s after a change.
 *   orange: FEEDBACK: a spring-loaded pawl on D's rim, bigger with feedback, chattering while a note sounds.
 *   operator pages (hook): the selected gear in white with a second rim and knob-coloured marks under it (blue ratio,
 *     green fine, white level, orange envelope), its values top right (C 4.41 / LVL 11 SWELL).
 *   v0 (plain knobs): DETUNE n C in orange for ~1.5 s after a change; ratio labels of B, C, D in their SHIFT knob's
 *     colour.
 */
#ifndef UI_FM4_H
#define UI_FM4_H
#include "fm4.h"
#include "fm4_edit.h"
#include "modal_state.h"
#include "ui_hookdiag.h"

#define FU_COL 26                           /* half-column pitch (layout) */
#define FU_ROW 20                           /* layout row pitch (direction only: flat enough that siblings clear) */
#define FU_CARY 112                         /* carrier gear centres */
#define FU_TOP 6                            /* the train is scaled down to stay below this */
#define FU_RAIL 134                         /* output shaft */
#define FU_GX 4                             /* gear area x 4 .. 206 */
#define FU_GW 202
#define FU_CX 212                           /* output cycle: x, width, centre y, amplitude */
#define FU_CW 100
#define FU_CY 84
#define FU_CA 40
#define FU_SHOW 45
#define FU_TOOTH 4                          /* tooth height, units */
#ifndef FM4_VOICES
#define FM4_VOICES (voice_ram)              /* engines' per-voice RAM: fm4.c's struct fm4_voice per TE voice 0..5 */
#endif

/* (half column, row) of A, B, C, D per algorithm: the direction a modulator sits from what it drives */
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

static int16_t fu_prev[8] RAM = { 0 };      /* knobs at the last frame (what changed) */
static int32_t fu_detune_frame RAM = -1000;
static int32_t fu_time_frame RAM = -1000;
static uint32_t fu_ang[FM4_OPS] RAM = { 0 };     /* gear angles, 2^32 = one turn */
static uint32_t fu_blocks[6] RAM = { 0 };   /* fm4_voice.blocks of TE voices 0..5 at the last frame */

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

/* integer square root (n < 2^20) */
static uint32_t fu_isqrt(uint32_t n)
{
    uint32_t s = 0;
    for (uint32_t b = 1u << 10; b; b >>= 1)
        if ((s + b) * (s + b) <= n)
            s += b;
    return s;
}

/* gear radius (units, Q10) from the ratio (Q16): 10 at 0.5 .. 22 at 48, log scale */
static fp32 fu_radius(uint32_t ratio)
{
    uint32_t v = ratio >> 11, l = 0;        /* ratio x 32: 16 at 0.5, 1536 at 48 */
    while (v >= 32) {                       /* log2 in quarter octaves */
        v >>= 1;
        l += 4;
    }
    l += v >= 27 ? 3 : v >= 23 ? 2 : v >= 19 ? 1 : 0;      /* 0 at ratio <= 0.5, 4 at 1 .. 26 at 48 */
    return FP(10) + (fp32)(l * 473);        /* 12 units over 26 quarter octaves */
}

/* A gear: n teeth around centre (x, y), outer radius ro, turned by a */
static void fu_gear(void *r, fp32 x, fp32 y, fp32 ro, uint32_t a, int n, int colour)
{
    fp32 ri = ro - FP(FU_TOOTH);
    uint32_t q = udiv(0x40000000u, (uint32_t)n);      /* a quarter tooth */
    fp32 px = 0, py = 0, fx = 0, fy = 0;
    for (int i = 0; i < 4 * n; i++, a += q) {
        fp32 rr = (i & 3) < 2 ? ro : ri;
        fp32 qx = x + (((rr >> 5) * cos_t(a)) >> 10), qy = y - (((rr >> 5) * sin_t(a)) >> 10);
        if (i)
            line_fast(r, px, py, qx, qy, colour);
        else {
            fx = qx;
            fy = qy;
        }
        px = qx;
        py = qy;
    }
    line_fast(r, px, py, fx, fy, colour);
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
    if (k[2] != fu_prev[2] && u->frame > 1)
        fu_time_frame = (int32_t)u->frame;
    for (int i = 0; i < 8; i++)
        fu_prev[i] = k[i];
    unsigned car = fm4_car[p.alg];

    /* envelopes of the voices that rendered since the last frame (Q15, 0 = still) */
    int32_t env[FM4_OPS] = { 0, 0, 0, 0 };
    int playing = 0;
    for (int v = 0; v < 6; v++) {
        const struct fm4_voice *fv = (const struct fm4_voice *)(FM4_VOICES + v * VOICE_RAM_BYTES);
        if (fv->owner != OWNER_FM4)
            continue;
        uint32_t b = fv->blocks;
        if (b == fu_blocks[v])
            continue;
        fu_blocks[v] = b;
        playing = 1;
        for (int op = 0; op < FM4_OPS; op++) {
            int32_t e = fv->env[op] >> 15;
            env[op] = e > env[op] ? (e > 32767 ? 32767 : e) : env[op];
        }
    }

    /* bite per modulator: level x INDEX (Q15); radii; positions: carriers on a row, modulators from their target.
     * If the train would reach above FU_TOP, every radius is scaled down to fit and the positions redone. */
    int32_t ix = p.index * 2 > 32767 ? 32767 : p.index * 2;      /* INDEX 1x (mid knob 0.5x .. ) = full bite */
    int32_t eff[FM4_OPS];
    fp32 rad[FM4_OPS], cx[FM4_OPS], cy[FM4_OPS];
    int depth[FM4_OPS], prim[FM4_OPS], side[FM4_OPS];
    fp32 left = FP(FU_GX) + ((FP(FU_GW) - FP(FU_COL) * fu_width[p.alg]) >> 1);
    for (int op = 0; op < FM4_OPS; op++) {
        eff[op] = (fm4_level[p.level[op]] * ix) >> 15;
        rad[op] = fu_radius(p.ratio[op]);
    }
    for (int pass = 0; pass < 2; pass++) {
        fp32 top = FP(400);
        for (int op = 0; op < FM4_OPS; op++) {
            prim[op] = -1;
            depth[op] = 0;
            side[op] = fu_layout[p.alg][op][0] * 2 < fu_width[p.alg] ? -1 : 1;   /* label: outside of the train */
            if (car >> op & 1) {
                cx[op] = left + FP(FU_COL) * fu_layout[p.alg][op][0];
                cy[op] = FP(FU_CARY);
                continue;
            }
            /* the target nearest in the layout (lower index first) */
            int best = -1, bd = 99;
            for (int t = 0; t < op; t++)
                if (fm4_mods[p.alg][t] >> op & 1) {
                    int d = fu_layout[p.alg][op][0] - fu_layout[p.alg][t][0];
                    d = d < 0 ? -d : d;
                    if (d < bd) {
                        bd = d;
                        best = t;
                    }
                }
            if (best < 0) {                 /* not in any algorithm; stay safe */
                cx[op] = left;
                cy[op] = FP(FU_CARY);
                continue;
            }
            prim[op] = best;
            depth[op] = depth[best] + 1;
            int32_t dx = (fu_layout[p.alg][op][0] - fu_layout[p.alg][best][0]) * FU_COL;
            int32_t dy = (fu_layout[p.alg][best][1] - fu_layout[p.alg][op][1]) * FU_ROW;   /* up = negative y */
            side[op] = dx < 0 ? -1 : dx > 0 ? 1 : 0;           /* straight above: label on top */
            uint32_t len = fu_isqrt((uint32_t)(dx * dx + dy * dy));
            fp32 dist = rad[best] + rad[op] - ((FP(FU_TOOTH) * eff[op]) >> 15);
            uint32_t ox = udiv((uint32_t)(dx < 0 ? -dx : dx) * (uint32_t)dist, len);
            uint32_t oy = udiv((uint32_t)(dy < 0 ? -dy : dy) * (uint32_t)dist, len);
            cx[op] = cx[best] + (dx < 0 ? -(fp32)ox : (fp32)ox);
            cy[op] = cy[best] + (dy < 0 ? -(fp32)oy : (fp32)oy);
        }
        for (int op = 0; op < FM4_OPS; op++) {
            top = cy[op] - rad[op] < top ? cy[op] - rad[op] : top;
            if (side[op] || prim[op] < 0)
                continue;
            /* straight above its target: the label goes where no sibling is (outside the train; on top if both) */
            int l = 0, rt = 0;
            for (int j = 0; j < FM4_OPS; j++)
                if (j != op && prim[j] == prim[op])
                    l |= side[j] < 0, rt |= side[j] > 0;
            side[op] = l && rt ? 0 : l ? 1 : rt ? -1 : fu_layout[p.alg][op][0] * 2 < fu_width[p.alg] ? -1 : 1;
        }
        if (pass || top >= FP(FU_TOP))
            break;
        /* scale the radii by (room above the carrier row) / (what the train needs) */
        uint32_t need = (uint32_t)(FP(FU_CARY) - top) >> 6, room = (uint32_t)(FP(FU_CARY - FU_TOP)) >> 6;
        for (int op = 0; op < FM4_OPS; op++)
            rad[op] = (fp32)udiv((uint32_t)rad[op] * room, need);
    }

    /* turn: speed ~ ratio (capped at 4x) x envelope; meshing gears turn opposite ways */
    uint32_t jit[FM4_OPS] = { 0, 0, 0, 0 };
    for (int op = 0; op < FM4_OPS; op++) {
        uint32_t rq = p.ratio[op] > (4u << 16) ? 4u << 16 : p.ratio[op];
        uint32_t sp = rq * (uint32_t)(env[op] >> 5);              /* 1/64 turn per frame at 1:1, full */
        fu_ang[op] += depth[op] & 1 ? -sp : sp;
    }
    for (int op = 1; op < FM4_OPS; op++)    /* the driven gears jitter: bite x sin(modulator angle) */
        for (int t = 0; t < op; t++)
            if (fm4_mods[p.alg][t] >> op & 1) {
                int32_t a = (eff[op] * env[op]) >> 15;
                jit[t] += (uint32_t)(((a * sin_t(fu_ang[op])) >> 15) << 12);    /* up to 1/32 turn */
            }

    /* output shaft: carriers down to the rail, the rail to the cycle panel */
    fp32 rail_l = FP(400);
    for (int op = 0; op < FM4_OPS; op++)
        if (car >> op & 1) {
            line_fast(r, cx[op], cy[op] + rad[op], cx[op], FP(FU_RAIL), GREY);
            rail_l = cx[op] < rail_l ? cx[op] : rail_l;
        }
    line_fast(r, rail_l, FP(FU_RAIL), FP(FU_CX - 4), FP(FU_RAIL), GREY);
    line_fast(r, FP(FU_CX - 4), FP(FU_RAIL), FP(FU_CX - 4), FP(FU_CY), GREY);

    /* blue belts: a modulator's other targets */
    int mc = p.index < 2048 ? DARKBLUE : p.index < 12000 ? LIGHTBLUE : BLUE;
    for (int op = 1; op < FM4_OPS; op++)
        for (int t = 0; t < op; t++)
            if (fm4_mods[p.alg][t] >> op & 1 && t != prim[op])
                line_fp(r, cx[op], cy[op], cx[t], cy[t], FP(1) + ((eff[op] * 3) >> 5), p.level[op] ? mc : DARKBLUE);

    /* orange: feedback pawl on D's upper right rim, chattering while a note sounds */
    if (p.fb > 0) {
        int col = p.fb < 4096 ? OFFRED : ORANGE;
        fp32 len = FP(4) + ((p.fb * 9) >> 5);                     /* 4 .. 13 units */
        fp32 rimx = cx[3] + ((rad[3] * 23170) >> 15), rimy = cy[3] - ((rad[3] * 23170) >> 15);   /* 45 degrees */
        fp32 ax = rimx + len, ay = rimy - len;
        fp32 chat = playing && (u->frame & 1) ? (p.fb * 3) >> 5 : 0;              /* 0 .. 3 units */
        line_fp(r, ax, ay, rimx + chat, rimy - chat, FP(3) >> 1, col);          /* the pawl */
        fp32 sx = ax, sy = ay;                                                   /* its spring, zigzag up */
        for (int i = 1; i <= 4; i++) {
            fp32 nx = ax + (i & 1 ? FP(3) : -FP(1)), ny = ay - FP(2) * i;
            line_fast(r, sx, sy, nx, ny, col);
            sx = nx;
            sy = ny;
        }
    }

    /* gears */
    static const char *const letter[FM4_OPS] = { "A", "B", "C", "D" };
    for (int op = 0; op < FM4_OPS; op++) {
        int sel = page == op + 1, lv = p.level[op], colour;
        if (car >> op & 1)
            colour = !lv ? DARKBLUE : lv < 6 ? PURPLEGREY : lv < 11 ? 17 : WHITE;
        else
            colour = !lv ? DARKBLUE : eff[op] < 2048 ? DARKBLUE : eff[op] < 12000 ? LIGHTBLUE : BLUE;
        if (sel)
            colour = WHITE;
        int teeth = 3 + (int)(rad[op] >> 11);                     /* 8 .. 14: a similar pitch on every gear */
        uint32_t a = fu_ang[op] + jit[op];
        fu_gear(r, cx[op], cy[op], rad[op], a, teeth, colour);
        if (sel || ((car >> op & 1) && lv >= 11))
            fu_gear(r, cx[op], cy[op], rad[op] - 862, a, teeth, colour);       /* a second rim */
        /* a spoke, which shows the turning */
        fp32 hr = rad[op] - FP(FU_TOOTH + 2);
        line_fast(r, cx[op], cy[op], cx[op] + (((hr >> 5) * cos_t(a)) >> 10), cy[op] - (((hr >> 5) * sin_t(a)) >> 10),
                  colour);
        /* label: letter and ratio, beside the gear on the outside of the train */
        char buf[12];
        buf[0] = letter[op][0];
        fp32 tw = FP(6) * (fp32)(fm4_ratio_text(buf + 1, p.ratio[op]) - buf);
        int lc = sel ? WHITE : GREY;
        if (!p.packed && op)
            lc = op == 1 ? BLUE : op == 2 ? GREEN : WHITE;        /* v0: the SHIFT knob that sets it */
        fp32 lx, ly;
        if (car >> op & 1) {                /* carriers: below left, where no modulator sits */
            lx = cx[op] - ((rad[op] * 3) >> 2) - tw;
            ly = cy[op] + ((rad[op] * 3) >> 2) - FP(4);
        } else {
            lx = side[op] < 0 ? cx[op] - rad[op] - FP(2) - tw : side[op] > 0 ? cx[op] + rad[op] + FP(3) : cx[op] - (tw >> 1);
            ly = side[op] ? cy[op] - FP(4) : cy[op] - rad[op] - FP(10);
        }
        text_fast(r, buf, lx, ly, FP(1), lc);
        if (sel) {                          /* knob-coloured marks: ratio, fine, level, envelope */
            fp32 y = cy[op] + rad[op] + FP(3), x = cx[op] - FP(10);
            line_fp(r, x, y, x + FP(4), y, FP(2), BLUE);
            line_fp(r, x + FP(5), y, x + FP(9), y, FP(2), GREEN);
            line_fp(r, x + FP(10), y, x + FP(14), y, FP(2), WHITE);
            line_fp(r, x + FP(15), y, x + FP(19), y, FP(2), ORANGE);
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

    /* top right: the operator page's values, the algorithm number, TIME, or a v0 detune */
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
    } else if ((int32_t)u->frame - fu_time_frame < FU_SHOW) {
        /* TIME: x0.25 .. x4 in half octaves */
        static const char *const tname[9] = { "0.25", "0.35", "0.5", "0.71", "1", "1.41", "2", "2.83", "4" };
        int32_t t = (fm4_knob(k[2]) * 8 + 16384) >> 15;
        q = fu_str(q, "TIME X");
        fu_str(q, tname[t > 8 ? 8 : t]);
        text(r, buf, FP(FU_CX), FP(8), FP(3) >> 1, WHITE);
    } else if (!p.packed && (int32_t)u->frame - fu_detune_frame < FU_SHOW) {
        /* detune: up to 1 % = 17 cents on B, C (down), D (half) */
        q = fu_str(q, "DETUNE ");
        q = fu_dec(q, (uint32_t)((p.detune * 17 + 16384) >> 15));
        fu_str(q, " C");
        text(r, buf, FP(FU_CX), FP(8), FP(3) >> 1, ORANGE);
    }
    hook_delta_diag(r, FP(316), FP(131), 1);   /* TE's raw encoder delta, bottom right */
}

#endif
