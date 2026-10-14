/* fm4 screen (notes/fm-operator.md "Screen"; gear design 2026-10-14, SVG version), for inclusion into src/slot/ui.c
 * after its drawing primitives (line_fp, line_fast, text, text_fast, sin_t, cos_t, udiv, the colour enum, RAM,
 * voice_ram) and, with UI_HAS_SVG, its SVG helpers (svg_open, svg_render, show, set_aff, aff_mul, aff_rot, recol_*).
 * src/slot/test/screens_preview.py renders the line version on the host; src/slot/test/preview.py the SVG one.
 *
 * draw_fm4(r, k, u): k = the sound's 8 stored knobs, u = the edit state (fm4_edit.h).
 *   Each operator is a gear (design/synth/fm4.svg: teeth, rim, three spokes, hub), the algorithm is the gear train,
 *   fitted and centred on the screen below the readouts and the HOLD/CHORD badges. Carriers sit on the bottom row;
 *   each modulator meshes with the gear it modulates (one that modulates several meshes with the nearest and drives
 *   the others through a blue belt).
 *   size: the ratio (log scale, 0.5 small .. 48 large); speed: the ratio too (capped at 4x so the teeth don't strobe),
 *     times the operator's envelope, read from the voices that rendered since the last frame (fm4.c blocks); with no
 *     note sounding the gears stand still. Meshing gears turn opposite ways.
 *   blue (INDEX) x a modulator's level = tooth bite: barely touching .. meshed deep, the modulator gear dark blue ..
 *     blue, and the driven gear jitters as it turns (its angle + bite x sin of the modulator's angle: phase modulation).
 *   carriers: level = brightness (grey .. white).
 *   green: ALGORITHM rearranges the train; ALG n shows for ~1.5 s after a change, top left.
 *   white: TIME (the envelopes run faster or slower); TIME xN shows for ~1.5 s after a change.
 *   orange: FEEDBACK: a spring-loaded pawl on D's teeth, bigger with feedback, chattering while a note sounds.
 *   operator pages (hook): the selected gear white with an outer ring and knob-coloured marks under it (blue ratio,
 *     green fine, white level, orange envelope), its values top left (C 4.41 / LVL 11 SWELL).
 *   labels: letter and ratio beside each modulator (outside the train), under each carrier.
 *   v0 (plain knobs): DETUNE n C in orange for ~1.5 s after a change; ratio labels of B, C, D in their SHIFT knob's
 *     colour.
 * Without the SVG (file missing, or the host harness) the gears are drawn as 1-px polylines at the same places.
 */
#ifndef UI_FM4_H
#define UI_FM4_H
#include "fm4.h"
#include "fm4_edit.h"
#include "modal_state.h"
#include "ui_hookdiag.h"

#define FU_COL 26                           /* layout half-column pitch (base units) */
#define FU_ROW 16                           /* layout row pitch (direction only: flat enough that siblings clear) */
#define FU_X0 36                            /* the train's box on screen (labels fit beside it) */
#define FU_X1 284
#define FU_Y0 41                            /* below the readouts and the HOLD/CHORD badges */
#define FU_Y1 137                           /* the safe area's bottom (labels beside the gears) */
#define FU_KMAX 2560                        /* largest fit scale, Q10 (2.5) */
#define FU_SHOW 45
#define FU_TOOTH 4                          /* tooth height, base units */
#define FU_TIP 20                           /* tip radius of the gears in fm4.svg */
#ifndef FM4_VOICES
#define FM4_VOICES (voice_ram)              /* engines' per-voice RAM: fm4.c's struct fm4_voice per TE voice 0..5 */
#endif

