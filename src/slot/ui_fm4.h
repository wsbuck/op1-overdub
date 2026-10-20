/* fm4 screen (notes/fm-operator.md "Screen"; gear design, SVG version 2, 2026-10-14), for inclusion into src/slot/ui.c
 * after its drawing primitives (line_fp, line_fast, text, text_fast, sin_t, cos_t, udiv, the colour enum, RAM,
 * voice_ram) and, with UI_HAS_SVG, its SVG helpers (svg_open, svg_render, show, set_matrix, set_aff, aff_mul, aff_rot,
 * recol_*). src/slot/test/screens_preview.py renders the line version on the host; src/slot/test/preview.py the SVG.
 *
 * draw_fm4(r, k, u): k = the sound's 8 stored knobs, u = the edit state (fm4_edit.h).
 *   Every operator is a gear and the algorithm is the gear train (design/synth/fm4.svg). All gears share one tooth pitch,
 *   so a size is a tooth count (fu_teeth, from the ratio) and any two mesh truly: a modulator sits at its driver's pitch
 *   radius plus its own (minus nothing at full bite, plus 0.8 of the tooth depth at none: the teeth barely touch), on
 *   the layout's side of it, swung around the driver until it clears the other gears, and its angle is locked to the
 *   driver's (a tooth of one in a gap of the other): the train turns like a real one, modulators driven by carriers.
 *   Carriers (the operators you hear) spin by their own envelope x ratio (capped at 4x), read from the voices that
 *   rendered since the last frame (fm4.c blocks): still in silence. Carriers turn grey .. white with their level
 *   (white = the gear you hear); modulators are blue.
 *   blue (INDEX) x a modulator's level x its envelope (its level x INDEX when still) = tooth bite (how deep the mesh)
 *     and the modulator's colour (dark blue .. blue).
 *   green: ALGORITHM rearranges the train; ALG n shows for ~1.5 s after a change, top left.
 *   white: TIME (envelopes faster or slower); TIME xN shows for ~1.5 s after a change.
 *   orange: FEEDBACK: a spring-loaded pawl on D's teeth, bigger with feedback, chattering while a note sounds.
 *   A note starting gives the train a small hop.
 *   operator pages (hook): the selected gear white with an outer ring and knob-coloured marks under it (blue ratio,
 *     green fine, white level, orange envelope), its values top left (C 4.41 / LVL 11 SWELL).
 *   labels: letter and ratio beside each gear, placed where they hit no gear or other label.
 *   Sizes: 3 tooth counts (9, 14, 21) by ratio: 0.5 .. 1.5 small, 2 .. 5 middle, 6 and above large.
 *   The train is fitted into the screen below the readouts and the HOLD/CHORD badges at one fixed scale (smaller only
 *   when it doesn't fit), so sizes compare across algorithms.
 * Without the SVG (file missing, or the host harness) the gears are drawn as 1-px polylines at the same places.
 */
#ifndef UI_FM4_H
#define UI_FM4_H
#include "fm4.h"
#include "fm4_edit.h"
#include "modal_state.h"
#include "ui_hookdiag.h"

#define FU_COL 26                           /* layout half-column pitch (direction only) */
#define FU_ROW 16                           /* layout row pitch (direction only) */
#define FU_X0 30                            /* the train's box on screen */
#define FU_X1 290
#define FU_Y0 42                            /* below the readouts and the HOLD/CHORD badges (y <= 39) */
#define FU_Y1 139
#define FU_KFIX 1434                        /* the scale, Q10 (1.4), unless the train doesn't fit */
#define FU_SHOW 45
#define FU_HDEPTH 1741                      /* half the tooth depth, Q10 units (gen_fm4.py DEPTH / 2) */
#define FU_SIZES 3
#ifndef FM4_VOICES
#define FM4_VOICES (voice_ram)              /* engines' per-voice RAM: fm4.c's struct fm4_voice per TE voice 0..5 */
#endif

static const uint8_t fu_teeth[FU_SIZES] = { 9, 14, 21 };                  /* = gen_fm4.py SIZES */
static const int32_t fu_rp[FU_SIZES] = { 8800, 13689, 20534 };             /* pitch radii: teeth x 6 / 2 pi, Q10 */

