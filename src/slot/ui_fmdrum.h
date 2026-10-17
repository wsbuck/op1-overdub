/* The fmdrum engine's screen (src/slot/fmdrum.c, notes/drum-engine.md "fmdrum"), included by src/slot/ui.c after its
 * drawing helpers; drawn on the dbox screen the drum engines borrow (slot.S _drum_ui_update -> drum_ui_draw).
 *
 * The selected key (the last one pressed: the one the knobs edit) as a drum, with its envelopes, over the 24 keys:
 *   title    white: what the key's 8 values make it (KICK, TOM, SNARE, CLAP, HAT, METAL, BELL, ZAP, PERC) and its
 *            number; after a knob turn, for ~1.5 s, that knob's name and value instead, in the knob's colour (as harm,
 *            ui_harm.h): the 8 values are compared every frame, a change of key is not a turn
 *   drum     an oblique drum in grey and white: low pitch = a big drum, DECAY = a deep shell. On the head the FM: a
 *            rosette of 2 x RATIO petals (harmonic ratios close in one turn, inharmonic ones need two and shimmer),
 *            petal depth = FM. SWEEP is the green curve falling onto the head (its bend = SWDEC); NOISE is white/grey
 *            speckle above the head, low and sparse (AIR), higher (SNARE), high and dense (HAT), or clap marks (CLAP).
 *            While the key plays: the head hops and turns white, the rosette follows the live index (FMDEC) and turns,
 *            a dot rides the sweep, ripples spread with the amplitude and the speckle sprays.
 *   graph    level against log time (1.5 ms .. 6 s; ticks at 10 ms, 0.1 s, 1 s): DECAY (orange), the FM index's decay
 *            (white, starting at the FM depth), the sweep (green, starting at the SWEEP depth); a playhead while the
 *            key sounds
 *   values   grey labels over the knob values: PITCH SWEEP FM DECAY; below, dimmer and marked SHIFT: RATIO (TONE or
 *            METAL) SWDEC FMDEC NOISE. Knob colours are used for knob values only.
 *   keys     the OP-1's 24 keys (F .. E): each key's class colour as a mark (the only class colours), lit while it
 *            sounds, the selected key outlined white
 * Reads the slot's per-key values (TE's 0x01AAADB4) and fmdrum's voices (fmdrum.h). No division; 1-px lines except
 * the title, the head outline and the sweep curve (about 60 anti-aliased segments, as the other engine screens).
 */
#ifndef UI_FMDRUM_H
#define UI_FMDRUM_H
#include "fmdrum.h"
#include "fmdrum_tables.h"

extern struct fd_voice fmdrum_voice[] __attribute__((weak));

#define LIGHTORANGE 19
#define FD_SHOW 45                              /* frames a turned knob's readout stays in the title (~1.5 s) */
#define FD_PINK 0                               /* TE's palette 0 (255, 144, 167) */
#define FD_PURPLE 7                             /* TE's palette 7 (95, 66, 112) */
enum { FC_KICK, FC_TOM, FC_SNARE, FC_CLAP, FC_HAT, FC_METAL, FC_BELL, FC_ZAP, FC_PERC, FC_GRIND, FC_CRASH };
static const char *const fd_class_name[11] = { "KICK", "TOM", "SNARE", "CLAP", "HAT", "METAL", "BELL", "ZAP", "PERC",
                                               "GRIND", "CRASH" };
/* the key strip's marks only: CLAP lavender, ZAP red, GRIND pink, so they read apart */
static const uint8_t fd_class_colour[11] = { BLUE, LIGHTBLUE, WHITE, PURPLEGREY, LIGHTGREEN, GREEN, LIGHTGREEN, RED, GREY,
                                             FD_PINK, FD_PURPLE };
