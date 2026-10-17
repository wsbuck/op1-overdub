/* metal: struck and abused metal on the drum side (notes/drum-engine.md "metal"; tables and kits: gen_metal.py).
 *
 * Runs like fmdrum: drum engine id N on TE's dbox voice objects (12 voices) with our copy of dbox's vtable
 * (slot_gen.inc): render (+0x14), note-on (+0x1C), active (+0x24), note-off (+0x2C), each through a C_ENTRY wrapper.
 * `this` + 0x14 is the voice index TE's factory stored; our state is metal_voice[index] (struct mt_voice, metal.h).
 * TE's per-key loader and saver treat our type like "dbox" (slot.S DRUM_HOOKS): each key's 8 values load and save.
 *
 * Every key is a struck metal object:
 *   modes    8 resonant modes at a material's inharmonic ratios (BAR, PLATE, BELL, PIPE, SHEET, morphing; SPREAD
 *            stretches or squeezes them and detunes them apart), each a magic-circle oscillator
 *            (x -= e y; y += e x, e = 2 sin(pi f / fs): exactly stable, two modes per 32-bit word with the vector
 *            ops), struck: its level from the material and the strike's brightness, its own ring decay (DECAY x
 *            ratio^-alpha: higher modes shorter, by material)
 *   exciter  low-passed noise with its own envelope (NDECAY), its colour and level from STRIKE: a thud (BOOM, with a
 *            pitch drop that follows NDECAY), a mallet, a hammer's click, a scrape (the modes AM-ed by low-passed
 *            noise: noise bands on every partial, the modes held up while it lasts), a rattle (random impacts)
 *   drive    FOLD (a sine folder), CLIP (each mode a square: the 808's square cluster at the material's ratios, then
 *            a hard clip), SCREAM (ring mod with a feedback-FM operator at PITCH x sqrt 2, then the operator FMs
 *            the modes, the feedback to chaos)
 *   grit     CRUSH (bits 14 .. 3) and ALIAS (sample and hold 88 kHz .. 300 Hz)
 * Knobs (per key, TE's dbox path, range row 9; metal.h MT_*), read every block, so turning one changes a sounding note:
 *   [0] blue   PITCH     14 Hz .. 3.5 kHz         [4] SHIFT+blue    STRIKE  BOOM MALLET HAMMER SCRAPE RATTLE
 *   [1] green  MATERIAL  BAR PLATE BELL PIPE SHEET [5] SHIFT+green   SPREAD  TIGHT .. NATURAL .. WIDE
 *   [2] white  DRIVE     FOLD CLIP SCREAM          [6] SHIFT+white   GRIT    CRUSH ALIAS
 *   [3] orange DECAY     ring 30 ms .. 16 s        [7] SHIFT+orange  NDECAY  the exciter, 1.5 ms .. 4 s
 * Per block (128 samples at 88.2 kHz) the envelopes step once; the modes' gains, the exciter's gains, the pitch drop
 * and the operator's increment ramp per sample. A block runs in passes (pre: noise, exciter, operator; then the modes,
 * a pair at a time, summed; post: the drive, the grit, the DC blocker and the knee), each a Blackfin assembly loop with
 * its state in registers; the C loops under #ifndef __bfin__ are the reference, bit for bit (metal_check builds both).
 * Fixed point, no division (reciprocal tables, multiply-shift; udiv once per note), no libgcc, no .bss.
 */
#include <stdint.h>
#include "dsp.h"
#include "metal.h"
#include "metal_tables.h"

#if defined(__bfin__) || defined(__BFIN__)
#define RAM __attribute__((section(".data")))
#else
#define RAM                                 /* host builds (the screen preview, preview.py) */
#endif
#define BLOCK 128
#define ENV1 (1 << 30)
#define SILENT 339547                       /* -70 dB of ENV1 */
#define TAIL_END (8 << 8)                   /* the voice ends when the DC blocker's low-pass is under 8 LSB (Q23) */
#define INC_BASE 669567                     /* 13.75 Hz at 88.2 kHz */
#define Q_OFF 24576
#define O13_MAX 94168                       /* 0.45 fs */
#define CHOKE_DEC 144594432                 /* per block, Q30: T60 5 ms */
#define OSC_A 23000                         /* the oscillators' amplitude (x at y = 0) */
#define OSC_C 529000000                     /* its invariant x^2 + y^2 - e x y */
#define RAD 20861                           /* phase units per radian per Q15 */

struct tobj {
    const void *vptr;
    uint32_t w[4];
    int32_t voice;
};

struct mt_voice metal_voice[MT_VOICES] RAM;
struct mt_diag metal_diag RAM = { 0, 0, 0, 0, 0, -1 };

/* per-block scratch (render calls come one at a time, on TE's audio thread) */
static int32_t mt_acc[BLOCK] RAM;           /* the modes' sum, Q31 */
static int16_t mt_ex[BLOCK] RAM;            /* the exciter, Q15 */
static int16_t mt_rn[BLOCK] RAM;            /* the resonant noise's modulator, Q15 */
static int16_t mt_md[BLOCK / 4] RAM;        /* the modes' frequency factor - 1, Q15, per 4 samples */
static int16_t mt_m[BLOCK] RAM;
static int32_t mt_ex_zero RAM;              /* mt_ex holds zeros (no voice's noise pass ran since) */             /* the operator, Q15 */

static inline uint32_t cycles(void)
{
#ifdef __bfin__
    uint32_t c;
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c));
    return c;
#else
    return 0;
#endif
}

#ifdef __bfin__
#define mt_qmul qmul
#else
static inline int32_t mt_qmul(int32_t a, int32_t b)          /* as __builtin_bfin_mult_fr1x32x32 */
{
    int64_t lo = (int64_t)(((uint32_t)a & 0xFFFF) * ((uint32_t)b & 0xFFFF) >> 16);
    lo = (lo + (int64_t)(a >> 16) * (b & 0xFFFF) + (int64_t)(b >> 16) * (a & 0xFFFF)) >> 15;
    int64_t hi = (a >> 16) == -32768 && (b >> 16) == -32768 ? 0x7FFFFFFF : (int64_t)(a >> 16) * (b >> 16) * 2;
    int64_t r = hi + lo;
    return r > 0x7FFFFFFF ? 0x7FFFFFFF : r < -0x7FFFFFFF - 1 ? (int32_t)0x80000000 : (int32_t)r;
}
#endif

