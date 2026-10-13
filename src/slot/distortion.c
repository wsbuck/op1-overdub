/* "distortion" FX (src/slot/fx.json; source distortion): a multi-character distortion box. Contract and FX path: see
 * ensemble.c and notes/fx-slot.md; design and measurements: notes/fx-distortion.md.
 *
 *   blue   drive: 0.5x .. 64x (-6 .. +36 dB, exponential, through drive_taper(): the upper half of the knob is the
 *          heavy range, half way is already +27 dB) into tube / muff / fuzz / clip: a -6 dBFS sine goes from clean
 *          (a quarter of the clip level) to 32x over it. Fold and crush have their own laws (below). Loudness is
 *          auto-compensated per character (also for the filter's peak), so blue changes the dirt, not the volume.
 *          Blue also darkens the top: a shelf above ~4 kHz, to -9 dB at max (tames the fizz; half that in crush)
 *   green  voice: a resonant state-variable filter BEFORE the shaper, so the distortion screams through its peak.
 *          16384 +-2048 = flat (exactly the input). Down: resonant low-pass sweeping 6 kHz -> 150 Hz, Q 0.9 -> 5.
 *          Up: "focus", a band-pass peak (+9.5 dB, Q 2.5, the rest -12 dB) sweeping 350 Hz -> 4 kHz (a cocked wah
 *          into the shaper)
 *   white  character: six shapes; neighbours crossfade (plateaus 3072 wide, fades 2867 between; centres 1536,
 *          7475, 13414, 19353, 25292, 31231):
 *          0-3071        TUBE  soft, asymmetric (cubic on u + 0.3: even harmonics)
 *          5939-9010     MUFF  Big-Muff-style: stage 1 = the tube's soft knee on the biased u (ADAA), a coupling
 *                              high-pass (110 Hz), a low-pass (4 kHz), x6 + 0.2 into stage 2 (a - a^3/3, smooth
 *                              knee, saturated at +-1), then the tone stack's fixed mid scoop in pass 4 (mid band
 *                              250 Hz - 4 kHz x 0.3: -8 dB at 1 kHz). Thick, sustaining, odd harmonics with some even,
 *                              no gate. Loudness +5 dB over plain compensation (half the scoop at 1 kHz)
 *          11878-14949   FUZZ  gated octave-up rectifier fuzz: clamp(u / 8 + |u| - off, 0, 1), off = the signal's
 *                              mean |u| (slow-release envelope, at most 1.5x the fast one) + a gate threshold
 *                              (-48 dBFS in): 2nd harmonic 5-16 dB over the fundamental, gates and splutters as
 *                              notes decay
 *          17817-20888   CLIP  symmetric hard clip
 *          23756-26827   FOLD  triangle wavefolder, flat beyond +-17 (8 folds per side); depth 1x .. 16x (a -6 dBFS
 *                              sine: up to 4 folds per side); the slow envelope adds a bias (<= 0.5: even harmonics
 *                              on loud parts, fading with the note)
 *          29695-32767   CRUSH bit and sample-rate crusher (aliasing on purpose): blue takes the step from 2^-8 (9
 *                              bits) to 2^-2 (3 bits) of the clip level in 1.5 dB steps and the rate from 62 kHz to
 *                              1.9 kHz
 *   orange mix: dry .. wet (0 = the input, bit exact)
 * Master (stereo): L / R get -1 / +1 dB of drive (scaled by blue) and the crusher's R rate is 6 % higher: width.
 * Per block (64 frames), passes:
 *   0. per channel: the filter (Chamberlin SVF at 44.1 kHz, skipped at exactly 16384) and the block's mean |x|;
 *      envelopes shared by L / R: slow (5 ms attack, 190 ms release: fuzz offset, fold bias), fast (2.9 / 7 ms);
 *   1. drive (glided, saturated at +-32), a 3-tap treble lift, 2x up by linear interpolation (88.2 kHz);
 *   2. each active shape (at most two: neighbours) with its own input (bias, offset, depth), with first-order ADAA
 *      (tube, muff's stage 1, clip, fold; fuzz = clip of its input; not crush; in a crossfade only the closed-form
 *      mean), crossfaded with weights that include each shape's loudness compensation;
 *   3. 2x down (polyphase IIR half-band: flat to 20 kHz, 69 dB stop band from 26.5 kHz);
 *   4. the shapes' DC (tube and fold bias, the fuzz's mean from the fast envelope, muff's stage-2 bias), bands
 *      split at 4 kHz (and 250 Hz while MUFF has weight: its scoop) with loudness and the fizz shelf, DC blocker
 *      (7 Hz), mix.
 * Knobs: drive and character slew 2048 per block, green 1024; all gains glide linearly over the block: no clicks.
 * Fixed point: filter Q26, shaper input Q20, output Q24 (1.0 = clip level), 32 x 16-bit multiplies. The per-sample
 * loops are inline assembly except the crusher (GCC 4.3 spills and branches there); hardware loops
 * use LC0 (the FX entry wrapper saves the loop registers); I0-I3 / M0-M3 / B0 hold glides (L0-L3 = 0).
 *
 * For the screen (pure C, no state, no assembly; also builds alone with -DDISTORTION_CURVE_ONLY, e.g. on a host):
 *   int32_t distortion_curve(const int16_t *p, int32_t x);
 *       static transfer curve at knobs p[0..3]: output (Q15, clamped to +-32767) for input x (Q15, -32768..32767),
 *       with the drive, the character crossfade and the loudness compensation; without the filter, the shelf, the
 *       DC blocker, the envelope dynamics (fuzz offset and DC, fold bias as for a steady -6 dBFS sine) and the mix.
 *       Fuzz in its bipolar form (gate = -1 + its mean); crush: the quantiser's staircase (no rate reduction).
 *       MUFF: both stages without the filters around them. 210-305 cycles per call (simulator), up to ~450 inside
 *       a crossfade (two shapes; tube -> muff shares stage 1: ~300).
 *   const char *distortion_mode_name(const int16_t *p);
 *       the nearest character of p[2]: "TUBE", "MUFF", "FUZZ", "CLIP", "FOLD" or "CRUSH" (static strings; they
 *       switch at the crossfade midpoints 4506, 10445, 16384, 22323, 28262).
 */
#include <stdint.h>

#define BLOCK 64
#define U1 (1 << 20)                    /* shaper input 1.0 (Q20) */
#define KNEE (3 << 19)                  /* 1.5: the tube's knee (Q20) */
#define FOLDN 17                        /* fold: flat beyond +-17 (8 folds per side) */
#define BIAS 314573                     /* tube bias 0.3 (Q20), reached at blue 8192 */
#define TUBE_SB 310187                  /* tube_c(BIAS): s(0.3) at tube_c's resolution (Q20) */
#define MUFF_B2 209715                  /* MUFF stage-2 bias 0.2 (Q20): some even harmonics */
#define MUFF_S2 206976                  /* its DC: s2(0.2) as the DSP computes it (Q13 products), Q20 */
#define FUZZ_S0 209715                  /* fuzz gate offset cap 0.2 (Q20) */
#define ENV6 10430                      /* mean |x| of a -6 dBFS sine (Q15): the curve's envelope */
#define PLAT 3072                       /* character plateau and fade widths (knob units): 6 P + 5 F = 32767 */
#define FADE 2867
#define CSTEP (PLAT + FADE)

enum { TUBE, MUFF, FUZZ, CLIP, FOLD, CRUSH, NSH };

int32_t distortion_curve(const int16_t *p, int32_t x);
const char *distortion_mode_name(const int16_t *p);

/* ---- shared by the DSP and the curve: gain laws, character weights, loudness compensation (pure C) ---- */

/* Loudness compensation per shape at blue = 0, 2048, .. 32767 (Q12): rms(-6 dBFS 1 kHz sine) / rms(shaped, DC
 * removed), from the float model of the shapes (notes/fx-distortion.md):
 *   tube  8249 6143 4620 3527 2756 2191 1849 1681 1602 1555 1524 1502 1487 1477 1469 1463 1459
 *   muff  5110 4561 4323 4186 4100 4038 3997 3965 3941 3923 3911 3903 3899 3899 3898 3898 3898   (+5 dB: half the scoop at 1 kHz)
 *   fuzz  16000 13295 9817 7249 5353 3953 2919 2156 1871 1703 1573 1525 1500 1485 1475 1469 1465   (bipolar form 2 y - 1; 18005 capped)
 *   clip  8192 6049 4467 3298 2435 1891 1717 1627 1573 1536 1512 1494 1482 1473 1466 1461 1458
 *   fold  4096 3444 2896 2435 2198 2093 2187 2446 2779 2850 2618 2533 2602 2582 2467 2551 2563
 *   crush 3413 3269 3130 2997 2871 2748 2630 2518 2414 2307 2211 2119 2041 1944 1882 1916 1836
 * Packed per segment as c[i] | (c[i + 1] - c[i]) << 16: one load per lookup (loads are slow next to multiplies). */
static const int32_t comp_tab[NSH][16] = {
    {-138010567, -99805185, -71626228, -50524729, -37025084, -22411121, -11008199, -5175663, -3078590, -2030061,
     -1440268, -981538, -653873, -522811, -391747, -260681},
    {-35974154, -15593007, -8974109, -5631910, -4059132, -2682938, -2093155, -1568899, -1175707, -782509, -520377,
     -258241, 3899, -61637, 3898, 3898},
    {-177258880, -227920913, -168286631, -124249007, -91745047, -67760271, -50001049, -18675604, -11008177, -8517977,
     -3144155, -1636875, -981540, -653875, -391741, -260675},
    {-140435456, -103671903, -76607117, -56554270, -35649149, -11401373, -5896523, -3537317, -2423259, -1571328,
     -1178136, -784938, -588342, -457279, -326214, -195147},
    {-42725376, -35910284, -30209200, -15529597, -6879082, 6162477, 16976011, 21825934, 4655835, -15201502, -5567942,
     4524517, -1308118, -7534058, 5507491, 788983},
    {-9433771, -9106235, -8713158, -8254539, -8058057, -7730500, -7337402, -6813226, -7009938, -6289149, -6027101,
     -5109689, -6354951, -4061288, 2230106, -5240964},
};

