/* The metal engine's screen (src/slot/metal.c, notes/drum-engine.md "metal"), included by src/slot/ui.c after
 * ui_fmdrum.h (it reuses fd_value's PITCH, fd_put_time, fd_hash, fd_ellipse, fd_curve, fd_key_box and the graph's
 * geometry); drawn on the dbox screen the drum engines borrow (slot.S _drum_ui_update -> drum_ui_draw).
 *
 * The selected key (the last one pressed: the one the knobs edit) as a struck object, its modes and envelopes, over
 * the 24 keys:
 *   title    white: what the key's values make it (KICK, TOM, SNARE, HAT, CRASH, GONG, BELL, ANVIL, CHAIN, GRIND,
 *            SCREAM, CLANG) and its number; after a knob turn, for ~1.5 s, the knob's name and value in its colour
 *   object   grey and white, its outline morphing with MATERIAL (BAR, PLATE, BELL, PIPE, SHEET), smaller for a higher
 *            PITCH; the striker by STRIKE (a big beater for BOOM, a mallet, a hammer, a rasp for SCRAPE, chain links
 *            for RATTLE), touching while the key plays; DRIVE bends the outline (FOLD ripples it, CLIP squares it off,
 *            SCREAM tears it and throws sparks); GRIT snaps it to a grid (CRUSH) or to columns (ALIAS). While the key
 *            plays the outline rings and the sparks fly.
 *   modes    the 8 modes as a green comb under the object (MATERIAL and SPREAD are green knobs): x = the mode's ratio
 *            (log, 0 .. 5 octaves), height = its level (live while it rings)
 *   graph    level against log time: DECAY (orange, the lowest mode's ring), NDECAY (light orange, the exciter)
 *   values   grey labels over the values (PITCH MATERIAL DRIVE DECAY), a dimmer SHIFT row (STRIKE SPREAD GRIT NDECAY)
 *   keys     the 24 keys with each key's class as a mark (colours no knob uses), lit while it sounds, the selected
 *            one outlined
 * Reads the slot's per-key values (TE's 0x01AAADB4) and metal's voices (metal.h). No division; knob colours only on
 * knob values.
 */
#ifndef UI_METAL_H
#define UI_METAL_H
#include "metal.h"
#include "metal_tables.h"

extern struct mt_voice metal_voice[] __attribute__((weak));

#define MT_SHOW 45                              /* frames a turned knob's readout stays in the title (~1.5 s) */
#define MT_PALE 17                              /* TE's palette 17 (230, 231, 255) */
/* classes: the key's role (SCREAM is no role: a screaming key keeps its role, its key mark turns pink) */
enum { MC_KICK, MC_TOM, MC_SNARE, MC_RIM, MC_CLAP, MC_HAT, MC_CRASH, MC_GONG, MC_BELL, MC_PING, MC_ANVIL, MC_CLANG,
       MC_CHAIN, MC_GRIND };
static const char *const mt_class_name[14] = { "KICK", "TOM", "SNARE", "RIM", "CLAP", "HAT", "CRASH", "GONG", "BELL",
                                               "PING", "ANVIL", "CLANG", "CHAIN", "GRIND" };
/* the key strip's marks by family, a shape each (colours no knob uses): drums a double line, metals a short tick,
 * noise a zigzag */
enum { MF_DRUM, MF_METAL, MF_NOISE };
static const uint8_t mt_class_family[14] = { MF_DRUM, MF_DRUM, MF_DRUM, MF_DRUM, MF_DRUM, MF_METAL, MF_METAL, MF_METAL,
                                             MF_METAL, MF_METAL, MF_METAL, MF_METAL, MF_NOISE, MF_NOISE };
static const uint8_t mt_family_colour[3] = { PURPLEGREY, MT_PALE, GREY };
static const uint8_t mt_shift_colour[4] = { LIGHTBLUE, LIGHTGREEN, MT_PALE, LIGHTORANGE };
static const uint8_t mt_title_colour[8] = { BLUE, GREEN, WHITE, ORANGE, LIGHTBLUE, LIGHTGREEN, MT_PALE, LIGHTORANGE };
static const char *const mt_knob_name[8] = { "PITCH", "MATERIAL", "DRIVE", "DECAY", "STRIKE", "SPREAD", "GRIT",
                                             "NDECAY" };
