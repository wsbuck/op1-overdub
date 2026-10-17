/* CHORD v2 screens (notes/chord2.md "Screens"): the CHORD page, the badge on our engine screens and the chip on TE's.
 * Included by ui.c (device) and test/chord2_screens.c (host gallery, same code). Uses ui.c's primitives: line_fp
 * (AGG, ~20-35 k cycles), line_fast / vline_fast (1-px framebuffer lines, ~free), Q22.10 SVG units, 320 x 160,
 * safe area x 4..316, y 2..140. Immediate mode (no SVG), so it draws over any TE screen. No division, no floats.
 *
 * Page layout (MAP):
 *   y   4..18  header: MAP n (blue) and the map's name; right: HOLD (boxed red), B (orange, BASS)
 *   y  21..50  the edited chord's name, big (x4), white (spelled out when it fits); blue underline = root (blue
 *              knob), green = quality (green knob); while green turns, the TYPE scan strip under it
 *   y  22..48  right: TOP (the top note, white knob) and VOICING (orange knob)
 *   y  54..61  the edited key (+ blue padlock while pinned), a message; right: the chord sounding, if another one
 *   y  66..138 the 24 keys (F..E), mapped keys labelled root over quality; edited key outlined white (blue: pinned),
 *              sounding dark blue. The notes view (a chord knob turning and ~1 s after, a learn, SHAPE): the edited
 *              chord's notes lit and named instead (root blue, top white, others light blue; a learn red), notes off
 *              the keyboard as an arrow at that edge (+ count)
 * SHIFT held: the map's settings on the knobs and the diagnostic lines (the morning test reads them).
 * Colours: blue / green / white / orange = the four knobs; red = HOLD, LEARN; white = edited / confirmations;
 * dark / light blue = sounding.
 */
#include "chord_font.h"
#include "chord2.h"

