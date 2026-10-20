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

/* ---- FDN stage 1 (k_stage1_all): per segment of a line, the read's whole-sample index advances by exactly 1 per sample ---- */
/* y (R3.L) and z (R7.L): the interpolated read (the fraction R0 falls by R1 per sample; the difference x the fraction
 * a fractional multiply, rounded to nearest even), or the plain one */
#define K1_READ_INTERP                                                                                             \
    "1:\tR4 = R0 >> 1 || R3.L = W [I2++] || R7.L = W [I3++];\n\t"                                                  \
    "R7.L = R7.L - R3.L (S);\n\t"                                                                                  \
    "R7.L = R7.L * R4.L;\n\t"                                                                                      \
    "R0 = R0 - R1 (NS);\n\t"                                                                                       \
    "R3.L = R3.L + R7.L (NS) || R7.L = W [I0++];\n\t"
#define K1_READ_PLAIN "1:\tMNOP || R3.L = W [I2++] || R7.L = W [I0++];\n\t"

/* half = z / 2 requantised (R2.L): toward zero, (z + (z < 0)) >> 1 by a vector add with the shift (R2.H is free
 * then; R4 too, free here); to nearest even, z x 0.5 (R2.H) a fractional multiply, rounded. v = y +- half saturated (R4.L) */
#define K1_HALF_TZ                                                                                                 \
    "R2.L = R7.L >> 15;\n\t"                                                                                       \
    "R2 = R7 +|+ R2, R4 = R7 -|- R2 (ASR);\n\t"
#define K1_HALF_NE "R2.L = R7.L * R2.H;\n\t"
/* store v; the LP (A0): += 2 a z -+ a v - 2 a lh (lh R6.L, a R5.H), lh = A0 rounded; F (A1) = 2 gh z -+ gh v (gh
 * R5.L) + 2 gd lh (2 gd R6.H), (F >> 12 + 1) >> 1, as (F + 4096) >> 13: A1 starts each sample at 4096 (64 x 64, R3.H;
 * |F| < 5 x 11585 x 32768 < 2^31 - 4096, so no saturation to tell the two apart). The z and v terms of both a dual
 * multiply-accumulate each (fractional: 2 gh z, 2 a z; integer: gh v, a v) */
#define K1_TAIL(PMV, MPA)                                                                                          \
    "A1 += R7.L * R5.L, A0 += R7.L * R5.H;\n\t"                                                                    \
    "R4.L = R3.L " PMV " R2.L (S);\n\t"                                                                            \
    "A1 " MPA " R4.L * R5.L, A0 " MPA " R4.L * R5.H (IS) || W [I1++] = R4.L;\n\t"                                  \
    "A0 -= R6.L * R5.H;\n\t"                                                                                       \
    "R6.L = A0;\n\t"                                                                                               \
    "R7 = (A1 += R6.L * R6.H) (IS);\n\t"                                                                           \
    "R7 >>>= 13;\n"                                                                                                \
    "2:\tA1 = R3.H * R3.H (IS) || [P2++] = R7 || NOP;\n\t"

/* the jobs: one segment of one line each, listed by variant; cont = the line's earlier segment just ran (its state is
 * still in the registers). A line's first job is its own, kept from block to block (the LP's 40-bit state lw / lx and
 * lh stay in it; *lo, saturated, is what the C reference keeps: lo_seen is it, or its complement when it saturated,
 * and the next block restarts from *lo whenever they differ) */
struct k1j {
    int32_t *F;                 /* 0 */
    const int16_t *ra, *rb;     /* 4, 8 */
    const int16_t *ring;        /* 12 */
    uint32_t rbytes;            /* 16 */
    int16_t *lrd, *lwr, *lap;   /* 20, 24, 28 */
    uint32_t lbytes;            /* 32 */
    uint32_t frac, st;          /* 36, 40 */
    uint32_t gha, lhd, pad;     /* 44, 48, 52: gh | a << 16, lh | 2 gd << 16 */
    int32_t lw, lx;             /* 56, 60 */
    int32_t n, cont;            /* 64, 68 */
    int32_t lo_seen;            /* 72 */
    int32_t *lo;                /* 76: written at the end, saturated (as the C reference keeps it) */
};

#define K1M_HEAD                                                                                                   \
    RND_ON                                                                                                         \
    "LSETUP (3f, 4f) LC1 = %[nj];\n"                                                                               \
    "3:\t%[k] = [%[l]++];\n\t"                                                                                     \
    "R3 = [%[k] + 68];\n\t"                                                                                        \
    "CC = R3 == 0;\n\t"                                                                                            \
    "IF !CC JUMP 5f;\n\t"                                                                                          \
    "R3 = [%[k] + 56];\n\tA0.W = R3;\n\t"                                                                          \
    "R3 = [%[k] + 60];\n\tA0.X = R3.L;\n\t"                                                                        \
    "R2 = 0x4000;\n\tR2 <<= 16;\n\t"                                                                                 \
    "R5 = [%[k] + 44];\n\t"                                                                                        \
    "R6 = [%[k] + 48];\n\t"                                                                                        \
    "P2 = [%[k] + 0];\n\t"                                                                                         \
    "R3 = [%[k] + 20];\n\tI0 = R3;\n\t"                                                                            \
    "R3 = [%[k] + 24];\n\tI1 = R3;\n\t"                                                                            \
    "R3 = [%[k] + 28];\n\tB0 = R3;\n\tB1 = R3;\n\t"                                                                \
    "R3 = [%[k] + 32];\n\tL0 = R3;\n\tL1 = R3;\n\t"                                                                \
    "R3 = [%[k] + 12];\n\tB2 = R3;\n\tB3 = R3;\n\t"                                                                \
    "R3 = [%[k] + 16];\n\tL2 = R3;\n\tL3 = R3;\n"                                                                  \
    "5:\tR0 = [%[k] + 36];\n\t"                                                                                    \
    "R1 = [%[k] + 40];\n\t"                                                                                        \
    "R3 = [%[k] + 4];\n\tI2 = R3;\n\t"                                                                             \
    "R3 = [%[k] + 8];\n\tI3 = R3;\n\t"                                                                             \
    "P0 = [%[k] + 64];\n\t"                                                                                        \
    "R3.H = 64;\n\t"                                                                                               \
    "A1 = R3.H * R3.H (IS);\n\t"                   /* F's rounding, 4096, preset */                          \
    "LSETUP (1f, 2f) LC0 = P0;\n"

