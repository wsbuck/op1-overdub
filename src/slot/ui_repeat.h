/* repeat FX screen: the Wheel of Fortune, the sphinx and the snake (design/fx/repeat.svg by William Buck, from
 * design/fx/gen_repeat.py, published as content/display/repeat.svg; its header comment is the runtime contract,
 * followed here). Included by ui.c after its SVG helpers. Inputs: the knobs the FX used (fx_live: TE's LFO moves them),
 * the DSP's state (repeat_state: the grid played, the repeat, the depth, the wheel's turn, the bar clock, the gate
 * left, won and lost rolls) and the frame counter (30 fps). Knob turns (stored knobs, >= 512 raw) show the value
 * ~1.5 s in the design's top-left box in the knob's colour. While SHIFT is held: the bar-clock probe page instead
 * (TE's transport, playhead, tempo, metronome and sequencer words, and repeat's own clock; notes/fx-beatrepeat.md
 * "Probe"). Without repeat.svg (or one without the marker `stripes_dim`): a line screen. */
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
#define WH_X 100                                    /* the wheel's centre (repeat.svg) */
#define WH_Y 80
#define WH_POW 4                                    /* frames `pow` flashes on each repeat (0.13 s) */
#define WH_SHOW 45                                  /* frames a readout stays (~1.5 s) */

enum { W_WHEEL, W_PLAYHEAD, W_EYESHUT, W_EYEOPEN, W_POW, W_TAPESTOP, W_SPINUP, W_SNAKE, W_SNAKEGREY, W_SNAKEUP,
       W_SNAKEDOWN, W_SINGLE };
static const char *const wh_single[W_SINGLE] = { "wheel", "playhead", "eye_shut", "eye_open", "pow", "tapestop",
    "spinup", "snake", "snake_grey", "snake_up", "snake_down" };
static const uint8_t wh_grids[11] = { 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };

static struct svgdoc wheel_doc RAM = { 0, 0 };
static void *wh_node[W_SINGLE] RAM = { 0 };
static void *wh_ticks[11] RAM = { 0 }, *wh_slice[11] RAM = { 0 }, *wh_stripe[10] RAM = { 0 };
static void *wh_gate[16] RAM = { 0 }, *wh_gdim[16] RAM = { 0 }, *wh_lap[3] RAM = { 0 };
static void *wh_up[12] RAM = { 0 }, *wh_down[12] RAM = { 0 };
struct wh_view {
    uint32_t frame;
    uint32_t lost;                                  /* the DSP's lost count at the last frame */
    uint32_t rep_f;                                 /* frame of the last new repeat (or fill) */
    uint32_t lost_line;                             /* a lost roll's line (repeat.c s->line: the bar, or beat, its
                                                     * gate would have ended on; 0: none) */
    int32_t lost_beats;                             /* that line counts beats (CHANCE / VARY gates of a beat or less) */
    uint32_t turn0;                                 /* the wheel's turn at the fill's start */
    int32_t rep, was;                               /* the repeat and `active` at the last frame */
    int32_t ctx, shown;                             /* the readout: screen context, knob shown, until frame */
    uint32_t until;
    int16_t ref[4];
};
static struct wh_view wh RAM = { 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, 0, { 0, 0, 0, 0 } };

/* "pre" + n (1 .. 99) into buf, no libc */
static const char *wh_id(char *buf, const char *pre, int n)
{
    char *p = buf;
    while (*pre)
        *p++ = *pre++;
    int t = 0;
    while (n >= 10)
        n -= 10, t++;
    if (t)
        *p++ = (char)('0' + t);
    *p++ = (char)('0' + n);
    *p = 0;
    return buf;
}

static void wh_find(void **dst, const char *pre, int n)
{
    char id[16];
    for (int i = 0; i < n; i++)
        dst[i] = te_node_find(wheel_doc.doc, wh_id(id, pre, i + 1), 0);
}