/* Fuzz: steady-state mean of clamp(u / 8 + |u| - e, 0, 1) for a sine with mean |u| = e (its offset), at log2 e =
 * -3, -2.5, .. 5 (Q15); below e = 1/8 it is 0.215 e. The DSP subtracts it (from the fast envelope), so the gated
 * fuzz has no DC step when notes start and stop. */
static const int16_t fuzz_dc[17] = {880, 1244, 1759, 2488, 3518, 4975, 7036, 9797, 12014, 14197, 15491, 16284, 16810,
                                    17168, 17415, 17588, 17708};

/* fuzz_dc at pos = (log2 e + 3) in half octaves, Q15; Q20 out. */
static int32_t fuzz_dc_at(int32_t pos)
{
    int32_t i = pos >> 15, f = pos & 32767;
    if (i >= 16)
        return fuzz_dc[16] << 5;
    return (fuzz_dc[i] + (((fuzz_dc[i + 1] - fuzz_dc[i]) * f) >> 15)) << 5;
}

static inline int32_t kn(int16_t k)
{
    return k < 0 ? 0 : k;
}

/* Blue's taper: t = 1 - (1 - k)^2 (1 - 0.3 k), so the knob's upper half spans what was its top fifth (k 0.8 -> 1:
 * the heavy range players reach for) and the lower half the gentler rest (OP-1 voices reach the FX well below
 * full scale, so a linear-in-dB knob stayed nearly clean up to half way). 0 -> 0, 0.25 -> 0.48, 0.5 -> 0.79,
 * 0.75 -> 0.95, 1 -> 1. */
static inline int32_t drive_taper(int16_t k)
{
    int32_t x = kn(k), r = 32768 - x;
    int32_t r2 = (r * r) >> 15;                     /* (1 - k)^2, Q15 */
    int32_t f = 32768 - ((x * 9830) >> 15);         /* 1 - 0.3 k, Q15 */
    int32_t t = 32768 - ((r2 * f) >> 15);
    return t > 32767 ? 32767 : t;
}

static inline int32_t lim(int32_t x, int32_t lo, int32_t hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

/* 2^(x / 32768) * 65536 for x in 0 .. 13 * 32768: octave shift and a cubic for 2^f - 1 (1.7e-4 relative error, exact
 * at whole octaves; no table). */
static uint32_t exp2_q16(int32_t x)
{
    int32_t oct = x >> 15, f = x & 32767;
    int32_t t = (2536 * f) >> 15;
    t = ((7436 + t) * f) >> 15;
    t = ((22796 + t) * f) >> 15;
    return (65536u + (t << 1)) << oct;
}

static inline int32_t comp(int s, int32_t kd)
{
    int32_t c = comp_tab[s][kd >> 11];
    return (int16_t)c + (((c >> 16) * (kd & 2047)) >> 11);
}

/* Gains (Q16): g = 0.5 * 2^(7 b) for tube / fuzz / clip, fold depth 2^(4 b), b = blue / 32768. */
static inline int32_t gain_q16(int32_t kd)
{
    return exp2_q16(7 * kd) >> 1;
}

static inline int32_t fold_q16(int32_t kd)
{
    return exp2_q16(4 * kd);
}

/* Tube: s(v) = v - 4 v^3 / 27 on |v| <= 1.5, +-1 beyond (Q20 in and out; 2^-10 resolution). */
static int32_t tube_c(int32_t v)
{
    int32_t a = lim(v, -KNEE, KNEE) >> 10;          /* Q10 */
    int32_t a3 = ((a * a) >> 10) * a;               /* Q20 */
    return (a << 10) - (((a3 >> 5) * 4855) >> 10);  /* 4855 = 4/27 Q15 */
}

/* MUFF stage 2: s2(a) = a - a^3 / 3 on a = clamp(v, -1, 1) (Q20 in and out, 2^-10 resolution). */
static int32_t s2_c(int32_t v)
{
    int32_t a = lim(v, -U1, U1) >> 10;              /* Q10 */
    int32_t a3 = ((a * a) >> 10) * a;               /* Q20 */
    return (a << 10) - (((a3 >> 5) * 10923) >> 10); /* 10923 = 1/3 Q15 */
}

static inline int32_t tube_bias(int32_t kd)
{
    return kd >= 8192 ? BIAS : (BIAS * kd) >> 13;
}

/* Fold: 1 - |((w + 1) mod 4) - 2|, flat beyond +-17 (Q20). */
static int32_t fold_c(int32_t w)
{
    int32_t m = (lim(w, -FOLDN * U1, FOLDN * U1) + U1) & (4 * U1 - 1);
    m -= 2 * U1;
    return U1 - (m < 0 ? -m : m);
}

/* Crusher quantiser: step 2^-m, m = 8 - 6 b in quarter octaves (1.5 dB steps); round(c 2^m) 2^-m via 2^frac(m)
 * and its inverse. */
struct crq {
    int32_t m14, inv15, sh, half;
};

static const int16_t crush_m14[4] = {16384, 19484, 23170, 27554};      /* 2^(i / 4), Q14 */
static const int16_t crush_inv15[4] = {32767, 27554, 23170, 19484};    /* 2^(-i / 4), Q15 */

static void crush_q(int32_t kd, struct crq *q)
{
    int32_t m = (8 * 32768 - 6 * kd + 4096) >> 13, im = m >> 2;      /* quarter octaves, rounded */
    q->m14 = crush_m14[m & 3];
    q->inv15 = crush_inv15[m & 3];
    q->sh = 19 - im;
    q->half = 1 << (18 - im);
}

/* Crush input gain relative to x: 1.2 * 2^b (Q12). */
static inline int32_t crush_g12(int32_t kd)
{
    return (int32_t)((exp2_q16(kd) * 19661u) >> 18);
}

/* ---- the curve (pure C: 32-bit products only, each within 31 bits) ---- */

/* Shape s at drive kd for input x (Q15), with its DC offset as in the DSP and its loudness compensation; Q15. */
static inline __attribute__((always_inline)) int32_t shape_c(int s, int32_t kd, int32_t x)
{
    int32_t y;
    if (s == FOLD) {
        int32_t gf = fold_q16(kd) >> 5;             /* Q11 */
        int32_t w = (x * gf) >> 6, b = lim((ENV6 * gf) >> 8, 0, U1 / 2);
        if (kd > 28086) {                           /* the DSP's u = x g saturates at +-32: w at 32 gf / g */
            int32_t wl = (exp2_q16(3 * (32768 - kd)) >> 5) << 12;                          /* Q20 */
            w = lim(w, -wl, wl);
        }
        y = fold_c(w + b) - b;
    } else if (s == CRUSH) {
        struct crq q;
        crush_q(kd, &q);
        int32_t c = lim((x * crush_g12(kd)) >> 7, -U1, U1) >> 5;     /* Q15 */
        int32_t l = (((c * q.m14) >> 10) + q.half) >> q.sh;
        y = (((l << (q.sh + 1)) >> 5) * q.inv15) >> 10;
    } else {
        int32_t g8 = gain_q16(kd) >> 8;             /* Q8: 128 .. 16384 */
        int32_t u = lim((x * g8) >> 3, -32 * U1, 32 * U1);      /* Q20, saturated as in the DSP */
        if (s == TUBE) {
            int32_t b = tube_bias(kd);
            y = tube_c(u + b) - (b == BIAS ? TUBE_SB : tube_c(b));
        } else if (s == MUFF) {                     /* two stages, without the filters between and after */
            int32_t b = tube_bias(kd);              /* stage 1's DC (the coupling high-pass takes it): s(b) ~ b */
            y = s2_c(4 * (tube_c(u + b) - (b == BIAS ? TUBE_SB : b)) + MUFF_B2) - MUFF_S2;
        } else if (s == FUZZ) {                     /* bipolar form: clamp(2 w - 1), minus its mean */
            int32_t s0 = lim(g8 << 4, 0, FUZZ_S0), off = ((ENV6 * g8) >> 3) + s0;
            int32_t w = (u >> 3) + (u < 0 ? -u : u) - off;
            y = lim(2 * w - U1, -U1, U1) - 2 * fuzz_dc_at(22872 + 14 * kd) + U1;     /* log2 e = -2.65 + 7 b */
        } else {
            y = lim(u, -U1, U1);
        }
    }
    return ((y >> 5) * comp(s, kd)) >> 12;
}

int32_t distortion_curve(const int16_t *p, int32_t x)
{
    int32_t kd = drive_taper(p[0]), kc = kn(p[2]);
    int32_t j = (kc * 11300) >> 26, r = kc - j * CSTEP;            /* plateau j (kc / 5939), position in it */
    int32_t y, f = r > PLAT ? ((r - PLAT) * 46813) >> 14 : 0;     /* fading into j + 1: weight Q13 */
    if (j == TUBE && f) {                                           /* tube -> muff: one stage 1 for both */
        int32_t g8 = gain_q16(kd) >> 8, b = tube_bias(kd);
        int32_t y1 = tube_c(lim((x * g8) >> 3, -32 * U1, 32 * U1) + b) - (b == BIAS ? TUBE_SB : tube_c(b));
        int32_t ym = s2_c(4 * y1 + MUFF_B2) - MUFF_S2;
        y = ((y1 >> 5) * comp(TUBE, kd)) >> 12;
        return lim(y + (((((ym >> 5) * comp(MUFF, kd)) >> 12) - y) * f >> 13), -32767, 32767);
    }
    y = shape_c(j, kd, x);
    if (f)
        y += ((shape_c(j + 1, kd, x) - y) * f) >> 13;
    return lim(y, -32767, 32767);
}

const char *distortion_mode_name(const int16_t *p)
{
    int32_t kc = kn(p[2]);
    if (kc < CSTEP - FADE / 2)
        return "TUBE";
    if (kc < 2 * CSTEP - FADE / 2)
        return "MUFF";
    if (kc < 3 * CSTEP - FADE / 2)
        return "FUZZ";
    if (kc < 4 * CSTEP - FADE / 2)
        return "CLIP";
    if (kc < 5 * CSTEP - FADE / 2)
        return "FOLD";
    return "CRUSH";
}

#ifndef DISTORTION_CURVE_ONLY
/* ======================================== the DSP (Blackfin) ======================================== */

#define RAM __attribute__((section(".data")))
#define WMAX (60 << 20)                 /* shape input clamp (Q20) */
#define DC(d) ((d) >> 10)               /* DC blocker low-pass step: 6.9 Hz */
#define SLEW 2048                       /* drive and character: knob units per block */
#define GSLEW 1024                      /* green */
#define GDEAD 2048                      /* green: flat within +-GDEAD of 16384 */
#define FIXMAX 32                       /* straddling pairs recomputed per shape and block (CPU bound) */
#define FIXMIX 0                        /*   per shape while two crossfade (the closed-form mean only) */
#define TRIM_L 1782                     /* stereo drive trim at max blue: 1 - 10^(-1/20), Q14 */
#define TRIM_R 1999                     /*   10^(1/20) - 1 */
#define C0 2617                         /* half-band allpass coefficients, Q15 (HIIR design: 4 coefs, transition */
#define C1 9301                         /*   0.1): path 0 = C0, C2; path 1 = C1, C3 */
#define C2 17869
#define C3 27342

#define qmul(a, b) __builtin_bfin_mult_fr1x32x32((a), (b))     /* (a * b) >> 31, saturating; per block only */

void distortion_mono(void *self, int16_t *buf, const int16_t *p);
void distortion_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);

