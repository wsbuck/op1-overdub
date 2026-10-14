/* The amp engine's screen (src/engine/amp.c, notes/amp.md), included by src/slot/ui.c after its drawing helpers.
 *
 * A small combo amp in an oblique view (front face, top with a handle, right side), about 55 % of the width, drawn
 * like TE's object screens: thin lavender outlines, the knob colours only where a knob acts, a few moving details.
 *   blue   drive: the combo rumbles and the speakers throw sparks, by drive x input level, only while a note sounds
 *   green  tone: the cloth's glints and the speakers' dust caps, dim and few .. bright and many (green shades), and
 *          the weave
 *   white  gate: the threshold mark on the panel's level meter, and the gate lamp
 *   orange cab: the speakers behind the cloth (none = direct "DI", 1, 2, 4)
 * The panel knobs are small cylinders in their colours; the red pilot jewel's rays grow with the input. The tuner (top
 * left) shows only while no note sounds: the UI thread polls TE IN (amp_capture) and runs YIN (amp_tune.h).
 */
#ifndef UI_AMP_H
#define UI_AMP_H

#define AMP_SPARKS 12
struct amp_spark { int32_t x, y, vx, vy, life; };              /* Q10 units, per frame */
struct amp_view {
    uint32_t frame, renders, inited;
    int32_t level;                                              /* smoothed |TE IN| peak, Q15 */
    int32_t playing;                                            /* frames since the engine last rendered (0 = now) */
    struct amp_spark sp[AMP_SPARKS];
    struct amp_tuner tn;
};
static struct amp_view amp_v RAM = { 0, 0, 0, 0, 99, { { 0, 0, 0, 0, 0 } }, { { 0, 0, 0 }, 0, 0, 0, 0 } };
static struct amp_tune_work amp_tw RAM = { { 0 }, { 0 } };
static int16_t amp_cap[AMP_TUNE_N] RAM = { 0 };

#ifdef __bfin__
/* AMP_TUNE_BLOCKS consecutive blocks of TE IN, polled on the UI thread: a block is new when te_rx_sel toggles or its
 * samples change (the audio interrupt writes it before the voice loop). Refused (0) if a block may have been missed or
 * overwritten while copying (block-to-block time over 1.5x the shortest), or after ~40 M cycles. */
#define AMP_CAPTURE_MAX 40000000u
static int amp_capture(int16_t *dst)
{
    uint32_t t0 = cycles(), last = 0, dmin = 0xFFFFFFFFu, dmax = 0;
    uint32_t sel = te_rx_sel & 1;
    int16_t s0 = te_audio_in[0], s1 = te_audio_in[21], s2 = te_audio_in[42], s3 = te_audio_in[63];
    for (int b = 0; b < AMP_TUNE_BLOCKS; b++) {
        while ((te_rx_sel & 1) == sel && te_audio_in[0] == s0 && te_audio_in[21] == s1 && te_audio_in[42] == s2
               && te_audio_in[63] == s3)
            if (cycles() - t0 > AMP_CAPTURE_MAX)
                return 0;
        uint32_t now = cycles();
        sel = te_rx_sel & 1;
        int16_t *d = dst + b * 64;
        for (int i = 0; i < 64; i++)
            d[i] = te_audio_in[i];
        s0 = d[0];
        s1 = d[21];
        s2 = d[42];
        s3 = d[63];
        if ((te_rx_sel & 1) != sel || te_audio_in[0] != s0 || te_audio_in[63] != s3)
            return 0;                                           /* overwritten while copying */
        if (b) {
            uint32_t dt = now - last;
            dmin = dt < dmin ? dt : dmin;
            dmax = dt > dmax ? dt : dmax;
        }
        last = now;
    }
    return dmax < dmin + (dmin >> 1);
}
#else
extern int host_amp_capture(int16_t *dst);                     /* preview_host.c */
#define amp_capture host_amp_capture
#endif

/* cheap hash noise, -32768..32767 */
static int32_t amp_hash(uint32_t a, uint32_t b)
{
    uint32_t h = (a * 2654435761u) ^ (b * 2246822519u);
    h ^= h >> 15;
    h *= 2654435761u;
    return (int32_t)h >> 16;
}