static const char *const mt_mat_short[5] = { "BA", "PL", "BE", "PI", "SH" };
static const char *const mt_strike_short[5] = { "BOOM", "MALL", "HAMR", "SCRP", "RATL" };
static const char *const mt_drive_short[4] = { "CLEAN", "FD", "CL", "SC" };

struct mt_view {
    uint32_t frame;
    int32_t edit;
    int16_t last[8];
    int32_t shown, until;
};
static struct mt_view mt_v RAM = { 0, -1, { 0 }, 0, 0 };

/* the zone a knob value plays (one detent ahead, metal.h mt_sound), and the detent's level inside it: the zone's first
 * detent is 1 (a value between detents shows the lower detent) */
#define MT_SZ(v) mt_zone(mt_strike_edge, 5, mt_strike_pos(v))
#define MT_DZ(v) mt_zone(mt_drive_edge, 4, mt_drive_pos(v))
#define MT_GZ(v) mt_zone(mt_grit_edge, 3, mt_grit_pos(v))
static int mt_level(const uint8_t *edge, int z, int16_t v)
{
    int lv = (int)((mt_pos(v) + 512) >> 10) - edge[z] + 1;             /* (rounded: 32767 is detent 32) */
    return lv < 1 ? 1 : lv;
}

/* what a key's values make it: its role (DECAY: index 65 ~ 0.15 s, 93 ~ 0.3 s, 105 ~ 0.4 s, 142 ~ 1 s, 170 ~ 2 s;
 * PITCH: -9300 ~ 80 Hz, 2300 ~ 300 Hz, 9000 ~ 610 Hz, 12300 ~ 900 Hz) */
static int mt_class(const int16_t *k)
{
    int sz = MT_SZ(k[MT_STRIKE]), dz = MT_DZ(k[MT_DRIVE]);
    int32_t dk = mt_pos(k[MT_DECAY]) >> 7, nk = mt_pos(k[MT_NDECAY]) >> 7, mat = mt_pos(k[MT_MATERIAL]);
    int32_t pitch = k[MT_PITCH];
    int sheet_plate = mat >= 28672 || (mat >= 4096 && mat < 12288);
    if (sz == SZ_RATTLE)
        return dk < 93 ? MC_CLAP : MC_CHAIN;
    if (sz == SZ_SCRAPE && dz >= DZ_CLIP && nk >= 176)       /* a long scrape, driven: a grinder */
        return MC_GRIND;
    if (pitch < -9300)
        return dk >= 200 ? MC_GONG : MC_KICK;
    if (sz == SZ_SCRAPE && dk < 160 && pitch < 9000)
        return MC_SNARE;
    if (pitch >= 12300 && dk < 65 && sz == SZ_HAMMER && !sheet_plate)
        return MC_RIM;
    if (pitch >= 12300 && dk < 190 && sheet_plate)
        return MC_HAT;
    if (dk >= 190 && pitch < 2300)
        return MC_GONG;
    if (mat >= 12288 && mat < 20480 && dk >= 160)
        return MC_BELL;
    if (dk >= 175 && sheet_plate && sz >= SZ_HAMMER)
        return MC_CRASH;
    if (pitch >= 9000 && dk >= 105)
        return MC_PING;
    if (pitch < 2300)
        return MC_TOM;
    if (mat < 4096 && sz == SZ_HAMMER)
        return MC_ANVIL;
    return MC_CLANG;
}

static char *mt_two(char *p, uint32_t h)        /* h 0 .. 99 as two digits (no division) */
{
    uint32_t t = (h * 205) >> 11;
    *p++ = (char)('0' + t);
    *p++ = (char)('0' + h - 10 * t);
    *p = 0;
    return p;
}