static int wheel_ready(void)
{
    if (wheel_doc.state)
        return wheel_doc.state > 0;
    if (!svg_open(&wheel_doc, "/yaffs2/content/display/repeat.svg", "repeat.svg"))
        return 0;
    if (!te_node_find(wheel_doc.doc, "stripes_dim", 0)) {          /* not this design's file: the line screen */
        wheel_doc.state = -1;
        return 0;
    }
    char id[16];
    for (int i = 0; i < W_SINGLE; i++)
        wh_node[i] = te_node_find(wheel_doc.doc, wh_single[i], 0);
    for (int i = 0; i < 11; i++) {
        wh_ticks[i] = te_node_find(wheel_doc.doc, wh_id(id, "ticks", wh_grids[i]), 0);
        wh_slice[i] = te_node_find(wheel_doc.doc, wh_id(id, "slice", wh_grids[i]), 0);
    }
    wh_find(wh_stripe, "stripe", 10);
    wh_find(wh_gate, "gate", 16);
    wh_find(wh_gdim, "gate_dim", 16);
    wh_find(wh_lap, "lap", 3);
    wh_find(wh_up, "up", 12);
    wh_find(wh_down, "down", 12);
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

/* The line screen (no repeat.svg): the values, the bar as a line with the gate lit, the playhead. */
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

/* The wheel's turn per slice of grid g (repeat.c boundary(): one slice per repeat) */
static uint32_t wh_slice_turn(int g)
{
    return g & REP_T ? 0x55555555u >> (g & 7) : (g & 7) ? 1u << (32 - (g & 7)) : 0;
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
        if (st)
            wh.lost = st->lost;                                     /* rolls lost before the screen showed */
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
    int hold = active && st->hold, manual = active && st->key;
    int fm = rep_fmode_at[pf], fa = rep_fa_at[pf];
    int ge = rep_eff_gate(blue, rep_gate_at[pt]);                   /* the gate played (the grid wins) */
    /* the bar clock: phase in 1/1024 bar, the beat */
    uint32_t ph10 = 0, b10 = !bypass && st->bar8 >= 1u << 12 ? st->bar8 >> 10 : 0, bar = bypass ? 0 : st->bar;
    if (b10) {
        ph10 = udiv(st->ph8 > st->bar8 ? st->bar8 : st->ph8, b10);
        ph10 = ph10 > 1023 ? 1023 : ph10;
    }
    /* the DSP's events since the last frame: a new fill or repeat (the wheel's start, pow), a lost roll */
    if (active && (!wh.was || st->rep < wh.rep)) {
        wh.turn0 = st->turn - (st->kind == RR_STEP ? (uint32_t)st->rep * wh_slice_turn(st->grid) : 0);
        wh.rep_f = f;
    } else if (active && st->rep != wh.rep)
        wh.rep_f = f;
    if (st && st->lost != wh.lost && !active) {                     /* repeat.c: such gates roll on every beat */
        wh.lost_line = st->line;
        wh.lost_beats = (fm == RF_CHANCE || fm == RF_VARY) && ge <= 4;
    }
    uint32_t at = wh.lost_beats ? bar << 2 | (ph10 >> 8) : bar;     /* the bar (beat) the playhead is in */
    int lost = wh.lost_line && wh.lost_line - at - 1 < 4;           /* the lost gate's end line still to come */
    if (st) {
        wh.lost = st->lost;
        wh.rep = st->rep;
    }
    wh.was = active;

    /* blue: the grid's ticks (VARY's pick while it plays), the captured slice while firing; the wheel turns one slice
     * per repeat from the fill's start (TAPE STOP / SPIN UP: with the read), upright otherwise */
    int n = rep_slices(active ? st->grid : blue);
    for (int i = 0; i < 11; i++) {
        show(wh_ticks[i], wh_grids[i] == n);
        show(wh_slice[i], active && wh_grids[i] == n);
    }
    set_rotation(wh_node[W_WHEEL], active ? st->turn - wh.turn0 : 0, WH_X, WH_Y);

    /* green: the headdress stripes. CHANCE 10..90 % the first 1..9, ALWAYS all, VARY 2 4 6 8 10, EVERY n the bars
     * elapsed of the interval x 8 / n (EVERY BAR: 8, every bar is due), OFF none */
    uint32_t lit = 0;                                               /* bit i: stripe i + 1 */
    if (fm == RF_ALWAYS)
        lit = 0x3FF;
    else if (fm == RF_VARY)
        lit = 0x2AA;
    else if (fm == RF_CHANCE)
        lit = (1u << (((uint32_t)rep_chance_at[pf] * 10 + 32768) >> 16)) - 1;
    else if (fm == RF_EVERY)
        lit = (1u << (fa == 1 ? 8 : udiv((bar & (uint32_t)(fa - 1)) * 8, (uint32_t)fa))) - 1;
    for (int i = 0; i < 10; i++)
        show(wh_stripe[i], lit >> i & 1);

    /* white: the gate on the ground (one bar, ending at the bar line). Waiting: the last ge sixteenths, white when
     * this bar will fire, darkblue when not (OFF, a lost roll, EVERY n off the interval, the FX off). Firing: from
     * the playhead to the gate's end (it burns down), the laps the bars still to come; the latch (ALWAYS, the manual
     * key): the whole bar from the playhead. */
    int first = 17 - (ge > 16 ? 16 : ge), last = 16, laps = ge > 16 ? (ge > 48 ? 3 : (ge + 15) / 16 - 1) : 0;
    int lit_gate;
    if (active) {
        lit_gate = 1;
        first = (int)(ph10 >> 6) + 1;
        if (hold || !b10)
            laps = 0;
        else {
            uint32_t e10 = ph10 + udiv((uint32_t)(st->grem < 0 ? 0 : st->grem), b10);
            last = e10 > 1024 ? 16 : (int)((e10 + 63) >> 6);
            laps = e10 > 1024 ? (int)((e10 - 1) >> 10) : 0;
            laps = laps > 3 ? 3 : laps;
        }
    } else {
        uint32_t m = fa > 0 ? (uint32_t)(fa - 1) : 0;
        int due = fm != RF_EVERY || (ge < 16 && !((bar + 1) & m)) || fa == 1;
        lit_gate = !bypass && fm != RF_OFF && due && !lost;
    }
    for (int i = 1; i <= 16; i++) {
        int on = first <= i && i <= last;
        show(wh_gate[i - 1], on && lit_gate);
        show(wh_gdim[i - 1], on && !lit_gate);
    }
    for (int i = 0; i < 3; i++)
        show(wh_lap[i], i < laps);
    translate(wh_node[W_PLAYHEAD], (fp32)(280 * ph10), 0);          /* translate(280 x bar phase, 0) */
    show(wh_node[W_PLAYHEAD], !bypass);

    /* firing: the sphinx's eye opens, pow flashes on each repeat (held steady while the manual key holds the latch:
     * the old screen's lit hub) */
    show(wh_node[W_EYEOPEN], active);
    show(wh_node[W_EYESHUT], !active);
    show(wh_node[W_POW], active && (manual || f - wh.rep_f < WH_POW));

    /* orange: the knob's dots on its side of the dial, the extremes' marks; the snake (grey flat, climbing up, diving
     * down) rides rot(3.5 st) about the wheel's centre, st = the semitones reached so far (+-12.5 at the extremes) */
    int s = rep_pitch_at[pp], ext = s == REP_SPIN || s == REP_STOP;
    int nd = ext ? 12 : s < 0 ? -s : s;
    for (int i = 0; i < 12; i++) {
        show(wh_up[i], s > 0 && i < nd);
        show(wh_down[i], s < 0 && i < nd);
    }
    show(wh_node[W_TAPESTOP], s == REP_STOP);
    show(wh_node[W_SPINUP], s == REP_SPIN);
    show(wh_node[W_SNAKEGREY], !s);
    show(wh_node[W_SNAKEUP], s > 0);
    show(wh_node[W_SNAKEDOWN], s < 0);
    int32_t d = active && s ? clamp_i(st->depth, 0, 32767) : 0;    /* Q15 of the full 12 (12.5) */
    uint32_t a = (uint32_t)d * (ext ? 15929u : 15292u);             /* 43.75 / 42 deg per 32768, in turns */
    set_rotation(wh_node[W_SNAKE], s < 0 ? (uint32_t)-a : a, WH_X, WH_Y);

    int ro = f < wh.until && wh.shown >= 0;
    svg_render(wheel_doc.doc);
    if (!bypass) {                          /* the bar within the EVERY n interval, top right over the headdress */
        char bb[12];
        char *e = wh_bar_in(bb, k, st);
        if (e)
            text_fast(r, bb, FP(316) - FP(6) * (fp32)(e - bb), FP(5), FP(1), WHITE);
    }
    if (ro) {                               /* the readout in the design's top-left box */
        static const int colours[4] = { BLUE, GREEN, WHITE, ORANGE };
        char buf[24];
        wh_readout(buf, wh.shown, k, bypass ? 0 : st, nokey);
        text(r, buf, FP(8), FP(7), FP(2), colours[wh.shown]);
        if (wh.shown == 0 && !bypass && st->fold) {                 /* the grid that doesn't fit plays its half */
            put_str(put_str(buf, wh_grid_name(st->blue - st->fold)), " TOO LONG");
            text_fast(r, buf, FP(136), FP(13), FP(1), GREY);
        }
    }
    svg_cost(r);
}
