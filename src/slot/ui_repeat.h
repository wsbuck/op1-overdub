/* repeat FX screen: the Wheel of Fortune (design/fx/wheel.svg, published as content/display/wheel.svg; its header
 * lists every id; design/fx/gen_wheel.py runtime_state() is the reference, followed here). Included by ui.c after its
 * SVG helpers. Inputs: the knobs the FX used (fx_live: TE's LFO moves them), the DSP's state (repeat_state: the grid
 * played, the repeat, the depth, the wheel's turn, the bar clock, won and lost rolls) and the frame counter (30 fps).
 * Knob turns (stored knobs, >= 512 raw) show the value ~1.5 s at the title position in the knob's colour; the angel
 * steps aside meanwhile. While SHIFT is held: the bar-clock probe page instead (TE's transport, playhead, tempo,
 * metronome and sequencer words, and repeat's own clock; notes/fx-beatrepeat.md "Probe"). Without wheel.svg (or an
 * older one): a line screen. */
#include "repeat.h"

extern volatile int32_t te_tape_state __attribute__((weak));     /* 0xFF802960 (build_slot.py TE_UI) */
extern volatile uint32_t te_tape_pos __attribute__((weak));      /* 0xFF802974 samples x 64 */
extern volatile int32_t te_metro_count __attribute__((weak));    /* 0x01AADADC Q14 samples to the next click */
extern volatile int32_t te_ppqn_tick __attribute__((weak));      /* 0x018C1884 the sequencer's tick */
extern volatile int32_t te_tempo_spb __attribute__((weak));      /* 0x01AADAD8 samples per beat, Q14 */
#ifndef TE_TAPE_SPEED                                            /* overridable for the host preview */
#define TE_TAPE_SPEED (*(volatile int32_t *)0xFF802968)          /* tape speed (varispeed; 0x08000000 = 1.0?) */
#define WH_SHIFT C2_SHIFT                                        /* SHIFT held (0x01DD7F78) */
#endif

#define LPG 17                                      /* TE's lpg (palette order, notes/screen.md) */
#define WH_X 160                                    /* the wheel's centre (wheel.svg) */
#define WH_Y 82
#define WH_SWORD_X 168                              /* the sword's pivot and its resting angle */
#define WH_SWORD_Y 30
#define WH_SWORD_REST 109
#define WH_SNAKE_TRAVEL 75                          /* degrees the wheel carries the snake at the full drop */
#define WH_ANUB_R 75                                /* Anubis's feet on r 75 from 125 deg to 70 deg */
#define WH_ANUB_A0 125
#define WH_ANUB_TRAVEL 55
#define WH_RAISE 6                                  /* frames the sword takes to rise (0.2 s) */
#define WH_BLINK 8                                  /* frames the lots blink out on a lost roll (0.27 s) */
#define WH_SHOW 45                                  /* frames a readout stays (~1.5 s) */

enum { W_WHEEL, W_BLUR1, W_BLUR2, W_LAP1, W_LAP2, W_PLAYHEAD, W_SWUP, W_SWREST, W_HUBMAN, W_SNAKEORG, W_SNAKEGREY,
       W_ANUBORG, W_ANUBGREY, W_SNAKE, W_ANUBIS, W_ANGEL, W_SINKEND, W_RISEEND, W_SINGLE };
static const char *const wh_single[W_SINGLE] = { "wheel", "blur1", "blur2", "lap1", "lap2", "playhead", "swordup",
    "swordrest", "hubman", "snakeorg", "snakegrey", "anuborg", "anubgrey", "snake", "anubis", "angel", "sinkend",
    "riseend" };
static const uint8_t wh_spokes[9] = { 2, 3, 4, 6, 8, 12, 16, 24, 32 };
/* FORTUNE's lots per position (gen_wheel.py: ALWAYS 12, OFF 0, EVERY n 12 / n, CHANCE / VARY round(12 p)) */
static const uint8_t wh_lots[33] = { 12, 0, 0, 2, 2, 3, 3, 6, 6, 1, 2, 4, 5, 6, 7, 8, 10, 11, 12, 12, 12,
                                     6, 6, 6, 8, 8, 8, 10, 10, 10, 12, 12, 12 };

