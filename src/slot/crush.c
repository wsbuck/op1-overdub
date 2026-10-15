/* "crush" FX (src/slot/fx.json; source crush): bitcrusher and sample-rate reducer behind a compressor, tuned for
 * artifacts. Contract and FX path: see ensemble.c and notes/fx-slot.md; design and measurements: notes/fx-crush.md.
 *
 * Chain per channel: compressor -> sample-and-hold -> quantiser (+ bit flips) -> reconstruction low-pass -> DC
 * blocker -> level compensation. Every knob turned fully anticlockwise = clean (the input, bit exact).
 *   blue   RATE: sample-and-hold at 44.1 kHz * 2^(-6.785 k): 44.1 kHz .. 400 Hz, logarithmic. No anti-alias
 *          filter: tones above half the rate fold back and move against the pitch. The clock is a fractional
 *          time counter: each hold samples the input at its exact instant (linear interpolation) and the step lands
 *          between output samples (the edge sample is the box-filtered mix), so the rate sweeps without steps
 *   green  BITS: 16 -> 1 bit, continuous (the step size 2^(16 - b) is scaled, not an integer bit count): 16 -> 7 bits
 *          over the first half, 7 -> 1 over the second. Between 1/4 and 3/4 of the knob the quantiser companding
 *          morphs from linear to mu-law (G.711's segment form, mu 1 -> 256 -> 1; full at the centre: 7-bit codes, the
 *          quietest step 16 LSB, i.e. ~12-bit resolution for quiet signals and ~5 bits at full scale: the companded
 *          early-sampler grain). Mid-tread, no dither: quiet tails gate to zero through stair steps. Below 3 bits the
 *          levels move to mid-rise (1 bit = +-half scale, a square), with a dead zone of 1/8 step so silence stays
 *          silent
 *   white  SQUASH: a compressor before the crusher: threshold -6 -> -36 dBFS, ratio 1 -> 20 (slope 0.95 s (2 - s)),
 *          make-up so a -6 dBFS peak keeps its level (up to +28.6 dB for quiet parts: tails are held up into the
 *          low bits, where they crackle), peak detector with instant attack and a release of 150 -> 37 ms that
 *          pumps; stereo linked. Its smoothed gain reduction is crush_state[].gr for the screen
 *   orange GRIT: lower half: jitter on the sample-and-hold clock, up to +-75 % of the period, a smoothed random
 *          walk whose rate goes from ~3 Hz (wobble) to every sample (sizzle). Upper half: jitter stays at full and
 *          XOR bit flips on the held code (sign-magnitude, so no DC): up to 1 in 4 holds, on bits that reach from
 *          the lowest to the top of the code as the knob goes up
 * Reconstruction: one-pole low-pass at about half the hold rate (bypassed from 44.1 kHz down to 22 kHz, faded in
 * below), so low rates come out warm. Output level: per-knob compensation tables (BITS and RATE, measured in the
 * simulator), ramped per sample. DC blocker 3.4 Hz (not in the clean path).
 * Knobs: two cascaded one-poles per block (1/4 each: a detent is half way after ~6 blocks, 8 ms); gains and the
 * compressor ramp per sample. Fixed point, no division, no 64-bit arithmetic, no tables in the per-sample path.
 */
#include <stdint.h>
#include "crush.h"

#define RAM __attribute__((section(".data")))
#define BLOCK 64
#define SUB 8                           /* compressor sub-block */
#define ONE 65536                       /* one sample, Q16 */

void crush_mono(void *self, int16_t *buf, const int16_t *p);
void crush_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);

#define CRUSH0 {0, {0, 0, 0, 0}, 44100, 16 << 12, 0, 0, {0, 0, 0, 0}, {0, 0, 0, 0}, ONE, 0, 0, 0, 0x2545F491u, 0, \
                32768, 8192, 1, {{0, 0, 0, 0}, {0, 0, 0, 0}}}
struct crush crush_state[2] RAM = {CRUSH0, CRUSH0};

/* Output level compensation per knob (Q12) at k = 0, 2048, .. 32767, linearly interpolated: the inverse of the mean
 * level change measured in the simulator (sines at 1 kHz -6 / -12 dBFS and 440 Hz, saws at 220 / 110 Hz), smoothed;
 * RATE's last point capped at +3 dB (notes/fx-crush.md "Level"). */
static const int16_t comp_bits[17] = {4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096,
                                      3976, 3976, 4490, 4096, 2330};
