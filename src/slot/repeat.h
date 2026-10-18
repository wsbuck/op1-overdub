/* repeat FX (src/slot/repeat.c; notes/fx-beatrepeat.md): the knob maps, shared by the DSP, the screen (ui.c,
 * ui_repeat.h) and the host checks, and the state the DSP publishes for the screen (repeat_state[0] mono, [1] the
 * master). The maps follow design/fx/gen_wheel.py (grid_of, fortune_of, gate_of, eff_gate, pitch_of, vary_pick). */
#ifndef REPEAT_H
#define REPEAT_H
#include <stdint.h>

#define REP_RING 65536                      /* samples per line (pingpong's lines, shared through fx_line_owner) */
#define REP_LMAX 65280                      /* the longest slice; a longer grid plays its half (the fold) */
#define REP_XF 32                           /* crossfade at every slice boundary and gate edge, samples */
#define REP_HOLD_LAG 1323                   /* ALWAYS starts at once within 30 ms after a 1/16 line, else at the next */

/* One encoder detent = 1024 raw, 33 positions (0 .. 32); raw 0 .. 32767 (TE's range row). */
static inline int rep_pos(int32_t raw)
{
    int32_t p = (raw + 512) >> 10;
    return p < 0 ? 0 : p > 32 ? 32 : (int)p;
}

/* blue GRID: a grid is a byte, bit 7 triplet, bits 0..2 = sh: slices per bar n = (triplet ? 3 : 1) << sh.
 * Straight divisions are 4 positions wide and triplets 2 (No Triplets by landing). */
#define REP_T 0x80
static const uint8_t rep_grid_at[33] = {
    6, 6, 6,                                /* 1/64 */
    REP_T | 4, REP_T | 4,                   /* 1/32T */
    5, 5, 5, 5,                             /* 1/32 */
    REP_T | 3, REP_T | 3,                   /* 1/16T */
    4, 4, 4, 4,                             /* 1/16 */
    REP_T | 2, REP_T | 2,                   /* 1/8T */
    3, 3, 3, 3,                             /* 1/8 */
    REP_T | 1, REP_T | 1,                   /* 1/4T */
    2, 2, 2, 2,                             /* 1/4 */
    REP_T | 0, REP_T | 0,                   /* 1/2T */
    1, 1, 1, 1,                             /* 1/2 */
};

static inline int rep_slices(int g)         /* slices per bar */
{
    return (g & REP_T ? 3 : 1) << (g & 7);
}

/* green FORTUNE: 0 ALWAYS (the latch), 1-2 OFF, 3-8 EVERY 8 / 4 / 2 BARS, 9-17 CHANCE 10 .. 90 %, 18-20 EVERY BAR,
 * 21-32 VARY a = 1 .. 4 (3 positions each) at 50 / 67 / 83 / 100 %. */
enum { RF_ALWAYS, RF_OFF, RF_EVERY, RF_CHANCE, RF_VARY };
static const uint8_t rep_fmode_at[33] = { RF_ALWAYS, RF_OFF, RF_OFF, RF_EVERY, RF_EVERY, RF_EVERY, RF_EVERY, RF_EVERY,
    RF_EVERY, RF_CHANCE, RF_CHANCE, RF_CHANCE, RF_CHANCE, RF_CHANCE, RF_CHANCE, RF_CHANCE, RF_CHANCE, RF_CHANCE,
    RF_EVERY, RF_EVERY, RF_EVERY, RF_VARY, RF_VARY, RF_VARY, RF_VARY, RF_VARY, RF_VARY, RF_VARY, RF_VARY, RF_VARY,
    RF_VARY, RF_VARY, RF_VARY };
/* EVERY: n bars; VARY: the amount a */
static const uint8_t rep_fa_at[33] = { 0, 0, 0, 8, 8, 4, 4, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4 };
/* the chance per bar, / 65536 (65535 = always) */
static const uint16_t rep_chance_at[33] = { 65535, 0, 0, 65535, 65535, 65535, 65535, 65535, 65535,
    6554, 13107, 19661, 26214, 32768, 39322, 45875, 52429, 58982, 65535, 65535, 65535,
    32768, 32768, 32768, 43909, 43909, 43909, 54395, 54395, 54395, 65535, 65535, 65535 };

/* white GATE, in sixteenths, ending on the bar line: 1 2 3 4 6 8 12 16 32 64 (3 positions each, 4 BARS from 27). */
static const uint8_t rep_gate_at[33] = { 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 6, 6, 6, 8, 8, 8, 12, 12, 12,
    16, 16, 16, 32, 32, 32, 64, 64, 64, 64, 64, 64 };

