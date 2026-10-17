/* CHORD v2 screens (design draft for notes/chord2.md; rendered on the host by chord2_preview.c, not in firmware).
 * Uses ui.c's primitives with their device meaning: line_fp (AGG line, ~20-35 k cycles), line_fast / vline_fast
 * (1-px framebuffer lines, ~free), Q22.10 SVG units, 320 x 160, safe area x 4..316, y 2..140, TE palette.
 * Immediate mode only (no SVG document), so it can draw on any screen; no division, no floats.
 *
 * Layout of the CHORD page (MAP mode), after the review rounds (notes/chord2.md "Review log"):
 *   y   4..18  header: MAP n + name (blue: SHIFT+blue picks it); right: what is on (strum, SMOOTH, BASS, HOLD)
 *   y  24..47  the edited chord's name, big, white; blue underline = root (blue knob), green = quality (green knob)
 *   y  24..43  right: its notes on a 3-octave ruler (root blue, top note white = the white knob, others light blue)
 *   y  46..53  right: TOP note (white) and VOICING (orange) = the white and orange knobs
 *   y  54..61  the edited key (+ padlock while frozen); right: the chord that is sounding, if it is another one
 *   y  66..138 the 24 keys (F..E), mapped keys labelled root over quality; edited key outlined white, sounding dark blue
 * SHIFT held: the name / ruler area shows the map's four settings in knob colours instead.
 * Colours: blue / green / white / orange = the four knobs only; red = HOLD, LEARN, overwrite; white = edited / confirmed;
 * dark / light blue = sounding.
 */
#include "chord_font.h"
#include "chord2_model.h"

struct c2_ui {
    int mode;                       /* 0 off, 1 SHAPE, 2 MAP */
    int map;                        /* 1..8 */
    const char *map_name;
    struct c2_chord keys[C2_KEYS];
    struct c2_chord shape;
    int sel;                        /* edited key, -1 none */
    int locked;                     /* edit target frozen: a knob was turned < 2 s ago, or the key is held */
    uint32_t sounding;              /* keys sounding (held or latched) */
    int octave;                     /* TE's octave for the slot (-2..2): ruler and names follow it */
    int learning, learn_n, learn_over;   /* SHIFT held: notes so far (intervals from the edited key); > 4 played */
    int8_t learn_iv[C2_MAX];
    int strum, strum_dir;           /* ms from the first note to the last (0 off); 0 up 1 down */
    int smooth, bass, hold, flats;  /* per map: SMOOTH, BASS (root an octave under) */
    int seq;                        /* a sequencer / the arp takes the keys: chords are bypassed */
    int turned;                     /* last knob turned 0..3 (its readout is bright), -1 none */
    int playing;                    /* key whose chord sounds last (shown when it is not the edited key), -1 none */
    int shift;                      /* SHIFT held: the readouts show the SHIFT (map) layer */
    const char *flash;              /* a one-line message for ~2 s (CLEARED, MAX 4, ...), 0 none */
    int fill_src, fill_tonic, fill_scale, fill_type, fill_slot, fill_confirm;
    int voices;                     /* notes held / 6 */
};

/* ---- text with per-glyph advance ---- */
static fp32 ctext(void *r, const char *s, fp32 x, fp32 y, fp32 step, int colour, int fast)
{
    fp32 w = step >> 2 > FP(1) ? step >> 2 : FP(1);
    for (; *s; s++) {
        int c = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
        if (c < CFONT_FIRST || c > CFONT_LAST)
            continue;
        for (const uint8_t *g = cfont_seg + cfont_off[c - CFONT_FIRST]; *g != 0xFF; g += 2) {
            fp32 x0 = x + (g[0] >> 4) * step, y0 = y + (g[0] & 15) * step;
            fp32 x1 = x + (g[1] >> 4) * step, y1 = y + (g[1] & 15) * step;
            if (fast)
                line_fast(r, x0, y0, x1, y1, colour);
            else
                line_fp(r, x0, y0, x1, y1, w, colour);
        }
        x += cfont_adv[c - CFONT_FIRST] * step;
    }
    return x;
}