static inline int32_t sat16(int32_t x)
{
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

/* the Blackfin's fractional 16 x 16 multiply with a 16-bit result: rounded, ties to even (as the simulator; TE's
 * RND_MOD could make the device round ties up: 1 LSB, rarely) */
#ifdef __bfin__
static inline int32_t mulr(int32_t a, int32_t b)                  /* (GCC's mult_fr1x16 rounds differently) */
{
    int32_t r;
    __asm__("%0.L = %1.L * %2.L;\n\t%0 = %0.L (X);" : "=&d"(r) : "d"(a), "d"(b));
    return r;
}
#else
static inline int32_t mulr(int32_t a, int32_t b)
{
    if (a == -32768 && b == -32768)
        return 32767;
    int32_t p = a * b * 2, r = (p + 0x8000) >> 16;
    if ((p & 0xFFFF) == 0x8000)
        r &= ~1;
    return r;
}
#endif

static inline int32_t env_step(int32_t e, int32_t factor)
{
    return mt_qmul(e, factor << 1);
}

static uint32_t inc_of(int32_t q)
{
    int32_t u = q + Q_OFF;
    uint32_t o13 = u < 0 ? 0 : ((uint32_t)u * 21845u) >> 14;
    if (o13 > O13_MAX)
        o13 = O13_MAX;
    uint32_t oct = o13 >> 13, fr = o13 & 8191, i = fr >> 7, f7 = fr & 127;
    uint32_t m = mt_exp2[i] + (((mt_exp2[i + 1] - mt_exp2[i]) * f7) >> 7);
    return (((INC_BASE >> 4) * m) >> 11) << oct;
}

/* 2^-(u / 2^16), u >= 0 (log2 units Q16), Q30 */
static int32_t exp2neg(int32_t u)
{
    if (u <= 0)
        return ENV1;
    int32_t n = u >> 16;
    if (n >= 30)
        return 0;
    int32_t i = (u >> 8) & 255, f = u & 255;
    int32_t a = mt_exp2n[i], b = mt_exp2n[i + 1];
    return (a + (((b - a) >> 8) * f)) >> n;
}

/* 2^(u / 6144) for u in K0 units (octaves x 6144), u <= 0: Q15 (1.0 = 32767) */
static int32_t ratio_down(int32_t u)
{
    if (u >= 0)
        return 32767;
    uint32_t a = (uint32_t)(-u);
    return (int32_t)(exp2neg((int32_t)((a * 683u) >> 6)) >> 15);   /* a / 6144 x 2^16 = a x 683 / 64 */
}

/* 2^(u / 6144) for 0 <= u < 6 octaves: Q12 */
static int32_t ratio_up_q12(int32_t u)
{
    if (u <= 0)
        return 4096;
    uint32_t o13 = ((uint32_t)u * 21845u) >> 14;                     /* octaves Q13 */
    uint32_t oct = o13 >> 13, fr = o13 & 8191, i = fr >> 7, f7 = fr & 127;
    uint32_t m = mt_exp2[i] + (((mt_exp2[i + 1] - mt_exp2[i]) * f7) >> 7);   /* Q15 */
    return oct > 6 ? 0x40000 : (int32_t)((m << oct) >> 3);
}

/* e (Q23) of a mode at increment inc: 2 sin(pi inc / 2^32) = 2 (t - t^3 / 6 + t^5 / 120), t = pi inc / 2^32 <= pi / 6 */
static int32_t e_q23(uint32_t inc)
{
    int32_t t = mt_qmul((int32_t)(inc >> 1), 1686629713) << 2;      /* t, Q31: (inc / 2^32) pi; pi / 4 in Q31 */
    int32_t t2 = mt_qmul(t, t), t3 = mt_qmul(t2, t), t5 = mt_qmul(t3, t2);
    int32_t s = t - mt_qmul(t3, 357913941) + mt_qmul(t5, 17895697);  /* 1 / 6, 1 / 120 in Q31 */
    return s >> 7;                                                     /* 2 sin, Q23 */
}

/* 2^22 / a for a >= 1 (within 0.1 %): a = m 2^k, m in 2^10 .. 2^11, 1 / m from mt_recip (65 points, interpolated) */
static int32_t recip_q22(uint32_t a)
{
    int32_t k = 0;
    while (a >= 1u << 11) {
        a >>= 1;
        k++;
    }
    while (a < 1u << 10) {
        a <<= 1;
        k--;
    }
    uint32_t i = (a - 1024) >> 4, f = a & 15;
    int32_t r = mt_recip[i] + (((mt_recip[i + 1] - mt_recip[i]) * (int32_t)f) >> 4);   /* 2^22 / m */
    return k >= 0 ? r >> k : r << -k;
}

/* MT_MODE_AMP x 2^12 / sqrt(n2) (the strike's gain, Q14, at most 32767): n2 = m 4^k, m in 2^14 .. 2^16, 1 / sqrt(m)
 * from mt_rsqrt (49 points, interpolated) */
static int32_t rsqrt_gain(uint32_t n2)
{
    if (!n2)
        return 32767;
    int32_t k = 0;
    while (n2 >= 1u << 16) {
        n2 >>= 2;
        k++;
    }
    while (n2 < 1u << 14) {
        n2 <<= 2;
        k--;
    }
    uint32_t i = (n2 - (1u << 14)) >> 10, f = n2 & 1023;
    int32_t t = mt_rsqrt[i] + (((mt_rsqrt[i + 1] - mt_rsqrt[i]) * (int32_t)f) >> 10);
    int32_t g = (MT_MODE_AMP * t) >> 10;
    if (k > 0)
        g >>= k;
    else
        for (; k < 0 && g <= 32767; k++)
            g <<= 1;
    return g > 32767 ? 32767 : g;
}

/* ---- the passes. Ramps (struct ramp) are per sample over one run of n samples. ---- */
struct pre {                                /* the exciter, the resonant noise and the operator */
    int32_t c1, c2, rng;                    /* one-poles Q15, the rn gain Q9 */
    int32_t dgex, dgrn, dmdv;               /* ramps */
    int32_t fmd;                            /* the operator's FM of the modes Q15 */
};

struct post {
    int32_t fw, fg, cg, rw, lv;             /* the drive (per block) */
    int32_t bi, bf;                         /* crush bits (integer, Q8 fraction) */
    uint32_t hinc;                          /* sample and hold */
};


/* 1a. noise -> the exciter (mt_ex) and the resonant noise's modulator (mt_rn), both one-poles at once (a vector) */
static void pass_noise(struct mt_voice *v, int n, const struct pre *c)
{
#ifdef __bfin__
    struct {
        uint32_t seed, a, cc;               /*  0  4  8: the LCG */
        uint32_t lp, cp;                    /* 12 16: lp1 | lp2 << 16, c1 | c2 << 16 */
        uint32_t rng;                       /* 20 */
        int32_t gex, dgex;                  /* 24 28: the exciter's gain Q12 << 16 and its ramp */
    } k;
    k.seed = v->seed;
    k.a = 1664525u;
    k.cc = 1013904223u;
    k.lp = (uint16_t)v->lp1 | (uint32_t)(uint16_t)v->lp2 << 16;
    k.cp = (uint16_t)c->c1 | (uint32_t)(uint16_t)c->c2 << 16;
    k.rng = (uint32_t)(uint16_t)c->rng;
    k.gex = v->gex;
    k.dgex = c->dgex;
#define NOISE_HEAD                                                                                                   \
    "R7 = [%[c] + 0];\n\tR6 = [%[c] + 4];\n\tR4 = [%[c] + 12];\n\tR5 = [%[c] + 16];\n\tR3 = [%[c] + 20];\n\t"       \
    "R2 = [%[c] + 24];\n\t"                                                                                         \
    "I0 = %[ex];\n\tI1 = %[rn];\n\t"                                                                                \
    "LSETUP (1f, 2f) LC0 = %[n];\n"                                                                                \
    "1:\tR7 *= R6;\n\t"                                                                                             \
    "R1 = [%[c] + 8];\n\t"                                                                                          \
    "R7 = R7 + R1 (NS) || R1 = [%[c] + 28];\n\t"                                                                    \
    "R0 = R7 >>> 17;\n\t"                                                                                           \
    "R0 = PACK (R0.L, R0.L);\n\t"                                                                                   \
    "R0 = R0 -|- R4 (S);\n\t"                                                                                       \
    "R0.H = R0.H * R5.H, R0.L = R0.L * R5.L;\n\t"                                                                   \
    "R4 = R4 +|+ R0 (S);\n\t"                                                                                       \
    "A0 = R4.L * R2.H (IS);\n\t"                                                                                    \
    "A1 = R4.H * R3.L (IS);\n\t"                                                                                    \
    "A0 = A0 >>> 12;\n\t"                                                                                           \
    "A1 = A1 >>> 9;\n\t"                                                                                            \
    "R0.H = A1, R0.L = A0 (IS);\n\t"                                                                                \
    "R2 = R2 + R1 (NS) || W [I0++] = R0.L;\n"
#define NOISE_TAIL                                                                                                   \
    "2:\tW [I1++] = R0.H;\n\t"                                                                                      \
    "[%[c] + 0] = R7;\n\t[%[c] + 12] = R4;\n\t[%[c] + 24] = R2;\n\t"                                             \
    : : [c] "a"(&k), [ex] "a"(mt_ex), [rn] "a"(mt_rn), [n] "a"(n)                                                   \
    : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "I0", "I1", "A0", "A1", "LC0", "LT0", "LB0", "memory"
    __asm__ volatile(NOISE_HEAD NOISE_TAIL);
    v->seed = k.seed;
    v->lp1 = (int16_t)k.lp;
    v->lp2 = (int16_t)(k.lp >> 16);
    v->gex = k.gex;
#else
    uint32_t seed = v->seed;
    int32_t lp1 = v->lp1, lp2 = v->lp2, gex = v->gex;
    for (int i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        int32_t x = (int32_t)seed >> 17;                            /* white, +-16384 */
        lp1 = sat16(lp1 + mulr(sat16(x - lp1), c->c1));
        lp2 = sat16(lp2 + mulr(sat16(x - lp2), c->c2));
        mt_ex[i] = (int16_t)sat16((lp1 * (gex >> 16)) >> 12);
        mt_rn[i] = (int16_t)sat16((lp2 * c->rng) >> 9);
        gex = (int32_t)((uint32_t)gex + (uint32_t)c->dgex);
    }
    v->seed = seed;
    v->lp1 = lp1;
    v->lp2 = lp2;
    v->gex = gex;
#endif
}

/* 1b. SCREAM's operator (mt_m): a sine with feedback (the mean of its last two outputs x beta, as the DX7); its
 * increment and feedback depth step per block (it follows the pitch; a chaotic or ring-mod carrier, never alone) */
static void pass_op(struct mt_voice *v, int n, const struct pre *c)
{
    (void)c;
#ifdef __bfin__
    struct {
        uint32_t oph, oinc, pad;            /*  0  4  8 */
        int32_t m1, s, gf, pad2;            /* 12 16 20 24: s = m1 + m2 */
        const uint32_t *tab;                /* 28 */
    } k;
    k.oph = v->oph;
    k.oinc = v->oinc;
    k.m1 = v->m1;
    k.s = v->m1 + v->m2;
    k.gf = v->gf;
    k.tab = (const uint32_t *)(const void *)mt_sin1k;
    __asm__ volatile(
        "R0 = [%[c] + 0];\n\tI2 = R0;\n\t"
        "R0 = [%[c] + 4];\n\tM2 = R0;\n\t"
        "R6 = [%[c] + 12];\n\tR7 = [%[c] + 16];\n\tR5 = [%[c] + 20];\n\t"
        "P4 = [%[c] + 28];\n\t"
        "I0 = %[m];\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR1 = I2;\n\t"
        "R7 *= R5;\n\t"                                        /* (m1 + m2) gf */
        "R0 = R7 + R1;\n\t"
        "R0 >>= 22;\n\t"                                      /* mt_sin1k[ph >> 22] */
        "P0 = R0;\n\t"
        "I2 += M2;\n\t"                                       /* oph += oinc */
        "R3 = R6;\n\t"                                        /* (m1, before P0's use as an address) */
        "P0 = P4 + (P0 << 1);\n\t"
        "R2 = W [P0] (X);\n\t"
        "R7 = R2 + R3 (NS) || W [I0++] = R2.L;\n"             /* m1 + m2 for the next sample */
        "2:\tR6 = R2;\n\t"
        "R0 = I2;\n\t[%[c] + 0] = R0;\n\t"
        "[%[c] + 12] = R6;\n\t[%[c] + 16] = R7;\n\t[%[c] + 20] = R5;\n\t"
        :
        : [c] "a"(&k), [m] "a"(mt_m), [n] "a"(n)
        : "R0", "R1", "R2", "R3", "R5", "R6", "R7", "P0", "P4", "I0", "I2", "M2", "LC0",
          "LT0", "LB0", "memory");
    v->oph = k.oph;
    v->m1 = k.m1;
    v->m2 = k.s - k.m1;
#else
    uint32_t oph = v->oph, oinc = v->oinc;
    int32_t m1 = v->m1, m2 = v->m2, gf = v->gf;
    for (int i = 0; i < n; i++) {
        int32_t ms = mt_sin1k[(oph + (uint32_t)(m1 + m2) * (uint32_t)gf) >> 22];
        m2 = m1;
        m1 = ms;
        mt_m[i] = (int16_t)ms;
        oph += oinc;
    }
    v->oph = oph;
    v->m1 = m1;
    v->m2 = m2;
#endif
}

/* 1c. the modes' frequency factor - 1, one per 4 samples: the BOOM drop's ramp (mdv) plus the operator's FM (fmd,
 * 0 without the operator: mt_m is then not read for anything) */
static void build_md(struct mt_voice *v, int n, const struct pre *c, int32_t fmd)
{
    uint32_t mdv = (uint32_t)v->mdv, d4 = (uint32_t)c->dmdv << 2;
#ifdef __bfin__
    __asm__ volatile(
        "I0 = %[m];\n\tI1 = %[md];\n\tM0 = 8 (X);\n\t"
        "LSETUP (1f, 2f) LC0 = %[n4];\n"
        "1:\tR3.L = W [I0];\n\t"
        "R3.L = R3.L * %[f].L || I0 += M0;\n\t"                /* mulr(m, fmd) */
        "R3.L = R3.L + %[v].H (S);\n\t"                      /* + the drop's factor */
        "%[v] = %[v] + %[d] (NS);\n"
        "2:\tW [I1++] = R3.L;\n\t"
        : [v] "+d"(mdv)
        : [m] "a"(mt_m), [md] "a"(mt_md), [n4] "a"(n >> 2), [f] "d"(fmd), [d] "d"(d4)
        : "R3", "I0", "I1", "M0", "LC0", "LT0", "LB0", "memory");
#else
    for (int j = 0; j < n >> 2; j++) {
        mt_md[j] = (int16_t)sat16(((int32_t)mdv >> 16) + mulr(mt_m[4 * j], fmd));
        mdv += d4;
    }
#endif
    v->mdv = (int32_t)mdv;
}

/* 2. one pair of modes (j: modes 2j, 2j + 1) into mt_acc: the magic circle with e x (1 + md) (MD: md taken every 4
 * samples; the loop runs 4 samples per pass, n a multiple of 16), each mode blended
 * toward its square sat(y << 3) by sq (SQ), x its gain (ramped by da every 4 samples: slopes under 1 LSB per
 * sample still move) */
#ifdef __bfin__
/* the pair's words: R0 x, R1 y, R2 e, R3 gains, R4.L sq; mt_md through I0, mt_acc read through I1, written through
 * P1; the gains' ramp at [P2]. Low half = mode 2j, high half = 2j + 1 (the int16 arrays' word). */
#define PAIR_HEAD                                                                                                    \
    "R0 = [%[c] + 0];\n\tR1 = [%[c] + 4];\n\tR2 = [%[c] + 8];\n\tR3 = [%[c] + 12];\n\tR4 = [%[c] + 24];\n\t"         \
    "P2 = %[c];\n\tP2 += 16;\n\t"                                                                                \
    "I0 = %[md];\n\tI1 = %[acc];\n\tP1 = %[acc];\n\t"                                                              \
    "LSETUP (1f, 2f) LC0 = %[n4];\n"                                                                               \
    "1:\t"
#define PAIR_MD                                         /* R2 = e x (1 + md) for the next 4 samples */               \
    "R5.L = W [I0++];\n\t"                                                                                          \
    "R2 = [%[c] + 8];\n\t"                                                                                          \
    "R6.H = R2.H * R5.L, R6.L = R2.L * R5.L;\n\t"                                                                   \
    "R2 = R2 +|+ R6 (S);\n\t"
#define PAIR_OSC                                        /* x -= e y; y += e x */                                      \
    "R5.H = R2.H * R1.H, R5.L = R2.L * R1.L;\n\t"                                                                   \
    "R0 = R0 -|- R5 (S);\n\t"                                                                                       \
    "R5.H = R2.H * R0.H, R5.L = R2.L * R0.L;\n\t"                                                                   \
    "R1 = R1 +|+ R5 (S);\n\t"
#define PAIR_SQ                                         /* R5 = sat(y << 3): the modes' squares */                    \
    "R5 = R1 << 3 (V, S);\n\t"
/* acc += 2 (ya ga + yb gb), + 2 (sa qa + sb qb) with the squares (Q: their gains in R4); L labels the 4th body's last
 * instruction (the loop's end); the 4th (_R) also steps the gains ([P2], the squares' [P2 + 4]) */
#define PAIR_MAC(LOAD) "A1 = R1.H * R3.H, A0 = R1.L * R3.L" LOAD ";\n\t"
#define PAIR_MACQ(LOAD) "A1 = R1.H * R3.H, A0 = R1.L * R3.L" LOAD ";\n\tA1 += R5.H * R4.H, A0 += R5.L * R4.L;\n\t"
#define PAIR_ACC(M, L)                                                                                               \
    M(" || R6 = [I1++]")                                                                                            \
    "R7 = (A0 += A1);\n\t"                                                                                          \
    "R6 = R6 + R7 (S);\n" L                                                                                         \
    "\t[P1++] = R6;\n"
#define PAIR_ACC_R(L)                                                                                                \
    PAIR_MAC(" || R6 = [I1++]")                                                                                     \
    "R7 = (A0 += A1) || R5 = [P2];\n\t"                                                                             \
    "R6 = R6 + R7 (S);\n" L                                                                                         \
    "\tR3 = R3 +|+ R5 (S) || [P1++] = R6;\n"
#define PAIR_ACCQ_R(L)                                                                                               \
    PAIR_MACQ(" || R6 = [I1++]")                                                                                    \
    "R7 = (A0 += A1) || R5 = [P2];\n\t"                                                                             \
    "R6 = R6 + R7 (S) || R7 = [P2 + 4];\n\t"                                                                        \
    "R4 = R4 +|+ R7 (S);\n" L                                                                                       \
    "\tR3 = R3 +|+ R5 (S) || [P1++] = R6;\n"
#define PAIR_SET(M, L)                                                                                               \
    M("")                                                                                                           \
    "R7 = (A0 += A1);\n" L                                                                                          \
    "\t[P1++] = R7;\n"
#define PAIR_SET_R(L)                                                                                                \
    PAIR_MAC("")                                                                                                    \
    "R7 = (A0 += A1) || R5 = [P2];\n" L                                                                             \
    "\tR3 = R3 +|+ R5 (S) || [P1++] = R7;\n"
#define PAIR_SETQ_R(L)                                                                                               \
    PAIR_MACQ("")                                                                                                   \
    "R7 = (A0 += A1) || R5 = [P2];\n\t"                                                                             \
    "R6 = [P2 + 4];\n\t"                                                                                            \
    "R4 = R4 +|+ R6 (S);\n" L                                                                                       \
    "\tR3 = R3 +|+ R5 (S) || [P1++] = R7;\n"
#define PAIR_X4(BODY, BODY_R) BODY("") BODY("") BODY("") BODY_R("2:")
#define PAIR_END                                                                                                     \
    "\t[%[c] + 0] = R0;\n\t[%[c] + 4] = R1;\n\t[%[c] + 12] = R3;\n\t[%[c] + 24] = R4;\n\t"
#define PAIR_ARGS                                                                                                     \
    : : [c] "a"(&pc), [md] "a"(mt_md), [acc] "a"(mt_acc), [n4] "a"(n >> 2)                                           \
    : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P1", "P2", "I0", "I1", "A0", "A1", "LC0", "LT0", "LB0",      \
      "memory"
/* the 4-sample bodies: oscillators, squares, the sum */
#define B_PA(L) PAIR_OSC PAIR_ACC(PAIR_MAC, L)
#define B_PS(L) PAIR_OSC PAIR_SET(PAIR_MAC, L)
#define B_QA(L) PAIR_OSC PAIR_SQ PAIR_ACC(PAIR_MACQ, L)
#define B_QS(L) PAIR_OSC PAIR_SQ PAIR_SET(PAIR_MACQ, L)
#define B_PA_R(L) PAIR_OSC PAIR_ACC_R(L)
#define B_PS_R(L) PAIR_OSC PAIR_SET_R(L)
#define B_QA_R(L) PAIR_OSC PAIR_SQ PAIR_ACCQ_R(L)
#define B_QS_R(L) PAIR_OSC PAIR_SQ PAIR_SETQ_R(L)
#endif

static void pass_pair(struct mt_voice *v, int j, int n, const int16_t *da, const int16_t *dq, int MD, int SQ,
                      int FIRST)
{
#ifdef __bfin__
    struct {
        uint32_t x, y, e, amp, damp, dampq, ampq;   /* 0 4 8 12 16 20 24: pair words */
    } pc;
    const uint32_t *w32;
    w32 = (const uint32_t *)(void *)v->x;
    pc.x = w32[j];
    w32 = (const uint32_t *)(void *)v->y;
    pc.y = w32[j];
    w32 = (const uint32_t *)(void *)v->e;
    pc.e = w32[j];
    w32 = (const uint32_t *)(void *)v->amp;
    pc.amp = w32[j];
    pc.damp = (uint16_t)da[0] | (uint32_t)(uint16_t)da[1] << 16;
    pc.dampq = (uint16_t)dq[0] | (uint32_t)(uint16_t)dq[1] << 16;
    w32 = (const uint32_t *)(void *)v->ampq;
    pc.ampq = w32[j];
    if (FIRST) {
        if (MD && SQ)
            __asm__ volatile(PAIR_HEAD PAIR_MD PAIR_X4(B_QS, B_QS_R) PAIR_END PAIR_ARGS);
        else if (MD)
            __asm__ volatile(PAIR_HEAD PAIR_MD PAIR_X4(B_PS, B_PS_R) PAIR_END PAIR_ARGS);
        else if (SQ)
            __asm__ volatile(PAIR_HEAD PAIR_X4(B_QS, B_QS_R) PAIR_END PAIR_ARGS);
        else
            __asm__ volatile(PAIR_HEAD PAIR_X4(B_PS, B_PS_R) PAIR_END PAIR_ARGS);
    } else {
        if (MD && SQ)
            __asm__ volatile(PAIR_HEAD PAIR_MD PAIR_X4(B_QA, B_QA_R) PAIR_END PAIR_ARGS);
        else if (MD)
            __asm__ volatile(PAIR_HEAD PAIR_MD PAIR_X4(B_PA, B_PA_R) PAIR_END PAIR_ARGS);
        else if (SQ)
            __asm__ volatile(PAIR_HEAD PAIR_X4(B_QA, B_QA_R) PAIR_END PAIR_ARGS);
        else
            __asm__ volatile(PAIR_HEAD PAIR_X4(B_PA, B_PA_R) PAIR_END PAIR_ARGS);
    }
    uint32_t *o32;
    o32 = (uint32_t *)(void *)v->x;
    o32[j] = pc.x;
    o32 = (uint32_t *)(void *)v->y;
    o32[j] = pc.y;
    o32 = (uint32_t *)(void *)v->amp;
    o32[j] = pc.amp;
    o32 = (uint32_t *)(void *)v->ampq;
    o32[j] = pc.ampq;
#else
    int32_t xa = v->x[2 * j], xb = v->x[2 * j + 1], ya = v->y[2 * j], yb = v->y[2 * j + 1];
    const int32_t ea = v->e[2 * j], eb = v->e[2 * j + 1];
    int32_t aa = v->amp[2 * j], ab = v->amp[2 * j + 1], qa = v->ampq[2 * j], qb = v->ampq[2 * j + 1];
    const int32_t dxa = da[0], dxb = da[1], dqa = dq[0], dqb = dq[1];
    for (int i = 0; i < n; i++) {
        int32_t fa = ea, fb = eb;
        if (MD) {                           /* the factor of every 4th sample */
            int32_t md = mt_md[i >> 2];
            fa = sat16(ea + mulr(ea, md));
            fb = sat16(eb + mulr(eb, md));
        }
        xa = sat16(xa - mulr(fa, ya));
        xb = sat16(xb - mulr(fb, yb));
        ya = sat16(ya + mulr(fa, xa));
        yb = sat16(yb + mulr(fb, xb));
        int64_t p = (int64_t)(ya * aa) * 2 + (int64_t)(yb * ab) * 2;   /* (A0 += A1): 40 bits, then saturated */
        if (SQ)                                                          /* + the squares sat(y << 3) x their gains */
            p += (int64_t)(sat16(ya << 3) * qa) * 2 + (int64_t)(sat16(yb << 3) * qb) * 2;
        p = p > 0x7FFFFFFF ? 0x7FFFFFFF : p < -0x7FFFFFFFLL - 1 ? -0x7FFFFFFFLL - 1 : p;
        int64_t s = (FIRST ? 0 : (int64_t)mt_acc[i]) + p;
        mt_acc[i] = (int32_t)(s > 0x7FFFFFFF ? 0x7FFFFFFF : s < -0x7FFFFFFFLL - 1 ? -0x7FFFFFFFLL - 1 : s);
        if ((i & 3) == 3) {                 /* the gains' ramp every 4th sample */
            aa = sat16(aa + dxa);
            ab = sat16(ab + dxb);
            if (SQ) {
                qa = sat16(qa + dqa);
                qb = sat16(qb + dqb);
            }
        }
    }
    v->x[2 * j] = (int16_t)xa;
    v->x[2 * j + 1] = (int16_t)xb;
    v->y[2 * j] = (int16_t)ya;
    v->y[2 * j + 1] = (int16_t)yb;
    v->amp[2 * j] = (int16_t)aa;
    v->amp[2 * j + 1] = (int16_t)ab;
    v->ampq[2 * j] = (int16_t)qa;
    v->ampq[2 * j + 1] = (int16_t)qb;
#endif
}

/* 3. the output. Clean keys: the modes plus the exciter, the DC blocker and the knee (pass_clean). Otherwise two
 * passes through mt_s: A (pass_drive) the resonant noise (RN: the modes AM-ed by the low-passed noise), the exciter,
 * then (DRV) the fold, (RING) the ring mod with the operator, the clip and the level; B (pass_out) the crush and the
 * sample and hold (GRIT), the DC blocker and the knee. */
static int16_t mt_s[BLOCK] RAM;

#ifdef __bfin__
/* the DC blocker (a one-pole high-pass at 6.9 Hz: lp += (x - lp) >> 11, lp Q23 in R7) and the knee: y (Q23) in R0
 * -> out (P1); the knee out of line above 0.75 FS (constants at [P5 + 64 .. 72]). NEXT: the loop's last instruction
 * (the next sample's load). */
#define OUT_DC(NEXT)                                                                                                 \
    "R1 = R0 - R7 (NS);\n\t"                                                                                        \
    "R2 = R1 >>> 11;\n\t"                                                                                           \
    "R7 = R7 + R2;\n\t"                                                                                             \
    "R0 = R1 >>> 8;\n\t"                                                                                            \
    "R1 = ABS R0;\n\t"                                                                                              \
    "R2 = [%[c] + 64];\n\t"                                                                                         \
    "CC = R1 <= R2;\n\t"                                                                                            \
    "IF !CC JUMP 4f;\n"                                                                                             \
    "3:\tW [P1++] = R0;\n"                                                                                          \
    "2:\t" NEXT "\n\t"                                                                                              \
    "JUMP 5f;\n"                                                                                                    \
    "4:\tR2 = [%[c] + 68];\n\t"                 /* the knee: a = min(|x|, top), d = a - KNEE_T, */                    \
    "R1 = MIN (R1, R2);\n\t"                    /* min(a - d^2 >> 15, 32767), x's sign */                             \
    "R2 = [%[c] + 64];\n\t"                                                                                         \
    "R2 = R1 - R2;\n\t"                                                                                             \
    "R2 = R2.L * R2.L (IS);\n\t"                                                                                    \
    "R2 >>>= 15;\n\t"                                                                                               \
    "R1 = R1 - R2;\n\t"                                                                                             \
    "R2 = [%[c] + 72];\n\t"                                                                                         \
    "R1 = MIN (R1, R2);\n\t"                                                                                        \
    "R2 = -R1;\n\t"                                                                                                 \
    "CC = R0 < 0;\n\t"                                                                                              \
    "IF CC R1 = R2;\n\t"                                                                                            \
    "R0 = R1;\n\t"                                                                                                  \
    "JUMP 3b;\n"                                                                                                    \
    "5:\t[%[c] + 52] = R7;\n\t[%[c] + 32] = R6;\n\t"
/* the same with a plain 16-bit saturation instead of the knee (driven keys: already clipped) */
#define OUT_DC_SAT(NEXT)                                                                                             \
    "R1 = R0 - R7 (NS);\n\t"                                                                                        \
    "R2 = R1 >>> 11;\n\t"                                                                                           \
    "R7 = R7 + R2;\n\t"                                                                                             \
    "R0 = R1 << 8 (S);\n\t"                                                                                         \
    "R0 >>>= 16;\n\t"                                                                                               \
    "W [P1++] = R0;\n"                                                                                              \
    "2:\t" NEXT "\n\t"                                                                                              \
    "[%[c] + 52] = R7;\n\t[%[c] + 32] = R6;\n\t"
/* the post gain (the envelope and DRIVE's level: I1 += M1, Q12 << 16) on s (R1), then the exciter, dry; y Q23 in R0 */
#define OUT_GAIN                                                                                                     \
    "R2 = I1;\n\t"                                                                                                  \
    "A0 = R1.L * R2.H (IS);\n\t"                                                                                    \
    "A0 = A0 >>> 12;\n\t"                                                                                           \
    "R0.L = A0 (IS) || R1.L = W [I2++];\n\t"                                                                        \
    "R0.L = R0.L + R1.L (S);\n\t"                                                                                   \
    "I1 += M1;\n\t"                                                                                                 \
    "R0 = R0.L (X);\n\t"                                                                                            \
    "R0 <<= 8;\n\t"
#define OUT_ARGS                                                                                                     \
    : : [c] "a"(&k)                                                                                                 \
    : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P0", "P1", "P2", "P4", "I0", "I1", "I2", "I3", "M1", "A0", "A1", \
      "LC0", "LT0", "LB0", "CC", "memory"
#endif

struct outk {                               /* the output passes' constants and state (asm offsets) */
    int32_t grn, dgrn;                      /*  0  4 */
    uint32_t fwfg, rwcg, ctlv;              /*  8 12 16: fw | fg << 16, rw | cg << 16, lv << 16 */
    uint32_t hadd, mask;                    /* 20 24: crush h | h2 << 16, m | m2 << 16 */
    int32_t bfq;                            /* 28: the crush blend Q15 (low half) */
    uint32_t shph, hinc;                    /* 32 36 */
    int32_t shv;                            /* 40 */
    uint32_t c0r;                           /* 44: the ring's (1 - rw) / 2 << 16 */
    int32_t dgp2;                           /* 48: dgp again, in a short load's reach (pass_drive) */
    int32_t lp, pad2, pad3;                 /* 52: the DC blocker's low-pass, Q23 */
    int32_t knee, top, one;                 /* 64 68 72 */
    const uint32_t *tab;                    /* 76 */
    const int32_t *acc;                     /* 80 */
    const int16_t *rn, *ex, *mo;            /* 84 88 92 */
    int16_t *out, *sb;                      /* 96 100 */
    int32_t n;                              /* 104 */
    int32_t gp, dgp;                        /* 108 112: the post gain (Q12 << 16) and its ramp */
};

/* the clean output: s = the modes + the exciter -> DC -> knee */
static void pass_clean(struct mt_voice *v, int16_t *out, int n, int32_t dgp)
{
#ifdef __bfin__
    struct outk k;
    k.gp = v->gp;
    k.dgp = dgp;
    k.lp = v->dcy;
    k.knee = KNEE_T;
    k.top = KNEE_T + 2 * (32767 - KNEE_T);
    k.one = 32767;
    k.acc = mt_acc;
    k.ex = mt_ex;
    k.out = out;
    k.n = n;
    __asm__ volatile(
        "R7 = [%[c] + 52];\n\t"
        "R0 = [%[c] + 80];\n\tI0 = R0;\n\tR0 = [%[c] + 88];\n\tI2 = R0;\n\tP1 = [%[c] + 96];\n\tP2 = [%[c] + 104];\n\t"
        "R0 = [%[c] + 108];\n\tI1 = R0;\n\tR0 = [%[c] + 112];\n\tM1 = R0;\n\t"
        "R3 = [I0++];\n\t"
        "LSETUP (1f, 2f) LC0 = P2;\n"
        "1:\tR1 = R3 >>> 16;\n\t"
        OUT_GAIN
        OUT_DC("R3 = [I0++];")
        OUT_ARGS);
    v->dcy = k.lp;
    v->gp = (int32_t)((uint32_t)v->gp + (uint32_t)dgp * (uint32_t)n);
#else
    int32_t lp = v->dcy, gp = v->gp;
    for (int i = 0; i < n; i++) {
        int32_t s = sat16(sat16(((mt_acc[i] >> 16) * (gp >> 16)) >> 12) + mt_ex[i]);
        gp = (int32_t)((uint32_t)gp + (uint32_t)dgp);
        int32_t y = (s << 8) - lp;                                   /* Q23, the DC blocker */
        lp += y >> 11;
        out[i] = soft_knee(y >> 8);
    }
    v->dcy = lp;
    v->gp = gp;
#endif
}

#ifdef __bfin__
#define DRIVE_HEAD                                                                                                   \
    "R7 = [%[c] + 0];\n\tR5 = [%[c] + 8];\n\tR4 = [%[c] + 12];\n\tR3 = [%[c] + 16];\n\tP4 = [%[c] + 76];\n\t"       \
    "R0 = [%[c] + 80];\n\tI0 = R0;\n\tR0 = [%[c] + 84];\n\tI1 = R0;\n\tR0 = [%[c] + 88];\n\tI2 = R0;\n\t"           \
    "R0 = [%[c] + 92];\n\tI3 = R0;\n\tP1 = [%[c] + 100];\n\tP2 = [%[c] + 104];\n\tR3 = [%[c] + 108];\n\t"           \
    "R6 = [I0++];\n\tR1.L = W [I1++];\n\t"                                                                          \
    "LSETUP (1f, 2f) LC0 = P2;\n"                                                                                  \
    "1:\t"
#define DRIVE_RN                                        /* s x 2 ((1 - g) / 2 + g rn): g R7.H, (1 - g) / 2 R7.L */     \
    "R1.L = R7.H * R1.L;\n\t"                                                                                       \
    "R1.L = R1.L + R7.L (S) || R0 = [%[c] + 4];\n\t"                                                                \
    "R6.H = R6.H * R1.L;\n\t"                                                                                       \
    "R6 = R6 << 1 (V, S);\n\t"                                                                                      \
    "R7 = R7 +|+ R0 (S);\n\t"                                                    /* the ramps */
#define DRIVE_EX ""
#define DRIVE_FOLD(MLOAD)                               /* s += fw (sin(pi / 2 fg s) - s), mt_sin1k (P4) */           \
    "A0 = R6.H * R5.H (IS);\n\t"                                                                                    \
    "R0 = A0;\n\t"                                                                                                  \
    "R0 <<= 4;\n\t"                                                                                                 \
    "R0 >>= 22;\n\t"                                                                                                \
    "P0 = R0;\n\t"                                                                                                  \
    "R0 = [%[c] + 44];\n\t"                       /* the ring's (1 - rw) / 2 << 16 (before P0's use as an address) */ \
    "P0 = P4 + (P0 << 1);\n\t"                                                                                      \
    "R2 = W [P0] (X);\n\t"                                                                                          \
    "R2.L = R2.L - R6.H (S)" MLOAD ";\n\t"                                                                          \
    "R2.L = R2.L * R5.L;\n\t"                                                                                       \
    "R6.H = R6.H + R2.L (S);\n\t"
#define DRIVE_RING                                      /* s x 2 ((1 - rw) / 2 + rw m): rw R4.L, (1 - rw) / 2 R0.H */  \
    "R1.L = R4.L * R1.L;\n\t"                                                                                       \
    "R1.L = R1.L + R0.H (S);\n\t"                                                                                   \
    "R6.H = R6.H * R1.L;\n\t"                                                                                       \
    "R6 = R6 << 1 (V, S);\n\t"
#define DRIVE_CLIP                                      /* sat(s cg >> 10) */                                         \
    "A0 = R6.H * R4.H (IS);\n\t"                                                                                    \
    "A0 = A0 >>> 10;\n\t"                                                                                           \
    "R0.L = A0 (IS) || R6 = [I0++];\n\t"
#define DRIVE_NOCLIP                                                                                                 \
    "R0 = R6 >>> 16;\n\t"                                                                                           \
    "R6 = [I0++];\n\t"
/* the post gain (R3: Q12 << 16, + [P5 + 48] a sample) and the exciter, dry (without GRIT; with it pass_out does this
 * after the crush) */
#define DRIVE_GAIN                                                                                                   \
    "A0 = R0.L * R3.H (IS) || R2 = [%[c] + 48] || R1.L = W [I2++];\n\t"                                            \
    "A0 = A0 >>> 12;\n\t"                                                                                           \
    "R0.L = A0 (IS);\n\t"                                                                                           \
    "R0.L = R0.L + R1.L (S);\n\t"                                                                                   \
    "R3 = R3 + R2 (NS);\n"
#define DRIVE_TAIL(RN)                                                                                               \
    "2:\t" RN "\n\t"                                                                                                \
    "[%[c] + 0] = R7;\n\t[%[c] + 108] = R3;\n\t"
#define DRIVE_STORE_RN "MNOP || W [P1++] = R0 || R1.L = W [I1++];"
#define DRIVE_STORE "W [P1++] = R0;"
#endif

static void pass_drive(struct mt_voice *v, int n, const struct post *c, int32_t dgrn, int32_t dgp, int RN, int DRV,
                       int RING, int GAIN)
{
    int32_t g = v->grn, c0 = (32767 - g) >> 1, dg = dgrn, dc0 = -(dgrn >> 1);   /* the resonant noise's factor */
    const int32_t c0r = (32767 - c->rw) >> 1;                     /* the ring's */
#ifdef __bfin__
    struct outk k;
    k.grn = (int32_t)((uint16_t)c0 | (uint32_t)(uint16_t)g << 16);
    k.dgrn = (int32_t)((uint16_t)dc0 | (uint32_t)(uint16_t)dg << 16);
    k.c0r = (uint32_t)(uint16_t)c0r << 16;
    k.fwfg = (uint16_t)c->fw | (uint32_t)(uint16_t)c->fg << 16;
    k.rwcg = (uint16_t)c->rw | (uint32_t)(uint16_t)c->cg << 16;
    k.tab = (const uint32_t *)(const void *)mt_sin1k;
    k.acc = mt_acc;
    k.rn = mt_rn;
    k.ex = mt_ex;
    k.mo = mt_m;
    k.sb = mt_s;
    k.n = n;
    k.gp = v->gp;
    k.dgp = dgp;
    k.dgp2 = dgp;
#define DV(RNP, FOLD, RINGP, CLIPP, G, ST) __asm__ volatile(DRIVE_HEAD RNP FOLD RINGP CLIPP G DRIVE_TAIL(ST) OUT_ARGS)
#define DV2(RNP, FOLD, RINGP, CLIPP, ST) do { if (GAIN) DV(RNP, FOLD, RINGP, CLIPP, DRIVE_GAIN, ST); \
                                             else DV(RNP, FOLD, RINGP, CLIPP, "", ST); } while (0)
    if (RN && RING)
        DV2(DRIVE_RN, DRIVE_FOLD(" || R1.L = W [I3++]"), DRIVE_RING, DRIVE_CLIP, DRIVE_STORE_RN);
    else if (RN && DRV)
        DV2(DRIVE_RN, DRIVE_FOLD(""), "", DRIVE_CLIP, DRIVE_STORE_RN);
    else if (RN)
        DV2(DRIVE_RN, "", "", DRIVE_NOCLIP, DRIVE_STORE_RN);
    else if (RING)
        DV2("", DRIVE_FOLD(" || R1.L = W [I3++]"), DRIVE_RING, DRIVE_CLIP, DRIVE_STORE);
    else if (DRV)
        DV2("", DRIVE_FOLD(""), "", DRIVE_CLIP, DRIVE_STORE);
    else
        DV2("", "", "", DRIVE_NOCLIP, DRIVE_STORE);
#undef DV2
#undef DV
    if (GAIN)
        v->gp = k.gp;
#else
    int32_t gp = v->gp;
    for (int i = 0; i < n; i++) {
        int32_t s = mt_acc[i] >> 16;
        if (RN) {                           /* the modes AM-ed by the low-passed noise: s x 2 (c0 + g rn), c0 = (1 - g) / 2 */
            int32_t f = sat16(c0 + mulr(g, mt_rn[i]));
            s = sat16(mulr(s, f) << 1);
            c0 = sat16(c0 + dc0);
            g = sat16(g + dg);
        }
        if (DRV) {
            int32_t f = mt_sin1k[((uint32_t)(s * c->fg) << 4) >> 22];   /* sin(pi / 2 x fg s) */
            s = sat16(s + mulr(sat16(f - s), c->fw));
            if (RING) {                     /* ring mod with the operator: s x 2 ((1 - rw) / 2 + rw m) */
                int32_t f2 = sat16(c0r + mulr(c->rw, mt_m[i]));
                s = sat16(mulr(s, f2) << 1);
            }
            s = sat16((s * c->cg) >> 10);                           /* the clip */
        }
        if (GAIN) {                         /* the post gain, the exciter dry (without GRIT) */
            s = sat16(sat16((s * (gp >> 16)) >> 12) + mt_ex[i]);
            gp = (int32_t)((uint32_t)gp + (uint32_t)dgp);
        }
        mt_s[i] = (int16_t)s;
    }
    if (GAIN)
        v->gp = gp;
#endif
}