static const char *const fd_knob_name[8] = { "PITCH", "SWEEP", "FM", "DECAY", "RATIO", "SWDEC", "FMDEC", "NOISE" };
static const uint8_t fd_knob_colour[8] = { BLUE, GREEN, WHITE, ORANGE, BLUE, GREEN, WHITE, ORANGE };
static const uint8_t fd_shift_colour[4] = { LIGHTBLUE, LIGHTGREEN, GREY, LIGHTORANGE };

struct fd_view {
    uint32_t frame;
    int32_t edit;                               /* the key the last frame showed (-1: none yet) */
    int16_t last[8];                            /* its values then */
    int32_t shown, until;                       /* the turned knob and the frame its readout ends */
};
static struct fd_view fd_v RAM = { 0, -1, { 0 }, 0, 0 };

/* what a key's values make it */
static int fd_class(const int16_t *k)
{
    int z = fd_noise_zone_of(k[FD_NOISE]);
    int32_t dk = fd_pos(k[FD_DECAY]) >> 7;
    int fz = fd_fm_zone(k[FD_FM]);
    if (z >= NZ_METAL) {                        /* the body first: a kick, a pipe or snare under ~250 Hz */
        if (k[FD_PITCH] < -9000)                /* below ~80 Hz */
            return FC_KICK;
        if (k[FD_PITCH] < 2300)
            return dk >= 150 ? FC_TOM : FC_SNARE;
        if (z == NZ_GRIND)
            return FC_GRIND;
        int32_t t = fd_noise_sound(k[FD_NOISE]) - 20480;   /* the 808's squares: in METAL's upper half under ~1 s a hat,
                                                         * above a crash; lower down metal */
        if (t >= 3072)
            return dk >= 192 ? FC_CRASH : FC_HAT;
        return FC_METAL;
    }
    if (z == NZ_CLAP)
        return FC_CLAP;
    if (z == NZ_HAT)
        return FC_HAT;
    if (z == NZ_SNARE)
        return FC_SNARE;
    int32_t idx = fd_index_q8(k[FD_FM]), dec = dk;
    if (!z && k[FD_PITCH] >= -9000 && (fz == FZ_SQUARE || fd_fm_level(k[FD_FM]) >= 5))
        return FC_ZAP;                          /* noise off, FEEDBACK 5+ or SQUARE, above a kick: a scream */
    int harm = (FD_RATIO_HARMONIC >> (fd_pos(k[FD_RATIO]) >> 10)) & 1;
    if (fd_sweep_q(k[FD_SWEEP]) >= 30 * 512)
        return FC_ZAP;
    if (idx >= 384 && dec >= 192)               /* >= 1.5 rad and a decay of ~1 s or more */
        return FC_BELL;
    if (!harm && idx >= 512)                    /* inharmonic, >= 2 rad */
        return FC_METAL;
    if (k[FD_PITCH] < -9000)                    /* below ~80 Hz */
        return FC_KICK;
    if (k[FD_PITCH] < 5300)                     /* below ~400 Hz */
        return FC_TOM;
    return FC_PERC;
}

/* pitch -> Hz (as fmdrum.c's inc_of: 13.75 Hz x 2^octaves) */
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

/* 0.1 ms units -> "450MS" below 1 s, "1.2SEC" above (no division: at most 4 subtractions per digit) */
static char *fd_put_time(char *p, uint32_t t10)
{
    if (t10 < 9995)
        return put_str(put_dec(p, (t10 * 6554 + 32768) >> 16), "MS");
    uint32_t s = 0, d = 0;
    t10 += 500;                                 /* round to 0.1 s */
    while (t10 >= 10000) {
        t10 -= 10000;
        s++;
    }
    while (t10 >= 1000) {
        t10 -= 1000;
        d++;
    }
    return put_str(put_dec(put_str(put_dec(p, s), "."), d), "S");
}