/* (half column, row) of A, B, C, D per algorithm: the side a modulator sits on from what it drives */
static const uint8_t fu_layout[FM4_ALGS][FM4_OPS][2] = {
    { { 0, 0 }, { 1, 1 }, { 0, 2 }, { 1, 3 } },    /* the stack zigzags */
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

static int16_t fu_prev[8] RAM = { 0 };      /* knobs at the last frame (what changed) */
static int32_t fu_detune_frame RAM = -1000;
static int32_t fu_time_frame RAM = -1000;
static uint32_t fu_ang[FM4_OPS] RAM = { 0 };     /* gear angles, 2^32 = one turn */
static uint32_t fu_blocks[6] RAM = { 0 };   /* fm4_voice.blocks of TE voices 0..5 at the last frame */
static int32_t fu_bounce RAM = 0;           /* note-start bounce, Q15, decaying */
static int32_t fu_was_playing RAM = 0;

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

/* size index from the ratio (Q16), in quarter octaves: 0.5 .. 1.5 small, 2 .. 5 middle, 6 and above large */
static int fu_size(uint32_t ratio)
{
    uint32_t v = ratio >> 11, l = 0;        /* ratio x 32 */
    while (v >= 32) {
        v >>= 1;
        l += 4;
    }
    l += v >= 27 ? 3 : v >= 23 ? 2 : v >= 19 ? 1 : 0;
    return l < 7 ? 0 : l < 14 ? 1 : 2;
}

/* direction of (dx, dy) as a turn (1/64 resolution: the layout's directions; y down) */
static uint32_t fu_dir(int32_t dx, int32_t dy)
{
    uint32_t best = 0;
    int32_t bd = -0x7FFFFFFF;
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t t = i << 26;
        int32_t d = (dx * (cos_t(t) >> 5) + dy * (sin_t(t) >> 5)) >> 4;
        if (d > bd) {
            bd = d;
            best = t;
        }
    }
    return best;
}

/* A gear as a 1-px polyline: n teeth, tooth 0 centred at angle a (as the SVG's), pitch radius rp */
static void fu_gear(void *r, fp32 x, fp32 y, fp32 rp, uint32_t a, int n, int colour)
{
    fp32 ro = rp + FU_HDEPTH, ri = rp - FU_HDEPTH;
    uint32_t st = udiv(0xFFFFFFFFu, (uint32_t)n), o1 = (st >> 6) * 23, o2 = (st >> 6) * 11;
    fp32 px = 0, py = 0, fx = 0, fy = 0;
    for (int i = 0; i < n; i++, a += st) {
        const uint32_t ang[4] = { a - o1, a - o2, a + o2, a + o1 };
        for (int j = 0; j < 4; j++) {
            fp32 rr = j == 0 || j == 3 ? ri : ro;
            fp32 qx = x + (((rr >> 5) * cos_t(ang[j])) >> 10), qy = y + (((rr >> 5) * sin_t(ang[j])) >> 10);
            if (i || j)
                line_fast(r, px, py, qx, qy, colour);
            else {
                fx = qx;
                fy = qy;
            }
            px = qx;
            py = qy;
        }
    }
    line_fast(r, px, py, fx, fy, colour);
}

#ifdef UI_HAS_SVG
/* ---- the SVG document (design/synth/fm4.svg): found once, cached ---- */
static struct svgdoc fu_doc RAM = { 0, 0 };
static int32_t fu_found RAM = 0;
static void *fu_g[FM4_OPS] RAM = { 0 }, *fu_v[FM4_OPS][FU_SIZES] RAM = { { 0 } };
static void *fu_vsel[FM4_OPS][FU_SIZES] RAM = { { 0 } }, *fu_belt[2] RAM = { 0 }, *fu_pawl RAM = 0;
static struct recol fu_body[FM4_OPS][FU_SIZES] RAM = { { { 0, { 0 } } } }, fu_hub[FM4_OPS][FU_SIZES] RAM = { { { 0, { 0 } } } };
static struct recol fu_beltc[2] RAM = { { 0, { 0 } } }, fu_pawlc[3] RAM = { { 0, { 0 } } };

