/* Guitar amp: plays TE's audio input (TE IN, 0xFF800CB4) through gate, drive, tone and cab. Design: notes/amp.md;
 * input research: notes/input.md. TE's note envelope still applies, so a held or latched note (SHIFT+T2 HOLD) opens
 * the amp; the presets use full sustain.
 *
 * Knobs (engine page, 0..0x7FFF, slewed 1024 per block):
 *   [0] blue    drive: pre-gain 0 .. +44 dB into a two-stage tube curve (clean -> crunch -> lead); also tightens the
 *               lows (high-pass 30 -> 160 Hz) and pushes the upper mids (+6 dB pre-emphasis); loudness compensated
 *   [1] green   tone: tilt, dark (+5 dB lows, -10 dB highs) .. flat (centre) .. bright, in a blackface-style stack
 *               before stage 2 (it changes what clips) with a fixed mid scoop (-8 dB at 450 Hz)
 *   [2] white   gate threshold: off at 0, then -84 .. -30 dBFS (opens above it, closes 6 dB below, 50 ms hold)
 *   [3] orange  cab: direct (no cab) -> 1x12 open back -> 2x12 -> 4x12 closed back
 *
 * Chain per block: TE IN (64 at 44.1 kHz) -> gate detector -> 2x half-band upsampler (24 taps, 88.2 kHz) -> tight
 * high-pass -> pre-emphasis -> drive x gate gain -> tube (biased, ADAA) -> interstage low-pass (two poles at 7 kHz) ->
 * tone stack (low and high shelf, mid scoop; scaled to a peak gain of 0 dB like a passive stack) -> stage 2 (ADAA) ->
 * high-pass, mid bell, presence bell, two low-passes (cab) -> loudness and the stack's make-up -> limiter (-3 dBFS, gain
 * from the previous block's peak, 93 ms release) -> soft knee. Run as passes over the block (amp_dsp): the gate (C),
 * then hand-written loops: upsampler, drive, stage 1, interstage, the SVFs, stage 2. Both shapers are distortion.c's
 * tube kernel with first-order antiderivative anti-aliasing (stage 2 is the tube at 1.5x its input).
 *
 * One voice only plays the input (else a chord would stack copies of the guitar). Every voice calls render; the
 * shared state is amp_ram (amp.h). Per call:
 *   1. New TE block? TE IN differs from the last block's copy, or te_rx_sel toggled, or this voice rendered already in
 *      the current block while the input is digital silence. Then blk++ and the block is copied (and into the tuner's
 *      the output y[] is computed once per block, by the first voice that needs it.
 *   2. Owner: a voice takes over when it has a fresh note-on (TE's 0x80000000 in state bytes 0..3: newest note wins),
 *      when there is no owner, when the owner skipped a whole block (it stopped rendering: release ended or stolen),
 *      or when the owner is released and this voice isn't (iter's note-off sets state byte 28; Hypothesis).
 *   3. Each voice's gain (table in amp_ram) moves towards 1 (owner) or 0 by 1/8 per block, ramped per sample: a
 *      handover is an 11.6 ms crossfade of the same y[], so the level stays put. Non-owners at gain 0 output silence
 *      (the DSP runs once per block whoever renders first). A voice that skipped a block, or has a fresh note-on, fades in from 0.
 *
 * Fixed point: audio Q24 (1.0 = full scale), the shapers' inputs Q20, coefficients Q31 / Q28 / Q29 (amp_tables.h, from
 * gen_amp_tables.py), 32 x 32 multiplies without the low x low partial product. No division operator (udiv() / recip_q31() loops, per block only).
 * State bytes: 0..3 note-on mark (TE writes 0x80000000), 4..7 the block of this voice's last render, 28 iter's
 * released flag; the rest is TE's (note-on writes 8..11, 16..27).
 */
#include "engine.h"
#include "dsp.h"
#include "inprobe.h"
#include "amp.h"
#include "amp_tables.h"

struct voice {
    uint32_t mark;                          /*  0: 0x80000000 after TE's note-on; we write 0 */
    uint32_t stamp;                         /*  4: amp_ram.blk at this voice's last render */
    uint32_t te8, te12, te16, te20, te24;   /*  8..27: TE's (note-on) */
    uint8_t released;                       /* 28: iter's note-off writes 1 (Hypothesis); cleared at our note-on */
    uint8_t pad[3];
};

#define HOLD 2205                           /* gate hold: 50 ms at 44.1 kHz */
#define LIM_T 11877049                      /* limiter threshold: -3 dBFS, Q24 */
#define SLEW 1024                           /* knob units per block */

/* One row of a knob table, interpolated: AMP_N points at 0, 1024, .. 32768 (k 0..32767). */
static void knob_row(const int32_t *t, int w, int32_t k, int32_t *row)
{
    int32_t i = k >> 10, f = k & 1023;
    const int32_t *a = t + i * w, *b = a + w;
    for (int j = 0; j < w; j++) {
        int32_t d = b[j] - a[j];
        row[j] = a[j] + (d >> 10) * f + (((d & 1023) * f) >> 10);
    }
}

static void set_svf(struct amp_svf *f, const int32_t *c)
{
    struct svf s = svf_coefs(c[0], c[1]);
    f->a1 = s.a1;
    f->a2 = s.a2;
    f->a3 = s.a3;
    f->m0 = c[2];
    f->m1 = c[3];
    f->m2 = c[4];
}

/* (a * b) >> 31 without the low x low partial product (at most 1 LSB off): 4 instructions instead of the builtin's 6.
 * Not saturating: |a|, |b| < 2^31 and never both -2^31 here. */
#ifdef __bfin__
static inline int32_t fmul(int32_t a, int32_t b)
{
    int32_t r;
    __asm__("A1 = %1.H * %2.L (M), A0 = %1.H * %2.H;\n\tA1 += %2.H * %1.L (M);\n\tA1 = A1 >>> 15;\n\t%0 = (A0 += A1);"
            : "=d"(r) : "d"(a), "d"(b) : "A0", "A1");
    return r;
}
#else
#define fmul(a, b) qmul((a), (b))
#endif