/* Filter coefficients, Chamberlin SVF at 44.1 kHz: f = 2 sin(pi fc / fs) (Q15), d = 1 / Q (Q14). Down side
 * (green 16384 -> 0): fc 6 kHz -> 150 Hz, Q 0.9 -> 5, both exponential; up side: fc 350 Hz -> 4 kHz, Q 2.5. All
 * stable (pole radius < 0.999). */
static const int16_t svf_fl[17] = {27167, 21819, 17451, 13920, 11085, 8818, 7010, 5571, 4426, 3515, 2792, 2217, 1761,
                                   1398, 1111, 882, 700};
static const int16_t svf_dl[17] = {18204, 16354, 14692, 13199, 11858, 10652, 9570, 8597, 7723, 6939, 6233, 5600,
                                   5031, 4519, 4060, 3648, 3277};
static const int16_t svf_fr[17] = {1634, 1902, 2215, 2579, 3003, 3497, 4071, 4740, 5517, 6422, 7474, 8697, 10116,
                                   11763, 13671, 15878, 18423};
#define SVF_DR 6554
/* log2 of the filter's peak gain (Q12 octaves; float model of the SVF and its output mix), per side as above. 3/4
 * of it is taken off the loudness compensation as extra drive for each shape's gain law (7 octaves for tube /
 * fuzz / clip, 4 for fold, 1 for crush): a resonant peak cannot push the output to full scale at low drive. */
static const int16_t svf_lgl[17] = {0, 0, 0, 83, 754, 2393, 3295, 3915, 4535, 5155, 5778, 6397, 7020, 7651, 8274,
                                    8905, 9503};
static const int16_t svf_lgr[17] = {0, 0, 0, 2870, 4796, 6246, 6501, 6505, 6510, 6517, 6526, 6539, 6556, 6580,
                                    6613, 6660, 6725};

struct chan {
    int32_t lp, bp;                     /* filter states (Q26) */
    int32_t v0, v1;                     /* the last two drive outputs (treble lift) */
    int32_t dn[6];                      /* downsampler: per path input and its allpasses */
    int32_t post, lo, dc;               /* band split low-passes (4 kHz, 250 Hz), DC blocker low-pass (Q24) */
    int32_t u;                          /* last 88.2 kHz shaper input of the previous block (ADAA), unbiased */
    int32_t ph, hv;                     /* crusher: hold phase (Q16), held value (Q24) */
    int32_t mh, ml;                     /* MUFF: coupling high-pass and low-pass states (Q24) */
};

/* Glided values: drive (Q24), per-shape mix weights incl. loudness (Q15), the shapes' DC offset (Q24), fuzz offset
 * and fold bias (Q20), fold and crush input gains (Q13 << 16), post gains low / mid / high (Q13 << 16), mix (Q15), filter f (Q15 <<
 * 16) and output mix (Q13 << 16). */
enum { G_DRV, G_A, G_OFF = G_A + NSH, G_FOFF, G_FB, G_KF, G_KC, G_QL, G_QM, G_QH, G_MIX, G_F, G_CL, G_CB, G_CH, NG };

struct dist {
    int32_t v[NG];                      /* values at the end of the last block */
    int32_t kd, kc, kg;                 /* slew-limited drive, character, green */
    int32_t bias;                       /* tube bias (Q20, slew-limited per block) */
    int32_t envs, envf;                 /* slow / fast envelope of mean |filtered x| (Q26) */
    struct chan ch[2];
};

#define CHAN0 {0, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, 0, 0}
#define DIST0 {{[G_CL] = 1 << 29, [G_CB] = 1 << 29, [G_CH] = 1 << 29}, 0, 0, 16384, 0, 0, 0, {CHAN0, CHAN0}}
static struct dist mono_st RAM = DIST0;
static struct dist st_st RAM = DIST0;

/* Scratch, shared by the mono and the stereo instance (TE calls them one after the other, never nested). */
static struct {
    int32_t xf[2][BLOCK];               /* filtered input (Q26) */
    int32_t u[2 * BLOCK + 1];           /* shaper input, u[0] = the previous block's last */
    int32_t v[2 * BLOCK + 1];           /* a shape's own input (bias, offset, depth) */
    int32_t y[2][2 * BLOCK + 2];        /* up to two shapes' outputs (neighbours only), from [2]; y[0][0..1]: the
                                         * downsampler's previous two inputs */
    int32_t t[BLOCK];                   /* 44.1 kHz after down */
    int32_t *list[2 * BLOCK];           /* straddling pairs (address after their y) */
} sc RAM = {{{0}}, {0}, {0}, {{0}}, {0}, {0}};

/* (a * c) >> 15 for 32-bit a and 16-bit c: two MACs (signed high half, mixed-mode low half). */
static inline int32_t mul16(int32_t a, int32_t c)
{
    int32_t r;
    __asm__("A1 = %2.L * %1.L (M);\n\tA0 = %1.H * %2.L;\n\tA1 = A1 >>> 15;\n\t%0 = (A0 += A1);"
            : "=d"(r) : "d"(a), "d"(c) : "A0", "A1");
    return r;
}

/* min / max as instructions (GCC 4.3 often branches for a two-sided clamp) */
static inline int32_t clamp(int32_t x, int32_t lo, int32_t hi)
{
    __asm__("%0 = max(%0, %1);\n\t%0 = min(%0, %2);" : "+d"(x) : "d"(lo), "d"(hi));
    return x;
}

static inline int32_t iabs(int32_t x)
{
    __asm__("%0 = abs %0;" : "+d"(x));
    return x;
}

/* ---- shapes (input Q20, output Q24; 1.0 = clip level) and their ADAA ----
 * tube: s(v) = v - 4 v^3 / 27 on |v| <= 1.5 (slope 1 at 0, flat at the knee), +-1 beyond; v = u + bias.
 *       Mean over [a, b] inside the knee: (a + b) / 2 - (a + b) (a^2 + b^2) / 27. F = v^2 / 2 - v^4 / 27.
 * hard: clamp(u, -1, 1). Mean inside one piece: clamp((a + b) / 2). F = u^2 / 2. (clip, and fuzz on its input)
 * fold: triangle through (0, 0), (1, 1), (3, -1), (5, 1), .., odd, flat beyond +-17: f = 1 - |((u + 1) mod 4) - 2|.
 *       Mean inside one piece: f((a + b) / 2). F = (f^2 - 1) / 2 on rising pieces, (1 - f^2) / 2 on falling ones.
 * Beyond the clamp, F grows by |u| - |clamped u| (f = sign u there), so F is continuous. Inputs stay within +-60, so
 * F differences fit 32 bits (F itself may wrap: only differences are used).
 * ADAA: y = (F(b) - F(a)) / (b - a), the mean of the shape between consecutive samples. Inside one piece the mean
 * has the closed form above (no division, and exact where F's rounding would not be); only pairs straddling a
 * breakpoint take the division. */

/* 2^14 / (1 + i / 64), i = 0 .. 64: reciprocal, linearly interpolated (7.6e-5 relative error) */
static const uint16_t recip_tab[65] = {
    16384, 16132, 15888, 15650, 15420, 15197, 14980, 14769, 14564, 14364, 14170, 13981, 13797, 13618, 13443, 13273,
    13107, 12945, 12788, 12633, 12483, 12336, 12193, 12053, 11916, 11782, 11651, 11523, 11398, 11275, 11155, 11038,
    10923, 10810, 10700, 10592, 10486, 10382, 10280, 10180, 10082, 9986, 9892, 9800, 9709, 9620, 9533, 9447,
    9362, 9279, 9198, 9118, 9039, 8962, 8886, 8812, 8738, 8666, 8595, 8525, 8456, 8389, 8322, 8257, 8192,
};

/* ---- pass 2 kernels: each shape into its buffer, 128 samples. Hand-written loops: y = the fast mean of each pair;
 * where the pair straddles a breakpoint, the address after its y goes on a list (conditional pointer move, no
 * branch). The listed pairs are recomputed below. Each returns the list end. ---- */