/* ellipse of 1-px lines, n = 8, 16 or 32 segments, from turn a0 for `part` of n (part = n: closed) */
static void amp_ellipse(void *r, fp32 cx, fp32 cy, fp32 rx, fp32 ry, int n, int part, uint32_t a0, int colour)
{
    uint32_t step = n >= 32 ? 1u << 27 : n >= 16 ? 1u << 28 : 1u << 29;
    fp32 px = cx + (((rx >> 5) * cos_t(a0)) >> 10), py = cy + (((ry >> 5) * sin_t(a0)) >> 10);
    for (int i = 1; i <= part; i++) {
        uint32_t a = a0 + step * (uint32_t)i;
        fp32 qx = cx + (((rx >> 5) * cos_t(a)) >> 10), qy = cy + (((ry >> 5) * sin_t(a)) >> 10);
        line_fast(r, px, py, qx, qy, colour);
        px = qx;
        py = qy;
    }
}

#define AMP_W (FP(5) >> 2)                                      /* outline weight (TE's thin strokes) */

/* panel knob as a small cylinder seen from above: a top ellipse in the knob colour with an AA pointer (7 .. 5
 * o'clock), the skirt's sides and lower rim in grey */
static void amp_knob_draw(void *r, fp32 cx, fp32 cy, int32_t k, int colour)
{
    const fp32 rx = FP(6), ry = FP(5) >> 1, h = FP(3);
    line_fast(r, cx - rx, cy, cx - rx, cy + h, GREY);
    line_fast(r, cx + rx, cy, cx + rx, cy + h, GREY);
    amp_ellipse(r, cx, cy + h, rx, ry, 16, 8, 0, GREY);          /* the near half of the lower rim */
    amp_ellipse(r, cx, cy, rx, ry, 16, 16, 0, colour);
    uint32_t a = 0x60000000u + (uint32_t)(amp_knob((int16_t)k) * 131072);   /* 0.75 turn over the knob */
    line_fp(r, cx, cy, cx + (((rx >> 5) * cos_t(a)) >> 10), cy + (((ry >> 5) * sin_t(a)) >> 10), FP(3) >> 1, colour);
}