/* Simper SVF over the block, in to out (may be the same), hand-written: per sample
 *   v3 = v0 - ic2;  v1 = a1 ic1 + a2 v3;  c2 = a2 ic1 + a3 v3;  v2 = ic2 + c2;  ic1 = 2 v1 - ic1;  ic2 += 2 c2
 * with each sum of two products in one accumulator chain (32 x 32 without the low x low partial products, one
 * truncation per sum), coefficients reloaded each sample in parallel slots (I1, rewound by M1), the next input
 * loaded and the output stored in parallel. Outputs (Q29 mix: products << 2): SHELF v0 + m1 v1 + m2 v2 (both
 * shelves: the high shelf's m0 = A^2 is divided out here and multiplied in elsewhere), HP v0 + m1 v1 - v2, BELL
 * v0 + m1 v1, BELL20 the same >> 4 (Q24 in, Q20 out), LOW v2: 25 / 24 / 23 / 24 / 17 instructions per sample.
 * Reads in[AMP_OUT] (one past the end). */
enum { SHELF, HP, BELL, BELL20, LOW };
#define SVF_IN                                                                                                       \
    "I0 = %[in];\n\tI2 = %[out];\n\tI1 = %[co];\n\tM1 = %[rw];\n\t"                                                   \
    "R6 = [%[st]];\n\tR7 = [%[st] + 4];\n\tR2 = %[n];\n\tLC0 = R2;\n\tR0 = [I0++];\n\t"
/* v1 -> R5, c2 -> R2, then ic1 (R6), v2 (R1), ic2 (R7); on entry R5 a1, R3 a2, R1 v3; R4 <- a3; X1, X2 extra slots */
#define SVF_CHAINS(X1, X2)                                                                                           \
    "A1 = R5.H * R6.L (M), A0 = R5.H * R6.H || R4 = [I1++] || NOP;\n\t"                                              \
    "A1 += R6.H * R5.L (M);\n\t"                                                                                     \
    "A1 += R3.H * R1.L (M), A0 += R3.H * R1.H;\n\t"                                                                  \
    "A1 += R1.H * R3.L (M);\n\t"                                                                                     \
    "A1 = A1 >>> 15;\n\t"                                                                                            \
    "R5 = (A0 += A1);\n\t"                                                                                           \
    "A1 = R3.H * R6.L (M), A0 = R3.H * R6.H;\n\t"                                                                    \
    "A1 += R6.H * R3.L (M);\n\t"                                                                                     \
    "A1 += R4.H * R1.L (M), A0 += R4.H * R1.H" X1 ";\n\t"                                                            \
    "A1 += R1.H * R4.L (M);\n\t"                                                                                     \
    "A1 = A1 >>> 15" X2 ";\n\t"                                                                                      \
    "R2 = (A0 += A1);\n\t"                                                                                           \
    "R6 = R5 - R6;\n\t"                                                                                              \
    "R6 = R6 + R5;\n\t"                                                                                              \
    "R1 = R7 + R2;\n\t"                                                                                              \
    "R7 = R1 + R2;\n\t"
#define SVF_START "R5 = [I1++];\n\tR3 = [I1++];\n\tR1 = R0 - R7;\n\tLSETUP (1f, 2f) LC0;\n1:\t"
#define M1R3 " || R3 = [I1++] || NOP"
#define M2R4 " || R4 = [I1++] || NOP"
/* m1 (R3) x v1 (R5) into A0 / A1, rewinding the coefficients; reloads a1 (R5), a2 (R3) for the next sample */
#define SVF_M1(X3, X4)                                                                                               \
    "A1 = R3.H * R5.L (M), A0 = R3.H * R5.H || I1 -= M1 || NOP;\n\t"                                                 \
    "A1 += R5.H * R3.L (M);\n\t" X3 X4
#define SVF_END "R2 <<= 2;\n\tR2 = R2 + R0 (NS) || R0 = [I0++] || NOP;\n\t"
#define SVF_LOOP_END "2:\tR1 = R0 - R7 (NS) || [I2++] = R2 || NOP;\n\t[%[st]] = R6;\n\t[%[st] + 4] = R7;\n"
#define SVF_ARGS                                                                                                     \
    : : [st] "a"(f), [in] "a"(in), [out] "a"(out), [co] "a"(&f->a1), [rw] "a"(rw), [n] "i"(AMP_OUT)                   \
    : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "A0", "A1", "I0", "I1", "I2", "M1", "cc", "LC0", "LT0", "LB0", \
      "memory"
