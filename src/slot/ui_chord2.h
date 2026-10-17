/* CHORD v2 screens (notes/chord2.md "Screens"): the CHORD page, the badge on our engine screens and the chip on TE's.
 * Included by ui.c (device) and test/chord2_screens.c (host gallery, same code). Uses ui.c's primitives: line_fp
 * (AGG, ~20-35 k cycles), line_fast / vline_fast (1-px framebuffer lines, ~free), Q22.10 SVG units, 320 x 160,
 * safe area x 4..316, y 2..140. Immediate mode (no SVG), so it draws over any TE screen. No division, no floats.
 *
 * Page layout (MAP):
 *   y   4..18  header: MAP n (blue) and the map's name; right: HOLD (boxed red), B (orange, BASS)
 *   y  24..47  the edited chord's name, big, white (spelled out when it fits); blue underline = root (blue knob),
 *              green = quality (green knob); while green turns, the TYPE scan strip under it
 *   y  24..43  right: its notes on a 3-octave ruler (root blue, top white, others light blue); under it TOP (white)
 *              and VOICING (orange)
 *   y  54..61  the edited key (+ padlock while frozen), a message; right: the chord sounding, if another one
 *   y  66..138 the 24 keys (F..E), mapped keys labelled root over quality; edited key outlined white, sounding dark blue
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
    int rt, q = c2_shown(c, &rt);
    if (q == Q_NONE)
        return;
    const char *tag = q == Q_CUSTOM ? "?" : c2_q[q].tag;
    fp32 ts = step;                 /* a long tag (7S4 on a black key) gets smaller, never wider than its key */
    while (ctext_w(tag, ts) > maxw && ts > (FP(3) >> 2))
        ts -= FP(1) >> 3;
    ctext_c(r, (flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(key + rt)], cx, y, step + (step >> 2), colour, 1);
    ctext_c(r, tag, cx, y + 9 * step, ts, colour, 1);
}