static struct svgdoc wheel_doc RAM = { 0, 0 };
static void *wh_node[W_SINGLE] RAM = { 0 };
static void *wh_spk[9] RAM = { 0 };
static void *wh_gate[16] RAM = { 0 }, *wh_gatex[16] RAM = { 0 }, *wh_ch[12] RAM = { 0 };
static void *wh_book[4][3] RAM = { { 0 } };          /* book1..4: closed, open, page */
static void *wh_sink[6] RAM = { 0 }, *wh_rise[6] RAM = { 0 };
struct wh_view {
    uint32_t frame;
    uint32_t fires, lost;                           /* the DSP's counts at the last frame */
    uint32_t fire_f, lost_f, rep_f;                 /* frames of the last won roll, lost roll, new repeat */
    int32_t rep;
    int32_t ctx, shown;                             /* the readout: screen context, knob shown, until frame */
    uint32_t until;
    int16_t ref[4];
};
static struct wh_view wh RAM = { 0, 0, 0, 0, 0, 0, -1, -1, -1, 0, { 0, 0, 0, 0 } };

/* "pre" + n (1 .. 99) into buf, no libc */
static const char *wh_id(char *buf, const char *pre, int n)
{
    char *p = buf;
    while (*pre)
        *p++ = *pre++;
    if (n >= 10)
        *p++ = (char)('0' + (n >= 20 ? n >= 30 ? 3 : 2 : 1)), n -= n >= 30 ? 30 : n >= 20 ? 20 : 10;
    *p++ = (char)('0' + n);
    *p = 0;
    return buf;
}

static int wheel_ready(void)
{
    if (wheel_doc.state)
        return wheel_doc.state > 0;
    if (!svg_open(&wheel_doc, "/yaffs2/content/display/wheel.svg", "wheel.svg"))
        return 0;
    if (!te_node_find(wheel_doc.doc, "wheel1", 0)) {                /* not this generation's file: the line screen */
        wheel_doc.state = -1;
        return 0;
    }
    char id[16];
    for (int i = 0; i < W_SINGLE; i++)
        wh_node[i] = te_node_find(wheel_doc.doc, wh_single[i], 0);
    for (int i = 0; i < 9; i++)
        wh_spk[i] = te_node_find(wheel_doc.doc, wh_id(id, "spk", wh_spokes[i]), 0);
    for (int i = 0; i < 16; i++) {
        wh_gate[i] = te_node_find(wheel_doc.doc, wh_id(id, "gate", i + 1), 0);
        wh_gatex[i] = te_node_find(wheel_doc.doc, wh_id(id, "gatex", i + 1), 0);
    }
    for (int i = 0; i < 12; i++)
        wh_ch[i] = te_node_find(wheel_doc.doc, wh_id(id, "ch", i + 1), 0);
    for (int b = 0; b < 4; b++)
        for (int j = 0; j < 3; j++) {
            wh_id(id, "book", b + 1);
            id[5] = "cof"[j];
            id[6] = 0;
            wh_book[b][j] = te_node_find(wheel_doc.doc, id, 0);
        }
    for (int i = 0; i < 6; i++) {
        wh_sink[i] = te_node_find(wheel_doc.doc, wh_id(id, "sinkdot", i + 1), 0);
        wh_rise[i] = te_node_find(wheel_doc.doc, wh_id(id, "risedot", i + 1), 0);
    }
    return 1;
}

static const char *const wh_grid_names[11] = { "1/64", "1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4",
                                               "1/2T", "1/2" };
static const char *wh_grid_name(int g)              /* repeat.h grid byte -> its name */
{
    int sh = g & 7;
    int i = g & REP_T ? 9 - 2 * (sh > 4 ? 4 : sh) : 12 - 2 * (sh < 1 ? 1 : sh > 6 ? 6 : sh);
    return wh_grid_names[i];
}