/* knob i's value as text */
static char *fd_value(char *p, int i, const int16_t *k, int long_ratio)
{
    if (i == FD_PITCH) {
        uint32_t hz = fd_hz(k[FD_PITCH]), kh = 0;
        if (hz < 1000)
            return put_str(put_dec(p, hz), "HZ");
        while (hz >= 1000) {                    /* at most 3 steps (3.6 kHz) */
            hz -= 1000;
            kh++;
        }
        return put_str(put_dec(put_str(put_dec(p, kh), "."), (hz * 655) >> 16), "K");
    }
    if (i == FD_SWEEP)
        return put_str(put_dec(put_str(p, "+"), ((uint32_t)fd_sweep_q(k[FD_SWEEP]) + 256) >> 9), "ST");
    if (i == FD_FM) {                           /* index; FEEDBACK 0..7 ("FB3"; title "3 FEEDBACK"); SQUARE 1..5 */
        int z = fd_fm_zone(k[FD_FM]), lv = fd_fm_level(k[FD_FM]);
        if (z == FZ_FEEDBACK)
            return put_dec(put_str(p, long_ratio ? "FEEDBACK " : "FB"), (uint32_t)lv);
        if (z == FZ_SQUARE)
            return long_ratio ? put_dec(put_str(p, "SQUARE "), (uint32_t)lv) : put_dec(put_str(p, "SQ"), (uint32_t)lv);
        uint32_t q = (uint32_t)fd_index_q8(k[FD_FM]);
        return put_dec(put_str(put_dec(p, q >> 8), "."), ((q & 255) * 10) >> 8);
    }
    if (i == FD_RATIO) {
        int ri = fd_pos(k[FD_RATIO]) >> 10;
        uint32_t fr = fd_ratio_x100[ri], ip = 0, tens = 0;
        while (fr >= 100) {
            fr -= 100;
            ip++;
        }
        while (fr >= 10) {
            fr -= 10;
            tens++;
        }
        p = put_dec(put_dec(put_str(put_dec(p, ip), "."), tens), fr);
        return long_ratio ? put_str(p, (FD_RATIO_HARMONIC >> ri) & 1 ? " TONE" : " METAL") : p;
    }
    if (i == FD_NOISE)
        return put_str(p, fd_noise_zone[fd_noise_zone_of(k[FD_NOISE])]);
    int t = i == FD_DECAY ? 0 : i == FD_SWDEC ? 1 : 2;
    return fd_put_time(p, fd_time_10us[t][fd_pos(k[i]) >> 7]);
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
#define FD_GY1 68                               /* 40 units for 60 dB */
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
        fp32 x = FD_GX(i << 8), y = FP(FD_GY0) + (fp32)(((-db) >> 8) * 171 >> 6);   /* 40 units per 60 dB */
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
    char buf[24];
    struct fd_view *vw = &fd_v;
    slot &= 7;
    int edit = te_drum_edit;
    edit = edit < 0 ? 0 : edit >= FD_KEYS ? FD_KEYS - 1 : edit;
    const int16_t *kit = te_drum_params + slot * FD_KEYS * 8, *k = kit + edit * 8;
    vw->frame++;

    /* a turned knob: the edit key's values changed since the last frame (a change of key is not a turn) */
    if (vw->edit == edit) {
        for (int i = 0; i < 8; i++)
            if (k[i] != vw->last[i]) {
                vw->shown = i;
                vw->until = (int32_t)vw->frame + FD_SHOW;
            }
    } else {
        vw->until = 0;
    }
    vw->edit = edit;
    for (int i = 0; i < 8; i++)
        vw->last[i] = k[i];

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

    /* ---- title: the key's class and number (white), or the knob just turned (its colour) ---- */
    if ((int32_t)(vw->until - (int32_t)vw->frame) > 0) {
        char *q = put_str(put_str(buf, fd_knob_name[vw->shown]), " ");
        fd_value(q, vw->shown, k, 1);
        text(r, buf, FP(8), FP(7), FP(2), fd_knob_colour[vw->shown]);
    } else {
        int cls = fd_class(k), len = 0;
        text(r, fd_class_name[cls], FP(8), FP(7), FP(2), WHITE);
        while (fd_class_name[cls][len])
            len++;
        put_dec(put_str(buf, "KEY "), (uint32_t)edit + 1);
        text_fast(r, buf, FP(14 + 12 * len), FP(13), FP(1), GREY);
    }

    /* ---- the drum (grey and white) ---- */
    int32_t u = k[FD_PITCH] + 24320;
    u = u < 0 ? 0 : u > 49152 ? 49152 : u;
    int32_t rxu = 62 - ((u * 43) >> 16);                            /* 62 (14 Hz) .. 30 (3.6 kHz) */
    fp32 rx = FP(rxu), ry = FP((rxu * 10) >> 5);
    fp32 depth = FP(6 + (((fd_pos(k[FD_DECAY]) >> 7) * 16) >> 8));
    fp32 cx = FP(86), hy = FP(72) - (age && age < 12 ? FP(12 - (int32_t)age) >> 2 : 0);   /* the hop */
    fp32 by = FP(72) + depth;
    line_fast(r, cx - rx, hy, cx - rx, by, GREY);                  /* shell, the near half of the bottom rim, lugs */
    line_fast(r, cx + rx, hy, cx + rx, by, GREY);
    fd_ellipse(r, cx, by, rx, ry, 16, 0x08000000u, 0, GREY);
    int32_t ksn = fd_noise_sound(k[FD_NOISE]);
    const uint16_t *nrow = fd_noise + (ksn >> 8) * FD_NOISE_W;
    int32_t wm = nrow[9], wsq = nrow[10];
    for (int i = 1; i < 6; i++) {                                  /* lugs; METAL: hex nuts on the shell (the 808's
                                                                    * six squares), rattling while it plays */
        uint32_t a = (uint32_t)i * 0x15555555u;                    /* 30 .. 150 degrees: the near side */
        fp32 lx = cx + (((rx >> 5) * cos_t(a)) >> 10), ly = ((ry >> 5) * sin_t(a)) >> 10;
        if (wm > 3000) {
            uint32_t h = fd_hash((uint32_t)i * 0x165667B1u + (live ? vw->frame : 0));
            fp32 j = live && amp > 2000 ? (fp32)((int32_t)(h & 3) - 1) * FP(1) : 0;
            fp32 my = hy + ly + ((by - hy) >> 1);
            fd_ellipse(r, lx + j, my, FP(4), FP(4), 6, 0x2AAAAAABu, 0, live ? WHITE : GREY);    /* hexagon */
            fd_ellipse(r, lx + j, my, FP(2), FP(2), 6, 0x2AAAAAABu, 0, live ? GREY : PURPLEGREY);
        } else {
            line_fast(r, lx, hy + ly + FP(3), lx, by + ly - FP(2), PURPLEGREY);
        }
    }
    if (live && amp > 200)                                          /* ripples while it sounds */
        for (int j = 0; j < 2; j++) {
            fp32 g = FP((int32_t)((age * 2 + (uint32_t)j * 9) & 15) + 3);
            fd_ellipse(r, cx, hy, rx + g, ry + (g >> 1), 32, 0x08000000u, 0, amp > 5000 ? GREY : DARKBLUE);
        }
    /* FM's top zones: FEEDBACK 0..7 tears the head and the rosette (jag) and throws sparks off the rim; SQUARE squares
     * the petals off (flat tops) at full jag */
    int fz = fd_fm_zone(k[FD_FM]), flv = fd_fm_level(k[FD_FM]);
    int32_t jag = fz == FZ_FEEDBACK ? flv : fz == FZ_SQUARE ? 7 - flv : 0;    /* SQUARE calms as the feedback falls */
    uint32_t jseed = live ? vw->frame * 0x9E3779B9u : 0x1234567u;
    {                                                               /* head outline (anti-aliased), torn by FEEDBACK */
        fp32 px = cx + rx, py = hy;
        for (int i = 1; i <= 24; i++) {
            uint32_t a = (uint32_t)i * 0x0AAAAAABu;
            int32_t g = 32767;
            if (jag && i < 24) {                    /* odd points out, even in: up to +-9 % of the radius */
                int32_t h = (int32_t)(fd_hash((uint32_t)i * 2654435761u + jseed) >> 20);       /* 0 .. 4095 */
                int32_t d = (jag * (1500 + ((h * 1500) >> 12))) >> 3;
                g += i & 1 ? d : -d;
            }
            fp32 qx = cx + (((((rx >> 5) * g) >> 15) * cos_t(a)) >> 10), qy = hy + (((((ry >> 5) * g) >> 15) * sin_t(a)) >> 10);
            line_fp(r, px, py, qx, qy, FP(3) >> 1, live ? WHITE : GREY);
            px = qx;
            py = qy;
        }
    }
    if (jag) {                                                      /* sparks off the rim: 2 per level */
        int n = jag * 2;
        if (live)
            n = (n * (amp > 16384 ? 32767 : amp << 1)) >> 15;
        for (int i = 0; i < n; i++) {
            uint32_t h = fd_hash((uint32_t)i * 0x85EBCA6Bu + jseed);
            uint32_t a = 0x80000000u + ((h & 0xFFFF) << 15);           /* the far half of the rim: up and out */
            int32_t c = cos_t(a), sn = sin_t(a), len = 3 + (int32_t)((h >> 16) & 7) + jag;
            fp32 x0 = cx + (((rx >> 5) * c) >> 10), y0 = hy + (((ry >> 5) * sn) >> 10);
            fp32 x1 = x0 + (fp32)((c * len) >> 5) * (FP(1) >> 10), y1 = y0 + (fp32)((sn * len) >> 5) * (FP(1) >> 10) - FP(2);
            line_fast(r, x0, y0, x1, y1, (h >> 13) & 1 ? WHITE : GREY);
        }
    }
    /* the FM rosette: radius (fraction of the head) 0.56 + 0.31 depth cos(petals x angle) */
    int ri = fd_pos(k[FD_RATIO]) >> 10, harm = (FD_RATIO_HARMONIC >> ri) & 1;
    int32_t ipk = fd_index_q8(k[FD_FM]), iq8 = live ? live->index_q8 : ipk;
    int32_t dq = iq8 >= 2048 ? 32767 : iq8 << 4;                    /* depth: full at 8 rad */
    {
        uint32_t n_q16 = fd_ratio[ri] << 1;                         /* petals = 2 x ratio */
        int segs = harm ? 64 : 128;                                 /* inharmonic: two turns */
        uint32_t dps = (0x04000000u >> 8) * (n_q16 >> 8);           /* petals x 1/64 turn, mod 2^32 */
        uint32_t ph0 = live ? vw->frame * 0x01000000u : 0x40000000u;
        int32_t amp_q15 = (10158 * dq) >> 15;
        int col = live ? WHITE : ipk ? GREY : DARKBLUE;
        fp32 px = 0, py = 0;
        for (int i = 0; i <= segs; i++) {
            uint32_t th = 0x04000000u * (uint32_t)i, ps = dps * (uint32_t)i + ph0;
            int32_t cp = cos_t(ps);
            if (fz == FZ_SQUARE) {                                  /* SQUARE 1..5: petals clip flat */
                cp = cp * (1 + 3 * flv);
                cp = cp > 32767 ? 32767 : cp < -32767 ? -32767 : cp;
            }
            int32_t ru = 18350 + ((amp_q15 * cp) >> 15);           /* Q15 */
            if (jag)                                                /* FEEDBACK: a ragged edge, +-4 % per level */
                ru += (int32_t)((fd_hash((uint32_t)i * 0x27D4EB2Fu + jseed) >> 21) - 1024) * jag;
            int32_t c = (ru * cos_t(th)) >> 15, s = (ru * sin_t(th)) >> 15;
            fp32 x = cx + (((rx >> 5) * c) >> 10), y = hy + (((ry >> 5) * s) >> 10);
            if (i)
                line_fast(r, px, py, x, y, col);
            px = x;
            py = y;
        }
    }
    /* the sweep (green: SWEEP's colour): a curve falling onto the head, its bend from SWDEC (~160 ms shown) */
    int32_t sq = fd_sweep_q(k[FD_SWEEP]);
    fp32 head_top = hy - ry;
    if (sq > 0) {
        fp32 room = head_top - FP(26);
        fp32 h = room > 0 ? (fp32)((((room >> 5) * (sq >> 3)) * 341) >> 15) : 0;   /* sq / 24576 of the room above */
        int32_t f = fd_swp_dec[fd_pos(k[FD_SWDEC]) >> 7];           /* per block, Q30 */
        int32_t f2 = (f >> 15) * (f >> 15), f4 = (f2 >> 15) * (f2 >> 15), f8 = (f4 >> 15) * (f4 >> 15);
        int32_t fs = (((f8 >> 15) * (f2 >> 15)) >> 15) * (f >> 15);      /* f^11 per segment */
        fp32 x0 = cx - ((rx * 7) >> 3), dx = (((rx * 7) >> 3) * 6554) >> 16;   /* 10 segments to the centre */
        int32_t e = 1 << 30, se = live ? live->senv : -1;
        fp32 px = x0, py = head_top - h, dotx = -1, doty = 0;
        for (int j = 1; j <= 10; j++) {
            int32_t e1 = (e >> 15) * (fs >> 15);
            fp32 x = x0 + dx * j, y = head_top - (fp32)(((h >> 5) * (e1 >> 15)) >> 10);
            line_fp(r, px, py, x, y, FP(3) >> 1, GREEN);
            if (dotx < 0 && se >= e1 && se >= 0) {
                dotx = px;
                doty = py;
            }
            px = x;
            py = y;
            e = e1;
        }
        line_fp(r, px, py, px - FP(3), py - FP(4), FP(3) >> 1, GREEN);  /* arrow head */
        line_fp(r, px, py, px + FP(1), py - FP(5), FP(3) >> 1, GREEN);
        if (dotx >= 0)
            line_fp(r, dotx, doty - FP(1), dotx, doty + FP(1), FP(4), WHITE);
    }
    /* noise: white/grey speckle above the head, zones apart by height and density; CLAP adds clap marks */
    int32_t kn = ksn;
    int zone = fd_noise_zone_of(k[FD_NOISE]);
    if (zone) {
        int32_t nl = fd_noise[(kn >> 8) * FD_NOISE_W + 1];         /* noise level, Q15 */
        static const uint8_t dens[7] = { 0, 14, 30, 46, 22, 10, 26 }, rise_u[7] = { 0, 5, 12, 22, 14, 8, 10 };
        int n = (nl * dens[zone]) >> 15;
        if (live)
            n = (n * (amp > 16384 ? 32767 : amp << 1)) >> 15;
        fp32 rise = FP(rise_u[zone]);
        int c0 = zone == NZ_AIR ? DARKBLUE : zone == NZ_HAT ? WHITE : GREY;
        int c1 = zone == NZ_AIR ? GREY : zone == NZ_SNARE || zone >= NZ_METAL ? WHITE : GREY;
        uint32_t seed = live ? vw->frame : 7;
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
        if (zone == NZ_GRIND) {                                     /* GRIND: an angle grinder's spray off the right rim */
            int32_t tg = kn - 26624;
            int n = 8 + (tg >> 9) + (wsq >> 12);                     /* 8 .. ~27 streaks */
            if (live)
                n = (n * (amp > 16384 ? 32767 : amp << 1)) >> 15;
            fp32 sx = cx + ((rx * 7) >> 3), sy = hy - (ry >> 2);
            for (int i = 0; i < n; i++) {
                uint32_t h = fd_hash((uint32_t)i * 0x9E3779B1u + (live ? vw->frame * 7u : 11u));
                uint32_t a = 0xF0000000u + ((h & 0xFFFF) << 13);       /* -22 .. +22 degrees, up and out to the right */
                int32_t len = 6 + (int32_t)((h >> 16) & 15);
                fp32 x0 = sx + FP((int32_t)((h >> 20) & 3)), y0 = sy;
                fp32 x1 = x0 + (fp32)((cos_t(a) * len) >> 5) * (FP(1) >> 10);
                fp32 y1 = y0 - (fp32)((sin_t(a + 0x10000000u) * len) >> 5) * (FP(1) >> 10);
                line_fast(r, x0, y0, x1, y1, (h >> 28) & 1 ? WHITE : GREY);
            }
        }
        if (zone == NZ_CLAP) {                                      /* clap: three marks each side */
            int col = live ? WHITE : GREY;
            for (int j = 0; j < 3; j++) {
                fp32 dy = FP(5 * j);
                line_fp(r, cx - rx - FP(4), hy - FP(10) + dy, cx - rx - FP(11), hy - FP(14) + (dy << 1) - FP(2), FP(3) >> 1, col);
                line_fp(r, cx + rx + FP(4), hy - FP(10) + dy, cx + rx + FP(11), hy - FP(14) + (dy << 1) - FP(2), FP(3) >> 1, col);
            }
        }
    }

    /* ---- the graph (40 units tall, ticks without labels) ---- */
    line_fast(r, FP(FD_GX0), FP(FD_GY1), FP(FD_GX0 + FD_GW), FP(FD_GY1), DARKBLUE);
    line_fast(r, FP(FD_GX0), FP(FD_GY0), FP(FD_GX0), FP(FD_GY1), DARKBLUE);
    static const int16_t tick_q8[3] = { 2852, 6254, 9656 };        /* 10 ms, 0.1 s, 1 s: 4 log2(t / 1 block), Q8 */
    for (int i = 0; i < 3; i++)
        vline_fast(r, FD_GX(tick_q8[i]), FP(FD_GY1 - 2 - i), FP(FD_GY1), GREY);
    if (sq > 0)                                                    /* sweep: from its depth (48 st = top) */
        fd_curve(r, (fd_log2_q8((uint32_t)sq) - 3734) * 1541, fd_db_blk[1][fd_pos(k[FD_SWDEC]) >> 7], GREEN);
    if (ipk > 0)                                                   /* FM index: from its depth (16 rad = top) */
        fd_curve(r, (fd_log2_q8((uint32_t)ipk) - 3072) * 1541, fd_db_blk[2][fd_pos(k[FD_FMDEC]) >> 7], WHITE);
    fd_curve(r, 0, fd_db_blk[0][fd_pos(k[FD_DECAY]) >> 7], ORANGE);
    if (live)
        vline_fast(r, FD_GX(fd_log2_q8(age) << 2), FP(FD_GY0), FP(FD_GY1), GREY);

    /* ---- values under grey labels: the plain knobs, then SHIFT's (dimmer) ---- */
    static const uint16_t col_x[4] = { 172, 207, 243, 281 };
    for (int i = 0; i < 4; i++) {
        text_fast(r, fd_knob_name[i], FP(col_x[i]), FP(73), FP(3) >> 2, GREY);
        fd_value(buf, i, k, 0);
        text_fast(r, buf, FP(col_x[i]), FP(80), FP(1), fd_knob_colour[i]);
        text_fast(r, fd_knob_name[4 + i], FP(col_x[i]), FP(97), FP(3) >> 2, DARKBLUE);
        fd_value(buf, 4 + i, k, 0);
        text_fast(r, buf, FP(col_x[i]), FP(104), FP(1), fd_shift_colour[i]);
    }
    text_fast(r, "SHIFT", FP(172), FP(91), FP(3) >> 2, DARKBLUE);
    line_fast(r, FP(198), FP(93), FP(312), FP(93), DARKBLUE);

    /* ---- the keys ---- */
    fd_keys(r, kit, edit, lit);
}

#endif