static void fu_id(char *b, char g, int op, int j, const char *tail)       /* "ga2body" */
{
    *b++ = g;
    *b++ = (char)('a' + op);
    if (j >= 0)
        *b++ = (char)('0' + j);
    fu_str(b, tail);
}

static int fu_svg(void)
{
    if (!svg_open(&fu_doc, "/yaffs2/content/display/fm4.svg", "fm4.svg"))
        return 0;
    if (!fu_found) {
        void *d = fu_doc.doc;
        char id[16];
        for (int op = 0; op < FM4_OPS; op++) {
            fu_id(id, 'g', op, -1, "");
            fu_g[op] = te_node_find(d, id, 0);
            for (int j = 0; j < FU_SIZES; j++) {
                fu_id(id, 'g', op, j, "");
                fu_v[op][j] = te_node_find(d, id, 0);
                fu_id(id, 'g', op, j, "sel");
                fu_vsel[op][j] = te_node_find(d, id, 0);
                fu_id(id, 'g', op, j, "body");
                recol_init(&fu_body[op][j], d, id, 1);
                fu_id(id, 'g', op, j, "hub");
                recol_init(&fu_hub[op][j], d, id, 1);
            }
        }
        fu_belt[0] = te_node_find(d, "belt1", 0);
        fu_belt[1] = te_node_find(d, "belt2", 0);
        recol_init(&fu_beltc[0], d, "belt1", 1);
        recol_init(&fu_beltc[1], d, "belt2", 1);
        fu_pawl = te_node_find(d, "pawl", 0);
        recol_init(&fu_pawlc[0], d, "arm", 1);
        recol_init(&fu_pawlc[1], d, "pivot", 1);
        recol_init(&fu_pawlc[2], d, "spring", 1);
        fu_found = 1;
    }
    return 1;
}

#endif

static const uint32_t fu_rgb[19] = {        /* the palette entries used here, as 0xRRGGBB (colors.svg) */
    [DARKBLUE] = 0x383572, [LIGHTBLUE] = 0xBFD8FF, [BLUE] = 0x698EFF, [GREY] = 0x7C7990, [PURPLEGREY] = 0xAEB1DC,
    [17] = 0xE6E7FF, [WHITE] = 0xFFFFFF, [ORANGE] = 0xFF423F, [OFFRED] = 0x4E2832,
};