/* knob i's value as text: short for the value row (at most 6 characters), long for the title */
static char *mt_value(char *p, int i, const int16_t *k, int full)
{
    int16_t v = k[i];
    if (i == MT_PITCH)
        return fd_value(p, 0, k, 0);            /* fmdrum's PITCH: the same K0 scale, 220HZ / 1.2K */
    if (i == MT_MATERIAL) {
        int32_t km = mt_pos(v);
        int mi = km >> 13 > 3 ? 3 : km >> 13;
        int32_t f = km - (mi << 13);            /* 0 .. 8191 toward the next material */
        if (f < 128 || f > 8063)
            return put_str(p, mt_mat_name[f > 8063 ? mi + 1 : mi]);
        if (!full)
            return put_str(put_str(put_str(p, mt_mat_short[mi]), ">"), mt_mat_short[mi + 1]);
        p = put_str(put_str(put_str(p, mt_mat_name[mi]), ">"), mt_mat_name[mi + 1]);
        return put_str(put_dec(put_str(p, " "), (uint32_t)((f * 100 + 4096) >> 13)), "%");
    }
    if (i == MT_DRIVE) {
        int z = MT_DZ(v);
        if (z == DZ_CLEAN)
            return put_str(p, "CLEAN");
        p = put_str(p, full ? mt_drive_zone[z] : mt_drive_short[z]);
        return put_dec(full ? put_str(p, " ") : p, (uint32_t)mt_level(mt_drive_edge, z, v));
    }
    if (i == MT_STRIKE) {
        int z = MT_SZ(v);
        p = put_str(p, full ? mt_strike_zone[z] : mt_strike_short[z]);
        p = put_dec(full ? put_str(p, " ") : p, (uint32_t)mt_level(mt_strike_edge, z, v));
        if (full && z == SZ_BOOM) {             /* the pitch drop */
            int32_t b = mt_strike[(mt_strike_pos(v) >> 8) * MT_STRIKE_W];
            p = put_str(put_dec(put_str(p, " +"), (uint32_t)((b + 256) >> 9)), "ST");
        }
        return p;
    }
    if (i == MT_SPREAD) {                       /* the stretch: X1.00 at NATURAL */
        uint32_t h = ((uint32_t)mt_spread[(mt_pos(v) >> 8) * 2] * 100 + 2048) >> 12, ip = 0;
        int32_t d = mt_pos(v) >> 10;
        if (full)
            p = put_str(p, d < 16 ? "TIGHT " : d == 16 ? "NATURAL " : "WIDE ");
        while (h >= 100) {
            h -= 100;
            ip++;
        }
        return mt_two(put_str(put_dec(put_str(p, "X"), ip), "."), h);
    }
    if (i == MT_GRIT) {
        int z = MT_GZ(v);
        int32_t g = mt_grit_pos(v) >> 8;
        if (z == GZ_CLEAN)
            return put_str(p, "CLEAN");
        if (full)
            p = put_str(put_str(p, mt_grit_zone[z]), " ");
        if (z == GZ_CRUSH)
            return put_str(put_dec(p, (uint32_t)((mt_grit_bits[g] + 128) >> 8)), "BIT");
        uint32_t hz = ((mt_grit_hold[g] >> 16) * 1378u) >> 10;   /* the hold's rate: inc / 2^32 x 88200 */
        if (!full)
            p = put_str(p, "AL");
        if (hz < 1000)
            return full ? put_str(put_dec(p, hz), "HZ") : put_dec(p, hz);
        uint32_t kh = 0;
        while (hz >= 1000) {
            hz -= 1000;
            kh++;
        }
        p = put_dec(p, kh);
        if (kh < 10 && full)
            p = put_dec(put_str(p, "."), (hz * 655) >> 16);
        return put_str(p, full ? "KHZ" : "K");
    }
    return fd_put_time(p, mt_time_10us[i == MT_DECAY ? 0 : 1][mt_pos(v) >> 7]);
}

/* the object's 40 points (outline 0..23, details 24..31 and 32..39): MATERIAL's two shapes mixed, scaled by PITCH,
 * bent by DRIVE, rung by the live voice, snapped by GRIT */
struct mt_obj {
    fp32 x[40], y[40];
    fp32 tx, ty;                                /* the outline's top point: where the striker lands */
};