#define LOOP_IN "R1 = %[n];\n\tLC0 = R1;\n\tLSETUP (1f, 2f) LC0;\n1:\t"
#define LIST "[%[lp]] = %[y];\n\t%[lq] = %[lp] + %[four];\n\tIF CC %[lp] = %[lq];\n\t"
#define LOOP_OUT [up] "+a"(up), [y] "+a"(y), [lp] "+a"(lp), [lq] "=&a"(lq)
#define LOOP_CLOBBERS "R1", "R2", "R3", "A0", "M3", "cc", "LC0", "LT0", "LB0", "memory"

/* Hard: mean = clamp((a + b) / 2); straddle when a sign of (a -+ 1) and (b -+ 1) differs. */
static __attribute__((noinline)) int32_t **hard_fast(const int32_t *u, int32_t *y, int32_t **lp)
{
    int32_t a = u[0], am = a - U1, ap = a + U1, k = U1, nk = -U1;
    const int32_t *up = u + 1;
    int32_t **lq;
    __asm__ volatile(LOOP_IN
        "R1 = [%[up]++];\n\t"
        "R2 = %[a] + R1;\n\t"
        "R2 >>>= 1;\n\t"
        "R2 = MAX (R2, %[nk]);\n\t"
        "R2 = MIN (R2, %[k]);\n\t"
        "R2 <<= 4;\n\t"
        "[%[y]++] = R2;\n\t"
        "R2 = R1 - %[k];\n\t"
        "%[am] = %[am] ^ R2;\n\t"
        "R3 = R1 + %[k];\n\t"
        "%[ap] = %[ap] ^ R3;\n\t"
        "%[am] = %[am] | %[ap];\n\t"
        "CC = %[am] < 0;\n\t"
        LIST
        "%[am] = R2;\n\t"
        "%[ap] = R3;\n"
        "2:\t%[a] = R1;\n"
        : LOOP_OUT, [a] "+d"(a), [am] "+d"(am), [ap] "+d"(ap)
        : [k] "d"(k), [nk] "d"(nk), [four] "a"(4), [n] "i"(2 * BLOCK)
        : LOOP_CLOBBERS);
    return lp;
}

/* Tube (on v = u + bias): the closed-form mean of the pair clamped to the knee (16-bit products: 6e-4 at worst);
 * straddle as for hard, at +-1.5. */
static __attribute__((noinline)) int32_t **tube_fast(const int32_t *v, int32_t *y, int32_t **lp)
{
    int32_t a = clamp(v[0], -KNEE, KNEE), am = v[0] - KNEE, ap = v[0] + KNEE, k = KNEE, nk = -KNEE;
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
        : [k] "d"(k), [nk] "d"(nk), [four] "a"(4), [c27] "i"(1214), [n] "i"(2 * BLOCK)
        : LOOP_CLOBBERS);
    return lp;
}

/* Fold: mean = fold((a + b) / 2) of the pair clamped to +-(17 + e); straddle when the piece index differs. */
static __attribute__((noinline)) int32_t **fold_fast(const int32_t *u, int32_t *y, int32_t **lp)
{
    int32_t hi = FOLDN * U1 + 1, lo = -hi, one = U1;
    int32_t a = clamp(u[0], lo, hi), ka = (a + U1) >> 21;
    const int32_t *up = u + 1;
    int32_t **lq;
    __asm__ volatile(LOOP_IN
        "R1 = [%[up]++];\n\t"
        "R1 = MAX (R1, %[lo]);\n\t"
        "R1 = MIN (R1, %[hi]);\n\t"
        "R3 = R1 + %[one];\n\t"
        "R3 >>>= 21;\n\t"                    /* piece -9 .. 9 */
        "R2 = %[a] + R1;\n\t"
        "R2 >>>= 1;\n\t"
        "R2 = R2 + %[one];\n\t"
        "R2 <<= 10;\n\t"
        "R2 >>= 10;\n\t"                     /* (mid + 1) mod 4 */
        "R2 = R2 - %[one];\n\t"
        "R2 = R2 - %[one];\n\t"
        "R2 = ABS R2;\n\t"
        "R2 = %[one] - R2;\n\t"
        "R2 <<= 4;\n\t"
        "[%[y]++] = R2;\n\t"
        "CC = R3 == %[ka];\n\t"
        "CC = ! CC;\n\t"
        LIST
        "%[ka] = R3;\n"
        "2:\t%[a] = R1;\n"
        : LOOP_OUT, [a] "+d"(a), [ka] "+d"(ka)
        : [lo] "d"(lo), [hi] "d"(hi), [one] "d"(one), [four] "a"(4), [n] "i"(2 * BLOCK)
        : LOOP_CLOBBERS);
    return lp;
}

/* Straddling pairs: (F(b) - F(a)) / (b - a); for a step below 2^-10 the fast mean stays (F's rounding would
 * dominate). F inside the clamp from a Q29/Q30 square (32 x 32 fractional multiply, SQ). The division: |du|
 * normalised (SIGNBITS), 2^14 / it from recip_tab, dF pre-shifted by n - 5 so the product keeps 15+ bits.
 * At most FIXMAX per shape and block, which bounds the CPU at high drive on bright input (the rest keep the fast
 * mean: more aliasing only there). Registers: R0 a, R1 b, R2 F(b), R5 F(a), R3/R4 temporaries, R6/R7 +-clamp,
 * M1 1.0 (Q24), M2 1.0 (Q20), M3 2/27 (Q15); I0 the y entry. */
#define SQ(X, Y) /* Y = X * X >> 31 */                                                                              \
    "A1 = " X ".L * " X ".L (FU);\n\tA1 = A1 >> 16;\n\tA0 = " X ".H * " X ".H, A1 += " X ".H * " X ".L (M);\n\t"     \
    "A1 += " X ".H * " X ".L (M);\n\tA1 = A1 >>> 15;\n\t" Y " = (A0 += A1);\n\t"
#define F_HARD(I, O, T1, T2) /* u^2 / 2 inside, |u| - |c| outside */                                                \
    T1 " = MAX (" I ", R7);\n\t" T1 " = MIN (" T1 ", R6);\n\t" T2 " = ABS " I ";\n\t" O " = ABS " T1 ";\n\t"       \
    T2 " = " T2 " - " O ";\n\t" T2 " <<= 4;\n\t" T1 " <<= 10;\n\t" SQ(T1, O) O " >>>= 6;\n\t"                     \
    O " = " O " + " T2 ";\n\t"
#define F_TUBE(I, O, T1, T2) /* v^2 / 2 - v^4 / 27 inside */                                                         \
    O " = MAX (" I ", R7);\n\t" O " = MIN (" O ", R6);\n\t" T1 " = ABS " I ";\n\t" T2 " = ABS " O ";\n\t"           \
    T1 " = " T1 " - " T2 ";\n\t" T1 " <<= 4;\n\t" O " <<= 9;\n\t" SQ(O, O) SQ(O, T2) O " >>>= 4;\n\t"             \
    O " = " O " + " T1 ";\n\t" T1 " = M3;\n\tA1 = " T1 ".L * " T2 ".L (M);\n\tA0 = " T2 ".H * " T1 ".L;\n\t"       \
    "A1 = A1 >>> 15;\n\t" T2 " = (A0 += A1);\n\t" O " = " O " - " T2 ";\n\t"
#define F_FOLD(I, O, T1, T2) /* +-(f^2 - 1) / 2 on rising / falling pieces */                                       \
    O " = MAX (" I ", R7);\n\t" O " = MIN (" O ", R6);\n\t" T1 " = ABS " I ";\n\t" T2 " = ABS " O ";\n\t"           \
    T1 " = " T1 " - " T2 ";\n\t" T1 " <<= 4;\n\t" T2 " = M2;\n\t" O " = " O " + " T2 ";\n\t" O " <<= 10;\n\t"      \
    O " >>= 10;\n\t" O " = " O " - " T2 ";\n\t" O " = " O " - " T2 ";\n\tCC = " O " < 0;\n\t" O " = ABS " O ";\n\t" \
    O " = " T2 " - " O ";\n\t" O " <<= 10;\n\t" SQ(O, O) O " >>>= 5;\n\t" T2 " = M1;\n\t" O " = " O " - " T2 ";\n\t" \
    O " >>>= 1;\n\t" T2 " = -" O ";\n\tIF !CC " O " = " T2 ";\n\t" O " = " O " + " T1 ";\n\t"
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

enum { K_TUBE, K_HARD, K_FOLD };

/* Kernel k over the block (input u[0 .. 128]) into y, then the straddling pairs. */
static __attribute__((noinline)) void shape_pass(int k, const int32_t *u, int32_t *y, int32_t fixmax)
{
    int32_t **lp = sc.list, **end, n, off = (const char *)u - (const char *)y;
    end = k == K_TUBE ? tube_fast(u, y, lp) : k == K_HARD ? hard_fast(u, y, lp) : fold_fast(u, y, lp);
    n = end - lp;
    n = n > fixmax ? fixmax : n;
    if (n <= 0)
        return;
    if (k == K_TUBE)
        FIX(F_TUBE, KNEE);
    else if (k == K_HARD)
        FIX(F_HARD, U1);
    else
        FIX(F_FOLD, FOLDN * U1);
}

/* MUFF after stage 1 (the tube kernel with ADAA on the biased u, output in y): the coupling high-pass (one-pole step
 * 2^-7: 110 Hz; it also takes stage 1's DC), a gentle low-pass (step 1/4: 4 kHz), then stage 2: s2(a) = a - a^3 / 3
 * on a = 4 lp + 0.2 saturated to +-1 (a saturating shift; Q13 products; the same knee as the tube's at 1.5 x the
 * scale, i.e. a gain of 6 after stage 1), in place (Q24, +-2/3). Stage 1 carries the anti-aliasing (it clips
 * hardest at high drive); stage 2 sees a low-passed signal. */