/* (half column, row) of A, B, C, D per algorithm: the direction a modulator sits from what it drives */
static const uint8_t fu_layout[FM4_ALGS][FM4_OPS][2] = {
    { { 0, 0 }, { 1, 1 }, { 0, 2 }, { 1, 3 } },    /* the stack zigzags: less tall, bigger gears */
    { { 1, 0 }, { 1, 1 }, { 0, 2 }, { 2, 2 } },
    { { 1, 0 }, { 0, 1 }, { 0, 2 }, { 2, 1 } },
    { { 1, 0 }, { 0, 1 }, { 2, 1 }, { 2, 2 } },
    { { 0, 0 }, { 0, 1 }, { 2, 0 }, { 2, 1 } },
    { { 2, 0 }, { 0, 1 }, { 2, 1 }, { 4, 1 } },
    { { 0, 0 }, { 2, 0 }, { 3, 1 }, { 2, 2 } },
    { { 0, 0 }, { 2, 0 }, { 1, 1 }, { 1, 2 } },
    { { 0, 0 }, { 2, 0 }, { 4, 0 }, { 2, 1 } },
    { { 0, 0 }, { 2, 0 }, { 4, 0 }, { 4, 1 } },
    { { 0, 0 }, { 2, 0 }, { 4, 0 }, { 6, 0 } },
};
static const uint8_t fu_width[FM4_ALGS] = { 1, 2, 2, 2, 2, 4, 3, 2, 4, 4, 6 };   /* widest half column */

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

#ifdef UI_HAS_SVG
/* ---- the SVG document (design/synth/fm4.svg): found once, cached ---- */
static struct svgdoc fu_doc RAM = { 0, 0 };
static int32_t fu_found RAM = 0;
static void *fu_g[FM4_OPS] RAM = { 0 }, *fu_sel[FM4_OPS] RAM = { 0 }, *fu_belt[2] RAM = { 0 }, *fu_pawl RAM = 0;
static struct recol fu_body[FM4_OPS] RAM = { { 0, { 0 } } }, fu_hub[FM4_OPS] RAM = { { 0, { 0 } } };
static struct recol fu_beltc[2] RAM = { { 0, { 0 } } }, fu_pawlc[3] RAM = { { 0, { 0 } } };

static int fu_svg(void)
{
    if (!svg_open(&fu_doc, "/yaffs2/content/display/fm4.svg", "fm4.svg"))
        return 0;
    if (!fu_found) {
        static const char *const g[FM4_OPS] = { "ga", "gb", "gc", "gd" };
        static const char *const body[FM4_OPS] = { "gabody", "gbbody", "gcbody", "gdbody" };
        static const char *const hub[FM4_OPS] = { "gahub", "gbhub", "gchub", "gdhub" };
        static const char *const sel[FM4_OPS] = { "gasel", "gbsel", "gcsel", "gdsel" };
        static const char *const pawl[3] = { "arm", "pivot", "spring" };
        void *d = fu_doc.doc;
        for (int i = 0; i < FM4_OPS; i++) {
            fu_g[i] = te_node_find(d, g[i], 0);
            fu_sel[i] = te_node_find(d, sel[i], 0);
            recol_init(&fu_body[i], d, body[i], 1);
            recol_init(&fu_hub[i], d, hub[i], 1);
        }
        fu_belt[0] = te_node_find(d, "belt1", 0);
        fu_belt[1] = te_node_find(d, "belt2", 0);
        recol_init(&fu_beltc[0], d, "belt1", 1);
        recol_init(&fu_beltc[1], d, "belt2", 1);
        fu_pawl = te_node_find(d, "pawl", 0);
        for (int i = 0; i < 3; i++)
            recol_init(&fu_pawlc[i], d, pawl[i], 1);
        fu_found = 1;
    }
    return 1;
}
#endif