static void mt_object(struct mt_obj *o, const int16_t *k, int32_t ring, uint32_t seed, uint32_t frame)
{
    int32_t km = mt_pos(k[MT_MATERIAL]);
    int mi = km >> 13 > 3 ? 3 : km >> 13;
    int32_t f = (km - (mi << 13)) >> 5;         /* Q8 toward the next material */
    int32_t u = k[MT_PITCH] + 24320;
    u = u < 0 ? 0 : u > 49152 ? 49152 : u;
    int32_t sc = 20316 - ((u * 5) >> 5);        /* Q15: 0.62 (14 Hz) .. 0.38 (3.5 kHz): every object fits 60 px */
    int dz = MT_DZ(k[MT_DRIVE]), gz = MT_GZ(k[MT_GRIT]);
    int32_t dt = mt_zone_t(mt_drive_edge, 4, mt_drive_pos(k[MT_DRIVE]));
    int32_t fold = dz == DZ_FOLD ? dt : dz == DZ_CLIP ? 32767 - (dt >> 1) : dz == DZ_SCREAM ? 16383 : 0;
    int32_t clip = dz == DZ_CLIP ? dt : dz == DZ_SCREAM ? 32767 - (dt >> 1) : 0;
    int32_t tear = dz == DZ_SCREAM ? 8192 + ((dt * 3) >> 2) : 0;
    int32_t gmask = -1, xmask = -1;             /* GRIT: CRUSH snaps both axes (1 .. 8 px), ALIAS the columns */
    if (gz == GZ_CRUSH) {
        int32_t b = mt_grit_bits[mt_grit_pos(k[MT_GRIT]) >> 8] >> 8;  /* 14 .. 3 */
        gmask = -(1 << (8 + (b >= 12 ? 0 : b >= 9 ? 1 : b >= 6 ? 2 : 3)));
    } else if (gz == GZ_ALIAS) {
        gmask = -(1 << 11);
        xmask = -(1 << (11 + (mt_zone_t(mt_grit_edge, 3, mt_grit_pos(k[MT_GRIT])) >> 14)));
    }
    int32_t px[40], py[40], maxy = 1;
    for (int i = 0; i < 40; i++) {              /* the shape, mixed and scaled (Q8 px) */
        int32_t x = (mt_shape[mi][i][0] << 8) + (mt_shape[mi + 1][i][0] - mt_shape[mi][i][0]) * f;
        int32_t y = (mt_shape[mi][i][1] << 8) + (mt_shape[mi + 1][i][1] - mt_shape[mi][i][1]) * f;
        px[i] = (x * (sc >> 3)) >> 12;
        py[i] = (y * (sc >> 3)) >> 12;
        int32_t ay = py[i] < 0 ? -py[i] : py[i];
        maxy = i < 24 && ay > maxy ? ay : maxy;
    }
    int32_t lim = maxy - ((maxy * ((clip * 115) >> 15)) >> 8);     /* CLIP: |y| down to 0.55 of the object's */
    int32_t ymin = 1 << 20, ymax = -(1 << 20);
    for (int i = 0; i < 40; i++) {
        int32_t x = px[i], y = py[i];
        if (i < 24) {
            if (fold) {                          /* ripples, more and deeper up the zone */
                uint32_t a = (uint32_t)i * (0x0AAAAAABu * (uint32_t)(3 + (fold >> 13))) + (frame << 26);
                int32_t d = (sin_t(a) * (1 + (fold >> 12))) >> 15;        /* -9 .. 9 (x 1/64 of the radius) */
                x += (x * d) >> 6;
                y += (y * d) >> 6;
            }
            if (clip)                            /* squared off */
                y = y > lim ? lim : y < -lim ? -lim : y;
            if (tear) {                          /* torn: a jag per point */
                int32_t h = (int32_t)(fd_hash((uint32_t)i * 0x27D4EB2Fu + seed) >> 20) - 2048;
                int32_t d = (h * (tear >> 10)) >> 6;                         /* Q8, up to ~ +-1000 (4 px) */
                x += d;
                y += d >> 1;
            }
            if (ring > 400)                      /* rung by the modes */
                y += (((sin_t((uint32_t)i * 0x15555555u + frame * 0x0D000000u) * (ring >> 4)) >> 15) * 3) >> 3;
        }
        px[i] = x & gmask & xmask;
        py[i] = y & gmask;
        ymin = py[i] < ymin ? py[i] : ymin;
        ymax = py[i] > ymax ? py[i] : ymax;
    }
    /* inside y 40 .. 100 (centre 70): clear of the title, the striker above it, the comb below */
    int32_t shift = ymin < -(30 << 8) ? -(30 << 8) - ymin : ymax > (30 << 8) ? (30 << 8) - ymax : 0;
    int32_t ty = 1 << 20;
    o->tx = FP(84);
    o->ty = FP(40);
    for (int i = 0; i < 40; i++) {
        o->x[i] = FP(84) + (px[i] << 2);
        o->y[i] = FP(70) + ((py[i] + shift) << 2);
        if (i < 24 && py[i] < ty) {
            ty = py[i];
            o->tx = o->x[i];
            o->ty = o->y[i];
        }
    }
}

