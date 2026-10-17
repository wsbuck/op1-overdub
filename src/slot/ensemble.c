/* "ensemble" FX (src/slot/fx.json; first of our FX ids 8+): a BBD chorus, from a Boss CE-2-style single voice
 * through a Juno-style dual chorus to a Solina-style three-tap string ensemble. Design, values and sources:
 * notes/fx-slot.md "ensemble". Contract for every FX source (<source>.c exports <source>_mono/_stereo):
 *
 * TE calls process through the vtable (+0x14) of a filter FX object whose vptr build_slot.py's factory case
 * swapped for ours, from the L1 audio callback, once per 64-frame block (44.1 kHz, Q15):
 *   synth/drum (mono): process(this, int16 buf[64] in place, const int16 params[8])
 *   master (stereo):   process(this, int16 L[64], int16 R[64], const int16 params[8])
 * slot_gen.inc wraps both (FX_MONO_ENTRY/FX_STEREO_ENTRY in slot.S: save the hardware loops, zero L0-L3).
 * The object itself (TE's filter state) is not touched; state lives in the slot block (.data) or a FREE
 * SDRAM buffer. Keep one mono and one stereo instance: both can run at once (synth FX + master FX).
 *
 * Knobs:
 *   blue   RATE: triangle LFO 0.1 .. 8 Hz, exponential (Juno I 0.51 Hz at 12288, II 0.89 Hz at 16384)
 *   green  DEPTH: sweep 0 .. +-3 ms
 *   white  CHARACTER (ensemble.h): CE (0 .. 8191): one voice, fixed 6 ms centre; on the master L = dry + wet,
 *          R = dry - wet (CE-1 / stereo CE-2 wiring). DUAL (12288 .. 20479): Juno 60 style, 1.66 ms + depth,
 *          R's delay swept in anti-phase. 20480 .. 32767: the old three-tap ensemble fades in (taps at 120 / 240
 *          degrees, 6.5 Hz vibrato, 2 ms + depth); the top is the old ensemble at full.
 *   orange MIX: dry .. wet, constant average power; 16384 = 50/50 (each at 0.707), the CE-2 blend
 * Wet path, as a BBD: 2-pole low-pass -> pre-emphasis (2x - x[-1]) -> soft clip (the BBD's headroom; only loud
 * treble gets there) -> de-emphasis (the exact inverse) -> 2-pole low-pass -> delay line. The two low-pass sections
 * make a 4th-order Butterworth at 9 kHz. All of it is linear around the clip, so it runs once per channel before
 * the line (filters commute with the delay and the tap sum). Delays are linearly interpolated (the line holds
 * band-limited audio: the interpolation loss is <= 0.6 dB below 5 kHz). The LFO is evaluated every SUB samples
 * with the delays linear in between; depth and centre are slew-limited, character and mix glide: no clicks.
 * The per-sample loops are hand-written (16-bit fractional MACs; GCC's code was 3x slower).
 */
#include <stdint.h>
#include "ensemble.h"
#include "fxpub.h"

#define RAM __attribute__((section(".data")))
#define BLOCK 64
#define SUB 8
#define RING 1024                       /* power of two (+1 guard sample); longest tap 265 + 132 + 1 = 398 samples */
#define CE_C (265 << 16)                /* CE centre 6.0 ms, whole samples (no fixed interpolation loss) */
#define DUAL_BASE 4797235               /* 1.66 ms = 73.2 samples, Q16: Juno 60's shortest delay (> BLOCK) */
#define ENS_BASE (89 << 16)             /* 2 ms + 1 sample: the old ensemble's base */
#define DEPTH_MAX 132                   /* samples: 3 ms */
#define VIB_MAX 9                       /* samples: 0.2 ms */
#define VIB_INC 633008u                 /* 6.5 Hz: 6.5 / 44100 * 2^32 */
#define THIRD 0x55555555u               /* 120 degrees of phase */
#define SLEW 4096                       /* Q16: delays glide at most 1/16 sample per sample (~1 semitone) */
#define MIX_KAPPA 54292                 /* 4 (sqrt 2 - 1), Q15: the 50/50 make-up gain (0.5 -> 0.707) */
#define DUAL_RATE 2048                  /* Q15 per block: the CE -> DUAL crossfade takes >= 16 blocks */

void ensemble_mono(void *self, int16_t *buf, const int16_t *p);
void ensemble_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);

/* 4th-order Butterworth low-pass at 9 kHz as two biquads, Q15 fractional: b = b0 (1, 2, 1), a = (1, a1, a2),
 * rounded so that 4 b0 = 1 + a1 + a2 exactly (unity at DC). Packed for the dual MACs: (2 b0 : b0), (a1 : a2). */