static void pass_out(struct mt_voice *v, int16_t *out, int n, const struct post *c, int32_t dgp, int GRIT, int SAT)
{
    const int32_t h = 1 << (15 - c->bi), m = -(h << 1);            /* crush: bits bi, step 2^(16 - bi), rounded */
    const int32_t bfq = c->bf >= 256 ? 32767 : c->bf << 7;
#ifdef __bfin__
    struct outk k;
    k.hadd = (uint16_t)h | (uint32_t)(uint16_t)(h >> 1) << 16;
    k.mask = (uint16_t)m | (uint32_t)(uint16_t)(m >> 1) << 16;
    k.bfq = bfq;
    k.shph = v->shph;
    k.hinc = c->hinc;
    k.shv = v->shv;
    k.lp = v->dcy;
    k.knee = KNEE_T;
    k.top = KNEE_T + 2 * (32767 - KNEE_T);
    k.one = 32767;
    k.sb = mt_s;
    k.ex = mt_ex;
    k.out = out;
    k.n = n;
    k.gp = v->gp;
    k.dgp = dgp;
#define OUT_HEAD                                                                                                     \
    "R7 = [%[c] + 52];\n\tR6 = [%[c] + 32];\n\tR5 = [%[c] + 36];\n\tR4 = [%[c] + 20];\n\tR3 = [%[c] + 24];\n\t"     \
    "R0 = [%[c] + 100];\n\tI0 = R0;\n\tP1 = [%[c] + 96];\n\tP2 = [%[c] + 104];\n\t"                                 \
    "R0 = [%[c] + 88];\n\tI2 = R0;\n\tR0 = [%[c] + 108];\n\tI1 = R0;\n\tR0 = [%[c] + 112];\n\tM1 = R0;\n\t"      \
    "R0.L = W [I0++];\n\t"                                                                                          \
    "LSETUP (1f, 2f) LC0 = P2;\n"                                                                                  \
    "1:\t"
#define OUT_GRIT \
    "R0 = PACK (R0.L, R0.L);\n\t" \
    "R0 = R0 +|+ R4 (S) || R1 = [%[c] + 28];\n\t"      /* y1 = sat(s + h), y2 = sat(s + h2); bfq */ \
    "R0 = R0 & R3;\n\t"                                 /* & m, & m2 */ \
    "R1.H = R0.H - R0.L (S);\n\t" \
    "R1.H = R1.H * R1.L;\n\t" \
    "R0.L = R0.L + R1.H (S) || R1 = [%[c] + 40];\n\t"  /* y1 + (y2 - y1) bfq; shv */ \
    "R6 = R6 + R5 (NS);\n\t"                            /* the hold (phase R6, step R5): a carry takes a sample */ \
    "CC = AC0;\n\t" \
    "R0 = R0.L (X);\n\t" \
    "IF CC R1 = R0;\n\t" \
    "[%[c] + 40] = R1;\n\t"
#define OUT_PLAIN "R0 = R0.L (X);\n\tR0 <<= 8;\n\t"

    if (GRIT && SAT)
        __asm__ volatile(OUT_HEAD OUT_GRIT OUT_GAIN OUT_DC_SAT("R0.L = W [I0++];") OUT_ARGS);
    else if (GRIT)
        __asm__ volatile(OUT_HEAD OUT_GRIT OUT_GAIN OUT_DC("R0.L = W [I0++];") OUT_ARGS);
    else if (SAT)
        __asm__ volatile(OUT_HEAD OUT_PLAIN OUT_DC_SAT("R0.L = W [I0++];") OUT_ARGS);
    else
        __asm__ volatile(OUT_HEAD OUT_PLAIN OUT_DC("R0.L = W [I0++];") OUT_ARGS);
    v->shph = k.shph;
    v->shv = k.shv;
    v->dcy = k.lp;
    if (GRIT)
        v->gp = (int32_t)((uint32_t)v->gp + (uint32_t)dgp * (uint32_t)n);
#else
    int32_t gp = v->gp;
    int32_t lp = v->dcy, shv = v->shv;
    uint32_t shph = v->shph;
    for (int i = 0; i < n; i++) {
        int32_t s = mt_s[i];
        if (GRIT) {
            int32_t y1 = sat16(s + h) & m, y2 = sat16(s + (h >> 1)) & (m >> 1);
            s = sat16(y1 + mulr(sat16(y2 - y1), bfq));
            uint32_t ph = shph + c->hinc;
            if (ph < shph)                                           /* a carry: take a new sample */
                shv = s;
            shph = ph;
            s = shv;
        }
        if (GRIT) {                         /* the post gain, the exciter dry (after the crush) */
            s = sat16(sat16((s * (gp >> 16)) >> 12) + mt_ex[i]);
            gp = (int32_t)((uint32_t)gp + (uint32_t)dgp);
        }
        int32_t y = (s << 8) - lp;                                   /* Q23, the DC blocker */
        lp += y >> 11;
        out[i] = (int16_t)(SAT ? sat16(y >> 8) : soft_knee(y >> 8));
    }
    v->gp = gp;
    v->dcy = lp;
    v->shv = shv;
    v->shph = shph;
#endif
}