/* the striker, by STRIKE, landing on the outline's top while the key plays (else lifted) */
static void mt_striker(void *r, const int16_t *k, const struct mt_obj *o, int hit, uint32_t frame)
{
    int z = MT_SZ(k[MT_STRIKE]);
    int32_t t = mt_zone_t(mt_strike_edge, 5, mt_strike_pos(k[MT_STRIKE]));
    fp32 lift = hit ? 0 : FP(7);
    fp32 hx = o->tx + FP(6) + lift, hy = o->ty - FP(6) - lift;          /* the head's centre */
    hy = hy < FP(30) ? FP(30) : hy;                                       /* (clear of the title) */
    fp32 hend = hy - FP(20) < FP(24) ? FP(24) : hy - FP(20);
    int col = hit ? WHITE : GREY;
    if (z == SZ_SCRAPE) {                        /* a rasp along the top edge: a bar with teeth */
        fp32 x0 = o->tx - FP(18), x1 = o->tx + FP(18), y0 = o->ty - FP(3) - (lift >> 1);
        line_fp(r, x0, y0 - FP(2), x1, y0 - FP(6), FP(2), col);
        for (int i = 0; i < 7; i++) {
            fp32 x = x0 + FP(5) * i + (hit ? (fp32)((frame * 3 + i) & 3) << 9 : 0);
            line_fast(r, x, y0 - FP(2) - ((FP(4) * i) >> 3), x + FP(2), y0 + FP(1) - ((FP(4) * i) >> 3), GREY);
        }
        line_fp(r, x1, y0 - FP(6), x1 + FP(16), y0 - FP(16), FP(3) >> 1, GREY);   /* the handle */
        return;
    }
    if (z == SZ_RATTLE) {                        /* a chain lying along the top, its end hanging off; shaking while it
                                                 * plays */
        for (int i = 0; i < 8; i++) {
            uint32_t h = fd_hash((uint32_t)i * 0x9E3779B1u + (hit ? frame : 0));
            fp32 j = hit ? (fp32)((int32_t)(h & 7) - 3) << 8 : 0;
            fp32 x = i < 6 ? o->tx - FP(15) + FP(6) * i : o->tx + FP(17) + (i == 7 ? FP(1) : 0);
            fp32 y = i < 6 ? o->ty - FP(3) - (lift >> 1) : o->ty + FP(3) + FP(6) * (i - 6);
            int across = (i & 1) ^ (i >= 6);
            fd_ellipse(r, x + j, y + (j >> 1), across ? FP(2) : FP(4), across ? FP(4) : FP(2), 8, 0x20000000u, 0, col);
        }
        return;
    }
    /* a beater: the handle up and to the right, the head by zone */
    line_fp(r, hx, hy, hx + FP(22), hend, FP(3) >> 1, GREY);
    if (z == SZ_HAMMER) {                        /* a hammer head, longer up the zone */
        fp32 w = FP(5) + ((FP(5) >> 5) * (t >> 10));
        line_fp(r, hx - w, hy - FP(3) + (w >> 2), hx + w, hy + FP(3) - (w >> 2), FP(5), col);
    } else {                                     /* BOOM: a big soft beater, shrinking into MALLET's */
        int32_t rad = z == SZ_BOOM ? 10 - ((t * 3) >> 15) : 7 - ((t * 4) >> 15);
        fd_ellipse(r, hx, hy, FP(rad), FP(rad), 16, 0x10000000u, 0, col);
        if (z == SZ_BOOM)
            fd_ellipse(r, hx, hy, FP(rad - 3), FP(rad - 3), 12, 0x15555555u, 0, PURPLEGREY);
    }
}