static const char *const amp_notes[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static void amp_tuner_draw(void *r, const struct amp_tuner *t)
{
    char buf[8], *p;
    const fp32 sx = FP(46), sw = FP(80), sy = FP(15);         /* cents scale: -50 .. +50 over x 46 .. 126 */
    int on = t->note >= 0;
    for (int i = 0; i <= 4; i++)
        vline_fast(r, sx + (sw >> 2) * i, i == 2 ? FP(9) : FP(12), FP(18), on ? (i == 2 ? WHITE : GREY) : DARKBLUE);
    line_fast(r, sx, sy + FP(3), sx + sw, sy + FP(3), on ? GREY : DARKBLUE);
    if (!on) {
        text(r, "--", FP(8), FP(5), FP(2), DARKBLUE);
        return;
    }
    int n = t->note, oct = -1;
    while (n >= 12) {
        n -= 12;
        oct++;
    }
    int32_t c = t->cents;                                       /* Q8 cents */
    int in_tune = c > -3 * 256 && c < 3 * 256;
    text(r, amp_notes[n], FP(8), FP(4), FP(2), in_tune ? GREEN : WHITE);
    p = buf;
    if (oct >= 0)
        put_dec(p, (uint32_t)oct);
    else
        put_str(p, "-");
    text_fast(r, buf, amp_notes[n][1] ? FP(32) : FP(20), FP(10), FP(1), GREY);
    fp32 nx = sx + (sw >> 1) + (fp32)((c * 205) >> 6);         /* 0.8 units per cent (c in Q8 cents) */
    nx = nx < sx ? sx : nx > sx + sw ? sx + sw : nx;
    line_fp(r, nx, FP(7), nx, FP(19), FP(2), in_tune ? GREEN : WHITE);
    p = buf;
    if (c >= 0)
        *p++ = '+';
    put_sdec(p, (c + (c >= 0 ? 128 : -128)) >> 8);
    text_fast(r, buf, FP(100), FP(2), FP(1), in_tune ? GREEN : GREY);
}

static void draw_amp(void *r, const int16_t *k)
{
    struct amp_view *v = &amp_v;
    const struct amp_ram *e = &amp_ram;
    if (!v->inited) {
        amp_tune_reset(&v->tn);
        v->inited = 1;
    }
    v->frame++;
    /* input level from TE IN (live with or without a note), smoothed; is a note sounding (the engine rendered)? */
    int32_t pk = 0;
    for (int i = 0; i < 64; i++) {
        int32_t a = te_audio_in[i];
        a = a < 0 ? -a : a;
        pk = a > pk ? a : pk;
    }
    v->level = pk > v->level ? pk : v->level - (v->level >> 3);
    uint32_t renders = e ? e->renders : 0;
    v->playing = renders != v->renders ? 0 : v->playing < 99 ? v->playing + 1 : 99;
    v->renders = renders;
    int playing = v->playing < 2;
    int32_t drive = amp_knob(k[0]), tone = amp_knob(k[1]), gate = amp_knob(k[2]), cab = amp_knob(k[3]);
    int32_t ldb = dbfs_q8(v->level);                            /* dBFS Q8 */
    int32_t lnorm = ldb <= -60 * 256 ? 0 : ldb >= 0 ? 32767 : ((ldb + 60 * 256) * 2184) >> 10;   /* -60..0 dB, Q15 */

    /* tuner: only while no note sounds; a capture every 4th frame when there is signal */
    if (playing) {
        amp_tune_reset(&v->tn);
    } else {
        if (!(v->frame & 3))
            amp_tune_update(&v->tn, pk >= AMP_TUNE_SILENT && amp_capture(amp_cap) ? amp_tune_pitch(amp_cap, &amp_tw) : -1);
        amp_tuner_draw(r, &v->tn);
    }

    /* blue: rumble = drive^2 x level while a note sounds (clean playing stays still) */
    int32_t rumble = playing ? (((drive * drive) >> 15) * lnorm) >> 15 : 0;
    fp32 ox = 0, oy = 0;
    if (rumble > 2048) {
        int32_t amt = (rumble * 2) >> 5;                        /* up to 2 units */
        ox = (amp_hash(v->frame, 1) * amt) >> 15;
        oy = (amp_hash(v->frame, 2) * amt) >> 15;
    }

    /* geometry: front face x0..x1, y0..y1; the top and right side recede by (dx, dy) */
    const fp32 x0 = FP(74) + ox, x1 = FP(232) + ox, y0 = FP(54) + oy, y1 = FP(134) + oy;
    const fp32 dx = FP(24), dy = -FP(13), c = FP(3);           /* depth, corner chamfer */
    /* floor shadow */
    line_fast(r, x0 + FP(2), FP(137), x1, FP(137), DARKBLUE);
    line_fast(r, x1, FP(137), x1 + dx - FP(2), FP(137) + dy, DARKBLUE);
    /* top and side faces */
    line_fp(r, x0 + c, y0, x0 + dx + c, y0 + dy, AMP_W, PURPLEGREY);
    line_fp(r, x0 + dx + c, y0 + dy, x1 + dx - c, y0 + dy, AMP_W, PURPLEGREY);
    line_fp(r, x1 + dx - c, y0 + dy, x1 + dx, y0 + dy + c, AMP_W, PURPLEGREY);
    line_fp(r, x1 + dx, y0 + dy + c, x1 + dx, y1 + dy - c, AMP_W, PURPLEGREY);
    line_fp(r, x1 + dx, y1 + dy - c, x1, y1, AMP_W, PURPLEGREY);
    line_fp(r, x1 - c, y0, x1 + dx - c, y0 + dy, AMP_W, GREY);  /* the top's right edge */
    line_fast(r, x1 + FP(18), y0 + FP(4) + ((dy * 3) >> 2), x1 + FP(18), y1 - FP(4) + ((dy * 3) >> 2), OFFGREY);   /* back seam */
    /* the handle: two caps on the top and a strap arching over them */
    const fp32 hx = (x0 + x1) >> 1, hy = y0 + (dy >> 1);
    for (int s = -1; s <= 1; s += 2) {
        fp32 bx = hx + dx / 2 + s * FP(20);
        line_fp(r, bx - FP(3), hy, bx + FP(3), hy, AMP_W, GREY);
        line_fp(r, bx, hy, bx, hy - FP(3), AMP_W, GREY);
    }
    {
        fp32 ax = hx + dx / 2 - FP(20), bx = hx + dx / 2 + FP(20), ty = hy - FP(3);
        line_fp(r, ax, ty, ax + FP(6), ty - FP(4), FP(3) >> 1, PURPLEGREY);
        line_fp(r, ax + FP(6), ty - FP(4), bx - FP(6), ty - FP(4), FP(3) >> 1, PURPLEGREY);
        line_fp(r, bx - FP(6), ty - FP(4), bx, ty, FP(3) >> 1, PURPLEGREY);
    }
    /* front face, chamfered corners */
    line_fp(r, x0 + c, y0, x1 - c, y0, AMP_W, PURPLEGREY);
    line_fp(r, x1 - c, y0, x1, y0 + c, AMP_W, PURPLEGREY);
    line_fp(r, x1, y0 + c, x1, y1 - c, AMP_W, PURPLEGREY);
    line_fp(r, x1, y1 - c, x1 - c, y1, AMP_W, PURPLEGREY);
    line_fp(r, x1 - c, y1, x0 + c, y1, AMP_W, PURPLEGREY);
    line_fp(r, x0 + c, y1, x0, y1 - c, AMP_W, PURPLEGREY);
    line_fp(r, x0, y1 - c, x0, y0 + c, AMP_W, PURPLEGREY);
    line_fp(r, x0, y0 + c, x0 + c, y0, AMP_W, PURPLEGREY);

    /* control panel: a strip with silver piping under it */
    const fp32 py0 = y0 + FP(4), py1 = y0 + FP(21), ky = y0 + FP(11);
    line_fast(r, x0 + FP(4), py0, x1 - FP(4), py0, GREY);
    line_fp(r, x0 + FP(4), py1, x1 - FP(4), py1, AMP_W, PURPLEGREY);
    amp_knob_draw(r, x0 + FP(16), ky, drive, BLUE);
    amp_knob_draw(r, x0 + FP(33), ky, tone, GREEN);
    amp_knob_draw(r, x0 + FP(50), ky, gate, WHITE);
    amp_knob_draw(r, x0 + FP(67), ky, cab, ORANGE);
    /* level meter (-60 .. 0 dBFS) with the gate threshold (white) and the gate lamp */
    const fp32 mx = x0 + FP(82), mw = FP(40), my = ky + FP(1);
    line_fast(r, mx, my, mx + mw, my, DARKBLUE);
    int open;
    if (gate) {
        int32_t g = (amp_gate_db_q8(gate) + 60 * 256) * 2184 >> 10;
        fp32 gx = mx + (fp32)(((g < 0 ? 0 : g) * (mw >> 7)) >> 8);
        line_fp(r, gx, my - FP(4), gx, my + FP(4), FP(3) >> 1, WHITE);
        open = playing && e ? e->gate_open : ldb > amp_gate_db_q8(gate);
    } else {
        open = 1;
    }
    fp32 lw = (fp32)((lnorm * (mw >> 7)) >> 8);
    if (lw > FP(1))
        line_fp(r, mx, my, mx + lw, my, FP(2), open ? LIGHTBLUE : DARKBLUE);
    amp_ellipse(r, mx + mw + FP(6), my, FP(2), FP(2), 8, 8, 0, open ? WHITE : DARKBLUE);
    if (open)
        line_fp(r, mx + mw + FP(5), my, mx + mw + FP(7), my, FP(2), WHITE);
    /* the pilot jewel: a red lens in a grey bezel; its rays grow with the input */
    const fp32 jx = x1 - FP(12);
    amp_ellipse(r, jx, ky, FP(4), FP(4), 16, 16, 0, GREY);
    line_fp(r, jx - FP(1), ky, jx + FP(1), ky, FP(4), RED);
    int32_t rl = (lnorm * 6) >> 15;                             /* ray length, units */
    if (rl > 0)
        for (int i = 0; i < 8; i++) {
            uint32_t a = (uint32_t)i << 29;
            fp32 cs = cos_t(a), sn = sin_t(a);
            line_fast(r, jx + ((FP(6) >> 5) * cs >> 10), ky + ((FP(6) >> 5) * sn >> 10),
                      jx + ((FP(6 + rl) >> 5) * cs >> 10), ky + ((FP(6 + rl) >> 5) * sn >> 10), RED);
        }

    /* grille: recessed in the front frame (the recess's top and left walls), the cloth's weave and a few glints
     * (green: tone), the speakers behind it (orange: cab) */
    const fp32 gx0 = x0 + FP(6), gx1 = x1 - FP(6), gy0 = py1 + FP(5), gy1 = y1 - FP(6);
    line_fast(r, gx0, gy0, gx1, gy0, GREY);
    line_fast(r, gx1, gy0, gx1, gy1, GREY);
    line_fast(r, gx1, gy1, gx0, gy1, GREY);
    line_fast(r, gx0, gy1, gx0, gy0, GREY);
    line_fast(r, gx0 + FP(2), gy0 + FP(2), gx1, gy0 + FP(2), OFFGREY);
    line_fast(r, gx0 + FP(2), gy0 + FP(2), gx0 + FP(2), gy1, OFFGREY);
    int bright = tone < 10923 ? 0 : tone < 21845 ? 1 : 2;
    static const uint8_t glint[3] = { DARKGREEN, GREEN, LIGHTGREEN };
    static const uint8_t cap[3] = { DARKGREEN, GREEN, LIGHTGREEN };
    int count = cab < 5461 ? 0 : cab < 16384 ? 1 : cab < 27307 ? 2 : 4;   /* nearest of DI, 1x12, 2x12, 4x12 */
    static const int16_t spk[5][4][3] = {                       /* centre offset from the grille's centre, radius */
        { { 0 } }, { { 0, 0, 21 } }, { { -34, 0, 19 }, { 34, 0, 19 } }, { { 0 } },
        { { -34, -10, 10 }, { 34, -10, 10 }, { -34, 11, 10 }, { 34, 11, 10 } } };
    const fp32 gcx = (gx0 + gx1) >> 1, gcy = (gy0 + gy1) >> 1;
    fp32 cone = rumble > 2048 ? (fp32)(amp_hash(v->frame, 3) * ((rumble * 3) >> 5) >> 15) : 0;
    for (int s = 0; s < count; s++) {
        const int16_t *q = spk[count][s];
        fp32 cx = gcx + FP(q[0]) + cone, cy = gcy + FP(q[1]) - cone, rad = FP(q[2]);
        amp_ellipse(r, cx, cy, rad, rad, 32, 32, 0, ORANGE);
        amp_ellipse(r, cx, cy, (rad * 3) >> 2, (rad * 3) >> 2, 32, 32, 0, OFFRED);
        amp_ellipse(r, cx, cy, (rad * 5) >> 4, (rad * 5) >> 4, 16, 16, 0, cap[bright]);
        if (bright)                                             /* the cap lights up as the tone opens */
            amp_ellipse(r, cx, cy, (rad * 3) >> 4, (rad * 3) >> 4, 16, 16, 0, cap[bright]);
    }
    for (fp32 y = gy0 + FP(6); y < gy1 - FP(1); y += FP(5))     /* the cloth's weave, in front of the speakers */
        line_fast(r, gx0 + FP(4), y, gx1 - FP(2), y, bright == 2 ? DARKBLUE : OFFGREY);
    if (!count)
        text_fast(r, "DI", gcx - FP(5), gcy - FP(3), FP(1), ORANGE);
    /* glints: a few four-point sparkles at fixed spots in the cloth (more and brighter as the tone opens up); one
     * of them twinkles at a time */
    static const int8_t gl[8][2] = { { -50, -14 }, { 44, -16 }, { -14, 15 }, { 56, 13 }, { -58, 12 }, { 14, -15 },
                                     { -30, 2 }, { 30, 3 } };
    int ng = 2 + 3 * bright, tw = (int)((v->frame >> 3) & 7);
    for (int i = 0; i < ng; i++) {
        fp32 sx = gcx + FP(gl[i][0]), sy = gcy + FP(gl[i][1]), sz = i == tw ? FP(3) : FP(1);
        int col = glint[bright];
        line_fast(r, sx - sz, sy, sx + sz, sy, col);
        line_fast(r, sx, sy - sz, sx, sy + sz, col);
    }

    /* sparks: from the speakers (or the panel without one), at high drive while playing loud */
    int32_t heat = playing && drive > 16384 ? (((drive - 16384) * 2) * lnorm) >> 15 : 0;  /* 0..32767 */
    for (int i = 0; i < AMP_SPARKS; i++) {
        struct amp_spark *p = &v->sp[i];
        if (p->life <= 0) {
            if (heat > 4096 && amp_hash(v->frame, 10 + i) + 32768 < heat >> 2) {
                int s = count ? (int)(((uint32_t)amp_hash(v->frame, 30 + i) & 0xFFFF) * (uint32_t)count >> 16) : 0;
                p->x = count ? gcx + FP(spk[count][s][0]) : jx;
                p->y = count ? gcy + FP(spk[count][s][1]) : ky;
                p->vx = amp_hash(v->frame, 50 + i) >> 3;        /* +-4 units per frame */
                p->vy = -FP(3) - ((amp_hash(v->frame, 70 + i) + 32768) >> 3);
                p->life = 5 + ((amp_hash(v->frame, 90 + i) + 32768) >> 13);
            }
            continue;
        }
        fp32 nx = p->x + p->vx, ny = p->y + p->vy;              /* a streak over the last frame and a half */
        if (ny > FP(4) && ny < FP(140) && nx > FP(4) && nx < FP(316))
            line_fp(r, p->x - (p->vx >> 1), p->y - (p->vy >> 1), nx, ny, FP(3) >> 1, (i & 1) ? WHITE : ORANGE);
        p->x = nx;
        p->y = ny;
        p->vy += FP(1) >> 1;                                    /* gravity */
        p->life--;
    }
}

#endif
