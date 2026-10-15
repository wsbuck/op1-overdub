/* Guitar amp engine (amp.c) and its screen (src/slot/ui.c): the shared RAM record, knob laws and the drive curve.
 * Design: notes/amp.md. Input research: notes/input.md.
 *
 * amp_ram is a FREE SDRAM buffer (engines.json "ram"; the image zero-fills its first 256 bytes, so `magic` is 0 at
 * boot and the engine initialises the rest). The engine writes it from the audio callback; the screen only reads it.
 */
#ifndef AMP_H
#define AMP_H
#include <stdint.h>

#define AMP_MAGIC 0x414D5031u              /* "AMP1"; any other value: (re)initialise */
#define AMP_IN 64                          /* TE IN samples per block (44.1 kHz) */
#define AMP_OUT 128                        /* engine samples per call (88.2 kHz) */
#define AMP_VOICES 8                       /* gain table entries (TE has 6 synth voices) */
#define AMP_FADE 8                         /* ownership crossfade, blocks (11.6 ms) */
#define AMP_HIST 24                        /* upsampler history (2 x AMP_HB_K) */

/* One SVF: state and the block's coefficients (amp.c). */
struct amp_svf {
    int32_t ic1, ic2;                      /* state, Q24 */
    int32_t a1, a2, a3;                    /* Q31 */
    int32_t m1, m2, m0;                    /* output mix, Q29 (in the order svf_block loads them) */
};

enum { F_LOW, F_HIGH, F_HP, F_MID, F_PRES, F_LP1, F_LP2, AMP_FILTERS };

struct amp_ram {
    uint32_t magic;
    /* blocks: a new TE callback is seen as a change of TE IN, of te_rx_sel, or a voice rendering again over digital
     * silence (amp.c new_block) */
    uint32_t blk;                          /* blocks seen (any voice rendered) */
    uint32_t sel;                          /* te_rx_sel at the last block */
    uint32_t y_blk;                        /* block whose output y[] holds */
    /* ownership: one voice plays the input, the others are silent; gains crossfade (amp.c) */
    uint32_t owner;                        /* the owner's state address (0: none) */
    uint32_t owner_blk;                    /* the last block the owner rendered */
    uint32_t cur_owner;                    /* the owner at the start of this block (the gains follow it) */
    uint32_t takeovers;                    /* ownership changes (screen, checks) */
    uint32_t renders;                      /* render calls */
    struct { uint32_t id, blk; int32_t gain; } v[AMP_VOICES];   /* per voice: state address, last block, gain Q15 */
    /* for the screen (per block) */
    int32_t in_peak;                       /* |TE IN| peak, Q15 */
    int32_t gate_open;                     /* 1 open, 0 closed (the last sample of the block) */
    int32_t gate_gain;                     /* Q30 */
    int32_t out_peak;                      /* output peak before the voice gains, Q15 */
    int32_t lim_gain;                      /* limiter gain, Q15 */
    uint32_t dsp_blocks;                   /* blocks processed */
    /* DSP state (amp.c) */
    int32_t knob[4];                       /* slewed knobs */
    int32_t env;                           /* gate detector, Q15 */
    int32_t hold;                          /* gate hold, samples */
    int32_t tight, emph, inter;            /* one-pole states (Q24) */
    int32_t gain_prev, comp_prev;          /* last block's drive gain and make-up (ramped across the block; amp.c) */
    struct amp_svf f[AMP_FILTERS];
    int16_t buf[AMP_HIST + AMP_IN];        /* upsampler history (the last processed block's end), then this block's
                                            * TE IN (compared to detect a new block) */
    int16_t bs[AMP_HIST + AMP_IN];         /* buf shifted by one sample (bs[i + 1] = buf[i]): the upsampler's word
                                            * loads are aligned in one of the two */
    int32_t gd[AMP_IN];                    /* per input sample: gate gain x drive gain (Q23) */
    int32_t pc[3];                         /* drive pass coefficients: tight a, pre-emphasis a, e (1 - a) (Q31) */
    int32_t in1[AMP_OUT + 1];              /* stage-1 input (Q20, biased): [0] the last block's last, then the block */
    int32_t in2[AMP_OUT + 1];              /* stage-2 input (Q20), the same layout */
    int32_t *list[AMP_OUT + 1];            /* ADAA: the pairs straddling the knee (amp.c adaa) */
    int32_t z[AMP_OUT];                    /* DSP scratch (Q24) */
    int16_t y[AMP_OUT];                    /* this block's output (before the voice gains) */
};
extern struct amp_ram amp_ram;

/* ---- knob laws (shared with the screen) ---- */

static inline int32_t amp_knob(int16_t k)
{
    return k < 0 ? 0 : k;
}

/* Gate threshold in dBFS x 256 for white 1..32767 (0 = off): -84 .. -30 dB, linear (gen_amp_tables.py GATE_DB). */
static inline int32_t amp_gate_db_q8(int32_t white)
{
    return -84 * 256 + ((white * 54 * 256) >> 15);
}

/* ---- the drive curve (pure C; the engine's per-sample code in amp.c does the same in line) ---- */

#define AMP_U1 (1 << 20)                   /* 1.0 in Q20 */
#define AMP_KNEE (3 << 19)                 /* the tube's knee, 1.5 (Q20) */

static inline int32_t amp_lim(int32_t x, int32_t lo, int32_t hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

/* Stage 1, tube: s(v) = v - 4 v^3 / 27 on |v| <= 1.5, +-1 beyond (Q20 in and out; distortion.c's tube_c). */
static inline int32_t amp_tube(int32_t v)
{
    int32_t c = amp_lim(v, -AMP_KNEE, AMP_KNEE), a = c >> 6;    /* Q14, |a| <= 24576 */
    int32_t a3 = ((a * a) >> 14) * a;                           /* Q28, < 2^30 */
    return c - (((a3 >> 13) * 4855) >> 10);                     /* 4855 = 4/27 Q15; the linear term exact */
}

/* Stage 2: s2(a) = 1.5 (a - a^3 / 3) on a = clamp(x, -1, 1) is the tube at 1.5 x: s2(x) = amp_tube(1.5 x), so
 * amp.c runs both stages through one kernel (the stage-2 gain carries the 1.5). */

#endif