/* The value of knob `knob` (0..3) for the readout, as heard: k = the knobs drawn, st = the DSP (0 when not running),
 * master / drum: no manual key yet (OFF NO KEY). */
static void wh_readout(char *p, int knob, const int16_t *k, const struct rep *st, int nokey)
{
    int pos = rep_pos(knob_pos(k[knob]));
    if (knob == 0) {
        int g = st ? st->blue : rep_grid_at[pos];
        put_str(put_str(p, "GRID "), wh_grid_name(g));
    } else if (knob == 1) {
        int m = rep_fmode_at[pos], a = rep_fa_at[pos];
        if (m == RF_ALWAYS)
            put_str(p, "ALWAYS");
        else if (m == RF_OFF)
            put_str(p, nokey ? "OFF NO KEY" : "OFF");
        else if (m == RF_EVERY)
            put_str(a == 1 ? p : put_dec(put_str(p, "EVERY "), (uint32_t)a), a == 1 ? "EVERY BAR" : " BARS");
        else {
            uint32_t pc = ((uint32_t)rep_chance_at[pos] * 100 + 32768) >> 16;
            if (m == RF_CHANCE)
                put_str(put_dec(put_str(p, "CHANCE "), pc), "%");
            else
                put_dec(put_str(put_dec(put_str(p, "VARY "), pc), "% X"), (uint32_t)a);     /* VARY 67% X2 */
        }
    } else if (knob == 2) {
        int g = rep_gate_at[pos];
        p = put_str(p, "GATE ");
        if (g >= 16)
            put_str(p, g == 16 ? "1 BAR" : g == 32 ? "2 BARS" : "4 BARS");
        else
            put_str(put_dec(p, (uint32_t)g), "/16");           /* sixteenths: never a GRID-like 1/4 */
    } else {
        int s = rep_pitch_at[pos];
        if (s == REP_STOP)
            put_str(p, "TAPE STOP");
        else if (s == REP_SPIN)
            put_str(p, "SPIN UP");
        else if (!s)
            put_str(p, "PITCH FLAT");
        else
            put_str(put_dec(put_str(p, s < 0 ? "PITCH -" : "PITCH +"), (uint32_t)(s < 0 ? -s : s)), " ST");
    }
}

/* SHIFT held: the probe page. TE's words as read now, and the DSP's clock. */
static char *wh_frac2(char *p, uint32_t v100)       /* v / 100 with two decimals */
{
    uint32_t w = udiv(v100, 100), f = v100 - 100 * w;
    p = put_dec(p, w);
    *p++ = '.';
    *p++ = (char)('0' + udiv(f, 10));
    *p++ = (char)('0' + f - 10 * udiv(f, 10));
    *p = 0;
    return p;
}

/* "BAR k/n": where the clock's bar stands in the EVERY n interval (n / n: the bar whose last beat fills); 0 otherwise */
static char *wh_bar_in(char *p, const int16_t *k, const struct rep *st)
{
    int pf = rep_pos(knob_pos(k[1]));
    if (!st || rep_fmode_at[pf] != RF_EVERY)
        return 0;
    uint32_t n = rep_fa_at[pf];
    p = put_str(p, "BAR ");
    p = put_dec(p, (st->bar & (n - 1)) + 1);
    *p++ = '/';
    return put_dec(p, n);
}