/* The grid wins: the gate holds at least two slices (the live capture and one repeat), at most 4 bars. */
static inline int rep_eff_gate(int g, int gate16)
{
    static const uint8_t two_straight[7] = { 32, 16, 8, 4, 2, 1, 1 }, two_trip[5] = { 11, 6, 3, 2, 1 };
    int two = g & REP_T ? two_trip[(g & 7) > 4 ? 4 : g & 7] : two_straight[g & 7];
    int e = gate16 > two ? gate16 : two;
    return e > 64 ? 64 : e;
}

/* orange PITCH, bipolar: semitones reached at the gate's last repeat; REP_SPIN / REP_STOP are the two ends. */
#define REP_SPIN 100
#define REP_STOP (-100)
static const int8_t rep_pitch_at[33] = {
    REP_SPIN, REP_SPIN, REP_SPIN, 12, 12, 7, 7, 5, 5, 3, 3, 2, 2, 1, 1, 0, 0, 0,
    -1, -1, -2, -2, -3, -3, -5, -5, -7, -7, -12, -12, REP_STOP, REP_STOP, REP_STOP,
};

/* VARY: one repeat's grid within +-a places of blue's in blue's own family (straight / triplet), never longer than
 * half the gate. r: a random 32-bit word. */
static inline int rep_vary_pick(int g, int a, int eff16, uint32_t r)
{
    int trip = g & REP_T, top = trip ? 4 : 5, sh = g & 7;
    int i = (trip ? 4 : 6) - sh;            /* index in the family, longer = higher: straight 64..2, triplet 48..3 */
    uint32_t span = (uint32_t)(2 * a + 1);
    int d = (int)(((r >> 16) * span) >> 16) - a;
    i += d;
    i = i < 0 ? 0 : i > top ? top : i;
    for (;;) {
        int n = (trip ? 3 : 1) << ((trip ? 4 : 6) - i);
        if (n * eff16 >= 32 || i == 0)
            break;
        i--;
    }
    return (trip ? REP_T : 0) | ((trip ? 4 : 6) - i);
}

/* The repeat state. The fields up to `turn` are the screen's (updated once per 64-frame block); ui_repeat.h draws from
 * them while the FX runs (repeat_pub moves). */
enum { RL_NONE, RL_TAPE, RL_METRO };       /* what the bar clock follows */
enum { RR_LIVE, RR_STEP, RR_RAMP, RR_TAPE };   /* how the repeats read: the live first slice, a staircase step, a
                                                * continuous ramp (SPIN UP, the looped TAPE STOP), the real TAPE STOP */