static __attribute__((noinline)) void svf_block(struct amp_svf *f, const int32_t *in, int32_t *out, int kind)
{
    int32_t rw = kind == SHELF ? 20 : kind == LOW ? 12 : 16;
    if (kind == SHELF)
        __asm__ volatile(SVF_IN SVF_START
            SVF_CHAINS(M1R3, M2R4)
            SVF_M1("A1 += R4.H * R1.L (M), A0 += R4.H * R1.H || R5 = [I1++] || NOP;\n\t",
                   "A1 += R1.H * R4.L (M) || R3 = [I1++] || NOP;\n\tA1 = A1 >>> 15;\n\tR2 = (A0 += A1);\n\t")
            SVF_END SVF_LOOP_END SVF_ARGS);
    else if (kind == HP)
        __asm__ volatile(SVF_IN SVF_START
            SVF_CHAINS(M1R3, "")
            SVF_M1("A1 = A1 >>> 15 || R5 = [I1++] || NOP;\n\t", "R2 = (A0 += A1) || R3 = [I1++] || NOP;\n\t")
            SVF_END "R2 = R2 - R1;\n" SVF_LOOP_END SVF_ARGS);
    else if (kind == BELL)
        __asm__ volatile(SVF_IN SVF_START
            SVF_CHAINS(M1R3, "")
            SVF_M1("A1 = A1 >>> 15 || R5 = [I1++] || NOP;\n\t", "R2 = (A0 += A1) || R3 = [I1++] || NOP;\n\t")
            SVF_END SVF_LOOP_END SVF_ARGS);
    else if (kind == BELL20)
        __asm__ volatile(SVF_IN SVF_START
            SVF_CHAINS(M1R3, "")
            SVF_M1("A1 = A1 >>> 15 || R5 = [I1++] || NOP;\n\t", "R2 = (A0 += A1) || R3 = [I1++] || NOP;\n\t")
            SVF_END "R2 >>>= 4;\n" SVF_LOOP_END SVF_ARGS);
    else    /* LOW: a1..a3 stay in R3..R5; v0 R0, v3 R1, v1 R0, c2 R2 */
        __asm__ volatile(SVF_IN
            "R3 = [I1++];\n\tR4 = [I1++];\n\tR5 = [I1++];\n\tR1 = R0 - R7;\n\tLSETUP (1f, 2f) LC0;\n1:\t"
            "A1 = R3.H * R6.L (M), A0 = R3.H * R6.H;\n\t"
            "A1 += R6.H * R3.L (M);\n\t"
            "A1 += R4.H * R1.L (M), A0 += R4.H * R1.H;\n\t"
            "A1 += R1.H * R4.L (M);\n\t"
            "A1 = A1 >>> 15;\n\t"
            "R0 = (A0 += A1);\n\t"
            "A1 = R4.H * R6.L (M), A0 = R4.H * R6.H;\n\t"
            "A1 += R6.H * R4.L (M);\n\t"
            "A1 += R5.H * R1.L (M), A0 += R5.H * R1.H;\n\t"
            "A1 += R1.H * R5.L (M);\n\t"
            "A1 = A1 >>> 15;\n\t"
            "R2 = (A0 += A1);\n\t"
            "R6 = R0 - R6;\n\t"
            "R6 = R6 + R0;\n\t"
            "R1 = R7 + R2;\n\t"
            "R7 = R1 + R2 (NS) || [I2++] = R1 || R0 = [I0++];\n"
            "2:\tR1 = R0 - R7;\n\t"
            "[%[st]] = R6;\n\t[%[st] + 4] = R7;\n" SVF_ARGS);
}
/* ---- the drive section (passes over the block) ----
 * hb_pass: 2x up into z: the even sample is the input delayed by AMP_HB_K, the odd one the half-band interpolation
 *   (dual 16-bit MACs, taps in R2-R7); Q24.
 * drive_pass: z -> in1[1..128]: tight high-pass, pre-emphasis, x gate x drive gain (gd, per input sample), saturated
 *   to +-32, + bias (Q20). in1[0] gets the last block's last value (stored through the pipelined store).
 * adaa: the tube's first-order antiderivative anti-aliasing, in -> y (Q24, 1.0 = clip), from distortion.c: per pair
 *   (a, b) of consecutive inputs the mean of the curve over [a, b]; within the knee the closed form
 *   (a + b) / 2 - (a + b) (a^2 + b^2) / 27 (no division); pairs straddling the knee go on a list and take
 *   (F(b) - F(a)) / (b - a) (at most FIXMAX per call: the rest keep the closed form of the clamped pair).
 * inter_pass: z (stage 1) -> in2[1..128]: the interstage low-pass without stage 1's DC, x the stage-2 gain (Q24);
 *   the tone stack's SVFs then run in place there, the last (the scoop) returning Q20 for stage 2. */
#define HB_B(C, X) "A0 += R0.L * " C ".H, A1 += R0.H * " C ".L (IS)" X ";\n\t"   /* c[-k] hb[k] + c[-k-1] hb[k+1] */
#define HB_F(C, X) "A0 += R1.L * " C ".L, A1 += R1.H * " C ".H (IS)" X ";\n\t"   /* c[1+k] hb[k] + c[2+k] hb[k+1] */
#define HB_ONE(IB, IF)                                                                                               \
    "MNOP || R0 = [" IB "--] || R1 = [" IF "++];\n\t"                                                                \
    "A0 = R0.L * R2.H, A1 = R0.H * R2.L (IS) || R0 = [" IB "--] || NOP;\n\t"                                         \
    HB_F("R2", " || R1 = [" IF "++] || NOP") HB_B("R3", " || R0 = [" IB "--] || NOP")                               \
    HB_F("R3", " || R1 = [" IF "++] || NOP") HB_B("R4", " || R0 = [" IB "--] || NOP")                               \
    HB_F("R4", " || R1 = [" IF "++] || NOP") HB_B("R5", " || R0 = [" IB "--] || NOP")                               \
    HB_F("R5", " || R1 = [" IF "++] || NOP") HB_B("R6", " || R0 = [" IB "--] || NOP")                               \
    HB_F("R6", " || R1 = [" IF "++] || NOP") HB_B("R7", " || " IB " += M0 || NOP")                                   \
    HB_F("R7", " || " IF " -= M1 || NOP")                                                                            \
    "R0 = (A0 += A1);\n\t"                                                                                           \
    "R0 >>>= 6;\n\t"                                       /* Q30 -> Q24 */                                         \
    "R1 = W[%[ev]++] (X);\n\t"                                                                                          \
    "R1 <<= 9;\n\t"                                        /* the even sample: c[0], Q24 */                         \
    "[%[x]++] = R1;\n\t"                                                                                               \
    "[%[x]++] = R0;\n\t"
/* 2x up into x2 (Q24): x2[2i] = c[0], x2[2i + 1] = sum hb[k] (c[-k] + c[1 + k]), c = b + i + AMP_HB_K. Word loads of
 * sample pairs (dual 16-bit MACs, the taps in R2-R7 as pairs): for even i from bs, for odd i from b, where the words
 * c - 1 - 2j and c + 1 + 2j are aligned. */
