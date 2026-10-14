/* fm4, the 4-operator FM engine (Ableton Operator style): shared by the engine (fm4.c), the screen (src/slot/ui_fm4.h)
 * and the edit function (src/slot/fm4_edit.h). Design: notes/fm-operator.md; implementation notes and presets:
 * notes/harm-fm4.md.
 *
 * Operators A, B, C, D (index 0..3). A is always a carrier; a modulator always has a higher index than what it
 * modulates, so evaluating D, C, B, A in that order is enough. Feedback is on D in every algorithm.
 *
 * Knobs (range rows type 3: 8 independent knobs, SHIFT + knob = knobs 4..7):
 *   0 blue    INDEX: every modulator's depth, 0 .. 2x (squared; mid = 0.5x), I = level x 4 rad x INDEX
 *   1 green   ALGORITHM 1..11 (k * 11 >> 15)
 *   2 white   TIME: scales every operator envelope, x0.25 .. x4 (mid x1)
 *   3 orange  FEEDBACK on D, 0 .. 1.4 rad (DX7-style: the mean of D's last two outputs)
 *   4..7, v0 (plain knobs, no hook; values 0..32767):
 *     4 SHIFT blue   ratio of B, 5 SHIFT green ratio of C, 6 SHIFT white ratio of D: coarse table entry k >> 10
 *     7 SHIFT orange DETUNE: B x (1 + e), C x (1 - e), D x (1 + e / 2), e up to 1 % (17 cents)
 *     A's ratio is 1, every level 15, carriers' envelopes flat, modulators' FM4_ENV_V0 (decay to half).
 *   4..7, packed (the encoder hook's operator pages; all four negative = packed): knob 4 + op =
 *     0x8000 | coarse << 10 | fine << 7 | level << 3 | envelope   (5 + 3 + 4 + 3 bits)
 * Knobs 0..3 are plain in both forms, so TE's LFO can modulate them. Knobs 4..7 are read from the per-voice params
 * too: if TE's LFO could target them it would scramble packed values (Hypothesis that it can't; notes/fm-operator.md).
 */
#ifndef FM4_H
#define FM4_H
#include <stdint.h>

#define FM4_OPS 4
#define FM4_ALGS 11
#define FM4_COARSES 32
#define FM4_FINES 8
#define FM4_ENVS 8
#define FM4_ENV_V0 5                        /* v0 modulators: decay to half, 0.2 s at TIME x1 */

/* Modulators of A, B, C, D (bit j: op j modulates it) and the carriers, per algorithm. */
static const uint8_t fm4_mods[FM4_ALGS][FM4_OPS] = {
    { 0x2, 0x4, 0x8, 0 },                   /*  1  D -> C -> B -> A */
    { 0x2, 0xC, 0, 0 },                     /*  2  (C + D) -> B -> A */
    { 0xA, 0x4, 0, 0 },                     /*  3  (C -> B) + D -> A */
    { 0x6, 0, 0x8, 0 },                     /*  4  (D -> C) + B -> A */
    { 0x2, 0, 0x8, 0 },                     /*  5  B -> A, D -> C: two pairs */
    { 0xE, 0, 0, 0 },                       /*  6  B + C + D -> A */
    { 0, 0x4, 0x8, 0 },                     /*  7  D -> C -> B, and A */
    { 0x4, 0x4, 0x8, 0 },                   /*  8  D -> C -> (A, B) */
    { 0x8, 0x8, 0x8, 0 },                   /*  9  D -> (A, B, C) */
    { 0, 0, 0x8, 0 },                       /* 10  D -> C, and A, B */
    { 0, 0, 0, 0 },                         /* 11  A + B + C + D */
};
static const uint8_t fm4_car[FM4_ALGS] = { 1, 1, 1, 1, 5, 1, 3, 3, 7, 7, 15 };

/* Coarse ratios in half units: 0.5, 1 .. 24, 26, 28, 30, 32, 36, 40, 48 */
static const uint8_t fm4_coarse2[FM4_COARSES] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48,
    52, 56, 60, 64, 72, 80, 96,
};
/* Fine: ratio x mul (Q16) + add (Q16): exact, +5 cents, -5 cents, +20 cents, +0.25, +0.41, +0.5, x sqrt 2 */
static const uint32_t fm4_fine_mul[FM4_FINES] = { 65536, 65726, 65347, 66297, 65536, 65536, 65536, 92682 };
static const uint16_t fm4_fine_add[FM4_FINES] = { 0, 0, 0, 0, 16384, 26870, 32768, 0 };
/* Level 0 (off) .. 15: -42 .. 0 dB in 3 dB steps, Q15 */
static const uint16_t fm4_level[16] = { 0, 260, 368, 519, 734, 1036, 1464, 2067, 2920, 4125, 5827, 8231, 11626, 16422,
                                        23197, 32767 };
