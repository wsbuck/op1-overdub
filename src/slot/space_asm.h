/* The space FX's hot loops in Blackfin assembly (included by space.c on the device; SPACE_C builds the C reference
 * instead). Each kernel computes exactly what its C reference in space.c does (space_check.py runs both in the
 * simulator and compares them sample for sample):
 *   - integer multiply-accumulates, or fractional ones read out of the accumulator with rounding; those clear ASTAT's
 *     RND_MOD (unbiased rounding: ties to even, as the C reference; the simulator's too) and restore ASTAT after;
 *   - circular buffers (I/B/L) for every ring: no index arithmetic in the loops (a P register written by the ALU and
 *     then used as an address stalls), and L = 0 again before each asm ends;
 *   - loads and stores issued in parallel with the arithmetic where they fit.
 * Parameters go in a struct the asm reads. */
#ifndef SPACE_ASM_H
#define SPACE_ASM_H

#define PK2(lo, hi) ((uint32_t)(uint16_t)(lo) | ((uint32_t)(uint16_t)(hi) << 16))

/* rnd16 for a 32-bit state, without 64-bit arithmetic (the wrappers run per call) */
static inline int32_t rnd16_32(int32_t l)
{
    int32_t r = (l >> 16) + (((l & 0xFFFF) + ((l >> 16) & 1) + 0x7FFF) >> 16);
    return r > 32767 ? 32767 : r;
}
#define RND_ON "R0 = ASTAT;\n\t[--SP] = R0;\n\tBITCLR (R0, 8);\n\tASTAT = R0;\n\t"
#define RND_OFF "R0 = [SP++];\n\tASTAT = R0;\n\t"

/* ---- FDN stage 1 (k_stage1), one segment: the read's whole-sample index advances by exactly 1 per sample ---- */
struct k1p {
    int32_t *F;                 /* 0 (advanced) */
    const int16_t *ra, *rb;     /* 4, 8: the read pointers (rb = ra + 1 sample, interp only) */
    const int16_t *ring;        /* 12 */
    uint32_t rbytes;            /* 16 */
    int16_t *lrd, *lwr, *lap;   /* 20, 24, 28 (advanced): the in-line allpass ring: read, write, base */
    uint32_t lbytes;            /* 32 */
    uint32_t frac, st;          /* 36, 40: the fraction now (Q16) and its step per sample (it falls by st) */
    uint32_t ghp;               /* 44: 2 gh | gh << 16 */
    uint32_t alh;               /* 48: lh | a << 16 (advanced) */
    uint32_t gd2;               /* 52: 2 gd << 16 */
    int32_t lw, lx;             /* 56, 60: the LP state, A0.W and A0.X (advanced) */
    int32_t n;                  /* 64 */
};

#define K1_SETUP                                                                                                   \
    RND_ON                                                                                                         \
    "R3 = [%[k] + 56];\n\tA0.W = R3;\n\t"                                                                          \
    "R3 = [%[k] + 60];\n\tA0.X = R3.L;\n\t"                                                                        \
    "R0 = [%[k] + 36];\n\t"                                                                                        \
    "R1 = [%[k] + 40];\n\t"                                                                                        \
    "R2 = [%[k] + 52];\n\t"                                                                                        \
    "R5 = [%[k] + 44];\n\t"                                                                                        \
    "R6 = [%[k] + 48];\n\t"                                                                                        \
    "P2 = [%[k] + 0];\n\t"                                                                                         \
    "R3 = [%[k] + 20];\n\tI0 = R3;\n\t"                                                                            \
    "R3 = [%[k] + 24];\n\tI1 = R3;\n\t"                                                                            \
    "R3 = [%[k] + 28];\n\tB0 = R3;\n\tB1 = R3;\n\t"                                                                \
    "R3 = [%[k] + 32];\n\tL0 = R3;\n\tL1 = R3;\n\t"                                                                \
    "R3 = [%[k] + 4];\n\tI2 = R3;\n\t"                                                                             \
    "R3 = [%[k] + 8];\n\tI3 = R3;\n\t"                                                                             \
    "R3 = [%[k] + 12];\n\tB2 = R3;\n\tB3 = R3;\n\t"                                                                \
    "R3 = [%[k] + 16];\n\tL2 = R3;\n\tL3 = R3;\n\t"                                                                \
    "P0 = [%[k] + 64];\n\t"                                                                                        \
    "LSETUP (1f, 2f) LC0 = P0;\n"

/* y (R3.L) and z (R7.L): the interpolated read (the fraction R0 falls by R1 per sample), or the plain one */
#define K1_READ_INTERP                                                                                             \
    "1:\tR4 = R0 >> 1 || R3.L = W [I2++] || R7.L = W [I3++];\n\t"                                                  \
    "R7.L = R7.L - R3.L (S);\n\t"                                                                                  \
    "R7 = R7.L * R4.L (IS);\n\t"                                                                                   \
    "R7 >>>= 14;\n\t"                                                                                              \
    "R7 += 1;\n\t"                                                                                                 \
    "R7 >>>= 1;\n\t"                                                                                               \
    "R3.L = R3.L + R7.L (NS);\n\t"                                                                                 \
    "R0 = R0 - R1 (NS) || R7.L = W [I0++];\n\t"
