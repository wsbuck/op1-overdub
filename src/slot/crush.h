/* crush FX state (src/slot/crush.c), shared with the FX screen. ui.c reads crush_state[0] (synth / drum FX) or
 * crush_state[1] (master FX); the fields above `v1` are the screen's interface (notes/fx-crush.md "Screen"). */
#ifndef CRUSH_H
#define CRUSH_H
#include <stdint.h>

struct crush_ch {
    int32_t xp;                         /* last input sample after the compressor (Q15) */
    int32_t qh;                         /* held, quantised value (Q15) */
    int32_t z;                          /* reconstruction low-pass state (Q15) */
    int32_t dq;                         /* DC blocker state (Q23) */
};

struct crush {
    /* ---- for the screen (updated once per 64-frame block, ~690 times a second) ---- */
    int32_t gr;                         /* compressor gain reduction, smoothed, 0 .. 32767 = 0 .. 30 dB */
    int16_t k[4];                       /* knobs after smoothing (0 .. 32767): rate, bits, squash, grit */
    int32_t rate_hz;                    /* sample-and-hold rate, Hz (44100 .. 400) */
    int32_t bits_q12;                   /* bit depth, Q12 (16.0 .. 1.0) */
    int32_t mu;                         /* mu-law share of the quantiser, Q15 (0 .. 32768) */
    uint32_t flips;                     /* bit flips so far (GRIT upper half), wraps */
    /* ---- DSP state ---- */
    int32_t v1[4], v2[4];               /* knob smoothing: two cascaded one-poles per block */
    int32_t t;                          /* time to the next hold, Q16 samples */
    int32_t jv, jstep, jcnt;            /* clock jitter: value (Q15), its ramp, samples to the next target */
    uint32_t rnd;                       /* xorshift32 */
    int32_t env;                        /* compressor peak envelope (Q26) */
    int32_t g;                          /* compressor gain at the end of the last block (Q15) */
    int32_t gout;                       /* output level compensation at the end of the last block (Q13) */
    int32_t bypass;                     /* 1: the last block was exactly clean (input passed through) */
    struct crush_ch ch[2];
};

extern struct crush crush_state[2];     /* [0] mono (synth/drum FX), [1] stereo (master FX) */

#endif