static __attribute__((noinline)) void hb_pass(struct amp_ram *g, int32_t *x2)
{
    const int16_t *b = g->buf;
    int16_t *bs = g->bs;
    const int16_t *ev = b + AMP_HB_K;
    for (int i = 0; i < AMP_HIST + AMP_IN - 1; i++)
        bs[i + 1] = b[i];
    __asm__ volatile(
        "R2 = [%[hb]];\n\tR3 = [%[hb] + 4];\n\tR4 = [%[hb] + 8];\n\tR5 = [%[hb] + 12];\n\t"
        "R6 = [%[hb] + 16];\n\tR7 = [%[hb] + 20];\n\t"
        "I0 = %[e0];\n\tI1 = %[e0];\n\tI1 += 4;\n\tI2 = %[o0];\n\tI3 = %[o0];\n\tI3 += 4;\n\t"
        "R0 = 28;\n\tM0 = R0;\n\tR0 = 20;\n\tM1 = R0;\n\t"
        "R0 = %[n];\n\tLC0 = R0;\n\t"
        "LSETUP (1f, 2f) LC0;\n1:\t"
        HB_ONE("I0", "I1")
        HB_ONE("I2", "I3")
        "2:\tNOP;\n"
        : [x] "+a"(x2), [ev] "+a"(ev)
        : [hb] "a"(amp_hb), [e0] "a"(bs + AMP_HB_K), [o0] "a"(b + AMP_HB_K), [n] "i"(AMP_IN / 2)
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "A0", "A1", "I0", "I1", "I2", "I3", "M0", "M1", "cc", "LC0",
          "LT0", "LB0", "memory");
}

/* One drive sample: R0 s (Q24) -> R3 v (Q20); R4 gain, R5 bias, R6 tight state, R7 pre-emphasis state; I1 pc[]. The
 * first instruction stores the previous v (R3); LOADG: the gain load (once per input sample). */
#define DRIVE_SMP(LOADG)                                                                                             \
    "R1 = R0 - R6 (NS) || [I2++] = R3 || R2 = [I1++];\n\t"     /* s - tight lp; R2 tight a */                       \
    "A1 = R2.H * R1.L (M), A0 = R2.H * R1.H" LOADG ";\n\t"                                                           \
    "A1 += R1.H * R2.L (M);\n\t"                                                                                     \
    "A1 = A1 >>> 15 || R3 = [I1++] || NOP;\n\t"                /* R3 pre-emphasis a */                              \
    "R2 = (A0 += A1);\n\t"                                                                                           \
    "R6 = R6 + R2;\n\t"                                                                                              \
    "R0 = R0 - R6;\n\t"                                        /* tight */                                          \
    "R1 = R0 - R7;\n\t"                                        /* d = s - pre-emphasis lp */                        \
    "A1 = R3.H * R1.L (M), A0 = R3.H * R1.H || R2 = [I1++] || NOP;\n\t"   /* R2 e (1 - a) */                        \
    "A1 += R1.H * R3.L (M);\n\t"                                                                                     \
    "A1 = A1 >>> 15;\n\t"                                                                                            \
    "R3 = (A0 += A1);\n\t"                                                                                           \
    "R7 = R7 + R3;\n\t"                                                                                              \
    "A1 = R2.H * R1.L (M), A0 = R2.H * R1.H || I1 -= M1 || NOP;\n\t"                                                 \
    "A1 += R1.H * R2.L (M);\n\t"                                                                                     \
    "A1 = A1 >>> 15;\n\t"                                                                                            \
    "R3 = (A0 += A1);\n\t"                                                                                           \
    "R0 = R0 + R3;\n\t"                                        /* s + e (s - new lp) */                             \
    "R0 <<= 4;\n\t"                                                                                                  \
    "A1 = R4.H * R0.L (M), A0 = R4.H * R0.H;\n\t"                                                                    \
    "A1 += R0.H * R4.L (M);\n\t"                                                                                     \
    "A1 = A1 >>> 15;\n\t"                                                                                            \
    "R0 = (A0 += A1);\n\t"                                     /* drive, Q20 */                                     \
    "R0 = R0 << 6 (S);\n\t"                                                                                          \
    "R0 >>>= 6;\n\t"                                           /* saturated to +-32 */                              \
    "R3 = R0 + R5 (NS) || R0 = [I0++] || NOP;\n\t"             /* + bias; the next s */
static __attribute__((noinline)) void drive_pass(struct amp_ram *g, int32_t bias)
{
    __asm__ volatile(
        "I0 = %[x];\n\tI1 = %[pc];\n\tI2 = %[v];\n\tI3 = %[gd];\n\tR0 = 12;\n\tM1 = R0;\n\t"
        "R5 = %[bias];\n\tR6 = [%[st]];\n\tR7 = [%[st] + 4];\n\tR3 = [%[v] + 4 * %[n2]];\n\tR0 = [I0++];\n\t"
        "R1 = %[n];\n\tLC0 = R1;\n\t"
        "LSETUP (1f, 2f) LC0;\n1:\t"
        DRIVE_SMP(" || R4 = [I3++] || NOP")
        DRIVE_SMP("")
        "2:\tNOP;\n\t"
        "[I2++] = R3;\n\t"
        "[%[st]] = R6;\n\t[%[st] + 4] = R7;\n"
        :
        : [x] "a"(g->z), [pc] "a"(g->pc), [v] "a"(g->in1), [gd] "a"(g->gd), [bias] "a"(bias), [st] "a"(&g->tight),
          [n] "i"(AMP_IN), [n2] "i"(AMP_OUT)
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "A0", "A1", "I0", "I1", "I2", "I3", "M1", "cc", "LC0", "LT0",
          "LB0", "memory");
}