static const uint32_t fu_rgb[19] = {        /* the palette entries used here, as 0xRRGGBB (colors.svg) */
    [DARKBLUE] = 0x383572, [LIGHTBLUE] = 0xBFD8FF, [BLUE] = 0x698EFF, [GREY] = 0x7C7990, [PURPLEGREY] = 0xAEB1DC,
    [17] = 0xE6E7FF, [WHITE] = 0xFFFFFF, [ORANGE] = 0xFF423F, [OFFRED] = 0x4E2832,
};

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

    /* the train in base units (Q10): carriers on a row at y 0, modulators placed from their target, meshing at a
     * centre distance of both radii minus the bite (level x INDEX, Q15) */
    int32_t ix = p.index * 2 > 32767 ? 32767 : p.index * 2;      /* INDEX 1x (mid knob 0.5x .. ) = full bite */
    int32_t eff[FM4_OPS];
    fp32 rad[FM4_OPS], cx[FM4_OPS], cy[FM4_OPS];
    int depth[FM4_OPS], prim[FM4_OPS], side[FM4_OPS];
    for (int op = 0; op < FM4_OPS; op++) {
        eff[op] = (fm4_level[p.level[op]] * ix) >> 15;
        rad[op] = fu_radius(p.ratio[op]);
    }
    /* carriers left to right (layout columns increase with the index), packed by size: each column as wide as the
     * largest gear standing in it, 6 units apart */
    fp32 carx[FM4_OPS], prevx = 0, prevw = 0;
    int ncar = 0, prevc = 0;
    for (int op = 0; op < FM4_OPS; op++) {
        if (!(car >> op & 1))
            continue;
        int c = fu_layout[p.alg][op][0];
        fp32 w = 0, gap = FP(6);
        for (int j = 0; j < FM4_OPS; j++) {
            int cj = fu_layout[p.alg][j][0];
            if (cj == c && rad[j] > w)
                w = rad[j];
            if (ncar && cj > prevc && cj < c && rad[j] + FP(6) > gap)
                gap = rad[j] + FP(6);       /* a gear standing between the two columns (alg 8's C) */
        }
        carx[op] = ncar++ ? prevx + prevw + w + gap : 0;
        prevx = carx[op];
        prevw = w;
        prevc = c;
    }
    for (int op = 0; op < FM4_OPS; op++) {
        prim[op] = -1;
        depth[op] = 0;
        side[op] = fu_layout[p.alg][op][0] * 2 < fu_width[p.alg] ? -1 : 1;
        if (car >> op & 1) {
            cx[op] = carx[op];
            cy[op] = 0;
            continue;
        }
        int best = -1, bd = 99;             /* the target nearest in the layout (lower index first) */
        for (int t = 0; t < op; t++)
            if (fm4_mods[p.alg][t] >> op & 1) {
                int d = fu_layout[p.alg][op][0] - fu_layout[p.alg][t][0];
                d = d < 0 ? -d : d;
                if (d < bd) {
                    bd = d;
                    best = t;
                }
            }
        if (best < 0) {                     /* not in any algorithm; stay safe */
            cx[op] = cy[op] = 0;
            continue;
        }
        prim[op] = best;
        depth[op] = depth[best] + 1;
        int32_t dx = (fu_layout[p.alg][op][0] - fu_layout[p.alg][best][0]) * FU_COL;
        int32_t dy = (fu_layout[p.alg][best][1] - fu_layout[p.alg][op][1]) * FU_ROW;   /* up = negative y */
        side[op] = dx < 0 ? -1 : dx > 0 ? 1 : 0;
        uint32_t len = fu_isqrt((uint32_t)(dx * dx + dy * dy));
        fp32 dist = rad[best] + rad[op] - ((FP(FU_TOOTH) * eff[op]) >> 15);
        uint32_t ox = udiv((uint32_t)(dx < 0 ? -dx : dx) * (uint32_t)dist, len);
        uint32_t oy = udiv((uint32_t)(dy < 0 ? -dy : dy) * (uint32_t)dist, len);
        cx[op] = cx[best] + (dx < 0 ? -(fp32)ox : (fp32)ox);
        cy[op] = cy[best] + (dy < 0 ? -(fp32)oy : (fp32)oy);
    }
    for (int op = 0; op < FM4_OPS; op++) {  /* straight above its target: the label goes where no sibling is */
        if (side[op] || prim[op] < 0)
            continue;
        int l = 0, rt = 0;
        for (int j = 0; j < FM4_OPS; j++)
            if (j != op && prim[j] == prim[op])
                l |= side[j] < 0, rt |= side[j] > 0;
        side[op] = l && rt ? 0 : l ? 1 : rt ? -1 : fu_layout[p.alg][op][0] * 2 < fu_width[p.alg] ? -1 : 1;
    }

    /* fit: scale k (Q10, at most FU_KMAX) and centre the train's bounding box in the screen box */
    {
        fp32 x0 = FP(9999), x1 = -FP(9999), y0 = FP(9999), y1 = -FP(9999);
        for (int op = 0; op < FM4_OPS; op++) {
            x0 = cx[op] - rad[op] < x0 ? cx[op] - rad[op] : x0;
            x1 = cx[op] + rad[op] > x1 ? cx[op] + rad[op] : x1;
            y0 = cy[op] - rad[op] < y0 ? cy[op] - rad[op] : y0;
            y1 = cy[op] + rad[op] > y1 ? cy[op] + rad[op] : y1;
        }
        if (p.fb > 0) {                     /* the pawl and its spring: up to 2.15 / 1.75 radii above / right of D */
            fp32 py = cy[3] - ((rad[3] * 2202) >> 10), px = cx[3] + ((rad[3] * 1792) >> 10);
            y0 = py < y0 ? py : y0;
            x1 = px > x1 ? px : x1;
        }
        uint32_t kx = udiv((uint32_t)(FU_X1 - FU_X0) << 20, (uint32_t)(x1 - x0));
        uint32_t ky = udiv((uint32_t)(FU_Y1 - FU_Y0) << 20, (uint32_t)(y1 - y0));
        int32_t kk = (int32_t)(kx < ky ? kx : ky);
        kk = kk > FU_KMAX ? FU_KMAX : kk;
        fp32 mx = (x0 + x1) >> 1, my = (y0 + y1) >> 1;
        for (int op = 0; op < FM4_OPS; op++) {
            cx[op] = FP((FU_X0 + FU_X1) / 2) + (((cx[op] - mx) >> 4) * kk >> 6);
            cy[op] = FP((FU_Y0 + FU_Y1) / 2) + (((cy[op] - my) >> 4) * kk >> 6);
            rad[op] = (rad[op] >> 4) * kk >> 6;
        }
    }

    /* turn: speed ~ ratio (capped at 4x) x envelope; meshing gears turn opposite ways */
    uint32_t fjit[FM4_OPS] = { 0, 0, 0, 0 };
    for (int op = 0; op < FM4_OPS; op++) {
        uint32_t rq = p.ratio[op] > (4u << 16) ? 4u << 16 : p.ratio[op];
        uint32_t sp = rq * (uint32_t)(env[op] >> 5);              /* 1/64 turn per frame at 1:1, full */
        fu_ang[op] += depth[op] & 1 ? -sp : sp;
    }
    for (int op = 1; op < FM4_OPS; op++)    /* the driven gears jitter: bite x sin(modulator angle) */
        for (int t = 0; t < op; t++)
            if (fm4_mods[p.alg][t] >> op & 1) {
                int32_t a = (eff[op] * env[op]) >> 15;
                fjit[t] += (uint32_t)(((a * sin_t(fu_ang[op])) >> 15) << 12);    /* up to 1/32 turn */
            }

    /* colours: carriers by level, modulators by bite, the selected page white */
    int col[FM4_OPS];
    for (int op = 0; op < FM4_OPS; op++) {
        int lv = p.level[op];
        if (page == op + 1)
            col[op] = WHITE;
        else if (car >> op & 1)
            col[op] = !lv ? DARKBLUE : lv < 6 ? GREY : lv < 11 ? PURPLEGREY : lv < 14 ? 17 : WHITE;
        else
            col[op] = !lv || eff[op] < 2048 ? DARKBLUE : eff[op] < 12000 ? LIGHTBLUE : BLUE;
    }
    int mc = p.index < 2048 ? DARKBLUE : p.index < 12000 ? LIGHTBLUE : BLUE;
    fp32 chat = playing && (u->frame & 1) ? (p.fb * 3) >> 5 : 0;   /* pawl chatter, 0 .. 3 units */