static fp32 ctext_w(const char *s, fp32 step)
{
    fp32 w = 0;
    for (; *s; s++) {
        int c = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
        if (c >= CFONT_FIRST && c <= CFONT_LAST)
            w += cfont_adv[c - CFONT_FIRST] * step;
    }
    return w > 2 * step ? w - 2 * step : w;   /* without the trailing gap */
}

static void ctext_c(void *r, const char *s, fp32 cx, fp32 y, fp32 step, int colour, int fast)
{
    ctext(r, s, cx - (ctext_w(s, step) >> 1), y, step, colour, fast);
}

static void fill_rect(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, int colour)
{
    for (fp32 x = x0; x <= x1; x += FP(3) >> 2)
        vline_fast(r, x, y0, y1, colour);
}

static void frame_fast(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, int colour)
{
    line_fast(r, x0, y0, x1, y0, colour);
    line_fast(r, x1, y0, x1, y1, colour);
    line_fast(r, x1, y1, x0, y1, colour);
    line_fast(r, x0, y1, x0, y0, colour);
}

static void frame_aa(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, fp32 w, int colour)
{
    line_fp(r, x0, y0, x1, y0, w, colour);
    line_fp(r, x1, y0, x1, y1, w, colour);
    line_fp(r, x1, y1, x0, y1, w, colour);
    line_fp(r, x0, y1, x0, y0, w, colour);
}

/* ---- the 24 keys ---- */
#define KB_X0 6
#define KB_WK 22                    /* white key width: 14 x 22 = 308 */
#define KB_BK 18
#define KB_Y0 66
#define KB_Y1 138
#define KB_YB 104                   /* black keys' bottom */
static const int8_t kb_white_of[24] = { 0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1,
                                        13 };   /* F F# G G# A A# B C C# D D# E | F .. E */

static int kb_is_black(int key) { return key >= 0 && key < 24 && kb_white_of[key] < 0; }

static fp32 kb_x(int key)           /* left edge */
{
    if (!kb_is_black(key))
        return FP(KB_X0 + KB_WK * kb_white_of[key]);
    return FP(KB_X0 + KB_WK * (kb_white_of[key - 1] + 1) - (KB_BK >> 1));
}

/* The visible outline of a white key (its top is narrowed by the black keys beside it), as 8 points. */
static void kb_white_shape(int key, fp32 *px, fp32 *py)
{
    fp32 xl = kb_x(key), xr = xl + FP(KB_WK);
    fp32 nl = kb_is_black(key - 1) ? FP(KB_BK >> 1) : 0, nr = kb_is_black(key + 1) ? FP(KB_BK >> 1) : 0;
    fp32 x[8] = { xl + nl, xr - nr, xr - nr, xr, xr, xl, xl, xl + nl };
    fp32 y[8] = { FP(KB_Y0), FP(KB_Y0), FP(KB_YB), FP(KB_YB), FP(KB_Y1), FP(KB_Y1), FP(KB_YB), FP(KB_YB) };
    for (int i = 0; i < 8; i++) {
        px[i] = x[i];
        py[i] = y[i];
    }
}

static void label2(void *r, const struct c2_ui *u, const struct c2_chord *keys, int key, fp32 cx, fp32 y, int colour,
                   fp32 step)
{
    const struct c2_chord *c = &keys[key];
    if (c->q == Q_NONE)
        return;
    char root[4], *p = root;
    int rt = c->root, q = c->q;
    if (q == Q_CUSTOM) {
        q = c2_recognise(c->iv, c->n, &rt);
        if (q == Q_CUSTOM)
            rt = c->iv[0];
    }
    const char *s = (u->flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(key + rt)];
    while (*s)
        *p++ = *s++;
    *p = 0;
    ctext_c(r, root, cx, y, step + (step >> 2), colour, 1);           /* root a size up: read first */
    ctext_c(r, q == Q_CUSTOM ? "?" : c2_q[q].tag, cx, y + 9 * step, step, colour, 1);
}

