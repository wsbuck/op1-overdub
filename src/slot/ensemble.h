/* ensemble FX state, shared with the FX screen (ui.c reads the LFO phase to animate the sweep). */
#ifndef ENSEMBLE_H
#define ENSEMBLE_H
#include <stdint.h>

struct ens {
    uint32_t lfo, vib;                  /* phases */
    int32_t depth, vdepth;              /* slew-limited, Q16 samples */
    int32_t ens, mix;                   /* glide per block, Q15 */
    int32_t d[2][3];                    /* current tap delays (Q16 samples) per channel; 0 = not started */
};

extern struct ens ensemble_state[2];    /* [0] mono (synth/drum FX), [1] stereo (master FX) */

#endif
