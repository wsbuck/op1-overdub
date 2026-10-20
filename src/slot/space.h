/* The space (reverb) FX: knob maps shared by the DSP (space.c) and its screen (ui.c), and what the DSP publishes for
 * the screen. Design: notes/fx-reverb.md (Option A); as built: notes/fx-reverb.md §11.
 *   blue   SPACE  ROOM (0) -> HALL (8192) -> CHURCH (16384) -> CATHEDRAL (24576) -> VOID (32767), one morph
 *   green  DECAY  x1/4 .. x4 of the space's own RT60 over detents 0 .. 29, noon (16384) = the space's own; detent 30
 *                 INFINITE (input open, the tail holds and layers), 31 FREEZE (input muted, the tail holds)
 *   white  TAIL   zones: TONE 0 .. 9 (dark .. natural at 5 .. bright), SHIMMER +12 10 .. 14, SHIMMER +12+19 15 .. 18,
 *                 SUB -12 19 .. 22 (amount 40 .. 100 % across each, faded in over the zone's first detent); GATE
 *                 23 .. 27 (the wet held while the input plays, then closed after a hold of 50 .. 350 ms); SWELL
 *                 28 .. 31 (the wet fades in over 0.15 .. 1.5 s after each onset). Natural tone outside TONE
 *   orange MIX    0 .. 16384: dry at unity, wet 0 -> 1 (0 = dry, bit exact). Above the middle, on the master: the
 *                 wet ducks under the playing, -12 dB one detent above the middle -> -24 dB at the top; on synth /
 *                 drum: the dry fades out (all wet at the top)
 */
#ifndef SPACE_H
#define SPACE_H
#include <stdint.h>

/* What the DSP publishes per instance ([0] mono: synth / drum FX, [1] stereo: master FX), once per block. */
struct space_ui {
    uint32_t blocks;                    /* process calls (the screen trusts the fields only while this moves) */
    int32_t t60_ms;                     /* the decay played (mid band), ms */
    int32_t level;                      /* tank level, Q15 (the meter) */
    int32_t duck;                       /* the wet's duck gain now, Q15 (32768 = none) */
    int32_t frz, inf;                   /* FREEZE / INFINITE crossfades, Q15 (32768 = held) */
    uint8_t zone;                       /* the TAIL zone played (SP_Z_*) */
    uint8_t key;                        /* the green push held: momentary freeze */
    uint8_t clearing;                   /* starting clean: the lines are being cleared (wet muted) */
    uint8_t half;                       /* the tank runs at 22.05 kHz */
    uint32_t cyc_max, cyc_mean;         /* CYCLES per process call over the last 256 calls */
    uint32_t cyc_peak;                  /* the largest since the FX last started clean */
    uint32_t cyc_fdn;                   /* the FDN's share (mean over the same 256 calls) */
};
extern struct space_ui space_ui[2];

/* ---- blue SPACE ---- */
static const char *const sp_space_names[5] = { "ROOM", "HALL", "CHURCH", "CATHEDRAL", "VOID" };
static inline int sp_space_nearest(int32_t k)  /* the anchor nearest to blue k */
{
    return k < 0 ? 0 : (k + 4096) >> 13 > 4 ? 4 : (k + 4096) >> 13;
}
/* log2 of the space's own RT60 (s), Q16, per detent (gen_space.py sp_pt SP_L2T60, kept here for the screen) */
static const int32_t sp_l2t60_at[33] = {
    -33723, -20189, -6655, 6878, 20412, 33946, 47480, 61014, 74547, 81007, 87466, 93926, 100385, 106844, 113304,
    119763, 126222, 133442, 140662, 147882, 155103, 162323, 169543, 176763, 183983, 195146, 206310, 217473, 228637,
    239800, 250964, 262128, 273280,
};
/* Detent position of blue k: index d (0 .. 31) and fraction f (Q10) towards d + 1 (the last detent spans 1023). */
static inline void sp_detent(int32_t k, int *d, int32_t *f)
{
    k = k < 0 ? 0 : k > 32767 ? 32767 : k;
    *d = k >> 10;
    *f = *d == 31 ? ((k & 1023) * 1025) >> 10 : k & 1023;
}
static inline int32_t sp_l2t60(int32_t k)
{
    int d;
    int32_t f;
    sp_detent(k, &d, &f);
    return sp_l2t60_at[d] + (((sp_l2t60_at[d + 1] - sp_l2t60_at[d]) * f) >> 10);
}