/* keys: the chords to label (the map, or a WRITE preview); sel: outlined key; sounding: dark-blue keys */
static void keyboard(void *r, const struct c2_ui *u, const struct c2_chord *keys, int sel, uint32_t sounding, int labels)
{
    /* sounding keys: dark blue (white keys in their visible shape) */
    for (int k = 0; k < C2_KEYS; k++) {
        if (!(sounding >> k & 1))
            continue;
        if (kb_is_black(k))
            fill_rect(r, kb_x(k) + FP(1), FP(KB_Y0) + FP(1), kb_x(k) + FP(KB_BK - 1), FP(KB_YB) - FP(1), DARKBLUE);
        else {
            fp32 px[8], py[8];
            kb_white_shape(k, px, py);
            fill_rect(r, px[0] + FP(1), FP(KB_Y0) + FP(1), px[1] - FP(1), FP(KB_YB), DARKBLUE);
            fill_rect(r, px[5] + FP(1), FP(KB_YB), px[4] - FP(1), FP(KB_Y1) - FP(1), DARKBLUE);
        }
    }
    /* white key separators: full height, or from the black key's bottom where one sits on the boundary */
    for (int i = 0; i <= 14; i++) {
        int black = 0;
        for (int k = 1; k < C2_KEYS; k++)
            black |= kb_is_black(k) && kb_white_of[k - 1] + 1 == i;
        vline_fast(r, FP(KB_X0 + KB_WK * i), black ? FP(KB_YB) : FP(KB_Y0), FP(KB_Y1), GREY);
    }
    line_fast(r, FP(KB_X0), FP(KB_Y0), FP(KB_X0 + KB_WK * 14), FP(KB_Y0), GREY);
    line_fast(r, FP(KB_X0), FP(KB_Y1), FP(KB_X0 + KB_WK * 14), FP(KB_Y1), GREY);
    for (int k = 0; k < C2_KEYS; k++)       /* black keys: black inside, grey frame, shorter */
        if (kb_is_black(k))
            frame_fast(r, kb_x(k), FP(KB_Y0), kb_x(k) + FP(KB_BK), FP(KB_YB), GREY);
    if (labels)
        for (int k = 0; k < C2_KEYS; k++) {
            int black = kb_is_black(k);
            int col = k == sel || (sounding >> k & 1) ? WHITE : GREY;
            fp32 cx = kb_x(k) + FP(black ? KB_BK >> 1 : KB_WK >> 1);
            label2(r, u, keys, k, cx, black ? FP(KB_Y0 + 4) : FP(KB_YB + 5), col, FP(1) + (FP(1) >> 2));
        }
    if (sel >= 0) {                      /* the edited key: its visible outline, 2 px, white */
        for (int d = 0; d < 2; d++) {
            fp32 in = FP(1) + d * (FP(3) >> 2);
            if (kb_is_black(sel)) {
                frame_fast(r, kb_x(sel) + in, FP(KB_Y0) + in, kb_x(sel) + FP(KB_BK) - in, FP(KB_YB) - in, WHITE);
                continue;
            }
            fp32 px[8], py[8];
            kb_white_shape(sel, px, py);
            static const int8_t sx[8] = { 1, -1, -1, -1, -1, 1, 1, 1 }, sy[8] = { 1, 1, 1, 1, -1, -1, 1, 1 };
            for (int i = 0; i < 8; i++) {
                px[i] += sx[i] * in;
                py[i] += sy[i] * in;
            }
            py[2] = py[3] = py[6] = py[7] = FP(KB_YB) + in;
            for (int i = 0; i < 8; i++)
                line_fast(r, px[i], py[i], px[(i + 1) & 7], py[(i + 1) & 7], WHITE);
        }
    }
}

/* ---- notes of a chord on a 3-octave ruler: a tick per semitone (C tall, black keys short and dim); marks under
 * the ticks: root blue, top note white, others light blue (learning: all red) ---- */