static void wh_probe(void *r, const int16_t *k, const struct rep *st, int running)
{
    char buf[48], *p;
    fp32 x = FP(8), step = FP(3) >> 1, dy = FP(13);
    fp32 y = FP(7);
    text(r, "REPEAT PROBE", x, y, FP(2), WHITE);
    int32_t ts = &te_tape_state ? te_tape_state : -1, spb = &te_tempo_spb ? te_tempo_spb : 0;
    int32_t mc = &te_metro_count ? te_metro_count : 0, tick = &te_ppqn_tick ? te_ppqn_tick : 0;
    uint32_t pos = &te_tape_pos ? te_tape_pos : 0;
    /* 1: transport and playhead */
    p = put_str(buf, "TAPE ");
    p = put_dec(p, (uint32_t)(ts < 0 ? 0 : ts));
    p = put_str(p, ts == 0 ? " STOP" : ts == 1 ? " REC" : ts == 2 ? " PLAY" : ts == 3 ? " FFREW" : " ?");
    p = put_str(p, "  POS ");
    put_dec(p, pos >> 6);
    y += FP(18);
    text_fast(r, buf, x, y, step, LIGHTBLUE);
    /* 2: the tape's bar and beat from the playhead (4 beats from 0) */
    uint32_t beat = spb > 0 ? ((uint32_t)spb + 32) >> 6 : 0;       /* Q8 samples per beat */
    p = put_str(buf, "TAPE BAR ");
    if (beat) {
        uint32_t q = pos << 2, b = udiv(q, beat), fr = q - b * beat;
        p = put_dec(p, (b >> 2) + 1);
        p = put_str(p, " BEAT ");
        p = put_dec(p, (b & 3) + 1);
        p = put_str(p, ".");
        uint32_t c = udiv(fr * 25, beat >> 2);                      /* 0 .. 99 */
        *p++ = (char)('0' + udiv(c, 10));
        *p++ = (char)('0' + c - 10 * udiv(c, 10));
        *p = 0;
    } else
        put_str(p, "-");
    y += dy;
    text_fast(r, buf, x, y, step, LIGHTBLUE);
    /* 3: tempo word, BPM */
    p = put_str(buf, "SPB ");
    p = wh_frac2(p, spb > 0 ? (((uint32_t)spb >> 7) * 25) >> 5 : 0);        /* samples x 100 */
    p = put_str(p, "  ");
    if (spb >= 1024)
        p = put_str(wh_frac2(p, udiv(4233600000u, (uint32_t)spb >> 10)), "BPM");   /* 2646000 x 100 x 16 / (spb / 1024) */
    y += dy;
    text_fast(r, buf, x, y, step, LPG);
    /* 4: metronome countdown, sequencer tick, tape speed */
    p = put_str(buf, "METRO ");
    p = put_dec(p, mc > 0 ? (uint32_t)mc >> 14 : 0);
    if (mc < 0)
        p = put_str(p, " NEG");
    p = put_str(p, "  TICK ");
    put_dec(p, (uint32_t)tick);
    y += dy;
    text_fast(r, buf, x, y, step, LPG);
    /* 5: the tape speed word, as a ratio of 0x08000000 (1.0: Hypothesis) and in hex */
    uint32_t spd = (uint32_t)TE_TAPE_SPEED;
    p = put_str(buf, "SPEED ");
    p = wh_frac2(p, spd < 0x40000000u ? udiv(spd, 0x08000000u / 100) : 0);
    p = put_str(p, " = HEX ");
    put_hex(p, spd, 8);
    y += dy;
    text_fast(r, buf, x, y, step, LPG);
    /* 5: repeat's own clock */
    p = put_str(buf, "OURS BAR ");
    if (st) {
        p = put_dec(p, st->bar + 1);
        p = put_str(p, " BEAT ");
        uint32_t b4 = st->bar8 >> 2;
        uint32_t bt = b4 ? udiv(st->ph8, b4) : 0;
        p = put_dec(p, (bt > 3 ? 3 : bt) + 1);
        p = put_str(p, st->lock == RL_TAPE ? "  CLOCK TAPE" : st->lock == RL_METRO ?
                    (st->metro_run ? "  CLOCK METRO" : "  CLOCK FREE") : "  CLOCK -");
        if (!running)
            p = put_str(p, " IDLE");
    } else
        put_str(p, "-");
    y += dy;
    text_fast(r, buf, x, y, step, GREEN);
    /* 6: the repeats */
    if (st) {
        p = put_str(buf, "FIRES ");
        p = put_dec(p, st->fires);
        p = put_str(p, " LOST ");
        p = put_dec(p, st->lost);
        p = put_str(p, st->active ? (st->hold ? " LATCH" : " GATE") : " DRY");
        p = put_str(p, " REP ");
        p = put_dec(p, (uint32_t)(st->rep < 0 ? 0 : st->rep));
        char *q = wh_bar_in(put_str(p, "  "), k, st);
        if (!q)
            *p = 0;
        y += dy;
        text_fast(r, buf, x, y, step, GREEN);
    }
    /* 8: the encoder pushes that reached a controller (any screen), the last code and controller, green taken / held */
    if (&rep_key) {
        p = put_str(buf, "KEYS P ");
        p = put_dec(p, rep_key.presses);
        p = put_str(p, " R ");
        p = put_dec(p, rep_key.releases);
        p = put_str(p, " LAST ");
        p = put_hex(p, rep_key.last, 2);
        p = put_str(p, rep_key.last_ctl == RK_MASTER ? " M" : rep_key.last_ctl == RK_DRUM ? " D" : " S");
        p = put_str(p, " TAKEN ");
        p = put_dec(p, rep_key.taken);
        if (rep_key.held[0] | rep_key.held[1] | rep_key.held[2])
            put_str(p, " HELD");
        y += dy;
        text_fast(r, buf, x, y, step, GREEN);
    }
}

