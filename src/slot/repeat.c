/* "repeat" FX (src/slot/fx.json): a beat repeat in the manner of Ableton Live's, on the OP-1's tempo and the tape's
 * bars. Design, maps and the screen: notes/fx-beatrepeat.md; knob maps: repeat.h (= design/fx/gen_wheel.py); FX
 * contract: notes/fx-slot.md.
 *   blue   GRID: the slice, 1/64 .. 1/2 with triplets (repeat.h rep_grid_at)
 *   green  FORTUNE: when it fires: ALWAYS (the latch, hard left), OFF, EVERY 8 / 4 / 2 BARS, CHANCE 10 .. 90 % per
 *          bar, EVERY BAR, VARY 1 .. 4 (each repeat draws its grid near blue's)
 *   white  GATE: how long, 1/16 .. 4 bars, ending on the bar line; the grid wins (at least two slices)
 *   orange PITCH: SPIN UP, +12 .. +1, FLAT, -1 .. -12, TAPE STOP: the total reached at the gate's last repeat
 * Insert mode: dry outside the gates (bit exact), the repeats only inside them; every slice boundary and gate edge
 * crossfades over 32 samples. An automatic gate captures forward (its first slice is the live input; the line records
 * it), the latch backward (the grid that ended at its 1/16 line). The line keeps recording until it would overwrite
 * the slice (a freeze), so a gate or the latch can last any time and VARY can draw longer grids from the same start.
 *
 * Bars: TE's tape grid, 4 beats from tape position 0. While the tape rolls (transport 1 record, 2 play) the bar phase
 * is the tape playhead 0xFF802974 (samples x 64); stopped, our own count advances 64 samples a block and locks to the
 * metronome's countdown 0x01AADADC (the click), continuing the tape's bar count. A trigger is the sample where the
 * position crosses the gate's start (the bar line minus the gate); gates then count samples (a tape loop, jump or stop
 * inside a gate cannot strand it, and nothing retriggers within a gate: a trigger extends it). CHANCE and VARY roll a
 * hash of the bar number (and the instance): the same fills from the same spot.
 *
 * Memory: pingpong's lines (pingpong_ram, 0x01E80000): stereo L, R and mono, 65536 samples each, shared through
 * fx_line_owner (pingpong.h): the FX that finds another owner takes the set over and counts what it has written since
 * (filled), so nothing the other one left is ever read. No division in the sample loop; one shift-subtract division
 * per block for the bar phase.
 */
#include <stdint.h>
#include "repeat.h"
#include "pingpong.h"
#include "fxpub.h"

#ifdef __bfin__
#define RAM __attribute__((section(".data")))
#else
#define RAM                                 /* the host previews (test/preview.py) */
#endif
#define BLOCK 64
#define RING REP_RING
#define MASK (RING - 1)
#define XF REP_XF
#define FREEZE (RING - 128)                 /* the line stops recording this far from the slice's start */
#define LP_START ((int32_t)0x80000000)      /* the tone's state: start from the signal */
#define CATCH8 (128 << 8)                   /* a line passed within the last 128 samples still fires (varispeed) */

void repeat_mono(void *self, int16_t *buf, const int16_t *p);
void repeat_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);

extern int16_t pingpong_ram[];              /* stereo L, stereo R, mono (pingpong.c) */
/* TE's words (build_slot.py TE_UI; notes/fx-beatrepeat.md "Sync"), read once per block */
extern volatile int32_t te_tempo_spb;       /* 0x01AADAD8 samples per beat, Q14 */
extern volatile int32_t te_tape_state;      /* 0xFF802960 transport: 0 stop, 1 record, 2 play, 3 FF / REW */
extern volatile uint32_t te_tape_pos;       /* 0xFF802974 playhead, samples x 64 */
extern volatile int32_t te_metro_count;     /* 0x01AADADC samples to the next click, Q14 */
extern volatile int32_t te_play_slot;       /* 0xFF8016D4 the synth slot the keyboard plays (the fills' seed) */

struct rep repeat_state[2] RAM = { { .vpos = 0 }, { .vpos = 0 } };
struct fx_pub repeat_pub[2] RAM = { { { 0 }, 0 }, { { 0 }, 0 } };
volatile struct rep_key rep_key RAM = { .presses = 0 };

/* The manual key (repeat.h): from the KeyHandler thread, on the controllers' press / release slots. Returns 1 when the
 * press is taken (TE's function is then not called). */
int rep_key_event(int ctl, int key, int down)
{
    if (key < 0x3B || key > 0x3E || ctl < 0 || ctl > 2)
        return 0;
    if (down)
        rep_key.presses++;
    else
        rep_key.releases++;
    rep_key.last = (uint8_t)key;
    rep_key.last_ctl = (uint8_t)ctl;
    if (key != REP_KEY_GREEN)
        return 0;
    if (!down) {
        rep_key.held[ctl] = 0;
        return 0;
    }
    if (rep_key.screen_ctl != ctl || te_blocks - rep_key.screen_seen > REP_KEY_STALE)
        return 0;                                       /* not on the repeat screen of this context: TE's */
    rep_key.held[ctl] = 1;
    rep_key.taken++;
    return 1;
}

/* The drum and mixer controllers' press / release slots (build_slot.py installs them with this FX: drum vtable
 * 0x0190FA64, mixer 0x01910AAC, +0x14 / +0x24, checked against stock); the synth's are CHORD v2's c2_press /
 * c2_release (hold.c), which call rep_key_event too. */