struct blk {
    struct pre pr;
    struct post po;
    int32_t a1[8];                          /* the modes' gains at the run's end */
    int32_t sq, dgrn, dgp;
    int op, md, rn, drv, ring, grit, noise;
};

static void run(struct mt_voice *v, int16_t *out, int n, struct blk *b)
{
    if (b->noise) {
        pass_noise(v, n, &b->pr);
        mt_ex_zero = 0;
    } else if (!mt_ex_zero) {               /* the exciter has died out: no noise pass, mt_ex silent */
        uint32_t *z = (uint32_t *)(void *)mt_ex;
        for (int i = 0; i < BLOCK / 2; i++)
            z[i] = 0;
        mt_ex_zero = 1;
    }
    if (b->op)
        pass_op(v, n, &b->pr);
    if (b->md)
        build_md(v, n, &b->pr, b->op ? b->pr.fmd : 0);
    int32_t rc = (int32_t)mt_recip16[(n >> 4) - 1];
    int first = 1;
    for (int j = 0; j < MT_PAIRS; j++) {
        int16_t da[2], dq[2];
        int sq = 0, any = 0;
        for (int k = 0; k < 2; k++) {       /* the sine's and the square's gains: a1 x (1 - sq), a1 x sq */
            int32_t m = 2 * j + k, ay = (b->a1[m] * (32767 - b->sq)) >> 15, aq = (b->a1[m] * b->sq) >> 15;
            da[k] = (int16_t)(((ay - v->amp[m]) * (rc << 2) + 32768) >> 16);   /* per 4 samples */
            dq[k] = (int16_t)(((aq - v->ampq[m]) * (rc << 2) + 32768) >> 16);
            sq |= aq | v->ampq[m];
            any |= b->a1[m] | v->amp[m] | v->ampq[m];
        }
        if (!any)
            continue;                       /* (both silent: the oscillators keep their state, a phase skip) */
        pass_pair(v, j, n, da, dq, b->md, sq != 0, first);
        first = 0;                          /* (the gains end where their ramp took them: the next run's ramp
                                             * takes the rest, so a slope under 1 LSB per sample never steps) */
    }
    if (first)                              /* every pair silent */
        for (int i = 0; i < n; i++)
            mt_acc[i] = 0;
    if (!b->rn && !b->drv && !b->grit) {
        pass_clean(v, out, n, b->dgp);
    } else {
        int gn = !b->grit;                  /* the post gain in pass_drive, or after the crush in pass_out */
        if (b->ring)
            b->rn ? pass_drive(v, n, &b->po, b->dgrn, b->dgp, 1, 1, 1, gn) : pass_drive(v, n, &b->po, b->dgrn, b->dgp, 0, 1, 1, gn);
        else if (b->drv)
            b->rn ? pass_drive(v, n, &b->po, b->dgrn, b->dgp, 1, 1, 0, gn) : pass_drive(v, n, &b->po, b->dgrn, b->dgp, 0, 1, 0, gn);
        else
            b->rn ? pass_drive(v, n, &b->po, b->dgrn, b->dgp, 1, 0, 0, gn) : pass_drive(v, n, &b->po, b->dgrn, b->dgp, 0, 0, 0, gn);
        if (b->drv)                         /* driven: clipped already, a plain saturation */
            b->grit ? pass_out(v, out, n, &b->po, b->dgp, 1, 1) : pass_out(v, out, n, &b->po, b->dgp, 0, 1);
        else
            b->grit ? pass_out(v, out, n, &b->po, b->dgp, 1, 0) : pass_out(v, out, n, &b->po, b->dgp, 0, 0);
    }
}