/* keys: the chords to label (0 = none); sel: outlined key; lit: dark-blue keys */
static void keyboard(void *r, const struct c2_chord *keys, int flats, int sel, uint32_t lit)
{
    for (int k = 0; k < C2_KEYS; k++) {
        if (!(lit >> k & 1))
            continue;
        if (kb_is_black(k))
            c2u_fill(r, kb_x(k) + FP(1), FP(KB_Y0) + FP(1), kb_x(k) + FP(KB_BK - 1), FP(KB_YB) - FP(1), DARKBLUE);
        else {
            fp32 px[8], py[8];
            kb_white_shape(k, px, py);
            c2u_fill(r, px[0] + FP(1), FP(KB_Y0) + FP(1), px[1] - FP(1), FP(KB_YB), DARKBLUE);
            c2u_fill(r, px[5] + FP(1), FP(KB_YB), px[4] - FP(1), FP(KB_Y1) - FP(1), DARKBLUE);
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
    if (keys)
        for (int k = 0; k < C2_KEYS; k++) {
            int black = kb_is_black(k);
            int col = k == sel || (lit >> k & 1) ? WHITE : GREY;
            fp32 cx = kb_x(k) + FP(black ? KB_BK >> 1 : KB_WK >> 1);
            label2(r, &keys[k], k, flats, cx, black ? FP(KB_Y0 + 4) : FP(KB_YB + 5), col, FP(1) + (FP(1) >> 2),
                   black ? FP(KB_BK - 3) : FP(KB_WK - 3));
        }
    if (sel >= 0 && sel < C2_KEYS) {   /* the edited key: its visible outline, 2 px, white */
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

/* ---- a chord's notes on a 3-octave ruler: a tick per semitone (C tall, black keys short and dim); marks under the
 * ticks: root blue, top note white, others light blue (learning: all red) ---- */
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

static void padlock(void *r, fp32 x, fp32 y, int colour)   /* 5 x 7 */
{
    frame_fast(r, x, y + FP(3), x + FP(5), y + FP(7), colour);
    line_fast(r, x + FP(1), y + FP(3), x + FP(1), y + FP(1), colour);
    line_fast(r, x + FP(1), y + FP(1), x + FP(4), y + FP(1), colour);
    line_fast(r, x + FP(4), y + FP(1), x + FP(4), y + FP(3), colour);
}

/* A framed tag: the badge on our screens, the chip on TE's, LEARN on the page. x is the left edge, or the right edge
 * when right != 0. */
static void c2u_tag(void *r, const char *s, fp32 x, fp32 y, fp32 step, int frame, int colour, int right, int backing)
{
    fp32 w = ctext_w(s, step), h = 6 * step, pad = step + FP(2), bw = w + 2 * pad, bh = h + 2 * step + FP(2);
    if (right)
        x -= bw;
    if (backing)
        c2u_fill(r, x, y, x + bw, y + bh, 1 /* black */);
    frame_aa(r, x, y, x + bw, y + bh, FP(1), frame);
    ctext(r, s, x + pad, y + step + FP(1), step, colour, 0);
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
    fp32 x = ctext(r, c2u_mode_name(mode, b), FP(8), FP(5), FP(2), m >= 0 ? BLUE : mode ? WHITE : GREY, 0);
    if (m >= 0 && s->d.set[m].name[0]) {
        char nm[13];
        for (int i = 0; i < 12; i++)
            nm[i] = s->d.set[m].name[i];
        nm[12] = 0;
        ctext(r, nm, x + FP(4), FP(9), FP(1), GREY, 1);
    }
    fp32 xr = FP(312);
    if (v->hold) {
        fp32 w = ctext_w("HOLD", FP(1) + (FP(1) >> 2));
        xr -= w + FP(6);
        frame_aa(r, xr, FP(4), xr + w + FP(5), FP(16), FP(1), RED);
        ctext(r, "HOLD", xr + FP(3), FP(6) + (FP(1) >> 1), FP(1) + (FP(1) >> 2), RED, 0);
        xr -= FP(8);
    }
    if (m >= 0 && s->d.set[m].bass) {
        xr -= ctext_w("B", FP(1) + (FP(1) >> 2));
        ctext(r, "B", xr, FP(6), FP(1) + (FP(1) >> 2), ORANGE, 1);
        line_fast(r, xr, FP(15), xr + FP(5), FP(15), ORANGE);
    }
}

static const char *c2u_msg(const struct c2_state *s, int mode, char *b)
{
    switch (s->msg) {
    case MSG_CLEARED: return "CLEARED  SHIFT+[ UNDOES";
    case MSG_COPIED: return "COPIED  ] PASTES";
    case MSG_PASTED: return "PASTED  SHIFT+[ UNDOES";
    case MSG_UNDONE: return "UNDONE  AGAIN = REDO";
    case MSG_LEARNED: return "LEARNED  SHIFT+[ UNDOES";
    case MSG_MAX4: return "MAX 4: LOWEST KEPT";
    case MSG_EMPTY_CLIP: return "[ COPIES FIRST";
    case MSG_LATER: return "NOT IN THIS VERSION";
    case MSG_TURN_GREEN: return mode ? "TURN GREEN FOR A CHORD" : "SHIFT+BLUE: MODE";
    case MSG_NOTHING_UNDO: return "NOTHING TO UNDO";
    case MSG_MODE: return c2u_mode_name(s->msg_arg, b);
    case MSG_BASS: return s->msg_arg ? "BASS ON" : "BASS OFF";
    default: return 0;
    }
}

/* SHIFT layer: the map's settings in knob colours, and the diagnostics (the device test reads them). */
static const char *const c2u_load_name[5] = { "-", "NONE", "A", "B", "BAD" };
static const char *const c2u_save_name[6] = { "-", "OK", "OPEN", "WRITE", "CLOSE", "TORN" };
static void c2u_shift_layer(void *r, const struct c2_state *s, const struct c2_view *v)
{
    char b[48], *p;
    int mode = c2u_mode(s, v), m = c2_mode_map(mode);
    const char *lab[4] = { "MODE", "STRUM", "SMOOTH", "BASS" };
    const char *val[4] = { c2u_mode_name(mode, b), "-", "-", m < 0 ? "-" : s->d.set[m].bass ? "ON" : "OFF" };
    for (int i = 0; i < 4; i++) {
        fp32 x = FP(8 + 78 * i);
        ctext(r, lab[i], x, FP(24), FP(1), GREY, 1);
        ctext(r, val[i], x, FP(33), FP(1) + (FP(1) >> 1), i == 1 || i == 2 ? GREY : c2u_knob_col[i], s->turned != i + 4);
    }
    const struct c2_diag *g = &s->dg;
    p = c2u_s(b, "HOOK F");
    p = c2u_i(p, (int32_t)(g->frames & 0xFFFF));
    p = c2u_i(c2u_s(p, " K"), (int32_t)g->knobs);
    p = c2u_i(c2u_s(p, " D"), g->knob_delta);
    p = c2u_i(c2u_s(p, " SP"), (int32_t)g->split);
    p = c2u_i(c2u_s(p, " LI"), (int32_t)g->lift);
    p = c2u_i(c2u_s(p, " DR"), (int32_t)g->drop);
    p = c2u_i(c2u_s(p, " UP"), (int32_t)g->shift_up);
    c2u_s(p, g->ctl_synth ? " CT" : " CT0");
    ctext(r, b, FP(8), FP(45), FP(1), WHITE, 1);
    p = c2u_s(b, "FILE ");
    p = c2u_s(p, c2u_load_name[g->load >= 0 && g->load < 5 ? g->load : 0]);
    p = c2u_i(c2u_s(p, " "), g->load_seq);
    p = c2u_i(c2u_s(p, " SAVE "), (int32_t)g->saves);
    p = c2u_s(c2u_s(p, " "), c2u_save_name[g->save_err >= 0 && g->save_err < 6 ? g->save_err : 0]);
    p = c2u_i(c2u_s(p, " MK"), g->mkdir_ret);
    p = c2u_i(c2u_s(p, " VOX "), g->voices);
    p = c2u_i(c2u_s(p, " SHED "), (int32_t)g->sheds);
    if (g->fb_frames)
        c2u_s(p, " FB");
    ctext(r, b, FP(8), FP(54), FP(1), WHITE, 1);
}

/* The TYPE scan strip: every quality as a tick, the current one tall green (Model:Cycles-like position). */
static void c2u_scan(void *r, int q, fp32 x0, fp32 y)
{
    for (int i = 0; i < Q_COUNT; i++) {
        fp32 x = x0 + i * FP(5);
        if (i == q)
            line_fp(r, x, y - FP(1), x, y + FP(4), FP(2), GREEN);
        else
            vline_fast(r, x, y + FP(1), y + FP(3), i == Q_NONE ? GREY : (i == Q_MAJ || i == Q_SUS2 || i == Q_7 || i == Q_7B9 ||
                                                                          i == Q_MIN || i == Q_HDIM) ? LIGHTBLUE : DARKBLUE);
    }
}

/* The edited chord (plain layer). */
static void c2u_editor(void *r, const struct c2_state *s, const struct c2_view *v, const struct c2_chord *c, int key)
{
    char name[24], rootname[4], b[40];
    int8_t iv[C2_NOTES];
    int flats = c2u_flats(s, v), mode = c2u_mode(s, v);
    int learning = (int)s->learning && s->learn_page;
    int n = 0, bi, rt = 0, q;
    struct c2_chord t;
    if (learning) {                 /* live: the notes played so far, named, in red */
        c2_clear(&t);
        int cnt = 0;
        for (int i = 0; i < s->learn_n && cnt < C2_MAX; i++) {
            int32_t d = s->learn_note[i] - s->learn_base;
            t.iv[cnt++] = (int8_t)(d > C2_IV_MAX ? C2_IV_MAX : d < -C2_IV_MAX ? -C2_IV_MAX : d);
        }
        c2_sort(t.iv, cnt);
        t.q = cnt ? Q_CUSTOM : Q_NONE;
        t.n = (uint8_t)cnt;
        c = &t;
    }
    if (c)
        n = c2_notes(c, c2_mode_map(mode) >= 0 && s->d.set[c2_mode_map(mode)].bass, iv, &bi);
    q = c ? c2_shown(c, &rt) : Q_NONE;
    c2u_s(rootname, (flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(key + rt)]);
    fp32 big = FP(3);
    if (!c || q == Q_NONE) {
        if (learning)
            ctext(r, "...", FP(8), FP(24), big, RED, 0);
        else if (!mode) {
            ctext(r, "SHIFT+BLUE: MODE", FP(8), FP(26), FP(1) + (FP(1) >> 1), WHITE, 0);
            ctext(r, "SHIFT+NOTES: LEARN A SHAPE", FP(8), FP(39), FP(1), GREY, 1);
        } else
            ctext(r, "NOTE", FP(8), FP(24), big, GREY, 0);
    } else {
        c2_name(name, c, key, flats, 2);                 /* spelled out where it fits left of the ruler */
        if (ctext_w(name, big) > FP(152))
            c2_name(name, c, key, flats, 0);
        if (ctext_w(name, big) > FP(152))
            big = FP(2);
        ctext(r, name, FP(8), FP(24), big, learning ? RED : WHITE, 0);
        if (!learning) {            /* which knob edits which part of the name */
            fp32 wr = ctext_w(rootname, big), wn = ctext_w(name, big);
            fp32 yu = FP(24) + 7 * big + FP(2);
            line_fp(r, FP(8), yu, FP(8) + wr, yu, FP(2), s->turned == 0 ? WHITE : BLUE);
            if (wn > wr + FP(6))
                line_fp(r, FP(8) + wr + FP(6), yu, FP(8) + wn, yu, FP(2), s->turned == 1 ? WHITE : GREEN);
        }
    }
    /* the TYPE scan while green turns */
    if (!learning && s->turned == 1 && v->now - s->knob_frame <= C2_FREEZE && c)
        c2u_scan(r, c->q == Q_CUSTOM ? c2_custom_q(c) : c->q, FP(8), FP(50));
    /* the key, frozen or not; learning: the red LEARN tag */
    fp32 xe = FP(8);
    if (learning) {
        c2u_tag(r, "LEARN", FP(8), FP(52), FP(1) + (FP(1) >> 2), RED, RED, 0, 0);
        xe = FP(8) + ctext_w("LEARN", FP(1) + (FP(1) >> 2)) + FP(10);
    }
    if (mode) {
        c2u_note(b, v->octave, flats, key);
        xe = ctext(r, b, xe, FP(56), FP(1) + (FP(1) >> 2), learning ? RED : GREY, 1);
    }
    int frozen = s->knob_frame && v->now - s->knob_frame <= C2_FREEZE;
    if (frozen && !learning && mode) {
        padlock(r, xe + FP(1), FP(54), WHITE);
        xe += FP(8);
    }
    const char *msg = v->now - s->msg_frame <= C2_FLASH ? c2u_msg(s, mode, b) : 0;
    if (!msg && learning && s->learn_n > C2_MAX)
        msg = "MAX 4: LOWEST KEPT";
    if (!msg && !learning && c && c->q == Q_CUSTOM)
        msg = "AS PLAYED";
    if (msg)
        ctext(r, msg, xe + FP(10), FP(56), FP(1) + (FP(1) >> 2), learning || s->msg == MSG_MAX4 ? RED :
              s->msg && v->now - s->msg_frame <= C2_FLASH ? WHITE : GREY, 1);
    if (n)
        ruler(r, iv, n, key + 12 * v->octave, rt, learning);
    if (!learning && c && q != Q_NONE && n) {   /* TOP (white knob) and VOICING (orange knob), under the ruler */
        c2u_note(b, v->octave, flats, key + iv[n - 1]);
        fp32 x = ctext(r, b, FP(RU_X0), FP(46), FP(1) + (FP(1) >> 2), WHITE, s->turned != 2);
        if (c->q < Q_COUNT)
            ctext(r, c2_fam_name[c->fam < F_COUNT ? c->fam : 0], x + FP(8), FP(46), FP(1) + (FP(1) >> 2), ORANGE,
                  s->turned != 3);
    }
    /* the chord sounding, when it is not the edited one */
    int pk = s->last_key;
    if (!learning && mode > 1 && pk >= 0 && pk < C2_KEYS && pk != key && (s->sounding >> pk & 1) && !msg) {
        const struct c2_chord *pc = c2u_chord(s, v, pk);
        if (pc && pc->q != Q_NONE) {
            c2_name(name, pc, pk, flats, 0);
            fp32 w = ctext_w(name, FP(1) + (FP(1) >> 1));
            ctext(r, name, FP(312) - w, FP(54), FP(1) + (FP(1) >> 1), LIGHTBLUE, 0);
        }
    }
}

/* The CHORD page, over whatever TE drew: an opaque black backing first. */
static void c2_draw_page(void *r, const struct c2_state *s, const struct c2_view *v)
{
    c2u_fill(r, 0, 0, FP(320), FP(160), 1 /* black */);
    c2u_header(r, s, v);
    int mode = c2u_mode(s, v), key = s->sel >= 0 && s->sel < C2_KEYS ? s->sel : 7;
    int flats = c2u_flats(s, v);
    if (v->shift && !(s->learning && s->learn_page))
        c2u_shift_layer(r, s, v);
    else
        c2u_editor(r, s, v, c2u_chord(s, v, key), key);
    int m = c2_mode_map(mode);
    if (m >= 0)
        keyboard(r, s->d.maps[m], flats, key, s->sounding);
    else if (mode == 1) {           /* SHAPE: the keyboard lights where its notes fall from the edited key */
        int8_t iv[C2_NOTES];
        int bi, n = c2_notes(&s->d.shape[v->slot], 0, iv, &bi);
        uint32_t lit = 0;
        for (int i = 0; i < n; i++)
            if (key + iv[i] >= 0 && key + iv[i] < 24)
                lit |= 1u << (key + iv[i]);
        keyboard(r, 0, flats, key, lit);
    } else
        keyboard(r, 0, 0, -1, 0);
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
        c2u_tag(r, nm, FP(314), FP(3), FP(2), c2_mode_map(mode) >= 0 ? BLUE : WHITE, fresh ? WHITE : GREY, 1, 1);
        return;
    }
    c2u_tag(r, c2u_mode_name(mode, b), FP(314), FP(4), FP(1) + (FP(1) >> 2), GREY, GREY, 1, 1);
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
    if (s->page && (play_mode != 0 || (!synth && s->dg.ctl_synth)))
        s->page = 0;
}
