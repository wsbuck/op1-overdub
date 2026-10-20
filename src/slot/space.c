/* "space" FX (src/slot/fx.json): an algorithmic reverb from rooms to cathedrals, with shimmer and hold modes. Design:
 * notes/fx-reverb.md (§2 architecture, §5 Option A); as built: notes/fx-reverb.md §11; knob maps: space.h; FX
 * contract: notes/fx-slot.md (ensemble.c).
 *
 * Signal flow per instance (mono: one channel, L's patterns), at the tank rate (master 44.1 kHz, or 22.05 kHz with
 * SPACE_MASTER_HALF; synth / drum always 22.05 kHz: a half-band decimator in, an interpolator out):
 *   in -> predelay -> 8 early-reflection taps (integer delays) -> 2 allpasses (g 0.6)  -----------------+-> wet
 *              \-> direct x 0.5 + ER x 0.7, x the input gain (0.5 RT60^-1/4; x (1 - freeze); INFINITE's
 *                  level servo) -> 2nd-order low cut (two one-poles) -> one-pole bandwidth -> 4 allpasses
 *                  (g = the space's diffusion) -> + the shimmer return -> FDN inputs                       |
 *   FDN-8, block-staged (every line > 64 samples): stage 1 per line: read (linear interpolation; 4 lines
 *   modulated, all glide), an in-line allpass (13 % of the line, g +-0.5), the two-band absorbent filter
 *   g_hi y + (g_lo - g_hi) LP(y), with 1/sqrt 8 folded into the gains -> Q18 (3 guard bits); stage 2 per frame:
 *   the Hadamard in 32-bit (its rows 1 and 2 are the late outputs L, R), + the inputs on rows 3 and 4's signs,
 *   requantised to 16 bits toward zero (nearest while frozen / INFINITE); stage 3 per line: the writes.  -----+
 *   shimmer: (L + R) / 2 of the late output -> one-pole high-pass 300 Hz -> biquad low-pass (anti-alias) -> a ring;
 *   two-head delay-line pitch shifters (+12; +12 and +19), sine windows, each head's start offset jittered per grain
 *   (the pitch stays exact), x min(1, 4 sqrt(1 - g^2)) -> soft limit 0.5 FS -> the next block's FDN input.
 *   FREEZE: the loop gain 1 - 2^-13, the input muted (50 ms crossfade); in a shimmer zone the return is servoed so
 *   the tank holds its level while the octaves build (frozen shimmer). INFINITE: the same loop gain, the input open
 *   with its gain servoed on the tank level.
 *   master DUCK: a peak follower on the dry input ducks the wet (MIX above the middle).
 * Every recursive requantisation rounds toward zero (the tail ends in exact silence, no limit cycles); the damping
 * low-passes keep a 32-bit state updated exactly. No division in the sample loops.
 *
 * Memory: pingpong's FREE SDRAM lines (pingpong_ram, 0x01E80000), shared with pingpong and repeat through
 * fx_line_owner (pingpong.h): the stereo set (256 KB) for the master, the mono line (128 KB) for synth / drum. A clean
 * start (taking the set over, the first call, or the FX re-enabled after a gap) clears the set, 8 KB per call, with
 * the wet muted (~45 ms on the master at 44.1 kHz), so nothing another FX left is ever heard. The hot loops are
 * hand-written (space_asm.h); SPACE_C builds the C reference (bit exact with them, space_check.py).
 */
#include <stdint.h>
#include "space.h"
#include "space_tables.h"
#include "pingpong.h"
#include "repeat.h"
#include "fxpub.h"

#ifdef __bfin__
#define RAM __attribute__((section(".data")))
static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}
#else
#define RAM
static inline uint32_t cycles(void) { return 0; }
#endif
#ifndef SPACE_MASTER_HALF
#define SPACE_MASTER_HALF 0                 /* build option (build.py --space-tank 22): the master tank at 22.05 kHz */
#endif

#define BLOCK 64
#define NL 8
#define RN 11585                            /* 1 / sqrt 8, Q15 */
#define GFRZ ((1 << 30) - (1 << 17))        /* the held loop gain 1 - 2^-13, Q30 */
#define HOLD_STEP 952                       /* the hold crossfades: 50 ms, Q15 per call */
#define SLEW 1024                           /* line lengths glide at most 1/64 sample per sample (Q16) */
#define THR 1966                            /* the duck's threshold, -24 dBFS (0.06), Q15 */
#define INF_LV 1966                         /* INFINITE's input servo: gain 1 / (1 + level / 0.06) */
#define CLEAR_WORDS 4096                    /* a clean start clears 8 KB per call */
#define SH_W 4410                           /* the shimmer window at 44.1 kHz (100 ms: a 10 Hz grain rate) */
#define NEAR_HI 4                           /* the loop rounds to nearest above this tank level (-78 dBFS), */
#define NEAR_LO 3                           /* toward zero below this one (-81 dBFS) */
#define STALE 64                            /* calls missed (~93 ms): the FX was off, start clean */
#define LOG2_HP_SPLIT 41681                 /* log2 1.554, Q16: two one-poles at fc / 1.554 are -3 dB at fc */
#define LOG2_15 38337                       /* log2 1.5, Q16 */
#define LOG2_03 (-113836)                   /* log2 0.3, Q16 */
#define LOG2_300 539284                     /* log2 300 Hz, Q16 */
#define LOG2_20 283242                      /* log2 20 Hz, Q16 */

void space_mono(void *self, int16_t *buf, const int16_t *p);
void space_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);

extern int16_t pingpong_ram[];              /* stereo L, stereo R, mono: 3 x 65536 int16 (pingpong.c) */
extern volatile uint32_t te_blocks;         /* 0x01AADAF4: TE's audio block counter (+1 per callback) */
/* TE's stored FX knobs and slots (the TAIL zone on synth / drum: the stored knob, not TE's LFO; fx-slot.md "LFO") */
extern volatile int32_t te_fx_slot[2];      /* 0x01AAAC0C: [0] the synth slot, [1] the drum slot */
extern volatile int16_t te_fx_knobs_synth[64], te_fx_knobs_drum[64];   /* 0xFF801444, 0xFF8015C4: 8 x 8 int16 */
#ifndef SPACE_NO_KEY
extern volatile struct rep_key rep_key __attribute__((weak));       /* repeat.c: the green push (momentary freeze) */
#endif

#ifdef SPACE_DIAG
int32_t space_diag_peak;                    /* the largest line write (space_check.py's headroom cases) */
#endif
struct space_ui space_ui[2] RAM = { { .blocks = 0 }, { .blocks = 0 } };
struct fx_pub space_pub[2] RAM = { { { 0 }, 0 }, { { 0 }, 0 } };

#ifdef SPACE_PROF                           /* the simulator's per-stage CYCLES (space_check.py --prof) */
uint32_t space_prof[12], space_prof_last;
#define SPROF(i) do { uint32_t c_ = cycles(); space_prof[i] += c_ - space_prof_last; space_prof_last = c_; } while (0)
#define SPROF_START() (space_prof_last = cycles())
#else
#define SPROF(i) do { } while (0)
#define SPROF_START() do { } while (0)
#endif