/* the modes as a comb: x by ratio (0 .. 5 octaves), height by level; the same ratios as metal.c */
static void mt_comb(void *r, const int16_t *k, const struct mt_voice *live)
{
    int32_t km = mt_pos(k[MT_MATERIAL]), kp = mt_pos(k[MT_SPREAD]) >> 8;
    int mi = km >> 13 > 3 ? 3 : km >> 13;
    int32_t mf = km - (mi << 13);
    int32_t stretch = mt_spread[kp * 2], dt = mt_spread[kp * 2 + 1];
    int32_t ks = mt_strike_pos(k[MT_STRIKE]) >> 8;
    int32_t fcq = mt_strike[ks * MT_STRIKE_W + 1] * 2, slope = mt_strike[ks * MT_STRIKE_W + 2];   /* (as metal.c) */
    const fp32 y0 = FP(115), x0 = FP(12);
    line_fast(r, x0, y0, x0 + FP(148), y0, DARKBLUE);
    for (int o = 1; o <= 5; o++)                /* octave ticks */
        vline_fast(r, x0 + FP(29 * o + (o >> 1)), y0, y0 + FP(2), DARKBLUE);
    for (int j = 0; j < 8; j++) {
        int32_t l = mt_mat_l[mi][j] + (((mt_mat_l[mi + 1][j] - mt_mat_l[mi][j]) * mf) >> 13);
        l = ((l * stretch) >> 12) + ((mt_detune[j] * dt) >> 15);       /* K0 units, 6144 per octave */
        int32_t a = mt_mat_a[mi][j] + (((mt_mat_a[mi + 1][j] - mt_mat_a[mi][j]) * mf) >> 13);
        int32_t qk = k[MT_PITCH] + l;
        if (qk > fcq) {                          /* the strike's brightness: `slope` octaves of level per octave */
            uint32_t u = (((((uint32_t)(qk - fcq) * 683u) >> 10) * (uint32_t)slope) >> 8);   /* log2, Q16 */
            a = u >= (15u << 16) ? 0 : ((a * (mt_exp2n[(u >> 8) & 255] >> 15)) >> 15) >> (u >> 16);
        }
        int32_t boost = mt_strike[ks * MT_STRIKE_W + 13];                    /* HAMMER's tilt up (roughly) */
        if (boost && l > 0) {
            a += (a * ((l * boost) >> 12)) >> 13;
            a = a > 32767 ? 32767 : a;
        }
        if (live && live->wk[0] | live->wk[1]) {                       /* live: the voice's level */
            a = (live->wk[j] * (live->env[j] >> 15)) >> 13;
            a = a > 32767 ? 32767 : a;
        }
        if (l > 5 * 6144)
            continue;
        fp32 x = x0 + (fp32)(((l < 0 ? 0 : l) * 631) >> 7);              /* 148 px (Q10) for 5 octaves of K0 */
        fp32 h = FP(1) + (fp32)((a * 13) >> 5);                          /* up to 14 px */
        vline_fast(r, x, y0 - h, y0, GREEN);
        if (a > 2000)
            vline_fast(r, x + FP(1), y0 - h, y0, GREEN);
    }
}

/* a key's mark (its class's family: drums a double line, metals a short tick, noise a zigzag), pink when it screams */
static void mt_mark(void *r, fp32 x0, fp32 x1, fp32 y, const int16_t *k)
{
    int cls = mt_class(k), fam = mt_class_family[cls];
    int col = MT_DZ(k[MT_DRIVE]) == DZ_SCREAM ? FD_PINK : mt_family_colour[fam];   /* (pink: screaming) */
    fp32 xm = (x0 + x1) >> 1;
    if (fam == MF_DRUM) {
        line_fast(r, x0, y, x1, y, col);
        line_fast(r, x0, y + FP(1), x1, y + FP(1), col);
    } else if (fam == MF_METAL) {
        line_fast(r, xm - FP(3), y, xm + FP(3), y, col);
        vline_fast(r, xm, y - FP(2), y + FP(1), col);
    } else {
        for (int j = 0; j < 3; j++)
            line_fast(r, xm - FP(4) + FP(3) * j, j & 1 ? y + FP(1) : y - FP(1), xm - FP(1) + FP(3) * j,
                      j & 1 ? y - FP(1) : y + FP(1), col);
    }
}