static const int16_t comp_rate[17] = {4096, 4115, 4129, 4143, 4158, 4187, 4240, 4339, 4491, 4703, 4868, 5039,
                                      5157, 5277, 5400, 5525, 5786};

static inline int32_t kn(int16_t k)
{
    return k < 0 ? 0 : k;
}

/* min / max as instructions (GCC 4.3 often branches for a two-sided clamp) */
static inline int32_t clamp(int32_t x, int32_t lo, int32_t hi)
{
#ifdef __bfin__
    __asm__("%0 = max(%0, %1);\n\t%0 = min(%0, %2);" : "+d"(x) : "d"(lo), "d"(hi));
    return x;
#else
    return x < lo ? lo : x > hi ? hi : x;
#endif
}

/* (a * c) >> 15 for 32-bit a and 16-bit c: two MACs (signed high half, mixed-mode low half). */
static inline int32_t mul16(int32_t a, int32_t c)
{
#ifdef __bfin__
    int32_t r;
    __asm__("A1 = %2.L * %1.L (M);\n\tA0 = %1.H * %2.L;\n\tA1 = A1 >>> 15;\n\t%0 = (A0 += A1);"
            : "=d"(r) : "d"(a), "d"(c) : "A0", "A1");
    return r;
#else
    return (int32_t)(((int64_t)a * (int16_t)c) >> 15);
#endif
}

/* Redundant sign bits of a positive x (31 - 1 - the position of its top bit). */
static inline int32_t norm(int32_t x)
{
#ifdef __bfin__
    int32_t r;
    __asm__("%0.L = SIGNBITS %1;" : "=d"(r) : "d"(x));
    return (int16_t)r;
#else
    int32_t n = 0;
    while (!(x & 0x40000000))
        x <<= 1, n++;
    return n;
#endif
}

/* 2^(x / 32768) * 65536 for x in 0 .. 13 * 32768: octave shift and a cubic for 2^f - 1 (1.7e-4 relative error, exact
 * at whole octaves; as distortion.c). */
static uint32_t exp2_q16(int32_t x)
{
    int32_t oct = x >> 15, f = x & 32767;
    int32_t t = (2536 * f) >> 15;
    t = ((7436 + t) * f) >> 15;
    t = ((22796 + t) * f) >> 15;
    return (65536u + (t << 1)) << oct;
}

/* log2(x) for x > 0 in Q12, x in units of 2^q: normalise, then log2(1 + f) ~ f (1.42086 - 0.57725 f + 0.15639 f^2)
 * (max error 0.001 bits, exact at 0 and 1). */
static int32_t log2_q12(int32_t x, int32_t q)
{
    int32_t n = norm(x);
    int32_t f = ((x << n) >> 15) - 32768;               /* Q15 */
    int32_t t = (5124 * f) >> 15;
    t = ((-18915 + t) * f) >> 15;
    t = ((46559 + t) * f) >> 15;
    return ((30 - n - q) << 12) + (t >> 3);
}

static inline uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* A table at k (0 .. 32767), Q12. */
static inline int32_t tab(const int16_t *t, int32_t k)
{
    int32_t i = k >> 11, f = k & 2047;
    return t[i] + (((t[i + 1] - t[i]) * f) >> 11);
}

/* One-pole knob smoothing step toward t (exact arrival). */
static inline int32_t smooth(int32_t v, int32_t t)
{
    int32_t d = (t - v) >> 2;
    return d ? v + d : t;
}

/* Per-block quantiser: step 2^sh * mant (mant Q14 = 2^frac; s1 = 14 + sh, s2 = 14 - sh), its inverse mant_inv
 * (Q14), the rounding offsets (hterm linear: 1/2 - the mid-rise offset; hmu mu-law: 1/2), the mid-rise level offset
 * and its dead zone; mu-law: K (Q7), Z = log2(1 + K) (Q15 bits), 1 / Z (r14, Q14 of 2^15), 1 / K (rk, Q15). Codes are
 * sign-magnitude: rounding and bit flips are symmetric (no DC). */
struct quant {
    int32_t s1, s2, mant, mant_inv, hterm, hmu, offs, dz, kq7, zq, r14, rk;
};

#define M16(a, b) ((int32_t)(int16_t)(a) * (int16_t)(b))    /* 16 x 16 -> 32 (one MAC) */

/* Code n of c (0 .. 32767) for the step: floor(c / step + h), flipped unless 0 (silence stays silent). */
static inline __attribute__((always_inline)) int32_t code(const struct quant *q, int32_t c, int32_t h, int32_t fm)
{
    int32_t n = (M16(c, q->mant_inv) + h) >> q->s1;
    return n ? n ^ fm : 0;
}