#ifdef UI_HAS_SVG
    if (fu_svg()) {
        int nb = 0;
        for (int i = 0; i < 2; i++)
            show(fu_belt[i], 0);
        for (int op = 1; op < FM4_OPS; op++)            /* blue belts: a modulator's other targets */
            for (int t = 0; t < op; t++)
                if (fm4_mods[p.alg][t] >> op & 1 && t != prim[op] && nb < 2) {
                    fp32 dx = cx[t] - cx[op], dy = cy[t] - cy[op];
                    uint32_t len = fu_isqrt((uint32_t)((dx >> 10) * (dx >> 10) + (dy >> 10) * (dy >> 10)));
                    int32_t nx = len ? (int32_t)udiv((uint32_t)(dx < 0 ? -dx : dx), len) : 1024;
                    int32_t ny = len ? (int32_t)udiv((uint32_t)(dy < 0 ? -dy : dy), len) : 0;
                    nx = dx < 0 ? -nx : nx;
                    ny = dy < 0 ? -ny : ny;
                    set_matrix(fu_belt[nb], (dx * 655) >> 16, (dy * 655) >> 16, -ny, nx, cx[op], cy[op]);
                    recol_set(&fu_beltc[nb], fu_rgb[p.level[op] ? mc : DARKBLUE], 256);
                    show(fu_belt[nb++], 1);
                }
        for (int op = 0; op < FM4_OPS; op++) {
            int32_t s = (rad[op] * 3277) >> 16;            /* radius / FU_TIP, Q10 */
            uint32_t a = fu_ang[op] + fjit[op];
            int32_t c = ((cos_t(a) >> 5) * s) >> 10, sn = ((sin_t(a) >> 5) * s) >> 10;
            set_matrix(fu_g[op], c, sn, -sn, c, cx[op], cy[op]);
            recol_set(&fu_body[op], fu_rgb[col[op]], 256);
            recol_set(&fu_hub[op], fu_rgb[col[op]], 256);
            show(fu_sel[op], page == op + 1);
        }
        show(fu_pawl, p.fb > 0);
        if (p.fb > 0) {                     /* D's translate x scale, then the size about the tooth contact, then chatter */
            int32_t s = (rad[3] * 3277) >> 16, f = 717 + ((p.fb * 512) >> 15);
            struct aff g = { s, 0, 0, s, cx[3], cy[3] }, sc = { f, 0, 0, f, 0, 0 }, rot, in, m;
            sc.e = (14479 * (1024 - f)) >> 10;              /* the contact (14.14, -14.14) stays put */
            sc.f = (-14479 * (1024 - f)) >> 10;
            aff_rot(&rot, (uint32_t)chat * 23300u, FP(24), -FP(26));     /* up to 6 deg about the pivot */
            aff_mul(&in, &sc, &rot);
            aff_mul(&m, &g, &in);
            set_aff(fu_pawl, &m);
            for (int i = 0; i < 3; i++)
                recol_set(&fu_pawlc[i], p.fb < 4096 ? 0x4E2832 + (uint32_t)i : 0xFF423F - (uint32_t)i, 256);
        }
        svg_render(fu_doc.doc);
    } else