static __attribute__((noinline)) void muff_post(struct chan *h, int32_t *y)
{
    int32_t mh = h->mh, ml = h->ml, b2 = MUFF_B2, c3 = 10923;
    const int32_t *ip = y + 1;
    int32_t *op = y;
    __asm__ volatile(
        "R2 = [%[op]];\n\t"                     /* the first sample; the loop loads the next in parallel */
        "R0 = %[n];\n\tLC0 = R0;\n\t"
        "LSETUP (1f, 2f) LC0;\n"
        "1:\tR1 = R2 - %[mh];\n\t"
        "R1 >>>= 7;\n\t"
        "%[mh] = %[mh] + R1;\n\t"
        "R0 = R2 - %[mh];\n\t"                  /* high-passed */
        "R1 = R0 - %[ml];\n\t"
        "R1 >>>= 2;\n\t"
        "%[ml] = %[ml] + R1;\n\t"               /* low-passed */
        "R0 = %[ml] >>> 2;\n\t"
        "R0 = R0 + %[b2];\n\t"                  /* 4 lp + 0.2, Q20 */
        "R0 = R0 << 11 (S);\n\t"
        "R0 >>>= 18;\n\t"                       /* a, Q13, +-1 */
        "R1 = R0.L * R0.L (IS) || R2 = [%[ip]++] || NOP;\n\t"
        "R1 >>>= 13;\n\t"
        "R1 = R1.L * R0.L (IS);\n\t"
        "R1 >>>= 13;\n\t"                       /* a^3 */
        "R1 = R1.L * %[c3].L (IS);\n\t"
        "R1 >>>= 15;\n\t"
        "R0 = R0 - R1;\n\t"
        "R0 <<= 11;\n"                           /* Q24 */
        "2:\t[%[op]++] = R0;\n"
        : [ip] "+a"(ip), [op] "+a"(op), [mh] "+d"(mh), [ml] "+d"(ml)
        : [b2] "d"(b2), [c3] "d"(c3), [n] "i"(2 * BLOCK)
        : "R0", "R1", "R2", "cc", "LC0", "LT0", "LB0", "memory");
    h->mh = mh, h->ml = ml;
}

/* Fuzz input over u[0 .. 128]: 2 (u / 8 + |u| - off) - 1, so the clip at +-1 is the gate (w <= 0: 0) and the clip
 * (w >= 1) of w. u without the tube's bias bt; off2 = 2 off + 1 glides by doff2. */
static __attribute__((noinline)) void fuzz_in(const int32_t *u, int32_t *w, int32_t off2, int32_t doff2, int32_t bt)
{
    int32_t hi = WMAX, lo = -WMAX;
    __asm__ volatile(
        "I0 = %[off];\n\tM0 = %[doff];\n\t"
        "R0 = %[n];\n\tLC0 = R0;\n\t"
        "LSETUP (1f, 2f) LC0;\n"
        "1:\tR0 = [%[up]++];\n\t"
        "R0 = R0 - %[bt];\n\t"
        "R1 = ABS R0;\n\t"
        "R0 >>>= 3;\n\t"
        "R0 = R0 + R1;\n\t"
        "R1 = I0;\n\t"
        "R0 = R0 + R0 (NS) || I0 += M0 || NOP;\n\t"
        "R0 = R0 - R1;\n\t"
        "R0 = MAX (R0, %[lo]);\n\t"
        "R0 = MIN (R0, %[hi]);\n"
        "2:\t[%[wp]++] = R0;\n"
        : [up] "+a"(u), [wp] "+a"(w)
        : [off] "d"(off2), [doff] "d"(doff2), [bt] "d"(bt), [lo] "d"(lo), [hi] "d"(hi), [n] "i"(2 * BLOCK + 1)
        : "R0", "R1", "I0", "M0", "cc", "LC0", "LT0", "LB0", "memory");
}

/* Fold input over u[0 .. 128]: u k + b (k: depth / g, Q13 high half; b: bias, Q20), clamped to +-60; k and b glide
 * (gk: value, step, value, step). */
static __attribute__((noinline)) void fold_in(const int32_t *u, int32_t *w, const int32_t *gk)
{
    int32_t hi = WMAX, lo = -WMAX;
    __asm__ volatile(
        "R0 = [%[gk]];\n\tI0 = R0;\n\tR0 = [%[gk] + 4];\n\tM0 = R0;\n\t"
        "R0 = [%[gk] + 8];\n\tI1 = R0;\n\tR0 = [%[gk] + 12];\n\tM1 = R0;\n\t"
        "R0 = %[n];\n\tLC0 = R0;\n\t"
        "LSETUP (1f, 2f) LC0;\n"
        "1:\tR0 = [%[up]++];\n\t"
        "R1 = I0;\n\t"
        "A1 = R1.H * R0.L (M) || I0 += M0 || NOP;\n\t"
        "A0 = R0.H * R1.H;\n\t"
        "R1 = I1;\n\t"
        "A1 = A1 >>> 15 || I1 += M1 || NOP;\n\t"
        "R0 = (A0 += A1);\n\t"
        "R0 <<= 2;\n\t"
        "R0 = R0 + R1;\n\t"
        "R0 = MAX (R0, %[lo]);\n\t"
        "R0 = MIN (R0, %[hi]);\n"
        "2:\t[%[wp]++] = R0;\n"
        : [up] "+a"(u), [wp] "+a"(w)
        : [gk] "a"(gk), [lo] "d"(lo), [hi] "d"(hi), [n] "i"(2 * BLOCK + 1)
        : "R0", "R1", "A0", "A1", "I0", "I1", "M0", "M1", "cc", "LC0", "LT0", "LB0", "memory");
}

/* Crusher: sample-and-hold at rinc / 65536 of 88.2 kHz of the clipped, scaled input, quantised (no ADAA). */
static __attribute__((noinline)) void crush_pass(struct chan *h, const int32_t *u, int32_t *y, int32_t k, int32_t dk,
                                                 int32_t rinc, const struct crq *q)
{
    int32_t ph = h->ph, hv = h->hv;
    for (int i = 1; i <= 2 * BLOCK; i++) {
        k += dk;
        ph += rinc;
        if (ph >= 65536) {
            ph -= 65536;
            int32_t c = clamp(mul16(u[i], k >> 16) << 2, -U1, U1);
            int32_t l = (mul16(c, q->m14) + q->half) >> q->sh;
            hv = mul16(l << (q->sh + 1), q->inv15) << 4;
        }
        y[i - 1] = hv;
    }
    h->ph = ph, h->hv = hv;
}

/* Pass 0: the filter. hp = x - lp - d bp; out = cl lp + cb (d bp) + ch hp from the states BEFORE the update, so out
 * = x exactly when cl = cb = ch = 1, whatever f and d; then bp += f hp; lp += f bp. gm: glides (value, step) of f
 * (Q15 high half, in I0) and the mix weights (Q13 high halves, I1-I3), then d (Q14), which waits in B0 (L0 = 0:
 * B0 is free).
 * Returns the mean |out| (Q26). */
static __attribute__((noinline)) int32_t svf_pass(struct chan *h, const int16_t *x, int32_t *xf, const int32_t *gm)
{
    int32_t lp = h->lp, bp = h->bp, sum = 0;
    __asm__ volatile(
        "R0 = [%[gm] + 32];\n\tB0 = R0;\n\t"
        "R0 = [%[gm]];\n\tI0 = R0;\n\tR0 = [%[gm] + 4];\n\tM0 = R0;\n\t"
        "R0 = [%[gm] + 8];\n\tI1 = R0;\n\tR0 = [%[gm] + 12];\n\tM1 = R0;\n\t"
        "R0 = [%[gm] + 16];\n\tI2 = R0;\n\tR0 = [%[gm] + 20];\n\tM2 = R0;\n\t"
        "R0 = [%[gm] + 24];\n\tI3 = R0;\n\tR0 = [%[gm] + 28];\n\tM3 = R0;\n\t"
        "R0 = %[n];\n\tLC0 = R0;\n\t"
        "LSETUP (1f, 2f) LC0;\n"
        "1:\tR0 = W [%[xp]++] (X);\n\t"
        "R0 <<= 11;\n\t"
        "R2 = B0;\n\t"
        "A1 = R2.L * %[bp].L (M) || I1 += M1 || NOP;\n\t"
        "A0 = %[bp].H * R2.L || I2 += M2 || NOP;\n\t"
        "A1 = A1 >>> 15 || I3 += M3 || NOP;\n\t"
        "R1 = (A0 += A1) || I0 += M0 || NOP;\n\t"
        "R1 <<= 1;\n\t"                         /* d bp */
        "R0 = R0 - %[lp];\n\t"
        "R0 = R0 - R1;\n\t"                     /* hp */
        "R2 = I1;\n\tR3 = I2;\n\tR4 = I3;\n\t"
        "A1 = R2.H * %[lp].L (M);\n\tA0 = %[lp].H * R2.H;\n\t"
        "A1 += R3.H * R1.L (M);\n\tA0 += R1.H * R3.H;\n\t"
        "A1 += R4.H * R0.L (M);\n\tA0 += R0.H * R4.H;\n\t"
        "A1 = A1 >>> 15;\n\tR1 = (A0 += A1);\n\t"
        "R1 <<= 2;\n\t"                         /* out, Q26 */
        "[%[yp]++] = R1;\n\t"
        "R1 = ABS R1;\n\t"
        "R1 >>= 6;\n\t"
        "%[sum] = %[sum] + R1;\n\t"
        "R2 = I0;\n\t"                          /* f */
        "A1 = R2.H * R0.L (M);\n\tA0 = R0.H * R2.H;\n\tA1 = A1 >>> 15;\n\tR1 = (A0 += A1);\n\t"
        "%[bp] = %[bp] + R1;\n\t"
        "A1 = R2.H * %[bp].L (M);\n\tA0 = %[bp].H * R2.H;\n\tA1 = A1 >>> 15;\n\tR1 = (A0 += A1);\n"
        "2:\t%[lp] = %[lp] + R1;\n"
        : [xp] "+a"(x), [yp] "+a"(xf), [lp] "+d"(lp), [bp] "+d"(bp), [sum] "+d"(sum)
        : [gm] "a"(gm), [n] "i"(BLOCK)
        : "R0", "R1", "R2", "R3", "R4", "A0", "A1", "I0", "I1", "I2", "I3", "M0", "M1", "M2", "M3", "B0", "cc", "LC0",
          "LT0", "LB0", "memory");
    h->lp = lp, h->bp = bp;
    return sum;
}