/* ---- the instance ---- */
struct sp {
    /* the line set (laid out at a clean start) */
    int16_t *line[NL], *lap[NL], *pd[2], *ap[8], *erap[4], *shr;
    uint32_t lmask, lapmask, pdmask, apmask[8], erapmask[4], shmask, shwords, words;
    int n, lgn, half, nch;
    uint32_t clr;                           /* words cleared so far: below `words` the set is still being cleared */
    uint32_t last_blk;
    uint8_t started, running, first, zone, hold_key, sh_on, near;
    uint32_t t;                             /* tank frames since the clean start */
    int32_t ks[4];                          /* knobs, smoothed, Q4 */
    int32_t tx;                             /* TONE, smoothed, Q15 */
    /* FDN */
    int32_t d[NL], dn[NL], base[NL];        /* read delay now and at the block's end, glided base (Q16 samples) */
    int32_t tgt[NL];                        /* the base's target (from SPACE) */
    uint32_t mstep[NL], lm[NL];             /* the modulation's phase step a block; the loop's delay (samples) */
    uint32_t mph[NL];
    int32_t lo[NL];                         /* damping low-passes, Q31 */
    uint32_t k1[NL][20];                    /* stage 1's per-line jobs (the asm's; with lo) */
    int32_t gh[NL], gd[NL];                 /* gains, Q15, x 1/sqrt 8 */
    int32_t lap_len[NL], ap_len[8], erap_len[4], pre, ert[2][8], erg[8], erlev, diff, axo, ahp, alp, ash;
    int32_t v[SP_NF];                       /* the space at blue (smoothed): sp_pt interpolated */
    int32_t hi, ig0, glo[NL];               /* the HF ratio after TONE; the input gain before the holds; loop gains */
    uint32_t c, ch, last_m[NL];             /* log2 loss per sample (Q32) below / above the crossover */
    int32_t last_ks0, last_tx, last_L, last_hh, shk;
    int32_t filt[2][3];                     /* the input's two high-passes and low-pass per channel, Q31 */
    int32_t erdc[2], erlp[2], adc;          /* the ERs' DC blocker and low-pass */
    int32_t sd, sd_t;                       /* the direct send (0.5 x the input gain) now and its target, Q15 */
    int32_t frz, inf;                       /* hold crossfades, Q15 */
    int32_t lv, frz_target;                 /* tank level (Q15), the frozen-shimmer servo's target */
    int32_t ef, duck, dhold;                /* the duck's follower and gain (Q15), its hold (calls) */
    int32_t wg, dg, wet, dry;               /* wet (with the duck) and dry gains now; MIX's wet and dry, Q15 */
    int32_t t60_ms;
    /* shimmer */
    uint32_t shph[2], rng;
    int32_t shd0[2][2];                     /* each head's start offset (samples), jittered per grain */
    int32_t shg[2], shr_gain;               /* voice gains (Q15, glided), the return's gain (Q15) */
    int32_t shhp, shbq[2][4];               /* high-pass (Q31), the biquads' x1 x2 y1 y2 */
    uint32_t sht, sh_clr;                   /* the shifter's time (half the tank rate), the words of its ring cleared */
    int32_t shprev;                         /* the return's last sample (the interpolation up) */
    int16_t shf[BLOCK];                     /* the return for the next block */
    /* half-band histories */
    /* the half-band filters' buffers (k_decimate, k_interpolate; the asm's shifted copies too) */
    int16_t dbo[2][48] __attribute__((aligned(4))), dbe[2][48] __attribute__((aligned(4)));
    int16_t dbs[2][48] __attribute__((aligned(4))), ic[2][48] __attribute__((aligned(4)));
    int16_t ics[2][48] __attribute__((aligned(4)));
    /* CYCLES */
    uint32_t cmax, csum, cfdn, ccount;
};

static struct sp st[2] RAM = { { .started = 0 }, { .started = 0 } };

/* scratch, shared by both instances (they run one after the other in TE's callback; nothing carries over) */
static struct {
    int16_t x[2][BLOCK], er[2][BLOCK], u[2][BLOCK], y[BLOCK], zero[BLOCK], wet[2][BLOCK], wetf[2][BLOCK];
    int32_t acc[BLOCK], late[2][BLOCK], F[NL][BLOCK], inj[2][BLOCK];
    int16_t sh[BLOCK];
} sc RAM;

static inline int32_t knob(int16_t k)
{
    return k < 0 ? 0 : k;
}