#define RU_X0 166
#define RU_Y0 24
#define RU_Y1 36
static void ruler(void *r, const int8_t *iv, int n, int key, int root_iv, int learning)
{
    int lo = 99;
    for (int i = 0; i < n; i++)
        lo = iv[i] < lo ? iv[i] : lo;
    int base = key + lo;            /* from the C at or below the lowest note */
    base -= c2_key_pc(base);
    for (int s = 0; s <= 36; s++) {
        int p = c2_mod12(s), black = (0x54A >> p) & 1;
        fp32 x = FP(RU_X0) + s * FP(4);
        vline_fast(r, x, FP(RU_Y0), black ? FP(RU_Y0 + 5) : p == 0 ? FP(RU_Y1) : FP(RU_Y0 + 8), black ? PURPLEGREY : GREY);
    }
    for (int i = 0; i < n; i++) {
        int s = key + iv[i] - base;
        if (s < 0 || s > 36)
            continue;
        fp32 x = FP(RU_X0) + s * FP(4);
        int root = c2_mod12(iv[i] - root_iv) == 0, top = i == n - 1;
        int col = learning ? RED : top ? WHITE : root ? BLUE : LIGHTBLUE;
        line_fp(r, x, FP(RU_Y1 + 1), x, FP(RU_Y1 + (root ? 8 : 7)), FP(3), col);
    }
}

static const int knob_col[4] = { BLUE, GREEN, WHITE, ORANGE };

