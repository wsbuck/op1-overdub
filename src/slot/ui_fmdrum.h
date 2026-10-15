/* The fmdrum engine's screen (src/slot/fmdrum.c, notes/drum-engine.md "fmdrum"), included by src/slot/ui.c after its
 * drawing helpers; drawn on the dbox screen the drum engines borrow (slot.S _drum_ui_update -> drum_ui_draw).
 *
 * The selected key (the last one pressed: the one the knobs edit) as a drum, with its envelopes, over the 24 keys:
 *   title    what the key's 8 values make it: KICK, TOM, SNARE, CLAP, HAT, METAL, BELL, ZAP or PERC, in that class's
 *            colour, and the key number
 *   drum     an oblique drum: low pitch = a big drum, DECAY = a deep shell (orange lugs). On the head the FM: a rosette
 *            of 2 x RATIO petals (harmonic ratios close in one turn, inharmonic ones need two and shimmer), petal depth
 *            = FM. SWEEP is the blue curve falling onto the head (its bend = SNAP); NOISE the speckle above the head
 *            (AIR dark and low, SNARE grey, HAT bright and high) or orange clap marks.
 *            While the key plays: the head hops and turns white, the rosette follows the live index (MOD's decay) and
 *            turns, a blue dot rides the sweep, ripples spread with the amplitude and the speckle sprays.
 *   graph    level against log time (1.5 ms .. 6 s; ticks at 10 ms, 0.1 s, 1 s): DECAY (orange), the FM index's decay
 *            (MOD, white, starting at the FM depth) and the sweep (blue, starting at the SWEEP depth); a playhead while
 *            the key sounds
 *   values   PITCH Hz, RATIO, FM index (rad), DECAY; below, SHIFT's: SWEEP, SNAP, NOISE zone, MOD (knob colours)
 *   keys     the OP-1's 24 keys (F .. E): each key's class colour as a mark, lit while it sounds, the selected key
 *            outlined white
 * Reads the slot's per-key values (TE's 0x01AAADB4) and fmdrum's voices (fmdrum.h). No division; 1-px lines except
 * the title, the head outline and the sweep curve (about 60 anti-aliased segments, as the other engine screens).
 */
#ifndef UI_FMDRUM_H
#define UI_FMDRUM_H
#include "fmdrum.h"
#include "fmdrum_tables.h"

extern struct fd_voice fmdrum_voice[] __attribute__((weak));

#define LIGHTORANGE 19
enum { FC_KICK, FC_TOM, FC_SNARE, FC_CLAP, FC_HAT, FC_METAL, FC_BELL, FC_ZAP, FC_PERC };
static const char *const fd_class_name[9] = { "KICK", "TOM", "SNARE", "CLAP", "HAT", "METAL", "BELL", "ZAP", "PERC" };
static const uint8_t fd_class_colour[9] = { BLUE, LIGHTBLUE, WHITE, ORANGE, LIGHTGREEN, GREEN, LIGHTGREEN, RED, GREY };

static uint32_t fd_frame RAM = 0;

/* what a key's values make it */
static int fd_class(const int16_t *k)
{
    int z = fd_noise_zone_of(k[6]);
    if (z == 4)
        return FC_CLAP;
    if (z == 3)
        return FC_HAT;
    if (z == 2)
        return FC_SNARE;
    int32_t idx = fd_index_q8(k[2]), dec = fd_pos(k[3]) >> 7;
    int harm = (FD_RATIO_HARMONIC >> (fd_pos(k[1]) >> 10)) & 1;
    if (fd_sweep_q(k[4]) >= 30 * 512)
        return FC_ZAP;
    if (idx >= 384 && dec >= 192)               /* >= 1.5 rad and a decay of ~1 s or more */
        return FC_BELL;
    if (!harm && idx >= 512)                    /* inharmonic, >= 2 rad */
        return FC_METAL;
    if (k[0] < -9000)                           /* below ~80 Hz */
        return FC_KICK;
    if (k[0] < 5300)                            /* below ~400 Hz */
        return FC_TOM;
    return FC_PERC;
}