#define K1M_TAIL                                                                                                   \
    "P0 = [%[k] + 76];\n\t"                                                                                        \
    "[%[k] + 0] = P2;\n\t"                                                                                         \
    "R3 = I0;\n\t[%[k] + 20] = R3;\n\t"                                                                            \
    "R3 = I1;\n\t[%[k] + 24] = R3;\n\t"                                                                            \
    "[%[k] + 48] = R6;\n\t"                                                                                        \
    "R3 = A0.W;\n\t[%[k] + 56] = R3;\n\t"                                                                          \
    "R1.L = A0.X;\n\tR1 = R1.L (X);\n\t[%[k] + 60] = R1;\n\t"                                                      \
    "R4 = A0;\n\t"                                 /* *lo: saturated (R2, R5, R6 carry over: cont) */        \
    "R3 >>>= 31;\n\t"                                                                                              \
    "CC = R3 == R1;\n\t"                           /* within 32 bits */                                       \
    "R0 = ~R4;\n\t"                                                                                                \
    "IF CC R0 = R4;\n\t"                           /* else lo_seen != *lo: restart from it */                 \
    "[%[k] + 72] = R0;\n"                                                                                         \
    "4:\t[P0] = R4;\n\t"                                                                                           \
    "R3 = 0;\n\tL0 = R3;\n\tL1 = R3;\n\tL2 = R3;\n\tL3 = R3;\n\t"                                                  \
    RND_OFF

