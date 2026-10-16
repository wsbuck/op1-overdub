/* ensemble FX state, shared with the FX screen (ui.c reads the LFO phase to animate the sweep). */
#ifndef ENSEMBLE_H
#define ENSEMBLE_H
#include <stdint.h>

struct ens {
    uint32_t lfo, vib;                  /* phases (keep first: preview_host.c mirrors these two) */
    int32_t depth, vdepth;              /* slew-limited, Q16 samples */
    int32_t ens, mix;                   /* glide per block, Q15 */
    int32_t d[2][3];                    /* current tap delays (Q16 samples) per channel; 0 = not started */
    int32_t dual, centre;               /* character blend CE -> dual (Q15, glides); centre delay (Q16, slewed) */
};

extern struct ens ensemble_state[2];    /* [0] mono (synth/drum FX), [1] stereo (master FX) */

/* White = CHARACTER (ensemble.c): CE plateau 0 .. 8191, CE -> DUAL 8192 .. 12287, DUAL plateau 12288 .. 20479,
 * then the ensemble taps fade in 20480 .. 32767. */
#define ENS_DUAL_LO 8192
#define ENS_DUAL_HI 12288
#define ENS_ENS_LO 20480

/* CE -> DUAL blend for white k (0 .. 32768, Q15). */
static inline int32_t ens_dual(int32_t k)
{
    return k <= ENS_DUAL_LO ? 0 : k >= ENS_DUAL_HI ? 32768 : (k - ENS_DUAL_LO) << 3;
}

/* Ensemble amount (the two extra taps and the vibrato) for white k (0 .. 32767, Q15). */
static inline int32_t ens_amount(int32_t k)
{
    if (k <= ENS_ENS_LO)
        return 0;
    int32_t e = ((k - ENS_ENS_LO) * 21845) >> 13;   /* x 32768 / 12288 */
    return e > 32767 ? 32767 : e;
}

#endif