/* The line screen (no wheel.svg): the values, the bar as a line with the gate lit, the playhead. */
static void draw_repeat_lines(void *r, const int16_t *k, const struct rep *st, int nokey)
{
    char buf[24];
    static const int colours[4] = { BLUE, GREEN, WHITE, ORANGE };
    title(r, "REPEAT", WHITE);
    for (int i = 0; i < 4; i++) {
        wh_readout(buf, i, k, st, nokey);
        text(r, buf, FP(8), FP(30 + 18 * i), FP(3) >> 1, colours[i]);
    }
    if (st && rep_fmode_at[rep_pos(knob_pos(k[1]))] != RF_OFF)   /* the roll: playing or waiting */
        text(r, st->active ? (st->hold ? "LATCHED" : "FIRING") : "WAITING", FP(200), FP(30), FP(3) >> 1,
             st->active ? GREEN : DARKGREEN);
    char bb[12];
    if (wh_bar_in(bb, k, st))
        text_fast(r, bb, FP(200), FP(50), FP(3) >> 1, WHITE);
    fp32 x0 = FP(16), x1 = FP(304), yb = FP(120);
    line_fast(r, x0, yb, x1, yb, DARKBLUE);
    int g = rep_eff_gate(st ? st->blue : rep_grid_at[rep_pos(knob_pos(k[0]))], rep_gate_at[rep_pos(knob_pos(k[2]))]);
    fp32 gw = (fp32)(((x1 - x0) * (g > 16 ? 16 : g)) >> 4);
    line_fp(r, x1 - gw, yb, x1, yb, FP(3), WHITE);
    if (st && st->bar8 >= 1u << 14) {
        uint32_t t = udiv(st->ph8 >> 4, (st->bar8 >> 14) + 1);     /* 0 .. 1023 */
        fp32 px = x0 + (fp32)(((uint32_t)(x1 - x0) * (t > 1023 ? 1023 : t)) >> 10);
        line_fp(r, px, yb - FP(6), px, yb + FP(6), FP(2), LIGHTBLUE);
    }
}