#define K1M_ASM(READ, HALF, PMV, MPA)                                                                              \
    __asm__ volatile(K1M_HEAD READ HALF K1_TAIL(PMV, MPA) K1M_TAIL                                                 \
                     : [k] "=&a"(jp), [l] "+a"(lst)                                                                \
                     : [nj] "a"(nj)                                                                                \
                     : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P0", "P2", "I0", "I1", "I2", "I3", "B0",   \
                       "B1", "B2", "B3", "L0", "L1", "L2", "L3", "A0", "A1", "LC0", "LT0", "LB0", "LC1", "LT1",     \
                       "LB1", "memory", "cc")

static __attribute__((noinline)) void k1_group(struct k1j **lst, int nj, int v)
{
    struct k1j *jp;
    switch (v) {
    case 0: K1M_ASM(K1_READ_PLAIN, K1_HALF_TZ, "+", "-="); break;
    case 1: K1M_ASM(K1_READ_PLAIN, K1_HALF_NE, "+", "-="); break;
    case 2: K1M_ASM(K1_READ_PLAIN, K1_HALF_TZ, "-", "+="); break;
    case 3: K1M_ASM(K1_READ_PLAIN, K1_HALF_NE, "-", "+="); break;
    case 4: K1M_ASM(K1_READ_INTERP, K1_HALF_TZ, "+", "-="); break;
    case 5: K1M_ASM(K1_READ_INTERP, K1_HALF_NE, "+", "-="); break;
    case 6: K1M_ASM(K1_READ_INTERP, K1_HALF_TZ, "-", "+="); break;
    default: K1M_ASM(K1_READ_INTERP, K1_HALF_NE, "-", "+="); break;
    }
    (void)jp;
}

/* stage 1 for all 8 lines: their segments (the read's whole-sample index steps by exactly 1 inside one) as jobs,
 * listed by variant (read, sign): four calls a block, the rounding the same for all */
static __attribute__((noinline)) void k_stage1_all(const struct l1 *L, uint32_t mask, uint32_t lmask, uint32_t t,
                                                   int32_t a, int near, int n)
{
    struct k1j *lst[4][NL * 3], xj[NL * 2], *last[NL];     /* (<= 3 segments a line: the fraction moves < 2 samples a block) */
    int nj[4] = { 0, 0, 0, 0 }, nx = 0;
    /* each line's own job for the whole block, and its code: the variant (interp 2, sign 1), 4 if its fraction leaves
     * [0, 1) inside the block (segments) */
    struct { uint32_t mask, lmask, t, ah; int32_t n; uint32_t rbytes, lbytes; int32_t nm1; } kc = {
        mask, lmask, t, (uint32_t)a << 16, n, (mask + 1) * 2, (lmask + 1) * 2, n - 1 };
    uint8_t code[NL], *cp = code;
    __asm__ volatile(
        "LSETUP (1f, 4f) LC0 = %[nl];\n"
        "1:\tP1 = [%[L] + 44];\n\t"                 /* the job */
        "P2 = [%[L] + 32];\n\t"
        "R1 = [%[L] + 4];\n\t"                      /* ring */
        "R0 = [%[L] + 0];\n\t"
        "R2 = [P2];\n\t"                            /* *lo */
        "R3 = [P1 + 72];\n\t"
        "[P1 + 0] = R0;\n\t"
        "[P1 + 12] = R1;\n\t"
        "[P1 + 76] = P2;\n\t"
        "CC = R2 == R3;\n\t"
        "IF CC JUMP 8f (BP);\n\t"
        "[P1 + 56] = R2;\n\t"                       /* set from outside: lw, lx, lh = rnd16 */
        "R3 = R2 >>> 31;\n\t"
        "[P1 + 60] = R3;\n\t"
        "R3 = R2 >>> 16;\n\t"
        "R4 = R2.L (Z);\n\t"
        "R5 = 1;\n\tR5 = R5 & R3;\n\t"
        "R4 = R4 + R5;\n\t"
        "R5 = 0x7FFF (Z);\n\t"
        "R4 = R4 + R5;\n\t"
        "R4 >>= 16;\n\t"
        "R3 = R3 + R4;\n\t"
        "R3 = MIN (R3, R5);\n\t"
        "W [P1 + 48] = R3;\n"
        "8:\tR0 = [%[c] + 20];\n\t[P1 + 16] = R0;\n\t"   /* rbytes */
        "R0 = [%[c] + 24];\n\t[P1 + 32] = R0;\n\t"     /* lbytes */
        "R2 = [%[L] + 20];\n\t[P1 + 28] = R2;\n\t"     /* lap */
        "R3 = [%[L] + 24];\n\t"                     /* dly */
        "R0 = [%[c] + 8];\n\t"                      /* t */
        "R4 = [%[c] + 4];\n\t"                      /* lmask */
        "R3 = R0 - R3;\n\t"
        "R3 = R3 & R4;\n\t"
        "R3 <<= 1;\n\t"
        "R3 = R3 + R2;\n\t"
        "[P1 + 20] = R3;\n\t"                       /* lrd */
        "R0 = R0 & R4;\n\t"
        "R0 <<= 1;\n\t"
        "R0 = R0 + R2;\n\t"
        "[P1 + 24] = R0;\n\t"                       /* lwr */
        "R0 = W [%[L] + 36] (Z);\n\t"               /* gh */
        "R4 = [%[c] + 12];\n\t"
        "R0 = R0 | R4;\n\t"
        "[P1 + 44] = R0;\n\t"                       /* gh | a << 16 */
        "R0 = [%[L] + 40];\n\t"                     /* gd */
        "R0 <<= 17;\n\t"
        "R4 = W [P1 + 48] (Z);\n\t"
        "R0 = R0 | R4;\n\t"
        "[P1 + 48] = R0;\n\t"                       /* lh | 2 gd << 16 */
        "R0 = 0;\n\t[P1 + 68] = R0;\n\t"            /* cont */
        "R0 = [%[c] + 16];\n\t[P1 + 64] = R0;\n\t"  /* n */
        "R2 = [%[L] + 12];\n\t"                     /* inc */
        "R3 = [%[L] + 8];\n\t"                      /* p */
        "R3 = R3 + R2;\n\t"
        "R4 = [%[c] + 0];\n\t"                      /* mask */
        "R5 = R3 >> 16;\n\t"
        "R5 = R5 & R4;\n\t"
        "R0 = R5 << 1;\n\t"
        "R0 = R0 + R1;\n\t"
        "[P1 + 4] = R0;\n\t"                        /* ra */
        "R5 += 1;\n\t"
        "R5 = R5 & R4;\n\t"
        "R5 <<= 1;\n\t"
        "R5 = R5 + R1;\n\t"
        "[P1 + 8] = R5;\n\t"                        /* rb */
        "R3 = R3.L (Z);\n\t"
        "[P1 + 36] = R3;\n\t"                       /* frac */
        "R0 = 1;\n\tR0 <<= 16;\n\t"
        "R2 = R0 - R2;\n\t"
        "[P1 + 40] = R2;\n\t"                       /* st */
        "R6 = [%[L] + 16];\n\t"                     /* interp */
        "R7 = [%[L] + 28];\n\t"
        "R7 >>= 31;\n\t"                            /* sgn < 0 */
        "CC = R6 == 0;\n\t"
        "IF CC JUMP 9f;\n\t"
        "R7 += 2;\n\t"
        "CC = R2 == 0;\n\t"
        "IF CC JUMP 9f;\n\t"
        "R6 = ABS R2;\n\t"
        "R4 = [%[c] + 28];\n\t"
        "R6 = R6.L * R4.L (IS);\n\t"                /* (n - 1) |st| */
        "CC = R2 < 0;\n\t"
        "IF CC JUMP 6f;\n\t"
        "CC = R3 < R6 (IU);\n\t"                    /* falls below 0 */
        "JUMP 5f;\n"
        "6:\tR6 = R3 + R6;\n\t"
        "R4 = 0xFFFF (Z);\n\t"
        "CC = R4 < R6 (IU);\n"                      /* passes 1 */
        "5:\tR4 = CC;\n\t"
        "R4 <<= 2;\n\t"
        "R7 = R7 | R4;\n"
        "9:\tB [%[o]++] = R7;\n"
        "4:\t%[L] += 48;\n"
        : [L] "+a"(L), [o] "+a"(cp)
        : [c] "a"(&kc), [nl] "a"(NL)
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P1", "P2", "LC0", "LT0", "LB0", "memory", "cc");
    L -= NL;
    for (int i = 0; i < NL; i++) {
        const struct l1 *l = &L[i];
        struct k1j *q = (struct k1j *)(void *)l->job;
        int g = code[i] & 3;
        last[i] = q;
        if (!(code[i] & 4)) {
            lst[g][nj[g]++] = q;                    /* one segment (the usual case) */
            continue;
        }
        /* the fraction would fall below 0 (the index repeats) or pass 1 (it skips one) inside the block: segments */
        uint32_t inc = l->inc;
        int32_t st = 65536 - (int32_t)inc;
        for (int j0 = 0; j0 < n;) {
            uint32_t pj = l->p + (uint32_t)(j0 + 1) * inc, m = (uint32_t)(n - j0);
            uint32_t idx = (pj >> 16) & mask, fr = pj & 0xFFFF;
            uint32_t num = st > 0 ? fr : 65535 - fr, den = (uint32_t)(st > 0 ? st : -st);
            if (num < (m - 1) * den) {              /* m = num / den + 1 (< 64) */
                uint32_t qq = 0;
                for (uint32_t b = 32; b; b >>= 1)
                    if ((qq + b) * den <= num)
                        qq += b;
                m = qq + 1;
            }
            struct k1j *x = q;
            if (j0) {                               /* a further segment: the state carries over */
                x = last[i] = &xj[nx++];
                x->cont = 1;
                x->lo = l->lo;
            }
            x->ra = l->ring + idx;
            x->rb = l->ring + ((idx + 1) & mask);
            x->frac = fr;
            x->st = (uint32_t)st;
            x->n = (int32_t)m;
            lst[g][nj[g]++] = x;
            j0 += (int)m;
        }
    }
    for (int g = 0; g < 4; g++)
        if (nj[g])
            k1_group(lst[g], nj[g], (g << 1) | (near ? 1 : 0));
    if (nx)                                         /* the lines in segments: their state back from the last */
        for (int i = 0; i < NL; i++) {
            struct k1j *q = (struct k1j *)(void *)L[i].job, *x = last[i];
            if (x != q) {
                q->lw = x->lw;
                q->lx = x->lx;
                q->lhd = x->lhd;
                q->lo_seen = x->lo_seen;
            }
        }
}

/* ---- the input / ER allpasses (k_aps) ---- */
/* a chain of allpasses in place (k_aps), one call: per allpass a job (its ring's read and write pointers, base,
 * length), the gains the same for all */
struct kapj {
    int16_t *rd, *wr, *ring;    /* 0, 4, 8 */
    uint32_t bytes;             /* 12 */
};

static __attribute__((noinline)) void k_aps(int16_t *x, int16_t *const *ring, const uint32_t *mask, uint32_t t,
                                            const int32_t *dly, int32_t g, int32_t g2c, int na, int n)
{
    struct kapj jobs[4], *jp = jobs;
    for (int a = 0; a < na; a++)
        jobs[a] = (struct kapj){ ring[a] + ((t - (uint32_t)dly[a]) & mask[a]), ring[a] + (t & mask[a]), ring[a],
                                 (mask[a] + 1) * 2 };
    __asm__ volatile(
        RND_ON
        "R5 = %[gg];\n\t"
        "LSETUP (3f, 4f) LC1 = %[na];\n"
        "3:\tR0 = [%[k] + 0];\n\tI0 = R0;\n\t"
        "R0 = [%[k] + 4];\n\tI1 = R0;\n\t"
        "R0 = [%[k] + 8];\n\tB0 = R0;\n\tB1 = R0;\n\t"
        "R0 = [%[k] + 12];\n\tL0 = R0;\n\tL1 = R0;\n\t"
        "I2 = %[x];\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tMNOP || R0.L = W [I0++] || R1.L = W [I2];\n\t"   /* z, x */
        "R2 = R0.L * R5.L (IS);\n\t"                /* z g */
        "R3 = R2 >>> 31;\n\t"
        "R3 >>= 17;\n\t"                            /* 0x7FFF when z g < 0: toward zero */
        "R2 = R2 + R3;\n\t"
        "R2 >>>= 15;\n\t"
        "R2.L = R1.L + R2.L (S);\n\t"               /* v */
        "A0 = R0.L * R5.H || W [I1++] = R2.L;\n\t"  /* z (1 - g^2) */
        "R4.L = (A0 -= R1.L * R5.L);\n"             /* - x g, rounded */
        "2:\tW [I2++] = R4.L;\n"
        "4:\t%[k] += 16;\n\t"
        "R0 = 0;\n\tL0 = R0;\n\tL1 = R0;\n\t"
        RND_OFF
        : [k] "+a"(jp)
        : [gg] "d"(PK2(g, g2c)), [x] "a"(x), [na] "a"(na), [n] "a"(n)
        : "R0", "R1", "R2", "R3", "R4", "R5", "I0", "I1", "I2", "B0", "B1", "L0", "L1", "A0", "LC0", "LT0", "LB0",
          "LC1", "LT1", "LB1", "memory", "cc");
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

/* two one-poles in series (k_pole2): the first in A0 (h1 R6.L, a1 R6.H), the second in A1 (a2 R7.L, h2 R7.H) */
struct kp2 {
    int16_t *x;                 /* 0 */
    int32_t l1;                 /* 4 (in / out, saturated) */
    uint32_t ah1;               /* 8: h1 | a1 << 16 */
    int32_t l2;                 /* 12 (in / out, saturated) */
    uint32_t ah2;               /* 16: a2 | h2 << 16 */
    int32_t n;                  /* 20 */
};
#define KP2_HEAD                                                                                                   \
    RND_ON                                                                                                         \
    "R0 = [%[k] + 0];\n\tI0 = R0;\n\tI1 = R0;\n\t"                                                                 \
    "R2 = [%[k] + 4];\n\tA0 = R2;\n\t"                                                                             \
    "R2 = [%[k] + 12];\n\tA1 = R2;\n\t"                                                                            \
    "R6 = [%[k] + 8];\n\t"                                                                                         \
    "R7 = [%[k] + 16];\n\t"                                                                                        \
    "P0 = [%[k] + 20];\n\t"                                                                                        \
    "LSETUP (1f, 2f) LC0 = P0;\n"                                                                                  \
    "1:\tMNOP || R0.L = W [I0++];\n\t"                                                                             \
    "A0 += R0.L * R6.H;\n\t"                                                                                       \
    "A0 -= R6.L * R6.H;\n\t"                                                                                       \
    "R6.L = A0;\n\t"
#define KP2_MID                                                                                                    \
    "A1 += R1.L * R7.L;\n\t"                                                                                       \
    "A1 -= R7.H * R7.L;\n\t"                                                                                       \
    "R7.H = A1;\n\t"
#define KP2_TAIL                                                                                                   \
    "R2 = A0;\n\t"                                                                                                 \
    "[%[k] + 4] = R2;\n\t"                                                                                         \
    "R3 = A1;\n\t"                                                                                                 \
    "[%[k] + 12] = R3;\n\t"                                                                                        \
    RND_OFF
#define KP2_CLOB "R0", "R1", "R2", "R3", "R6", "R7", "P0", "I0", "I1", "A0", "A1", "LC0", "LT0", "LB0", "memory", "cc"

/* the first a high-pass; the second a high-pass (hp2) or a low-pass */
static __attribute__((noinline)) void k_pole2(int16_t *x, int32_t *s1, int32_t a1, int32_t *s2, int32_t a2, int hp2,
                                              int n)
{
    struct kp2 q = { x, *s1, PK2(rnd16_32(*s1), a1), *s2, PK2(a2, rnd16_32(*s2)), n };
    if (hp2)
        __asm__ volatile(KP2_HEAD "R1.L = R0.L - R6.L (S);\n\t" KP2_MID "R2.L = R1.L - R7.H (S);\n"
                         "2:\tW [I1++] = R2.L;\n\t" KP2_TAIL : : [k] "a"(&q) : KP2_CLOB);
    else
        __asm__ volatile(KP2_HEAD "R1.L = R0.L - R6.L (S);\n\t" KP2_MID "NOP;\n" "2:\tW [I1++] = R7.H;\n\t" KP2_TAIL
                         : : [k] "a"(&q) : KP2_CLOB);
    *s1 = q.l1;
    *s2 = q.l2;
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

/* ---- stage 2: the Hadamard in place (k_wht): per frame, rows 0-3 read and written through I0 / I2, rows 4-7 through
 * I1 / I3 (+256 bytes a row: M0; back to the next frame: M1), the butterflies issued with the loads and stores ---- */
static __attribute__((noinline)) void k_wht(int32_t F[NL][BLOCK], int32_t *latel, int32_t *later, int n)
{
    __asm__ volatile(
        "I0 = %[f];\n\t"
        "I2 = %[f];\n\t"
        "R0 = %[f];\n\t"
        "R1 = 1024 (X);\n\t"
        "R0 = R0 + R1;\n\t"
        "I1 = R0;\n\t"
        "I3 = R0;\n\t"
        "R0 = 256 (X);\n\tM0 = R0;\n\t"
        "R0 = -764 (X);\n\tM1 = R0;\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tMNOP || R0 = [I0++M0] || R4 = [I1++M0];\n\t"
        "MNOP || R1 = [I0++M0] || R5 = [I1++M0];\n\t"
        "R0 = R0 + R1, R1 = R0 - R1 || R2 = [I0++M0] || R6 = [I1++M0];\n\t"
        "R4 = R4 + R5, R5 = R4 - R5 || R3 = [I0++M1] || R7 = [I1++M1];\n\t"
        "R2 = R2 + R3, R3 = R2 - R3;\n\t"
        "R6 = R6 + R7, R7 = R6 - R7;\n\t"
        "R0 = R0 + R2, R2 = R0 - R2;\n\t"
        "R1 = R1 + R3, R3 = R1 - R3;\n\t"
        "R4 = R4 + R6, R6 = R4 - R6;\n\t"
        "R5 = R5 + R7, R7 = R5 - R7;\n\t"
        "R0 = R0 + R4, R4 = R0 - R4;\n\t"
        "R1 = R1 + R5, R5 = R1 - R5;\n\t"
        "R2 = R2 + R6, R6 = R2 - R6 || [I2++M0] = R0 || NOP;\n\t"
        "R3 = R3 + R7, R7 = R3 - R7 || [I3++M0] = R4 || NOP;\n\t"
        "[I2++M0] = R1;\n\t"
        "[I3++M0] = R5;\n\t"
        "[I2++M0] = R2;\n\t"
        "[I3++M0] = R6;\n\t"
        "[I2++M1] = R3;\n\t"
        "[I3++M1] = R7;\n\t"
        "[%[ll]++] = R1;\n"
        "2:\t[%[lr]++] = R2;\n"
        : [ll] "+a"(latel), [lr] "+a"(later)
        : [f] "a"(F), [n] "a"(n)
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "I0", "I1", "I2", "I3", "M0", "M1", "LC0", "LT0", "LB0",
          "memory", "cc");
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

/* ---- stage 3: the line writes (k_write_all), two samples per 32-bit store. The rows and inputs stay below 2^23 in
 * magnitude (F <= 2^19 x 8 rows, the input < 2^18), so x >> 29 (logical) is 7 exactly when x < 0: toward zero is
 * x + that, >> 3 (via << 13 saturated, the high half); to nearest even, << 13 saturated and the high half read out of
 * the accumulator (rounded, unbiased) ---- */
#define KW_TZ(PM)                                                                                                  \
    "R2 = R0 >> 29;\n\t"                                                                                           \
    "R0 = R0 + R2 (NS);\n\t"                                                                                       \
    "R3 = R3 " PM " R1 (NS);\n\t"                                                                                  \
    "R2 = R3 >> 29;\n\t"                                                                                           \
    "R3 = R3 + R2 (NS);\n\t"                                                                                       \
    "R0 = R0 << 13 (S);\n\t"                                                                                       \
    "R3 = R3 << 13 (S);\n\t"                                                                                       \
    "R0 = PACK (R3.H, R0.H);\n"
#define KW_NE(PM)                                                                                                  \
    "R0 = R0 << 13 (S);\n\t"                                                                                       \
    "R3 = R3 " PM " R1 (NS);\n\t"                                                                                  \
    "R3 = R3 << 13 (S);\n\t"                                                                                       \
    "A0 = R0;\n\tA1 = R3;\n\t"                                                                                     \
    "R0.H = A1, R0.L = A0;\n"
#define KW_ASM(PM, RQ)                                                                                             \
    __asm__ volatile(                                                                                              \
        RND_ON                                                                                                     \
        "P2 = 12 (X);\n\t"                                                                                         \
        "LSETUP (3f, 4f) LC1 = %[nj];\n"                                                                           \
        "3:\tR0 = [%[j] + 0];\n\tI0 = R0;\n\t"                                                                     \
        "R0 = [%[j] + 8];\n\tI1 = R0;\n\t"                                                                         \
        "P1 = [%[j] + 4];\n\t"                                                                                     \
        "LSETUP (1f, 2f) LC0 = %[n2];\n"                                                                           \
        "1:\tMNOP || R0 = [P1++] || R1 = [I1++];\n\t"                                                              \
        "R0 = R0 " PM " R1 (NS) || R3 = [P1++] || R1 = [I1++];\n\t"                                                \
        RQ(PM)                                                                                                     \
        "2:\t[I0++] = R0;\n"                                                                                       \
        "4:\t%[j] = %[j] + P2;\n\t"                                                                                \
        RND_OFF                                                                                                    \
        : [j] "+a"(jp)                                                                                             \
        : [nj] "a"(4), [n2] "a"(n >> 1)                                                                            \
        : "R0", "R1", "R2", "R3", "P1", "P2", "I0", "I1", "A0", "A1", "LC0", "LT0", "LB0", "LC1", "LT1", "LB1",    \
          "memory", "cc")

struct kwj {
    int16_t *d;                 /* 0 */
    const int32_t *row, *inj;   /* 4, 8 */
};

/* stage 3 for all 8 lines, as two calls (by the input's sign), two samples per 32-bit store; then the guards.
 * Inputs on rows 3 and 4's signs: +P on lines 0 3, +M on 4 7; -M on 1 2, -P on 5 6 */
static __attribute__((noinline)) void k_write_all(int16_t *const *ln, uint32_t mask, uint32_t t,
                                                  int32_t F[NL][BLOCK], int32_t inj[2][BLOCK], int near, int n)
{
    static const uint8_t plus[4] = { 0, 3, 4, 7 }, minus[4] = { 1, 2, 5, 6 };
    struct kwj jobs[2][4];
    for (int k = 0; k < 4; k++) {
        int i = plus[k], m = minus[k];
        jobs[0][k] = (struct kwj){ ln[i] + (t & mask), F[i], inj[i >= 4] };
        jobs[1][k] = (struct kwj){ ln[m] + (t & mask), F[m], inj[m < 4] };
    }
    struct kwj *jp = jobs[0];
    if (!near)
        KW_ASM("+", KW_TZ);
    else
        KW_ASM("+", KW_NE);
    jp = jobs[1];
    if (!near)
        KW_ASM("-", KW_TZ);
    else
        KW_ASM("-", KW_NE);
    for (int i = 0; i < NL; i++)
        ln[i][mask + 1] = ln[i][0];
}

/* ---- small block moves: the predelay write (k_pdw: n divides the ring and t, so one contiguous run), the shimmer's
 * return into the send (k_add, saturated), the shimmer's input (k_shin: the late outputs >> 4, or the one >> 3, the
 * low 16 bits) ---- */
static __attribute__((noinline)) void k_pdw(int16_t *pd, uint32_t pmask, uint32_t t, const int16_t *x, int n)
{
    __asm__ volatile(
        "I0 = %[x];\n\t"
        "I1 = %[d];\n\t"
        "LSETUP (1f, 2f) LC0 = %[n2];\n"
        "1:\tR0 = [I0++];\n"
        "2:\t[I1++] = R0;\n"
        :
        : [x] "a"(x), [d] "a"(pd + (t & pmask)), [n2] "a"(n >> 1)
        : "R0", "I0", "I1", "LC0", "LT0", "LB0", "memory");
}

static __attribute__((noinline)) void k_zero(uint32_t *w, uint32_t nw)
{
    if (nw)
        __asm__ volatile(
            "R0 = 0;\n\t"
            "LSETUP (1f, 1f) LC0 = %[n];\n"
            "1:\t[%[w]++] = R0;\n"
            : [w] "+a"(w)
            : [n] "a"(nw)
            : "R0", "LC0", "LT0", "LB0", "memory");
}

/* the largest |x| of two blocks (the duck's input): the pairs' max and min by vector ops, |-32768| = 32768 */
static __attribute__((noinline)) int32_t k_peak(const int16_t *a, const int16_t *b, int n)
{
    int32_t pk;
    __asm__ volatile(
        "I0 = %[a];\n\t"
        "I1 = %[b];\n\t"
        "R6 = 0;\n\t"
        "R7 = 0;\n\t"
        "LSETUP (1f, 2f) LC0 = %[n2];\n"
        "1:\tMNOP || R0 = [I0++] || R1 = [I1++];\n\t"
        "R6 = MAX (R6, R0) (V);\n\t"
        "R7 = MIN (R7, R0) (V);\n\t"
        "R6 = MAX (R6, R1) (V);\n"
        "2:\tR7 = MIN (R7, R1) (V);\n\t"
        "R0 = R6.L (X);\n\tR1 = R6 >>> 16;\n\tR0 = MAX (R0, R1);\n\t"
        "R2 = R7.L (X);\n\tR3 = R7 >>> 16;\n\tR2 = MIN (R2, R3);\n\t"
        "R2 = -R2;\n\t"
        "%[pk] = MAX (R0, R2);\n"
        : [pk] "=d"(pk)
        : [a] "a"(a), [b] "a"(b), [n2] "a"(n >> 1)
        : "R0", "R1", "R2", "R3", "R6", "R7", "I0", "I1", "LC0", "LT0", "LB0", "cc");
    return pk;
}

/* the shimmer's ring write: every second sample (the odd ones), circular */
static __attribute__((noinline)) void k_ringw(int16_t *ring, uint32_t mask, uint32_t t, const int16_t *in, int n2)
{
    __asm__ volatile(
        "B0 = %[r];\n\t"
        "L0 = %[b];\n\t"
        "I0 = %[w];\n\t"
        "I1 = %[in];\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR0 = [I1++];\n"
        "2:\tW [I0++] = R0.H;\n\t"
        "R0 = 0;\n\tL0 = R0;\n"
        :
        : [r] "a"(ring), [b] "a"((mask + 1) * 2), [w] "a"(ring + (t & mask)), [in] "a"(in), [n] "a"(n2)
        : "R0", "I0", "I1", "B0", "L0", "LC0", "LT0", "LB0", "memory");
}

static __attribute__((noinline)) void k_add(int16_t *u, const int16_t *a, int n)
{
    __asm__ volatile(
        "I0 = %[u];\n\t"
        "I1 = %[a];\n\t"
        "I2 = %[u];\n\t"
        "LSETUP (1f, 2f) LC0 = %[n2];\n"
        "1:\tMNOP || R0 = [I0++] || R1 = [I1++];\n\t"
        "R0 = R0 +|+ R1 (S);\n"
        "2:\t[I2++] = R0;\n"
        :
        : [u] "a"(u), [a] "a"(a), [n2] "a"(n >> 1)
        : "R0", "R1", "I0", "I1", "I2", "LC0", "LT0", "LB0", "memory");
}

static __attribute__((noinline)) void k_shin(int16_t *sh, const int32_t *l0, const int32_t *l1, int stereo, int n)
{
    if (stereo)
        __asm__ volatile(
            "I0 = %[l0];\n\t"
            "I1 = %[l1];\n\t"
            "LSETUP (1f, 2f) LC0 = %[n];\n"
            "1:\tMNOP || R0 = [I0++] || R1 = [I1++];\n\t"
            "R0 = R0 + R1;\n\t"
            "R0 >>>= 4;\n"
            "2:\tW [%[s]++] = R0;\n"
            : [s] "+a"(sh)
            : [l0] "a"(l0), [l1] "a"(l1), [n] "a"(n)
            : "R0", "R1", "I0", "I1", "LC0", "LT0", "LB0", "memory");
    else
        __asm__ volatile(
            "I0 = %[l0];\n\t"
            "LSETUP (1f, 2f) LC0 = %[n];\n"
            "1:\tMNOP || R0 = [I0++] || NOP;\n\t"
            "R0 >>>= 3;\n"
            "2:\tW [%[s]++] = R0;\n"
            : [s] "+a"(sh)
            : [l0] "a"(l0), [n] "a"(n)
            : "R0", "I0", "LC0", "LT0", "LB0", "memory");
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
        "R2 = R2 << 16 (S);\n"                       /* saturated: the high half */
        "2:\tW [I2++] = R2.H;\n\t"
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

/* ---- the shimmer's biquad (k_bq): DF1, integer MACs, toward zero ---- */
struct kbq {
    int16_t *x;                 /* 0 */
    int32_t *st;                /* 4 */
    uint32_t b01, b2a1, a2;     /* 8, 12, 16 */
    int32_t n;                  /* 20 */
};

static __attribute__((noinline)) void k_bq(int16_t *x, int32_t *st_, const int16_t *q, int n)
{
    struct kbq k = { x, st_, PK2(q[0], q[1]), PK2(q[2], q[3]), PK2(0, q[4]), n };
    __asm__ volatile(
        "P1 = [%[k] + 0];\n\t"
        "P2 = [%[k] + 4];\n\t"
        "R6 = [%[k] + 8];\n\t"
        "R7 = [%[k] + 12];\n\t"
        "R3 = [%[k] + 16];\n\t"
        "R0 = [P2 + 0];\n\tR1 = [P2 + 4];\n\tR4 = PACK (R1.L, R0.L);\n\t"   /* x1 | x2 << 16 */
        "R0 = [P2 + 8];\n\tR1 = [P2 + 12];\n\tR5 = PACK (R1.L, R0.L);\n\t"  /* y1 | y2 << 16 */
        "P0 = [%[k] + 20];\n\t"
        "LSETUP (1f, 2f) LC0 = P0;\n"
        "1:\tR0 = W [P1] (X);\n\t"
        "A0 = R0.L * R6.L (IS);\n\t"
        "A0 += R4.L * R6.H (IS);\n\t"
        "A0 += R4.H * R7.L (IS);\n\t"
        "A0 -= R5.L * R7.H (IS);\n\t"
        "R2 = (A0 -= R5.H * R3.H) (IS);\n\t"
        "R1 = R2 >>> 31;\n\t"
        "R1 >>= 18;\n\t"                            /* 0x3FFF when negative: toward zero */
        "R2 = R2 + R1;\n\t"
        "R2 = R2 << 2 (S);\n\t"
        "R2 >>>= 16;\n\t"
        "R4 = PACK (R4.L, R0.L);\n\t"
        "R5 = PACK (R5.L, R2.L);\n"
        "2:\tW [P1++] = R2;\n\t"
        "R0 = R4.L (X);\n\t[P2 + 0] = R0;\n\tR0 = R4 >>> 16;\n\t[P2 + 4] = R0;\n\t"
        "R0 = R5.L (X);\n\t[P2 + 8] = R0;\n\tR0 = R5 >>> 16;\n\t[P2 + 12] = R0;\n"
        :
        : [k] "a"(&k)
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P0", "P1", "P2", "A0", "LC0", "LT0", "LB0", "memory", "cc");
}

/* ---- the shimmer's heads (k_heads): per segment (a job), the read steps r whole samples per sample, its fraction
 * fixed (the difference x the fraction a fractional multiply, rounded to nearest even); the window x gain w steps
 * linearly; acc += y w >> 15 ---- */
struct khj {
    int32_t *acc;               /* 0 */
    const int16_t *ra, *rb;     /* 4, 8 */
    uint32_t step;              /* 12: r x 2 bytes */
    uint32_t f;                 /* 16: the fraction, Q15 */
    uint32_t wd;                /* 20: w | dw << 16 */
    int32_t n;                  /* 24 */
};

static __attribute__((noinline)) void k_heads(const struct hseg *g, int ng, const int16_t *ring, uint32_t mask)
{
    struct khj jobs[8];
    for (int k = 0; k < ng; k++) {
        uint32_t p1 = g[k].p + g[k].inc, i = (p1 >> 16) & mask;
        jobs[k] = (struct khj){ g[k].acc, ring + i, ring + ((i + 1) & mask), (g[k].inc >> 16) * 2, (p1 & 0xFFFF) >> 1,
                                PK2(g[k].w, g[k].dw), g[k].n };
    }
    struct khj *jp = jobs;
    __asm__ volatile(
        RND_ON
        "B2 = %[ring];\n\tB3 = %[ring];\n\t"
        "L2 = %[bytes];\n\tL3 = %[bytes];\n\t"
        "LSETUP (3f, 4f) LC1 = %[nj];\n"
        "3:\tR0 = [%[k] + 0];\n\tI0 = R0;\n\t"
        "R0 = [%[k] + 4];\n\tI2 = R0;\n\t"
        "R0 = [%[k] + 8];\n\tI3 = R0;\n\t"
        "R0 = [%[k] + 12];\n\tM2 = R0;\n\t"
        "R2 = [%[k] + 16];\n\t"
        "R5 = [%[k] + 20];\n\t"
        "R6 = R5 >> 16;\n\t"                        /* dw in R6.L */
        "P0 = [%[k] + 24];\n\t"
        "LSETUP (1f, 2f) LC0 = P0;\n"
        "1:\tMNOP || R3.L = W [I2] || R7.L = W [I3];\n\t"
        "MNOP || I2 += M2 || R0 = [I0];\n\t"
        "R7.L = R7.L - R3.L (S) || I3 += M2 || NOP;\n\t"
        "R7.L = R7.L * R2.L;\n\t"
        "R3.L = R3.L + R7.L (NS);\n\t"              /* y */
        "R3 = R3.L * R5.L (IS);\n\t"                /* y w */
        "R3 >>>= 15;\n\t"
        "R5.L = R5.L + R6.L (NS);\n\t"
        "R0 = R0 + R3;\n"
        "2:\t[I0++] = R0;\n"
        "4:\t%[k] += 28;\n\t"
        "R0 = 0;\n\tL2 = R0;\n\tL3 = R0;\n\t"
        RND_OFF
        : [k] "+a"(jp)
        : [ring] "a"(ring), [bytes] "a"((mask + 1) * 2), [nj] "a"(ng)
        : "R0", "R2", "R3", "R5", "R6", "R7", "P0", "I0", "I2", "I3", "B2", "B3", "L2", "L3", "M2", "LC0", "LT0",
          "LB0", "LC1", "LT1", "LB1", "memory", "cc");
}

/* ---- the shimmer's return (k_shret): x the gain, the soft limit, up to the tank rate ----
 * (x 43691 >> 15 = x + (x 10923 >> 15); x 24576 >> 15 = 3 x >> 2; u u by a 16-bit multiply: +-32768 square alike) */
struct ksr {
    const int32_t *acc;         /* 0 */
    int16_t *out;               /* 4 */
    int32_t g, prev, n;         /* 8, 12 (in / out), 16 */
};

static __attribute__((noinline)) void k_shret(int16_t *out, const int32_t *acc, int32_t g, int32_t *prev, int n)
{
    struct ksr q = { acc, out, g, *prev, n };
    __asm__ volatile(
        "P1 = [%[k] + 0];\n\t"
        "P2 = [%[k] + 4];\n\t"
        "R7 = [%[k] + 8];\n\t"
        "R6 = [%[k] + 12];\n\t"
        "P0 = [%[k] + 16];\n\t"
        "R5 = 10923 (X);\n\t"
        "R4 = 1;\n\tR4 <<= 15;\n\t"                /* 32768 */
        "R3 = -R4;\n\t"
        "LSETUP (1f, 2f) LC0 = P0;\n"
        "1:\tR0 = [P1++];\n\t"
        "R0 *= R7;\n\t"
        "R0 >>>= 15;\n\t"                           /* xx */
        "R1 = R0;\n\t"
        "R1 *= R5;\n\t"
        "R1 >>>= 15;\n\t"
        "R0 = R0 + R1;\n\t"
        "R0 = MIN (R0, R4);\n\t"
        "R0 = MAX (R0, R3);\n\t"                    /* u */
        "R1 = R0.L * R0.L (IS);\n\t"
        "R1 >>>= 15;\n\t"
        "R1 *= R0;\n\t"
        "R1 >>>= 15;\n\t"                           /* u^3 */
        "R1 *= R5;\n\t"
        "R1 >>>= 15;\n\t"
        "R0 = R0 - R1;\n\t"
        "R1 = R0 << 1;\n\t"
        "R0 = R0 + R1;\n\t"
        "R0 >>>= 2;\n\t"                            /* y */
        "R1 = R0 + R6;\n\t"
        "R1 >>>= 1;\n\t"
        "R6 = R0;\n\t"
        "W [P2++] = R1;\n"
        "2:\tW [P2++] = R0;\n\t"
        "[%[k] + 12] = R6;\n"
        :
        : [k] "a"(&q)
        : "R0", "R1", "R3", "R4", "R5", "R6", "R7", "P0", "P1", "P2", "LC0", "LT0", "LB0", "memory", "cc");
    *prev = q.prev;
}

/* ---- the half-band decimator and interpolator (k_decimate, k_interpolate), two outputs at a time: A0 sums output m,
 * A1 output m + 1 (m even), one dual multiply-accumulate per tap and side on a pair of neighbouring samples read as one
 * word. A pair starting at an odd index is read from the shifted copy (bs[k] = bo[k + 1], cs[k] = c[k + 1]), which these
 * keep alongside: the backward pairs from bs (taps 0 2 4 6: I0) and bo (1 3 5 7: I1), the forward ones from bo (I2) and
 * bs (I3); after the 8 taps the pointers move on to the next pair of outputs (back +20 bytes: M0, forward -12: M1). A
 * MAC reads its operands before a parallel load writes. ---- */
#define HB2_TAPS                                                                                                   \
    "A1 += R0.H * R4.L, A0 += R0.L * R4.L || R0 = [I1--] || NOP;\n\t"                                              \
    "A1 += R1.H * R4.L, A0 += R1.L * R4.L || R1 = [I3++] || NOP;\n\t"                                              \
    "A1 += R0.H * R4.H, A0 += R0.L * R4.H || R0 = [I0--] || NOP;\n\t"                                              \
    "A1 += R1.H * R4.H, A0 += R1.L * R4.H || R1 = [I2++] || NOP;\n\t"                                              \
    "A1 += R0.H * R5.L, A0 += R0.L * R5.L || R0 = [I1--] || NOP;\n\t"                                              \
    "A1 += R1.H * R5.L, A0 += R1.L * R5.L || R1 = [I3++] || NOP;\n\t"                                              \
    "A1 += R0.H * R5.H, A0 += R0.L * R5.H || R0 = [I0--] || NOP;\n\t"                                              \
    "A1 += R1.H * R5.H, A0 += R1.L * R5.H || R1 = [I2++] || NOP;\n\t"                                              \
    "A1 += R0.H * R6.L, A0 += R0.L * R6.L || R0 = [I1--] || NOP;\n\t"                                              \
    "A1 += R1.H * R6.L, A0 += R1.L * R6.L || R1 = [I3++] || NOP;\n\t"                                              \
    "A1 += R0.H * R6.H, A0 += R0.L * R6.H || R0 = [I0--] || NOP;\n\t"                                              \
    "A1 += R1.H * R6.H, A0 += R1.L * R6.H || R1 = [I2++] || NOP;\n\t"                                              \
    "A1 += R0.H * R7.L, A0 += R0.L * R7.L || R0 = [I1--] || NOP;\n\t"                                              \
    "A1 += R1.H * R7.L, A0 += R1.L * R7.L || R1 = [I3++] || NOP;\n\t"                                              \
    "A1 += R0.H * R7.H, A0 += R0.L * R7.H || I0 += M0 || NOP;\n\t"                                                 \
    "A1 += R1.H * R7.H, A0 += R1.L * R7.H || I1 += M0 || NOP;\n\t"                                                 \
    "MNOP || I2 += M1 || NOP;\n\t"                                                                                 \
    "MNOP || I3 += M1 || NOP;\n\t"
struct khb2 {
    const int16_t *i0, *i1, *i2, *i3;   /* 0..12: the pointers' starts (back: copy, plain; forward: plain, copy) */
    const int16_t *ct;                  /* 16: the centre pairs */
    int16_t *out;                       /* 20 */
};
#define HB2_SETUP                                                                                                  \
    "R4 = [%[h] + 0];\n\t"                                                                                         \
    "R5 = [%[h] + 4];\n\t"                                                                                         \
    "R6 = [%[h] + 8];\n\t"                                                                                         \
    "R7 = [%[h] + 12];\n\t"                                                                                        \
    "R0 = [%[k] + 0];\n\tI0 = R0;\n\t"                                                                             \
    "R0 = [%[k] + 4];\n\tI1 = R0;\n\t"                                                                             \
    "R0 = [%[k] + 8];\n\tI2 = R0;\n\t"                                                                             \
    "R0 = [%[k] + 12];\n\tI3 = R0;\n\t"                                                                            \
    "P1 = [%[k] + 16];\n\t"                                                                                        \
    "P2 = [%[k] + 20];\n\t"                                                                                        \
    "R0 = 20;\n\tM0 = R0;\n\t"                                                                                     \
    "R0 = -12;\n\tM1 = R0;\n\t"
#define HB2_CLOB "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P0", "P1", "P2", "I0", "I1", "I2", "I3", "M0", "M1", \
    "A0", "A1", "LC0", "LT0", "LB0", "memory", "cc"

/* the history (15 of each, 14 of the copy) moved to the front: 7 words and a sample */
static inline void hb_hist(int16_t *b)
{
    uint32_t *w = (uint32_t *)(void *)b;
    for (int i = 0; i < 7; i++)
        w[i] = w[16 + i];
    b[14] = b[46];
}

static __attribute__((noinline)) void k_decimate(int16_t *out, const int16_t *in, int16_t *bo, int16_t *be,
                                                 int16_t *bs)
{
    hb_hist(bo);
    hb_hist(be);
    hb_hist(bs);
    /* the new block deinterleaved: be[15 + i] = in[2 i], bo[15 + i] = bs[14 + i] = in[2 i + 1] */
    be += 15;
    bo += 15;
    bs += 14;
    __asm__ volatile(
        "I0 = %[in];\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR0 = [I0++];\n\t"
        "W [%[e]++] = R0;\n\t"
        "R0 >>= 16;\n\t"
        "W [%[o]++] = R0;\n"
        "2:\tW [%[s]++] = R0;\n"
        : [e] "+a"(be), [o] "+a"(bo), [s] "+a"(bs)
        : [in] "a"(in), [n] "a"(BLOCK / 2)
        : "R0", "I0", "LC0", "LT0", "LB0", "memory");
    bo -= 47;
    be -= 47;
    bs -= 46;
    /* out[m] = (16384 be[m+8] + sum_j hb[j] (bo[m+7-j] + bo[m+8+j])) rounded >> 15 */
    struct khb2 q = { bs + 6, bo + 6, bo + 8, bs + 8, be + 8, out };
    __asm__ volatile(
        RND_ON
        HB2_SETUP
        "R3 = 16384 (X);\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR2 = [P1++];\n\t"
        "MNOP || R0 = [I0--] || R1 = [I2++];\n\t"
        "A1 = R2.H * R3.L, A0 = R2.L * R3.L;\n\t"
        HB2_TAPS
        "R2.H = A1, R2.L = A0;\n"
        "2:\t[P2++] = R2;\n\t"
        RND_OFF
        :
        : [h] "a"(sp_hb), [k] "a"(&q), [n] "a"(BLOCK / 4)
        : HB2_CLOB);
}

static __attribute__((noinline)) void k_interpolate(int16_t *out, const int16_t *in, int16_t *c, int16_t *cs)
{
    hb_hist(c);
    hb_hist(cs);
    /* the new block: c[15 + i] = cs[14 + i] = in[i] */
    c += 15;
    cs += 14;
    __asm__ volatile(
        "I0 = %[in];\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR0 = [I0++];\n\t"
        "[%[s]++] = R0;\n\t"
        "W [%[c]++] = R0;\n\t"
        "R0 >>= 16;\n"
        "2:\tW [%[c]++] = R0;\n"
        : [c] "+a"(c), [s] "+a"(cs)
        : [in] "a"(in), [n] "a"(BLOCK / 4)
        : "R0", "I0", "LC0", "LT0", "LB0", "memory");
    c -= 47;
    cs -= 46;
    /* out[2m] = sum_j hb[j] (c[m+8+j] + c[m+7-j]) x 2 rounded >> 15, out[2m+1] = c[m+8] */
    struct khb2 q = { cs + 6, c + 6, c + 8, cs + 8, c + 8, out };
    __asm__ volatile(
        RND_ON
        HB2_SETUP
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR2 = [P1++];\n\t"
        "MNOP || R0 = [I0--] || R1 = [I2++];\n\t"
        "A1 = A0 = 0;\n\t"
        HB2_TAPS
        "R3.H = A1, R3.L = A0 (S2RND);\n\t"
        "R0 = PACK (R2.L, R3.L);\n\t"
        "[P2++] = R0;\n\t"
        "R0 = PACK (R2.H, R3.H);\n"
        "2:\t[P2++] = R0;\n\t"
        RND_OFF
        :
        : [h] "a"(sp_hb), [k] "a"(&q), [n] "a"(BLOCK / 4)
        : HB2_CLOB);
}

#endif
