/* pingpong FX state, shared with the FX screen (ui.c reads the write index as an audio clock, the glided delay to
 * time the ball and the division / tempo for the readout), and blue's time map (pp_target), shared by pingpong.c,
 * ui.c and the host preview. */
#ifndef PINGPONG_H
#define PINGPONG_H
#include <stdint.h>

struct pp {
    uint32_t w;                             /* next write index (masked on use) */
    uint32_t filled;                        /* samples written since boot, saturating at RING */
    int32_t d;                              /* current delay, Q15 samples; 0 = not started */
    int32_t g, tone, mix;                   /* glide per block, Q15 (tone: -32768 dark .. 32766 bright) */
    int32_t lp[2];                          /* tone low-pass state per line */
    int32_t dt;                             /* the delay blue and the tempo ask for, Q15 samples (d glides to it) */
    int32_t spb;                            /* the tempo in use: samples per beat, Q14 (0 = none seen yet) */
    int16_t div;                            /* the division played (PP_FREE: free time) */
    int16_t fold;                           /* halvings that made the division fit the line (slow tempo) */
};

extern struct pp pingpong_state[2];         /* [0] mono (synth/drum FX), [1] stereo (master FX) */

/* Who uses the lines (pingpong_ram): [0] the mono line, [1] the stereo pair. pingpong, repeat and space share them (one
 * mono FX and one master FX run at a time); the FX that finds another owner takes the set over and resets what it has
 * written (filled; space clears the set), so it never reads the other's samples (notes/fx-beatrepeat.md "Memory",
 * notes/fx-reverb.md §11). */
enum { FX_OWNER_NONE, FX_OWNER_PINGPONG, FX_OWNER_REPEAT, FX_OWNER_SPACE };
extern uint32_t fx_line_owner[2];

/* Blue (raw 0 .. 32767, 1024 per detent): FREE time below PP_SYNC0, 20 ms * 2^(6 k / 20480) (detents 0 .. 20, 3.33
 * detents per octave, flat at 1.28 s from 20480); from PP_SYNC0 one tempo division per detent (21 .. 32, each
 * centred on its detent), shortest first. */
#define PP_FREE (-1)
#define PP_SYNC0 20992                      /* detent 20.5 */
#define PP_DIVS 12
#define PP_D_MIN 882                        /* 20 ms */
#define PP_D_MAX 65280                      /* the longest delay the 65536-sample line holds (Q15 < 2^31), 1.48 s */
#define PP_SPB_DEF (22050 << 14)            /* 120 BPM: TE's samples per beat, Q14 (notes/fx-pingpong.md "Tempo") */
#define PP_SPB_LO (11980 << 14)             /* valid tempo words: 221 .. 39.9 BPM (TE asserts 40 .. 220) */
#define PP_SPB_HI (66300 << 14)

/* Division j in beats = a * 2^-p (/ 3 for triplets): {a, p, triplet}. Each is 3 places above its half, so a fold
 * (halving) plays division j - 3 (from 1/4 down; only those can be too long, at <= 40.5 BPM). */
static const int8_t pp_div[PP_DIVS][3] = {
    { 1, 3, 0 },                            /* 1/32   1/8 beat */
    { 1, 1, 1 },                            /* 1/16T  1/6 */
    { 1, 2, 0 },                            /* 1/16   1/4 */
    { 1, 0, 1 },                            /* 1/8T   1/3 */
    { 3, 3, 0 },                            /* 1/16.  3/8 */
    { 1, 1, 0 },                            /* 1/8    1/2 */
    { 2, 0, 1 },                            /* 1/4T   2/3 */
    { 3, 2, 0 },                            /* 1/8.   3/4 */
    { 1, 0, 0 },                            /* 1/4    1 */
    { 2, -1, 1 },                           /* 1/2T   4/3 */
    { 3, 1, 0 },                            /* 1/4.   3/2 */
    { 2, 0, 0 },                            /* 1/2    2 */
};

static const uint32_t pp_exp2_frac[17] = {  /* (2^(i/16) - 1) in Q16 */
    0, 2902, 5932, 9096, 12400, 15850, 19454, 23216, 27146, 31249, 35534, 40009, 44682, 49562, 54658, 59979, 65536,
};

/* floor(x / 3) for any uint32, without a divide: x = hi 2^16 + lo, hi = 3 q + r, 2^16 = 3 * 21845 + 1, and
 * (n * 43691) >> 17 = n / 3 exactly for n < 98304. */
static inline uint32_t pp_div3(uint32_t x)
{
    uint32_t hi = x >> 16, q = (hi * 43691u) >> 17, r = hi - 3 * q;
    return (q << 16) + r * 21845u + (((r + (x & 0xFFFF)) * 43691u) >> 17);
}

/* A valid tempo word, else the last valid one (*last; 0 = none yet: 120 BPM). */
static inline int32_t pp_tempo(int32_t spb, int32_t *last)
{
    if (spb >= PP_SPB_LO && spb <= PP_SPB_HI)
        *last = spb;
    return *last ? *last : PP_SPB_DEF;
}

/* The delay for blue = k at tempo spb (Q14 samples per beat, valid), Q15 samples; *div = the division played
 * (PP_FREE in the free zone), *fold = how many halvings made it fit (<= PP_D_MAX). */
static inline int32_t pp_target(int32_t k, int32_t spb, int *div, int *fold)
{
    *fold = 0;
    if (k < PP_SYNC0) {
        int32_t e = k < 0 ? 0 : (k * 39322) >> 12;  /* Q15 octaves, 9.6 per raw: 6 at 20480 */
        if (e > 6 << 15)
            e = 6 << 15;
        int32_t oct = e >> 15, f = e & 32767, i = f >> 11, t = f & 2047;   /* 16 table steps per octave */
        uint32_t m = pp_exp2_frac[i] + (((pp_exp2_frac[i + 1] - pp_exp2_frac[i]) * t) >> 11);
        *div = PP_FREE;
        if (oct == 6)
            return PP_D_MIN * 64 << 15;
        return (int32_t)(((PP_D_MIN * (65536u + m)) >> 1) << oct);
    }
    int j = (k - PP_SYNC0) >> 10;
    if (j >= PP_DIVS)
        j = PP_DIVS - 1;
    const int8_t *v = pp_div[j];
    uint32_t u = (uint32_t)spb * (uint32_t)v[0];    /* Q14 samples x 2^p; < 2^32: spb < 2^30.1, a <= 3 */
    if (v[2])
        u = pp_div3(u);
    u = v[1] >= 0 ? u >> v[1] : u << 1;             /* p = -1: u <= 2/3 spb, doubled < 2^31 */
    while (u > (uint32_t)PP_D_MAX << 14) {          /* too long for the line: a half, still on the beat grid */
        u >>= 1;
        ++*fold;
    }
    *div = j - 3 * *fold;
    return (int32_t)(u << 1);
}

#endif