static void draw_wheel(void *r, const int16_t *k, const int16_t *stored, int running, int stereo, int ctx, int nokey,
                       int mode)
{
    const struct rep *st = repeat_state ? &repeat_state[stereo] : 0;
    uint32_t f = ++wh.frame;
    /* the readout: a stored knob moved >= 512 raw shows its value ~1.5 s (not on entering the screen) */
    if (ctx != wh.ctx) {
        wh.ctx = ctx;
        wh.until = f;
        for (int i = 0; i < 4; i++)
            wh.ref[i] = stored[i];
    }
    for (int i = 0; i < 4; i++) {
        int32_t d = stored[i] - wh.ref[i];
        if (d >= 512 || d <= -512) {
            wh.ref[i] = stored[i];
            wh.shown = i;
            wh.until = f + WH_SHOW;
        }
    }
    if (&rep_key) {                                                 /* the manual key's heartbeat (repeat.h) */
        rep_key.screen_ctl = (uint8_t)(mode == 3 ? RK_MASTER : mode == 1 ? RK_DRUM : RK_SYNTH);
        rep_key.screen_seen = &te_blocks ? te_blocks : 0;
    }
    if (WH_SHIFT) {
        wh_probe(r, k, st, running);
        return;
    }
    if (!wheel_ready()) {
        draw_repeat_lines(r, k, running ? st : 0, nokey);
        return;
    }
    int bypass = !running || !st;
    int pg = rep_pos(knob_pos(k[0])), pf = rep_pos(knob_pos(k[1])), pt = rep_pos(knob_pos(k[2]));
    int pp = rep_pos(knob_pos(k[3]));
    int blue = bypass ? rep_grid_at[pg] : st->blue;
    int active = !bypass && st->active;
    int fm = rep_fmode_at[pf], fa = rep_fa_at[pf];
    int latch = !bypass && fm == RF_ALWAYS;
    /* the DSP's events since the last frame */
    if (st) {
        if (st->fires != wh.fires && active)
            wh.fire_f = f;
        if (st->lost != wh.lost)
            wh.lost_f = f;
        if (st->rep != wh.rep && active)
            wh.rep_f = f;
        wh.fires = st->fires;
        wh.lost = st->lost;
        wh.rep = st->rep;
    }

    /* blue: the spokes of the grid played (VARY's pick while it plays), blur rings for 1/32T and 1/64 */
    int g = active ? st->grid : blue, n = rep_slices(g);
    for (int i = 0; i < 9; i++)
        show(wh_spk[i], wh_spokes[i] == n);
    show(wh_node[W_BLUR1], n >= 48);
    show(wh_node[W_BLUR2], n >= 64);
    /* the repeats turn the wheel */
    set_rotation(wh_node[W_WHEEL], st ? st->turn : 0, WH_X, WH_Y);

    /* white: the gate on the bar clock (its sixteenths ending at 12), the grid-wins stretch, the laps; the playhead */
    int manual = active && st->key;
    int gate = rep_gate_at[pt], ge = rep_eff_gate(blue, gate), nogate = latch || manual || bypass || fm == RF_OFF;
    for (int i = 1; i <= 16; i++) {
        show(wh_gate[i - 1], !nogate && i > 16 - gate);
        show(wh_gatex[i - 1], !nogate && 16 - ge < i && i <= 16 - gate);
    }
    show(wh_node[W_LAP1], !nogate && gate >= 32);
    show(wh_node[W_LAP2], !nogate && gate >= 64);
    uint32_t ph = 0;
    if (st && st->bar8 >= 4096)
        ph = udiv(st->ph8 > st->bar8 ? st->bar8 : st->ph8, st->bar8 >> 12) << 20;    /* turns, 12 bits */
    set_rotation(wh_node[W_PLAYHEAD], ph, WH_X, WH_Y);
    show(wh_node[W_PLAYHEAD], !bypass);

    /* green: the lots (a lost roll blinks them out), the books (VARY), the sword (a won roll to the gate's end) */
    int lots = bypass || f - wh.lost_f < WH_BLINK ? 0 : wh_lots[pf];
    for (int i = 0; i < 12; i++)
        show(wh_ch[i], i < lots);
    static const uint8_t order[4] = { 2, 3, 1, 0 };                 /* bull, lion, eagle, angel */
    int nbk = bypass ? 0 : fm == RF_ALWAYS ? 4 : fm == RF_VARY ? fa : 0;
    int flip = active && fm == RF_VARY && st->rep >= 1 && f - wh.rep_f < WH_BLINK && st->grid != st->blue;
    for (int j = 0; j < 4; j++) {
        int b = order[j];
        show(wh_book[b][1], j < nbk);
        show(wh_book[b][0], j >= nbk);
        uint32_t rep = flip ? (uint32_t)st->rep : 0;
        show(wh_book[b][2], flip && j < nbk && (int)(rep - (uint32_t)nbk * udiv(rep, (uint32_t)nbk)) == j);
    }
    int up = active;
    show(wh_node[W_SWUP], up);
    show(wh_node[W_SWREST], !up);
    if (up && f - wh.fire_f < WH_RAISE - 1)                         /* rotate(109 (1 - u)), u = 1 / 6 .. 1 */
        set_rotation(wh_node[W_SWUP], udiv(WH_SWORD_REST * DEG, WH_RAISE) * (WH_RAISE - 1 - (f - wh.fire_f)),
                     WH_SWORD_X, WH_SWORD_Y);
    else
        set_rotation(wh_node[W_SWUP], 0, WH_SWORD_X, WH_SWORD_Y);
    show(wh_node[W_HUBMAN], manual);                                /* the manual key (green's push) held */

    /* orange: the snake carried down / Anubis climbing with the current repeat's depth; the setting's dots */
    int s = rep_pitch_at[pp];
    int down = s == REP_STOP || (s < 0), upw = s == REP_SPIN || (s > 0 && s != REP_SPIN);
    static const int8_t dots_of[13] = { 0, 1, 2, 3, 0, 4, 0, 5, 0, 0, 0, 0, 6 };
    int dots = s == REP_STOP || s == REP_SPIN ? 6 : dots_of[s < 0 ? -s : s];
    show(wh_node[W_SNAKEORG], down);
    show(wh_node[W_SNAKEGREY], !down);
    show(wh_node[W_ANUBORG], upw);
    show(wh_node[W_ANUBGREY], !upw);
    for (int i = 0; i < 6; i++) {
        show(wh_sink[i], down && i < dots);
        show(wh_rise[i], upw && i < dots);
    }
    show(wh_node[W_SINKEND], s == REP_STOP);                       /* the mode marks past the 6th dot */
    show(wh_node[W_RISEEND], s == REP_SPIN);
    int32_t d = active ? clamp_i(st->depth, 0, 32767) : 0;          /* Q15 */
    if (down)                                                       /* rotate(-75 d): 75 deg / 32768 = 27307 turns */
        set_rotation(wh_node[W_SNAKE], (uint32_t)-(int32_t)(27307 * d), WH_X, WH_Y);
    else
        set_matrix(wh_node[W_SNAKE], 1024, 0, 0, 1024, 0, 0);
    if (upw) {
        uint32_t a0 = WH_ANUB_A0 * DEG, a1 = a0 - 20025u * (uint32_t)d;   /* 55 deg / 32768 */
        fp32 dx = (WH_ANUB_R * (sin_t(a1) - sin_t(a0))) >> 5, dy = -((WH_ANUB_R * (cos_t(a1) - cos_t(a0))) >> 5);
        translate(wh_node[W_ANUBIS], dx, dy);
    } else
        set_matrix(wh_node[W_ANUBIS], 1024, 0, 0, 1024, 0, 0);

    int ro = f < wh.until && wh.shown >= 0;
    show(wh_node[W_ANGEL], !ro);
    svg_render(wheel_doc.doc);
    if (!bypass) {                                                  /* white: the bar within the EVERY n interval */
        char bb[12];
        if (wh_bar_in(bb, k, st))
            text_fast(r, bb, FP(62), FP(127), FP(1), WHITE);
    }
    if (ro) {
        static const int colours[4] = { BLUE, GREEN, WHITE, ORANGE };
        char buf[24];
        wh_readout(buf, wh.shown, k, bypass ? 0 : st, nokey);
        text(r, buf, FP(8), FP(7), FP(2), colours[wh.shown]);
        if (wh.shown == 0 && !bypass && st->fold) {                 /* the grid that doesn't fit plays its half */
            put_str(put_str(buf, wh_grid_name(st->blue - st->fold)), " TOO LONG");
            text_fast(r, buf, FP(8), FP(23), FP(1), GREY);
        }
    }
    svg_cost(r);
}