/* 2^14 / (1 + i / 64), i = 0 .. 64: reciprocal, linearly interpolated (7.6e-5 relative error); distortion.c's */
static const uint16_t recip_tab[65] = {
    16384, 16132, 15888, 15650, 15420, 15197, 14980, 14769, 14564, 14364, 14170, 13981, 13797, 13618, 13443, 13273,
    13107, 12945, 12788, 12633, 12483, 12336, 12193, 12053, 11916, 11782, 11651, 11523, 11398, 11275, 11155, 11038,
    10923, 10810, 10700, 10592, 10486, 10382, 10280, 10180, 10082, 9986, 9892, 9800, 9709, 9620, 9533, 9447,
    9362, 9279, 9198, 9118, 9039, 8962, 8886, 8812, 8738, 8666, 8595, 8525, 8456, 8389, 8322, 8257, 8192,
};

#define LOOP_IN "R1 = %[n];\n\tLC0 = R1;\n\tLSETUP (1f, 2f) LC0;\n1:\t"
#define LIST "[%[lp]] = %[y];\n\t%[lq] = %[lp] + %[four];\n\tIF CC %[lp] = %[lq];\n\t"
#define LOOP_OUT [up] "+a"(up), [y] "+a"(y), [lp] "+a"(lp), [lq] "=&a"(lq)
#define LOOP_CLOBBERS "R1", "R2", "R3", "A0", "M3", "cc", "LC0", "LT0", "LB0", "memory"

/* The closed-form mean of each pair (a, b) clamped to the knee (16-bit products for the cubic, the linear term
 * exact); a pair whose ends lie on different sides of +-1.5 goes on the list (distortion.c's tube_fast). */
static __attribute__((noinline)) int32_t **tube_fast(const int32_t *v, int32_t *y, int32_t **lp)
{
    int32_t a = amp_lim(v[0], -AMP_KNEE, AMP_KNEE), am = v[0] - AMP_KNEE, ap = v[0] + AMP_KNEE, k = AMP_KNEE, nk = -AMP_KNEE;
    const int32_t *up = v + 1;
    int32_t **lq;
    __asm__ volatile(
        "R2 = %[c27] (X);\n\tM3 = R2;\n\t"
        LOOP_IN
        "R1 = [%[up]++];\n\t"
        "R2 = R1 - %[k];\n\t"
        "%[am] = %[am] ^ R2;\n\t"
        "R3 = R1 + %[k];\n\t"
        "%[ap] = %[ap] ^ R3;\n\t"
        "%[am] = %[am] | %[ap];\n\t"
        "CC = %[am] < 0;\n\t"
        "%[am] = R2;\n\t"
        "%[ap] = R3;\n\t"
        "R1 = MAX (R1, %[nk]);\n\t"
        "R1 = MIN (R1, %[k]);\n\t"           /* b clamped */
        "R2 = %[a] >>> 7;\n\t"               /* a, b in Q13 */
        "R3 = R1 >>> 7;\n\t"
        "A0 = R2.L * R2.L (IS);\n\t"
        "A0 += R3.L * R3.L (IS);\n\t"        /* a^2 + b^2, Q26 */
        "R3 = R2 + R3;\n\t"
        "R2 = M3;\n\t"
        "R3 = R3.L * R2.L (IS);\n\t"
        "R3 >>>= 15;\n\t"                    /* (a + b) / 27, Q13 */
        "R2 = A0;\n\t"
        "R2 >>>= 14;\n\t"                    /* Q12 */
        "R2 = R2.L * R3.L (IS);\n\t"
        "R2 >>>= 1;\n\t"                     /* (a + b) (a^2 + b^2) / 27, Q24 */
        "R3 = %[a] + R1;\n\t"
        "R3 <<= 3;\n\t"
        "R3 = R3 - R2;\n\t"
        "[%[y]++] = R3;\n\t"
        LIST
        "2:\t%[a] = R1;\n"
        : LOOP_OUT, [a] "+d"(a), [am] "+d"(am), [ap] "+d"(ap)
        : [k] "d"(k), [nk] "d"(nk), [four] "a"(4), [c27] "i"(1214), [n] "i"(2 * (AMP_OUT / 2))
        : LOOP_CLOBBERS);
    return lp;
}

/* Straddling pairs: (F(b) - F(a)) / (b - a), F = v^2 / 2 - v^4 / 27 inside the knee, + |v| - 1.5 beyond it; for a
 * step below 2^-10 the closed form stays (distortion.c's FIX with F_TUBE: SIGNBITS-normalised |du|, recip_tab). */
#define SQ(X, Y) /* Y = X * X >> 31 */                                                                              \
    "A1 = " X ".L * " X ".L (FU);\n\tA1 = A1 >> 16;\n\tA0 = " X ".H * " X ".H, A1 += " X ".H * " X ".L (M);\n\t"     \
    "A1 += " X ".H * " X ".L (M);\n\tA1 = A1 >>> 15;\n\t" Y " = (A0 += A1);\n\t"
#define F_TUBE(I, O, T1, T2) /* v^2 / 2 - v^4 / 27 inside */                                                         \
    O " = MAX (" I ", R7);\n\t" O " = MIN (" O ", R6);\n\t" T1 " = ABS " I ";\n\t" T2 " = ABS " O ";\n\t"           \
    T1 " = " T1 " - " T2 ";\n\t" T1 " <<= 4;\n\t" O " <<= 9;\n\t" SQ(O, O) SQ(O, T2) O " >>>= 4;\n\t"             \
    O " = " O " + " T1 ";\n\t" T1 " = M3;\n\tA1 = " T1 ".L * " T2 ".L (M);\n\tA0 = " T2 ".H * " T1 ".L;\n\t"       \
    "A1 = A1 >>> 15;\n\t" T2 " = (A0 += A1);\n\t" O " = " O " - " T2 ";\n\t"