#define K1_READ_PLAIN "1:\tMNOP || R3.L = W [I2++] || R7.L = W [I0++];\n\t"

/* half = z / 2 requantised (R2.L; R2.H keeps 2 gd), v = y +- half saturated (R4.L) */
#define K1_HALF_TZ                                                                                                 \
    "R2.L = R7.L >>> 15;\n\t"                                                                                      \
    "R2.L = R7.L - R2.L (NS);\n\t"                                                                                 \
    "R2.L = R2.L >>> 1;\n\t"
#define K1_HALF_NE                                                                                                 \
    "R2.L = R7.L >>> 1;\n\t"                                                                                       \
    "R2.L = R2.L << 15;\n\t"                                                                                       \
    "R2.L = R2.L >> 15;\n\t"                                                                                       \
    "R2.L = R7.L + R2.L (S);\n\t"                                                                                  \
    "R2.L = R2.L >>> 1;\n\t"
/* store v; the LP (A0): += 2 a z -+ a v - 2 a lh (lh R6.L, a R6.H), lh = A0 rounded; F (A1) = 2 gh z -+ gh v +
 * 2 gd lh, (F >> 12 + 1) >> 1 */
#define K1_TAIL(PMV, MPA)                                                                                          \
    "R4.L = R3.L " PMV " R2.L (S);\n\t"                                                                            \
    "A0 += R7.L * R6.H || W [I1++] = R4.L;\n\t"                                                                    \
    "A0 " MPA " R4.L * R6.H (IS);\n\t"                                                                             \
    "A0 -= R6.L * R6.H;\n\t"                                                                                       \
    "R6.L = A0;\n\t"                                                                                               \
    "A1 = R7.L * R5.L (IS);\n\t"                                                                                   \
    "A1 " MPA " R4.L * R5.H (IS);\n\t"                                                                             \
    "R7 = (A1 += R6.L * R2.H) (IS);\n\t"                                                                           \
    "R7 >>>= 12;\n\t"                                                                                              \
    "R7 += 1;\n\t"                                                                                                 \
    "R7 >>>= 1;\n"                                                                                                 \
    "2:\t[P2++] = R7;\n\t"

#define K1_END                                                                                                     \
    "[%[k] + 0] = P2;\n\t"                                                                                         \
    "R3 = I0;\n\t[%[k] + 20] = R3;\n\t"                                                                            \
    "R3 = I1;\n\t[%[k] + 24] = R3;\n\t"                                                                            \
    "[%[k] + 36] = R0;\n\t"                                                                                        \
    "[%[k] + 48] = R6;\n\t"                                                                                        \
    "R3 = A0.W;\n\t[%[k] + 56] = R3;\n\t"                                                                          \
    "R3.L = A0.X;\n\tR3 = R3.L (X);\n\t[%[k] + 60] = R3;\n\t"                                                      \
    "R3 = 0;\n\tL0 = R3;\n\tL1 = R3;\n\tL2 = R3;\n\tL3 = R3;\n\t"                                                  \
    RND_OFF

