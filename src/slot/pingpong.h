/* pingpong FX state, shared with the FX screen (ui.c reads the write index as an audio clock and the glided delay
 * to time the ball). */
#ifndef PINGPONG_H
#define PINGPONG_H
#include <stdint.h>

struct pp {
    uint32_t w;                             /* next write index (masked on use) */
    uint32_t filled;                        /* samples written since boot, saturating at RING */
    int32_t d;                              /* current delay, Q15 samples; 0 = not started */
    int32_t g, tone, mix;                   /* glide per block, Q15 (tone: -32768 dark .. 32766 bright) */
    int32_t lp[2];                          /* tone low-pass state per line */
};

extern struct pp pingpong_state[2];         /* [0] mono (synth/drum FX), [1] stereo (master FX) */

#endif