/* pitch K0 -> Hz (as fmdrum.c's inc_of: 13.75 Hz x 2^octaves) */
static uint32_t fd_hz(int16_t k0)
{
    int32_t u = k0 + 24576;
    uint32_t o13 = u < 0 ? 0 : ((uint32_t)u * 21845u) >> 14;
    uint32_t oct = o13 >> 13, fr = o13 & 8191, i = fr >> 7, f7 = fr & 127;
    uint32_t m = fd_exp2[i] + (((fd_exp2[i + 1] - fd_exp2[i]) * f7) >> 7);
    return (((3520u * m) >> 15) << oct) >> 8;  /* 13.75 Hz = 3520 in Q8 */
}

/* log2(x) in Q8 for x >= 1 (the top bit's position plus a linear fraction: within 0.09 octave) */
static int32_t fd_log2_q8(uint32_t x)
{
    int32_t n = 0;
    while (n < 31 && x >> (n + 1))
        n++;
    uint32_t frac = n >= 8 ? (x >> (n - 8)) & 255 : (x << (8 - n)) & 255;
    return (n << 8) + (int32_t)frac;
}

/* 0.1 ms units -> "450MS", "4000MS" (always ms: the stroke font's S and 5 look alike, so no "1.2S") */
static char *fd_put_time(char *p, uint32_t t10)
{
    return put_str(put_dec(p, (t10 * 6554 + 32768) >> 16), "MS");
}

/* ellipse of 1-px lines: n segments of `step` turns (uint32) from a0 */
static void fd_ellipse(void *r, fp32 cx, fp32 cy, fp32 rx, fp32 ry, int n, uint32_t step, uint32_t a0, int colour)
{
    fp32 px = cx + (((rx >> 5) * cos_t(a0)) >> 10), py = cy + (((ry >> 5) * sin_t(a0)) >> 10);
    for (int i = 1; i <= n; i++) {
        uint32_t a = a0 + step * (uint32_t)i;
        fp32 qx = cx + (((rx >> 5) * cos_t(a)) >> 10), qy = cy + (((ry >> 5) * sin_t(a)) >> 10);
        line_fast(r, px, py, qx, qy, colour);
        px = qx;
        py = qy;
    }
}

static uint32_t fd_hash(uint32_t a)
{
    a ^= a >> 16;
    a *= 0x7FEB352Du;
    a ^= a >> 15;
    a *= 0x846CA68Bu;
    return a ^ (a >> 16);
}

/* the graph: x of time point i (quarter octaves from 1 block), y of a level in dB (Q16, 0 .. -60) */
#define FD_GX0 172
#define FD_GW 140
#define FD_GY0 28
#define FD_GY1 84
#define FD_GX(i_q8) (FP(FD_GX0) + (fp32)((((int32_t)(i_q8)) * (FP(FD_GW) / 48)) >> 8))

/* one envelope: level(t) = top - slope x t over the 49 time points, until it reaches -60 dB */
static void fd_curve(void *r, int32_t top_db_q16, int32_t slope_q16, int colour)
{
    fp32 px = 0, py = 0;
    uint32_t s8 = (uint32_t)(slope_q16 >> 8);   /* Q8 dB per block x Q8 blocks = Q16 dB: small, no overflow */
    for (int i = 0; i < 49; i++) {
        int32_t db = top_db_q16 - (int32_t)(s8 * fd_t_q8[i]);
        int end = db <= -(60 << 16);
        if (end)
            db = -(60 << 16);
        if (db > 0)
            db = 0;
        fp32 x = FD_GX(i << 8), y = FP(FD_GY0) + (fp32)(((-db) >> 8) * 239 >> 6);   /* 56 units per 60 dB */
        if (i)
            line_fast(r, px, py, x, y, colour);
        px = x;
        py = y;
        if (end)
            break;
    }
}

/* the OP-1's keys from F: white key w -> key; black keys follow white 0, 1, 2, 4, 5 of each 7 */
static const uint8_t fd_white_key[14] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23 };

static void fd_key_box(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, int colour)
{
    line_fast(r, x0, y0, x1, y0, colour);
    line_fast(r, x1, y0, x1, y1, colour);
    line_fast(r, x1, y1, x0, y1, colour);
    line_fast(r, x0, y1, x0, y0, colour);
}