/* does the box (x0, y0)-(x1, y1) hit a gear (outer radius ro, a margin m) or leave the screen? */
static int fu_hits(fp32 x0, fp32 y0, fp32 x1, fp32 y1, const fp32 *cx, const fp32 *cy, const fp32 *ro, int n)
{
    if (x0 < FP(4) || x1 > FP(316) || y0 < FP(41) || y1 > FP(140))
        return 1;
    for (int i = 0; i < n; i++) {
        fp32 nx = cx[i] < x0 ? x0 : cx[i] > x1 ? x1 : cx[i], ny = cy[i] < y0 ? y0 : cy[i] > y1 ? y1 : cy[i];
        int32_t dx = (nx - cx[i]) >> 8, dy = (ny - cy[i]) >> 8, rr = (ro[i] + FP(1)) >> 8;
        if (dx * dx + dy * dy < rr * rr)
            return 1;
    }
    return 0;
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
    if (playing && !fu_was_playing)
        fu_bounce = 32767;
    fu_was_playing = playing;
    fu_bounce = (fu_bounce * 27) >> 5;      /* x 0.84 per frame */

    /* sizes, bite (Q15: level x INDEX, x the envelope while playing) */
    int32_t ix = p.index * 2 > 32767 ? 32767 : p.index * 2;      /* INDEX 1x (mid knob 0.5x .. ) = full bite */
    int32_t bite[FM4_OPS], eff[FM4_OPS];
    int sz[FM4_OPS];
    fp32 rp[FM4_OPS], ro[FM4_OPS], cx[FM4_OPS], cy[FM4_OPS];
    uint32_t th[FM4_OPS];
    int depth[FM4_OPS], prim[FM4_OPS];
    for (int op = 0; op < FM4_OPS; op++) {
        eff[op] = (fm4_level[p.level[op]] * ix) >> 15;
        bite[op] = playing ? (eff[op] * env[op]) >> 15 : eff[op];
        sz[op] = fu_size(p.ratio[op]);
        rp[op] = fu_rp[sz[op]];
        ro[op] = rp[op] + FU_HDEPTH;
        prim[op] = -1;
        depth[op] = 0;
        th[op] = 0;
    }
    /* carriers left to right (layout columns increase with the index), packed by size: each column as wide as the
     * largest gear standing in it, more where a gear stands between two carriers */
    fp32 prevx = 0, prevw = 0;
    int ncar = 0, prevc = 0;
    for (int op = 0; op < FM4_OPS; op++) {
        if (!(car >> op & 1))
            continue;
        int c = fu_layout[p.alg][op][0];
        fp32 w = 0, gap = FP(8);
        for (int j = 0; j < FM4_OPS; j++) {
            int cj = fu_layout[p.alg][j][0];
            if (cj == c && ro[j] > w)
                w = ro[j];
            if (ncar && cj > prevc && cj < c && 2 * ro[j] + FP(4) > gap)
                gap = 2 * ro[j] + FP(4);
        }
        cx[op] = ncar++ ? prevx + prevw + w + gap : 0;
        cy[op] = 0;
        prevx = cx[op];
        prevw = w;
        prevc = c;
    }
    /* modulators: on the layout's side of their driver, at both pitch radii + 0.8 tooth depth x (1 - bite), swung
     * around the driver (7.5 degree steps, alternating) until clear of every gear placed so far */
    for (int op = 0; op < FM4_OPS; op++) {
        if (car >> op & 1)
            continue;
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
        if (best < 0) {
            cx[op] = cy[op] = 0;
            continue;
        }
        prim[op] = best;
        depth[op] = depth[best] + 1;
        int32_t dx = (fu_layout[p.alg][op][0] - fu_layout[p.alg][best][0]) * FU_COL;
        int32_t dy = (fu_layout[p.alg][best][1] - fu_layout[p.alg][op][1]) * FU_ROW;
        uint32_t t0 = fu_dir(dx, dy);
        fp32 dist = rp[best] + rp[op] + (((FU_HDEPTH * 13) >> 3) * (32767 - bite[op]) >> 15);
        int32_t best_gap = -0x7FFFFFFF;
        for (int s = 0; s < 25; s++) {      /* up to +-90 degrees: the clearest spot, the layout's side first */
            int32_t step = (s + 1) >> 1;
            uint32_t t = t0 + (uint32_t)((s & 1) ? step : -step) * 0x05555555u;   /* 7.5 degrees */
            fp32 x = cx[best] + (((dist >> 5) * cos_t(t)) >> 10), y = cy[best] + (((dist >> 5) * sin_t(t)) >> 10);
            int32_t gap = 0x7FFFFFFF;       /* the smallest clearance to any other gear, units^2 / 16 */
            for (int j = 0; j < FM4_OPS; j++) {
                if (j == op || j == best || (j > op && !(car >> j & 1)) || (j < op && prim[j] < 0 && !(car >> j & 1)))
                    continue;
                int32_t ddx = (x - cx[j]) >> 8, ddy = (y - cy[j]) >> 8, rr = (ro[op] + ro[j] + FP(2)) >> 8;
                int32_t g = ddx * ddx + ddy * ddy - rr * rr;
                gap = g < gap ? g : gap;
            }
            if (y > -(ro[op] >> 1))         /* stay above the carriers' row */
                gap -= 0x100000;
            if (gap > best_gap) {
                best_gap = gap;
                cx[op] = x;
                cy[op] = y;
                th[op] = t;
            }
            if (gap >= 0)
                break;
        }
    }

    /* fit: one scale (smaller only if needed), centred in the box */
    int32_t kk;
    {
        fp32 x0 = FP(9999), x1 = -FP(9999), y0 = FP(9999), y1 = -FP(9999);
        for (int op = 0; op < FM4_OPS; op++) {
            x0 = cx[op] - ro[op] < x0 ? cx[op] - ro[op] : x0;
            x1 = cx[op] + ro[op] > x1 ? cx[op] + ro[op] : x1;
            y0 = cy[op] - ro[op] < y0 ? cy[op] - ro[op] : y0;
            y1 = cy[op] + ro[op] > y1 ? cy[op] + ro[op] : y1;
        }
        y0 -= FP(2);                        /* room for the note-start hop */
        if (p.fb > 0) {                     /* the pawl and its spring at 1.2x: up to 2.45 / 2 tip radii above / right of D */
            fp32 py = cy[3] - ((ro[3] * 2509) >> 10), px = cx[3] + ((ro[3] * 2048) >> 10);
            y0 = py < y0 ? py : y0;
            x1 = px > x1 ? px : x1;
        }
        uint32_t kx = udiv((uint32_t)(FU_X1 - FU_X0) << 20, (uint32_t)(x1 - x0));
        uint32_t ky = udiv((uint32_t)(FU_Y1 - FU_Y0) << 20, (uint32_t)(y1 - y0));
        kk = (int32_t)(kx < ky ? kx : ky);
        kk = kk > FU_KFIX ? FU_KFIX : kk;
        fp32 mx = (x0 + x1) >> 1, my = (y0 + y1) >> 1;
        fp32 hop = (fu_bounce * (cos_t(u->frame * 0x2AAAAAAAu) >> 5)) >> 14;       /* up to 2 units, ringing */
        fp32 ox = FP((FU_X0 + FU_X1) / 2), oy = FP((FU_Y0 + FU_Y1) / 2);
        for (int op = 0; op < FM4_OPS; op++) {
            cx[op] = ox + (((cx[op] - mx) >> 4) * kk >> 6);
            cy[op] = oy + (((cy[op] - my) >> 4) * kk >> 6) - (hop > 0 ? hop : 0);
            rp[op] = (rp[op] >> 4) * kk >> 6;
            ro[op] = (ro[op] >> 4) * kk >> 6;
        }
    }

    /* turn: carriers by their own speed (ratio, capped at 4x, x envelope); every modulator locked to its driver: the
     * driver's tooth phase at the contact direction th, mirrored into a gap of the modulator */
    for (int op = 0; op < FM4_OPS; op++) {
        if (car >> op & 1) {
            uint32_t rq = p.ratio[op] > (4u << 16) ? 4u << 16 : p.ratio[op];
            fu_ang[op] += rq * (uint32_t)(env[op] >> 5);  /* 1/64 turn per frame at 1:1, full */
        } else if (prim[op] >= 0) {
            int t = prim[op];
            uint32_t ph = (th[op] - fu_ang[t]) * fu_teeth[sz[t]];       /* the driver's tooth phase there */
            uint32_t n = fu_teeth[sz[op]];
            fu_ang[op] = th[op] + 0x80000000u - udiv(ph, n) - udiv(0x80000000u, n);
        }
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
            col[op] = !lv || bite[op] < 4096 ? DARKBLUE : BLUE;        /* never light blue: white is for carriers */
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
            int32_t sc = kk;
            int32_t a = (int32_t)fu_ang[op], cs = ((cos_t((uint32_t)a) >> 5) * sc) >> 10, sn = ((sin_t((uint32_t)a) >> 5) * sc) >> 10;
            set_matrix(fu_g[op], cs, sn, -sn, cs, cx[op], cy[op]);
            for (int j = 0; j < FU_SIZES; j++)
                show(fu_v[op][j], j == sz[op]);
            int j = sz[op];
            show(fu_vsel[op][j], page == op + 1);
            recol_set(&fu_body[op][j], fu_rgb[col[op]], 256);
            recol_set(&fu_hub[op][j], fu_rgb[col[op]], 256);
        }
        show(fu_pawl, p.fb > 0);
        if (p.fb > 0) {                     /* D's axle x its tip radius / 20, then the size about the tooth contact, then chatter */
            int32_t s = (ro[3] * 3277) >> 16, f = 717 + ((p.fb * 512) >> 15);
            struct aff g = { s, 0, 0, s, cx[3], cy[3] }, sc = { f, 0, 0, f, 0, 0 }, rot, in, m;
            sc.e = (14479 * (1024 - f)) >> 10;              /* the contact (14.14, -14.14) stays put */
            sc.f = (-14479 * (1024 - f)) >> 10;
            aff_rot(&rot, (uint32_t)chat * 23300u, FP(24), -FP(26));     /* up to 6 deg about the pivot */
            aff_mul(&in, &sc, &rot);
            aff_mul(&m, &g, &in);
            set_aff(fu_pawl, &m);
            for (int i = 0; i < 3; i++)
                recol_set(&fu_pawlc[i], p.fb < 4096 ? 0x4E2832 : 0xFF423F, 256);
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
            fp32 rimx = cx[3] + ((ro[3] * 23170) >> 15), rimy = cy[3] - ((ro[3] * 23170) >> 15);
            fp32 ax = rimx + len, ay = rimy - len;
            line_fp(r, ax, ay, rimx + chat, rimy - chat, FP(3) >> 1, pc);
        }
        for (int op = 0; op < FM4_OPS; op++) {
            fu_gear(r, cx[op], cy[op], rp[op], fu_ang[op], fu_teeth[sz[op]], col[op]);
            if (page == op + 1)
                fu_gear(r, cx[op], cy[op], rp[op] - 862, fu_ang[op], fu_teeth[sz[op]], col[op]);
        }
    }

    /* labels: letter and ratio, where they hit no gear and no other label (beside, outside first; then the other
     * side, above, below); operator page marks */
    static const char *const letter[FM4_OPS] = { "A", "B", "C", "D" };
    fp32 lb[FM4_OPS][4];
    for (int op = 0; op < FM4_OPS; op++) {
        int sel = page == op + 1;
        char buf[12];
        buf[0] = letter[op][0];
        fp32 tw = FP(6) * (fp32)(fm4_ratio_text(buf + 1, p.ratio[op]) - buf);
        int lc = sel ? WHITE : GREY;
        if (!p.packed && op)
            lc = op == 1 ? BLUE : op == 2 ? GREEN : WHITE;        /* v0: the SHIFT knob that sets it */
        int out = prim[op] >= 0 ? (cx[op] < cx[prim[op]] ? -1 : 1) : (cx[op] < FP(160) ? -1 : 1);
        fp32 cand[6][2] = {
            { out < 0 ? cx[op] - ro[op] - FP(3) - tw : cx[op] + ro[op] + FP(4), cy[op] - FP(4) },
            { out < 0 ? cx[op] + ro[op] + FP(4) : cx[op] - ro[op] - FP(3) - tw, cy[op] - FP(4) },
            { cx[op] - (tw >> 1), cy[op] - ro[op] - FP(11) },
            { out < 0 ? cx[op] - ro[op] - tw : cx[op] + ro[op], cy[op] - ro[op] - FP(6) },
            { out < 0 ? cx[op] - ro[op] - tw : cx[op] + ro[op], cy[op] + ro[op] - FP(2) },
            { cx[op] - (tw >> 1), cy[op] + ro[op] + FP(3) },
        };
        int pick = 0;
        for (int c = 0; c < 6; c++) {
            fp32 x0 = cand[c][0], y0 = cand[c][1], x1 = x0 + tw, y1 = y0 + FP(9);
            int bad = fu_hits(x0, y0, x1, y1, cx, cy, ro, FM4_OPS);
            for (int j = 0; j < op && !bad; j++)
                bad = x0 < lb[j][2] && x1 > lb[j][0] && y0 < lb[j][3] && y1 > lb[j][1];
            if (!bad) {
                pick = c;
                break;
            }
        }
        lb[op][0] = cand[pick][0];
        lb[op][1] = cand[pick][1];
        lb[op][2] = cand[pick][0] + tw;
        lb[op][3] = cand[pick][1] + FP(9);
        text_fast(r, buf, cand[pick][0], cand[pick][1], FP(1), lc);
        if (sel) {                          /* knob-coloured marks: ratio, fine, level, envelope */
            fp32 y = cy[op] + ro[op] + FP(3), x = cx[op] - FP(10);
            if (car >> op & 1)
                y = cy[op] - ro[op] - FP(5);
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
    hook_delta_diag(r, FP(316), FP(131), 1);   /* TE's raw encoder delta, bottom right (--debug-ui only) */
}

#endif