#endif
    {
        for (int op = 1; op < FM4_OPS; op++)            /* blue belts */
            for (int t = 0; t < op; t++)
                if (fm4_mods[p.alg][t] >> op & 1 && t != prim[op])
                    line_fp(r, cx[op], cy[op], cx[t], cy[t], FP(1) + ((eff[op] * 3) >> 5), p.level[op] ? mc : DARKBLUE);
        if (p.fb > 0) {                                 /* the pawl */
            int pc = p.fb < 4096 ? OFFRED : ORANGE;
            fp32 len = FP(4) + ((p.fb * 9) >> 5);
            fp32 rimx = cx[3] + ((rad[3] * 23170) >> 15), rimy = cy[3] - ((rad[3] * 23170) >> 15);
            fp32 ax = rimx + len, ay = rimy - len;
            line_fp(r, ax, ay, rimx + chat, rimy - chat, FP(3) >> 1, pc);
            fp32 sx = ax, sy = ay;
            for (int i = 1; i <= 4; i++) {
                fp32 nx = ax + (i & 1 ? FP(3) : -FP(1)), ny = ay - FP(2) * i;
                line_fast(r, sx, sy, nx, ny, pc);
                sx = nx;
                sy = ny;
            }
        }
        for (int op = 0; op < FM4_OPS; op++) {
            int teeth = 3 + (int)(rad[op] >> 11);
            uint32_t a = fu_ang[op] + fjit[op];
            fu_gear(r, cx[op], cy[op], rad[op], a, teeth, col[op]);
            if (page == op + 1)
                fu_gear(r, cx[op], cy[op], rad[op] - 862, a, teeth, col[op]);
            fp32 hr = rad[op] - FP(FU_TOOTH + 2);
            line_fast(r, cx[op], cy[op], cx[op] + (((hr >> 5) * cos_t(a)) >> 10), cy[op] - (((hr >> 5) * sin_t(a)) >> 10),
                      col[op]);
        }
    }

    /* labels: letter and ratio beside each modulator (outside the train), under each carrier; operator page marks */
    static const char *const letter[FM4_OPS] = { "A", "B", "C", "D" };
    for (int op = 0; op < FM4_OPS; op++) {
        int sel = page == op + 1;
        char buf[12];
        buf[0] = letter[op][0];
        fp32 tw = FP(6) * (fp32)(fm4_ratio_text(buf + 1, p.ratio[op]) - buf);
        int lc = sel ? WHITE : GREY;
        if (!p.packed && op)
            lc = op == 1 ? BLUE : op == 2 ? GREEN : WHITE;        /* v0: the SHIFT knob that sets it */
        fp32 lx, ly;
        if (car >> op & 1) {                /* carriers: lower left, clear of the modulators above */
            lx = cx[op] - ((rad[op] * 3) >> 2) - tw;
            ly = cy[op] + ((rad[op] * 3) >> 2) - FP(4);
        } else {
            lx = side[op] < 0 ? cx[op] - rad[op] - FP(3) - tw : side[op] > 0 ? cx[op] + rad[op] + FP(4) : cx[op] - (tw >> 1);
            ly = side[op] ? cy[op] - FP(4) : cy[op] - rad[op] - FP(11);
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

    /* top left: the operator page's values, the algorithm number, TIME, or a v0 detune */
    char buf[32], *q = buf;
    if (page) {
        int op = page - 1;
        q = fu_str(q, letter[op]);
        q = fu_str(q, " ");
        q = fm4_ratio_text(q, p.ratio[op]);
        text(r, buf, FP(8), FP(8), FP(2), WHITE);
        q = fu_str(buf, "LVL ");
        q = fu_dec(q, p.level[op]);
        q = fu_str(q, " ");
        fu_str(q, fm4_env_name[p.env[op]]);
        text_fast(r, buf, FP(8), FP(26), FP(1), WHITE);
    } else if (u->frame - u->alg_frame < FU_SHOW) {
        q = fu_str(q, "ALG ");
        fu_dec(q, (uint32_t)p.alg + 1);
        text(r, buf, FP(8), FP(8), FP(2), GREEN);
    } else if ((int32_t)u->frame - fu_time_frame < FU_SHOW) {
        static const char *const tname[9] = { "0.25", "0.35", "0.5", "0.71", "1", "1.41", "2", "2.83", "4" };
        int32_t t = (fm4_knob(k[2]) * 8 + 16384) >> 15;
        q = fu_str(q, "TIME X");
        fu_str(q, tname[t > 8 ? 8 : t]);
        text(r, buf, FP(8), FP(8), FP(3) >> 1, WHITE);
    } else if (!p.packed && (int32_t)u->frame - fu_detune_frame < FU_SHOW) {
        q = fu_str(q, "DETUNE ");
        q = fu_dec(q, (uint32_t)((p.detune * 17 + 16384) >> 15));
        fu_str(q, " C");
        text(r, buf, FP(8), FP(8), FP(3) >> 1, ORANGE);
    }
    hook_delta_diag(r, FP(316), FP(131), 1);   /* TE's raw encoder delta, bottom right */
}

#endif