static void fd_keys(void *r, const int16_t *kit, int edit, const int32_t *lit)
{
    const fp32 y0 = FP(121), y1 = FP(140), yb = FP(132);
    line_fast(r, FP(13), y0, FP(13 + 21 * 14), y0, DARKBLUE);
    line_fast(r, FP(13), y1, FP(13 + 21 * 14), y1, DARKBLUE);
    for (int w = 0, pos = 0; w <= 14; w++, pos = pos == 6 ? 0 : pos + 1) {   /* separators, stopping under a black key */
        int black = w > 0 && w < 14 && pos != 0 && pos != 4;     /* pos = w mod 7: after F G A, C D */
        vline_fast(r, FP(13 + 21 * w), black ? yb : y0, y1, DARKBLUE);
    }
    for (int w = 0; w < 14; w++) {              /* white keys: lit from the bottom up by the level, a class mark */
        int key = fd_white_key[w], cls = fd_class(kit + key * 8);
        fp32 x0 = FP(13 + 21 * w), x1 = x0 + FP(21);
        if (lit[key] > 200) {
            fp32 top = y1 - (fp32)((lit[key] * 19) >> 5);
            for (fp32 x = x0 + FP(2); x <= x1 - FP(2); x += FP(1))
                vline_fast(r, x, top, y1 - FP(1), fd_class_colour[cls]);
        }
        if (key == edit)
            fd_key_box(r, x0, y0, x1, y1, WHITE);
        line_fast(r, x0 + FP(7), FP(136), x1 - FP(7), FP(136), fd_class_colour[cls]);
        line_fast(r, x0 + FP(7), FP(137), x1 - FP(7), FP(137), fd_class_colour[cls]);
    }
    for (int w = 0, pos = 0; w < 13; w++, pos = pos == 6 ? 0 : pos + 1) {   /* black keys (pos = w mod 7) */
        if (pos == 3 || pos == 6)               /* none after B, E */
            continue;
        int key = fd_white_key[w] + 1, cls = fd_class(kit + key * 8);
        fp32 x0 = FP(13 + 21 * w + 15), x1 = x0 + FP(12);
        if (lit[key] > 200) {
            fp32 top = yb - (fp32)((lit[key] * 11) >> 5);
            for (fp32 x = x0 + FP(2); x <= x1 - FP(2); x += FP(1))
                vline_fast(r, x, top, yb - FP(1), fd_class_colour[cls]);
        }
        fd_key_box(r, x0, y0, x1, yb, key == edit ? WHITE : GREY);
        line_fast(r, x0 + FP(4), yb - FP(3), x1 - FP(4), yb - FP(3), fd_class_colour[cls]);
    }
}