struct rep {
    /* ---- the bar clock ---- */
    uint32_t vpos;                          /* our position at the latest block start, Q8 samples (the tape's while it
                                             * rolls, else our own count locked to the metronome's clicks) */
    uint32_t bar8;                          /* bar length, Q8 samples (4 beats) */
    uint32_t ph8;                           /* vpos within the bar, Q8 */
    uint32_t bar;                           /* bar index of vpos (bars of 4 beats from tape position 0) */
    int32_t spb;                            /* the tempo in use (pingpong.h pp_tempo), Q14 samples per beat */
    uint8_t lock;                           /* RL_*: tape playhead or metronome */
    uint8_t clocked;                        /* the clock has had the tape or a click (else the next click is beat 1) */
    uint8_t jumped;                         /* this block's clock jumped (the first click): no late trigger */
    uint8_t metro_run;                      /* stopped: the metronome's countdown runs (else our count runs free) */
    uint8_t last_ok;                        /* last_c was in range */
    int32_t last_c;                         /* the countdown at the last block */
    /* ---- the repeats ---- */
    uint8_t active;                         /* a gate (or the latch) is playing */
    uint8_t hold;                           /* it is the latch (ALWAYS, or the manual key): no gate */
    uint8_t key;                            /* the manual key holds it (the screen's lit hub) */
    uint8_t kind;                           /* RR_* of the current slice */
    uint8_t grid;                           /* the grid played (VARY's pick; after the fold) */
    uint8_t blue;                           /* blue's grid (after the fold) */
    uint8_t fold;                           /* halvings that made blue's grid fit the line (slow tempos) */
    int8_t dir;                             /* the pitch's direction: -1 down, 0 flat, +1 up */
    int16_t rep;                            /* repeat index in the gate: 0 = the live first slice */
    int16_t depth;                          /* the current repeat's |pitch| / 12 (TAPE STOP / SPIN UP: progress), Q15 */
    uint32_t fires;                         /* won rolls (gates started or extended), wraps */
    uint32_t lost;                          /* lost rolls (CHANCE / VARY), wraps */
    uint32_t turn;                          /* the wheel: one turn = 2^32; steps 1 / slices-per-bar per repeat */
    /* ---- DSP ---- */
    uint32_t w, filled;                     /* next write index; samples written since this FX owns the lines */
    uint32_t s0;                            /* the slice's first sample (ring index, unmasked) */
    uint32_t rel;                           /* main read: offset from s0, Q16 */
    uint32_t inc;                           /* main read rate, Q16 */
    uint32_t len16;                         /* the slice played, Q16 samples */
    uint32_t tpos, tinc;                    /* tail read (the old position during a crossfade), Q16 */
    int32_t txf;                            /* crossfade samples left (main ramps in, tail out) */
    uint8_t tone_on;                        /* the coupled tone runs (from the first repeat) */
    uint8_t ending;                         /* the fade to dry is running (2: then the new gate's live slice goes on) */
    int32_t due;                            /* Q8 samples to the next slice boundary */
    int32_t grem;                           /* Q8 samples left in the gate */
    int32_t gate8;                          /* the gate's length, Q8 (the ramps' span) */
    int32_t nrep;                           /* repeats until the pitch total is reached (N - 1) */
    int32_t pstep;                          /* the staircase: 1/16 semitones x 65536 per repeat */
    int32_t pitch;                          /* the setting: semitones, REP_SPIN, REP_STOP */
    int32_t drate;                          /* the ramp: rate change per sample, Q28 */
    int32_t rate_end;                       /* the ramp's end rate, Q28 */
    int32_t rr;                             /* the ramp's rate now (it runs from the gate's start), Q28 */
    int32_t prog8;                          /* Q8 samples since the gate (or the latch) started, per block */
    int32_t gfade;                          /* TAPE STOP's fade-out, Q23 */
    int32_t gain;                           /* the repeat's gain at the end of the block, Q15 (g x gfade) */
    int32_t efade;                          /* gate-end fade: dry share Q15 */
    int32_t g, gt;                          /* repeat gain and its target, Q15 (-6 dB at the full drop) */
    int32_t gstep;                          /* TAPE STOP's fade-out per sample, Q23 (0: none yet) */
    int32_t b, bt;                          /* the coupled one-pole's coefficient and target (LP Q15 / HP Q16) */
    int32_t lp[2];                          /* its state per channel, Q8 */
    uint32_t tps;                           /* wheel turn per sample at rate 1 (2^32 / bar) */
    uint32_t line;                          /* the bar line of the last trigger handled */
    int32_t since;                          /* blocks since that trigger (no double fire) */
    uint32_t seed;                          /* per instance (and synth sound) */
    uint32_t armed;                         /* the latch missed its 30 ms: it waits for the next 1/16 line */
    int32_t last_spb;                       /* pp_tempo's memory */
    uint32_t vnext;                         /* the stopped clock's prediction for the next block */
};

extern struct rep repeat_state[2];          /* [0] mono (synth / drum FX), [1] stereo (master FX) */

/* The manual key (REPEAT2): the green encoder's push. TE's KeyHandler (FUN_019b6c74) sends a press without SHIFT to the
 * current controller's +0x14 slot and every release to +0x24; encoder pushes would be key events 0x3B..0x3E (GPIO bits
 * 8..11; whether the hardware has push switches is unproven), which TE's synth, drum and mixer controllers leave
 * unhandled (they return 0). Our wrappers on those slots (synth: CHORD v2's c2_press / c2_release; drum and mixer:
 * repeat.c rk_*) call rep_key_event first: it counts every encoder key, and takes green's press only while the repeat
 * screen of that controller's context was drawn within the last REP_KEY_STALE blocks; everything else goes on to TE's
 * function with the same arguments. The DSP holds the latch while the key is held and the screen still shows. */
enum { RK_SYNTH, RK_DRUM, RK_MASTER };
#define REP_KEY_GREEN 0x3C                  /* blue 0x3B, green 0x3C, white 0x3D, orange 0x3E (Hypothesis: GPIO order) */
#define REP_KEY_STALE 100                   /* blocks (~145 ms, ~4 frames) without the screen: the key lets go */
struct rep_key {
    uint32_t presses, releases;             /* encoder key events 0x3B..0x3E seen (any context) */
    uint32_t taken;                         /* green presses taken as the manual key */
    uint32_t screen_seen;                   /* TE's block counter when the repeat screen was last drawn */
    uint8_t last, last_ctl;                 /* the last encoder key and its controller (RK_*) */
    uint8_t screen_ctl;                     /* the controller of the context the repeat screen showed */
    uint8_t held[3];                        /* green held, per controller */
};
extern volatile struct rep_key rep_key;
extern volatile uint32_t te_blocks;         /* 0x01AADAF4: TE's audio block counter (+1 per callback) */
int rep_key_event(int ctl, int key, int down);

#endif