#define FIX(F, K)                                                                                                    \
    __asm__ volatile(                                                                                                \
        "R0 = 1;\n\tR1 = R0 << 24;\n\tM1 = R1;\n\tR1 = R0 << 20;\n\tM2 = R1;\n\t"                            \
        "R1 = 2427 (X);\n\tM3 = R1;\n\tR6.H = %[kh];\n\tR6.L = %[kl];\n\tR7 = -R6;\n\t"                               \
        "LSETUP (1f, 2f) LC0 = %[n];\n"                                                                              \
        "1:\tP1 = [%[lp]++];\n\tP1 += -4;\n\tI0 = P1;\n\tP2 = P1 + %[off];\n\tR0 = [P2];\n\tR1 = [P2 + 4];\n\t"         \
        F("R1", "R2", "R3", "R4") F("R0", "R5", "R3", "R4")                                                          \
        "R2 = R2 - R5;\n\tR1 = R1 - R0;\n\tR5 = ABS R1;\n\tR5 >>= 10;\n\t"     /* dF, du; R5 = 0: small step */      \
        "CC = R1 < 0;\n\tR3 = -R2;\n\tIF CC R2 = R3;\n\tR1 = ABS R1;\n\t"                                          \
        "R3.L = SIGNBITS R1;\n\tR3 = R3.L (X);\n\tR0 = LSHIFT R1 BY R3.L;\n\t"  /* normalised |du| */               \
        "R1 = R0 >> 24;\n\tBITCLR (R1, 6);\n\tP1 = R1;\n\tR0 >>= 8;\n\tP1 = %[tab] + (P1 << 1);\n\t"                 \
        "R1 = W [P1] (Z);\n\tR4 = W [P1 + 2] (Z);\n\tR4 = R1 - R4;\n\tR4 = R4.L * R0.L (FU);\n\tR4 >>= 16;\n\t"       \
        "R1 = R1 - R4;\n\t"                                                     /* 2^14 / normalised |du| */         \
        "R3 += -5;\n\tR2 = ASHIFT R2 BY R3.L;\n\t"                                                                   \
        "A1 = R1.L * R2.L (M);\n\tA0 = R2.H * R1.L;\n\tA1 = A1 >>> 15;\n\tR2 = (A0 += A1);\n\tR2 >>>= 4;\n\t"          \
        "CC = R5 == 0;\n\tR0 = [I0];\n\tIF CC R2 = R0;\n"                                                         \
        "2:\t[I0] = R2;\n"                                                                                           \
        : [lp] "+a"(lp)                                                                                              \
        : [off] "a"(off), [tab] "a"(recip_tab), [n] "a"(n), [kh] "i"((K) >> 16), [kl] "i"((K) & 0xFFFF)              \
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P1", "P2", "I0", "A0", "A1", "M1", "M2", "M3", "cc",       \
          "LC0", "LT0", "LB0", "memory")

#define FIXMAX 32                           /* straddling pairs recomputed per stage and block (CPU bound) */

/* The tube with ADAA over the block: v[0 .. AMP_OUT] in (Q20), y[0 .. AMP_OUT - 1] out (Q24). */
static __attribute__((noinline)) void adaa(struct amp_ram *g, const int32_t *v, int32_t *y)
{
    int32_t **lp = g->list, **end = tube_fast(v, y, lp), n = end - lp, off = (const char *)v - (const char *)y;
    n = n > FIXMAX ? FIXMAX : n;
    if (n > 0)
        FIX(F_TUBE, AMP_KNEE);
}

/* Interstage: two one-pole low-passes, il1 += a (y1 - dc - il1), il2 += a (il1 - il2), on stage 1's output z without
 * its DC (silence stays exactly 0), then il2 x gain (Q26, ramped by its step per sample) -> in2[1 ..] (Q24, into the
 * tone stack); in2[0] gets the last block's last value (stage 2's input, Q20) through the pipelined store. */
static __attribute__((noinline)) void inter_pass(struct amp_ram *g, int32_t dc, int32_t gain, int32_t dgain)
{
    g->ic[0] = dc;
    g->ic[1] = gain;
    g->ic[2] = dgain;
    __asm__ volatile(
        "I0 = %[z];\n\tI2 = %[w];\n\tR4 = %[a];\n\tR5 = [%[ic]];\n\tR6 = [%[ic] + 4];\n\tI3 = R6;\n\t"
        "R6 = [%[ic] + 8];\n\tM3 = R6;\n\tR6 = [%[st]];\n\tR7 = [%[st] + 4];\n\t"
        "R3 = [%[w] + 4 * %[n]];\n\tR0 = [I0++];\n\t"
        "R1 = %[n];\n\tLC0 = R1;\n\t"
        "LSETUP (1f, 2f) LC0;\n"
        "1:\tR1 = R0 - R5 (NS) || [I2++] = R3 || R0 = [I0++];\n\t"
        "R1 = R1 - R6;\n\t"
        "A1 = R4.H * R1.L (M), A0 = R4.H * R1.H;\n\t"
        "A1 += R1.H * R4.L (M);\n\t"
        "A1 = A1 >>> 15;\n\t"
        "R2 = (A0 += A1);\n\t"
        "R6 = R6 + R2;\n\t"                    /* il1 */
        "R1 = R6 - R7;\n\t"
        "A1 = R4.H * R1.L (M), A0 = R4.H * R1.H;\n\t"
        "A1 += R1.H * R4.L (M);\n\t"
        "A1 = A1 >>> 15;\n\t"
        "R2 = (A0 += A1);\n\t"
        "R7 = R7 + R2;\n\t"                    /* il2 */
        "R1 = R7 << 5;\n\t"
        "R2 = I3;\n\t"
        "A1 = R2.H * R1.L (M), A0 = R2.H * R1.H || I3 += M3 || NOP;\n\t"
        "A1 += R1.H * R2.L (M);\n\t"
        "A1 = A1 >>> 15;\n"
        "2:\tR3 = (A0 += A1);\n\t"
        "[I2++] = R3;\n\t"
        "[%[st]] = R6;\n\t[%[st] + 4] = R7;\n"
        :
        : [z] "a"(g->z), [w] "a"(g->in2), [a] "a"(AMP_INTER_A), [ic] "a"(g->ic), [st] "a"(&g->inter), [n] "i"(AMP_OUT)
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "A0", "A1", "I0", "I2", "I3", "M3", "cc", "LC0", "LT0", "LB0",
          "memory");
}

/* tube_fast's mean for the constant input bias (Q24): stage 1's DC, removed after the interstage low-pass. */
static int32_t tube_dc(int32_t bias)
{
    int32_t a = bias >> 7, m = ((((a * a * 2) >> 14) * (((2 * a) * 1214) >> 15)) >> 1);
    return (bias << 4) - m;
}

static void amp_init(struct amp_ram *g, const int16_t *params)
{
    uint32_t *w = (uint32_t *)g;
    for (uint32_t i = 0; i < sizeof *g / 4; i++)
        w[i] = 0;
    for (int j = 0; j < 4; j++)
        g->knob[j] = amp_knob(params[j]);
    g->blk = 1;
    g->lim_gain = 32767;
    g->magic = AMP_MAGIC;
}

/* The block's output y[] from in[] (the DSP; run once per block). */
static void amp_dsp(struct amp_ram *g, const int16_t *params)
{
    /* knobs, slewed */
    for (int j = 0; j < 4; j++) {
        int32_t t = amp_knob(params[j]), k = g->knob[j];
        k = t > k + SLEW ? k + SLEW : t < k - SLEW ? k - SLEW : t;
        g->knob[j] = k;
    }
    struct amp_svf *f = g->f;                                       /* in place: a copy would cost 224 B of stack */
    int32_t row[AMP_ORANGE_W], thr = 0;
    knob_row(amp_green, AMP_GREEN_W, g->knob[1], row);
    set_svf(&f[F_LOW], row);
    set_svf(&f[F_HIGH], row + 5);                                   /* m1, m2 already divided by m0 = A^2 */
    /* the high shelf's A^2 x P (stage 2 sees the stack at a peak gain of 0 dB) and 1 / P - 1 (the make-up), Q29 */
    const int32_t a2p = row[10], mk1 = row[11];
    set_svf(&f[F_SCOOP], amp_scoop);
    knob_row(amp_orange, AMP_ORANGE_W, g->knob[3], row);
    for (int j = 0; j < 5; j++)
        set_svf(&f[F_HP + j], row + 5 * j);
    if (g->knob[2])
        knob_row(amp_gate_thr, 1, g->knob[2], &thr);
    knob_row(amp_blue, AMP_BLUE_W, g->knob[0], row);
    const int32_t gain = row[0], tight_a = row[1], emph = row[2] << 15, bias = row[3], k2 = row[4];
    /* loudness x the make-up 1 + (1 / P - 1) c (c: all of it on the dark side, the bright side's share at this drive),
     * Q27 (<= 4.3 x 3.2) */
    const int32_t mkc = g->knob[1] < 16384 ? 32768 : row[6];
    const int32_t comp = fmul(row[5] << 16, (1 << 29) + (mk1 >> 15) * mkc) << 1;
    /* stage-2 gain 1.5 k2 x A^2 P (the high shelf runs normalised, as v0 + .. / A^2), Q26 (<= 6.3) */
    const int32_t g2 = fmul((k2 + (k2 >> 1)) << 14, a2p) << 2;
    /* drive gain (per input sample), stage-2 gain and make-up (per output sample) ramp linearly from the last block's
     * values, so a knob step isn't a level step at the block edge */
    if (!g->dsp_blocks) {
        g->gain_prev = gain;
        g->comp_prev = comp;
        g->g2_prev = g2;
    }
    const int32_t dgain = (gain - g->gain_prev) >> 6, dcomp = (comp - g->comp_prev) >> 7;
    int32_t gk = g->gain_prev, ck = g->comp_prev;
    /* drive pass coefficients: e (s - new lp) = e (1 - a) (s - old lp) */
    g->pc[0] = tight_a;
    g->pc[1] = AMP_EMPH_A;
    g->pc[2] = fmul(emph, (int32_t)(0x7FFFFFFF - AMP_EMPH_A)) << 1;

    int16_t *b = g->buf;                                            /* 24 samples of history, then the block */

    int32_t env = g->env, hold = g->hold, open = g->gate_open, gg = g->gate_gain;
    int32_t thr_c = thr >> 1;
    int32_t lg = g->lim_gain, peak = 0, in_peak = 0;
    /* limiter: the gain for this block from the last block's peak (out_peak, Q15) */
    int32_t lt = 32767, p = g->out_peak;
    if (p > (LIM_T >> 9))
        lt = (int32_t)udiv((uint32_t)(LIM_T >> 9) << 15, (uint32_t)p);
    int32_t lnew = lt < lg ? lt : lg + ((32767 - lg) >> 6);
    if (lnew > lt)
        lnew = lt;
    int32_t ldelta = lnew - lg;
    int16_t *y = g->y;
    int32_t *zb = g->z;

    /* gate detector on the raw input (it runs AMP_HB_K samples ahead of the delayed audio: a little lookahead); the
     * gate gain multiplies the drive gain (after the tight and pre-emphasis filters: they are linear) */
    for (int i = 0; i < AMP_IN; i++) {
        int32_t x = b[AMP_HIST + i], ax = x < 0 ? -x : x;
        in_peak = ax > in_peak ? ax : in_peak;
        ax <<= 9;                                                   /* Q24 */
        env -= env >> 9;                                            /* 11.6 ms decay */
        env = ax > env ? ax : env;
        if (!thr) {
            open = 1;
        } else if (env > thr << 9) {
            open = 1;
            hold = HOLD;
        } else if (env > thr_c << 9) {
            if (open)
                hold = HOLD;
        } else if (hold > 0) {
            hold--;
        } else {
            open = 0;
        }
        gg = open ? gg + (((1 << 30) - gg) >> 5) : gg - (gg >> 12); /* 0.7 ms attack, 93 ms release */
        g->gd[i] = fmul(gk, gg) << 1;                               /* Q23 */
        gk += dgain;
    }
    if (!g->dsp_blocks)
        g->in1[AMP_OUT] = bias;                                     /* stage 1's previous input: at rest */
    hb_pass(g, zb);                                                 /* 2x up */
    drive_pass(g, bias);                                            /* tight, pre-emphasis, drive -> in1 */
    adaa(g, g->in1, zb);                                            /* stage 1, Q24 */
    inter_pass(g, tube_dc(bias), g->g2_prev, (g2 - g->g2_prev) >> 7);  /* interstage, x stage-2 gain -> in2 */
    g->g2_prev += ((g2 - g->g2_prev) >> 7) << 7;
    svf_block(&f[F_LOW], g->in2 + 1, g->in2 + 1, SHELF);            /* tone stack, Q24 */
    svf_block(&f[F_HIGH], g->in2 + 1, g->in2 + 1, SHELF);
    svf_block(&f[F_SCOOP], g->in2 + 1, g->in2 + 1, BELL20);         /* -> Q20 */
    adaa(g, g->in2, zb);                                            /* stage 2, Q24 */
    svf_block(&f[F_HP], zb, zb, HP);                                /* cab */
    svf_block(&f[F_MID], zb, zb, BELL);
    svf_block(&f[F_PRES], zb, zb, BELL);
    svf_block(&f[F_LP1], zb, zb, LOW);
    svf_block(&f[F_LP2], zb, zb, LOW);
    for (int n = 0; n < AMP_OUT; n++) {
        int32_t z = fmul(zb[n] << 4, ck);                           /* loudness */
        ck += dcomp;
        int32_t az = z < 0 ? -z : z;
        peak = az > peak ? az : peak;
        int32_t lgi = lg + ((ldelta * n) >> 7);
        y[n] = soft_knee(qmul(z, lgi << 16) >> 9);                  /* limiter, knee */
    }

    for (int i = 0; i < AMP_HIST; i++)
        b[i] = b[AMP_IN + i];
    g->env = env;
    g->hold = hold;
    g->gate_open = open;
    g->gate_gain = gg;
    g->gain_prev = gk;                                              /* = gain and comp within the ramps' truncation */
    g->comp_prev = ck;
    g->lim_gain = lnew;
    g->out_peak = peak >> 9;
    g->in_peak = in_peak;
    g->dsp_blocks++;
}

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct amp_ram *g = &amp_ram;
    struct voice *s = st;
    uint32_t me = (uint32_t)st;
    (void)pitch;
    if (g->magic != AMP_MAGIC)
        amp_init(g, params);
    g->renders++;

    /* 1. a new TE block? (TE writes TE IN before the voice loop; it doesn't change during the callback) */
    int16_t *in = g->buf + AMP_HIST;
    int diff = 0, zero = 1;
    for (int i = 0; i < AMP_IN; i++) {
        int16_t v = te_audio_in[i];
        diff |= v != in[i];
        zero &= v == 0;
    }
    uint32_t sel = te_rx_sel & 1;
    if (diff || sel != g->sel || (s->stamp == g->blk && zero)) {
        g->blk++;
        g->sel = sel;
        g->cur_owner = g->owner;                              /* gains follow the owner as of the block start */
        for (int i = 0; i < AMP_IN; i++)
            in[i] = te_audio_in[i];
    }
    uint32_t blk = g->blk;
    s->stamp = blk;

    /* 2. ownership */
    int fresh = s->mark == 0x80000000u;
    if (fresh) {
        s->mark = 0;
        s->released = 0;
    }
    uint32_t own = g->owner;
    if (own != me && (fresh || !own || g->owner_blk + 1 < blk || (((struct voice *)own)->released && !s->released))) {
        g->owner = me;
        g->takeovers++;
    }
    if (g->owner == me)
        g->owner_blk = blk;
    /* the gains step towards the owner as of this block's start, so a takeover in the middle of the voice loop moves
     * every voice's gain from the next block on: the gains always sum to one */
    int target = g->cur_owner == me;

    /* 3. the block's output: computed by the first voice to render in it (every block, so the DSP state runs on
     * through handovers) */
    if (g->y_blk != blk) {
        amp_dsp(g, params);
        g->y_blk = blk;
    }

    /* 4. this voice's gain: find its entry (or the stalest one, if it needs one) */
    int e = -1, old = 0;
    for (int j = 0; j < AMP_VOICES; j++) {
        if (g->v[j].id == me)
            e = j;
        if (g->v[j].blk < g->v[old].blk)
            old = j;
    }
    if (e < 0 && !target) {
        for (int i = 0; i < AMP_OUT; i++)
            out[i] = 0;
        return;
    }
    if (e < 0) {
        e = old;
        g->v[e].id = me;
        g->v[e].gain = 0;
        g->v[e].blk = 0;
    }
    int32_t g1 = g->v[e].gain;
    if ((fresh && g->cur_owner != me) || g->v[e].blk + 1 < blk)   /* new note, back after a gap: fade in */
        g1 = 0;
    int32_t g0 = g1;
    if (g->v[e].blk != blk) {                                 /* one step per block */
        g1 = target ? g1 + 32768 / AMP_FADE : g1 - 32768 / AMP_FADE;
        g1 = g1 > 32767 ? 32767 : g1 < 0 ? 0 : g1;
        g->v[e].gain = g1;
        g->v[e].blk = blk;
    }                                                         /* (rendering again in the same block: hold) */
    if (!g0 && !g1) {
        for (int i = 0; i < AMP_OUT; i++)
            out[i] = 0;
        return;
    }
    const int16_t *y = g->y;
    int32_t d = g1 - g0;
    for (int i = 0; i < AMP_OUT; i++)
        out[i] = (int16_t)((y[i] * (g0 + ((d * i) >> 7))) >> 15);
}