/* Envelope shapes: start and target (Q15 of the level), log2 of the time constant at TIME x1 (Q16 seconds).
 * 0 flat; 1..4 decay to 0 (50 ms, 0.2 s, 0.8 s, 3 s); 5, 6 decay to half (0.2 s, 1 s); 7 swell from 0 (0.5 s) */
static const uint16_t fm4_env_start[FM4_ENVS] = { 32767, 32767, 32767, 32767, 32767, 32767, 32767, 0 };
static const uint16_t fm4_env_target[FM4_ENVS] = { 32767, 0, 0, 0, 0, 16384, 16384, 32767 };
static const int32_t fm4_env_tau[FM4_ENVS] = { 0, -283242, -152170, -21098, 103872, -152170, 0, -65536 };
static const char *const fm4_env_name[FM4_ENVS] = { "FLAT", "PLUCK", "SHORT", "MEDIUM", "LONG", "HALF", "HALF LONG",
                                                     "SWELL" };

struct fm4_patch {
    int alg;                                /* 0..10 */
    int packed;                             /* 1: knobs 4..7 hold the operators (hook), 0: v0 plain knobs */
    uint8_t coarse[FM4_OPS], fine[FM4_OPS], level[FM4_OPS], env[FM4_OPS];
    uint32_t ratio[FM4_OPS];                /* Q16 */
    int32_t index;                          /* Q15: 32767 = 2x */
    int32_t time_q16;                       /* log2 of the envelope time scale, Q16, -2 .. +2 octaves */
    int32_t fb;                             /* 0..32767 = 0 .. 1.4 rad */
    int32_t detune;                         /* v0 knob 7, 0..32767 */
};

static inline int fm4_is_packed(const int16_t *k)
{
    return (k[4] & k[5] & k[6] & k[7]) < 0;
}

static inline int16_t fm4_pack(int coarse, int fine, int level, int env)
{
    return (int16_t)(uint16_t)(0x8000 | (coarse & 31) << 10 | (fine & 7) << 7 | (level & 15) << 3 | (env & 7));
}

static inline int32_t fm4_knob(int16_t v)
{
    return v < 0 ? 0 : v;
}

static inline int fm4_alg_of(int16_t k1)
{
    int a = (fm4_knob(k1) * FM4_ALGS) >> 15;
    return a < FM4_ALGS ? a : FM4_ALGS - 1;
}

static inline void fm4_decode(const int16_t *k, struct fm4_patch *p)
{
    int32_t ix = fm4_knob(k[0]);
    p->index = (ix * ix) >> 15;
    p->alg = fm4_alg_of(k[1]);
    p->time_q16 = (fm4_knob(k[2]) - 16384) << 3;
    p->fb = fm4_knob(k[3]);
    p->packed = fm4_is_packed(k);
    p->detune = 0;
    for (int i = 0; i < FM4_OPS; i++) {
        if (p->packed) {
            uint16_t u = (uint16_t)k[4 + i];
            p->coarse[i] = (u >> 10) & 31;
            p->fine[i] = (u >> 7) & 7;
            p->level[i] = (u >> 3) & 15;
            p->env[i] = u & 7;
        } else {
            p->coarse[i] = i ? fm4_knob(k[3 + i]) >> 10 : 1;
            p->fine[i] = 0;
            p->level[i] = 15;
            p->env[i] = (fm4_car[p->alg] >> i) & 1 ? 0 : FM4_ENV_V0;
        }
        p->ratio[i] = fm4_coarse2[p->coarse[i]] * (fm4_fine_mul[p->fine[i]] >> 1) + fm4_fine_add[p->fine[i]];
    }
    if (!p->packed) {
        int32_t e = (fm4_knob(k[7]) * 655) >> 15;           /* 1 % of 1.0, Q16 */
        p->detune = fm4_knob(k[7]);
        p->ratio[1] += ((p->ratio[1] >> 8) * (uint32_t)e) >> 8;
        p->ratio[2] -= ((p->ratio[2] >> 8) * (uint32_t)e) >> 8;
        p->ratio[3] += ((p->ratio[3] >> 8) * (uint32_t)(e >> 1)) >> 8;
    }
}

/* Ratio as text: "0.5", "1", "13", "1.41", "2.25" (2 decimals at most, no trailing zeros). Returns the end. */
static inline char *fm4_ratio_text(char *p, uint32_t r)
{
    uint32_t whole = r >> 16, frac = r & 0xFFFF;
    uint32_t c = (frac * 100 + 32768) >> 16;                /* hundredths */
    if (c >= 100) {
        whole++;
        c = 0;
    }
    if (whole >= 10) {
        uint32_t t = 0;
        while (whole >= 10) {
            whole -= 10;
            t++;
        }
        *p++ = (char)('0' + t);
    }
    *p++ = (char)('0' + whole);
    if (c) {
        uint32_t t = 0;
        while (c >= 10) {
            c -= 10;
            t++;
        }
        *p++ = '.';
        *p++ = (char)('0' + t);
        if (c)
            *p++ = (char)('0' + c);
    }
    *p = 0;
    return p;
}

#endif