/* the OP-1's keys with metal's classes (as fd_keys) */
static void mt_keys(void *r, const int16_t *kit, int edit, const int32_t *lit)
{
    const fp32 y0 = FP(121), y1 = FP(140), yb = FP(132);
    line_fast(r, FP(13), y0, FP(13 + 21 * 14), y0, DARKBLUE);
    line_fast(r, FP(13), y1, FP(13 + 21 * 14), y1, DARKBLUE);
    for (int w = 0, pos = 0; w <= 14; w++, pos = pos == 6 ? 0 : pos + 1) {
        int black = w > 0 && w < 14 && pos != 0 && pos != 4;
        vline_fast(r, FP(13 + 21 * w), black ? yb : y0, y1, DARKBLUE);
    }
    for (int w = 0; w < 14; w++) {
        int key = fd_white_key[w];
        int lc = mt_family_colour[mt_class_family[mt_class(kit + key * 8)]];
        fp32 x0 = FP(13 + 21 * w), x1 = x0 + FP(21);
        if (lit[key] > 200) {
            fp32 top = y1 - (fp32)((lit[key] * 19) >> 5);
            for (fp32 x = x0 + FP(2); x <= x1 - FP(2); x += FP(1))
                vline_fast(r, x, top, y1 - FP(1), lc);
        }
        if (key == edit)
            fd_key_box(r, x0, y0, x1, y1, WHITE);
        mt_mark(r, x0 + FP(7), x1 - FP(7), FP(136), kit + key * 8);
    }
    for (int w = 0, pos = 0; w < 13; w++, pos = pos == 6 ? 0 : pos + 1) {
        if (pos == 3 || pos == 6)
            continue;
        int key = fd_white_key[w] + 1;
        int lc = mt_family_colour[mt_class_family[mt_class(kit + key * 8)]];
        fp32 x0 = FP(13 + 21 * w + 15), x1 = x0 + FP(12);
        if (lit[key] > 200) {
            fp32 top = yb - (fp32)((lit[key] * 11) >> 5);
            for (fp32 x = x0 + FP(2); x <= x1 - FP(2); x += FP(1))
                vline_fast(r, x, top, yb - FP(1), lc);
        }
        fd_key_box(r, x0, y0, x1, yb, key == edit ? WHITE : GREY);
        mt_mark(r, x0 + FP(3), x1 - FP(3), yb - FP(3), kit + key * 8);
    }
}