#define PK(h, l) ((int32_t)(((uint32_t)(uint16_t)(h) << 16) | (uint16_t)(l)))
#define LP1_C PK(12432, 6216)
#define LP1_A PK(-9888, 1984)
#define LP2_C PK(17152, 8576)
#define LP2_A PK(-13642, 15178)
#define EMPH PK(10923, 21845)           /* 1/3 : 2/3 */

struct chan {
    int16_t ring[RING + 1];             /* ring[RING] = ring[0], so a tap reads two neighbours without a wrap */
    int16_t pad;
    uint32_t w;
    int32_t xx1, yy1;                   /* LP1: (x[-1] : x[-2]), (y[-1] : y[-2]) */
    int32_t xx2, yy2;                   /* LP2 */
    int32_t de;                         /* de-emphasis: previous output */
};

static struct chan mono_ch RAM = {{0}, 0, 0, 0, 0, 0, 0, 0};
static struct chan st_ch[2] RAM = {{{0}, 0, 0, 0, 0, 0, 0, 0}, {{0}, 0, 0, 0, 0, 0, 0, 0}};
struct ens ensemble_state[2] RAM = {{0, 0, 0, 0, 0, 0, {{0}}, 0, 0}, {0, 0, 0, 0, 0, 0, {{0}}, 0, 0}};   /* mono, stereo */
struct fx_pub ensemble_pub[2] RAM = {{{0}, 0}, {{0}, 0}};   /* the knobs each instance used last (fxpub.h) */

static const uint32_t exp2_frac[17] = {   /* (2^(i/16) - 1) in Q16 */
    0, 2902, 5932, 9096, 12400, 15850, 19454, 23216, 27146, 31249, 35534, 40009, 44682, 49562, 54658, 59979, 65536,
};

/* LFO phase increment for knob k: 0.1 Hz * 2^(log2(80) k / 32768), 0.1 .. 8 Hz. 0.1 Hz = 9739.2 per sample. */
static uint32_t rate_inc(int32_t k)
{
    k = (k * 25891) >> 15;                          /* 8 octaves of k -> log2(80) = 6.32 octaves */
    int32_t oct = k >> 12;                          /* 0..6 */
    int32_t f = k & 4095;                           /* fraction of an octave, Q12 */
    int32_t i = f >> 8, t = f & 255;               /* 16 table steps per octave */
    uint32_t m = exp2_frac[i] + (((exp2_frac[i + 1] - exp2_frac[i]) * t) >> 8);   /* Q16 mantissa - 1 */
    return (9739u * (65536u + m) >> 16) << oct;
}

/* Triangle, -32768 .. 32766 from a phase. */
static inline int32_t tri(uint32_t ph)
{
    int32_t x = ph >> 16;
    return x < 32768 ? 2 * x - 32768 : 2 * (65535 - x) - 32768;
}

static inline int32_t knob(int16_t k)
{
    return k < 0 ? 0 : k;
}

/* A delay that jumps would click; moving it faster than SLEW would swoop the pitch. */
static inline int32_t slew(int32_t d, int32_t max)
{
    return d > max ? max : d < -max ? -max : d;
}

/* Dry and wet gains (Q14) for mix m (0 .. 32768): (1 - m) g, m g with g = 1 + 4 (sqrt 2 - 1) m (1 - m). */
static inline void mix_gains(int32_t m, int32_t *dg, int32_t *wg)
{
    int32_t q = (m * (32768 - m)) >> 15;
    int32_t g = 32768 + ((q * MIX_KAPPA) >> 15);
    *dg = ((32768 - m) * g) >> 16;
    *wg = (m * g) >> 16;
}

/* Centre delay (Q16 samples) for character blend x (CE -> DUAL), ensemble amount e and depth (Q16). */
static int32_t centre_for(int32_t x, int32_t e, int32_t depth, int32_t vdepth)
{
    int32_t base = CE_C + (((DUAL_BASE - CE_C) >> 8) * x >> 7);
    base += ((ENS_BASE - DUAL_BASE) >> 8) * e >> 7;
    return base + (((depth >> 8) * x) >> 7) + vdepth;  /* CE: fixed centre; DUAL / ensemble: base + depth */
}

/* Biquad on the packed state: R0.L = y (rounded, saturated) for input X.L. */
#define BIQUAD(X)                                                                                                  \
    "A1 = %[xx].H * %[c].H, A0 = %[xx].L * %[c].L;\n\t"                                                           \
    "A1 -= %[yy].H * %[a].H, A0 -= %[yy].L * %[a].L;\n\t"                                                         \
    "A0 += A1;\n\t"                                                                                                \
    "R0.L = (A0 += " X ".L * %[c].L);\n\t"                                                                         \
    "%[xx] = PACK (" X ".L, %[xx].H);\n\t"