/* Character weights (Q15, sum 32768) from the white knob: at most two neighbours. */
static void weights(int32_t kc, int32_t *w)
{
    int32_t pos = 0;
    for (int i = 0; i < NSH - 1; i++) {
        int32_t d = lim(kc - (i * CSTEP + PLAT), 0, FADE);
        pos += d == FADE ? 32768 : (d * 46813) >> 12;                   /* 2867 -> 32768 */
    }
    int32_t i = pos >> 15, f = pos & 32767;
    for (int s = 0; s < NSH; s++)
        w[s] = 0;
    w[i] = 32768 - f;
    if (f)
        w[i + 1] = f;
}

/* fuzz_dc for e (Q20): log2 e from the top bit and a linear mantissa. */
static int32_t fuzz_dc_env(int32_t e)
{
    int32_t p = 17;
    if (e < U1 / 8)
        return (e * 7036) >> 15;                    /* 0.215 e */
    while (p < 30 && e >> (p + 1))
        p++;
    return fuzz_dc_at(((p - 17) << 16) + (((e - (1 << p)) >> (p - 15)) << 1));
}

/* Crossfade y += (yb - y) a over 128 samples; a glides (Q15 x 65535: the high half stays <= 32767). */
static __attribute__((noinline)) void xfade(int32_t *y, const int32_t *yb, int32_t a, int32_t da)
{
    __asm__ volatile(
        "I0 = %[a];\n\tM0 = %[da];\n\tI0 += M0;\n\t"
        "R0 = %[n];\n\tLC0 = R0;\n\t"
        "LSETUP (1f, 2f) LC0;\n"
        "1:\tR0 = [%[yp]];\n\t"
        "R1 = [%[bp]++];\n\t"
        "R1 = R1 - R0;\n\t"
        "R2 = I0;\n\t"
        "A1 = R2.H * R1.L (M) || I0 += M0 || NOP;\n\t"
        "A0 = R1.H * R2.H;\n\t"
        "A1 = A1 >>> 15;\n\t"
        "R1 = (A0 += A1);\n\t"
        "R0 = R0 + R1;\n"
        "2:\t[%[yp]++] = R0;\n"
        : [yp] "+a"(y), [bp] "+a"(yb)
        : [a] "d"(a), [da] "d"(da), [n] "i"(2 * BLOCK)
        : "R0", "R1", "R2", "A0", "A1", "I0", "M0", "cc", "LC0", "LT0", "LB0", "memory");
}

/* Slew-limit a knob. */
static inline int32_t slew(int32_t cur, int32_t t, int32_t r)
{
    return clamp(t, cur - r, cur + r);
}

/* num / den in Q15 (num <= den < 2^30), bitwise: no division instruction. */
static int32_t ratio15(int32_t num, int32_t den)
{
    int32_t q = 0;
    if (num >= den)
        return 32768;
    for (int i = 0; i < 15; i++) {
        num <<= 1;
        q <<= 1;
        if (num >= den)
            num -= den, q |= 1;
    }
    return q;
}

/* Pass 1's loop body after the load into R0 (Q26 input); END labels its last instruction (the loop end). */
#define PASS1_BODY(END) \
    "R1 = I0;\n\t" \
    "A1 = R1.H * R0.L (M) || I0 += M0 || NOP;\n\t" \
    "A0 = R0.H * R1.H;\n\t" \
    "A1 = A1 >>> 15;\n\t" \
    "R0 = (A0 += A1);\n\t" \
    "R0 = R0 << 7 (S);\n\t" \
    "R0 >>>= 6;\n\t"                  /* v, Q20, saturated to +-32 */ \
    "R1 = %[v1] + %[v1];\n\t" \
    "R1 = R1 - %[v0];\n\t" \
    "R1 = R1 - R0;\n\t" \
    "R1 >>>= 3;\n\t" \
    "R1 = R1 + %[v1];\n\t"            /* en */ \
    "%[v0] = %[v1];\n\t" \
    "%[v1] = R0;\n\t" \
    "R2 = %[e] + R1;\n\t" \
    "R2 >>>= 1;\n\t" \
    "%[e] = R1;\n\t" \
    "R2 = R2 + %[b];\n\t" \
    "R1 = R1 + %[b];\n\t" \
    "[%[up]++] = R2;\n" \
    END ":\t[%[up]++] = R1;\n\t"

#define PASS1_SETUP \
    "R0 = [%[gp]];\n\tI0 = R0;\n\tR0 = [%[gp] + 4];\n\tM0 = R0;\n\t" \
    "I0 += M0;\n\t" \
    "R0 = %[n];\n\tLC0 = R0;\n\t" \
    "LSETUP (1f, 2f) LC0;\n"
#define PASS1_OPS \
    : [xp] "+a"(xp), [up] "+a"(up), [v0] "+d"(v0), [v1] "+d"(v1), [e] "+d"(e) \
    : [gp] "a"(gp), [n] "i"(BLOCK), [b] "d"(b) \
    : "R0", "R1", "R2", "A0", "A1", "I0", "M0", "cc", "LC0", "LT0", "LB0", "memory"