static void draw_metal(int slot)
{
    void *r = te_raster();
    char buf[32];
    struct mt_view *vw = &mt_v;
    slot &= 7;
    int edit = te_drum_edit;
    edit = edit < 0 ? 0 : edit >= MT_KEYS ? MT_KEYS - 1 : edit;
    const int16_t *kit = te_drum_params + slot * MT_KEYS * 8, *k = kit + edit * 8;
    vw->frame++;
    if (vw->edit == edit) {                     /* a turned knob (a change of key is not a turn) */
        for (int i = 0; i < 8; i++)
            if (k[i] != vw->last[i]) {
                vw->shown = i;
                vw->until = (int32_t)vw->frame + MT_SHOW;
            }
    } else {
        vw->until = 0;
    }
    vw->edit = edit;
    for (int i = 0; i < 8; i++)
        vw->last[i] = k[i];

    /* the voices: per key the loudest level; the youngest voice playing the selected key */
    int32_t lit[MT_KEYS];
    for (int i = 0; i < MT_KEYS; i++)
        lit[i] = 0;
    const struct mt_voice *live = 0;
    if (metal_voice)
        for (int v = 0; v < MT_VOICES; v++) {
            const struct mt_voice *vv = &metal_voice[v];
            if (!vv->age || !vv->aenv || (uint32_t)vv->key >= MT_KEYS)
                continue;
            int32_t a = vv->aenv >> 15;
            lit[vv->key] = a > lit[vv->key] ? a : lit[vv->key];
            if (vv->key == edit && (!live || vv->age < live->age))
                live = vv;
        }
    int32_t amp = live ? live->aenv >> 15 : 0;
    uint32_t age = live ? live->age : 0;

    /* ---- the title ---- */
    if ((int32_t)(vw->until - (int32_t)vw->frame) > 0) {
        char *q = put_str(put_str(buf, mt_knob_name[vw->shown]), " ");
        q = mt_value(q, vw->shown, k, 1);
        fp32 step = q - buf > 20 ? FP(3) >> 1 : FP(2);    /* a long one smaller: it ends before the striker */
        text(r, buf, FP(8), FP(7) + (FP(2) - step) * 3, step, mt_title_colour[vw->shown]);
    } else {
        int cls = mt_class(k), len = 0;
        text(r, mt_class_name[cls], FP(8), FP(7), FP(2), WHITE);
        while (mt_class_name[cls][len])
            len++;
        put_dec(put_str(buf, "KEY "), (uint32_t)edit + 1);
        text_fast(r, buf, FP(14 + 12 * len), FP(13), FP(1), GREY);
    }

    /* ---- the object, the striker, the sparks ---- */
    struct mt_obj o;
    uint32_t seed = live ? vw->frame * 0x9E3779B9u : 0x2468ACE1u;
    mt_object(&o, k, amp, seed, live ? vw->frame : 0);
    int colo = live ? WHITE : GREY;
    for (int i = 0; i < 24; i++) {
        int j = i == 23 ? 0 : i + 1;
        line_fp(r, o.x[i], o.y[i], o.x[j], o.y[j], FP(3) >> 1, colo);
    }
    for (int i = 24; i < 39; i++)
        if (i != 31)
            line_fast(r, o.x[i], o.y[i], o.x[i + 1], o.y[i + 1], live ? GREY : PURPLEGREY);
    int dz = MT_DZ(k[MT_DRIVE]);
    if (dz == DZ_SCREAM || (dz == DZ_CLIP && mt_level(mt_drive_edge, dz, k[MT_DRIVE]) >= 6)) {   /* sparks */
        int n = dz == DZ_SCREAM ? 6 + mt_level(mt_drive_edge, dz, k[MT_DRIVE]) : 3;
        if (live)
            n = (n * (amp > 16384 ? 32767 : amp << 1)) >> 15;
        for (int i = 0; i < n; i++) {
            uint32_t h = fd_hash((uint32_t)i * 0x85EBCA6Bu + seed);
            uint32_t a = 0xA0000000u + ((h & 0xFFFF) << 14);              /* up and to both sides */
            int32_t c = cos_t(a), sn = sin_t(a), len = 4 + (int32_t)((h >> 16) & 7);
            fp32 x0 = o.tx + (fp32)((c * 3) >> 5), y0 = o.ty + (fp32)((sn * 3) >> 5);
            line_fast(r, x0, y0, x0 + (fp32)((c * len) >> 5), y0 + (fp32)((sn * len) >> 5), (h >> 13) & 1 ? WHITE : GREY);
        }
    }
    mt_striker(r, k, &o, live && age < 8, vw->frame);
    mt_comb(r, k, live);

    /* ---- the graph: the ring (orange) and the exciter (light orange) ---- */
    line_fast(r, FP(FD_GX0), FP(FD_GY1), FP(FD_GX0 + FD_GW), FP(FD_GY1), DARKBLUE);
    line_fast(r, FP(FD_GX0), FP(FD_GY0), FP(FD_GX0), FP(FD_GY1), DARKBLUE);
    static const int16_t tick_q8[3] = { 2852, 6254, 9656 };            /* 10 ms, 0.1 s, 1 s */
    for (int i = 0; i < 3; i++)
        vline_fast(r, FD_GX(tick_q8[i]), FP(FD_GY1 - 2 - i), FP(FD_GY1), GREY);
    fd_curve(r, -(6 << 16), mt_db_blk[1][mt_pos(k[MT_NDECAY]) >> 7], LIGHTORANGE);
    fd_curve(r, 0, mt_db_blk[0][mt_pos(k[MT_DECAY]) >> 7], ORANGE);
    if (live)
        vline_fast(r, FD_GX(fd_log2_q8(age) << 2), FP(FD_GY0), FP(FD_GY1), GREY);

    /* ---- values: the plain knobs, then SHIFT's (dimmer) ---- */
    static const uint16_t col_x[4] = { 172, 207, 243, 281 };
    for (int i = 0; i < 4; i++) {
        text_fast(r, mt_knob_name[i], FP(col_x[i]), FP(73), FP(5) >> 3, GREY);
        mt_value(buf, i, k, 0);
        text_fast(r, buf, FP(col_x[i]), FP(80), FP(1), fd_knob_colour[i]);
        text_fast(r, mt_knob_name[4 + i], FP(col_x[i]), FP(97), FP(5) >> 3, DARKBLUE);
        mt_value(buf, 4 + i, k, 0);
        text_fast(r, buf, FP(col_x[i]), FP(104), FP(1), mt_shift_colour[i]);
    }
    text_fast(r, "SHIFT", FP(172), FP(91), FP(3) >> 2, DARKBLUE);
    line_fast(r, FP(198), FP(93), FP(312), FP(93), DARKBLUE);

    mt_keys(r, kit, edit, lit);
}

#endif