static inline __attribute__((always_inline)) int32_t level(const struct quant *q, int32_t n)
{
    int32_t y = M16(n, q->mant) >> q->s2;
    return y > 32767 ? 32767 : y;
}

/* Piecewise-linear log2 of w (2^15 .. 2^23, Q15 of 1): exponent + linear mantissa, Q15 bits (G.711's segments). */
static inline __attribute__((always_inline)) int32_t plog2(int32_t w)
{
    int32_t sb = norm(w);                                       /* 8 .. 15 */
    return ((15 - sb) << 15) + ((w << sb) >> 15) - 32768;
}

/* Linear, mid-tread (3 bits and up). */
static inline __attribute__((always_inline)) int32_t qlin(const struct quant *q, int32_t u, int32_t fm)
{
    return level(q, code(q, u, q->hterm, fm));
}

/* Linear with the mid-rise offset (below 3 bits): level (n + o) step, dead zone o step / 4. */
static inline __attribute__((always_inline)) int32_t qoff(const struct quant *q, int32_t u, int32_t fm)
{
    int32_t n = code(q, u, q->hterm, fm);
    int32_t y = n ? level(q, n) + q->offs : u >= q->dz ? q->offs : 0;
    return y > 32767 ? 32767 : y;
}

/* mu-law in G.711's segment form with a variable mu (K = 2^(8 m), 1 .. 256): z = plog2(1 + K u) / plog2(1 + K)
 * (0 .. 1), quantised mid-tread with the step, back through the exact inverse. K = 1 is the linear quantiser; at
 * K = 256, 8 bits it is G.711 mu-law (8 segments, 16 steps each). */
static inline __attribute__((always_inline)) int32_t qmu(const struct quant *q, int32_t u, int32_t fm)
{
    int32_t z = plog2(32768 + ((q->kq7 * u) >> 7));            /* Q15 bits, 0 .. 8 */
    int32_t c = mul16(z, q->r14) << 1;                          /* z / Z, Q15 */
    c = c > 32767 ? 32767 : c;
    int32_t zq = mul16(q->zq, level(q, code(q, c, q->hmu, fm)));    /* back to Q15 bits */
    int32_t m = ((32768 + (zq & 32767)) << (zq >> 15)) - 32768;      /* 2^zq - 1, Q15 */
    int32_t y = mul16(m, q->rk);                                /* / K */
    return y > 32767 ? 32767 : y;
}

enum { Q_LIN, Q_OFF, Q_MU };

static inline __attribute__((always_inline)) int32_t quant(int kind, const struct quant *q, int32_t v, int32_t fm)
{
    int32_t u = v < 0 ? -v : v;
    int32_t y = kind == Q_LIN ? qlin(q, u, fm) : kind == Q_OFF ? qoff(q, u, fm) : qmu(q, u, fm);
    return v < 0 ? -y : y;
}

/* Scratch, shared by the mono and the stereo instance (TE calls them one after the other, never nested; every
 * entry is written before it is read in the same call). */
static struct {
    int32_t gb[BLOCK];                  /* compressor gain per sample (Q15) */
    int32_t ei[BLOCK + 1], ef[BLOCK], em[BLOCK];   /* holds: sample index (then BLOCK), distance back (Q15), flip mask */
    int32_t v[BLOCK + 1];               /* input after the compressor (Q15, +-32767); v[0] = the last block's last */
    int32_t s[BLOCK];                   /* the input at each hold's instant */
    int32_t q[BLOCK];                   /* its quantised value */
    int32_t y[BLOCK];                   /* sample-and-hold output */
} sc RAM = {{0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}};

static const int32_t no_flips[BLOCK] = {0};

/* Per-block values the channel pass needs. */
struct blk {
    struct quant q;
    int32_t a;                          /* reconstruction low-pass, Q15 (32768: through) */
    int32_t g, dg;                      /* output gain, Q20, and its step per sample */
};

/* Quantise n values (in -> out, flip masks fm), one loop per kind. */
static __attribute__((noinline)) void quant_pass(const struct quant *q, int kind, const int32_t *in, const int32_t *fm,
                                                 int32_t *out, int n)
{
    if (kind == Q_LIN)
        for (int i = 0; i < n; i++)
            out[i] = quant(Q_LIN, q, in[i], fm[i]);
    else if (kind == Q_OFF)
        for (int i = 0; i < n; i++)
            out[i] = quant(Q_OFF, q, in[i], fm[i]);
    else
        for (int i = 0; i < n; i++)
            out[i] = quant(Q_MU, q, in[i], fm[i]);
}