/* the knob tables: 129 points (point 128 = 32767), interpolated */
#define LERP(t, w, k, j) ((int32_t)(t)[((k) >> 8) * (w) + (j)] \
                          + ((((int32_t)(t)[(((k) >> 8) + 1) * (w) + (j)] - (int32_t)(t)[((k) >> 8) * (w) + (j)]) \
                              * ((k) & 255)) >> 8))

void metal_render(struct tobj *o, int16_t *out, const int16_t *p)
{
    uint32_t t0 = cycles();
    unsigned vi = (unsigned)o->voice < MT_VOICES ? (unsigned)o->voice : 0;
    struct mt_voice *v = &metal_voice[vi];
    metal_diag.renders++;
    int fresh = v->trig != v->seen, from_silence = 0;
    int32_t att = 0;
    int32_t ks = mt_strike_pos(p[MT_STRIKE]), kd = mt_drive_pos(p[MT_DRIVE]), kg = mt_grit_pos(p[MT_GRIT]);
    int32_t kp = mt_pos(p[MT_SPREAD]);
    int32_t bend0 = LERP(mt_strike, MT_STRIKE_W, ks, 0);
    if (fresh) {
        v->choke = 0;
        if (!v->age || !v->aenv) {          /* from silence (a finished note's DC decay counts as silence) */
            from_silence = 1;
            for (int k = 0; k < 8; k++) {
                v->x[k] = OSC_A;            /* every mode starts at its zero crossing */
                v->y[k] = 0;
                v->amp[k] = v->ampq[k] = 0;
                v->eacc[k] = 0;
            }
            v->lp1 = v->lp2 = 0;
            v->oph = 0;
            v->m1 = v->m2 = v->gf = 0;
            v->grn = 0;
            v->shph = 0xFFFFFFFFu;
            v->shv = 0;
            v->rat = 0;
            if (!v->age)
                v->dcy = 0;                 /* a voice the slot init silenced mid-note left it loud */
            v->q = p[MT_PITCH] + bend0;
            v->oinc = inc_of(v->q + MT_OP_RATIO);
        }
        for (int k = 0; k < 8; k++)
            v->env[k] = ENV1;
        v->exenv = v->benv = ENV1;
        v->age = 1;                         /* before seen: active() never reads 0 for a starting note */
        v->seen = v->trig;
        if (!v->seed) {
            v->seed = 0x2545F491u + vi * 0x9E3779B9u;
            v->rseed = 0x6A09E667u + vi * 0x85EBCA6Bu;
        }
        /* attack: STRIKE's contact time plus its swell x NDECAY's T60, in samples; Q15 steps per block */
        int32_t sw = LERP(mt_strike, MT_STRIKE_W, ks, 4);
        uint32_t t60 = mt_time_10us[1][mt_pos(p[MT_NDECAY]) >> 7];     /* 0.1 ms units: x 8.82 samples */
        att = LERP(mt_strike, MT_STRIKE_W, ks, 3) + (int32_t)(((uint32_t)sw * ((t60 * 565u) >> 6)) >> 15);
        v->atkd = att > 8 ? recip_q22((uint32_t)att) : 32767;     /* 2^22 / att ~ 32767 x 128 / att */
        v->atk = from_silence ? 0 : 32767;  /* a retrigger: no slow attack, from the gains it has */
    }
    if (!v->age) {
        for (int i = 0; i < BLOCK; i++)
            out[i] = 0;
        v->level = 0;
        return;
    }
    if (!v->aenv && !fresh) {               /* over: the DC blocker's own decay, then the voice ends */
        int32_t lp = v->dcy;                /* (the input is 0: y = -lp, rounded so it reaches 0 from both sides) */
        for (int i = 0; i < BLOCK; i++) {
            out[i] = soft_knee(-lp >> 8);
            lp += (-lp + 1024) >> 11;
        }
        v->dcy = lp;
        v->level = 0;
        v->age = lp > -TAIL_END && lp < TAIL_END ? 0 : v->age + 1;
        return;
    }
    v->level = 0;

    /* ---- the exciter's envelope; RATTLE's impacts ---- */
    int32_t ex1 = env_step(v->exenv, v->choke ? CHOKE_DEC : mt_ex_dec[mt_pos(p[MT_NDECAY]) >> 7]);
    int32_t chop = LERP(mt_strike, MT_STRIKE_W, ks, 11), hold = LERP(mt_strike, MT_STRIKE_W, ks, 10);
    int32_t gate = 32767, imp = 0;
    if (chop) {
        uint32_t h = v->rseed = v->rseed * 1664525u + 1013904223u;
        int32_t pr = (((chop * (ex1 >> 15)) >> 15) * 11469) >> 15;     /* up to 0.35 per block */
        if ((int32_t)(h >> 17) < pr || fresh) {
            v->rat = 32767;                 /* an impact: a burst, and the modes struck again (0.25 .. 0.75) */
            imp = ((((int32_t)((h >> 3) & 0x3FFF) + 8192) * (ex1 >> 15)) >> 15) << 15;
        } else {
            v->rat = (v->rat * 11469) >> 15;
        }
        gate = 32767 - chop + ((chop * v->rat) >> 15);
    }

    /* ---- pitch: PITCH plus the BOOM drop (decaying with the exciter); e from the block's highest pitch, the rest
     * as a per-sample factor (md) ---- */
    v->benv = env_step(v->benv, exp2neg(LERP(mt_strike, MT_STRIKE_W, ks, 12)));    /* the drop's own decay */
    int32_t q0 = v->q, q1 = p[MT_PITCH] + ((bend0 * (v->benv >> 15)) >> 15);
    int32_t qh = q0 > q1 ? q0 : q1;
    int32_t md0 = (ratio_down(q0 - qh) - 32767) << 16, md1 = (ratio_down(q1 - qh) - 32767) << 16;

    /* ---- the modes: MATERIAL (morph) and SPREAD -> their log ratios and levels (cached by the two knobs); DECAY ->
     * their ring factors; the pitch and STRIKE -> e and the strike's brightness (each recomputed when its inputs
     * move) ---- */
    int32_t km = mt_pos(p[MT_MATERIAL]);
    int32_t keya = (km | kp << 16) + 1, keyb = (mt_pos(p[MT_DECAY]) >> 7 | (int32_t)v->choke << 8) + 1;
    int newa = keya != v->cka, newc = newa || qh != v->ckq || ks != v->cks;
    if (newa) {
        int mi = km >> 13 > 3 ? 3 : km >> 13;
        int32_t mf = km - (mi << 13);
        int32_t stretch = LERP(mt_spread, 2, kp, 0), dt = LERP(mt_spread, 2, kp, 1);
        v->alpha = mt_mat_alpha[mi] + (((mt_mat_alpha[mi + 1] - mt_mat_alpha[mi]) * mf) >> 13);
        for (int k = 0; k < 8; k++) {
            int32_t l = mt_mat_l[mi][k] + (((mt_mat_l[mi + 1][k] - mt_mat_l[mi][k]) * mf) >> 13);
            v->lk[k] = ((l * stretch) >> 12) + ((mt_detune[k] * dt) >> 15);
            v->ak[k] = (int16_t)(mt_mat_a[mi][k] + (((mt_mat_a[mi + 1][k] - mt_mat_a[mi][k]) * mf) >> 13));
        }
        v->cka = keya;
    }
    if (newa || keyb != v->ckb) {           /* the ring: 2^-(rate x ratio^alpha) per block (choke: 5 ms) */
        uint32_t rate = (uint32_t)mt_ring_rate[mt_pos(p[MT_DECAY]) >> 7] >> 8;   /* log2 per block, Q12 */
        for (int k = 0; k < 8; k++) {
            int32_t l = v->lk[k], up = ratio_up_q12(l > 0 ? (l * v->alpha) >> 12 : 0);
            v->fk[k] = v->choke ? CHOKE_DEC : exp2neg((int32_t)((rate * (uint32_t)up) >> 8));
        }
        v->ckb = keyb;
    }
    if (newc) {                             /* e (Q23 targets), the strike's brightness, its normalisation */
        int32_t fcq = LERP(mt_strike, MT_STRIKE_W, ks, 1) * 2, slope = LERP(mt_strike, MT_STRIKE_W, ks, 2);
        uint32_t n2 = 0;
        for (int k = 0; k < 8; k++) {
            int32_t a = v->ak[k], qk = qh + v->lk[k];
            if (qk >= MT_MODE_TOP) {        /* above 12 kHz: off (its e would near 1.0) */
                a = 0;
                qk = MT_MODE_TOP;
            } else if (qk > MT_MODE_FADE) {
                a = (a * (((MT_MODE_TOP - qk) * MT_MODE_FADE_MUL) >> 12)) >> 15;   /* x (TOP - q) / (TOP - FADE) */
            }
            if (qk > fcq)                   /* `slope` octaves of level per octave above the strike's corner */
                a = (a * (exp2neg((int32_t)((((((uint32_t)(qk - fcq) * 683u) >> 10) * (uint32_t)slope) >> 8))) >> 15))
                    >> 15;
            v->wk[k] = (int16_t)a;
            int32_t boost = LERP(mt_strike, MT_STRIKE_W, ks, 13);      /* HAMMER's tilt up */
            if (boost && v->lk[k] > 0) {
                a = (a * ratio_up_q12((v->lk[k] * boost) >> 12)) >> 12;
                a = a > 32767 ? 32767 : a;
                v->wk[k] = (int16_t)a;
            }
            n2 += (uint32_t)(a * a) >> 4;
            v->et[k] = e_q23(inc_of(qk));
        }
        v->g = rsqrt_gain(n2);
        v->ckq = qh;
        v->cks = ks;
    }
    int32_t hv = mt_qmul(ex1, hold << 16);                             /* SCRAPE / RATTLE hold the modes up */
    for (int k = 0; k < 8; k++) {
        int32_t ed = v->et[k] + v->eacc[k];                            /* e, its error diffused */
        int32_t ei = ed >> 8;
        ei = ei > 32767 ? 32767 : ei;
        v->eacc[k] = ed - (ei << 8);
        v->e[k] = (int16_t)ei;
        int32_t e1 = env_step(v->env[k], v->fk[k]);
        if (!v->choke) {
            e1 = e1 > hv ? e1 : hv;
            e1 = e1 > imp ? e1 : imp;
        }
        v->env[k] = e1;
    }
    int32_t gv = (v->g * (v->vel >> 1)) >> 14;                         /* Q14 */
    if (p[MT_PITCH] > MT_TRIM_Q) {                                     /* -3 dB per octave above 500 Hz, to -9 dB */
        uint32_t u = (((uint32_t)(p[MT_PITCH] - MT_TRIM_Q) * 683u) >> 7);  /* log2 Q16 x 1/2 */
        gv = (gv * (exp2neg((int32_t)(u > 98304u ? 98304u : u)) >> 15)) >> 15;
    }
    int32_t atk1 = v->atk + v->atkd;
    atk1 = atk1 > 32767 ? 32767 : atk1;

    struct blk b;
    int32_t amax = ex1, full[8];
    for (int k = 0; k < 8; k++) {
        int32_t a = (v->wk[k] * gv) >> 14;
        full[k] = a > 32767 ? 32767 : a;                               /* the strike's level, Q15 */
        b.a1[k] = (((full[k] * (v->env[k] >> 15)) >> 15) * atk1) >> 15;
        amax = v->env[k] > amax && full[k] > 64 ? v->env[k] : amax;
    }
    /* The note ends at -70 dB, or once every mode's gain is under the gains' ramp resolution (their 16-bit ramps step
     * every 4 samples: a gain rests within +-16 of its target, which DRIVE's clip gain would turn into a buzz) with the
     * exciter quiet. Its last block fades the modes and then the output to 0. */
    int32_t amx = 0;
    for (int k = 0; k < 8; k++)
        amx = b.a1[k] > amx ? b.a1[k] : amx;
    int last = !fresh && (amax < SILENT || (amx < 24 && ex1 < (1 << 30) >> 9));     /* (the exciter at -54 dB) */
    if (last)
        for (int k = 0; k < 8; k++)
            b.a1[k] = 0;

    /* ---- the exciter and the resonant noise ---- */
    int32_t exg = LERP(mt_strike, MT_STRIKE_W, ks, 5), rnw = LERP(mt_strike, MT_STRIKE_W, ks, 7);
    int32_t vel12 = v->vel >> 3;
    int32_t gex1 = (((((exg * (ex1 >> 15)) >> 15) * vel12) >> 12) * gate >> 15) << 16;   /* Q12 << 16 */
    int32_t grn1 = (((rnw * (ex1 >> 15)) >> 15) * gate) >> 15;                          /* Q15 */
    b.pr.c1 = LERP(mt_strike, MT_STRIKE_W, ks, 6);
    b.pr.c2 = LERP(mt_strike, MT_STRIKE_W, ks, 8);
    b.pr.rng = LERP(mt_strike, MT_STRIKE_W, ks, 9);

    /* ---- the drive and its operator ---- */
    b.po.fw = LERP(mt_drive, MT_DRIVE_W, kd, 0);
    b.po.fg = LERP(mt_drive, MT_DRIVE_W, kd, 1);
    b.sq = LERP(mt_drive, MT_DRIVE_W, kd, 2);
    b.po.cg = LERP(mt_drive, MT_DRIVE_W, kd, 3);
    b.po.rw = LERP(mt_drive, MT_DRIVE_W, kd, 4);
    int32_t beta = LERP(mt_drive, MT_DRIVE_W, kd, 5);
    b.pr.fmd = LERP(mt_drive, MT_DRIVE_W, kd, 6);
    b.po.lv = LERP(mt_drive, MT_DRIVE_W, kd, 7);
    int drv = b.po.fw || b.po.rw || b.sq || b.po.cg != 1024 || b.po.lv != 4096;
    b.op = b.po.rw || b.pr.fmd || beta;
    uint32_t oinc1 = inc_of(q1 + MT_OP_RATIO);
    int32_t gf1 = (beta * RAD) >> 9;
    b.md = md0 || md1 || b.pr.fmd;

    /* ---- the grit ---- */
    int32_t bits = LERP(mt_grit_bits, 1, kg, 0);
    uint32_t h0 = mt_grit_hold[kg >> 8], h1 = mt_grit_hold[(kg >> 8) + 1];
    b.po.hinc = h0 - (uint32_t)(((int32_t)((h0 - h1) >> 8) * (kg & 255)));
    int grit = bits < 4096 || b.po.hinc != 0xFFFFFFFFu;
    b.po.bi = bits >= 3840 ? 15 : bits >> 8;                          /* 15 bits and up: no crush (bf 256) */
    b.po.bf = bits >= 3840 ? 256 : bits & 255;

    /* ---- runs: a fresh note's attack (to the strike's full level over `split` samples), then the block ---- */
    b.grit = grit;
    if (!grit)                              /* the hold primed: GRIT's first sample takes a new value (no stale one) */
        v->shph = 0xFFFFFFFFu;
    b.drv = drv;
    b.ring = drv && b.po.rw;
    b.rn = grn1 || v->grn;
    b.noise = b.rn || gex1 || v->gex || fresh;

    /* The modes run at the strike's level: their gains x R / m (R the strike's loudest mode, m the loudest now, at least
     * R / 256), and the post gain m / R x DRIVE's level puts the envelope back after the drive and the grit, so a
     * decaying note keeps its drive and grit and its attack its punch (the shaper never squashes the transient into the
     * tail), and the post gain's 32 bits carry the decay (the modes' 16-bit gains don't stall). The last block keeps the
     * post gain and fades out at the end. */
    int32_t rmax = 64;
    for (int k = 0; k < 8; k++)
        rmax = full[k] > rmax ? full[k] : rmax;
    int32_t mlev = amx > rmax >> 8 ? amx : rmax >> 8;
    mlev = mlev < 1 ? 1 : mlev;
    int32_t scl = (rmax * (recip_q22((uint32_t)mlev) >> 4)) >> 6;       /* Q12: R / m */
    for (int k = 0; k < 8; k++)
        b.a1[k] = (b.a1[k] * scl) >> 12;                                  /* (a1 <= m: at most R) */
    int32_t gq = (int32_t)(((uint32_t)mlev * (uint32_t)recip_q22((uint32_t)rmax)) >> 7);   /* Q15: m / R */
    gq = gq > 32767 ? 32767 : gq;
    int32_t gp1 = last ? v->gp : ((b.po.lv * gq) >> 15) << 16;
    v->mdv = md0;
    int split = 0;
    if (fresh) {
        if (!from_silence)
            att = 32;                       /* a retrigger: from the gains it has */
        if (att <= 8) {                     /* a hammer: at once (each mode starts at its zero crossing) */
            for (int k = 0; k < 8; k++) {
                v->amp[k] = (int16_t)((full[k] * (32767 - b.sq)) >> 15);
                v->ampq[k] = (int16_t)((full[k] * b.sq) >> 15);
            }
        } else if (att < BLOCK - 16) {      /* (both runs at least 16 samples: an asm loop of 0 is no loop of none) */
            split = (att + 15) & ~15;
        }
        if (from_silence)                   /* the post gain: at once for a hammer, else the attack's ramp */
            v->gp = att <= 8 ? gp1 : 0;
        if (from_silence) {                 /* the exciter starts at its full level */
            v->gex = (((((exg * 32767) >> 15) * vel12) >> 12) * gate >> 15) << 16;
            v->grn = (rnw * gate) >> 15;
        }
    }
    b.pr.dgex = (gex1 - v->gex) >> 7;
    b.dgrn = (grn1 - v->grn + 64) >> 7;      /* per sample; the mix lands on grn1 at the block's end */
    b.pr.dmdv = (md1 - md0) >> 7;
    v->oinc = oinc1;                        /* the operator: this block's increment and feedback depth */
    v->gf = gf1;
    if (split) {                            /* to the strike's full level over `split` samples, then the block */
        struct blk b1 = b;
        int32_t gfull = b.po.lv << 16;
        for (int k = 0; k < 8; k++)
            b1.a1[k] = full[k];
        b1.dgp = (((gfull - v->gp) >> 12) * (int32_t)mt_recip16[(split >> 4) - 1]) >> 4;
        b.dgp = (((gp1 - gfull) >> 12) * (int32_t)mt_recip16[((BLOCK - split) >> 4) - 1]) >> 4;
        run(v, out, split, &b1);
        v->gp = gfull;
        run(v, out + split, BLOCK - split, &b);
    } else {
        b.dgp = (gp1 - v->gp) >> 7;
        run(v, out, BLOCK, &b);
    }
    v->gp = gp1;
    v->gex = gex1;                          /* land on the block's end values */
    v->grn = grn1;
    v->mdv = md1;
    v->oinc = oinc1;
    v->gf = gf1;
    v->q = q1;
    v->exenv = ex1;
    v->atk = atk1;

    /* the oscillators' amplitude back to OSC_A where it drifted (the operator's FM, e's steps, the rounding): x, y x
     * (1.5 - C / 2 OSC_C), one Newton step of 1 / sqrt held to +-3 %, when the invariant C is off by 1/64; every block
     * (half the modes) while md moves e, else every 8th */
    if (!fresh && (b.md || !(v->age & 7))) {
    for (int k = (int)(v->age & 1); k < 8; k += 2) {          /* (half the modes per block) */
        int32_t x = v->x[k], y = v->y[k], e = sat16(v->e[k] + mulr(v->e[k], md1 >> 16));
        int32_t c = ((x * x) >> 2) + ((y * y) >> 2) - ((((e * x) >> 15) * y) >> 2);    /* C / 4 */
        int32_t d = c - (OSC_C >> 2);
        if (d > (OSC_C >> 8) || d < -(OSC_C >> 8)) {
            int32_t s = 49152 - mt_qmul(c, 266041);                    /* Q15 */
            s = s < 31744 ? 31744 : s > 33792 ? 33792 : s;           /* at most 3 % a block: no step */
            v->x[k] = (int16_t)sat16((x * s) >> 15);
            v->y[k] = (int16_t)sat16((y * s) >> 15);
        }
    }
    }

    v->age++;
    v->aenv = amax;
    if (last) {                             /* the output faded out over the block: no step when it ends */
        for (int i = 0; i < BLOCK; i++)
            out[i] = (int16_t)((out[i] * (BLOCK - 1 - i)) >> 7);
    }
    if (last || amax < SILENT) {            /* silent: the DC blocker's decay next, or the voice ends */
        v->aenv = 0;
        if (v->dcy > -TAIL_END && v->dcy < TAIL_END)
            v->age = 0;
    }
    uint32_t dtc = cycles() - t0;
    metal_diag.cyc_max = dtc > metal_diag.cyc_max ? dtc : metal_diag.cyc_max;
}