struct c2_view {                    /* what the screens need besides the state */
    int slot;                       /* synth slot 0..7, -1 unknown */
    int octave;                     /* TE's octave for the slot (-2..2): names and the ruler follow it */
    int hold;                       /* HOLD on */
    uint32_t held;                  /* keys HOLD latches */
    int shift;                      /* SHIFT held */
    uint32_t now;                   /* c2.clock */
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

static void c2u_fill(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, int colour)
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

static char *c2u_s(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *c2u_i(char *p, int32_t v)       /* decimal, no division */
{
    static const int32_t pw[5] = { 10000, 1000, 100, 10, 1 };
    if (v < 0) {
        *p++ = '-';
        v = -v;
    }
    if (v > 99999)
        v = 99999;
    int started = 0;
    for (int i = 0; i < 5; i++) {
        int d = 0;
        while (v >= pw[i]) {
            v -= pw[i];
            d++;
        }
        if (d || started || i == 4) {
            *p++ = (char)('0' + d);
            started = 1;
        }
    }
    *p = 0;
    return p;
}

static char *c2u_hex(char *p, int32_t v)     /* 0x0..0xFFF as 2 or 3 hex digits */
{
    for (int sh = v > 0xFF ? 8 : 4; sh >= 0; sh -= 4) {
        int d = (v >> sh) & 15;
        *p++ = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    *p = 0;
    return p;
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

static void label2(void *r, const struct c2_chord *c, int key, int flats, fp32 cx, fp32 y, int colour, fp32 step,
                   fp32 maxw)
{
    int rt, dist, q = c2_shown2(c, &rt, &dist);
    char dt[8];
    if (q == Q_NONE)
        return;
    if (q == Q_DYAD)
        c2_dyad_tag(dt, dist);
    const char *tag = q == Q_DYAD ? dt : q == Q_CUSTOM ? "?" : c2_q[q].tag;
    const char *root = (flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(key + rt)];
    fp32 ts = q == Q_DYAD ? FP(1) : step, rs = step + (step >> 2);   /* intervals one size; long labels shrink */
    fp32 ty = y + 9 * step;
    if (q != Q_DYAD && tag[0] && !tag[1]) {
        ts = FP(3) >> 1;               /* one glyph ("-", dim): bigger, so it doesn't read as a stray mark */
        ty += FP(1);
    }
    while (ctext_w(tag, ts) > maxw && ts > (FP(3) >> 2))
        ts -= FP(1) >> 3;
    while (ctext_w(root, rs) > maxw && rs > step)
        rs -= FP(1) >> 3;
    ctext_c(r, root, cx, y, rs, colour, 1);
    ctext_c(r, tag, cx, ty, ts, colour, 1);
}

/* keys: the chords to label (0 = none); sel: outlined key (selcol); lit: dark-blue keys (sounding); notecol: the
 * notes view instead (per key a fill colour, 0 = none; lit is then ignored) */
static void keyboard(void *r, const struct c2_chord *keys, int flats, int sel, uint32_t lit, int selcol,
                     const uint8_t *notecol)
{
    for (int k = 0; k < C2_KEYS; k++) {
        int col = notecol ? notecol[k] : (lit >> k & 1) ? DARKBLUE : 0;
        if (!col)
            continue;
        if (kb_is_black(k))
            c2u_fill(r, kb_x(k) + FP(1), FP(KB_Y0) + FP(1), kb_x(k) + FP(KB_BK - 1), FP(KB_YB) - FP(1), col);
        else {
            fp32 px[8], py[8];
            kb_white_shape(k, px, py);
            c2u_fill(r, px[0] + FP(1), FP(KB_Y0) + FP(1), px[1] - FP(1), FP(KB_YB), col);
            c2u_fill(r, px[5] + FP(1), FP(KB_YB), px[4] - FP(1), FP(KB_Y1) - FP(1), col);
        }
    }
    for (int i = 0; i <= 14; i++) {   /* white key separators: from the black key's bottom where one sits there */
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
    if (notecol)                    /* the notes view: each lit key named, black on its colour */
        for (int k = 0; k < C2_KEYS; k++)
            if (notecol[k]) {
                int black = kb_is_black(k);
                const char *nm = (flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(k)];
                fp32 st = FP(1) + (FP(1) >> 2);
                while (ctext_w(nm, st) > FP(KB_BK - 7) && st > FP(1))
                    st -= FP(1) >> 3;
                ctext_c(r, nm, kb_x(k) + FP(black ? KB_BK >> 1 : KB_WK >> 1), black ? FP(KB_Y0 + 24) : FP(KB_YB + 18),
                        st, 1 /* black */, 1);
            }
    if (keys)                       /* with the notes lit, the other keys keep their chords, dim */
        for (int k = 0; k < C2_KEYS; k++) {
            if (notecol && notecol[k])
                continue;
            int black = kb_is_black(k);
            int col = notecol ? GREY : k == sel || (lit >> k & 1) ? WHITE : GREY;
            fp32 cx = kb_x(k) + FP(black ? KB_BK >> 1 : KB_WK >> 1);
            label2(r, &keys[k], k, flats, cx, black ? FP(KB_Y0 + 4) : FP(KB_YB + 5), col, FP(1) + (FP(1) >> 2),
                   k == sel ? (black ? FP(KB_BK - 8) : FP(KB_WK - 8)) : black ? FP(KB_BK - 6) : FP(KB_WK - 6));
        }
    if (sel >= 0 && sel < C2_KEYS) {   /* the edited key: its visible outline, 2 px, white (blue: pinned) */
        for (int d = 0; d < 2; d++) {
            fp32 in = FP(1) + d * (FP(3) >> 2);
            if (kb_is_black(sel)) {
                frame_fast(r, kb_x(sel) + in, FP(KB_Y0) + in, kb_x(sel) + FP(KB_BK) - in, FP(KB_YB) - in, selcol);
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
                if (!(d == 1 && px[1] - px[0] < FP(8) && (i == 7 || i == 0 || i == 1)))   /* a narrow neck: 1 pass */
                    line_fast(r, px[i], py[i], px[(i + 1) & 7], py[(i + 1) & 7], selcol);
        }
    }
}

/* ---- the notes view: a chord's notes lit on the 24 keys (root blue, top note white, others light blue; a learn all
 * red); notes off the keyboard counted per side (an arrow at that edge) ---- */
static void c2u_notecols(uint8_t *col, const int8_t *iv, int n, int key, int root_iv, int learning, int *below,
                         int *above)
{
    *below = *above = 0;
    for (int k = 0; k < C2_KEYS; k++)
        col[k] = 0;
    for (int i = 0; i < n; i++) {
        int k = key + iv[i];
        if (k < 0) {
            ++*below;
            continue;
        }
        if (k >= C2_KEYS) {
            ++*above;
            continue;
        }
        int root = c2_mod12(iv[i] - root_iv) == 0, top = i == n - 1;
        col[k] = (uint8_t)(learning ? RED : top ? WHITE : root ? BLUE : LIGHTBLUE);
    }
}

/* An arrow at the keyboard's left (dir -1) or right edge for notes off it, the count when more than one. */
static void c2u_offkb(void *r, int dir, int count, int colour)
{
    if (!count)
        return;
    fp32 y = colour == 1 ? FP(KB_YB - 8) : FP(KB_YB + 6),   /* black = on a lit key: up beside the black keys */ x = dir < 0 ? FP(KB_X0 + 3) : FP(KB_X0 + KB_WK * 14 - 3), d = dir < 0 ? FP(4) : -FP(4);
    line_fast(r, x, y, x + d, y - FP(3), colour);
    line_fast(r, x, y, x + d, y + FP(3), colour);
    line_fast(r, x, y, x + 2 * d, y, colour);
    if (count > 1) {
        char b[4];
        c2u_i(b, count);
        if (dir < 0)
            ctext(r, b, x + 2 * d + FP(2), y - FP(3), FP(1), colour, 1);
        else
            ctext(r, b, x + 2 * d - FP(2) - ctext_w(b, FP(1)), y - FP(3), FP(1), colour, 1);
    }
}

static const int c2u_knob_col[4] = { BLUE, GREEN, WHITE, ORANGE };

/* "B4": a key-relative note (key + iv) at TE's octave, C4 = MIDI 60 */
static char *c2u_note(char *p, int octave, int flats, int key_iv)
{
    int m = key_iv + 0x35 + 12 * octave, o = -1;
    p = c2u_s(p, (flats ? c2_pc_flat : c2_pc_sharp)[c2_mod12(m)]);
    while (m >= 12) {
        m -= 12;
        o++;
    }
    return c2u_i(p, o);
}

static const char *c2u_mode_name(int mode, char *b)
{
    int m = c2_mode_map(mode);
    if (m >= 0)
        c2u_i(c2u_s(b, "MAP "), m + 1);
    else
        c2u_s(b, mode == 1 ? "SHAPE" : "OFF");
    return b;
}

static void padlock(void *r, fp32 x, fp32 y, int colour, int open)   /* 5 x 7; open: the shackle up, its right leg out */
{
    fp32 up = open ? FP(2) : 0;
    frame_fast(r, x, y + FP(3), x + FP(5), y + FP(7), colour);
    line_fast(r, x + FP(1), y + FP(3), x + FP(1), y + FP(1) - up, colour);
    line_fast(r, x + FP(1), y + FP(1) - up, x + FP(4), y + FP(1) - up, colour);
    line_fast(r, x + FP(4), y + FP(1) - up, x + FP(4), y + FP(3) - up - (open ? FP(1) : 0), colour);
}

/* A framed tag: the badge on our screens, the chip on TE's, LEARN on the page. x is the left edge, or the right edge
 * when right != 0. */
static void c2u_tag(void *r, const char *s, fp32 x, fp32 y, fp32 step, int frame, int colour, int right, int backing)
{
    int fast = backing >> 1;        /* backing 2|1: the text and frame in fast lines (the idle chip: 0 AA) */
    backing &= 1;
    fp32 w = ctext_w(s, step), h = 6 * step, pad = step + FP(2), bw = w + 2 * pad, bh = h + 2 * step + FP(2);
    if (right)
        x -= bw;
    if (backing)
        c2u_fill(r, x, y, x + bw, y + bh, 1 /* black */);
    if (fast)
        frame_fast(r, x, y, x + bw, y + bh, frame);
    else
        frame_aa(r, x, y, x + bw, y + bh, FP(1), frame);
    ctext(r, s, x + pad, y + step + FP(1), step, colour, fast);
}

static int c2u_mode(const struct c2_state *s, const struct c2_view *v)
{
    if (v->slot < 0 || v->slot >= C2_SLOTS)
        return 0;
    int m = s->d.mode[v->slot];
    return m < C2_MODE_COUNT ? m : 0;
}

/* The chord a key plays in the slot's mode (0 = its note). */
static const struct c2_chord *c2u_chord(const struct c2_state *s, const struct c2_view *v, int key)
{
    int mode = c2u_mode(s, v), m = c2_mode_map(mode);
    if (key < 0 || key >= C2_KEYS)
        return 0;
    if (m >= 0)
        return &s->d.maps[m][key];
    if (mode == 1)
        return &s->d.shape[v->slot];
    return 0;
}

static int c2u_flats(const struct c2_state *s, const struct c2_view *v)
{
    int m = c2_mode_map(c2u_mode(s, v));
    return m >= 0 ? s->d.set[m].flats != 0 : 0;
}

/* Header: mode (blue MAP, white SHAPE, grey OFF) and the map's name; right: HOLD boxed red, BASS. */
static void c2u_header(void *r, const struct c2_state *s, const struct c2_view *v)
{
    char b[16];
    int mode = c2u_mode(s, v), m = c2_mode_map(mode);
    fp32 x = ctext(r, c2u_mode_name(mode, b), FP(8), FP(5), FP(2), mode ? BLUE : GREY, 0);   /* SHIFT+blue sets it */
    if (m >= 0 && s->d.set[m].name[0]) {
        char nm[13];
        for (int i = 0; i < 12; i++)
            nm[i] = s->d.set[m].name[i];
        nm[12] = 0;
        ctext(r, nm, x + FP(4), FP(11), FP(1), GREY, 1);
    }
    fp32 xr = FP(312);
    int wr = s->wr_open != 0;
    if (v->hold) {
        fp32 w = ctext_w("HOLD", FP(1) + (FP(1) >> 2));
        xr -= w + FP(6);
        frame_aa(r, xr, FP(4), xr + w + FP(5), FP(16), FP(1), RED);
        ctext(r, "HOLD", xr + FP(3), FP(6) + (FP(1) >> 1), FP(1) + (FP(1) >> 2), RED, 1);
        xr -= FP(16);
    }
    fp32 hs = FP(1) + (FP(1) >> 2);
    if (m >= 0 && s->d.set[m].bass && !wr) {        /* the map's settings that are on: B (orange), SM (white), strum */
        xr -= ctext_w("B", hs);
        ctext(r, "B", xr, FP(6), hs, ORANGE, 1);
        xr -= FP(6);
    }
    if (m >= 0 && s->d.set[m].smooth && !wr) {
        xr -= ctext_w("SM", hs);         /* white while it leads on from the last chord, grey when it starts over */
        ctext(r, "SM", xr, FP(6), hs, s->smooth_n > 0 && s->smooth_map == m ? WHITE : GREY, 1);
        xr -= FP(6);
    }
    if (m >= 0 && s->d.set[m].strum && !wr) {
        char b2[8], *p = b2;
        int st = s->d.set[m].strum;
        *p++ = st > 0 ? '{' : '}';
        c2u_i(p, c2_strum_spread(st));
        xr -= ctext_w(b2, hs);
        ctext(r, b2, xr, FP(6), hs, s->seq_live >= 4 ? GREEN : GREY, 1);   /* grey: no tick seen (plays at once) */
        xr -= FP(6);
    }
    if (v->shift && m >= 0 && !wr && !(s->learning && s->learn_page)) {   /* SHIFT layer: the map's own actions */
        const char *h = "\\ PIN  SHIFT+] WRITE";
        ctext(r, h, xr - FP(6) - ctext_w(h, FP(1)), FP(11), FP(1), GREY, 1);
    }
}

static const char *c2u_msg(const struct c2_state *s, int mode, char *b)
{
    switch (s->msg) {
    case MSG_CLEARED: return "CLEARED  SHIFT+[ UNDOES";
    case MSG_COPIED: return "COPIED  ] PASTES";
    case MSG_PASTED: return "PASTED  SHIFT+[ UNDOES";
    case MSG_UNDONE: return "UNDONE  AGAIN = REDO";
    case MSG_REDONE: return "REDONE";
    case MSG_PINNED: return "PINNED  \\ AGAIN UNPINS";
    case MSG_UNPINNED: return "UNPINNED: LAST KEY EDITS";
    case MSG_WALL: return "AGAIN = NOTE";
    case MSG_LEARN_CANCEL: return "LEARN CANCELLED";
    case MSG_GREEN_FIRST: return "TURN GREEN FIRST";
    case MSG_FACTORY_ASK:               /* maps 5..8 have no factory chords: they restore to empty */
        c2u_s(c2u_i(c2u_s(b, s->msg_arg < 4 ? "MAP " : "CLEAR MAP "), s->msg_arg + 1),
              s->msg_arg < 4 ? " TO FACTORY: ] AGAIN" : ": ] AGAIN");
        return b;
    case MSG_FACTORY_DONE: return s->msg_arg < 4 ? "FACTORY  SHIFT+[ UNDOES" : "CLEARED  SHIFT+[ UNDOES";
    case MSG_FACTORY_NOMAP: return "SHIFT+BLUE: PICK A MAP";
    case MSG_LEARNED: return "LEARNED  SHIFT+[ UNDOES";
    case MSG_MAX4: return "MAX 5: BASS + TOP 4";
    case MSG_EMPTY_CLIP: return "[ COPIES FIRST";
    case MSG_LATER: return "NOT IN THIS VERSION";
    case MSG_STRUM:
        if (!s->msg_arg)
            return "STRUM OFF";
        c2u_s(c2u_i(c2u_s(b, s->msg_arg > 0 ? "STRUM { LOW>HIGH " : "STRUM } HIGH>LOW "), c2_strum_spread(s->msg_arg)),
              " MS");
        return b;
    case MSG_NO_TICK: return "STRUM: NO SEQ TICK, PLAYS AT ONCE";
    case MSG_VOI_HINT: return "UP KEEPS THE BASS, DROPS ARE LEFT";
    case MSG_SMOOTH: return s->msg_arg ? "SMOOTH ON: VOICE-LEADS" : "SMOOTH OFF";
    case MSG_WRITTEN: c2u_s(c2u_i(c2u_s(b, "WRITTEN: MAP "), s->msg_arg + 1), "  SHIFT+[ UNDOES"); return b;
    case MSG_OVERWRITE: c2u_s(c2u_i(c2u_s(b, "OVERWRITE MAP "), s->msg_arg + 1), "? ] AGAIN"); return b;
    case MSG_WRITE_CANCEL: return "WRITE CANCELLED";
    case MSG_VOI_END: return s->msg_arg > 0 ? "WIDEST UP" : "WIDEST DOWN";
    case MSG_TURN_GREEN: return mode ? "TURN GREEN FOR A CHORD" : "SHIFT+BLUE: MODE";
    case MSG_NOTHING_UNDO: return "NOTHING TO UNDO";
    case MSG_MODE: return c2u_mode_name(s->msg_arg, b);
    case MSG_BASS: return s->msg_arg ? "BASS ON" : "BASS OFF";
    default: return 0;
    }
}

/* SHIFT layer: the map's settings in knob colours, and the diagnostics (the device test reads them). */
static void c2u_diag_line(void *r, const char *b, fp32 y)
{
    fp32 st = FP(3) >> 2;
    while (ctext_w(b, st) > FP(306) && st > (FP(1) >> 1))
        st -= FP(1) >> 3;
    ctext(r, b, FP(8), y, st, GREY, 1);
}

static const char *const c2u_load_name[5] = { "-", "NONE", "A", "B", "BAD" };
static const char *const c2u_save_name[6] = { "-", "OK", "OPEN", "WRITE", "CLOSE", "TORN" };
static void c2u_shift_layer(void *r, const struct c2_state *s, const struct c2_view *v)
{
    char b[96], *p;
    int mode = c2u_mode(s, v), m = c2_mode_map(mode);
    const char *lab[4] = { "MODE", "STRUM", "SMOOTH", "BASS" };
    char sb[12], *q = sb;
    int st = m < 0 ? 0 : s->d.set[m].strum;
    if (st) {
        *q++ = st > 0 ? '{' : '}';
        c2u_s(c2u_i(q, c2_strum_spread(st)), " MS");
    } else
        c2u_s(sb, m < 0 ? "-" : "OFF");
    const char *val[4] = { c2u_mode_name(mode, b), sb, m < 0 ? "-" : s->d.set[m].smooth ? "ON" : "OFF",
                           m < 0 ? "-" : s->d.set[m].bass ? "ON" : "OFF" };
    for (int i = 0; i < 4; i++) {
        fp32 x = FP(8 + 78 * i);
        ctext(r, lab[i], x, FP(23), FP(1), GREY, 1);
        int off = m < 0 ? i > 0 : (i == 1 && !st) || (i == 2 && !s->d.set[m].smooth) || (i == 3 && !s->d.set[m].bass);
        ctext(r, val[i], x, FP(31), FP(1) + (FP(1) >> 2), off ? GREY : c2u_knob_col[i], 1);
    }
    const struct c2_diag *g = &s->dg;
    p = c2u_s(b, "HOOK F:");
    p = c2u_i(p, (int32_t)(g->frames & 0xFFFF));
    p = c2u_i(c2u_s(p, " K:"), (int32_t)g->knobs);
    p = c2u_i(c2u_s(p, " D:"), g->knob_delta);
    p = c2u_i(c2u_s(p, " SP:"), (int32_t)g->split);
    p = c2u_i(c2u_s(p, " LI:"), (int32_t)g->lift);
    p = c2u_i(c2u_s(p, " DR:"), (int32_t)g->drop);
    p = c2u_i(c2u_s(p, " UP:"), (int32_t)g->shift_up);
    p = c2u_i(c2u_s(p, " DN:"), (int32_t)g->shift_dn);
    c2u_s(p, g->ctl_synth ? " CT" : " CT0");
    c2u_diag_line(r, b, FP(41));
    p = c2u_s(b, "FILE ");
    p = c2u_s(p, c2u_load_name[g->load >= 0 && g->load < 5 ? g->load : 0]);
    if (g->load_ver)
        p = c2u_i(c2u_s(p, "V"), g->load_ver);
    p = c2u_i(c2u_s(p, " L:"), (int32_t)s->load_tries);
    p = c2u_i(c2u_s(p, " "), g->load_seq);
    p = c2u_i(c2u_s(p, " SAVE "), (int32_t)g->saves);
    p = c2u_s(c2u_s(p, " "), c2u_save_name[g->save_err >= 0 && g->save_err < 6 ? g->save_err : 0]);
    p = c2u_i(c2u_s(p, " MK:"), g->mkdir_ret);
    p = c2u_i(c2u_s(p, " VOX "), g->voices);
    int held = 0;
    for (int k = 0; k < C2_KEYS; k++)
        held += (int)(v->held >> k & 1);
    p = c2u_i(c2u_s(p, " H:"), held);
    p = c2u_i(c2u_s(p, " SHED "), (int32_t)g->sheds);
    p = c2u_i(c2u_s(p, " PG:"), (int32_t)g->purges);
    if (g->fb_frames)
        c2u_s(p, " FB");
    c2u_diag_line(r, b, FP(47));
    /* learn: starts, written, cancelled, blocked; the last end / block reason (C2_LE_*); the last combo key */
    p = c2u_i(c2u_s(b, "LEARN S:"), (int32_t)g->learn_starts);
    p = c2u_i(c2u_s(p, " OK:"), (int32_t)g->learn_ok);
    p = c2u_i(c2u_s(p, " X:"), (int32_t)g->learn_cancels);
    p = c2u_i(c2u_s(p, " B:"), (int32_t)g->learn_blocked);
    p = c2u_s(p, " END:");
    *p++ = (char)(g->learn_end >= 'A' && g->learn_end <= 'Z' ? g->learn_end : '-');
    p = c2u_s(p, " CB:");
    if (g->combo_key < 0)
        p = c2u_s(p, "-");
    else
        p = c2u_hex(p, g->combo_key);
    c2u_s(p, s->shift_used ? " USED" : "");
    c2u_diag_line(r, b, FP(53));
    /* STRUM's sequencer tick: rate per ~1 s, the longest gap (ms.tenths), who sends, notes sent / forced, takeovers,
     * SMOOTH resets */
    uint32_t gms = (g->tick_gap * 21u) >> 14, gt = ((g->tick_gap * 210u) >> 14) - gms * 10u;
    p = c2u_i(c2u_s(b, "SEQ TK:"), (int32_t)g->tick_rate);
    p = c2u_i(c2u_s(p, " GAP:"), (int32_t)gms);
    *p++ = '.';
    p = c2u_i(p, (int32_t)(gt < 10 ? gt : 9));
    p = c2u_s(p, s->seq_live >= 4 ? " LIVE" : " DEAD");
    p = c2u_i(c2u_s(p, " OWN:"), (int32_t)s->owner);
    p = c2u_i(c2u_s(p, " SENT:"), (int32_t)g->strum_sent);
    p = c2u_i(c2u_s(p, " FRC:"), (int32_t)g->strum_forced);
    p = c2u_i(c2u_s(p, " TO:"), (int32_t)g->takeovers);
    c2u_i(c2u_s(p, " SR:"), (int32_t)g->smooth_resets);
    c2u_diag_line(r, b, FP(59));
}

/* The TYPE scan strip, in place of the green underline while green turns: every quality a tick, family starts taller,
 * the current one green (the Model:Cycles-like position in the list). */
static void c2u_scan(void *r, int q, fp32 x0, fp32 y)
{
    int cur = c2_scan_pos(q);
    for (int pos = C2_SCAN_MIN; pos <= C2_SCAN_MAX; pos++) {
        fp32 x = x0 + (pos - C2_SCAN_MIN) * FP(3);
        int i = c2_scan_q(pos);
        int fam = pos == C2_SCAN_MIN || i == Q_MAJ || i == Q_6 || i == Q_SUS4 || i == Q_ADD9 || i == Q_7S11;
        if (pos == cur)
            line_fp(r, x, y - FP(2), x, y + FP(4), FP(3), GREEN);
        else if (!pos)
            vline_fast(r, x, y - FP(1), y + FP(5), WHITE);   /* NOTE: the walls either side */
        else if (fam)
            vline_fast(r, x, y, y + FP(4), PURPLEGREY);
        else
            vline_fast(r, x, y + FP(1), y + FP(3), GREY);
    }
}

/* The VOICING strip under its name: 9 positions, the centre (CLOSE / AS PLAYED) tall, the ones this chord has grey,
 * the current one orange; right-aligned at xr. */
static void c2u_voi_strip(void *r, const struct c2_chord *c, fp32 xr, fp32 y)
{
    for (int v = C2_VOI_MIN; v <= C2_VOI_MAX; v++) {
        fp32 x = xr - (C2_VOI_MAX - v) * FP(4);
        if (v == c->voi)
            line_fp(r, x, y - FP(1), x, y + FP(3), FP(2), ORANGE);
        else if (v && c2_voi_applies(c, v))
            vline_fast(r, x, y, y + FP(3), GREY);
        else if (v)
            vline_fast(r, x, y + FP(2), y + FP(3), PURPLEGREY);   /* not for this chord: a stub keeps the rhythm */
    }
    vline_fast(r, xr - C2_VOI_MAX * FP(4), y - FP(3), y + FP(5), WHITE);   /* the centre, above and below the bar */
}

/* The chord the page shows for the edited key: during a learn on the page, the notes played so far (in t). */
static const struct c2_chord *c2u_edited(const struct c2_state *s, const struct c2_view *v, int key, struct c2_chord *t)
{
    if (!(s->learning && s->learn_page))
        return c2u_chord(s, v, key);
    c2_clear(t);
    int cnt = 0;
    for (int i = 0; i < s->learn_n && cnt < C2_MAX; i++) {
        int32_t d = s->learn_note[i] - s->learn_base;
        t->iv[cnt++] = (int8_t)(d > C2_IV_MAX ? C2_IV_MAX : d < -C2_IV_MAX ? -C2_IV_MAX : d);
    }
    c2_sort(t->iv, cnt);
    t->q = cnt ? Q_CUSTOM : Q_NONE;
    t->n = (uint8_t)cnt;
    return t;
}

static int c2u_chord_notes(const struct c2_state *s, const struct c2_view *v, const struct c2_chord *c, int8_t *iv,
                           int *rt)
{
    int bi, m = c2_mode_map(c2u_mode(s, v));
    *rt = 0;
    if (!c || c2_shown(c, rt) == Q_NONE)
        return 0;
    return c2_notes(c, m >= 0 && s->d.set[m].bass, iv, &bi);
}

/* The keyboard shows the edited chord's notes (instead of the map's names) while a chord knob turns and ~1 s after,
 * after a learn and during a learn on the page. */
static int c2u_notes_view(const struct c2_state *s, const struct c2_view *v)
{
    if (s->learning && s->learn_page)
        return 1;
    if (!s->wr_open && s->turned >= 0 && s->turned <= 3 && s->knob_frame && v->now - s->knob_frame <= C2_NOTES_SHOW)
        return 1;
    return (s->msg == MSG_LEARNED || s->msg == MSG_MAX4) && v->now - s->msg_frame <= C2_NOTES_SHOW + 15;
}

/* The played-notes view: every note that sounds (chords and plain notes) while keys are held and ~0.5 s after the
 * last key-up; then the map view again (a HOLD pad keeps sounding: its keys stay dark blue there). */
static int c2u_played_view(const struct c2_state *s, const struct c2_view *v)
{
    if (!(s->view_rel[0] | s->view_rel[1] | s->view_rel[2]))
        return 0;
    return s->keys_held != 0 || v->now - s->release_frame <= C2_PLAYED_SHOW;
}

/* Its colours: the top note white, a sounding chord's root blue, the rest light blue; notes off the keyboard
 * counted for the edge arrows. */
static void c2u_played_cols(const struct c2_state *s, uint8_t *col, int *below, int *above)
{
    int top = -1;
    *below = *above = 0;
    for (int k = 0; k < C2_KEYS; k++)
        col[k] = 0;
    for (int r = 0; r < 96; r++)
        if (s->view_rel[r >> 5] >> (r & 31) & 1)
            top = r;
    for (int r = 0; r < 96; r++) {
        if (!(s->view_rel[r >> 5] >> (r & 31) & 1))
            continue;
        int k = r - C2_REL_OFS;
        if (k < 0)
            ++*below;
        else if (k >= C2_KEYS)
            ++*above;
        else
            col[k] = (uint8_t)(r == top ? WHITE : (s->view_roots >> c2_mod12(k) & 1) ? BLUE : LIGHTBLUE);
    }
}

/* The edited chord (plain layer): its name big; TOP (white knob) and VOICING (orange knob) at the right. */
static void c2u_editor(void *r, const struct c2_state *s, const struct c2_view *v, const struct c2_chord *c, int key)
{
    char name[24], rootname[4], b[40];
    int8_t iv[C2_NOTES];
    int flats = c2u_flats(s, v), mode = c2u_mode(s, v);
    int learning = (int)s->learning && s->learn_page;
    int rt = 0, n = c2u_chord_notes(s, v, c, iv, &rt), q = c ? c2_shown(c, &rt) : Q_NONE;
    c2u_s(rootname, (flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(key + rt)]);
    int scanning = !learning && s->turned == 1 && v->now - s->knob_frame <= C2_SCAN_SHOW && c;
    fp32 sx = FP(8), y0 = FP(21);   /* sx: where the TYPE scan strip goes (under the quality) */
    fp32 xr = FP(312);              /* the readouts' right edge; the name keeps left of them */
    if (!learning && c && q != Q_NONE && n) {
        fp32 ts = s->turned == 2 ? FP(2) : FP(3) >> 1, vs = s->turned == 3 ? FP(2) : FP(3) >> 1;
        char base[16];                 /* the centre: CLOSE, or a learned chord as played (with its span if wide) */
        int span = c->q == Q_CUSTOM ? c2_span(c, -1) : 0;
        if (c->q != Q_CUSTOM)
            c2u_s(base, "CLOSE");
        else if (span > 12 && c2_voi_n(c) > 2)          /* an interval names its own span */
            c2u_s(c2u_i(c2u_s(base, "PLAYED "), span > 24 ? 3 : 2), " OCT");
        else
            c2u_s(base, "AS PLAYED");
        const char *fam = c2_voi_label(c2_voi_n(c), c->voi, base);
        c2u_note(b, v->octave, flats, key + iv[n - 1]);
        fp32 wt = ctext_w(b, ts), wf = ctext_w(fam, vs), wl = ctext_w("TOP", FP(1));
        fp32 w = wt + wl + FP(4) > wf ? wt + wl + FP(4) : wf;
        w = w > FP(32) ? w : FP(32);   /* the strip's width */
        fp32 xt = xr - wt - wl - FP(4);
        ctext(r, "TOP", xt, FP(22) + 6 * ts - FP(6), FP(1), GREY, 1);
        int mm = c2_mode_map(mode);
        if (mm >= 0 && s->d.set[mm].smooth && s->smooth_n > 0 && s->smooth_map == mm && s->smooth_key == key &&
            s->smooth_top != c->top) {  /* SMOOTH played another TOP: it shows, grey, before the stored one */
            int8_t sv[C2_NOTES];
            int sb, sn = c2_notes_t(c, s->d.set[mm].bass, s->smooth_top, sv, &sb);
            char sm[12];
            c2u_note(c2u_s(sm, "SM "), v->octave, flats, key + sv[sn > 0 ? sn - 1 : 0]);
            fp32 ws = ctext_w(sm, FP(1));
            ctext(r, sm, xt - ws - FP(6), FP(22) + 6 * ts - FP(6), FP(1), GREY, 1);
            w += ws + FP(6);
        }
        ctext(r, b, xr - wt, FP(22), ts, WHITE, s->turned != 2);
        ctext(r, fam, xr - wf, FP(34), vs, c->voi ? ORANGE : GREY, s->turned != 3);
        c2u_voi_strip(r, c, xr, FP(50));
        xr -= w + FP(10);
    }
    fp32 big = FP(4), wmax = xr - FP(8);
    if (!c || q == Q_NONE) {
        if (learning)
            ctext(r, "...", FP(8), y0, FP(3), RED, 0);
        else if (!mode) {
            fp32 x = ctext(r, "SHIFT+", FP(8), FP(24), FP(1) + (FP(1) >> 1), WHITE, 1);
            x = ctext(r, "BLUE", x, FP(24), FP(1) + (FP(1) >> 1), BLUE, 1);
            ctext(r, ": MODE", x, FP(24), FP(1) + (FP(1) >> 1), WHITE, 1);
            ctext(r, "SHIFT+NOTES: LEARN A SHAPE", FP(8), FP(36), FP(1), GREY, 1);
            ctext(r, "SHIFT+SPLIT: CLOSE", FP(8), FP(44), FP(1), GREY, 1);
        } else {
            fp32 x = ctext(r, "NOTE", FP(8), y0, FP(3), GREY, 0) + FP(8);
            fp32 x1 = ctext(r, "GREEN", x, FP(26), FP(1) + (FP(1) >> 2), GREEN, 1);
            ctext(r, " > CHORDS", x1, FP(26), FP(1) + (FP(1) >> 2), GREY, 1);
            ctext(r, " < INTERVALS", x1, FP(36), FP(1) + (FP(1) >> 2), GREY, 1);
        }
    } else {
        c2_name(name, c, key, flats, 2);                 /* spelled out where it fits left of the readouts */
        while (q == Q_DYAD && ctext_w(name, big) > wmax && big > (FP(5) >> 1))
            big -= FP(1) >> 2;                           /* an interval: "C MAJOR 10TH" smaller before "CM10" */
        if (ctext_w(name, big) > wmax) {
            big = FP(4);
            c2_name(name, c, key, flats, 0);
        }
        while (ctext_w(name, big) > wmax && big > FP(2))
            big -= FP(1) >> 1;
        ctext(r, name, FP(8), y0, big, learning ? RED : WHITE, 0);
        if (!learning) {            /* which knob edits which part of the name: the turned one wider */
            fp32 wr = ctext_w(rootname, big), wn = ctext_w(name, big);
            fp32 yu = y0 + 7 * big + FP(2);
            if (yu > FP(50))
                yu = FP(50);
            line_fp(r, FP(8), yu, FP(8) + wr, yu, s->turned == 0 ? FP(3) : FP(2), BLUE);
            sx = FP(8) + wr + FP(6);
            if (!scanning && wn > wr + FP(6))
                line_fp(r, sx, yu, FP(8) + wn, yu, FP(2), GREEN);
            if (scanning)
                c2u_scan(r, c->q == Q_CUSTOM ? c2_custom_q(c) : c->q, sx, yu - FP(2));
        }
    }
    if (scanning && (!c || q == Q_NONE))
        c2u_scan(r, Q_NONE, sx, FP(46));
    /* the key, a blue padlock while pinned (open for the UNPINNED flash); learning: the red LEARN tag */
    fp32 xe = FP(8), ky = FP(56);
    if (learning) {
        c2u_tag(r, "LEARN", FP(8), FP(53), FP(1), RED, RED, 0, 0);
        xe = FP(8) + ctext_w("LEARN", FP(1)) + FP(9);
        ky = FP(55);
    }
    if (mode) {
        c2u_note(b, v->octave, flats, key);
        xe = ctext(r, b, xe, ky, FP(1) + (FP(1) >> 2), learning ? RED : GREY, 1);
    }
    const char *msg = v->now - s->msg_frame <= C2_FLASH ? c2u_msg(s, mode, b) : 0;
    int unpinned = msg && s->msg == MSG_UNPINNED;
    if ((s->pinned || unpinned) && !learning && mode) {
        padlock(r, xe + FP(1), FP(54), s->pinned ? BLUE : GREY, !s->pinned);
        xe += FP(8);
    }
    int red = learning || s->msg == MSG_MAX4 || s->msg == MSG_FACTORY_ASK;
    if (!msg && learning && s->learn_n > C2_NOTES) {
        msg = "MAX 5: BASS + TOP 4";
        red = 1;
    }
    if (msg) {
        fp32 st = FP(1) + (FP(1) >> 2), gap = FP(6);
        if (xe + gap + ctext_w(msg, st) > FP(220))
            st = FP(1);
        if (red && !learning)                            /* red outside a learn is always boxed */
            c2u_tag(r, msg, xe + gap, FP(53), FP(1), RED, RED, 0, s->msg == MSG_FACTORY_ASK ? 2 : 0);   /* long: fast lines */
        else
            ctext(r, msg, xe + gap, FP(56), st, red ? RED : WHITE, 1);
    }
    /* the chord sounding, when it is not the edited one */
    int pk = s->last_key;
    if (!learning && mode > 1 && pk >= 0 && pk < C2_KEYS && pk != key && (s->sounding >> pk & 1) && !msg) {
        const struct c2_chord *pc = c2u_chord(s, v, pk);
        if (pc && pc->q != Q_NONE) {
            c2_name(name, pc, pk, flats, 0);
            fp32 w = ctext_w(name, FP(1) + (FP(1) >> 1));
            ctext(r, name, FP(312) - w, FP(54), FP(1) + (FP(1) >> 1), LIGHTBLUE, 1);
        }
    }
}

/* WRITE (SHIFT+DROP): what goes into which map; the preview on the keys (they audition it). */
static const char *const c2u_from_name[W_COUNT] = { "FACTORY", "7THS", "TRIADS", "COPY", "EMPTY" };
static void c2_draw_write(void *r, const struct c2_state *s, const struct c2_view *v)
{
    char big[32], b[4][16], *p;
    int from = s->wr_from < W_COUNT && s->wr_from >= 0 ? s->wr_from : W_EMPTY, fl = s->wr_set.flats != 0;
    ctext(r, "WRITE", FP(8), FP(5), FP(2), WHITE, 0);
    const char *msg = v->now - s->msg_frame <= C2_FLASH && s->msg == MSG_OVERWRITE ? c2u_msg(s, 2, big) : 0;
    if (msg)
        c2u_tag(r, msg, FP(314), FP(3), FP(1), RED, RED, 1, 2);
    else
        ctext(r, "] WRITES  \\ CANCELS", FP(312) - ctext_w("] WRITES  \\ CANCELS", FP(1)), FP(8), FP(1), GREY, 1);
    /* the source, big */
    if (from == W_FACTORY) {
        int f = s->wr_fac >= 0 && s->wr_fac < 4 ? s->wr_fac : 0;
        static const char *const fac[4] = { "DIATONIC C", "II-V-I", "NEO SOUL", "POP" };
        c2u_s(big, fac[f]);
    } else if (from == W_SEVENTHS || from == W_TRIADS) {
        int sc = s->wr_scale >= 0 && s->wr_scale < S_COUNT ? s->wr_scale : 0;
        p = c2u_s(big, (fl ? c2_pc_flat : c2_pc_sharp)[c2_mod12(s->wr_key)]);
        p = c2u_s(c2u_s(p, " "), c2_scale_name[sc]);
        c2u_s(p, from == W_SEVENTHS ? " 7THS" : " TRIADS");
    } else if (from == W_COPY) {
        int t = s->wr_shift;
        p = c2u_i(c2u_s(big, "MAP "), s->wr_src + 1);
        if (t)
            c2u_i(c2u_s(p, t > 0 ? " +" : " -"), t > 0 ? t : -t);
    } else
        c2u_s(big, "EMPTY");
    fp32 st = FP(3);
    char to[4];
    c2u_i(to, s->wr_to + 1);
    fp32 wmax = FP(304) - ctext_w(to, st) - FP(16);
    while (ctext_w(big, st) > wmax && st > FP(3) >> 1)
        st -= FP(1) >> 2;
    ctext(r, big, FP(8), FP(23), st, WHITE, 0);
    ctext(r, to, FP(312) - ctext_w(to, st), FP(23), st, ORANGE, 0);   /* the target, on the name's baseline */
    /* the four knobs */
    c2u_s(b[0], c2u_from_name[from]);
    const char *lab[4] = { "FROM", "", "", "TO" };
    if (from == W_FACTORY) {
        lab[1] = "WHICH";
        c2u_i(b[1], s->wr_fac + 1);
        lab[2] = "";
        b[2][0] = 0;
    } else if (from == W_COPY) {
        lab[1] = "TRANSPOSE";
        p = b[1];
        if (s->wr_shift > 0)
            *p++ = '+';
        c2u_i(p, s->wr_shift);
        lab[2] = "OF";
        c2u_i(c2u_s(b[2], "MAP "), s->wr_src + 1);
    } else if (from == W_EMPTY) {
        b[1][0] = b[2][0] = 0;
    } else {
        lab[1] = "KEY";
        c2u_s(b[1], (fl ? c2_pc_flat : c2_pc_sharp)[c2_mod12(s->wr_key)]);
        lab[2] = "SCALE";
        c2u_s(b[2], c2_scale_short[s->wr_scale >= 0 && s->wr_scale < S_COUNT ? s->wr_scale : 0]);
    }
    c2u_i(c2u_s(b[3], "MAP "), s->wr_to + 1);
    int used = 0;
    for (int k = 0; k < C2_KEYS && s->wr_to >= 0 && s->wr_to < C2_MAPS; k++)
        used |= s->d.maps[s->wr_to][k].q != Q_NONE;
    for (int i = 0; i < 4; i++) {
        fp32 x = FP(8 + 78 * i);
        ctext(r, lab[i], x, FP(47), FP(1), GREY, 1);
        x = ctext(r, b[i], x, FP(55), FP(1) + (FP(1) >> 2), c2u_knob_col[i], 1);
        if (i == 3 && used)            /* a warning is never coral: grey, the red question comes on DROP */
            ctext(r, "USED", x + FP(4), FP(55), FP(1) + (FP(1) >> 2), GREY, 1);
    }
    int tk = -1;                       /* a scale fill: its tonic outlined (D dorian looks like C major otherwise) */
    for (int k = 0; k < C2_KEYS && (from == W_SEVENTHS || from == W_TRIADS); k++)
        if (tk < 0 && c2_key_pc(k) == c2_mod12(s->wr_key))
            tk = k;
    if (c2u_played_view(s, v)) {
        uint8_t col[C2_KEYS];
        int below, above;
        c2u_played_cols(s, col, &below, &above);
        keyboard(r, s->wr_map, fl, tk, 0, WHITE, col);
        c2u_offkb(r, -1, below, col[0] ? 1 : WHITE);
        c2u_offkb(r, 1, above, col[C2_KEYS - 1] ? 1 : WHITE);
    } else
        keyboard(r, s->wr_map, fl, tk, s->sounding, WHITE, 0);
}

/* The CHORD page, over whatever TE drew: an opaque black backing first. */
static void c2_draw_page(void *r, const struct c2_state *s, const struct c2_view *v)
{
    c2u_fill(r, 0, 0, FP(320), FP(160), 1 /* black */);
    if (s->wr_open) {
        c2_draw_write(r, s, v);
        return;
    }
    c2u_header(r, s, v);
    int mode = c2u_mode(s, v), key = s->sel >= 0 && s->sel < C2_KEYS ? s->sel : 7;
    int flats = c2u_flats(s, v);
    struct c2_chord t;
    const struct c2_chord *c = c2u_edited(s, v, key, &t);
    int factory = (s->msg == MSG_FACTORY_DONE || s->msg == MSG_FACTORY_NOMAP) && v->now - s->msg_frame <= C2_FLASH;
    if (v->shift && !(s->learning && s->learn_page) && !factory)
        c2u_shift_layer(r, s, v);
    else
        c2u_editor(r, s, v, c, key);
    int m = c2_mode_map(mode), selcol = s->pinned ? BLUE : WHITE;
    uint8_t col[C2_KEYS];
    int below, above;
    if (!c2u_notes_view(s, v) && c2u_played_view(s, v)) {   /* what sounds, lit (any mode), the map dim */
        c2u_played_cols(s, col, &below, &above);
        keyboard(r, m >= 0 ? s->d.maps[m] : 0, flats, mode ? key : -1, 0, selcol, col);
        c2u_offkb(r, -1, below, col[0] ? 1 /* black on a lit key */ : WHITE);
        c2u_offkb(r, 1, above, col[C2_KEYS - 1] ? 1 : WHITE);
        return;
    }
    if (!mode) {
        keyboard(r, 0, 0, -1, 0, WHITE, 0);
        return;
    }
    if (!c2u_notes_view(s, v) && mode != 1) {   /* the map view: every key's chord, the edited key outlined */
        keyboard(r, m >= 0 ? s->d.maps[m] : 0, flats, key, s->sounding, selcol, 0);
        return;
    }
    /* the notes view (SHAPE always): the edited chord's notes lit and named on the keys, off-keyboard notes as edge
     * arrows */
    int8_t iv[C2_NOTES];
    int rt, n = c2u_chord_notes(s, v, c, iv, &rt);
    c2u_notecols(col, iv, n, key, rt, s->learning && s->learn_page, &below, &above);
    keyboard(r, 0, flats, key, 0, selcol, col);
    int lc = s->learning && s->learn_page ? RED : WHITE;
    c2u_offkb(r, -1, below, col[0] ? 1 /* black on a lit key */ : lc);
    c2u_offkb(r, 1, above, col[C2_KEYS - 1] ? 1 : lc);
}

/* The name of the slot's last chord, if it sounds or sounded < ~1 s ago (*fresh = still sounding). */
static int c2u_last(const struct c2_state *s, const struct c2_view *v, char *b, int *fresh)
{
    int k = s->last_key;
    *fresh = 0;
    b[0] = 0;
    if (k < 0 || k >= C2_KEYS || s->last_slot != v->slot)
        return 0;
    const struct c2_chord *c = c2u_chord(s, v, k);
    if (!c || c->q == Q_NONE)
        return 0;
    *fresh = (s->sounding >> k & 1) != 0;
    if (!*fresh && v->now - s->last_frame > 36)
        return 0;
    c2_name(b, c, k, c2u_flats(s, v), 0);
    return b[0] != 0;
}

/* Badge on our engine screens (replaces CHORD1's box, under HOLD's): "1 G7", blue frame MAP, white SHAPE, red LEARN. */
static void c2_draw_badge(void *r, const struct c2_state *s, const struct c2_view *v)
{
    char b[24], nm[20], *p = b;
    int mode = c2u_mode(s, v), m = c2_mode_map(mode), fresh;
    int learning = (int)s->learning;
    if (!mode && !learning)
        return;
    if (m >= 0)
        p = c2u_s(c2u_i(p, m + 1), " ");
    if (learning)
        c2u_s(p, "LEARN");
    else if (c2u_last(s, v, nm, &fresh))
        c2u_s(p, nm);
    else
        c2u_s(p, m >= 0 ? "MAP" : "SHAPE");
    c2u_tag(r, b, FP(130), FP(24), FP(3) >> 1, learning ? RED : m >= 0 ? BLUE : WHITE, learning ? RED : WHITE, 0, 0);
}

/* Chip on TE's screens (frame hook): the sounding chord top right on a black backing, grey ~1 s after; silent: the
 * slot's mode, dim, so a slot left in MAP is visible. */
static void c2_draw_chip(void *r, const struct c2_state *s, const struct c2_view *v)
{
    char b[24], nm[20];
    int mode = c2u_mode(s, v), fresh;
    if (!mode && !s->learning)
        return;
    if (s->learning) {
        c2u_tag(r, "LEARN", FP(314), FP(4), FP(1) + (FP(1) >> 2), RED, RED, 1, 1);
        return;
    }
    if (c2u_last(s, v, nm, &fresh)) {
        c2u_tag(r, nm, FP(314), FP(3), FP(2), v->hold ? RED : c2_mode_map(mode) >= 0 ? BLUE : WHITE, fresh ? WHITE : GREY, 1, 1);
        return;
    }
    c2u_tag(r, c2u_mode_name(mode, b), FP(314), FP(4), FP(1) + (FP(1) >> 2), v->hold ? RED : GREY, GREY, 1, 3);   /* 0 AA */
}

/* The frame hook's bookkeeping (ui.c c2_frame): count the frame, and close the page once the synth screens are left
 * (drum / game play mode, or another controller: only trusted once the controller check has succeeded at least once,
 * so a wrong guess about TE's controller pointer cannot keep the page from opening). */
static void c2_frame_state(struct c2_state *s, int synth, int play_mode)
{
    s->dg.frames++;
    s->clock++;
    if (synth)
        s->dg.ctl_synth++;
    if (s->page && (play_mode != 0 || (!synth && s->dg.ctl_synth))) {
        s->page = 0;
        s->wr_open = 0;                /* WRITE closes with the page */
    }
}