static char *put_s(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_i(char *p, int v, int plus)
{
    if (v < 0) {
        *p++ = '-';
        v = -v;
    } else if (plus && v > 0)
        *p++ = '+';
    if (v >= 10) {
        int t = 0;
        while (v >= 10) {
            v -= 10;
            t++;
        }
        *p++ = (char)('0' + t);
    }
    *p++ = (char)('0' + v);
    *p = 0;
    return p;
}

/* "B4": a key-relative note (key + iv) at TE's octave, C4 = MIDI 60 */
static char *put_note(char *p, const struct c2_ui *u, int key_iv)
{
    int m = key_iv + 0x35 + 12 * u->octave, o = -1;
    p = put_s(p, (u->flats ? c2_pc_flat : c2_pc_sharp)[c2_mod12(m)]);
    while (m >= 12) {
        m -= 12;
        o++;
    }
    return put_i(p, o, 0);
}

static const char *mode_name(const struct c2_ui *u, char *b)
{
    if (u->mode == 2)
        put_i(put_s(b, "MAP "), u->map, 0);
    else
        put_s(b, u->mode == 1 ? "SHAPE" : "OFF");
    return b;
}

/* Right of the header: only what is on. HOLD boxed red (sampler.svg); BASS orange B + a low bar; SMOOTH white;
 * strum: a green glyph (its slope = direction) and the ms. */
static void status(void *r, const struct c2_ui *u)
{
    fp32 x = FP(312);
    char b[8];
    if (u->hold) {
        fp32 w = ctext_w("HOLD", FP(1) + (FP(1) >> 2));
        x -= w + FP(6);
        frame_aa(r, x, FP(4), x + w + FP(5), FP(16), FP(1), RED);
        ctext(r, "HOLD", x + FP(3), FP(6) + (FP(1) >> 1), FP(1) + (FP(1) >> 2), RED, 0);
        x -= FP(8);
    }
    if (u->bass) {
        put_s(b, "B");
        x -= ctext_w(b, FP(1) + (FP(1) >> 2));
        ctext(r, b, x, FP(6), FP(1) + (FP(1) >> 2), ORANGE, 1);
        line_fast(r, x, FP(15), x + FP(10), FP(15), ORANGE);
        x -= FP(8);
    }
    if (u->smooth) {
        x -= ctext_w("SMOOTH", FP(1));
        ctext(r, "SMOOTH", x, FP(7), FP(1), WHITE, 1);
        x -= FP(8);
    }
    if (u->strum) {
        put_i(b, u->strum, 0);
        x -= ctext_w(b, FP(1) + (FP(1) >> 2));
        ctext(r, b, x, FP(6), FP(1) + (FP(1) >> 2), GREEN, 1);
        x -= FP(11);
        for (int i = 0; i < 3; i++) {
            fp32 h = FP(u->strum_dir ? 3 * (2 - i) : 3 * i);
            vline_fast(r, x + FP(3 * i), FP(7) + h, FP(15), GREEN);
        }
    }
}

static void header(void *r, const struct c2_ui *u)
{
    char b[12];
    fp32 x = ctext(r, mode_name(u, b), FP(8), FP(5), FP(2), u->mode ? BLUE : GREY, 0);
    if (u->mode == 2)
        ctext(r, u->map_name, x + FP(4), FP(9), FP(1), GREY, 1);
    status(r, u);
}

static void padlock(void *r, fp32 x, fp32 y, int colour)   /* 5 x 7 */
{
    frame_fast(r, x, y + FP(3), x + FP(5), y + FP(7), colour);
    line_fast(r, x + FP(1), y + FP(3), x + FP(1), y + FP(1), colour);
    line_fast(r, x + FP(1), y + FP(1), x + FP(4), y + FP(1), colour);
    line_fast(r, x + FP(4), y + FP(1), x + FP(4), y + FP(3), colour);
}

/* A framed tag: the badge on our screens, the chip on TE's, LEARN on the page. Frame blue (MAP), white (SHAPE),
 * red (learning). x is the left edge, or the right edge when right != 0. */
static void tag(void *r, const char *s, fp32 x, fp32 y, fp32 step, int frame, int colour, int right, int backing)
{
    fp32 w = ctext_w(s, step), h = 6 * step, pad = step + FP(2), bw = w + 2 * pad, bh = h + 2 * step + FP(2);
    if (right)
        x -= bw;
    if (backing)
        fill_rect(r, x, y, x + bw, y + bh, 1 /* black */);
    frame_aa(r, x, y, x + bw, y + bh, FP(1), frame);
    ctext(r, s, x + pad, y + step + FP(1), step, colour, 0);
}

/* SHIFT layer: the map's four settings, large, in knob colours, in place of name and ruler. */
static void map_settings(void *r, const struct c2_ui *u)
{
    char b[4][12];
    const char *lab[4] = { "MODE", "STRUM", "SMOOTH", "BASS" };
    mode_name(u, b[0]);
    if (u->strum)
        put_s(put_i(b[1], u->strum, 0), u->strum_dir ? "MS DN" : "MS UP");   /* the whole spread */
    else
        put_s(b[1], "OFF");
    put_s(b[2], u->smooth ? "ON" : "OFF");
    put_s(b[3], u->bass ? "ON" : "OFF");
    for (int i = 0; i < 4; i++) {
        fp32 x = FP(8 + 78 * i);
        ctext(r, lab[i], x, FP(28), FP(1), GREY, 1);
        ctext(r, b[i], x, FP(38), FP(1) + (FP(1) >> 1), knob_col[i], u->turned != i);
    }
}

/* The edited chord (plain layer) or the map's settings (SHIFT). */
static void editor(void *r, const struct c2_ui *u, const struct c2_chord *c, int key)
{
    if (u->shift && !u->learning) {
        map_settings(r, u);
        return;
    }
    char name[12], rootname[4];
    int8_t iv[C2_NOTES];
    int n, rt = c->root, q = c->q;
    struct c2_chord t = { 0, Q_CUSTOM, 0, 0, { 0 }, 0, 0, 0 };
    const int8_t *src = c->iv;
    if (u->learning) {
        n = u->learn_n > C2_MAX ? C2_MAX : u->learn_n;
        for (int i = 0; i < n; i++)
            iv[i] = t.iv[i] = u->learn_iv[i];
        t.n = (uint8_t)n;
        c = &t;
        src = t.iv;
        q = Q_CUSTOM;
    } else
        n = c2_notes(c, u->bass, iv);
    if (q == Q_CUSTOM && (q = c2_recognise(src, c->n, &rt)) == Q_CUSTOM)
        rt = c->n ? src[0] : 0;
    put_s(rootname, (u->flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(key + rt)]);
    if (c->q == Q_NONE)
        ctext(r, "NOTE", FP(8), FP(24), FP(3), GREY, 0);
    else if (u->learning && !n)
        ctext(r, "...", FP(8), FP(24), FP(3), RED, 0);
    else {
        c2_name(name, c, key, u->flats, 0);
        int col = u->learning ? RED : WHITE;
        ctext(r, name, FP(8), FP(24), FP(3), col, 0);
        if (!u->learning) {             /* which knob edits which part of the name */
            fp32 wr = ctext_w(rootname, FP(3)), wn = ctext_w(name, FP(3));
            line_fp(r, FP(8), FP(47), FP(8) + wr, FP(47), FP(2), u->turned == 0 ? WHITE : BLUE);
            if (wn > wr + FP(6))
                line_fp(r, FP(8) + wr + FP(6), FP(47), FP(8) + wn, FP(47), FP(2), u->turned == 1 ? WHITE : GREEN);
        }
    }
    /* the key, frozen or not; learning: the red LEARN tag */
    char b[24];
    fp32 xe = FP(8);
    if (u->learning) {
        tag(r, "LEARN", FP(8), FP(52), FP(1) + (FP(1) >> 2), RED, RED, 0, 0);
        xe = FP(8) + ctext_w("LEARN", FP(1) + (FP(1) >> 2)) + FP(10);
    }
    put_note(b, u, key);
    xe = ctext(r, b, xe, FP(54), FP(1) + (FP(1) >> 2), u->learning ? RED : GREY, 1);
    if (u->locked && !u->learning) {
        padlock(r, xe + FP(1), FP(52), WHITE);
        xe += FP(8);
    }
    const char *msg = u->flash ? u->flash : u->learning && u->learn_over ? "MAX 4" :
                      !u->learning && c->q == Q_CUSTOM ? "AS PLAYED" : 0;
    if (msg)
        ctext(r, msg, xe + FP(12), FP(54), FP(1) + (FP(1) >> 2), u->flash ? WHITE : u->learning ? RED : GREY, 1);
    if (n)
        ruler(r, iv, n, key + 12 * u->octave, rt, u->learning);
    if (!u->learning && c->q != Q_NONE) {  /* TOP (white knob) and VOICING (orange knob), under the ruler */
        put_note(b, u, key + iv[n - 1]);   /* SMOOTH on: the register anchor a phrase starts from */
        fp32 x = ctext(r, b, FP(RU_X0), FP(46), FP(1) + (FP(1) >> 2), WHITE, u->turned != 2);
        if (c->q < Q_COUNT)
            ctext(r, c2_fam_name[c->fam], x + FP(8), FP(46), FP(1) + (FP(1) >> 2), ORANGE, u->turned != 3);
    }
    if (u->mode == 2 && u->playing >= 0 && u->playing != key && u->keys[u->playing].q != Q_NONE && !u->learning) {
        c2_name(name, &u->keys[u->playing], u->playing, u->flats, 0);
        fp32 w = ctext_w(name, FP(1) + (FP(1) >> 1));
        ctext(r, name, FP(312) - w, FP(53), FP(1) + (FP(1) >> 1), LIGHTBLUE, 0);
    }
}

/* CHORD page, MAP mode */
static void c2_page_map(void *r, const struct c2_ui *u)
{
    header(r, u);
    int k = u->sel < 0 ? 0 : u->sel;
    editor(r, u, &u->keys[k], k);
    keyboard(r, u, u->keys, u->sel, u->sounding, 1);
}

/* CHORD page, SHAPE mode: one shape on every key; the keyboard lights where its notes fall from the edited key */
static void c2_page_shape(void *r, const struct c2_ui *u)
{
    header(r, u);
    int k = u->sel < 0 ? 7 : u->sel;
    editor(r, u, &u->shape, k);
    int8_t iv[C2_NOTES];
    int n = c2_notes(&u->shape, u->bass, iv);
    uint32_t lit = 0;
    for (int i = 0; i < n; i++)
        if (k + iv[i] >= 0 && k + iv[i] < 24)
            lit |= 1u << (k + iv[i]);
    keyboard(r, u, u->keys, k, lit, 0);
}

/* WRITE page (SHIFT+DROP): what to write into which slot. Sources: a scale fill, a copy of another map (green
 * transposes it), EMPTY, or a factory map. The slot
 * (orange knob) is the big number; DROP writes (twice over a used slot), SPLIT cancels: their key icons, top right. */
enum { SRC_SCALE, SRC_MAP, SRC_EMPTY, SRC_FACTORY };
static const char *const c2_factory_name[4] = { "DIATONIC C", "II-V-I", "NEO SOUL", "POP" };
static void c2_page_fill(void *r, const struct c2_ui *u, const struct c2_chord *preview)
{
    char b[24], *p;
    ctext(r, "WRITE", FP(8), FP(5), FP(2), WHITE, 0);
    if (u->fill_confirm) {
        fp32 w = ctext_w("OVERWRITE? ]", FP(1) + (FP(1) >> 2));
        ctext(r, "OVERWRITE? ]", FP(312) - w, FP(7), FP(1) + (FP(1) >> 2), RED, 1);
    } else {
        fp32 w = ctext_w("]  \\X", FP(1) + (FP(3) >> 2));
        ctext(r, "]  \\X", FP(312) - w, FP(6), FP(1) + (FP(3) >> 2), GREY, 1);
    }
    put_i(b, u->fill_slot, 0);
    fp32 w = ctext_w(b, FP(3));
    ctext(r, b, FP(312) - w, FP(24), FP(3), ORANGE, 0);
    ctext(r, "MAP", FP(312) - w - ctext_w("MAP", FP(1) + (FP(1) >> 2)) - FP(5), FP(35), FP(1) + (FP(1) >> 2), ORANGE, 1);
    int sc = u->fill_src == SRC_SCALE;
    if (sc) {
        p = put_s(b, (u->flats ? c2_pc_flat : c2_pc_sharp)[u->fill_tonic]);
        p = put_s(p, " ");
        put_s(p, c2_scale_name[u->fill_scale]);
    } else if (u->fill_src == SRC_MAP) {
        p = put_i(put_s(b, "MAP "), u->fill_scale, 0);
        if (u->fill_tonic)
            put_i(put_s(p, " "), u->fill_tonic, 1);
    } else
        put_s(b, u->fill_src == SRC_EMPTY ? "EMPTY" : c2_factory_name[u->fill_tonic & 3]);
    ctext(r, b, FP(8), FP(24), FP(3), WHITE, 0);
    /* FROM (blue), KEY or WHICH (green), SCALE (white): one line, knob colours */
    char tr[8];
    put_s(put_i(tr, u->fill_tonic, 1), " ST");
    int cp = u->fill_src == SRC_MAP;
    const char *val[3] = { sc ? (u->fill_type ? "7THS" : "TRIADS") : cp ? "MAP" : u->fill_src == SRC_EMPTY ? "EMPTY" : "FACTORY",
                           sc ? (u->flats ? c2_pc_flat : c2_pc_sharp)[u->fill_tonic] : cp ? tr :
                           u->fill_src == SRC_FACTORY ? "<>" : "",
                           sc ? c2_scale_name[u->fill_scale] : cp ? "<>" : "" };
    fp32 x = FP(8);
    for (int i = 0; i < 3; i++) {
        if (!*val[i])
            continue;
        line_fp(r, x, FP(56), x + FP(6), FP(56), FP(2), knob_col[i]);
        x = ctext(r, val[i], x + FP(9), FP(53), FP(1) + (FP(1) >> 2), knob_col[i], u->turned != i) + FP(10);
    }
    keyboard(r, u, preview, -1, 0, 1);
}

/* On our engines' screens (replaces CHORD1's badge, under HOLD's box) and on TE's screens (needs the frame hook,
 * probe 1: top right, black backing, while a chord sounds and ~1 s after): one tag. */
static void c2_badge(void *r, const struct c2_ui *u, const char *last)
{
    char b[16], *p = b;
    if (u->mode == 2)
        p = put_s(put_i(p, u->map, 0), " ");
    put_s(p, u->learning ? "LEARN" : u->seq ? "SEQ" : last ? last : "");
    tag(r, b, FP(130), FP(24), FP(3) >> 1, u->learning ? RED : u->mode == 2 ? BLUE : WHITE, u->learning ? RED : WHITE,
        0, 0);
}

/* name = the sounding chord, or 0 when silent: then the mode, dim (so a lead slot left in MAP is visible) */
static void c2_chip(void *r, const struct c2_ui *u, const char *name, int fade)
{
    char b[12];
    if (u->seq)
        name = "SEQ";
    if (!name) {
        tag(r, mode_name(u, b), FP(314), FP(4), FP(1) + (FP(1) >> 2), GREY, GREY, 1, 1);
        return;
    }
    tag(r, name, FP(314), FP(3), FP(2), u->mode == 2 ? BLUE : WHITE, fade ? GREY : WHITE, 1, 1);
}