/* key_params = &params[slot][key][0] (TE's per-key array); the key comes from TE's voice array */
void metal_note_on(struct tobj *o, const int16_t *key_params)
{
    unsigned vi = (unsigned)o->voice < MT_VOICES ? (unsigned)o->voice : 0;
    struct mt_voice *v = &metal_voice[vi];
    int32_t key = te_drum_key[vi], vel = te_drum_vel[vi];
    key = key < 0 ? 0 : key >= MT_KEYS ? MT_KEYS - 1 : key;
    (void)key_params;
    if (key >= MT_CHOKE_LO && key <= MT_CHOKE_HI)        /* the hats: cut the group's other voices (and this key's) */
        for (int i = 0; i < MT_VOICES; i++) {
            struct mt_voice *w = &metal_voice[i];
            if (i != (int)vi && w->age && w->key >= MT_CHOKE_LO && w->key <= MT_CHOKE_HI)
                w->choke = 1;
        }
    v->key = key;
    v->vel = vel < 0 ? 0 : vel;
    v->trig++;
    metal_diag.noteons++;
    metal_diag.last_voice = (int32_t)vi;
}

int32_t metal_active(struct tobj *o)
{
    unsigned vi = (unsigned)o->voice < MT_VOICES ? (unsigned)o->voice : 0;
    const struct mt_voice *v = &metal_voice[vi];
    return v->trig != v->seen ? 1 : v->age && !v->aenv ? 0x40000000 : (int32_t)v->age;
}

void metal_note_off(struct tobj *o, const int16_t *key_params)
{
    (void)o;
    (void)key_params;
}

void metal_slot_init(int32_t slot)
{
    (void)slot;
    metal_diag.inits++;
    for (int i = 0; i < MT_VOICES; i++) {
        metal_voice[i].seen = metal_voice[i].trig;
        metal_voice[i].age = 0;
        metal_voice[i].cka = 0;             /* the caches: recomputed at the next block */
    }
    const int16_t *r9 = ranges + 9 * 32;
    int16_t *rn = ranges + metal_id * 32;
    for (int i = 0; i < 8; i++)
        if (r9[4 * i] >= r9[4 * i + 1])
            return;
    for (int i = 0; i < 32; i++)
        rn[i] = r9[i];
    metal_diag.row_copied = 1;
}