static void draw_fmdrum(int slot)
{
    void *r = te_raster();
    char buf[24], *p;
    slot &= 7;
    int edit = te_drum_edit;
    edit = edit < 0 ? 0 : edit >= FD_KEYS ? FD_KEYS - 1 : edit;
    const int16_t *kit = te_drum_params + slot * FD_KEYS * 8, *k = kit + edit * 8;
    fd_frame++;

    /* the voices: per key the loudest level (keys); the youngest voice playing the selected key (the drum) */
    int32_t lit[FD_KEYS];
    for (int i = 0; i < FD_KEYS; i++)
        lit[i] = 0;
    const struct fd_voice *live = 0;
    if (fmdrum_voice)
        for (int v = 0; v < FD_VOICES; v++) {
            const struct fd_voice *vv = &fmdrum_voice[v];
            if (!vv->age || (uint32_t)vv->key >= FD_KEYS)
                continue;
            int32_t a = vv->aenv >> 15;
            lit[vv->key] = a > lit[vv->key] ? a : lit[vv->key];
            if (vv->key == edit && (!live || vv->age < live->age))
                live = vv;
        }
    int32_t amp = live ? live->aenv >> 15 : 0;                      /* Q15 */
    uint32_t age = live ? live->age : 0;

    /* ---- title: the key's class and number ---- */
    int cls = fd_class(k);
    text(r, fd_class_name[cls], FP(8), FP(7), FP(2), fd_class_colour[cls]);
    put_dec(put_str(buf, "KEY "), (uint32_t)edit + 1);
    int len = 0;
    while (fd_class_name[cls][len])
        len++;
    text_fast(r, buf, FP(14 + 12 * len), FP(13), FP(1), GREY);

    /* ---- the drum ---- */
    int32_t u = k[0] + 24320;
    u = u < 0 ? 0 : u > 49152 ? 49152 : u;
    int32_t rxu = 62 - ((u * 43) >> 16);                            /* 62 (14 Hz) .. 30 (3.6 kHz) */
    fp32 rx = FP(rxu), ry = FP((rxu * 10) >> 5);
    fp32 depth = FP(6 + (((fd_pos(k[3]) >> 7) * 16) >> 8));
    fp32 cx = FP(86), hy = FP(72) - (age && age < 12 ? FP(12 - (int32_t)age) >> 2 : 0);   /* the hop */
    fp32 by = FP(72) + depth;
    /* shell: sides, the near half of the bottom rim, lugs (orange: DECAY) */
    line_fast(r, cx - rx, hy, cx - rx, by, GREY);
    line_fast(r, cx + rx, hy, cx + rx, by, GREY);
    fd_ellipse(r, cx, by, rx, ry, 16, 0x08000000u, 0, GREY);
    for (int i = 1; i < 6; i++) {
        uint32_t a = (uint32_t)i * 0x15555555u;                    /* 30 .. 150 degrees: the near side */
        fp32 lx = cx + (((rx >> 5) * cos_t(a)) >> 10), ly = ((ry >> 5) * sin_t(a)) >> 10;
        line_fast(r, lx, hy + ly + FP(3), lx, by + ly - FP(2), ORANGE);
    }
    /* ripples while it sounds */
    if (live && amp > 200)
        for (int j = 0; j < 2; j++) {
            fp32 g = FP((int32_t)((age * 2 + (uint32_t)j * 9) & 15) + 3);
            fd_ellipse(r, cx, hy, rx + g, ry + (g >> 1), 32, 0x08000000u, 0, amp > 5000 ? GREY : DARKBLUE);
        }
    /* head outline (anti-aliased) */
    {
        fp32 px = cx + rx, py = hy;
        for (int i = 1; i <= 24; i++) {
            uint32_t a = (uint32_t)i * 0x0AAAAAABu;
            fp32 qx = cx + (((rx >> 5) * cos_t(a)) >> 10), qy = hy + (((ry >> 5) * sin_t(a)) >> 10);
            line_fp(r, px, py, qx, qy, FP(3) >> 1, live ? WHITE : GREY);
            px = qx;
            py = qy;
        }
    }
    /* the FM rosette: radius (fraction of the head) 0.56 + 0.31 depth cos(petals x angle) */
    int ri = fd_pos(k[1]) >> 10, harm = (FD_RATIO_HARMONIC >> ri) & 1;
    int32_t ipk = fd_index_q8(k[2]), iq8 = live ? live->index_q8 : ipk;
    int32_t dq = iq8 >= 2048 ? 32767 : iq8 << 4;                    /* depth: full at 8 rad */
    {
        uint32_t n_q16 = fd_ratio[ri] << 1;                         /* petals = 2 x ratio */
        int segs = harm ? 64 : 128;                                 /* inharmonic: two turns */
        uint32_t dps = (0x04000000u >> 8) * (n_q16 >> 8);           /* petals x 1/64 turn, mod 2^32 */
        uint32_t ph0 = live ? fd_frame * 0x01000000u : 0x40000000u;
        int32_t amp_q15 = (10158 * dq) >> 15;
        int col = live ? WHITE : ipk ? GREEN : DARKGREEN;
        fp32 px = 0, py = 0;
        for (int i = 0; i <= segs; i++) {
            uint32_t th = 0x04000000u * (uint32_t)i, ps = dps * (uint32_t)i + ph0;
            int32_t ru = 18350 + ((amp_q15 * cos_t(ps)) >> 15);    /* Q15 */
            int32_t c = (ru * cos_t(th)) >> 15, s = (ru * sin_t(th)) >> 15;
            fp32 x = cx + (((rx >> 5) * c) >> 10), y = hy + (((ry >> 5) * s) >> 10);
            if (i)
                line_fast(r, px, py, x, y, col);
            px = x;
            py = y;
        }
    }
    /* the sweep: a blue curve falling onto the head, its bend from SNAP (the sweep envelope over ~160 ms) */
    int32_t sq = fd_sweep_q(k[4]);
    fp32 head_top = hy - ry;
    if (sq > 0) {
        fp32 room = head_top - FP(26);
        fp32 h = room > 0 ? (fp32)((((room >> 5) * (sq >> 3)) * 341) >> 15) : 0;   /* sq / 24576 of the room above */
        int32_t f = fd_swp_dec[fd_pos(k[5]) >> 7];                  /* per block, Q30 */
        int32_t f2 = (f >> 15) * (f >> 15), f4 = (f2 >> 15) * (f2 >> 15), f8 = (f4 >> 15) * (f4 >> 15);
        int32_t fs = (((f8 >> 15) * (f2 >> 15)) >> 15) * (f >> 15);      /* f^11 per segment */
        fp32 x0 = cx - ((rx * 7) >> 3), dx = (((rx * 7) >> 3) * 6554) >> 16;   /* 10 segments to the centre */
        int32_t e = 1 << 30, se = live ? live->senv : -1;
        fp32 px = x0, py = head_top - h, dotx = -1, doty = 0;
        for (int j = 1; j <= 10; j++) {
            int32_t e1 = (e >> 15) * (fs >> 15);
            fp32 x = x0 + dx * j, y = head_top - (fp32)(((h >> 5) * (e1 >> 15)) >> 10);
            line_fp(r, px, py, x, y, FP(3) >> 1, BLUE);
            if (dotx < 0 && se >= e1 && se >= 0) {
                dotx = px;
                doty = py;
            }
            px = x;
            py = y;
            e = e1;
        }
        line_fp(r, px, py, px - FP(3), py - FP(4), FP(3) >> 1, BLUE);   /* arrow head */
        line_fp(r, px, py, px + FP(1), py - FP(5), FP(3) >> 1, BLUE);
        if (dotx >= 0)
            line_fp(r, dotx, doty - FP(1), dotx, doty + FP(1), FP(4), LIGHTBLUE);
    }
    /* noise: speckle above the head, or clap marks */
    int32_t k6 = fd_pos(k[6]);
    int zone = fd_noise_zone_of(k[6]);
    if (zone) {
        int32_t nl = fd_noise[(k6 >> 9) * FD_NOISE_W + 1];         /* noise level, Q15 */
        int n = (nl * 40) >> 15;
        if (live)
            n = (n * (amp > 16384 ? 32767 : amp << 1)) >> 15;
        fp32 rise = zone == 1 ? FP(7) : zone == 2 ? FP(14) : FP(24);
        int c0 = zone == 1 ? DARKBLUE : zone == 2 ? GREY : zone == 3 ? WHITE : ORANGE;
        int c1 = zone == 1 ? GREY : zone == 2 ? WHITE : zone == 3 ? LIGHTGREEN : LIGHTORANGE;
        uint32_t seed = live ? fd_frame : 7;
        fp32 spread = live ? FP(1) + (fp32)(age > 16 ? FP(1) : (fp32)age << 6) : FP(1);
        for (int i = 0; i < n; i++) {
            uint32_t hsh = fd_hash((uint32_t)i * 2654435761u + seed);
            int32_t sx = (int32_t)(hsh & 0xFFFF) - 32768, sy = (int32_t)((hsh >> 16) & 0x7FFF);
            fp32 x = cx + (fp32)((((((rx * 3) >> 2) >> 5) * sx) >> 10) * (spread >> 5) >> 5);   /* within 0.75 rx */
            fp32 y = head_top - FP(2) - (fp32)(((rise >> 5) * sy) >> 10);
            if (y < FP(26))
                y = FP(26);
            line_fast(r, x, y, x + FP(1), y, (hsh >> 13) & 1 ? c0 : c1);
        }
        if (zone == 4) {                                            /* clap: three marks each side */
            int col = live ? WHITE : ORANGE;
            for (int j = 0; j < 3; j++) {
                fp32 dy = FP(5 * j);
                line_fp(r, cx - rx - FP(4), hy - FP(10) + dy, cx - rx - FP(11), hy - FP(14) + (dy << 1) - FP(2), FP(3) >> 1, col);
                line_fp(r, cx + rx + FP(4), hy - FP(10) + dy, cx + rx + FP(11), hy - FP(14) + (dy << 1) - FP(2), FP(3) >> 1, col);
            }
        }
    }

    /* ---- the graph ---- */
    line_fast(r, FP(FD_GX0), FP(FD_GY1), FP(FD_GX0 + FD_GW), FP(FD_GY1), DARKBLUE);
    line_fast(r, FP(FD_GX0), FP(FD_GY0), FP(FD_GX0), FP(FD_GY1), DARKBLUE);
    static const int16_t tick_q8[3] = { 2852, 6254, 9656 };        /* 10 ms, 0.1 s, 1 s: 4 log2(t / 1 block), Q8 */
    static const char *const tick_name[3] = { "10MS", "100MS", "1 SEC" };
    for (int i = 0; i < 3; i++) {
        fp32 x = FD_GX(tick_q8[i]);
        vline_fast(r, x, FP(FD_GY1 - 3), FP(FD_GY1), GREY);
        text_fast(r, tick_name[i], x - FP(i == 0 ? 9 : 11), FP(FD_GY1 + 3), FP(3) >> 2, GREY);
    }
    if (sq > 0)                                                    /* sweep: from its depth (48 st = top) */
        fd_curve(r, (fd_log2_q8((uint32_t)sq) - 3734) * 1541, fd_db_blk[1][fd_pos(k[5]) >> 7], BLUE);
    if (ipk > 0)                                                   /* FM index: from its depth (16 rad = top) */
        fd_curve(r, (fd_log2_q8((uint32_t)ipk) - 3072) * 1541, fd_db_blk[2][fd_pos(k[7]) >> 7], WHITE);
    fd_curve(r, 0, fd_db_blk[0][fd_pos(k[3]) >> 7], ORANGE);
    if (live)
        vline_fast(r, FD_GX(fd_log2_q8(age) << 2), FP(FD_GY0), FP(FD_GY1), GREY);

    /* ---- values: the four knobs, and SHIFT's four below ---- */
    uint32_t hz = fd_hz(k[0]), kh = 0;
    if (hz < 1000)
        put_str(put_dec(buf, hz), "HZ");
    else {
        while (hz >= 1000) {                    /* at most 3 steps (3.6 kHz): no division */
            hz -= 1000;
            kh++;
        }
        put_str(put_dec(put_str(put_dec(buf, kh), "."), (hz * 655) >> 16), "K");
    }
    text_fast(r, buf, FP(172), FP(96), FP(1), BLUE);
    uint32_t fr = fd_ratio_x100[ri], ip = 0, tens = 0;
    while (fr >= 100) {
        fr -= 100;
        ip++;
    }
    while (fr >= 10) {
        fr -= 10;
        tens++;
    }
    p = put_str(put_dec(buf, ip), ".");
    put_dec(put_dec(p, tens), fr);
    text_fast(r, buf, FP(212), FP(96), FP(1), GREEN);
    put_dec(put_str(put_dec(buf, (uint32_t)ipk >> 8), "."), (((uint32_t)ipk & 255) * 10) >> 8);
    text_fast(r, buf, FP(246), FP(96), FP(1), WHITE);
    fd_put_time(buf, fd_time_10us[0][fd_pos(k[3]) >> 7]);
    text_fast(r, buf, FP(280), FP(96), FP(1), ORANGE);
    put_str(put_dec(put_str(buf, "+"), ((uint32_t)sq + 256) >> 9), " ST");
    text_fast(r, buf, FP(172), FP(107), FP(1), LIGHTBLUE);
    fd_put_time(buf, fd_time_10us[1][fd_pos(k[5]) >> 7]);
    text_fast(r, buf, FP(212), FP(107), FP(1), LIGHTGREEN);
    text_fast(r, fd_noise_zone[zone], FP(246), FP(107), FP(1), GREY);
    fd_put_time(buf, fd_time_10us[2][fd_pos(k[7]) >> 7]);
    text_fast(r, buf, FP(280), FP(107), FP(1), LIGHTORANGE);

    /* ---- the keys ---- */
    fd_keys(r, kit, edit, lit);
}

#endif