extern int te_drum_press(void *self, int key);      /* FUN_01978008 */
extern int te_drum_release(void *self, int key);    /* FUN_019773e4 */
extern int te_master_press(void *self, int key);    /* FUN_019db7c4 */
extern int te_master_release(void *self, int key);  /* FUN_019db708 */
int rk_drum_press(void *self, int key);
int rk_drum_release(void *self, int key);
int rk_master_press(void *self, int key);
int rk_master_release(void *self, int key);

int rk_drum_press(void *self, int key)
{
    return rep_key_event(RK_DRUM, key, 1) ? 1 : te_drum_press(self, key);
}

int rk_drum_release(void *self, int key)
{
    rep_key_event(RK_DRUM, key, 0);
    return te_drum_release(self, key);
}

int rk_master_press(void *self, int key)
{
    return rep_key_event(RK_MASTER, key, 1) ? 1 : te_master_press(self, key);
}

int rk_master_release(void *self, int key)
{
    rep_key_event(RK_MASTER, key, 0);
    return te_master_release(self, key);
}

static inline int32_t knob(int16_t k)
{
    return k < 0 ? 0 : k;
}

/* n / d and n % d (d > 0), shift-subtract over the quotient's bits only: no libgcc. */
static uint32_t udivmod(uint32_t n, uint32_t d, uint32_t *rem)
{
    uint32_t q = 0, bit = 1;
    if (!d) {
        *rem = 0;
        return 0xFFFFFFFFu;
    }
    while (d < n && !(d & 0x80000000u)) {
        d <<= 1;
        bit <<= 1;
    }
    while (bit) {
        uint32_t ge = n >= d;
        n -= d & -ge;
        q |= bit & -ge;
        d >>= 1;
        bit >>= 1;
    }
    *rem = n;
    return q;
}

static inline uint32_t udiv(uint32_t n, uint32_t d)
{
    uint32_t r;
    return udivmod(n, d, &r);
}