/* One channel, one block: compressor; sample-and-hold (every sample on time, or the hold list: the input at each
 * hold's instant, linear between samples; the step lands between output samples, so that sample is the box-filtered
 * mix of the old and the new value); quantiser and flips; reconstruction low-pass (a, Q15; 32768 = through), DC
 * blocker (3.4 Hz), output gain (Q20 ramp). */
static __attribute__((noinline)) void chan_pass(const struct blk *b, struct crush_ch *h, int16_t *x, int kind,
                                                int ne, int full, int comp)
{
    int32_t *v = sc.v;
    v[0] = h->xp;
    if (comp)
        for (int i = 0; i < BLOCK; i++)
            v[i + 1] = clamp(mul16(sc.gb[i], x[i]), -32767, 32767);
    else
        for (int i = 0; i < BLOCK; i++)
            v[i + 1] = x[i] < -32767 ? -32767 : x[i];
    h->xp = v[BLOCK];
    const int32_t *y;
    if (full) {
        quant_pass(&b->q, kind, v + 1, no_flips, sc.q, BLOCK);
        y = sc.q;
        h->qh = sc.q[BLOCK - 1];
    } else {
        for (int e = 0; e < ne; e++) {
            int32_t i = sc.ei[e];
            sc.s[e] = v[i + 1] - mul16(v[i + 1] - v[i], sc.ef[e]);
        }
        quant_pass(&b->q, kind, sc.s, sc.em, sc.q, ne);
        int32_t qh = h->qh, i0 = 0;
        int32_t *yb = sc.y;
        for (int e = 0; e < ne; e++) {
            int32_t i = sc.ei[e], qv = sc.q[e];
            while (i0 < i)
                yb[i0++] = qh;
            yb[i0++] = qv - mul16(qv - qh, sc.ef[e]);              /* the step lands fb before this sample */
            qh = qv;
        }
        while (i0 < BLOCK)
            yb[i0++] = qh;
        h->qh = qh;
        y = yb;
    }
    int32_t z = h->z, dq = h->dq, g = b->g, dg = b->dg, ra = 32768 - b->a;    /* z = y + (z - y)(1 - a) */
    for (int i = 0; i < BLOCK; i++) {
        int32_t yi = y[i];
        z = yi + mul16(z - yi, ra);
        dq += ((z << 8) - dq) >> 11;
        g += dg;
        x[i] = (int16_t)clamp(mul16((z - (dq >> 8)) << 2, g >> 7), -32768, 32767);
    }
    h->z = z, h->dq = dq;
}

/* num / den in Q15 (0 < num <= den < 2^30), bitwise: no division instruction (per block only). */
static int32_t ratio15(int32_t num, int32_t den)
{
    int32_t r = 0;
    for (int i = 0; i < 15; i++) {
        num <<= 1;
        r <<= 1;
        if (num >= den)
            num -= den, r |= 1;
    }
    return r;
}