/* One block for nch channels with shared knob glides. */
static void dist(struct dist *s, int16_t **io, int nch, const int16_t *p)
{
    int32_t kd = s->kd = slew(s->kd, drive_taper(p[0]), SLEW), kc = s->kc = slew(s->kc, kn(p[2]), SLEW);
    int32_t kg0 = s->kg, kg = s->kg = slew(s->kg, kn(p[1]), GSLEW);
    int32_t t[NG], st[NG], w[NSH], c[NSH];

    /* filter: coefficients per block, output mix glided */
    int32_t f, d, lg, tg = kg - 16384;
    {
        int32_t a = tg < 0 ? -tg : tg, i = a >> 10, fr = a & 1023, m = clamp((a - GDEAD) * 5, 0, 16384);
        if (i >= 16)
            i = 15, fr = 1024;
        if (tg < 0) {
            f = svf_fl[i] + (((svf_fl[i + 1] - svf_fl[i]) * fr) >> 10);
            d = svf_dl[i] + (((svf_dl[i + 1] - svf_dl[i]) * fr) >> 10);
            lg = svf_lgl[i] + (((svf_lgl[i + 1] - svf_lgl[i]) * fr) >> 10);
            t[G_CL] = 8192, t[G_CB] = t[G_CH] = (16384 - m) >> 1;
        } else {
            f = svf_fr[i] + (((svf_fr[i + 1] - svf_fr[i]) * fr) >> 10);
            d = SVF_DR;
            lg = svf_lgr[i] + (((svf_lgr[i + 1] - svf_lgr[i]) * fr) >> 10);
            t[G_CL] = t[G_CH] = (16384 - ((m * 3) >> 2)) >> 1, t[G_CB] = (16384 + 2 * m) >> 1;
        }
        t[G_F] = f << 16, t[G_CL] <<= 16, t[G_CB] <<= 16, t[G_CH] <<= 16;
    }
    /* pass 0 for every channel first: the envelope (shared) needs them all */
    int32_t emax = 0, flat = kg0 == 16384 && kg == 16384;
    for (int ch = 0; ch < nch; ch++) {
        struct chan *h = &s->ch[ch];
        int32_t e = 0, *xf = sc.xf[ch];
        if (flat) {                                 /* pass 1 reads the input itself; every 4th sample for the envelope */
            h->lp = h->bp = 0;
            for (int i = 0; i < BLOCK; i += 4)
                e += iabs(io[ch][i]) << 7;
        } else {
            const int32_t gm[9] = {s->v[G_F], (t[G_F] - s->v[G_F]) >> 6, s->v[G_CL], (t[G_CL] - s->v[G_CL]) >> 6,
                                   s->v[G_CB], (t[G_CB] - s->v[G_CB]) >> 6, s->v[G_CH], (t[G_CH] - s->v[G_CH]) >> 6, d};
            e = svf_pass(h, io[ch], xf, gm);
        }
        emax = e > emax ? e : emax;
    }
    s->envs += (emax - s->envs) >> (emax > s->envs ? 2 : 7);      /* attack 5 ms, release 190 ms (per block) */
    s->envf += (emax - s->envf) >> (emax > s->envf ? 1 : 2);      /* 2.9 / 7 ms */

    /* mix weights with loudness: a_s = w_s c_s / sum(w c), post gain sum(w c) */
    int32_t den = 0, act[NSH];
    weights(kc, w);
    lg = (lg * 3) << 1;                                              /* 3/4, Q15 octaves */
    for (int i = 0; i < NSH; i++) {
        int32_t dk = i == CRUSH ? lg : i == FOLD ? lg >> 2 : (lg * 4681) >> 15;
        c[i] = w[i] ? comp(i, clamp(kd + dk, 0, 32767)) : 0;
        den += w[i] * c[i] >> 4;
        act[i] = w[i] || s->v[G_A + i];
    }
    for (int i = 0; i < NSH; i++)
        t[G_A + i] = w[i] ? ratio15(w[i] * c[i] >> 4, den) : 0;
    int32_t pc = den >> 11;                                          /* Q12 */

    /* drive and the active shapes' own inputs (the others keep theirs) */
    int32_t gq = gain_q16(kd), sbias = 0, dcz = 0;
    t[G_DRV] = gq << 8;
    for (int i = G_FOFF; i <= G_KC; i++)
        t[i] = s->v[i];
    if (act[FUZZ]) {
        int32_t envs_u = qmul(s->envs << 2, gq << 8) >> 1, envf_u = qmul(s->envf << 2, gq << 8) >> 1;   /* Q20 */
        t[G_FOFF] = clamp(envs_u, 0, envf_u + (envf_u >> 1)) + clamp(gq >> 4, 0, FUZZ_S0);    /* <= 1.5 x fast */
        dcz = 2 * fuzz_dc_env(envf_u) - U1;                          /* the output's DC (bipolar form) */
    }
    if (act[FOLD]) {
        t[G_FB] = clamp(qmul(s->envs << 2, fold_q16(kd) << 8) >> 3, 0, U1 / 2);
        t[G_KF] = (exp2_q16(3 * (32768 - kd)) >> 5) << 16;         /* 2 * 2^(-3 b) = depth / g */
    }
    if (act[CRUSH])
        t[G_KC] = (int32_t)((exp2_q16(6 * (32768 - kd)) >> 6) * 19661u >> 16) << 16;   /* 2.4 * 2^(-6 b) */
    {
        int32_t bt = tube_bias(kd);
        s->bias = clamp(bt, s->bias - (BIAS >> 6), s->bias + (BIAS >> 6));
        if (act[TUBE])
            sbias = tube_c(s->bias) << 4;                            /* Q24 */
    }
    t[G_OFF] = mul16(sbias, clamp(t[G_A + TUBE], 0, 32767)) + mul16(t[G_FB] << 4, clamp(t[G_A + FOLD], 0, 32767))
             + mul16(dcz << 4, clamp(t[G_A + FUZZ], 0, 32767)) + mul16(MUFF_S2 << 4, clamp(t[G_A + MUFF], 0, 32767));
    {
        int32_t cut = mul16(mul16(kd, 21299), 32767 - (t[G_A + CRUSH] >> 1));  /* fizz shelf: to -9 dB (crush -4) */
        int32_t mid = 32767 - ((t[G_A + MUFF] * 22938) >> 15);                 /* MUFF scoop: mid band x 0.3 */
        t[G_QL] = pc << 17;
        t[G_QM] = ((pc * mid) >> 15) << 17;
        t[G_QH] = ((pc * (32767 - cut)) >> 15) << 17;
    }
    t[G_MIX] = kn(p[3]);
    for (int i = 0; i < NG; i++)
        st[i] = (t[i] - s->v[i]) >> 6;              /* per frame */

    struct crq q = {0, 0, 0, 0};
    int32_t rinc = 0;
    if (act[CRUSH]) {
        crush_q(kd, &q);
        rinc = clamp(exp2_q16(5 * (32768 - kd) + 16384) >> 6, 0, 65536);           /* 2^(-0.5 - 5 b) */
    }
    int32_t trim[2] = {16384, 16384}, tw = act[TUBE] || act[MUFF], mw = act[MUFF];   /* tw: u carries the bias */
    if (nch == 2)
        trim[0] = 16384 - ((TRIM_L * kd) >> 15), trim[1] = 16384 + ((TRIM_R * kd) >> 15);

    for (int ch = 0; ch < nch; ch++) {
        struct chan *h = &s->ch[ch];
        int16_t *x = io[ch];
        int32_t *u = sc.u, *y = sc.y[0] + 2, *tmp = sc.t;

        /* 1: drive (Q20, saturated to +-32: keeps the ADAA's F differences in 32 bits), treble lift e = v1 + (2 v1 -
         * v0 - v) / 8 (one frame late), 2x up: e and the midpoint before it, plus the tube bias while the tube has
         * weight (u[0] = the previous block's last e with this block's bias). The gain glides in I0 (+= M0); the
         * multiply uses its high half (Q8). */
        {
            int32_t v0 = h->v0, v1 = h->v1, e = h->u, b = tw ? s->bias : 0;
            const void *xp = flat ? (const void *)x : (const void *)sc.xf[ch];
            int32_t *up = u + 1;
            const int32_t gp[2] = {mul16(s->v[G_DRV], trim[ch]) << 1, mul16(st[G_DRV], trim[ch]) << 1};
            u[0] = e + b;
            if (flat)                               /* from the int16 input (filter bypassed) */
                __asm__ volatile(PASS1_SETUP "1:\tR0 = W [%[xp]++] (X);\n\tR0 <<= 11;\n\t" PASS1_BODY("2")
                                 PASS1_OPS);
            else
                __asm__ volatile(PASS1_SETUP "1:\tR0 = [%[xp]++];\n\t" PASS1_BODY("2") PASS1_OPS);
            h->v0 = v0, h->v1 = v1, h->u = e;
        }

        /* 2: the shapes with weight in this block (at most two neighbours: a plateau is wider than SLEW), mixed */
        {
            int na = 0, nact = 0;
            int32_t a = 0, da = 0, fm;
            for (int sh = 0; sh < NSH; sh++)
                nact += s->v[G_A + sh] || t[G_A + sh];
            fm = nact > 1 ? FIXMIX : FIXMAX;
            for (int sh = 0; sh < NSH; sh++) {
                int32_t a0 = s->v[G_A + sh], a1 = t[G_A + sh], *yo = sc.y[na] + 2, *v = sc.v;
                if (!a0 && !a1)
                    continue;
                if (sh == TUBE) {
                    shape_pass(K_TUBE, u, yo, fm);                  /* u carries the bias */
                } else if (sh == MUFF) {
                    if (na)                                         /* the tube ran: stage 1 is the same */
                        for (int i = 0; i < 2 * BLOCK; i++)
                            yo[i] = y[i];
                    else
                        shape_pass(K_TUBE, u, yo, fm);              /* stage 1, on the biased u */
                    muff_post(h, yo);
                } else if (sh == FUZZ) {
                    fuzz_in(u, v, 2 * s->v[G_FOFF] + U1, (t[G_FOFF] - s->v[G_FOFF]) >> 6, tw ? s->bias : 0);
                    shape_pass(K_HARD, v, yo, fm);
                } else if (sh == CLIP) {
                    shape_pass(K_HARD, u, yo, fm);
                } else if (sh == FOLD) {
                    const int32_t gk[4] = {s->v[G_KF], (t[G_KF] - s->v[G_KF]) >> 7, s->v[G_FB], (t[G_FB] - s->v[G_FB]) >> 7};
                    fold_in(u, v, gk);
                    shape_pass(K_FOLD, v, yo, fm);
                } else {
                    int32_t r = clamp(ch ? rinc + (rinc >> 4) : rinc, 0, 65536);
                    crush_pass(h, u, yo, s->v[G_KC], (t[G_KC] - s->v[G_KC]) >> 7, r, &q);
                }
                a = a0, da = a1;                            /* the second's weight: start, end */
                na++;
            }
            if (na == 2)
                xfade(y, sc.y[1] + 2, a * 65535, (da * 65535 - a * 65535) >> 7);
        }

        /* 3: 2x down: polyphase allpass pairs, a1 = (yb - d1) C0 + d0, a2 = (a1 - d2) C2 + d1 (path 0, the second
         * sample of each pair), b1 = (ya - d4) C1 + d3, b2 = (b1 - d5) C3 + d4 (path 1, the first); out (a2 + b2) / 2.
         * d0 / d3 are the previous yb / ya, read back from y (y[-1], y[-2]); d1, d2, d4, d5 in R4-R7. */
        {
            const int32_t *yp = y;
            int32_t *tp = tmp;
            int32_t *dn = h->dn;
            y[-2] = dn[3], y[-1] = dn[0];
            __asm__ volatile(
                "R4 = [%[dn] + 4];\n\tR5 = [%[dn] + 8];\n\tR6 = [%[dn] + 16];\n\tR7 = [%[dn] + 20];\n\t"
                "R2.L = %[c0];\n\tR2.H = %[c2];\n\tR3.L = %[c1];\n\tR3.H = %[c3];\n\t"
                "R0 = %[yp];\n\tR0 += 4;\n\tI0 = R0;\n\t"   /* I0 walks yb, d0, ya, d3: +4 -8 +4 -8 +20 */
                "R0 = -8 (X);\n\tM0 = R0;\n\tR0 = 20 (X);\n\tM1 = R0;\n\t"
                "R1 = [I0 ++ M0];\n\t"              /* the first yb */
                "R0 = %[n];\n\tLC0 = R0;\n\t"
                "LSETUP (1f, 2f) LC0;\n"
                "1:\tR1 = R1 - R4;\n\t"
                "A1 = R2.L * R1.L (M);\n\tA0 = R1.H * R2.L;\n\t"
                "A1 = A1 >>> 15 || R0 = [I0 ++] || NOP;\n\t"   /* d0 */
                "R1 = (A0 += A1);\n\t"
                "R1 = R1 + R0;\n\t"                /* a1 */
                "R0 = R1 - R5;\n\t"
                "A1 = R2.H * R0.L (M);\n\tA0 = R0.H * R2.H;\n\tA1 = A1 >>> 15;\n\tR0 = (A0 += A1);\n\t"
                "R0 = R0 + R4;\n\t"                /* a2 */
                "R4 = R1;\n\t"
                "R5 = R0;\n\t"
                "R1 = [I0 ++ M0];\n\t"             /* ya */
                "R1 = R1 - R6;\n\t"
                "A1 = R3.L * R1.L (M);\n\tA0 = R1.H * R3.L;\n\t"
                "A1 = A1 >>> 15 || R0 = [I0 ++ M1] || NOP;\n\t"   /* d3 */
                "R1 = (A0 += A1);\n\t"
                "R1 = R1 + R0;\n\t"                /* b1 */
                "R0 = R1 - R7;\n\t"
                "A1 = R3.H * R0.L (M);\n\tA0 = R0.H * R3.H;\n\tA1 = A1 >>> 15;\n\tR0 = (A0 += A1);\n\t"
                "R0 = R0 + R6;\n\t"                /* b2 */
                "R6 = R1;\n\t"
                "R7 = R0;\n\t"
                "R0 = R0 + R5 (NS) || R1 = [I0 ++ M0] || NOP;\n\t"   /* the next yb */
                "R0 >>>= 1;\n"
                "2:\t[%[tp]++] = R0;\n\t"
                "[%[dn] + 4] = R4;\n\t[%[dn] + 8] = R5;\n\t[%[dn] + 16] = R6;\n\t[%[dn] + 20] = R7;\n"
                : [tp] "+a"(tp)
                : [yp] "a"(yp), [dn] "a"(dn), [n] "i"(BLOCK), [c0] "i"(C0), [c1] "i"(C1), [c2] "i"(C2), [c3] "i"(C3)
                : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "A0", "A1", "I0", "M0", "M1", "cc", "LC0", "LT0",
                  "LB0", "memory");
            dn[3] = y[2 * BLOCK - 2], dn[0] = y[2 * BLOCK - 1];
        }

        /* 4: the shapes' DC offset (glides in I3); three bands: low (one-pole at ~250 Hz of the 4 kHz one), mid (up to
         * 4 kHz), high, with gains low = loudness (I0), mid = loudness x the MUFF scoop (a data register, += its
         * step), high = loudness x fizz cut (I1); DC blocker, mix (I2, Q15 in the high half): wet = (z + 256) >> 9
         * saturated via a saturating shift and a rounded high half; out = in + (wet - in) mix in saturating 16-bit
         * ops (mix 0 is exact). Equal gains give back the input (low + mid + high = x). */
        if (mw) {
            int32_t post = h->post, lo = h->lo, dc = h->dc, qm = s->v[G_QM] + st[G_QM], dqm = st[G_QM];
            const int32_t *tp = tmp;
            int16_t *xp = x;
            const int32_t gp[8] = {s->v[G_QL], st[G_QL], s->v[G_QH], st[G_QH], s->v[G_MIX] << 16, st[G_MIX] << 16,
                                   s->v[G_OFF], st[G_OFF]};
            __asm__ volatile(
                "R0 = [%[gp]];\n\tI0 = R0;\n\tR0 = [%[gp] + 4];\n\tM0 = R0;\n\t"
                "R0 = [%[gp] + 8];\n\tI1 = R0;\n\tR0 = [%[gp] + 12];\n\tM1 = R0;\n\t"
                "R0 = [%[gp] + 16];\n\tI2 = R0;\n\tR0 = [%[gp] + 20];\n\tM2 = R0;\n\t"
                "R0 = [%[gp] + 24];\n\tI3 = R0;\n\tR0 = [%[gp] + 28];\n\tM3 = R0;\n\t"
                "I0 += M0;\n\tI1 += M1;\n\tI2 += M2;\n\tI3 += M3;\n\t"     /* the first frame's values */
                "R0 = %[n];\n\tLC0 = R0;\n\t"
                "LSETUP (1f, 2f) LC0;\n"
                "1:\tR0 = [%[tp]++];\n\t"
                "R1 = I3;\n\t"
                "R0 = R0 - R1;\n\t"
                "R1 = R0 - %[post];\n\t"
                "R2 = R1 >>> 1;\n\t"
                "R1 >>>= 4;\n\t"
                "R2 = R2 - R1;\n\t"
                "%[post] = %[post] + R2;\n\t"   /* 4 kHz low-pass */
                "R1 = %[post] - %[lo];\n\t"
                "R2 = R1 >>> 5;\n\t"
                "R1 >>>= 8;\n\t"
                "R2 = R2 + R1;\n\t"
                "%[lo] = %[lo] + R2;\n\t"       /* 250 Hz low-pass (step 0.0352) */
                "R0 = R0 - %[post];\n\t"        /* high */
                "R1 = %[post] - %[lo];\n\t"     /* mid */
                "R2 = I0;\n\t"
                "A1 = R2.H * %[lo].L (M) || I0 += M0 || NOP;\n\t"
                "A0 = %[lo].H * R2.H || I3 += M3 || NOP;\n\t"
                "A1 += %[qm].H * R1.L (M);\n\t"
                "A0 += R1.H * %[qm].H;\n\t"
                "R2 = I1;\n\t"
                "A1 += R2.H * R0.L (M) || I1 += M1 || NOP;\n\t"
                "A0 += R0.H * R2.H;\n\t"
                "A1 = A1 >>> 15;\n\t"
                "R0 = (A0 += A1);\n\t"
                "R0 <<= 2;\n\t"                   /* Q24 */
                "%[qm] = %[qm] + %[dqm];\n\t"
                "R1 = R0 - %[dc];\n\t"
                "R1 >>>= 10;\n\t"
                "%[dc] = %[dc] + R1;\n\t"
                "R0 = R0 - %[dc];\n\t"
                "R0 = R0 << 7 (S);\n\t"
                "R0.L = R0 (RND);\n\t"            /* wet */
                "R2 = W [%[xp]] (X);\n\t"
                "R1.L = R0.L - R2.L (S);\n\t"
                "R0 = I2;\n\t"
                "R1.L = R1.L * R0.H || I2 += M2 || NOP;\n\t"
                "R1.L = R1.L + R2.L (S);\n"
                "2:\tW [%[xp]++] = R1;\n"
                : [tp] "+a"(tp), [xp] "+a"(xp), [post] "+d"(post), [lo] "+d"(lo), [dc] "+d"(dc), [qm] "+d"(qm)
                : [gp] "a"(gp), [n] "i"(BLOCK), [dqm] "d"(dqm)
                : "R0", "R1", "R2", "A0", "A1", "I0", "I1", "I2", "I3", "M0", "M1", "M2", "M3", "cc", "LC0",
                  "LT0", "LB0", "memory");
            h->lo = lo;
            h->post = post, h->dc = dc;
        } else {                                    /* no MUFF: two bands */
            int32_t post = h->post, dc = h->dc;
            const int32_t *tp = tmp;
            int16_t *xp = x;
            const int32_t gp[8] = {s->v[G_QL], st[G_QL], s->v[G_QH], st[G_QH], s->v[G_MIX] << 16, st[G_MIX] << 16,
                                   s->v[G_OFF], st[G_OFF]};
            __asm__ volatile(
                "R0 = [%[gp]];\n\tI0 = R0;\n\tR0 = [%[gp] + 4];\n\tM0 = R0;\n\t"
                "R0 = [%[gp] + 8];\n\tI1 = R0;\n\tR0 = [%[gp] + 12];\n\tM1 = R0;\n\t"
                "R0 = [%[gp] + 16];\n\tI2 = R0;\n\tR0 = [%[gp] + 20];\n\tM2 = R0;\n\t"
                "R0 = [%[gp] + 24];\n\tI3 = R0;\n\tR0 = [%[gp] + 28];\n\tM3 = R0;\n\t"
                "I0 += M0;\n\tI1 += M1;\n\tI2 += M2;\n\tI3 += M3;\n\t"     /* the first frame's values */
                "R0 = %[n];\n\tLC0 = R0;\n\t"
                "LSETUP (1f, 2f) LC0;\n"
                "1:\tR0 = [%[tp]++];\n\t"
                "R1 = I3;\n\t"
                "R0 = R0 - R1;\n\t"
                "R1 = R0 - %[post];\n\t"
                "R2 = R1 >>> 1;\n\t"
                "R1 >>>= 4;\n\t"
                "R2 = R2 - R1;\n\t"
                "%[post] = %[post] + R2;\n\t"
                "R0 = R0 - %[post];\n\t"
                "R1 = I0;\n\t"
                "R2 = I1;\n\t"
                "R3 = I2;\n\t"
                "A1 = R1.H * %[post].L (M) || I0 += M0 || NOP;\n\t"
                "A0 = %[post].H * R1.H || I1 += M1 || NOP;\n\t"
                "A1 += R2.H * R0.L (M) || I2 += M2 || NOP;\n\t"
                "A0 += R0.H * R2.H || I3 += M3 || NOP;\n\t"
                "A1 = A1 >>> 15;\n\t"
                "R0 = (A0 += A1);\n\t"
                "R0 <<= 2;\n\t"                   /* Q24 */
                "R1 = R0 - %[dc];\n\t"
                "R1 >>>= 10;\n\t"
                "%[dc] = %[dc] + R1;\n\t"
                "R0 = R0 - %[dc];\n\t"
                "R0 = R0 << 7 (S);\n\t"
                "R0.L = R0 (RND);\n\t"            /* wet */
                "R2 = W [%[xp]] (X);\n\t"
                "R1.L = R0.L - R2.L (S);\n\t"
                "R1.L = R1.L * R3.H;\n\t"
                "R1.L = R1.L + R2.L (S);\n"
                "2:\tW [%[xp]++] = R1;\n"
                : [tp] "+a"(tp), [xp] "+a"(xp), [post] "+d"(post), [dc] "+d"(dc)
                : [gp] "a"(gp), [n] "i"(BLOCK)
                : "R0", "R1", "R2", "R3", "A0", "A1", "I0", "I1", "I2", "I3", "M0", "M1", "M2", "M3", "cc", "LC0",
                  "LT0", "LB0", "memory");
            h->post = post, h->dc = dc;
        }
    }
    for (int i = 0; i < NG; i++)
        s->v[i] = t[i];                             /* exact at the block end (the steps truncate) */
}

void distortion_mono(void *self, int16_t *buf, const int16_t *p)
{
    (void)self;
    int16_t *io[1] = {buf};
    dist(&mono_st, io, 1, p);
}

void distortion_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p)
{
    (void)self;
    int16_t *io[2] = {l, r};
    dist(&st_st, io, 2, p);
}
#endif