/* Write side, 1 of 2: LP1 -> pre-emphasis -> BBD headroom, into t. With u = 2/3 (2 y - y[-1]) saturated (so the
 * pre-emphasised signal clamps at +-1.5): out = u (1.5 - u^2 / 2) = v - (2v/3)^3 / 2, flat at the clamp, odd (no
 * DC). At -6 dBFS its 3rd harmonic is ~-40 dB: subtle, and only loud treble gets near it. */
static __attribute__((noinline)) void bbd_in(struct chan *h, const int16_t *x, int16_t *t)
{
    int32_t xx = h->xx1, yy = h->yy1;
    __asm__ volatile(
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR1 = W [%[x]++] (X);\n\t"
        BIQUAD("R1")
        "A0 = R0.L * %[e].L;\n\t"
        "R1.L = (A0 -= %[yy].H * %[e].H) (S2RND);\n\t"   /* u = 4y/3 - 2y[-1]/3 */
        "%[yy] = PACK (R0.L, %[yy].H);\n\t"
        "R0.L = R1.L * R1.L;\n\t"
        "R0.L = R0.L >>> 2;\n\t"
        "R0.L = %[k].L - R0.L (NS);\n\t"                 /* 0.75 - u^2 / 4 */
        "R0.L = R1.L * R0.L (S2RND);\n"
        "2:\tW [%[t]++] = R0;\n"
        : [x] "+a"(x), [t] "+a"(t), [xx] "+d"(xx), [yy] "+d"(yy)
        : [c] "d"(LP1_C), [a] "d"(LP1_A), [e] "d"(EMPH), [k] "d"(24576), [n] "a"(BLOCK)
        : "R0", "R1", "A0", "A1", "LC0", "LT0", "LB0", "memory");
    h->xx1 = xx;
    h->yy1 = yy;
}

/* Write side, 2 of 2: de-emphasis (y = (v + y[-1]) / 2, rounded toward zero: decays to 0) -> LP2 -> line. */
static __attribute__((noinline)) void bbd_out(struct chan *h, const int16_t *t)
{
    int32_t xx = h->xx2, yy = h->yy2, de = h->de;
    int16_t *wp = h->ring + (h->w & (RING - 1));
    __asm__ volatile(
        "I0 = %[wp];\n\tB0 = %[rb];\n\tR0 = %[len];\n\tL0 = R0;\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR1 = W [%[t]++] (X);\n\t"
        "%[de] = %[de] + R1;\n\t"
        "R1 = %[de] >> 31;\n\t"
        "%[de] = %[de] + R1;\n\t"
        "%[de] >>>= 1;\n\t"
        BIQUAD("%[de]")
        "%[yy] = PACK (R0.L, %[yy].H);\n"
        "2:\tW [I0++] = R0.L;\n\t"
        "R0 = 0;\n\tL0 = R0;\n"
        : [t] "+a"(t), [xx] "+d"(xx), [yy] "+d"(yy), [de] "+d"(de)
        : [c] "d"(LP2_C), [a] "d"(LP2_A), [wp] "a"(wp), [rb] "a"(h->ring), [len] "i"(2 * RING), [n] "a"(BLOCK)
        : "R0", "R1", "A0", "A1", "I0", "B0", "L0", "LC0", "LT0", "LB0", "memory");
    h->xx2 = xx;
    h->yy2 = yy;
    h->de = de;
    h->ring[RING] = h->ring[0];
}

/* One tap over a sub-block into acc (Q31: tap x g x 2; stores, or with LD / ADD adds, saturating). p = position of
 * the older neighbour (Q16, ring index in bits 25..16) before the first sample, inc = 1 - delay step per sample;
 * the gain g steps by gs per sample (a weight that moved per sub-block would zipper). Linear interpolation
 * a (1 - f) + b f, with 1 - f as 32767 - f (0.003 % low at f = 0). */