/* One block for nch channels; the compressor and the clock are shared (stereo linked). */
static void crush(struct crush *s, int16_t **io, int nch, const int16_t *p)
{
    int clean = 1;
    for (int k = 0; k < 4; k++) {
        s->v1[k] = smooth(s->v1[k], kn(p[k]));
        s->v2[k] = smooth(s->v2[k], s->v1[k]);
        s->k[k] = s->v2[k];
        clean &= s->v1[k] == 0 && s->v2[k] == 0;
    }
    int32_t kr = s->v2[0], kb = s->v2[1], ks = s->v2[2], kg = s->v2[3];

    if (clean) {
        /* exactly the input; keep the states where processing would continue from */
        for (int c = 0; c < nch; c++) {
            struct crush_ch *h = &s->ch[c];
            h->xp = h->qh = h->z = clamp(io[c][BLOCK - 1], -32767, 32767);
            h->dq = 0;
        }
        s->t = ONE;
        s->g = 32768;
        s->env = 0;
        s->gr = 0;
        s->gout = 8192;
        s->rate_hz = 44100, s->bits_q12 = 16 << 12, s->mu = 0;
        s->bypass = 1;
        return;
    }
    s->bypass = 0;

    /* RATE: period 2^e samples, e = 6.785 k; reconstruction low-pass a = 1 - exp(-pi / period), faded to 1 (through)
     * between 22 and 44.1 kHz */
    int32_t e = (kr * 27791) >> 12;                                 /* Q15 */
    int32_t period = (int32_t)exp2_q16(e);                          /* Q16 samples, 1 .. 110 */
    int32_t inc = (int32_t)(exp2_q16(8 * 32768 - e) >> 9);          /* 1 / period, Q15 */
    int32_t a = 32768 - (int32_t)(exp2_q16(13 * 32768 - ((inc * 37126) >> 13)) >> 14);  /* 2^(-4.532 inc) */
    if (inc > 16384)
        a += ((32768 - a) * ((inc - 16384) << 1)) >> 15;
    if (inc >= 32768)
        a = 32768;
    s->rate_hz = (44100 * inc) >> 15;

    /* BITS: k = 16 - b in Q15 bits (0 .. 15 - 2^-15); mu-law amount m (K = 2^(8 m)); mid-rise offset below 3 bits */
    struct blk bk;
    struct quant *q = &bk.q;                                        /* filled in place (no struct copy: no memcpy) */
    int32_t kq = kb <= 16384 ? kb * 18 : 9 * 32768 + (kb - 16384) * 12;
    kq = kq > 15 * 32768 - 1 ? 15 * 32768 - 1 : kq;
    int32_t sh = kq >> 15, kf = kq & 32767;
    q->s1 = 14 + sh;
    q->s2 = 14 - sh;
    q->mant = (int32_t)(exp2_q16(kf) >> 2);                          /* Q14 */
    q->mant_inv = (int32_t)(exp2_q16(32768 - kf) >> 3);              /* Q14 */
    int32_t o = clamp((kq - 13 * 32768) >> 3, 0, 8192);             /* level offset, Q14 of a step */
    q->hterm = (8192 - o) << sh;
    q->hmu = 8192 << sh;
    q->offs = M16(o, q->mant) >> (28 - sh);
    q->dz = q->offs >> 2;
    int32_t mu = clamp(32768 - (kb > 16384 ? kb - 16384 : 16384 - kb) * 4, 0, 32768);
    q->kq7 = clamp((int32_t)(exp2_q16(mu << 3) >> 9), 128, 32767);    /* K, Q7 */
    q->zq = plog2(32768 + ((q->kq7 * 32767) >> 7));                   /* Z = log2(1 + K), Q15 bits */
    q->r14 = ratio15(32768, q->zq) >> 1;                              /* 1 / Z, Q14 */
    q->rk = clamp((int32_t)(exp2_q16((8 << 15) - (mu << 3)) >> 9), 0, 32767);   /* 1 / K, Q15 */
    int kind = mu ? Q_MU : q->offs ? Q_OFF : Q_LIN;
    int32_t nb = 15 - sh;                                           /* magnitude bits of the code */
    s->bits_q12 = 65536 - (kq >> 3);
    s->mu = mu;

    /* GRIT: jitter (amount, random-walk rate), flips (probability per hold, bit range) */
    int32_t J = kg >= 16384 ? 32767 : kg << 1, F = kg > 16384 ? (kg - 16384) << 1 : 0;
    int32_t ja = (J * 24576) >> 15;                                 /* +-0.75 of the period at most, Q15 */
    int32_t jl = 14 - ((14 * J + 16384) >> 15);                     /* log2 samples between random targets */
    int32_t pth = F >> 1;                                           /* flip if rand16 < pth: <= 1 in 4 */
    int32_t jr = 256 + (((nb - 1) * F) >> 7);                       /* bit range, Q8 */

    /* output level compensation, Q13 */
    int32_t g1 = (tab(comp_bits, kb) * tab(comp_rate, kr)) >> 11, g0 = s->gout;
    s->gout = g1;

    /* compressor: per sub-block peak (all channels), envelope, gain in log2 (Q12), ramped per sample */
    int comp = ks > 0;
    int32_t grmax = 0;
    if (comp) {
        int32_t c = (31130 * ((ks * (65536 - ks)) >> 15)) >> 15;     /* slope 1 - 1 / ratio, Q15 */
        int32_t s5 = (ks * 20480) >> 15;                            /* 5 s bits, Q12 */
        int32_t T = -4096 - s5, M = (c * s5) >> 15;
        int32_t rel = (int32_t)((exp2_q16(ks << 1) * 40) >> 16);    /* release per sub-block, Q15 */
        int32_t g = s->g, env = s->env;
        for (int i0 = 0; i0 < BLOCK; i0 += SUB) {
            int32_t pk = 0;
            for (int ch = 0; ch < nch; ch++)
                for (int i = i0; i < i0 + SUB; i++) {
                    int32_t x = io[ch][i];
                    x = x < 0 ? -x : x;
                    pk = x > pk ? x : pk;
                }
            pk <<= 11;                                              /* Q26 */
            env = pk > env ? pk : env - mul16(env - pk, rel);
            int32_t over = (env >= 1 << 15 ? log2_q12(env, 26) : -15 << 12) - T, gr;   /* below -66 dBFS: far under T */
            if (over <= -2048)
                gr = 0;
            else if (over >= 2048)
                gr = (c * over) >> 15;
            else {
                int32_t d = over + 2048;
                gr = (c * ((d * d) >> 13)) >> 15;                   /* soft knee, 1 bit wide */
            }
            grmax = gr > grmax ? gr : grmax;
            int32_t gn = (int32_t)(exp2_q16((M - gr + 8192) << 3) >> 3);   /* 2^(M - gr), Q15 */
            int32_t dg = (gn - g) >> 3;
            for (int i = i0; i < i0 + SUB - 1; i++)
                sc.gb[i] = g += dg;
            sc.gb[i0 + SUB - 1] = g = gn;
        }
        s->g = g;
        s->env = env;
    } else {
        s->g = 32768;
        s->env = 0;
    }
    int32_t grs = clamp((grmax * 6554) >> 12, 0, 32767);
    s->gr += (grs - s->gr) >> 1;

    /* clock: the block's holds (t = time to the next one, Q16 from the block start) and their flip masks, shared by
     * the channels. The jitter is a random walk: segments of 2^jl samples ramping to random targets (segments
     * passed over between two holds only leave their target), advanced to each hold. */
    int ne = 0, full = period == ONE && !ja;
    if (full)
        s->t = ONE;                                                 /* back on the sample grid */
    else {
        int32_t t = s->t, done = 0, jv = s->jv, jstep = s->jstep, jcnt = s->jcnt;
        uint32_t rnd = s->rnd;
        for (;;) {
            int32_t i = (t - 1) >> 16;
            if (i >= BLOCK)
                break;
            sc.ei[ne] = i;
            sc.ef[ne] = (((i + 1) << 16) - t) >> 1;                 /* Q15, 0 .. 32767 */
            int32_t pe = period;
            if (ja && !jl)                                          /* white: a new value every sample */
                jv = (int16_t)(xorshift(&rnd) >> 16);
            else if (ja) {
                int32_t n = i + 1 - done;
                done = i + 1;
                if (n < jcnt) {
                    jv += jstep * n;
                    jcnt -= n;
                } else {
                    n -= jcnt;
                    jv += jstep * jcnt;
                    if (n >> jl) {
                        jv = (int16_t)(xorshift(&rnd) >> 16);
                        n &= (1 << jl) - 1;
                    }
                    jstep = ((int16_t)(xorshift(&rnd) >> 16) - jv) >> jl;
                    jcnt = (1 << jl) - n;
                    jv += jstep * n;
                }
            }
            if (ja)
                pe += mul16(period, (ja * jv) >> 15);
            t += pe < ONE ? ONE : pe;
            int32_t fm = 0;
            if (pth) {
                uint32_t r = xorshift(&rnd);
                if ((int32_t)(r & 0xFFFF) < pth) {
                    fm = 1 << (((r >> 16) * (uint32_t)jr) >> 24);
                    s->flips++;
                }
            }
            sc.em[ne++] = fm;
        }
        if (ja && jl) {                                             /* to the block end */
            int32_t n = BLOCK - done;
            if (n < jcnt) {
                jv += jstep * n;
                jcnt -= n;
            } else {
                jv += jstep * jcnt;
                jcnt = 0;
            }
        }
        s->t = t - BLOCK * ONE, s->jv = jv, s->jstep = jstep, s->jcnt = jcnt, s->rnd = rnd;
    }

    sc.ei[ne] = BLOCK;
    bk.a = a;
    bk.g = g0 << 7;
    bk.dg = (g1 - g0) << 1;                                         /* Q20: lands on g1 exactly */
    for (int c = 0; c < nch; c++)
        chan_pass(&bk, &s->ch[c], io[c], kind, ne, full, comp);
}

void crush_mono(void *self, int16_t *buf, const int16_t *p)
{
    (void)self;
    int16_t *io[1] = {buf};
    crush(&crush_state[0], io, 1, p);
}

void crush_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p)
{
    (void)self;
    int16_t *io[2] = {l, r};
    crush(&crush_state[1], io, 2, p);
}