static inline int32_t sat16(int32_t x)
{
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

static inline int32_t tz(int32_t x, int sh)  /* x / 2^sh, rounded toward zero */
{
    return (x + ((x >> 31) & ((1 << sh) - 1))) >> sh;
}

/* x / 2^sh rounded to nearest, ties to even (unbiased: no DC creeps into the loop) */
static inline int32_t rne(int32_t x, int sh)
{
    return (x + ((1 << (sh - 1)) - 1) + ((x >> sh) & 1)) >> sh;
}

/* the loop's requantisation: toward zero (the tail ends in exact silence), or to nearest (the decay exact) */
static inline int32_t rq(int32_t x, int sh, int near)
{
    return near ? rne(x, sh) : tz(x, sh);
}

static inline int32_t clampi(int32_t x, int32_t lo, int32_t hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

/* 2^-e for e >= 0 (Q16), Q30 */
static int32_t exp2_neg(int32_t e)
{
    if (e <= 0)
        return 1 << 30;
    int k = e >> 16;
    if (k >= 31)
        return 0;
    int32_t f = e & 0xFFFF, i = f >> 8, r = f & 255;
    int32_t v = sp_exp2[i] - (((sp_exp2[i] - sp_exp2[i + 1]) * r) >> 8);
    return v >> k;
}

/* 2^x (x Q16, -16 .. 14), Q16 */
static int32_t exp2_q16(int32_t x)
{
    int32_t k = x >> 16, f = x & 0xFFFF;
    int32_t v = f ? exp2_neg(65536 - f) : 1 << 29;  /* 2^(f - 1) in [0.5, 1), Q30 */
    int s = (int)k + 1 - 14;                        /* Q30 x 2^(k + 1) -> Q16 */
    if (s >= 0)
        return s > 14 ? 0x7FFFFFFF : v << s;
    return s <= -31 ? 0 : v >> -s;
}

/* one-pole coefficient for log2 f (Q16) at the tank rate, Q15 */
static int32_t coef(int32_t l2f, int half)
{
    int32_t x = l2f - (SP_COEF_LO << 16);
    if (x <= 0)
        return sp_coef[half][0];
    int32_t i = x >> 12, f = x & 4095;
    if (i >= SP_COEF_N - 1)
        return sp_coef[half][SP_COEF_N - 1];
    return sp_coef[half][i] + (((sp_coef[half][i + 1] - sp_coef[half][i]) * f) >> 12);
}

/* n / d without libgcc (the slot block links without it): shift and subtract */
static uint32_t sp_udiv(uint32_t n, uint32_t d)
{
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    return q;
}

static uint32_t isqrt(uint32_t x)          /* floor(sqrt(x)) */
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x)
        b >>= 2;
    while (b) {
        if (x >= r + b) {
            x -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

static inline int32_t tri(uint32_t ph)     /* triangle, -32768 .. 32767 */
{
    int32_t x = (int32_t)(ph >> 16);
    return x < 32768 ? 2 * x - 32768 : 2 * (65535 - x) - 32767;
}

/* ---- the kernels: the C reference here; space_asm.h replaces them on the device (bit exact: space_check.py). Integer
 * multiply-accumulates (a 40-bit accumulator, saturated to 32 bits when read), explicit rounding: the asm needs no
 * rounding mode. ---- */
static inline __attribute__((unused)) int32_t sat32(int64_t x)
{
    return x > 2147483647 ? 2147483647 : x < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)x;
}

/* v / 2^sh to nearest, ties to even, saturated to 16 bits: an accumulator read with Blackfin's unbiased rounding */
static inline int32_t rne16(int64_t v, int sh)
{
    int64_t r = (v + ((1 << (sh - 1)) - 1) + ((v >> sh) & 1)) >> sh;
    return r > 32767 ? 32767 : r < -32768 ? -32768 : (int32_t)r;
}

static inline int32_t rnd16(int64_t l)       /* a 40-bit accumulator read as Q15 */
{
    return rne16(l, 16);
}

/* 32768 / k rounded, k = 1 .. 64 (the shimmer window's linear steps) */
static const int16_t sp_rcp[65] = {
    0, 32767, 16384, 10923, 8192, 6554, 5461, 4681, 4096, 3641, 3277, 2979, 2731, 2521, 2341, 2185, 2048, 1928, 1820,
    1725, 1638, 1560, 1489, 1425, 1365, 1311, 1260, 1214, 1170, 1130, 1092, 1057, 1024, 993, 964, 936, 910, 886, 862,
    840, 819, 799, 780, 762, 745, 728, 712, 697, 683, 669, 655, 643, 630, 618, 607, 596, 585, 575, 565, 555, 546, 537,
    529, 520, 512,
};

static inline int32_t half_q(int32_t z, int near)   /* z / 2 (z 16-bit): toward zero, or to nearest even */
{
    return near ? (z + ((z >> 1) & 1)) >> 1 : (z + (z < 0)) >> 1;
}

/* one FDN line's stage-1 parameters (k_stage1_all) */
struct l1 {
    int32_t *F;
    const int16_t *ring;
    uint32_t p, inc;
    int interp;
    int16_t *lap;
    int32_t dly, sgn, *lo, gh, gd;
    uint32_t *job;                          /* the asm's own per-line state (struct k1j), kept across blocks */
};

/* one segment of a shimmer head (k_heads): acc[0 .. n) += the ring read from p + inc on x the window w (+ dw a
 * sample) */
struct hseg {
    int32_t *acc;
    uint32_t p, inc;
    int32_t w, dw, n;
};

#if defined(__bfin__) && !defined(SPACE_C)
#include "space_asm.h"
#else
/* FDN stage 1 for one line, per sample: the read at position p + (j + 1) inc (Q16 frames; interp: linear
 * interpolation, exact at fraction 0; else the whole sample); the in-line allpass (g = sgn / 2: v = y + g z stored,
 * g z requantised by half_q; out = z - g v, exact); the two-band absorbent filter F = gh out + gd LP(out) (gains
 * x 1/sqrt 8, Q15; the LP's state a 40-bit accumulator, updated exactly) -> F, Q18. The loop's requantisations: the
 * allpass's here and the line's (k_write), toward zero or to nearest even (`near`). */
static void k_stage1(int32_t *F, const int16_t *ring, uint32_t mask, uint32_t p, uint32_t inc, int interp,
                     int16_t *lap, uint32_t lmask, uint32_t t, int32_t dly, int32_t sgn, int32_t *lo, int32_t a,
                     int32_t gh, int32_t gd, int near, int n)
{
    int64_t l = *lo;
    int32_t lh = rnd16(l);
    for (int j = 0; j < n; j++) {
        p += inc;
        uint32_t i = (p >> 16) & mask, w = t + (uint32_t)j;
        int32_t y = ring[i];
        if (interp)
            y += rne(sat16(ring[(i + 1) & mask] - y) * (int32_t)((p & 0xFFFF) >> 1), 15);
        int32_t z = lap[(w - (uint32_t)dly) & lmask];
        int32_t v = sat16(y + sgn * half_q(z, near));
        lap[w & lmask] = (int16_t)v;
        l += (int64_t)2 * a * z - (int64_t)sgn * a * v - (int64_t)2 * a * lh;   /* += 2 a (out - LP) */
        lh = rnd16(l);
        int32_t f = sat32((int64_t)2 * gh * z - sgn * gh * v + 2 * gd * lh);
        F[j] = ((f >> 12) + 1) >> 1;
    }
    *lo = sat32(l);
}

/* stage 1 for all 8 lines (the asm groups them into four calls) */
static void k_stage1_all(const struct l1 *L, uint32_t mask, uint32_t lmask, uint32_t t, int32_t a, int near, int n)
{
    for (int i = 0; i < NL; i++)
        k_stage1(L[i].F, L[i].ring, mask, L[i].p, L[i].inc, L[i].interp, L[i].lap, lmask, t, L[i].dly, L[i].sgn,
                 L[i].lo, a, L[i].gh, L[i].gd, near, n);
}

/* Schroeder allpass in place, g > 0: v = x + g z stored (toward zero: an isolated recursion must die out), y = z - g v
 * = (1 - g^2) z - g x (to nearest; g2c = 1 - g^2, Q15) */
static void k_ap(int16_t *x, int16_t *ring, uint32_t mask, uint32_t t, int32_t dly, int32_t g, int32_t g2c, int n)
{
    for (int j = 0; j < n; j++) {
        uint32_t w = t + (uint32_t)j;
        int32_t z = ring[(w - (uint32_t)dly) & mask], xx = x[j];
        int32_t v = sat16(xx + ((z * g + ((z >> 31) & 0x7FFF)) >> 15));
        ring[w & mask] = (int16_t)v;
        x[j] = (int16_t)rne16(z * g2c - xx * g, 15);
    }
}

/* a chain of na allpasses in place, the gains the same for all */
static void k_aps(int16_t *x, int16_t *const *ring, const uint32_t *mask, uint32_t t, const int32_t *dly, int32_t g,
                  int32_t g2c, int na, int n)
{
    for (int a = 0; a < na; a++)
        k_ap(x, ring[a], mask[a], t, dly[a], g, g2c, n);
}

/* the tank's send: u = direct x sd + ER x se (the sends ramping per sample), to nearest, saturated */
static void k_send(int16_t *u, const int16_t *pd, uint32_t pmask, uint32_t dpos, const int16_t *er, int32_t sd,
                   int32_t dsd, int32_t se, int32_t dse, int n)
{
    for (int j = 0; j < n; j++) {
        sd += dsd;
        se += dse;
        u[j] = (int16_t)rne16(pd[(dpos + (uint32_t)j) & pmask] * sd + er[j] * se, 15);
    }
}

/* one-pole in place (its state a 40-bit accumulator, updated exactly): hp 0 -> the low-pass, 1 -> x - the low-pass */
static void k_pole(int16_t *x, int32_t *st_, int32_t a, int hp, int n)
{
    int64_t l = *st_;
    int32_t h = rnd16(l);
    for (int j = 0; j < n; j++) {
        int32_t xx = x[j];
        l += (int64_t)2 * a * xx - (int64_t)2 * a * h;
        h = rnd16(l);
        x[j] = (int16_t)(hp ? sat16(xx - h) : h);
    }
    *st_ = sat32(l);
}

/* a high-pass, then a high-pass (hp2) or a low-pass */
static void k_pole2(int16_t *x, int32_t *s1, int32_t a1, int32_t *s2, int32_t a2, int hp2, int n)
{
    k_pole(x, s1, a1, 1, n);
    k_pole(x, s2, a2, hp2, n);
}

/* the 8 early-reflection taps: er[j] = sum ring[pos_k + j] g_k (two passes of 4, each read saturated to 32 bits),
 * >> 15 */
static void k_taps(int16_t *er, const int16_t *ring, uint32_t mask, const uint32_t *pos, const int32_t *g, int n)
{
    for (int j = 0; j < n; j++) {
        int64_t a = 0;
        for (int k = 0; k < 4; k++)
            a += ring[(pos[k] + (uint32_t)j) & mask] * g[k];
        int64_t b = sat32(a);
        for (int k = 4; k < 8; k++)
            b += ring[(pos[k] + (uint32_t)j) & mask] * g[k];
        er[j] = (int16_t)sat16(sat32(b) >> 15);
    }
}

/* stage 2: the Hadamard per frame, in place (F[i][j] -> row i), and the late outputs (rows 1, 2) */
static void k_wht(int32_t F[NL][BLOCK], int32_t *latel, int32_t *later, int n)
{
    for (int j = 0; j < n; j++) {
        int32_t a0 = F[0][j], a1 = F[1][j], a2 = F[2][j], a3 = F[3][j];
        int32_t a4 = F[4][j], a5 = F[5][j], a6 = F[6][j], a7 = F[7][j];
        int32_t b0 = a0 + a1, b1 = a0 - a1, b2 = a2 + a3, b3 = a2 - a3;
        int32_t b4 = a4 + a5, b5 = a4 - a5, b6 = a6 + a7, b7 = a6 - a7;
        int32_t c0 = b0 + b2, c2 = b0 - b2, c1 = b1 + b3, c3 = b1 - b3;
        int32_t c4 = b4 + b6, c6 = b4 - b6, c5 = b5 + b7, c7 = b5 - b7;
        F[0][j] = c0 + c4;
        F[4][j] = c0 - c4;
        F[1][j] = c1 + c5;
        F[5][j] = c1 - c5;
        F[2][j] = c2 + c6;
        F[6][j] = c2 - c6;
        F[3][j] = c3 + c7;
        F[7][j] = c3 - c7;
        latel[j] = c1 + c5;
        later[j] = c2 + c6;
    }
}

/* the inputs for the line writes, x 1/sqrt 8 (Q18): P = (L + R) rn, M = (L - R) rn */
static void k_inj(int32_t *P, int32_t *M, const int16_t *ul, const int16_t *ur, int n)
{
    for (int j = 0; j < n; j++) {
        int32_t ql = ul[j] * RN, qr = ur[j] * RN;
        P[j] = (ql + qr) >> 12;
        M[j] = (ql - qr) >> 12;
    }
}

/* stage 3: one line's writes: the row + sgn x the input (inj, Q18), requantised to 16 bits (rq), saturated; n
 * divides the ring and t (no wrap inside a block); then the guard sample */
static void k_write(int16_t *ln, uint32_t mask, uint32_t t, const int32_t *row, const int32_t *inj, int32_t sgn,
                    int near, int n)
{
    int16_t *d = ln + (t & mask);
    for (int j = 0; j < n; j++)
        d[j] = (int16_t)sat16(rq(row[j] + sgn * inj[j], 3, near));
    ln[mask + 1] = ln[0];
}

/* the predelay write (n divides the ring and t: one contiguous run) */
static void k_pdw(int16_t *pd, uint32_t pmask, uint32_t t, const int16_t *x, int n)
{
    for (int j = 0; j < n; j++)
        pd[(t + (uint32_t)j) & pmask] = x[j];
}

/* clearing: nw words */
static void k_zero(uint32_t *w, uint32_t nw)
{
    for (uint32_t i = 0; i < nw; i++)
        w[i] = 0;
}

/* the largest |x| of two blocks */
static int32_t k_peak(const int16_t *a, const int16_t *b, int n)
{
    int32_t pk = 0;
    for (int j = 0; j < n; j++) {
        int32_t x = a[j] < 0 ? -a[j] : a[j], y = b[j] < 0 ? -b[j] : b[j];
        pk = x > pk ? x : pk;
        pk = y > pk ? y : pk;
    }
    return pk;
}

/* the shimmer's ring write: every second sample (the odd ones) */
static void k_ringw(int16_t *ring, uint32_t mask, uint32_t t, const int16_t *in, int n2)
{
    for (int j = 0; j < n2; j++)
        ring[(t + (uint32_t)j) & mask] = in[2 * j + 1];
}

/* the shimmer's return into the send, saturated */
static void k_add(int16_t *u, const int16_t *a, int n)
{
    for (int j = 0; j < n; j++)
        u[j] = (int16_t)sat16(u[j] + a[j]);
}

/* the shimmer's input: the late outputs >> 4 (stereo) or the one >> 3, the low 16 bits */
static void k_shin(int16_t *sh, const int32_t *l0, const int32_t *l1, int stereo, int n)
{
    for (int j = 0; j < n; j++)
        sh[j] = (int16_t)(stereo ? (l0[j] + l1[j]) >> 4 : l0[j] >> 3);
}

/* the wet: er x erlev (Q15) + late >> 2, to nearest... (the late output is Q18: >> 3, x 2), saturated; and
 * the sum of |late| (the tank level) */
static int32_t k_wet(int16_t *wet, const int16_t *er, const int32_t *late, int32_t erlev, int n)
{
    int32_t sum = 0;
    for (int j = 0; j < n; j++) {
        int32_t lt = late[j];
        wet[j] = (int16_t)sat16(((er[j] * erlev) >> 15) + (lt >> 2));
        sum += lt < 0 ? -lt : lt;
    }
    return sum;
}

/* stage 3 for all 8 lines: inputs on rows 3 and 4's signs: +P on lines 0 3, -M on 1 2, +M on 4 7, -P on 5 6 */
static const int8_t inj_of[NL] = { 0, 1, 1, 0, 1, 0, 0, 1 }, inj_sgn[NL] = { 1, -1, -1, 1, 1, -1, -1, 1 };
static void k_write_all(int16_t *const *ln, uint32_t mask, uint32_t t, int32_t F[NL][BLOCK], int32_t inj[2][BLOCK],
                        int near, int n)
{
    for (int i = 0; i < NL; i++)
        k_write(ln[i], mask, t, F[i], inj[inj_of[i]], inj_sgn[i], near, n);
}

/* dry and wet: x = x g + y w (the gains Q15 <= 32767, stepping per sample), with a soft knee above -3 dBFS (4:1, so
 * the sum peaks at -1.6 dBFS instead of clipping), to nearest */
#define MIX_KNEE 0x5A827999                 /* -3 dBFS, Q31 */
static void k_mix(int16_t *x, const int16_t *y, int32_t g, int32_t dg, int32_t w, int32_t dw, int n)
{
    for (int j = 0; j < n; j++) {
        g += dg;
        w += dw;
        int32_t v = sat32(2 * ((int64_t)x[j] * g + (int64_t)y[j] * w));
        int32_t a = v == (int32_t)0x80000000 ? 0x7FFFFFFF : v < 0 ? -v : v;
        int32_t t = a - MIX_KNEE;
        t = t < 0 ? 0 : t;
        a -= t - (t >> 2);
        v = v < 0 ? -a : a;
        x[j] = (int16_t)(((v >> 15) + 1) >> 1);
    }
}

/* the shimmer's anti-alias biquad in place, {b0, b1, b2, a1, a2} Q14, its state {x1, x2, y1, y2}: y = b x .. - a y ..
 * toward zero, saturated */
static void k_bq(int16_t *x, int32_t *st_, const int16_t *q, int n)
{
    int32_t x1 = st_[0], x2 = st_[1], y1 = st_[2], y2 = st_[3];
    for (int j = 0; j < n; j++) {
        int32_t xx = x[j];
        int32_t y = sat16(tz(q[0] * xx + q[1] * x1 + q[2] * x2 - q[3] * y1 - q[4] * y2, 14));
        x2 = x1;
        x1 = xx;
        y2 = y1;
        y1 = y;
        x[j] = (int16_t)y;
    }
    st_[0] = x1;
    st_[1] = x2;
    st_[2] = y1;
    st_[3] = y2;
}

/* one shimmer head over a segment: acc += (the ring at p, interpolated) x w >> 15, the window x gain w stepping by dw;
 * the read runs at a whole number of samples per sample (inc = r << 16): the fraction stays fixed */
static void k_head(int32_t *acc, const int16_t *ring, uint32_t mask, uint32_t p, uint32_t inc, int32_t w, int32_t dw,
                   int n)
{
    for (int j = 0; j < n; j++) {
        p += inc;
        uint32_t i = (p >> 16) & mask;
        int32_t y = ring[i];
        y += rne(sat16(ring[(i + 1) & mask] - y) * (int32_t)((p & 0xFFFF) >> 1), 15);
        acc[j] += (y * w) >> 15;
        w += dw;
    }
}

/* the heads' segments of one block (<= 8: 2 voices x 2 heads, a grain ending in each at most once) */
static void k_heads(const struct hseg *g, int ng, const int16_t *ring, uint32_t mask)
{
    for (int k = 0; k < ng; k++)
        k_head(g[k].acc, ring, mask, g[k].p, g[k].inc, g[k].w, g[k].dw, g[k].n);
}

/* the shimmer's return: x the gain g (Q15), soft limit to 0.5 FS: y = 0.75 (u - u^3 / 3), u = x / 0.75 clamped to
 * +-1; then up to the tank rate (linear: the halfway sample the mean of its neighbours, from *prev) */
static void k_shret(int16_t *out, const int32_t *acc, int32_t g, int32_t *prev, int n)
{
    int32_t pv = *prev;
    for (int j = 0; j < n; j++) {
        int32_t xx = (acc[j] * g) >> 15;
        int32_t u = clampi((xx * 43691) >> 15, -32768, 32768);
        int32_t u3 = (((u * u) >> 15) * u) >> 15;
        int32_t y = ((u - ((u3 * 10923) >> 15)) * 24576) >> 15;
        out[2 * j] = (int16_t)((pv + y) >> 1);
        out[2 * j + 1] = (int16_t)y;
        pv = y;
    }
    *prev = pv;
}

/* the half-band decimator (2:1) and interpolator (1:2), 31 taps (space_tables.h sp_hb). The decimator's input
 * b = 30 history + 64 new samples, deinterleaved: bo[k] = b[2k + 1] (the taps), be[k] = b[2k] (the centre), 47 each;
 * out[m] = 16384 b[2m+16] + sum_j hb[j] (b[2m+15-2j] + b[2m+17+2j]). The interpolator's c = 15 history + 32 new. */
static void k_decim(int16_t *out, const int16_t *bo, const int16_t *be)
{
    for (int m = 0; m < BLOCK / 2; m++) {
        int32_t a = be[m + 8] * 16384;
        for (int j = 0; j < 8; j++)
            a += sp_hb[j] * (bo[m + 7 - j] + bo[m + 8 + j]);
        out[m] = (int16_t)rne16(a, 15);
    }
}

static void k_interp(int16_t *out, const int16_t *c)
{
    for (int m = 0; m < BLOCK / 2; m++) {
        int32_t a = 0;
        for (int j = 0; j < 8; j++)
            a += sp_hb[j] * (c[m + 8 + j] + c[m + 7 - j]);
        out[2 * m] = (int16_t)rne16(a, 14);
        out[2 * m + 1] = c[m + 8];
    }
}

/* with their histories: the decimator keeps its input deinterleaved (odd samples feed the taps, even ones the centre),
 * 15 of each from the last block, then this one's (the asm keeps bs / cs too: shifted copies) */
static void k_decimate(int16_t *out, const int16_t *in, int16_t *bo, int16_t *be, int16_t *bs)
{
    (void)bs;
    for (int i = 0; i < 15; i++) {
        bo[i] = bo[32 + i];
        be[i] = be[32 + i];
    }
    for (int i = 0; i < BLOCK / 2; i++) {
        be[15 + i] = in[2 * i];
        bo[15 + i] = in[2 * i + 1];
    }
    k_decim(out, bo, be);
}

static void k_interpolate(int16_t *out, const int16_t *in, int16_t *c, int16_t *cs)
{
    (void)cs;
    for (int i = 0; i < 15; i++)
        c[i] = c[32 + i];
    for (int i = 0; i < BLOCK / 2; i++)
        c[15 + i] = in[i];
    k_interp(out, c);
}
#endif

/* ---- the line set ---- */
static const int16_t ap_size[8] = { 512, 1024, 2048, 2048, 512, 1024, 1024, 2048 };   /* at 44.1 kHz */
static const int16_t erap_size[4] = { 512, 1024, 512, 1024 };

static void layout(struct sp *s, int16_t *base)
{
    int h = s->half;
    int16_t *q = base;
    s->lmask = (8192u >> h) - 1;
    for (int i = 0; i < NL; i++, q += s->lmask + 3)
        s->line[i] = q;                     /* + the guard sample (ring[N] = ring[0]) and a pad */
    s->lapmask = (1024u >> h) - 1;
    for (int i = 0; i < NL; i++, q += s->lapmask + 1)
        s->lap[i] = q;
    s->pdmask = (16384u >> h) - 1;
    for (int c = 0; c < s->nch; c++, q += s->pdmask + 1)
        s->pd[c] = q;
    for (int i = 0; i < 4 * s->nch; i++) {
        s->apmask[i] = ((uint32_t)ap_size[i] >> h) - 1;
        s->ap[i] = q;
        q += s->apmask[i] + 1;
    }
    for (int i = 0; i < 2 * s->nch; i++) {
        s->erapmask[i] = ((uint32_t)erap_size[i] >> h) - 1;
        s->erap[i] = q;
        q += s->erapmask[i] + 1;
    }
    s->shmask = (4096u >> h) - 1;                   /* the shifter runs at half the tank rate */
    s->shr = q;
    s->shwords = s->shmask + 1;
    q += s->shmask + 3;
    s->words = (uint32_t)(q - base);
}

/* Start clean: the state reset, the set laid out and cleared over the next calls (wet muted meanwhile). */
static void clean_start(struct sp *s, int stereo)
{
    uint8_t *b = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++)
        b[i] = 0;
    s->started = 1;
    s->dg = 32768;
    s->first = 1;
    s->half = stereo ? SPACE_MASTER_HALF : 1;
    s->nch = stereo ? 2 : 1;
    s->n = BLOCK >> s->half;
    s->lgn = 6 - s->half;
    s->duck = 32768;
    s->rng = stereo ? 0x2545F491u : 0x9E3779B9u;
    layout(s, pingpong_ram + (stereo ? 0 : 2 * 65536));
    space_ui[stereo].cyc_peak = 0;
}

/* The TAIL zone on synth / drum: TE's LFO moves only the copy process gets, so the zone comes from the stored knob of
 * the context this instance runs in. Which one (synth or drum) isn't passed: the live value's zone if it is one of
 * the two stored zones, else the stored value nearest to it. */
static int tail_zone_mono(int32_t k)
{
    int zp = sp_zone(k);
    int32_t ks = knob(te_fx_knobs_synth[(te_fx_slot[0] & 7) * 8 + 2]);
    int32_t kd = knob(te_fx_knobs_drum[(te_fx_slot[1] & 7) * 8 + 2]);
    int zs = sp_zone(ks), zd = sp_zone(kd);
    if (zp == zs || zp == zd)
        return zp;
    int32_t ds = k - ks, dd = k - kd;
    return (ds < 0 ? -ds : ds) <= (dd < 0 ? -dd : dd) ? zs : zd;
}

static int key_held(int stereo)
{
#ifndef SPACE_NO_KEY
    if (!&rep_key)
        return 0;
    int ctl = stereo ? RK_MASTER : rep_key.screen_ctl > RK_DRUM ? RK_SYNTH : rep_key.screen_ctl;
    return rep_key.held[ctl] && rep_key.screen_ctl == ctl && (stereo || ctl != RK_MASTER)
           && te_blocks - rep_key.screen_seen <= REP_KEY_STALE;
#else
    (void)stereo;
    return 0;
#endif
}

static inline int32_t spms(int32_t ms_q8, int half)   /* ms (Q8) -> samples at the tank rate */
{
    return (ms_q8 * (half ? 5645 : 11290)) >> 16;
}


/* ---- per block: the knobs -> the tank's parameters. The costly parts (the space's morph, exp2s, divisions) run only
 * when their inputs moved; the modulation, the glides and the holds every block. ---- */
static void params(struct sp *s, const int16_t *p, int stereo)
{
    int h = s->half, n = s->n, first = s->first;
    for (int i = 0; i < 4; i++) {
        int32_t k = knob(p[i]) << 4;
        s->ks[i] = first ? k : s->ks[i] + ((k - s->ks[i]) >> 3);
    }
    int32_t *v = s->v;
    int space_moved = first || s->ks[0] != s->last_ks0;
    if (space_moved) {
        int d;
        int32_t f;
        sp_detent(s->ks[0] >> 4, &d, &f);
        for (int i = 0; i < SP_NF; i++)
            v[i] = sp_pt[d][i] + (((sp_pt[d + 1][i] - sp_pt[d][i]) * f) >> 10);
        s->last_ks0 = s->ks[0];
    }

    /* TAIL: the zone, then TONE (in the TONE zone; elsewhere the space's natural tone) */
    int32_t kt = knob(p[2]);
    int z = stereo ? sp_zone(kt) : tail_zone_mono(kt);
    int32_t amt = sp_zone_amount(kt, z);
    s->zone = (uint8_t)z;
    int32_t xt = z == SP_Z_TONE ? sp_tone(kt) : 0;
    s->tx = first ? xt : s->tx + ((xt - s->tx) >> 3);
    int tone_moved = space_moved || s->tx != s->last_tx;
    if (tone_moved) {
        int32_t x = s->tx, hi = v[SP_HI], lpo = 2 * x, hpo = 0;
        if (x < 0) {
            hi = (hi * (exp2_q16(4 * x) >> 1)) >> 15;   /* x 4^x */
            hpo = (-x * 24838) >> 15;                   /* dark thins the low end a little: the low cut x 1.3 at most */
        } else
            hi += ((32768 - hi) * x) >> 15;             /* bright: towards a flat decay */
        s->hi = clampi(hi, 64, 32768);
        s->axo = coef(v[SP_LXO], h);
        s->ahp = coef(v[SP_LHP] - LOG2_HP_SPLIT + hpo, h);
        s->alp = coef(v[SP_LLP] + lpo, h);
        s->last_tx = s->tx;
    }

    /* DECAY and the holds (the hold from the knob as given, the multiplier from the smoothed knob) */
    int hold = sp_hold(knob(p[1])), key = key_held(stereo);
    int32_t kd = s->ks[1] >> 4;
    int32_t L = v[SP_L2T60] + (hold ? 2 << 16 : sp_decay_l2(kd > 30719 ? 30719 : kd));   /* log2 RT60 (s), Q16 */
    uint32_t K = h ? 1941163000u : 970581500u;      /* 9965.8 2^32 / fs: c = K / T60 (ms), log2 loss per sample */
    int decay_moved = tone_moved || L != s->last_L;
    if (decay_moved) {
        int32_t t60 = ((exp2_q16(L) >> 4) * 1000) >> 12;
        t60 = t60 < 20 ? 20 : t60;
        int32_t t60h = (int32_t)(((uint32_t)t60 * (uint32_t)s->hi) >> 15);
        t60h = t60h < 10 ? 10 : t60h;
        s->t60_ms = t60;
        s->c = sp_udiv(K, (uint32_t)t60);
        s->ch = sp_udiv(K, (uint32_t)t60h);
        int32_t Lg = L < LOG2_03 ? LOG2_03 : L;         /* the input gain 0.5 RT60^-1/4 (RT60 >= 0.3 s) */
        s->ig0 = exp2_q16(-((Lg - LOG2_15) >> 2)) >> 2;
        s->last_L = L;
    }
    s->hold_key = (uint8_t)key;
    int32_t ft = hold == SP_HOLD_FREEZE || key ? 32768 : 0, it = hold == SP_HOLD_INFINITE && !key ? 32768 : 0;
    if (first) {
        s->frz = ft;
        s->inf = it;
    }
    s->frz += clampi(ft - s->frz, -HOLD_STEP, HOLD_STEP);
    s->inf += clampi(it - s->inf, -HOLD_STEP, HOLD_STEP);
    int32_t hh = s->frz > s->inf ? s->frz : s->inf;
    int gains_moved = decay_moved || hh != s->last_hh;
    s->last_hh = hh;

    /* the lines: lengths glide (<= 1/64 sample per sample), 4 modulated; the gains for the RT60 per band (from the
     * glided lengths: the modulation's few samples change them by < 0.5 %) */
    int32_t sz4 = v[SP_SIZE] >> 4;
    int32_t mdep = ((v[SP_MDEP] << 8) >> h) >> 8;   /* Q8 samples */
    mdep = (mdep * (32768 - s->frz)) >> 15;         /* frozen: still (whole-sample reads: no interpolation loss) */
    mdep = (mdep * (32768 - (s->inf >> 1))) >> 15;  /* INFINITE: half */
    uint32_t c = s->c, ch = s->ch;
    uint32_t minc = (uint32_t)v[SP_MRATE] + (((uint32_t)v[SP_MRATE] * 31855u) >> 16);   /* Hz -> Q32 per 44.1 k */
    minc <<= h;
    if (space_moved)
        for (int i = 0; i < NL; i++) {
            s->tgt[i] = (((sp_base[i] * sz4) >> h) + 32768) & ~0xFFFF;   /* whole samples at rest */
            s->mstep[i] = ((minc * (uint32_t)(4096 + 942 * i)) >> 12) * (uint32_t)n;
        }
    for (int i = 0; i < NL; i++) {
        int32_t tgt = s->tgt[i];
        if (first || s->base[i] != tgt) {           /* gliding: the in-line allpass and the loop's delay follow */
            int32_t b = first ? tgt : s->base[i];
            b += clampi(tgt - b, -SLEW * n, SLEW * n);
            s->base[i] = b;
            int32_t lp_ = ((b >> 16) * 8520 + 32768) >> 16;   /* 13 % of the line */
            s->lap_len[i] = clampi(lp_, 1, (int32_t)s->lapmask);
            s->lm[i] = (uint32_t)((b >> 16) + s->lap_len[i]);   /* the loop's delay */
        }
        s->dn[i] = s->base[i];
    }
    static const uint8_t mod_line[4] = { 0, 3, 5, 6 };
    for (int k = 0; k < 4; k++) {
        int i = mod_line[k];
        s->mph[i] += s->mstep[i];
        s->dn[i] += (mdep * tri(s->mph[i] + (uint32_t)i * 0x5EB851EBu)) >> 7;
    }
    if (first)
        for (int i = 0; i < NL; i++)
            s->d[i] = s->dn[i];
    for (int i = 0; i < NL; i++) {
        uint32_t m = s->lm[i];
        if (!gains_moved && m == s->last_m[i])
            continue;
        s->last_m[i] = m;
        int32_t elo = (int32_t)(m * (c >> 16) + ((m * (c & 0xFFFF)) >> 16));
        int32_t ehi = (int32_t)(m * (ch >> 16) + ((m * (ch & 0xFFFF)) >> 16));
        int32_t glo = exp2_neg(elo), ghi = exp2_neg(ehi);
        s->glo[i] = glo;
        glo += ((GFRZ - glo) >> 15) * hh;
        ghi += ((GFRZ - ghi) >> 15) * hh;
        int32_t Gl = ((glo >> 14) * 23170 + (1 << 16)) >> 17, Gh = ((ghi >> 14) * 23170 + (1 << 16)) >> 17;
        s->gh[i] = Gh;
        s->gd[i] = Gl - Gh;
        gains_moved |= 2;                           /* (the shimmer's normalisation follows) */
    }
    if (space_moved) {
        int32_t sq4 = v[SP_SQRTS] >> 4;
        for (int i = 0; i < 4 * s->nch; i++)
            s->ap_len[i] = clampi(((sp_ap_base[i] * sq4) >> 16) >> h, 1, (int32_t)s->apmask[i]);
        for (int i = 0; i < 2 * s->nch; i++)
            s->erap_len[i] = clampi(((sp_erap_base[i] * sq4) >> 16) >> h, 1, (int32_t)s->erapmask[i]);
        int32_t pre = spms(v[SP_PRE], h);
        for (int k = 0; k < 8; k++) {
            int32_t tl = spms(v[SP_ERT + k], h);
            s->ert[0][k] = clampi(pre + tl, 1, (int32_t)s->pdmask - BLOCK);
            s->ert[1][k] = clampi(pre + ((tl * sp_er_r_skew[k]) >> 14), 1, (int32_t)s->pdmask - BLOCK);
            s->erg[k] = v[SP_ERG + k];
        }
        s->pre = clampi(pre, 1, (int32_t)s->pdmask - BLOCK);
        s->erlev = v[SP_ERLEV];
        s->diff = v[SP_DIFF];
        s->ash = coef(LOG2_300, h);
        s->adc = coef(LOG2_20, h);
    }

    /* the input gain x (1 - freeze); INFINITE: x 1 / (1 + level / 0.15) */
    int32_t ig = (s->ig0 * (32768 - s->frz)) >> 15;
    if (s->inf)
        ig -= (int32_t)sp_udiv((uint32_t)(((ig * (s->inf >> 1)) >> 14) * s->lv), (uint32_t)(INF_LV + s->lv));
    s->sd_t = stereo ? ig >> 1 : (ig * 23170) >> 15;  /* mono: x sqrt 2 (one input instead of two) */
    if (first)
        s->sd = s->sd_t;

    if (hh < 328)                                   /* the level a held shimmer keeps (+2 dB) */
        s->frz_target = (s->lv * 5) >> 2;
    /* shimmer: the voices by zone, glided over ~16 blocks; normalised by the loop gain; frozen: servoed */
    int32_t g0 = 0, g1 = 0, sa = z == SP_Z_SH12 || z == SP_Z_SH19 ? sp_shimmer_level(kt, z) : 0;
    if (z == SP_Z_SH12)
        g0 = sa;
    else if (z == SP_Z_SH19) {
        g0 = (sa * 22938) >> 15;                    /* x 0.7 */
        g1 = sa >> 1;                               /* x 0.5 */
    }
    s->shg[0] += (g0 - s->shg[0]) >> 4;
    s->shg[1] += (g1 - s->shg[1]) >> 4;
    if (g0 == 0 && s->shg[0] < 16)
        s->shg[0] = 0;
    if (g1 == 0 && s->shg[1] < 16)
        s->shg[1] = 0;
    if (s->shg[0] | s->shg[1]) {
        if (gains_moved || !s->shk) {
            int32_t gsum = 0;
            for (int i = 0; i < NL; i++)
                gsum += s->glo[i] >> 3;
            int32_t gm = gsum >> 15;                /* the mean loop gain, Q15 */
            int32_t shk = 4 * (int32_t)isqrt((uint32_t)((1 << 30) - gm * gm));
            s->shk = shk > 32768 ? 32768 : shk;
        }
        int32_t servo = 32768;
        if (hh && s->lv > s->frz_target) {
            servo = (int32_t)sp_udiv((uint32_t)s->frz_target << 15, (uint32_t)s->lv);
            servo = (servo * servo) >> 15;
            servo = (servo * servo) >> 15;
        }
        int32_t afs = z == SP_Z_SH12 || z == SP_Z_SH19 ? 9830 + ((amt * 13107) >> 15) : 0;   /* 0.3 + 0.4 amount */
        s->shr_gain = ((s->shk * (32768 - hh)) >> 15) + ((((afs * servo) >> 15) * hh) >> 15);   /* held: servoed */
    }
    s->first = 0;
}

/* the shimmer: one block of the late output -> high-pass 300 Hz -> anti-alias low-pass -> every second sample into the
 * ring (the shifter runs at half the tank rate: its input is band-limited below a quarter of the tank rate, 7 kHz at
 * 44.1, 3 kHz at 22.05); the heads -> the return (n / 2 samples) -> the limit -> linear interpolation up -> s->shf
 * (the next block's return). The ring starts from silence when the shimmer comes on: cleared 1 KB a call first. */
static void shimmer(struct sp *s, const int16_t *in, int n)
{
    int h = s->half, n2 = n >> 1;
    if (s->sh_clr < s->shwords) {                  /* clearing: no ring writes, no return yet */
        uint32_t *w = (uint32_t *)(void *)s->shr + (s->sh_clr >> 1), left = (s->shwords - s->sh_clr + 1) >> 1;
        uint32_t nw = left < 256 ? left : 256;
        k_zero(w, nw);
        s->sh_clr += 2 * nw;
        for (int j = 0; j < n; j++)
            s->shf[j] = 0;
        return;
    }
    const int16_t *q = sp_shbq[h][s->shg[1] ? 1 : 0];
    k_pole(sc.sh, &s->shhp, s->ash, 1, n);          /* the 300 Hz high-pass, in place */
    for (int pass = 0; pass <= h; pass++) {         /* the anti-alias low-pass: twice at 22.05 kHz (4th order) */
        int32_t *b = s->shbq[pass];
        k_bq(sc.sh, b, q, n);
        if (!(b[0] | b[1]) && b[2] >= -2 && b[2] <= 2 && b[3] >= -2 && b[3] <= 2)
            b[2] = b[3] = 0;                        /* silence in: no sub-LSB residue left to circulate */
    }
    k_ringw(s->shr, s->shmask, s->sht, in, n2);
    if (s->shr_gain == 0 || (s->shg[0] == 0 && s->shg[1] == 0)) {
        for (int j = 0; j < n; j++)
            s->shf[j] = 0;
        s->shprev = 0;
        s->sht += (uint32_t)n2;
        return;
    }
    int32_t W = (SH_W >> h) >> 1;                   /* the window in the shifter's samples */
    struct hseg hs[8];
    int nh = 0;
    for (int j = 0; j < n2; j++)
        sc.acc[j] = 0;
    for (int vc = 0; vc < 2; vc++) {
        int32_t g = s->shg[vc];
        if (!g)
            continue;
        uint32_t r = (uint32_t)vc + 2;              /* read speed: 2 (+12), 3 (+19: a twelfth) */
        uint32_t dph = (r - 1) * (h ? 3897429u : 1947831u);   /* (r - 1) / W per shifter sample, Q32 */
        for (int hd = 0; hd < 2; hd++) {
            uint32_t ph = s->shph[vc] + ((uint32_t)hd << 31);
            int j0 = 0;
            while (j0 < n2) {
                /* the samples left in this grain (its window phase wraps at 2^32); when it ends here, the quotient
                 * (< 32) by a binary search */
                uint32_t rest = 0xFFFFFFFFu - ph, left = (uint32_t)(n2 - j0) + 1;
                if (rest < (uint32_t)(n2 - j0) * dph) {
                    uint32_t qq = 0;
                    for (uint32_t b = 16; b; b >>= 1)
                        if ((qq + b) * dph <= rest)
                            qq += b;
                    left = qq + 1;
                }
                int m = left < (uint32_t)(n2 - j0) ? (int)left : n2 - j0;
                /* delay = n2 + offset + W (1 - phase) at the segment's first sample, shortening by r - 1 per
                 * sample: the read runs at speed r */
                uint32_t ph1 = ph + dph;
                int32_t dq = (n2 + s->shd0[vc][hd]) * 65536 + W * (int32_t)(65535 - (ph1 >> 16));
                uint32_t pos = ((s->sht + (uint32_t)j0) << 16) - (uint32_t)dq;
                /* the window (x the voice's gain) at the segment's first and last sample, linear between */
                int32_t wa = (sp_win[(ph + dph) >> 22] * g) >> 15, wb = (sp_win[(ph + dph * (uint32_t)m) >> 22] * g) >> 15;
                int32_t dw = m > 1 ? ((wb - wa) * sp_rcp[m - 1]) >> 15 : 0;
                hs[nh++] = (struct hseg){ sc.acc + j0, pos - (r << 16), r << 16, wa, dw, m };
                ph += dph * (uint32_t)m;
                j0 += m;
                if ((uint32_t)m == left) {          /* the grain ended: the next one's start offset, jittered */
                    s->rng = s->rng * 1664525u + 1013904223u;
                    s->shd0[vc][hd] = (int32_t)(((s->rng >> 16) * (uint32_t)((W * 6554) >> 16)) >> 16);   /* 0 .. 0.1 W */
                }
            }
        }
        s->shph[vc] += dph * (uint32_t)n2;
    }
    k_heads(hs, nh, s->shr, s->shmask);
    s->sht += (uint32_t)n2;
    /* x the return's gain, soft limit to 0.5 FS: y = 0.75 (u - u^3 / 3), u = x / 0.75 clamped to +-1; then up to the
     * tank rate (linear: the halfway sample the mean of its neighbours) */
    k_shret(s->shf, sc.acc, s->shr_gain, &s->shprev, n2);
}

/* ---- one block of the tank: sc.x[c] (n frames at the tank rate) -> sc.wet[c] ---- */
static void tank(struct sp *s)
{
    int n = s->n, nch = s->nch, lgn = s->lgn;
    uint32_t t = s->t;
    int32_t dsd = (s->sd_t - s->sd) >> lgn, se = (s->sd * 45875) >> 15;   /* ER send 0.7 = direct 0.5 x 1.4 */
    int32_t dse = (((s->sd_t * 45875) >> 15) - se) >> lgn;
    int32_t g2c = 32768 - ((s->diff * s->diff + 16384) >> 15);
    for (int c = 0; c < nch; c++) {
        int16_t *pd = s->pd[c], *x = sc.x[c], *u = sc.u[c], *er = sc.er[c];
        uint32_t pos[8];
        k_pdw(pd, s->pdmask, t, x, n);              /* the predelay line */
        for (int k = 0; k < 8; k++)
            pos[k] = t - (uint32_t)s->ert[c][k];
        SPROF(1);
        k_taps(er, pd, s->pdmask, pos, s->erg, n);
        SPROF(2);
        k_aps(er, &s->erap[2 * c], &s->erapmask[2 * c], t, &s->erap_len[2 * c], 19661, 20972, 2, n);   /* g 0.6 */
        /* a DC blocker (20 Hz): the ERs reach the wet unfiltered; the bandwidth on the ERs too: TONE darkens them */
        k_pole2(er, &s->erdc[c], s->adc, &s->erlp[c], s->alp, 0, n);
        SPROF(3);
        k_send(u, pd, s->pdmask, t - (uint32_t)s->pre, er, s->sd, dsd, se, dse, n);
        k_pole2(u, &s->filt[c][0], s->ahp, &s->filt[c][1], s->ahp, 1, n);   /* the low cut: two one-pole high-passes */
        k_pole(u, &s->filt[c][2], s->alp, 0, n);   /* the bandwidth */
        SPROF(4);
        k_aps(u, &s->ap[4 * c], &s->apmask[4 * c], t, &s->ap_len[4 * c], s->diff, g2c, 4, n);
        k_add(u, s->shf, n);                        /* the shimmer's return (last block's) */
        SPROF(5);
    }
    s->sd = s->sd_t;
    /* the loop's rounding: to nearest while the tail is above ~-57 dBFS (or held), so it decays at the RT60 the knobs
     * set; toward zero below (and on silence), so it ends in exact silence with no limit cycle */
    if (s->frz > 16384 || s->inf > 16384 || s->lv > NEAR_HI)
        s->near = 1;
    else if (s->lv < NEAR_LO)
        s->near = 0;
    /* FDN stage 1: per line, read (gliding, modulated), the in-line allpass, the absorbent filter -> F */
    uint32_t t0 = cycles();
    struct l1 L[NL];
    for (int i = 0; i < NL; i++) {
        int32_t st_ = (s->dn[i] - s->d[i]) >> lgn;
        L[i].F = sc.F[i];
        L[i].ring = s->line[i];
        L[i].p = (t << 16) - 65536u - (uint32_t)s->d[i];
        L[i].inc = 65536u - (uint32_t)st_;
        L[i].interp = st_ != 0 || (s->d[i] & 0xFFFF) != 0;   /* a whole-sample delay at rest reads plain */
        L[i].lap = s->lap[i];
        L[i].dly = s->lap_len[i];
        L[i].sgn = i & 1 ? -1 : 1;
        L[i].lo = &s->lo[i];
        L[i].gh = s->gh[i];
        L[i].gd = s->gd[i];
        L[i].job = s->k1[i];
        s->d[i] += st_ * n;
    }
    k_stage1_all(L, s->lmask, s->lapmask, t, s->axo, s->near, n);
    SPROF(6);
    /* stage 2: the Hadamard, the outputs, the inputs; stage 3: the writes */
    k_wht(sc.F, sc.late[0], sc.late[1], n);
    /* the inputs on rows 3 and 4's signs, x 1/sqrt 8 (Q18): +P on lines 0 3, -M on 1 2, +M on 4 7, -P on 5 6 */
    k_inj(sc.inj[0], sc.inj[1], sc.u[0], nch == 2 ? sc.u[1] : sc.zero, n);
    SPROF(7);
    k_write_all(s->line, s->lmask, t, sc.F, sc.inj, s->near, n);
    SPROF(8);
    s->cfdn += cycles() - t0;
#ifdef SPACE_DIAG
    for (int i = 0; i < NL; i++)
        for (int j = 0; j < n; j++) {
            int32_t a = s->line[i][(t + (uint32_t)j) & s->lmask];
            a = a < 0 ? -a : a;
            space_diag_peak = a > space_diag_peak ? a : space_diag_peak;
        }
#endif
    /* the wet: the ERs x their level + the late output x 2 (the input's -6 dB made up) */
    int32_t sum = 0;
    for (int c = 0; c < nch; c++)
        sum += k_wet(sc.wet[c], sc.er[c], sc.late[c], s->erlev, n);
    /* the tank level (mean |late| x 1.25 ~ RMS, Q15), ~45 ms */
    int32_t m = (sum * 5) >> (lgn + nch - 1 + 3 + 2);
    s->lv += (m - s->lv) >> 5;
    SPROF(9);
    /* the shimmer, from this block's late output (heard from the next block on) */
    if (s->shg[0] | s->shg[1] | s->shbq[0][0] | s->shbq[0][1] | s->shbq[0][2] | s->shbq[0][3] | s->shbq[1][2]) {
        if (!s->sh_on) {                            /* the ring holds an old tail: clear it (shimmer(), 1 KB a call) */
            s->sh_clr = 0;
            s->sh_on = 1;
        }
        k_shin(sc.sh, sc.late[0], sc.late[1], nch == 2, n);
        shimmer(s, sc.sh, n);
    } else {
        for (int j = 0; j < n; j++)
            s->shf[j] = 0;
        s->sh_on = 0;
    }
    s->t = t + (uint32_t)n;
    SPROF(10);
}

/* ---- the process call ---- */
static void run(int stereo, int16_t **io, const int16_t *p)
{
    uint32_t c0 = cycles();
    SPROF_START();
    struct sp *s = &st[stereo];
    struct space_ui *u = &space_ui[stereo];
    uint32_t now = te_blocks;
    if (!s->started || fx_line_owner[stereo] != FX_OWNER_SPACE || now - s->last_blk > STALE) {
        fx_line_owner[stereo] = FX_OWNER_SPACE;     /* (pingpong.h: the set is ours now) */
        clean_start(s, stereo);
    }
    s->last_blk = now;
    fx_publish(&space_pub[stereo], p);
    int nch = stereo ? 2 : 1;

    /* MIX (and on the master the duck: a peak follower on the dry input) */
    int32_t wet, dry, ddb;
    sp_mix(knob(p[3]), stereo, &wet, &dry, &ddb);
    if (stereo) {
        int32_t pk = k_peak(io[0], io[1], BLOCK);
        if (pk >= s->ef)
            s->dhold = 103;                         /* hold 150 ms after the last peak: no pumping between notes */
        if (pk > s->ef)
            s->ef += ((pk - s->ef) * 25231) >> 15;
        else if (s->dhold)
            s->dhold--;
        else
            s->ef -= ((s->ef - pk) * 230 + 32767) >> 15;   /* then ~200 ms */
        int32_t fg = exp2_q16(-((ddb * 10885) >> 8)) >> 1;          /* the floor gain, Q15 */
        int32_t lvl = (s->ef * 17067) >> 10;                        /* ef / THR, Q15 */
        lvl = lvl > 32768 ? 32768 : lvl;
        int32_t tg = 32768 - (((32768 - fg) * lvl) >> 15);
        s->duck += tg < s->duck ? ((tg - s->duck) * 15400) >> 15 : ((tg - s->duck) * 115 + 32767) >> 15;   /* ~400 ms */
        s->duck = clampi(s->duck, 0, 32768);
    }
    int32_t wg = (wet * s->duck) >> 15;
    if (!s->running) {                              /* clearing the set: the wet muted */
        uint32_t *w = (uint32_t *)(void *)s->line[0], left = (s->words - s->clr + 1) >> 1;
        uint32_t nw = left < CLEAR_WORDS / 2 ? left : CLEAR_WORDS / 2;
        k_zero(w + (s->clr >> 1), nw);
        s->clr += 2 * nw;
        if (s->clr >= s->words)
            s->running = 1;
        wg = 0;
        if (s->first)
            s->wg = 0;
    } else {
        SPROF(11);
        params(s, p, stereo);
        SPROF(0);
        for (int c = 0; c < nch; c++) {
            if (s->half)
                k_decimate(sc.x[c], io[c], s->dbo[c], s->dbe[c], s->dbs[c]);
            else
                for (int j = 0; j < BLOCK; j++)
                    sc.x[c][j] = io[c][j];
        }
        tank(s);
        for (int c = 0; c < nch; c++)
            if (s->half)
                k_interpolate(sc.wetf[c], sc.wet[c], s->ic[c], s->ics[c]);
            else
                for (int j = 0; j < BLOCK; j++)
                    sc.wetf[c][j] = sc.wet[c][j];
    }
    /* dry and wet, the gains ramped over the block; MIX 0 (wet 0, dry 1) is the input, bit exact */
    int32_t g0 = s->dg > 32767 ? 32767 : s->dg, g1 = dry > 32767 ? 32767 : dry;
    int32_t w0 = s->wg > 32767 ? 32767 : s->wg, w1 = wg > 32767 ? 32767 : wg;
    int exact = s->dg == 32768 && dry == 32768 && (!s->running || (s->wg == 0 && wg == 0));
    for (int c = 0; c < nch && !exact; c++) {
        if (!s->running)
            for (int j = 0; j < BLOCK; j++)
                sc.wetf[c][j] = 0;
        k_mix(io[c], sc.wetf[c], g0, (g1 - g0) >> 6, w0, (w1 - w0) >> 6, BLOCK);
    }
    s->dg = dry;
    s->wg = wg;
    SPROF(11);
    /* what the screen reads */
    u->t60_ms = s->t60_ms;
    u->level = s->lv;
    u->duck = s->duck;
    u->frz = s->frz;
    u->inf = s->inf;
    u->zone = s->zone;
    u->key = s->hold_key;
    u->clearing = (uint8_t)!s->running;
    u->half = (uint8_t)s->half;
    uint32_t dt = cycles() - c0;
    s->cmax = dt > s->cmax ? dt : s->cmax;
    s->csum += dt;
    u->cyc_peak = dt > u->cyc_peak ? dt : u->cyc_peak;
    if (++s->ccount == 256) {
        u->cyc_max = s->cmax;
        u->cyc_mean = s->csum >> 8;
        u->cyc_fdn = s->cfdn >> 8;
        s->cmax = s->csum = s->cfdn = s->ccount = 0;
    }
    u->blocks++;
}

void space_mono(void *self, int16_t *buf, const int16_t *p)
{
    (void)self;
    int16_t *io[1] = { buf };
    run(0, io, p);
}

void space_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p)
{
    (void)self;
    int16_t *io[2] = { l, r };
    run(1, io, p);
}