#define K1_ASM(READ, HALF, PMV, MPA)                                                                               \
    __asm__ volatile(K1_SETUP READ HALF K1_TAIL(PMV, MPA) K1_END                                                   \
                     :                                                                                             \
                     : [k] "a"(&q)                                                                                 \
                     : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P0", "P2", "I0", "I1", "I2", "I3", "B0",   \
                       "B1", "B2", "B3", "L0", "L1", "L2", "L3", "A0", "A1", "LC0", "LT0", "LB0", "memory", "cc")

static __attribute__((noinline)) void k_stage1(int32_t *F, const int16_t *ring, uint32_t mask, uint32_t p,
                                               uint32_t inc, int interp, int16_t *lap, uint32_t lmask, uint32_t t,
                                               int32_t dly, int32_t sgn, int32_t *lo, int32_t a, int32_t gh,
                                               int32_t gd, int near, int n)
{
    struct k1p q;
    q.F = F;
    q.ring = ring;
    q.rbytes = (mask + 1) * 2;
    q.lap = lap;
    q.lrd = lap + ((t - (uint32_t)dly) & lmask);
    q.lwr = lap + (t & lmask);
    q.lbytes = (lmask + 1) * 2;
    q.ghp = PK2(2 * gh, gh);
    q.alh = PK2(rnd16_32(*lo), a);
    q.gd2 = PK2(0, 2 * gd);
    q.lw = *lo;
    q.lx = *lo < 0 ? -1 : 0;
    int32_t st = 65536 - (int32_t)inc;
    int k = (interp ? 4 : 0) | (sgn < 0 ? 2 : 0) | (near ? 1 : 0);
    for (int j0 = 0; j0 < n;) {
        uint32_t pj = p + (uint32_t)(j0 + 1) * inc, idx = (pj >> 16) & mask, fr = pj & 0xFFFF;
        uint32_t m = (uint32_t)(n - j0);
        if (interp && st > 0 && fr < (m - 1) * (uint32_t)st)
            m = sp_udiv(fr, (uint32_t)st) + 1;              /* the fraction would fall below 0: the index repeats */
        else if (interp && st < 0 && fr + (m - 1) * (uint32_t)-st > 65535)
            m = sp_udiv(65535 - fr, (uint32_t)-st) + 1;   /* it would pass 1: the index skips one */
        q.ra = ring + idx;
        q.rb = ring + ((idx + 1) & mask);
        q.frac = fr;
        q.st = (uint32_t)st;
        q.n = (int32_t)m;
        switch (k) {
        case 0: K1_ASM(K1_READ_PLAIN, K1_HALF_TZ, "+", "-="); break;
        case 1: K1_ASM(K1_READ_PLAIN, K1_HALF_NE, "+", "-="); break;
        case 2: K1_ASM(K1_READ_PLAIN, K1_HALF_TZ, "-", "+="); break;
        case 3: K1_ASM(K1_READ_PLAIN, K1_HALF_NE, "-", "+="); break;
        case 4: K1_ASM(K1_READ_INTERP, K1_HALF_TZ, "+", "-="); break;
        case 5: K1_ASM(K1_READ_INTERP, K1_HALF_NE, "+", "-="); break;
        case 6: K1_ASM(K1_READ_INTERP, K1_HALF_TZ, "-", "+="); break;
        default: K1_ASM(K1_READ_INTERP, K1_HALF_NE, "-", "+="); break;
        }
        j0 += (int)m;
    }
    *lo = q.lx == (q.lw >> 31) ? q.lw : q.lx < 0 ? (int32_t)0x80000000 : 0x7FFFFFFF;   /* A0 saturated to 32 bits */
}

/* ---- the input / ER allpass (k_ap) ---- */
struct kap {
    int16_t *x;                 /* 0 */
    int16_t *rd, *wr, *ring;    /* 4, 8, 12 */
    uint32_t bytes;             /* 16 */
    uint32_t gg;                /* 20: g | g2c << 16 */
    int32_t n;                  /* 24 */
};

static __attribute__((noinline)) void k_ap(int16_t *x, int16_t *ring, uint32_t mask, uint32_t t, int32_t dly,
                                           int32_t g, int32_t g2c, int n)
{
    struct kap q = { x, ring + ((t - (uint32_t)dly) & mask), ring + (t & mask), ring, (mask + 1) * 2, PK2(g, g2c), n };
    __asm__ volatile(
        RND_ON
        "R0 = [%[k] + 4];\n\tI0 = R0;\n\t"
        "R0 = [%[k] + 8];\n\tI1 = R0;\n\t"
        "R0 = [%[k] + 12];\n\tB0 = R0;\n\tB1 = R0;\n\t"
        "R0 = [%[k] + 16];\n\tL0 = R0;\n\tL1 = R0;\n\t"
        "R0 = [%[k] + 0];\n\tI2 = R0;\n\t"
        "R5 = [%[k] + 20];\n\t"
        "P0 = [%[k] + 24];\n\t"
        "LSETUP (1f, 2f) LC0 = P0;\n"
        "1:\tMNOP || R0.L = W [I0++] || R1.L = W [I2];\n\t"   /* z, x */
        "R2 = R0.L * R5.L (IS);\n\t"                /* z g */
        "R3 = R0.L (X);\n\t"
        "R3 >>>= 31;\n\t"
        "R3 >>= 17;\n\t"                            /* 0x7FFF when z < 0: toward zero */
        "R2 = R2 + R3;\n\t"
        "R2 >>>= 15;\n\t"
        "R2.L = R1.L + R2.L (S);\n\t"               /* v */
        "A0 = R0.L * R5.H || W [I1++] = R2.L;\n\t"  /* z (1 - g^2) */
        "R4.L = (A0 -= R1.L * R5.L);\n"             /* - x g, rounded */
        "2:\tW [I2++] = R4.L;\n\t"
        "R0 = 0;\n\tL0 = R0;\n\tL1 = R0;\n\t"
        RND_OFF
        :
        : [k] "a"(&q)
        : "R0", "R1", "R2", "R3", "R4", "R5", "P0", "I0", "I1", "I2", "B0", "B1", "L0", "L1", "A0", "LC0", "LT0",
          "LB0", "memory", "cc");
}

/* ---- the send (k_send) ---- */
struct ksd {
    int16_t *u;                 /* 0 */
    int16_t *rd;                /* 4 */
    const int16_t *ring;        /* 8 */
    uint32_t bytes;             /* 12 */
    const int16_t *er;          /* 16 */
    uint32_t s, ds;             /* 20, 24: sd | se << 16 and their steps */
    int32_t n;                  /* 28 */
};

static __attribute__((noinline)) void k_send(int16_t *u, const int16_t *pd, uint32_t pmask, uint32_t dpos,
                                             const int16_t *er, int32_t sd, int32_t dsd, int32_t se, int32_t dse,
                                             int n)
{
    struct ksd q = { u, (int16_t *)pd + (dpos & pmask), pd, (pmask + 1) * 2, er, PK2(sd, se), PK2(dsd, dse), n };
    __asm__ volatile(
        RND_ON
        "R0 = [%[k] + 4];\n\tI0 = R0;\n\t"
        "R0 = [%[k] + 8];\n\tB0 = R0;\n\t"
        "R0 = [%[k] + 12];\n\tL0 = R0;\n\t"
        "R0 = [%[k] + 16];\n\tI1 = R0;\n\t"
        "R0 = [%[k] + 0];\n\tI2 = R0;\n\t"
        "R5 = [%[k] + 20];\n\t"
        "R6 = [%[k] + 24];\n\t"
        "P0 = [%[k] + 28];\n\t"
        "LSETUP (1f, 2f) LC0 = P0;\n"
        "1:\tR5 = R5 +|+ R6 || R0.L = W [I0++] || R1.L = W [I1++];\n\t"
        "A0 = R0.L * R5.L;\n\t"
        "R2.L = (A0 += R1.L * R5.H);\n"
        "2:\tW [I2++] = R2.L;\n\t"
        "R0 = 0;\n\tL0 = R0;\n\t"
        RND_OFF
        :
        : [k] "a"(&q)
        : "R0", "R1", "R2", "R5", "R6", "P0", "I0", "I1", "I2", "B0", "L0", "A0", "LC0", "LT0", "LB0", "memory",
          "cc");
}

/* ---- one-pole (k_pole) ---- */
struct kpl {
    int16_t *x;                 /* 0 */
    int32_t l;                  /* 4 (in / out, saturated) */
    uint32_t ah;                /* 8: h | a << 16 */
    int32_t n;                  /* 12 */
};

#define KPL_HEAD                                                                                                   \
    RND_ON                                                                                                         \
    "R0 = [%[k] + 0];\n\tI0 = R0;\n\tI1 = R0;\n\t"                                                                 \
    "R2 = [%[k] + 4];\n\t"                                                                                         \
    "A0 = R2;\n\t"                                                                                                 \
    "R6 = [%[k] + 8];\n\t"                                                                                         \
    "P0 = [%[k] + 12];\n\t"                                                                                        \
    "LSETUP (1f, 2f) LC0 = P0;\n"                                                                                  \
    "1:\tMNOP || R0.L = W [I0++];\n\t"                                                                             \
    "A0 += R0.L * R6.H;\n\t"                                                                                       \
    "A0 -= R6.L * R6.H;\n\t"                                                                                       \
    "R6.L = A0;\n\t"
#define KPL_TAIL                                                                                                   \
    "R2 = A0;\n\t"                                                                                                 \
    "[%[k] + 4] = R2;\n\t"                                                                                         \
    RND_OFF
#define KPL_CLOB "R0", "R2", "R6", "P0", "I0", "I1", "A0", "LC0", "LT0", "LB0", "memory", "cc"

static __attribute__((noinline)) void k_pole(int16_t *x, int32_t *st_, int32_t a, int hp, int n)
{
    struct kpl q = { x, *st_, PK2(rnd16_32(*st_), a), n };
    if (hp)
        __asm__ volatile(KPL_HEAD "R2.L = R0.L - R6.L (S);\n" "2:\tW [I1++] = R2.L;\n\t" KPL_TAIL
                         : : [k] "a"(&q) : KPL_CLOB);
    else
        __asm__ volatile(KPL_HEAD "NOP;\n" "2:\tW [I1++] = R6.L;\n\t" KPL_TAIL : : [k] "a"(&q) : KPL_CLOB);
    *st_ = q.l;
}

/* ---- the early reflections (k_taps): two passes of 4 circular reads ---- */
struct ktp {
    const int16_t *p[4];        /* 0..12: read pointers */
    const int16_t *ring;        /* 16 */
    uint32_t bytes;             /* 20 */
    uint32_t g01, g23;          /* 24, 28 */
    int32_t *acc;               /* 32 */
    int16_t *er;                /* 36 */
    int32_t n;                  /* 40 */
};

#define KTP_HEAD                                                                                                   \
    "R0 = [%[k] + 0];\n\tI0 = R0;\n\t"                                                                             \
    "R0 = [%[k] + 4];\n\tI1 = R0;\n\t"                                                                             \
    "R0 = [%[k] + 8];\n\tI2 = R0;\n\t"                                                                             \
    "R0 = [%[k] + 12];\n\tI3 = R0;\n\t"                                                                            \
    "R0 = [%[k] + 16];\n\tB0 = R0;\n\tB1 = R0;\n\tB2 = R0;\n\tB3 = R0;\n\t"                                        \
    "R0 = [%[k] + 20];\n\tL0 = R0;\n\tL1 = R0;\n\tL2 = R0;\n\tL3 = R0;\n\t"                                        \
    "R4 = [%[k] + 24];\n\t"                                                                                        \
    "R5 = [%[k] + 28];\n\t"                                                                                        \
    "P1 = [%[k] + 32];\n\t"                                                                                        \
    "P2 = [%[k] + 36];\n\t"                                                                                        \
    "P0 = [%[k] + 40];\n\t"                                                                                        \
    "LSETUP (1f, 2f) LC0 = P0;\n"                                                                                  \
    "1:\tMNOP || R0.L = W [I0++] || R1.L = W [I1++];\n\t"                                                          \
    "MNOP || R2.L = W [I2++] || R3.L = W [I3++];\n\t"
#define KTP_END "R0 = 0;\n\tL0 = R0;\n\tL1 = R0;\n\tL2 = R0;\n\tL3 = R0;\n"
#define KTP_CLOB "R0", "R1", "R2", "R3", "R4", "R5", "R6", "P0", "P1", "P2", "I0", "I1", "I2", "I3", "B0", "B1", "B2", \
    "B3", "L0", "L1", "L2", "L3", "A0", "LC0", "LT0", "LB0", "memory", "cc"

static __attribute__((noinline)) void k_taps(int16_t *er, const int16_t *ring, uint32_t mask, const uint32_t *pos,
                                             const int32_t *g, int n)
{
    struct ktp q;
    q.ring = ring;
    q.bytes = (mask + 1) * 2;
    q.acc = sc.acc;
    q.er = er;
    q.n = n;
    for (int k = 0; k < 4; k++)
        q.p[k] = ring + (pos[k] & mask);
    q.g01 = PK2(g[0], g[1]);
    q.g23 = PK2(g[2], g[3]);
    __asm__ volatile(KTP_HEAD
                     "A0 = R0.L * R4.L (IS);\n\t"
                     "A0 += R1.L * R4.H (IS);\n\t"
                     "A0 += R2.L * R5.L (IS);\n\t"
                     "R6 = (A0 += R3.L * R5.H) (IS);\n"
                     "2:\t[P1++] = R6;\n\t" KTP_END
                     : : [k] "a"(&q) : KTP_CLOB);
    for (int k = 0; k < 4; k++)
        q.p[k] = ring + (pos[k + 4] & mask);
    q.g01 = PK2(g[4], g[5]);
    q.g23 = PK2(g[6], g[7]);
    __asm__ volatile(KTP_HEAD
                     "R6 = [P1++];\n\t"
                     "A0 = R6;\n\t"
                     "A0 += R0.L * R4.L (IS);\n\t"
                     "A0 += R1.L * R4.H (IS);\n\t"
                     "A0 += R2.L * R5.L (IS);\n\t"
                     "R6 = (A0 += R3.L * R5.H) (IS);\n\t"
                     "R6 >>>= 15;\n\t"
                     "R6 = R6 << 16 (S);\n\t"
                     "R6 >>>= 16;\n"
                     "2:\tW [P2++] = R6;\n\t" KTP_END
                     : : [k] "a"(&q) : KTP_CLOB);
}

/* ---- stage 2: the Hadamard in place (k_wht) ---- */
static __attribute__((noinline)) void k_wht(int32_t F[NL][BLOCK], int32_t *latel, int32_t *later, int n)
{
    __asm__ volatile(
        "I0 = %[ll];\n\t"
        "I1 = %[lr];\n\t"
        "P1 = %[f];\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR0 = [P1];\n\t"
        "R1 = [P1 + 256];\n\t"
        "R2 = [P1 + 512];\n\t"
        "R3 = [P1 + 768];\n\t"
        "R4 = [P1 + 1024];\n\t"
        "R5 = [P1 + 1280];\n\t"
        "R6 = [P1 + 1536];\n\t"
        "R7 = [P1 + 1792];\n\t"
        "R0 = R0 + R1, R1 = R0 - R1;\n\t"
        "R2 = R2 + R3, R3 = R2 - R3;\n\t"
        "R4 = R4 + R5, R5 = R4 - R5;\n\t"
        "R6 = R6 + R7, R7 = R6 - R7;\n\t"
        "R0 = R0 + R2, R2 = R0 - R2;\n\t"
        "R1 = R1 + R3, R3 = R1 - R3;\n\t"
        "R4 = R4 + R6, R6 = R4 - R6;\n\t"
        "R5 = R5 + R7, R7 = R5 - R7;\n\t"
        "R0 = R0 + R4, R4 = R0 - R4;\n\t"
        "R1 = R1 + R5, R5 = R1 - R5;\n\t"
        "R2 = R2 + R6, R6 = R2 - R6;\n\t"
        "R3 = R3 + R7, R7 = R3 - R7;\n\t"
        "[P1] = R0;\n\t"
        "[P1 + 256] = R1;\n\t"
        "[P1 + 512] = R2;\n\t"
        "[P1 + 768] = R3;\n\t"
        "[P1 + 1024] = R4;\n\t"
        "[P1 + 1280] = R5;\n\t"
        "[P1 + 1536] = R6;\n\t"
        "[P1 + 1792] = R7;\n\t"
        "MNOP || [I0++] = R1 || NOP;\n\t"
        "MNOP || [I1++] = R2 || NOP;\n"
        "2:\tP1 += 4;\n"
        :
        : [f] "a"(F), [ll] "a"(latel), [lr] "a"(later), [n] "a"(n)
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P1", "I0", "I1", "LC0", "LT0", "LB0", "memory", "cc");
}

/* ---- the inputs for the writes (k_inj) ---- */
static __attribute__((noinline)) void k_inj(int32_t *P, int32_t *M, const int16_t *ul, const int16_t *ur, int n)
{
    __asm__ volatile(
        "I0 = %[ul];\n\t"
        "I1 = %[ur];\n\t"
        "R7 = 11585 (X);\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tMNOP || R0.L = W [I0++] || R1.L = W [I1++];\n\t"
        "R2 = R0.L * R7.L (IS);\n\t"
        "R3 = R1.L * R7.L (IS);\n\t"
        "R2 = R2 + R3, R3 = R2 - R3;\n\t"
        "R2 >>>= 12;\n\t"
        "R3 >>>= 12;\n\t"
        "[%[p]++] = R2;\n"
        "2:\t[%[m]++] = R3;\n"
        : [p] "+a"(P), [m] "+a"(M)
        : [ul] "a"(ul), [ur] "a"(ur), [n] "a"(n)
        : "R0", "R1", "R2", "R3", "R7", "I0", "I1", "LC0", "LT0", "LB0", "memory", "cc");
}

/* ---- stage 3: one line's writes (k_write), two samples per 32-bit store ---- */
#define KW_TZ(X)                                                                                                   \
    "R2 = " X " >>> 31;\n\t"                                                                                       \
    "R2 >>= 29;\n\t"                                                                                               \
    X " = " X " + R2;\n\t"
#define KW_NE(X)                                                                                                   \
    "R2 = " X " >>> 3;\n\t"                                                                                        \
    "R2 <<= 31;\n\t"                                                                                               \
    "R2 >>= 31;\n\t"                                                                                               \
    X " = " X " + R2;\n\t"                                                                                         \
    X " += 3;\n\t"
#define KW_ASM(PM, RQ0, RQ3)                                                                                       \
    __asm__ volatile(                                                                                              \
        "I0 = %[d];\n\t"                                                                                           \
        "I1 = %[inj];\n\t"                                                                                         \
        "P1 = %[row];\n\t"                                                                                         \
        "LSETUP (1f, 2f) LC0 = %[n2];\n"                                                                           \
        "1:\tMNOP || R0 = [P1++] || R1 = [I1++];\n\t"                                                              \
        "R0 = R0 " PM " R1;\n\t"                                                                                   \
        RQ0                                                                                                        \
        "R0 = R0 << 13 (S) || R3 = [P1++] || R1 = [I1++];\n\t"                                                     \
        "R3 = R3 " PM " R1;\n\t"                                                                                   \
        RQ3                                                                                                        \
        "R3 = R3 << 13 (S);\n\t"                                                                                   \
        "R0 = PACK (R3.H, R0.H);\n"                                                                                \
        "2:\t[I0++] = R0;\n"                                                                                       \
        :                                                                                                          \
        : [d] "a"(d), [row] "a"(row), [inj] "a"(inj), [n2] "a"(n >> 1)                                             \
        : "R0", "R1", "R2", "R3", "P1", "I0", "I1", "LC0", "LT0", "LB0", "memory", "cc")

static __attribute__((noinline)) void k_write(int16_t *ln, uint32_t mask, uint32_t t, const int32_t *row,
                                              const int32_t *inj, int32_t sgn, int near, int n)
{
    int16_t *d = ln + (t & mask);
    if (sgn > 0 && !near)
        KW_ASM("+", KW_TZ("R0"), KW_TZ("R3"));
    else if (sgn > 0)
        KW_ASM("+", KW_NE("R0"), KW_NE("R3"));
    else if (!near)
        KW_ASM("-", KW_TZ("R0"), KW_TZ("R3"));
    else
        KW_ASM("-", KW_NE("R0"), KW_NE("R3"));
    ln[mask + 1] = ln[0];
}

/* ---- the wet and the tank level (k_wet) ---- */
static __attribute__((noinline)) int32_t k_wet(int16_t *wet, const int16_t *er, const int32_t *late, int32_t erlev,
                                               int n)
{
    int32_t sum;
    __asm__ volatile(
        "I0 = %[er];\n\t"
        "I1 = %[lt];\n\t"
        "I2 = %[w];\n\t"
        "R7 = %[lev];\n\t"
        "R6 = 0;\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tMNOP || R0.L = W [I0++] || R1 = [I1++];\n\t"
        "R2 = R0.L * R7.L (IS);\n\t"
        "R2 >>>= 15;\n\t"
        "R3 = R1 >>> 2;\n\t"
        "R2 = R2 + R3;\n\t"
        "R1 = ABS R1;\n\t"
        "R6 = R6 + R1;\n\t"
        "R2 = R2 << 16 (S);\n\t"
        "R2 >>>= 16;\n"
        "2:\tW [I2++] = R2.L;\n\t"
        "%[s] = R6;\n"
        : [s] "=d"(sum)
        : [er] "a"(er), [lt] "a"(late), [w] "a"(wet), [lev] "d"(erlev), [n] "a"(n)
        : "R0", "R1", "R2", "R3", "R6", "R7", "I0", "I1", "I2", "LC0", "LT0", "LB0", "memory", "cc");
    return sum;
}

/* ---- dry and wet (k_mix) ---- */
struct kmx {
    int16_t *x;                 /* 0 */
    const int16_t *y;           /* 4 */
    uint32_t g, dg;             /* 8, 12: g | w << 16 and their steps */
    int32_t n;                  /* 16 */
};

static __attribute__((noinline)) void k_mix(int16_t *x, const int16_t *y, int32_t g, int32_t dg, int32_t w, int32_t dw,
                                            int n)
{
    struct kmx q = { x, y, PK2(g, w), PK2(dg, dw), n };
    __asm__ volatile(
        RND_ON
        "R0 = [%[k] + 0];\n\tI0 = R0;\n\tI1 = R0;\n\t"
        "R0 = [%[k] + 4];\n\tI2 = R0;\n\t"
        "R5 = [%[k] + 8];\n\t"
        "R6 = [%[k] + 12];\n\t"
        "P0 = [%[k] + 16];\n\t"
        "LSETUP (1f, 2f) LC0 = P0;\n"
        "R7.H = 0x5A82;\n\tR7.L = 0x7999;\n\t"     /* the knee, -3 dBFS (Q31) */
        "R4 = 0;\n\t"
        "1:\tR5 = R5 +|+ R6 || R0.L = W [I0++] || R1.L = W [I2++];\n\t"
        "A0 = R0.L * R5.L;\n\t"
        "R2 = (A0 += R1.L * R5.H);\n\t"             /* 2 (x g + y w), saturated */
        "R3 = ABS R2;\n\t"
        "R1 = R3 - R7;\n\t"
        "R1 = MAX (R1, R4);\n\t"                     /* above the knee */
        "R0 = R1 >>> 2;\n\t"
        "R1 = R1 - R0;\n\t"
        "R3 = R3 - R1;\n\t"                          /* 4:1 */
        "CC = R2 < 0;\n\t"
        "R0 = -R3;\n\t"
        "IF CC R3 = R0;\n\t"
        "R3 >>>= 15;\n\t"
        "R3 += 1;\n\t"
        "R3 >>>= 1;\n"
        "2:\tW [I1++] = R3.L;\n\t"
        RND_OFF
        :
        : [k] "a"(&q)
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P0", "I0", "I1", "I2", "A0", "LC0", "LT0", "LB0", "memory",
          "cc");
}

/* ---- a shimmer head (k_head): the read steps r whole samples per sample, its fraction fixed; the window x gain
 * steps linearly ---- */
struct khd {
    int32_t *acc;               /* 0 */
    const int16_t *ra, *rb;     /* 4, 8 */
    const int16_t *ring;        /* 12 */
    uint32_t bytes;             /* 16 */
    uint32_t step;              /* 20: r x 2 bytes */
    uint32_t f;                 /* 24: the fraction, Q15 */
    uint32_t wd;                /* 28: w | dw << 16 */
    int32_t n;                  /* 32 */
};

static __attribute__((noinline)) void k_head(int32_t *acc, const int16_t *ring, uint32_t mask, uint32_t p,
                                             uint32_t inc, int32_t w, int32_t dw, int n)
{
    uint32_t p1 = p + inc, i = (p1 >> 16) & mask;
    struct khd q = { acc, ring + i, ring + ((i + 1) & mask), ring, (mask + 1) * 2, (inc >> 16) * 2,
                     (p1 & 0xFFFF) >> 1, PK2(w, dw), n };
    __asm__ volatile(
        "R0 = [%[k] + 0];\n\tI0 = R0;\n\t"
        "R0 = [%[k] + 4];\n\tI2 = R0;\n\t"
        "R0 = [%[k] + 8];\n\tI3 = R0;\n\t"
        "R0 = [%[k] + 12];\n\tB2 = R0;\n\tB3 = R0;\n\t"
        "R0 = [%[k] + 16];\n\tL2 = R0;\n\tL3 = R0;\n\t"
        "R0 = [%[k] + 20];\n\tM2 = R0;\n\t"
        "R2 = [%[k] + 24];\n\t"
        "R5 = [%[k] + 28];\n\t"
        "R6 = R5 >> 16;\n\t"                        /* dw in R6.L */
        "P0 = [%[k] + 32];\n\t"
        "LSETUP (1f, 2f) LC0 = P0;\n"
        "1:\tMNOP || R3.L = W [I2] || R7.L = W [I3];\n\t"
        "MNOP || I2 += M2 || NOP;\n\t"
        "MNOP || I3 += M2 || NOP;\n\t"
        "R7.L = R7.L - R3.L (S);\n\t"
        "R7 = R7.L * R2.L (IS);\n\t"
        "R7 >>>= 14;\n\t"
        "R7 += 1;\n\t"
        "R7 >>>= 1;\n\t"
        "R3.L = R3.L + R7.L (NS);\n\t"              /* y */
        "R3 = R3.L * R5.L (IS);\n\t"                /* y w */
        "R3 >>>= 15;\n\t"
        "R5.L = R5.L + R6.L (NS) || R0 = [I0] || NOP;\n\t"
        "R0 = R0 + R3;\n"
        "2:\t[I0++] = R0;\n\t"
        "R0 = 0;\n\tL2 = R0;\n\tL3 = R0;\n"
        :
        : [k] "a"(&q)
        : "R0", "R2", "R3", "R5", "R6", "R7", "P0", "I0", "I2", "I3", "B2", "B3", "L2", "L3", "M2", "LC0", "LT0",
          "LB0", "memory", "cc");
}

/* ---- the half-band decimator and interpolator (k_decim, k_interp) ---- */
/* the 8 tap pairs: A0 sums the backward taps (I0), A1 the forward ones (I1), coefficients hb[0..7] in R4..R7 (L, H);
 * then I0 / I1 move on to the next output (+18 / -14 bytes: M0 / M1). The two halves of R0 are loaded by separate
 * instructions (two parallel loads into one register lose one); a MAC reads its operands before a parallel load
 * writes. */
#define HB_TAP(C, D)                                                                                               \
    "A1 += R0.H * " C ", A0 += R0.L * " C " || R0.L = W [I0--] || NOP;\n\t"                                          \
    "MNOP || R0.H = W [I1++] || NOP;\n\t"
#define HB_TAPS                                                                                                    \
    "MNOP || R0.L = W [I0--] || NOP;\n\t"                                                                          \
    "MNOP || R0.H = W [I1++] || NOP;\n\t"                                                                          \
    HB_TAP("R4.L", 0) HB_TAP("R4.H", 1) HB_TAP("R5.L", 2) HB_TAP("R5.H", 3)                                        \
    HB_TAP("R6.L", 4) HB_TAP("R6.H", 5) HB_TAP("R7.L", 6)                                                          \
    "A1 += R0.H * R7.H, A0 += R0.L * R7.H || I0 += M0 || NOP;\n\t"                                                \
    "MNOP || I1 += M1 || NOP;\n\t"                                                                                 \
    "A0 += A1;\n\t"
struct khb {
    const int16_t *bk, *fw, *ct;   /* 0, 4, 8: the backward and forward taps' first samples, the centre / copy */
    int16_t *out;                  /* 12 */
};
#define HB_SETUP                                                                                                   \
    RND_ON                                                                                                         \
    "R4 = [%[h] + 0];\n\t"                                                                                         \
    "R5 = [%[h] + 4];\n\t"                                                                                         \
    "R6 = [%[h] + 8];\n\t"                                                                                         \
    "R7 = [%[h] + 12];\n\t"                                                                                        \
    "R0 = [%[k] + 0];\n\tI0 = R0;\n\t"                                                                             \
    "R0 = [%[k] + 4];\n\tI1 = R0;\n\t"                                                                             \
    "R0 = [%[k] + 12];\n\tI2 = R0;\n\t"                                                                            \
    "P1 = [%[k] + 8];\n\t"                                                                                         \
    "R0 = 18;\n\tM0 = R0;\n\t"                                                                                     \
    "R0 = -14;\n\tM1 = R0;\n\t"
#define HB_CLOB "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P1", "I0", "I1", "I2", "M0", "M1", "A0", "A1", "LC0", \
    "LT0", "LB0", "memory", "cc"

static __attribute__((noinline)) void k_decim(int16_t *out, const int16_t *bo, const int16_t *be)
{
    /* out[m] = (16384 be[m+8] + sum_j hb[j] (bo[m+7-j] + bo[m+8+j])) rounded >> 15 */
    struct khb q = { bo + 7, bo + 8, be + 8, out };
    __asm__ volatile(
        HB_SETUP
        "R3 = 16384 (X);\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR1 = W [P1++] (X);\n\t"
        "A0 = R1.L * R3.L;\n\t"
        "A1 = 0;\n\t"
        HB_TAPS
        "R2.L = A0;\n"
        "2:\tW [I2++] = R2.L;\n\t"
        RND_OFF
        :
        : [h] "a"(sp_hb), [k] "a"(&q), [n] "a"(BLOCK / 2)
        : HB_CLOB);
}

static __attribute__((noinline)) void k_interp(int16_t *out, const int16_t *c)
{
    /* out[2m] = sum_j hb[j] (c[m+8+j] + c[m+7-j]) x 2 rounded >> 15, out[2m+1] = c[m+8] */
    struct khb q = { c + 7, c + 8, c + 8, out };
    __asm__ volatile(
        HB_SETUP
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR1 = W [P1++] (X);\n\t"
        "A0 = 0;\n\t"
        "A1 = 0;\n\t"
        HB_TAPS
        "R2.L = A0 (S2RND);\n\t"
        "W [I2++] = R2.L;\n"
        "2:\tW [I2++] = R1.L;\n\t"
        RND_OFF
        :
        : [h] "a"(sp_hb), [k] "a"(&q), [n] "a"(BLOCK / 2)
        : HB_CLOB);
}

#endif