#define TAP_LOOP(LD, ADD)                                                                                            \
    __asm__ volatile(                                                                                              \
        "LSETUP (1f, 2f) LC0 = %[n];\n"                                                                            \
        "1:\t%[p] = %[p] + %[inc];\n\t"                                                                            \
        "R1 = %[p] << 6;\n\t"                                                                                      \
        "R1 >>= 22;\n\t"                                                                                           \
        "P0 = R1;\n\t"                                                                                             \
        "R0 = %[p] << 16;\n\t"                                                                                     \
        "R0 >>= 17;\n\t"                                                                                           \
        "P0 = %[ring] + (P0 << 1);\n\t"                                                                            \
        "R0.H = %[one].L - R0.L (NS);\n\t"                                                                         \
        "%[g] = %[g] + %[gs];\n\t"                                                                                 \
        "R2 = W [P0] (X);\n\t"                                                                                     \
        "R1 = W [P0 + 2] (X);\n\t"                                                                                 \
        "R2 = PACK (R2.L, R1.L);\n\t"                                                                              \
        "A0 = R2.H * R0.H;\n\t"                                                                                    \
        "R1.L = (A0 += R2.L * R0.L);\n\t"                                                                          \
        "R2 = (A0 = R1.L * %[g].L);\n\t"                                                                           \
        LD                                                                                                         \
        ADD                                                                                                        \
        "2:\t[%[acc]++] = R2;\n"                                                                                   \
        : [p] "+d"(p), [acc] "+a"(acc), [g] "+d"(g)                                                                \
        : [inc] "d"(inc), [ring] "a"(ring), [gs] "d"(gs), [one] "d"(32767), [n] "a"(SUB)                           \
        : "R0", "R1", "R2", "P0", "A0", "LC0", "LT0", "LB0", "memory")

static __attribute__((noinline)) void taps(int32_t *acc, const int16_t *ring, uint32_t w, int32_t d, int32_t st,
                                           int32_t g, int32_t gs, int first)
{
    uint32_t p = (w << 16) - (uint32_t)d - 65536u;  /* sample i: write position w + 1 + i, delay d + (i + 1) st */
    int32_t inc = 65536 - st;
    if (first)
        TAP_LOOP("", "");
    else
        TAP_LOOP("R1 = [%[acc]];\n\t", "R2 = R2 + R1 (S);\n\t");
}

/* Tap weights for ensemble amount e and CE -> DUAL blend x (Q15): L's main tap 1 - 2e/3, the two extra taps e/3
 * each (sum 1); on R the main voice splits into its anti-phase tap (x) and L's delay inverted (1 - x). */
enum { G_MAIN, G_EXTRA, G_ANTI, G_INV, G_N };
static void weights(int32_t e, int32_t x, int32_t *g)
{
    int32_t w12 = (e * 10923) >> 15, w0 = 32767 - 2 * w12;
    g[G_MAIN] = w0;
    g[G_EXTRA] = w12;
    g[G_ANTI] = (w0 * x) >> 15;
    g[G_INV] = -((w0 * (32768 - x)) >> 15);
}

/* Dry + wet: x = x g + (acc rounded to 16 bits) k, gains Q14 packed (g : k), stepping by st per sample. */
static __attribute__((noinline)) void mix_out(int16_t *x, const int32_t *acc, int32_t gk, int32_t st)
{
    __asm__ volatile(
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR0 = W [%[x]] (X);\n\t"
        "R1 = [%[acc]++];\n\t"
        "%[gk] = %[gk] +|+ %[st];\n\t"
        "R1.L = R1 (RND);\n\t"
        "R0 = PACK (R0.L, R1.L);\n\t"
        "A0 = R0.H * %[gk].H;\n\t"
        "R0.L = (A0 += R0.L * %[gk].L) (S2RND);\n"
        "2:\tW [%[x]++] = R0;\n"
        : [x] "+a"(x), [acc] "+a"(acc), [gk] "+d"(gk)
        : [st] "d"(st), [n] "a"(BLOCK)
        : "R0", "R1", "A0", "LC0", "LT0", "LB0", "memory");
}

/* One block for nch channels sharing the LFO. Channel 1's taps sit at +180 degrees (mirrored around the centre);
 * while the character is CE, R's main voice is instead L's in-phase delay, inverted.
 * Every delay is >= 73 samples > BLOCK, so the block's input is written to the line first and the taps read after. */