static inline uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* 2^(x / 65536), Q16, for x within +-4 octaves (pingpong.h's 2^(i/16) table, interpolated). */
static uint32_t exp2_q16(int32_t x)
{
    int32_t oct = x >> 16;
    uint32_t f = (uint32_t)x & 0xFFFF, i = f >> 12, t = f & 4095;
    uint32_t m = 65536 + pp_exp2_frac[i] + (((pp_exp2_frac[i + 1] - pp_exp2_frac[i]) * t) >> 12);
    return oct >= 0 ? m << oct : m >> -oct;
}

/* One slice of grid g at tempo spb (Q14), Q8 samples, halved until it fits the line (*fold halvings: the grid played
 * is g + *fold, still on the beat grid). */
static uint32_t slice8(int g, int32_t spb, int *fold)
{
    int sh = g & 7;
    uint32_t u;                                         /* Q14: 4 beats / slices-per-bar */
    if (g & REP_T)
        u = (pp_div3((uint32_t)spb) << 2) >> sh;
    else
        u = sh >= 2 ? (uint32_t)spb >> (sh - 2) : (uint32_t)spb << (2 - sh);
    *fold = 0;
    while (u > (uint32_t)REP_LMAX << 14) {
        u >>= 1;
        ++*fold;
    }
    return (u + 32) >> 6;
}

/* The coupled tone's coefficient for depth d (Q15): down, a one-pole low-pass closing to ~1.4 kHz (Q15, 32768 = open:
 * exactly the input); up, the matching high-pass opening from ~5 Hz to ~600 Hz (Q16). */
static int32_t tone_coef(int dir, int32_t d)
{
    if (dir < 0) {
        uint32_t b = exp2_q16(-((d * 39) >> 3)) >> 1;  /* 2^(-2.44 d) */
        return b > 32768 ? 32768 : (int32_t)b;
    }
    return (int32_t)((47 * exp2_q16((d * 219) >> 4)) >> 16);    /* 47 x 2^(6.84 d) */
}

/* The bar clock for this block: s->vpos, ph8, bar, bar8, lock. */
static void clock(struct rep *s, int32_t spb)
{
    uint32_t bar8 = ((uint32_t)spb + 8) >> 4, v;
    int32_t st = te_tape_state;
    s->jumped = 0;
    uint32_t pos = te_tape_pos;
    if ((st == 1 || st == 2) && pos < 1u << 30) {
        v = pos << 2;                                   /* the tape's bars */
        s->lock = RL_TAPE;
        s->clocked = 1;
    } else {
        v = s->vnext;                                   /* our own count, on from the last block (or the tape) */
        int32_t c = te_metro_count;
        uint32_t beat8 = ((uint32_t)spb + 32) >> 6, d = (uint32_t)c - ((uint32_t)s->last_c - (BLOCK << 14));
        /* the countdown is the beat only while it runs: one block less than the last, or that plus a reload (a
         * click). With the tape stopped TE's metronome may hold it (tape-follow, 0x01AADAD0: it advances by the
         * tape's own advance): then our count runs free at the tempo (REPEAT1: no fill ever fired) */
        int run = c >= 0 && c <= spb + (spb >> 4) && s->last_ok                     /* d: c less the expected */
                  && (d + (1u << 14) < 2u << 14 || d - (uint32_t)spb + (1u << 14) < 2u << 14);
        if (run) {
            uint32_t m, c8 = (uint32_t)c >> 6;
            if (!s->clocked) {                          /* no clock before (boot): the next click is beat 1 */
                v = bar8 - c8;
                s->jumped = 1;                          /* (a line it skips is not a late trigger) */
            }
            else {
                udivmod(v + c8, beat8, &m);
                v += m < beat8 >> 1 && m <= v ? -m : beat8 - m;
            }
            s->clocked = 1;
        }
        s->metro_run = (uint8_t)run;
        if (v >= 0xF0000000u) {                         /* stopped for minutes: whole 8-bar groups off */
            uint32_t r;
            udivmod(v, bar8 << 3, &r);
            v = r;
        }
        s->lock = RL_METRO;
    }
    s->last_c = te_metro_count;                         /* (also while the tape rolls: a stop then continues it) */
    s->last_ok = s->last_c >= 0 && s->last_c <= spb + (spb >> 4);
    uint32_t ph;
    s->bar = udivmod(v, bar8, &ph);
    s->vpos = v;
    s->ph8 = ph;
    s->bar8 = bar8;
    s->vnext = v + (BLOCK << 8);
}

/* What the knobs and the tempo ask for this block. */
struct blk {
    int blue, fold;                         /* blue's grid played, its halvings */
    uint32_t len8;                          /* its slice, Q8 */
    int fm, fa;                             /* FORTUNE mode and amount */
    uint32_t chance;
    int eff16;                              /* the gate played, sixteenths */
    uint32_t g8;                            /* the gate, Q8 */
    int32_t pitch;                          /* semitones, REP_SPIN, REP_STOP */
};

static int pitch_dir(int32_t pitch)
{
    return pitch == REP_STOP || pitch < 0 ? -1 : pitch > 0 ? 1 : 0;
}

/* The pitch of repeat k (1/16 semitones) on the staircase, clamped to the total. */
static int32_t step16(const struct rep *s, int32_t k)
{
    if (s->pitch == 0 || s->pitch == REP_SPIN || s->pitch == REP_STOP)
        return 0;
    if (k > s->nrep || k < 0)
        k = s->nrep;
    int32_t a = (int32_t)(((uint32_t)(s->pstep < 0 ? -s->pstep : s->pstep) * (uint32_t)k) >> 16), top = 16 * s->pitch;
    if (a > (top < 0 ? -top : top))
        a = top < 0 ? -top : top;
    a = (a + 8) & ~15;                                  /* whole semitones: the repeats stay in key */
    return s->pstep < 0 ? -a : a;
}

/* Start the pitch plan of a gate (or the latch): the total at repeat n, the ramps across span8. */
static void plan(struct rep *s, const struct blk *b, int32_t n, uint32_t span8)
{
    s->pitch = b->pitch;
    s->dir = (int8_t)pitch_dir(b->pitch);
    s->nrep = n < 1 ? 1 : n;
    int32_t S = b->pitch == REP_SPIN || b->pitch == REP_STOP ? 0 : b->pitch;
    uint32_t a = udiv((uint32_t)(S < 0 ? -S : S) << 20, (uint32_t)s->nrep);   /* 16 x 65536 per semitone */
    s->pstep = S < 0 ? -(int32_t)a : (int32_t)a;
    s->gate8 = (int32_t)span8;
    s->drate = 0;
    s->rate_end = 1 << 28;
    if (b->pitch == REP_SPIN || b->pitch == REP_STOP) {     /* 1 -> 2 or 1 -> 0 across the span, per sample */
        s->drate = (int32_t)udiv(0xFFFFFFFFu, span8 >> 4);  /* 2^28 / (span8 / 256) = 2^36 / span8 */
        s->rate_end = b->pitch == REP_SPIN ? 1 << 29 : 0;
        if (b->pitch == REP_STOP)
            s->drate = -s->drate;
    }
    s->rr = 1 << 28;
    s->prog8 = 0;
    s->depth = 0;
    s->g = s->gt = s->gain = 32768;
    s->gfade = 32768 << 8;
    s->gstep = 0;
    s->b = s->bt = s->dir < 0 ? 32768 : tone_coef(1, 0);
    s->tone_on = 0;
    s->lp[0] = s->lp[1] = LP_START;
    s->tps = udiv(0xFFFFFFFFu, s->bar8 >> 8);
}

/* A crossfade from the main read's current position (the tail continues it) into the new one. */
static void handover(struct rep *s)
{
    s->tpos = (s->s0 << 16) + s->rel;
    s->tinc = s->inc > 65536 ? 65536 : s->inc;          /* (a faster read is near the write: it fades at 1) */
    s->txf = XF;
}

/* The real TAPE STOP fits: the read lags the write by half the span at most. */
static int tape_fits(uint32_t span8)
{
    return (span8 >> 9) <= REP_LMAX;
}

/* An automatic trigger at this sample, d0 (Q8, 0 .. 255) after the exact crossing: start a gate, or extend the one
 * playing (a gate longer than the interval merges the events). */
static void start_gate(struct rep *s, const struct blk *b, uint32_t d0)
{
    s->fires++;
    if (s->active && !s->ending && !s->hold) {
        s->grem = (int32_t)(b->g8 - d0) + 256;
        return;
    }
    int restart = 0;
    if (s->active) {
        if (s->grem <= 256 && !s->hold)                 /* the old gate ends at this very sample */
            s->efade = 0;
        else
            restart = 1;                                /* its fade to dry finishes over the new live slice */
    }
    s->active = 1;
    s->hold = 0;
    s->ending = (uint8_t)(restart ? 2 : 0);
    s->txf = 0;
    s->s0 = s->w;
    s->rel = 0;
    s->inc = 65536;
    s->len16 = b->len8 << 8;
    s->due = (int32_t)(b->len8 - d0) + 256;            /* (this sample counts) */
    s->grem = (int32_t)(b->g8 - d0) + 256;
    s->rep = 0;
    s->grid = (uint8_t)b->blue;
    uint32_t r;
    int32_t n = (int32_t)udivmod(b->g8, b->len8, &r);
    plan(s, b, n + (r != 0) - 1, b->g8);
    s->kind = b->pitch == REP_STOP && tape_fits(b->g8) ? RR_TAPE : RR_LIVE;
    s->tone_on = s->kind == RR_TAPE;
}

/* The latch (ALWAYS): the grid that ended at its 1/16 line e8 (Q8) ago, phase-aligned to that line; a crossfade
 * from the dry input. Pitch: the total after one bar, then held. */
static void start_hold(struct rep *s, const struct blk *b, uint32_t e8)
{
    s->fires++;
    if (s->active) {                                    /* a gate is playing: it just becomes the latch */
        s->hold = 1;
        if (s->ending == 1)
            s->ending = 0;
        return;
    }
    s->active = 1;
    s->hold = 1;
    s->ending = 0;
    s->efade = 0;
    s->rep = 0;
    s->grid = (uint8_t)b->blue;
    s->len16 = b->len8 << 8;
    plan(s, b, rep_slices(b->blue), s->bar8);
    if (b->pitch == REP_STOP && tape_fits(s->bar8)) {   /* the real TAPE STOP starts on the live input itself */
        s->kind = RR_TAPE;
        s->s0 = s->w;
        s->rel = 0;
        s->inc = 65536;
        s->txf = 0;
        s->tone_on = 1;
        return;
    }
    while (e8 >= b->len8)
        e8 -= b->len8;
    s->kind = b->pitch == REP_SPIN || b->pitch == REP_STOP ? RR_RAMP : RR_STEP;
    s->s0 = s->w - (e8 >> 8) - (b->len8 >> 8);
    s->rel = (e8 >> 8) << 16;
    s->inc = 65536;
    s->due = (int32_t)(b->len8 - e8) + 256;
    s->tpos = s->w << 16;                               /* the tail is the dry input: the line at this sample */
    s->tinc = 65536;
    s->txf = XF;
    s->tone_on = s->dir != 0;
}

/* A slice boundary: the next repeat (VARY draws its grid). */
static void boundary(struct rep *s, const struct blk *b)
{
    int k = s->rep < 30000 ? ++s->rep : s->rep;
    int g = b->blue - b->fold, f;                       /* blue as set */
    if (b->fm == RF_VARY && !s->hold)
        g = rep_vary_pick(g, b->fa, b->eff16, mix32(s->line * 0x9E3779B9u + (uint32_t)k * 0x85EBCA6Bu + s->seed));
    uint32_t len8 = slice8(g, s->spb, &f);
    g += f;
    if (s->kind == RR_LIVE)
        s->kind = s->pitch == REP_SPIN || s->pitch == REP_STOP ? RR_RAMP : RR_STEP;
    int32_t st = s->kind == RR_STEP ? step16(s, k) : 0;
    uint32_t rate = s->kind == RR_STEP ? exp2_q16((st * 21845) >> 6) : (uint32_t)s->rr >> 12;   /* Q16 */
    if (s->kind == RR_RAMP && s->drate > 0)             /* SPIN UP: the rate at the slice's end */
        rate += ((uint32_t)(s->drate >> 12) + 1) * (len8 >> 8);
    if (rate > 65536) {                                 /* faster than 1: the read runs on past the slice's end (each
                                                         * hit stays on the grid) and must not overtake the write:
                                                         * r x L within what the line holds at the slice's end */
        uint32_t cap = s->w - s->s0;
        for (;;) {
            uint32_t L = len8 >> 8, need = L + ((((rate - 65536) >> 1) * L) >> 15), have = cap + L;
            if (have > FREEZE)
                have = FREEZE;
            if (need + 4 <= have || (g & 7) >= (g & REP_T ? 4 : 6))
                break;
            len8 >>= 1;
            g++;
        }
    }
    s->grid = (uint8_t)g;
    handover(s);
    s->rel = 0;
    s->len16 = len8 << 8;
    s->due += (int32_t)len8;
    if (!s->tone_on)
        s->lp[0] = s->lp[1] = LP_START;
    s->tone_on = s->dir != 0;
    if (s->kind == RR_STEP) {
        int32_t d = (st < 0 ? -st : st) * 171;           /* 32768 / 192 per 1/16 semitone */
        s->inc = rate;
        s->depth = (int16_t)(d > 32767 ? 32767 : d);
        s->gt = s->dir < 0 ? 32768 - (s->depth >> 1) : 32768;       /* -6 dB at the full drop */
        if (s->dir)
            s->bt = tone_coef(s->dir, s->depth);
        s->turn += g & REP_T ? 0x55555555u >> (g & 7) : 1u << (32 - (g & 7));
    } else
        s->inc = (uint32_t)s->rr >> 12;
}

/* Once per block while a gate plays: the ramp's depth, tone and gain targets, TAPE STOP's fade, the glides; the gain
 * moves linearly across the block (dgain per sample). */
static int32_t block_step(struct rep *s)
{
    s->prog8 += BLOCK << 8;
    if (s->drate) {
        int32_t d = s->rr - (1 << 28);
        d = (d < 0 ? -d : d) >> 13;
        s->depth = (int16_t)(d > 32767 ? 32767 : d);
        if (s->kind == RR_RAMP || s->kind == RR_TAPE) {
            s->bt = tone_coef(s->dir, s->depth);
            s->gt = s->dir < 0 && s->kind == RR_RAMP ? 32768 - (s->depth >> 1) : 32768;
        }
    }
    if (s->pitch == REP_STOP && !s->gstep) {            /* TAPE STOP fades out over the last 15 % of its span */
        int32_t left = s->hold ? s->gate8 - s->prog8 : s->grem;
        if (left < (s->gate8 >> 3) + (s->gate8 >> 5))
            s->gstep = left > 256 ? (int32_t)udiv((uint32_t)s->gfade, (uint32_t)left >> 8) + 1 : s->gfade >> 6;
    }
    if (s->kind == RR_TAPE && !s->rr && !s->gstep)       /* stopped: no held sample */
        s->gstep = s->gfade >> 6;
    s->gfade = s->gfade > s->gstep * BLOCK ? s->gfade - s->gstep * BLOCK : 0;
    s->g += (s->gt - s->g) >> 2;
    s->b += (s->bt - s->b) >> 1;
    int32_t g1 = (s->g * (s->gfade >> 8)) >> 15, dg = (g1 - s->gain) >> 6;
    s->gain = g1;
    return dg;
}

static inline int16_t sat16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* The steady state's sample loop (no crossfade or fade to dry running, everything it reads written since this FX owns
 * the lines: from the slice's start on), specialised by channels, the pitch's direction (the tone: none, a low-pass
 * down, a high-pass up) and the ramp, so the few values it needs stay in registers. Runs n samples; returns them. */
struct run {
    uint32_t w, s0q, rel, inc, tpos, tinc;
    int32_t txf;                            /* a crossfade running: main ramps in, the tail out */
    int32_t ramp, drate, rend, cb, lp0, lp1, gain, dg;
    int ramped;                             /* the ramp drives the read (RR_RAMP, RR_TAPE) */
    int wr;                                 /* the line records */
};

static inline __attribute__((always_inline)) int steady_t(struct run *v, int16_t *l, int16_t *r, int16_t *rl,
                                                          int16_t *rr, int n, int stereo, int dir, int ramped)
{
    uint32_t w = v->w, s0q = v->s0q, rel = v->rel, inc = v->inc;
    int32_t ramp = v->ramp, drate = v->drate, rend = v->rend, cb = v->cb, lp0 = v->lp0, lp1 = v->lp1;
    int32_t gain = v->gain, dg = v->dg, wr = v->wr, rdrive = v->ramped, txf = v->txf;
    uint32_t tpos = v->tpos, tinc = v->tinc;
    int k = 0;
    while (k < n) {
        int32_t xl = l[k], xr = stereo ? r[k] : 0, yl, yr = 0;
        if (wr) {
            uint32_t wi = w & MASK;
            rl[wi] = (int16_t)xl;
            if (stereo)
                rr[wi] = (int16_t)xr;
            w++;
        }
        uint32_t pos = s0q + rel, i1 = pos >> 16, j0 = i1 & MASK, j1 = (i1 + 1) & MASK;
        if (j0 == ((w - 1) & MASK))                     /* the newest sample: nothing after it yet */
            j1 = j0;
        int32_t fr = (int32_t)((pos >> 1) & 0x7FFF), a0 = rl[j0];
        yl = a0 + (((rl[j1] - a0) * fr + 16384) >> 15);
        if (stereo) {
            a0 = rr[j0];
            yr = a0 + (((rr[j1] - a0) * fr + 16384) >> 15);
        }
        if (txf) {                                      /* the tail (behind the write: it continues a read) */
            uint32_t t1 = tpos >> 16, k0 = t1 & MASK, k1 = (t1 + 1) & MASK;
            if (k0 == ((w - 1) & MASK))
                k1 = k0;
            int32_t tf = (int32_t)((tpos >> 1) & 0x7FFF), m = (XF + 1 - txf) * (32768 / XF), b0 = rl[k0];
            int32_t tl = b0 + (((rl[k1] - b0) * tf + 16384) >> 15);
            yl = tl + (((yl - tl) * m + 16384) >> 15);
            if (stereo) {
                b0 = rr[k0];
                int32_t tr = b0 + (((rr[k1] - b0) * tf + 16384) >> 15);
                yr = tr + (((yr - tr) * m + 16384) >> 15);
            }
            tpos += tinc;
            txf--;
        }
        rel += inc;
        if (ramped) {
            ramp += drate;
            if (drate > 0 ? ramp > rend : ramp < rend)
                ramp = rend;
            if (rdrive)
                inc = (uint32_t)ramp >> 12;
        }
        if (dir < 0) {                                  /* the low-pass (|y - lp| <= 65535: x 32768 fits) */
            lp0 += ((yl - ((lp0 + 128) >> 8)) * cb) >> 7;
            yl = (lp0 + 128) >> 8;
            if (stereo) {
                lp1 += ((yr - ((lp1 + 128) >> 8)) * cb) >> 7;
                yr = (lp1 + 128) >> 8;
            }
        } else if (dir > 0) {                           /* the high-pass */
            lp0 += ((yl - ((lp0 + 128) >> 8)) * cb) >> 8;
            yl -= (lp0 + 128) >> 8;
            if (stereo) {
                lp1 += ((yr - ((lp1 + 128) >> 8)) * cb) >> 8;
                yr -= (lp1 + 128) >> 8;
            }
        }
        gain += dg;
        if (gain < 32768) {
            yl = (yl * gain + 16384) >> 15;
            yr = (yr * gain + 16384) >> 15;
        }
        l[k] = sat16(yl);
        if (stereo)
            r[k] = sat16(yr);
        k++;
    }
    v->w = w, v->rel = rel, v->inc = inc, v->ramp = ramp, v->lp0 = lp0, v->lp1 = lp1, v->gain = gain;
    v->tpos = tpos, v->txf = txf;
    return k;
}

#define STEADY(name, st, d, rp) \
    static __attribute__((noinline)) int name(struct run *v, int16_t *l, int16_t *r, int16_t *rl, int16_t *rr, int n) \
    { return steady_t(v, l, r, rl, rr, n, st, d, rp); }
STEADY(steady_m_flat, 0, 0, 0)
STEADY(steady_m_flatr, 0, 0, 1)
STEADY(steady_m_down, 0, -1, 0)
STEADY(steady_m_up, 0, 1, 0)
STEADY(steady_m_downr, 0, -1, 1)
STEADY(steady_m_upr, 0, 1, 1)
STEADY(steady_s_flat, 1, 0, 0)
STEADY(steady_s_flatr, 1, 0, 1)
STEADY(steady_s_down, 1, -1, 0)
STEADY(steady_s_up, 1, 1, 0)
STEADY(steady_s_downr, 1, -1, 1)
STEADY(steady_s_upr, 1, 1, 1)

/* The block: the clock, the trigger, then the samples. stereo: l/r with lines rl/rr, else l with line rl. */
static inline __attribute__((always_inline)) void run(struct rep *s, int set, int16_t *l, int16_t *r, int16_t *rl,
                                                      int16_t *rr, const int16_t *p, int stereo)
{
    if (fx_line_owner[set] != FX_OWNER_REPEAT) {        /* the lines were pingpong's: start from nothing */
        fx_line_owner[set] = FX_OWNER_REPEAT;
        s->filled = 0;
        s->active = 0;
        s->hold = 0;
        s->txf = 0;
        s->efade = 0;
        s->ending = 0;
    }
    s->seed = (stereo ? 0xA511E9B3u : 0x63D83595u) ^ ((uint32_t)(te_play_slot & 7) * 0x9E3779B9u);
    int32_t spb = pp_tempo(te_tempo_spb, &s->last_spb);
    s->spb = spb;
    clock(s, spb);
    struct blk b;
    int pg = rep_pos(knob(p[0])), pf = rep_pos(knob(p[1])), pt = rep_pos(knob(p[2])), pp = rep_pos(knob(p[3]));
    b.len8 = slice8(rep_grid_at[pg], spb, &b.fold);
    b.blue = rep_grid_at[pg] + b.fold;
    b.fm = rep_fmode_at[pf];
    b.fa = rep_fa_at[pf];
    b.chance = rep_chance_at[pf];
    b.eff16 = rep_eff_gate(b.blue, rep_gate_at[pt]);
    b.g8 = (uint32_t)(b.eff16 >> 4) * s->bar8 + (((uint32_t)(b.eff16 & 15) * s->bar8) >> 4);
    b.pitch = rep_pitch_at[pp];
    s->blue = (uint8_t)b.blue;
    s->fold = (uint8_t)b.fold;
    if (s->since < 1000)
        s->since++;

    int i0 = -1, ih = -1;
    uint32_t d0 = 0, eh = 0;
    /* the manual key: green held on this context's repeat screen (repeat.h rep_key), the screen still drawn */
    int ctl = stereo ? RK_MASTER : rep_key.screen_ctl > RK_DRUM ? RK_SYNTH : rep_key.screen_ctl;
    int key = rep_key.held[ctl] && rep_key.screen_ctl == ctl && (stereo || ctl != RK_MASTER)
          && te_blocks - rep_key.screen_seen <= REP_KEY_STALE;
    s->key = (uint8_t)key;
    if (b.fm == RF_ALWAYS || key) {
        if (!s->active || !s->hold) {                   /* the latch: at the next 1/16 line (the key: blue's grid line),
                                                         * or now just after one */
            uint32_t six = key && b.fm != RF_ALWAYS ? b.len8 : s->bar8 >> 4, e;
            udivmod(s->ph8, six, &e);
            if (e <= REP_HOLD_LAG << 8 && !s->armed) {
                ih = 0;
                eh = e;
            } else if (six - e < BLOCK << 8) {
                ih = (int)((six - e + 255) >> 8);
                eh = ((uint32_t)ih << 8) - (six - e);
                if (ih >= BLOCK)
                    ih = -1;
            }
            s->armed = ih < 0;
        }
    } else {
        s->armed = 0;
        if (s->active && s->hold) {                     /* the latch let go: back to dry within one crossfade */
            s->hold = 0;
            s->ending = 1;
            s->grem = XF << 8;
        }
        /* the gate's start: the position shifted by the gate crosses a bar line (L: the bar the gate ends on).
         * CHANCE / VARY with a gate of a beat or less roll on every beat line instead (L: the beat), a quarter of the
         * chance each: the same fills per bar, scattered through it (sound review round 2) */
        int rolled = b.fm == RF_CHANCE || b.fm == RF_VARY, beats = rolled && b.eff16 <= 4;
        uint32_t per = beats ? s->bar8 >> 2 : s->bar8, ph = s->ph8, L;
        if (beats) {
            uint32_t j = ph >= per << 1 ? 2 : 0;
            ph -= j * per;
            if (ph >= per)
                ph -= per, j++;
            if (ph >= per)                              /* (bar8 >> 2 rounds down: a sliver of a 5th beat) */
                ph -= per, j++;
            L = (s->bar << 2) + j;
        } else
            L = s->bar + ((uint32_t)b.eff16 >> 4);
        uint32_t gr = ((uint32_t)(b.eff16 & 15) * s->bar8) >> 4;
        uint32_t q = ph + gr, line = 0;
        if (q >= per) {
            q -= per;
            L++;
        }
        if (q < CATCH8 && !(L == s->line && s->since < 4) && !s->jumped) {
            i0 = 0;                                     /* passed in the last 128 samples (varispeed, a locate) */
            line = L;
        } else {
            uint32_t rem = per - q;
            if (rem < (BLOCK - 1) << 8) {
                i0 = (int)((rem + 255) >> 8);
                d0 = ((uint32_t)i0 << 8) - rem;
                line = L + 1;
            }
        }
        if (i0 >= 0) {
            int win = 0;
            if (b.fm == RF_EVERY)
                win = !(line & (uint32_t)(b.fa - 1));
            else if (rolled)
                win = (mix32(line * 0x9E3779B9u + s->seed + (uint32_t)beats * 0x632BE5ABu) >> 16)
                      < (beats ? (b.chance + 2) >> 2 : b.chance);
            s->line = line;
            s->since = 0;
            if (!win) {
                s->lost += (uint32_t)rolled;
                i0 = -1;
            }
        }
    }

    if (!s->active && i0 < 0 && ih < 0) {              /* dry: the line records, the output is the input */
        uint32_t w = s->w;
        for (int i = 0; i < BLOCK; i++, w++) {
            rl[w & MASK] = l[i];
            if (stereo)
                rr[w & MASK] = r[i];
        }
        s->w = w;
        s->filled = s->filled + BLOCK > RING ? RING : s->filled + BLOCK;
        return;
    }
    /* the samples: the hot state in locals (the event functions work on *s: SAVE before, LOAD after) */
    uint32_t w = s->w, filled = s->filled, s0, s0q, rel, inc, tpos, tinc;
    int32_t txf, grem, due, efade, cb, lp0, lp1, ramp, drate, rend;
    int32_t gain = s->gain, dg = s->active ? block_step(s) : 0;
    int active, hold, kind, ending, tone_on, dir;
#define LOAD() (s0 = s->s0, s0q = s0 << 16, rel = s->rel, inc = s->inc, tpos = s->tpos, tinc = s->tinc, \
                txf = s->txf, grem = s->grem, due = s->due, efade = s->efade, cb = s->b, lp0 = s->lp[0], \
                lp1 = s->lp[1], ramp = s->rr, drate = s->drate, rend = s->rate_end, active = s->active, \
                hold = s->hold, kind = s->kind, ending = s->ending, tone_on = s->tone_on && s->dir, dir = s->dir)
#define SAVE() (s->w = w, s->rel = rel, s->inc = inc, s->tpos = tpos, s->tinc = tinc, s->txf = txf, s->grem = grem, \
                s->due = due, s->efade = efade, s->lp[0] = lp0, s->lp[1] = lp1, s->rr = ramp, \
                s->active = (uint8_t)active, s->ending = (uint8_t)ending)
    LOAD();
    for (int i = 0; i < BLOCK; i++) {
        /* the steady state (no crossfade, no fade to dry, the reads all written since the takeover: every read is at or
         * after s0): a run of samples up to the next event */
        if (active && !ending && !efade && (filled >= RING || filled > w - s0 + 2) && i != i0 && i != ih
            && (lp0 != LP_START || !tone_on)) {
            int n = BLOCK - i;
            if (i0 > i && i0 - i < n)
                n = i0 - i;
            if (ih > i && ih - i < n)
                n = ih - i;
            if (!hold && (grem - (XF << 8)) >> 8 < n)   /* the sample where the fade to dry starts */
                n = (grem - (XF << 8)) >> 8;
            if (kind != RR_TAPE && (due - 1) >> 8 < n)  /* the boundary's sample */
                n = (due - 1) >> 8;
            int wr = w - s0 < FREEZE;
            if (wr && (int)(FREEZE - (w - s0)) < n)
                n = (int)(FREEZE - (w - s0));
            if (n > 0) {
                struct run v = { w, s0q, rel, inc, tpos, tinc, txf, ramp, drate, rend, cb, lp0, lp1, gain, dg,
                                 kind == RR_RAMP || kind == RR_TAPE, wr };
                int d = tone_on ? dir : 0;              /* no tone yet: the live slice */
                int16_t *pl = l + i, *pr = stereo ? r + i : 0;
                int k;
                if (stereo)
                    k = drate ? (d < 0 ? steady_s_downr(&v, pl, pr, rl, rr, n) : d > 0 ? steady_s_upr(&v, pl, pr, rl, rr, n)
                                 : steady_s_flatr(&v, pl, pr, rl, rr, n))
                        : d < 0 ? steady_s_down(&v, pl, pr, rl, rr, n) : d > 0 ? steady_s_up(&v, pl, pr, rl, rr, n)
                        : steady_s_flat(&v, pl, pr, rl, rr, n);
                else
                    k = drate ? (d < 0 ? steady_m_downr(&v, pl, 0, rl, 0, n) : d > 0 ? steady_m_upr(&v, pl, 0, rl, 0, n)
                                 : steady_m_flatr(&v, pl, 0, rl, 0, n))
                        : d < 0 ? steady_m_down(&v, pl, 0, rl, 0, n) : d > 0 ? steady_m_up(&v, pl, 0, rl, 0, n)
                        : steady_m_flat(&v, pl, 0, rl, 0, n);
                w = v.w, rel = v.rel, inc = v.inc, ramp = v.ramp, lp0 = v.lp0, lp1 = v.lp1, gain = v.gain;
                tpos = v.tpos, txf = v.txf;
                if (wr)
                    filled = filled + (uint32_t)k > RING ? RING : filled + (uint32_t)k;
                if (!hold)
                    grem -= k << 8;
                due -= k << 8;
                i += k - 1;
                continue;
            }
        }
        if (i == i0 || i == ih) {
            SAVE();
            if (i == i0)
                start_gate(s, &b, d0);
            else
                start_hold(s, &b, eh);
            LOAD();
            gain = s->gain;                             /* a new plan starts at unity */
            dg = 0;
        }
        int32_t xl = l[i], xr = stereo ? r[i] : 0;
        if (!active || w - s0 < FREEZE) {     /* the line records (until it would reach the slice) */
            uint32_t wi = w & MASK;
            rl[wi] = (int16_t)xl;
            if (stereo)
                rr[wi] = (int16_t)xr;
            w++;
            filled += filled < RING;
        }
        if (!active)
            continue;
        if (!hold) {                                    /* the gate's end: a fade to dry over its last XF samples */
            grem -= 256;
            if (grem < XF << 8 && !ending)
                ending = 1;
            if (grem <= 0) {                            /* dry from the bar line on */
                active = 0;
                ending = 0;
                efade = 0;
                txf = 0;
                continue;
            }
        }
        if (kind != RR_TAPE) {
            due -= 256;
            if (due <= 0) {
                SAVE();
                boundary(s, &b);
                LOAD();
            }
        }
        if (drate) {                                    /* the ramps: TAPE STOP, SPIN UP (from the gate's start) */
            ramp += drate;
            if (drate > 0 ? ramp > rend : ramp < rend)
                ramp = rend;
            if (kind == RR_RAMP || kind == RR_TAPE)
                inc = (uint32_t)ramp >> 12;
        }
        gain += dg;
        /* main read at s0 + rel (and the tail during a crossfade), linearly interpolated; stale samples read 0 */
        uint32_t pos = s0q + rel, i1 = pos >> 16, age = (w - 1 - i1) & MASK;
        int32_t fr = (int32_t)((pos >> 1) & 0x7FFF), yl = 0, yr = 0;
        if (age < filled) {
            uint32_t j0 = i1 & MASK, j1 = age ? (i1 + 1) & MASK : j0;
            int32_t a0 = rl[j0];
            yl = a0 + (((rl[j1] - a0) * fr + 16384) >> 15);
            if (stereo) {
                a0 = rr[j0];
                yr = a0 + (((rr[j1] - a0) * fr + 16384) >> 15);
            }
        }
        rel += inc;
        if (txf > 0) {                                  /* main ramps in, the tail out */
            int32_t m = (XF + 1 - txf) * (32768 / XF), tl = 0, tr = 0;
            uint32_t t1 = tpos >> 16, tage = (w - 1 - t1) & MASK;
            if (tage < filled) {
                uint32_t j0 = t1 & MASK, j1 = tage ? (t1 + 1) & MASK : j0;
                int32_t tf = (int32_t)((tpos >> 1) & 0x7FFF), a0 = rl[j0];
                tl = a0 + (((rl[j1] - a0) * tf + 16384) >> 15);
                if (stereo) {
                    a0 = rr[j0];
                    tr = a0 + (((rr[j1] - a0) * tf + 16384) >> 15);
                }
            }
            yl = tl + (((yl - tl) * m + 16384) >> 15);
            if (stereo)
                yr = tr + (((yr - tr) * m + 16384) >> 15);
            tpos += tinc;
            txf--;
        }
        if (tone_on) {                                  /* the coupled tone: down a low-pass, up a high-pass */
            if (lp0 == LP_START) {                      /* it starts where the signal is */
                lp0 = dir < 0 ? yl << 8 : 0;
                lp1 = dir < 0 ? yr << 8 : 0;
            }
            if (dir < 0) {                              /* |y - lp| <= 65535: x 32768 fits */
                lp0 += ((yl - ((lp0 + 128) >> 8)) * cb) >> 7;
                yl = (lp0 + 128) >> 8;
                if (stereo) {
                    lp1 += ((yr - ((lp1 + 128) >> 8)) * cb) >> 7;
                    yr = (lp1 + 128) >> 8;
                }
            } else {
                lp0 += ((yl - ((lp0 + 128) >> 8)) * cb) >> 8;
                yl -= (lp0 + 128) >> 8;
                if (stereo) {
                    lp1 += ((yr - ((lp1 + 128) >> 8)) * cb) >> 8;
                    yr -= (lp1 + 128) >> 8;
                }
            }
        }
        if (gain < 32768) {
            yl = (yl * gain + 16384) >> 15;
            yr = (yr * gain + 16384) >> 15;
        }
        if (ending) {                                   /* the fade to dry */
            efade = efade + 1024 > 32768 ? 32768 : efade + 1024;
            if (ending == 2 && efade == 32768)          /* a new gate's live slice took over under the fade */
                ending = 0, efade = 0;
        }
        if (efade > 0) {                                /* (a high-pass can overshoot: in range first) */
            yl = sat16(yl);
            yr = sat16(yr);
            yl += ((xl - yl) * efade + 16384) >> 15;
            yr += ((xr - yr) * efade + 16384) >> 15;
        }
        l[i] = sat16(yl);
        if (stereo)
            r[i] = sat16(yr);
    }
    SAVE();
    s->filled = filled;
#undef LOAD
#undef SAVE
    if (s->active && s->drate)                          /* TAPE STOP, SPIN UP: the wheel turns with the read, */
        s->turn += (s->tps * ((uint32_t)s->rr >> 18)) >> 4;  /* from the gate's start */
}

void repeat_mono(void *self, int16_t *buf, const int16_t *p)
{
    (void)self;
    run(&repeat_state[0], 0, buf, 0, pingpong_ram + 2 * RING, 0, p, 0);
    fx_publish(&repeat_pub[0], p);
}

void repeat_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p)
{
    (void)self;
    run(&repeat_state[1], 1, l, r, pingpong_ram, pingpong_ram + RING, p, 1);
    fx_publish(&repeat_pub[1], p);
}