/* ---- green DECAY ---- */
#define SP_INFINITE 30720                   /* detent 30 */
#define SP_FREEZE 31744                     /* detent 31 */
enum { SP_HOLD_NONE, SP_HOLD_INFINITE, SP_HOLD_FREEZE };
static inline int sp_hold(int32_t k)
{
    return k >= SP_FREEZE ? SP_HOLD_FREEZE : k >= SP_INFINITE ? SP_HOLD_INFINITE : SP_HOLD_NONE;
}
/* log2 of DECAY's multiplier, Q16: -2 (x1/4) at 0, 0 at noon (16384), +2 (x4) from 30719 (and in the holds) */
static inline int32_t sp_decay_l2(int32_t k)
{
    if (k <= 0)
        return -(2 << 16);
    if (k <= 16384)
        return k * 8 - (2 << 16);
    if (k >= 30719)
        return 2 << 16;
    return ((k - 16384) * 37452) >> 12;     /* x 2 / 14335 in Q16 */
}

/* ---- white TAIL ---- */
enum { SP_Z_TONE, SP_Z_SH12, SP_Z_SH19, SP_Z_SUB, SP_Z_GATE, SP_Z_SWELL, SP_ZONES };
static const uint8_t sp_zone_lo[SP_ZONES] = { 0, 10, 15, 19, 23, 28 };      /* first detent of each zone */
static const uint16_t sp_zone_mul[SP_ZONES] = { 13107, 26214, 32768, 32768, 26214, 32768 };   /* 32768 / span, Q12 */
static const char *const sp_zone_names[SP_ZONES] = { "TONE", "SHIMMER +12", "SHIMMER +12+19", "SUB -12", "GATE",
                                                     "SWELL" };
static inline int sp_zone(int32_t k)
{
    int d = k < 0 ? 0 : k >> 10 > 31 ? 31 : k >> 10;
    int z = 0;
    while (z + 1 < SP_ZONES && d >= sp_zone_lo[z + 1])
        z++;
    return z;
}
/* the amount inside zone z for white k, Q15 0 .. 32767 (k clamped into the zone) */
static inline int32_t sp_zone_amount(int32_t k, int z)
{
    int32_t lo = sp_zone_lo[z] << 10, hi = z + 1 < SP_ZONES ? (sp_zone_lo[z + 1] << 10) - 1 : 32767;
    k = k < lo ? lo : k > hi ? hi : k;
    int32_t a = ((k - lo) * sp_zone_mul[z]) >> 12;
    return a > 32767 ? 32767 : a;
}
/* TONE's position for white k in the TONE zone: -32768 (dark) .. 0 (natural, detent 5) .. +32767 (bright), Q15 */
static inline int32_t sp_tone(int32_t k)
{
    k = k < 0 ? 0 : k > 10239 ? 10239 : k;
    return ((k - 5120) * 13107) >> 11;          /* x 32768 / 5120 */
}
/* the shimmer amount 40 .. 100 % across a shimmer zone, Q15 */
static inline int32_t sp_shimmer_amount(int32_t a)
{
    return 13107 + ((a * 19661) >> 15);
}
/* ... for white k in shimmer zone z, faded in from 0 over the zone's first detent (no cliff at the join) */
static inline int32_t sp_shimmer_level(int32_t k, int z)
{
    int32_t sa = sp_shimmer_amount(sp_zone_amount(k, z)), in = k - (sp_zone_lo[z] << 10);
    return in < 0 ? 0 : in < 1024 ? (sa * in) >> 10 : sa;
}

/* GATE: the hold after the input drops, 50 .. 350 ms across the zone (amount a, Q15) */
static inline int32_t sp_gate_hold_ms(int32_t a)
{
    return 50 + ((a * 300 + 16384) >> 15);
}
/* SWELL: the wet's rise after an onset, 0.15 .. 1.5 s across the zone, exponential: 150 x 10^a ms (2^frac by a
 * quadratic, within 0.3 %) */
static inline int32_t sp_swell_ms(int32_t a)
{
    int32_t e = (int32_t)(((uint32_t)(a < 0 ? 0 : a) * 108853u) >> 15);  /* a x log2 10, Q15 */
    int32_t f = e & 32767, p = 32768 + ((f * (21512 + ((f * 11256) >> 15))) >> 15);
    return ((150 * p) >> 15) << (e >> 15);
}

/* ---- orange MIX ---- */
/* Gains for MIX k, Q15 (32768 = unity): the wet, the dry; and on the master the duck depth in dB, Q8. */
static inline void sp_mix(int32_t k, int stereo, int32_t *wet, int32_t *dry, int32_t *duck_db_q8)
{
    k = k < 0 ? 0 : k > 32767 ? 32767 : k;
    *wet = k >= 16384 ? 32768 : 2 * k;
    *dry = 32768;
    *duck_db_q8 = 0;
    if (k <= 16384)
        return;
    if (stereo)
        *duck_db_q8 = k <= 17408 ? (k - 16384) * 3 : 3072 + (((k - 17408) * 13109) >> 16);   /* 12 dB, then 24 */
    else
        *dry = k >= 32767 ? 0 : 32768 - 2 * (k - 16384);
}

#endif