static void ensemble(struct ens *s, struct chan *ch, int16_t **io, int nch, const int16_t *p)
{
    uint32_t inc = rate_inc(knob(p[0])) * SUB;
    int32_t depth_t = knob(p[1]) * DEPTH_MAX * 2;  /* Q16 samples: k / 32768 * DEPTH_MAX << 16 */
    int32_t ch_k = knob(p[2]);
    int32_t dual_t = ens_dual(ch_k), ens_t = ens_amount(ch_k);
    int32_t vdepth_t = ens_t * VIB_MAX * 2;
    int32_t mix_t = knob(p[3]) >= 32767 ? 32768 : knob(p[3]);
    if (!s->centre) {                               /* first block: start on the knobs, no glide from zero */
        s->depth = depth_t;
        s->vdepth = vdepth_t;
        s->ens = ens_t;
        s->dual = dual_t;
        s->mix = mix_t;
        s->centre = centre_for(dual_t, ens_t, depth_t, vdepth_t);
    }
    /* R's CE -> DUAL crossfade flips the wet's sign: spread it over >= 16 blocks (23 ms) */
    int32_t dual_e = s->dual + slew(dual_t - s->dual, DUAL_RATE);
    int32_t dens = (ens_t - s->ens) / (BLOCK / SUB), ddual = (dual_e - s->dual) / (BLOCK / SUB);

    int16_t tmp[BLOCK];
    for (int c = 0; c < nch; c++) {
        bbd_in(&ch[c], io[c], tmp);
        bbd_out(&ch[c], tmp);
    }

    int32_t acc[2][BLOCK];
    for (int i0 = 0; i0 < BLOCK; i0 += SUB) {
        s->depth += slew(depth_t - s->depth, SLEW * SUB);
        s->vdepth += slew(vdepth_t - s->vdepth, SLEW * SUB);
        int32_t g[G_N], ge[G_N], gs[G_N];
        weights(s->ens, s->dual, g);
        s->ens += dens;
        s->dual += ddual;
        weights(s->ens, s->dual, ge);
        for (int j = 0; j < G_N; j++)
            gs[j] = (ge[j] - g[j]) >> 3;            /* per sample (SUB = 8) */
        s->lfo += inc;
        s->vib += VIB_INC * SUB;
        int32_t ct = centre_for(s->dual, s->ens, s->depth, s->vdepth);
        s->centre += slew(ct - s->centre, SLEW * SUB);
        int32_t centre = s->centre;
        int32_t step[2][3];
        uint32_t ph = s->lfo, vph = s->vib;
        for (int t = 0; t < 3; t++, ph += THIRD, vph += THIRD) {
            int32_t m = (((s->depth >> 8) * tri(ph)) >> 7) + (((s->vdepth >> 8) * tri(vph)) >> 7);
            for (int c = 0; c < nch; c++) {
                int32_t target = c ? centre - m : centre + m;
                if (!s->d[c][t])
                    s->d[c][t] = target;            /* first block: start on the sweep */
                step[c][t] = (target - s->d[c][t]) / SUB;
            }
        }
        for (int c = 0; c < nch; c++) {
            const int16_t *ring = ch[c].ring;
            uint32_t w = ch[c].w + i0;
            int32_t *a = acc[c] + i0;
            int mi = c ? G_ANTI : G_MAIN, first = 1;
            if (c && (g[G_INV] || ge[G_INV])) {     /* L's delay (not yet advanced) */
                taps(a, ring, w, s->d[0][0], step[0][0], g[G_INV], gs[G_INV], 1);
                first = 0;
            }
            if (g[mi] || ge[mi] || first)
                taps(a, ring, w, s->d[c][0], step[c][0], g[mi], gs[mi], first);
            if (g[G_EXTRA] || ge[G_EXTRA]) {
                taps(a, ring, w, s->d[c][1], step[c][1], g[G_EXTRA], gs[G_EXTRA], 0);
                taps(a, ring, w, s->d[c][2], step[c][2], g[G_EXTRA], gs[G_EXTRA], 0);
            }
        }
        for (int c = 0; c < nch; c++)
            for (int t = 0; t < 3; t++)
                s->d[c][t] += step[c][t] * SUB;
    }

    int32_t dg, wg, dg1, wg1;                       /* mix, gliding over the block */
    mix_gains(s->mix, &dg, &wg);
    mix_gains(mix_t, &dg1, &wg1);
    int32_t st = PK((dg1 - dg) / BLOCK, (wg1 - wg) / BLOCK);
    for (int c = 0; c < nch; c++) {
        mix_out(io[c], acc[c], PK(dg, wg), st);
        ch[c].w += BLOCK;
    }
    s->ens = ens_t;                                 /* exact at the block end (the steps truncate) */
    s->dual = dual_e;
    s->mix = mix_t;
}

void ensemble_mono(void *self, int16_t *buf, const int16_t *p)
{
    (void)self;
    int16_t *io[1] = {buf};
    ensemble(&ensemble_state[0], &mono_ch, io, 1, p);
    fx_publish(&ensemble_pub[0], p);
}

void ensemble_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p)
{
    (void)self;
    int16_t *io[2] = {l, r};
    ensemble(&ensemble_state[1], st_ch, io, 2, p);
    fx_publish(&ensemble_pub[1], p);
}
