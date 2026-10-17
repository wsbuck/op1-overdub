/* fmdrum: an FM drum kit on the drum side (notes/drum-engine.md "fmdrum"; tables and kits: gen_fmdrum.py).
 *
 * Runs like the drum probe: drum engine id N on TE's dbox voice objects (12 voices) with our copy of dbox's vtable
 * (slot_gen.inc): render (+0x14), note-on (+0x1C), active (+0x24), note-off (+0x2C), each through a C_ENTRY wrapper.
 * `this` + 0x14 is the voice index TE's factory stored; our state is fmdrum_voice[index] (struct fd_voice, fmdrum.h).
 * TE's per-key loader and saver treat our type like "dbox" (slot.S DRUM_HOOKS), so each key's 8 values load and save
 * with the sound (dbox_data).
 *
 * Every key is a 2-operator phase-modulation drum with a pitch sweep, plus a filtered noise layer:
 *   carrier  sin(phc + I(t) sin(phm)),  phm advancing ratio x phc's rate; I(t) = FM x modulator envelope (own decay)
 *   pitch    K0 + SWEEP x sweep envelope (exponential, time constant SNAP), in semitones: carrier and modulator move
 *            together, so the ratio holds through the sweep
 *   noise    LCG white noise through a Chamberlin state-variable filter, its lp/bp/hp outputs blended; NOISE picks the
 *            colour, level, tone level and clap bursts along one knob (OFF, AIR, SNARE, HAT, CLAP), then METAL (the
 *            808's six squares from PITCH / 2 replace the noise, band-passed and ring-modulated with the voice) and
 *            GRIND (unfiltered noise, then a jittery square from 30 Hz to PITCH x sqrt 2, ring-modulated)
 *   amp      one exponential decay (DECAY) for tone and noise; velocity (MIDI; the OP-1's keys send 127) scales the
 *            level and the index (0.5 .. 1)
 * Knobs (per key, TE's dbox path, range row 9; fmdrum.h FD_*), read every block, so turning one changes a sounding note
 * smoothly; an amount on each plain knob, its time (or partner) on SHIFT:
 *   [0] blue   PITCH  14 Hz .. 3.6 kHz (512 per semitone, 0 = A3)   [4] SHIFT+blue    RATIO 15 harmonic, 17 inharmonic
 *   [1] green  SWEEP  0 .. +48 semitones                            [5] SHIFT+green   SWDEC sweep time 1 .. 300 ms
 *   [2] white  FM     index 0 .. 16 rad (detents 0..19), FEEDBACK 0..7 (20..27), SQUARE 1..5 (28..32)
 *                                                                   [6] SHIFT+white   FMDEC index decay 3 ms .. 3 s
 *   [3] orange DECAY  15 ms .. 4 s (to -60 dB)                      [7] SHIFT+orange  NOISE OFF/AIR/SNARE/HAT/CLAP/
 *                                                                                     METAL/GRIND
 * FEEDBACK: the modulator reads its own last two outputs (mean, as the DX7) x beta, following the index's envelope;
 * SQUARE: the carrier is driven into a clip (1 .. 32 x, level-matched) while the feedback falls back. Both only in the
 * harsh path (run_hv), so clean keys cost what they did. run_hv runs in passes over the run (FM core, METAL's squares,
 * the source, the filter, the mix), each a Blackfin assembly loop with its state in registers (dual MACs, parallel
 * loads); the C loops under #ifndef __bfin__ are their reference, bit for bit (fmdrum_check builds both).
 * Hi-hat choke: a note-on on keys 10..13 whose NOISE is in the HAT or METAL zone releases the other sounding voices of
 * the group in those zones, in ~5 ms (open hat cut by the closed one).
 * Per block (128 samples at 88.2 kHz): the three envelopes step once (per-block factors from the tables), and every
 * gain, the index and both phase increments ramp linearly across the block, so knob changes and envelopes never
 * zipper. A note starts with a 32-sample (0.36 ms) attack ramp to its start values (index and sweep at full: the FM
 * click and the pitch drop); from silence the phases start at 0, a sounding voice (retrigger, steal) keeps its phases
 * and ramps from its current gains, so restarting it doesn't click.
 * Aliasing: the carrier is held below 0.45 fs; the modulator fades out between 0.4375 and 0.5 fs (its own sine would
 * alias); and the index is limited so that fc + (1.5 I + 4) fm stays below 0.77 fs: past that the sidebands J_k(I) are
 * below -50 dB, and only components above 0.77 fs fold back under 20 kHz (the rest lands where TE's 2:1 decimation
 * removes it, as fm4). DC (a 1:1 ratio puts a sideband at
 * 0 Hz) goes through an 8 Hz one-pole high-pass; the output passes dsp.h's soft knee.
 * Fixed point, no division (reciprocal constants, multiply-shift), no libgcc, no .bss. The asm loops need n >= 1 (a hardware
 * loop count of 0 is not a loop of none); runs are 32, 96 or 128 samples.
 */
#include <stdint.h>
#include "dsp.h"
#include "sine_table.h"
#include "fmdrum.h"
#include "fmdrum_tables.h"

#define RAM __attribute__((section(".data")))
#define BLOCK 128
#define ENV1 (1 << 30)                      /* envelopes, Q30 */
#define SILENT 339547                       /* -70 dB of ENV1: the note ends */
#define TAIL_END (4 << 8)                   /* .. and the voice once the DC blocker's value is under 4 LSB (Q23) */
#define RAD 20861                           /* phase units per radian per Q15 sample: 2^32 / 2 pi / 32768 */
#define LIMIT 807404                        /* 0.77 fs, Q20 of fs: folds back to 20 kHz */
#define M_FADE 458752                       /* 0.4375 fs: the modulator fades out from here .. */
#define M_MUTE 524288                       /* .. to 0.5 fs */
#define INC_BASE 669567                     /* 13.75 Hz (A3 - 4 octaves) at 88.2 kHz, 2^32 = one cycle */
#define Q_OFF 24576                         /* 4 octaves in K0 units */
#define O13_MAX 94168                       /* 0.45 fs above 13.75 Hz, octaves Q13 */
#define DC_R 2146259790                     /* 1 - 2 pi 8 / 88200, Q31 */
#define ATTACK 32                           /* samples */
#define CHOKE_DEC 144594432                 /* per block, Q30: T60 5 ms (a choked hat) */

struct tobj {                               /* TE's dbox object, 0x48 B: only the vptr and the index are used */
    const void *vptr;
    uint32_t w[4];
    int32_t voice;                          /* 0x14: TE's voice index (FUN_01970da8) */
};

struct fd_voice fmdrum_voice[FD_VOICES] RAM;   /* zeroed in .data (the slot block must not use .bss) */
struct fd_diag fmdrum_diag RAM = { 0, 0, 0, 0, 0, -1 };

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
#define mul16(d, f) __builtin_bfin_mult_fr1x16((short)(d), (short)(f))
#else
#define mul16(d, f) (((int32_t)(short)(d) * (f)) >> 15)
#endif

/* qmul as the device computes it (__builtin_bfin_mult_fr1x32x32: the low halves' product only to 16 bits), so the C
 * build (the asm loops' reference, fmdrum_check --c-ref) renders bit for bit what the device does */
#ifdef __bfin__
#define fd_qmul qmul
#else
static inline int32_t fd_qmul(int32_t a, int32_t b)
{
    int64_t lo = (int64_t)(((uint32_t)a & 0xFFFF) * ((uint32_t)b & 0xFFFF) >> 16);
    lo = (lo + (int64_t)(a >> 16) * (b & 0xFFFF) + (int64_t)(b >> 16) * (a & 0xFFFF)) >> 15;
    int64_t hi = (a >> 16) == -32768 && (b >> 16) == -32768 ? 0x7FFFFFFF : (int64_t)(a >> 16) * (b >> 16) * 2;
    int64_t r = hi + lo;
    return r > 0x7FFFFFFF ? 0x7FFFFFFF : r < -0x7FFFFFFF - 1 ? (int32_t)0x80000000 : (int32_t)r;
}
#endif

/* sin(2 pi ph / 2^32), Q15: sine256 interpolated (~ -82 dB) */
static inline int32_t fsin(uint32_t ph)
{
    uint32_t i = ph >> 24;
    int32_t f = (int32_t)((ph >> 9) & 0x7FFF);
    return sine256[i] + mul16(sine256[i + 1] - sine256[i], f);
}

/* one block of an exponential envelope: e x factor, both Q30 (a full 32 x 32 product: at -60 dB a 16-bit one would
 * lose a few % per block) */
static inline int32_t env_step(int32_t e, int32_t factor)
{
    return fd_qmul(e, factor << 1);
}

/* carrier increment for pitch q (K0 units, 512 per semitone, 0 = 220 Hz), held below 0.45 fs */
static uint32_t inc_of(int32_t q)
{
    int32_t u = q + Q_OFF;
    uint32_t o13 = u < 0 ? 0 : ((uint32_t)u * 21845u) >> 14;       /* octaves Q13: u x 4/3 / 2^13 x 2^13 */
    if (o13 > O13_MAX)
        o13 = O13_MAX;
    uint32_t oct = o13 >> 13, fr = o13 & 8191, i = fr >> 7, f7 = fr & 127;
    uint32_t m = fd_exp2[i] + (((fd_exp2[i + 1] - fd_exp2[i]) * f7) >> 7);   /* Q15, 1.0 .. 2.0 */
    return (((INC_BASE >> 4) * m) >> 11) << oct;
}

/* inc x ratio (Q16), mod 2^32: an increment past 2^32 is a frequency the modulator mute has already silenced */
static inline uint32_t mul_ratio(uint32_t inc, uint32_t r)
{
    return (inc >> 16) * r + (((inc & 0xFFFF) * (r >> 6)) >> 10);
}

/* index (rad Q8) after the modulator's fade near fs/2 and the bandwidth limit fc + (1.5 I + 4) fm <= 0.77 fs: the
 * sidebands past it (J_k(I) for k > 1.5 I + 4: below -50 dB) are the only ones that fold back under 20 kHz */
static int32_t limit_index(int32_t i_q8, int32_t mute, uint32_t fc, uint32_t fm)
{
    i_q8 = (i_q8 * mute) >> 15;             /* fm >= 0.5 fs: 0, so fm4 below stays < 2^15 whenever i_q8 > 0 */
    if (!i_q8)
        return 0;
    int32_t allowed = LIMIT - (int32_t)fc - 4 * (int32_t)fm;
    if (allowed <= 0)
        return 0;
    uint32_t fm4 = fm >> 4;
    if ((((uint32_t)(3 * i_q8) * fm4) >> 5) <= (uint32_t)allowed)
        return i_q8;
    int32_t lo = 0, hi = i_q8;              /* largest index that fits */
    while (lo < hi) {
        int32_t mid = (lo + hi + 1) >> 1;
        if ((((uint32_t)(3 * mid) * fm4) >> 5) <= (uint32_t)allowed)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

struct seg {                                /* what one run of samples ramps */
    int32_t dgt, dgn, dgm, dgf, dgc, dgr;
    uint32_t dci, dmi;
};

struct flt {                                /* the block's noise path: filter, and (harsh variants) the source weights */
    int32_t f, q, wl, wb, wh;
    int32_t wx, wm, wsq;                    /* white noise, METAL's squares, GRIND's square, Q15 */
    int32_t unit;                           /* one METAL square's amplitude (wm x 3861) */
    int32_t go;                             /* the filtered source's gain, Q8 (METAL's level, fd_metal_gain) */
    int32_t mh;                             /* the squares' one-pole high-pass coefficient, Q15 */
    uint32_t mi[6], gi;                     /* METAL's and GRIND's square increments */
};

/* one run of n samples; NOISE and FM are constants at each call (run() below picks the variant), so a key without
 * noise or without FM skips that part of the loop. The clean path: no feedback, a sine carrier, white noise only. */
static inline __attribute__((always_inline)) void run_v(const int NOISE, const int FM, struct fd_voice *v, int16_t *out,
                                                        int n, const struct seg *s, const struct flt *nf)
{
    uint32_t cph = v->cph, mph = v->mph, ci = v->cinc, mi = v->minc, seed = v->seed;
    int32_t gt = v->gt, gn = v->gn, gm = v->gm, lp = v->lp, bp = v->bp, dcx = v->dcx, dcy = v->dcy;
    const int32_t f = nf->f, q = nf->q, wl = nf->wl, wb = nf->wb, wh = nf->wh;
    for (int i = 0; i < n; i++) {
        int32_t cs = fsin(FM ? cph + (uint32_t)fsin(mph) * (uint32_t)gm : cph);
        int32_t y = cs * (gt >> 8);
        if (NOISE) {
            seed = seed * 1664525u + 1013904223u;
            int32_t x = (int32_t)seed >> 17;                        /* white, +-16384 */
            lp += (f * bp) >> 15;
            int32_t hp = x - lp - ((q * bp) >> 15);
            bp += (f * hp) >> 15;
            y += ((wl * lp + wb * bp + wh * hp) >> 12) * (gn >> 8);
        }
        y >>= 7;                                                    /* Q23 */
        dcy = y - dcx + fd_qmul(DC_R, dcy);
        dcx = y;
        out[i] = soft_knee(dcy >> 8);
        cph += ci;
        mph += mi;
        ci += s->dci;
        mi += s->dmi;
        gt += s->dgt;
        gn += s->dgn;
        gm += s->dgm;
    }
    v->cph = cph;
    v->mph = mph;
    v->cinc = ci;
    v->minc = mi;
    v->seed = seed;
    v->gt = gt;
    v->gn = gn;
    v->gm = gm;
    v->lp = lp;
    v->bp = bp;
    v->dcx = dcx;
    v->dcy = dcy;
}

/* the asm loops' 16-bit saturation, for the C reference */
static inline int32_t sat16(int32_t x)
{
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

/* The harsh path (any of FEEDBACK, SQUARE, METAL, GRIND): the modulator fed back on itself (beta x the mean of its last
 * two outputs, as the DX7), the carrier driven into a clip (gc), the filter's source white noise plus METAL's / GRIND's
 * squares, and the filtered source ring-modulated with the voice (gr). In passes over the run, each small enough to keep
 * its state in registers: the FM core (the clipped carrier into out[]), the source (into fd_x[]), the filter (fd_x[] in
 * place) and the mix (out[] in place). */
static int16_t fd_src[BLOCK] RAM;           /* the squares; render calls come one at a time (TE's audio thread) */
static int32_t fd_x[BLOCK] RAM;             /* the filter's source, then its output */

/* METAL's six squares for a run, scaled by their weight, into fd_src[] (Q0; zeros without METAL). Each square is
 * +-3861 (six: RMS ~ white noise's). METAL: x = u (6 + 2 neg), neg = -(the squares below
 * zero), high-passed (x minus its one-pole low-pass lpm), clamped to Q15. */
static void squares(struct fd_voice *v, int n, const struct flt *nf)
{
    int16_t *d = fd_src;
    if (nf->wm) {
#ifdef __bfin__
        /* four phases in R4..R7 (their signs as two vector pairs), two in I0/I1 (+= M0/M1); x' = 2u neg and lpm' =
         * lpm - 6u (x - lpm and x - lpm's update are the same); x' - lpm' saturated to 16 bits */
        struct {
            uint32_t inc[4];                /*  0  4  8 12 */
            int32_t lpm;                    /* 16: lpm - 6u */
            uint32_t umh;                   /* 20: 2u | mh << 16 */
            uint32_t ph[6];                 /* 24 .. 44 */
            int16_t *d;                     /* 48 */
            int32_t n;                      /* 52 */
            uint32_t inc45[2];              /* 56 60 */
        } c;
        for (int j = 0; j < 4; j++)
            c.inc[j] = nf->mi[j];
        c.inc45[0] = nf->mi[4];
        c.inc45[1] = nf->mi[5];
        for (int j = 0; j < 6; j++)
            c.ph[j] = v->mt[j];
        c.lpm = v->mlp - 6 * nf->unit;
        c.umh = (uint32_t)(2 * nf->unit) | (uint32_t)nf->mh << 16;
        c.d = d;
        c.n = n;
        register void *cp __asm__("P5") = &c;
        __asm__ volatile(
            "R4 = [%[c] + 24];\n\t"
            "R5 = [%[c] + 28];\n\t"
            "R6 = [%[c] + 32];\n\t"
            "R7 = [%[c] + 36];\n\t"
            "R0 = [%[c] + 40];\n\tI0 = R0;\n\t"
            "R0 = [%[c] + 44];\n\tI1 = R0;\n\t"
            "R0 = [%[c] + 56];\n\tM0 = R0;\n\t"
            "R0 = [%[c] + 60];\n\tM1 = R0;\n\t"
            "R0 = [%[c] + 48];\n\tI2 = R0;\n\t"
            "R3 = [%[c] + 20];\n\t"
            "P2 = [%[c] + 52];\n\t"
            "LSETUP (1f, 2f) LC0 = P2;\n"
            "1:\tR0 = [%[c] + 0];\n\t"
            "R4 = R4 + R0 (NS) || R1 = [%[c] + 4];\n\t"
            "R5 = R5 + R1 (NS) || R0 = [%[c] + 8];\n\t"
            "R6 = R6 + R0 (NS) || R1 = [%[c] + 12];\n\t"
            "R7 = R7 + R1 (NS) || I0 += M0;\n\t"
            "R0 = PACK (R4.H, R5.H) || I1 += M1;\n\t"
            "R1 = PACK (R6.H, R7.H);\n\t"
            "R0 = R0 >>> 15 (V);\n\t"
            "R1 = R1 >>> 15 (V);\n\t"
            "R0 = R0 +|+ R1;\n\t"
            "R1 = I0;\n\t"
            "R2 = I1;\n\t"
            "R1 = PACK (R1.H, R2.H);\n\t"
            "R1 = R1 >>> 15 (V);\n\t"
            "R0 = R0 +|+ R1 || R2 = [%[c] + 16];\n\t"             /* neg in two halves; lpm' */
            "A0 = R3.L * R0.L (IS);\n\t"
            "R0 = (A0 += R3.L * R0.H) (IS);\n\t"                    /* x' = 2u neg */
            "R1 = R0 - R2;\n\t"
            "A1 = R3.H * R1.L (M), A0 = R3.H * R1.H;\n\t"
            "A1 = A1 >>> 15;\n\t"
            "R1 = (A0 += A1);\n\t"
            "R2 = R2 + R1;\n\t"                                     /* lpm' += ((x' - lpm') mh) >> 15 */
            "R1 = R0 - R2 (NS) || [%[c] + 16] = R2;\n\t"
            "R1 = R1 << 16 (S);\n"
            "2:\tW [I2++] = R1.H;\n\t"
            "[%[c] + 24] = R4;\n\t"
            "[%[c] + 28] = R5;\n\t"
            "[%[c] + 32] = R6;\n\t"
            "[%[c] + 36] = R7;\n\t"
            "R0 = I0;\n\t[%[c] + 40] = R0;\n\t"
            "R0 = I1;\n\t[%[c] + 44] = R0;\n\t"
            :
            : [c] "a"(cp)
            : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P2", "I0", "I1", "I2", "M0", "M1", "A0", "A1", "LC0",
              "LT0", "LB0", "cc", "memory");
        for (int j = 0; j < 6; j++)
            v->mt[j] = c.ph[j];
        v->mlp = c.lpm + 6 * nf->unit;
#else
        uint32_t a = v->mt[0], b = v->mt[1], c = v->mt[2];
        const uint32_t ia = nf->mi[0], ib = nf->mi[1], ic = nf->mi[2];
        for (int i = 0; i < n; i++) {       /* count the negative halves: 0 .. -3 */
            a += ia;
            b += ib;
            c += ic;
            d[i] = (int16_t)(((int32_t)a >> 31) + ((int32_t)b >> 31) + ((int32_t)c >> 31));
        }
        v->mt[0] = a;
        v->mt[1] = b;
        v->mt[2] = c;
        a = v->mt[3];
        b = v->mt[4];
        c = v->mt[5];
        const uint32_t ja = nf->mi[3], jb = nf->mi[4], jc = nf->mi[5];
        const int32_t u = nf->unit, mh = nf->mh;
        int32_t lpm = v->mlp;
        for (int i = 0; i < n; i++) {
            a += ja;
            b += jb;
            c += jc;
            int32_t neg = d[i] + ((int32_t)a >> 31) + ((int32_t)b >> 31) + ((int32_t)c >> 31);
            int32_t x = u * (6 + 2 * neg);                        /* +-23166 at most */
            lpm += ((x - lpm) * mh) >> 15;                         /* high-pass: x minus its low-pass */
            d[i] = (int16_t)sat16(x - lpm);
        }
        v->mlp = lpm;
        v->mt[3] = a;
        v->mt[4] = b;
        v->mt[5] = c;
#endif
    } else {
        for (int i = 0; i < n; i++)
            d[i] = 0;
    }
}

/* 1. The FM core: the clipped carrier cc (Q15) into out[] */
#ifdef __bfin__
struct hv_core_ctx {                        /* hv_core's asm loop reads and writes these (offsets fixed below) */
    uint32_t cph, ci, dci, mph, mi, dmi;    /*  0  4  8 12 16 20 */
    int32_t gm, dgm, gf, dgf, m1, s;        /* 24 28 32 36 40 44: s = m1 + m2 */
    uint32_t gc, dgc;                       /* 48 52: gc << 16 | the EXTRACT control (fsin's 15 fraction bits), dgc << 16 */
    const uint32_t *tab;                    /* 56 */
    int16_t *out;                           /* 60 */
    int32_t n;                              /* 64 */
};

/* fsin(R0) -> R2.L, as fsin() above: index ph >> 24, fraction (ph >> 9) & 0x7FFF (EXTRACT with R3.L), one table word
 * per step (fd_sin2: the step's sine and its difference to the next). FSIN_A: index to P0, fraction to R0, with FILL
 * between the move to P0 and the address (independent work: a register moved to P0 is not usable as an address for a
 * few cycles on the device); FSIN_B: the lookup. R0, R1, P0 used. */
#define FSIN_A(FILL)                                                                                                 \
    "R1 = R0 >> 24;\n\t"                                                                                              \
    "P0 = R1;\n\t"                                                                                                   \
    FILL                                                                                                             \
    "P0 = P4 + (P0 << 2);\n\t"
#define FSIN_B                                                                                                       \
    "R1 = [P0];\n\t"                                                                                                 \
    "R2.L = R1.H * R0.L (T);\n\t"                                                                                    \
    "R2.L = R2.L + R1.L (NS);\n\t"
#endif

static void hv_core(struct fd_voice *v, int16_t *out, int n, const struct seg *s)
{
#ifdef __bfin__
    struct hv_core_ctx c;
    c.cph = v->cph;
    c.ci = v->cinc;
    c.dci = s->dci;
    c.mph = v->mph;
    c.mi = v->minc;
    c.dmi = s->dmi;
    c.gm = v->gm;
    c.dgm = s->dgm;
    c.gf = v->gf;
    c.dgf = s->dgf;
    c.m1 = v->m1;
    c.s = v->m1 + v->m2;
    c.gc = ((uint32_t)v->gc << 16) | 0x090F;
    c.dgc = (uint32_t)s->dgc << 16;
    c.tab = fd_sin2;
    c.out = out;
    c.n = n;
    register struct hv_core_ctx *cp __asm__("P5") = &c;
    __asm__ volatile(
        "R0 = [%[c] + 0];\n\tI0 = R0;\n\t"
        "R0 = [%[c] + 4];\n\tI1 = R0;\n\t"
        "R0 = [%[c] + 8];\n\tM1 = R0;\n\t"
        "R0 = [%[c] + 12];\n\tI2 = R0;\n\t"
        "R0 = [%[c] + 16];\n\tI3 = R0;\n\t"
        "R0 = [%[c] + 20];\n\tM3 = R0;\n\t"
        "R4 = [%[c] + 24];\n\t"
        "R5 = [%[c] + 32];\n\t"
        "R6 = [%[c] + 40];\n\t"
        "R7 = [%[c] + 44];\n\t"
        "R3 = [%[c] + 48];\n\t"
        "P4 = [%[c] + 56];\n\t"
        "P1 = [%[c] + 60];\n\t"
        "P2 = [%[c] + 64];\n\t"
        "LSETUP (1f, 2f) LC0 = P2;\n"
        "1:\tR1 = I2;\n\t"
        "R7 *= R5;\n\t"                                         /* (m1 + m2) gf */
        "R0 = R7 + R1;\n\t"
        FSIN_A("R0 = EXTRACT (R0, R3.L) (Z) || R2 = [%[c] + 36];\n\t"
               "R5 = R5 + R2;\n\t"                                /* gf += dgf */
               "M2 = I3;\n\t"
               "I2 += M2;\n\t"                                    /* mph += mi */
               "I3 += M3;\n\t")                                   /* mi += dmi */
        FSIN_B                                                  /* ms */
        "R2 = R2.L (X);\n\t"
        "R7 = R2 + R6;\n\t"                                     /* m1 + m2 for the next sample */
        "R6 = R2;\n\t"
        "R2 *= R4;\n\t"                                         /* ms gm */
        "R1 = I0;\n\t"
        "R0 = R2 + R1;\n\t"
        FSIN_A("R0 = EXTRACT (R0, R3.L) (Z) || R2 = [%[c] + 28];\n\t"
               "R4 = R4 + R2;\n\t"                                /* gm += dgm */
               "M0 = I1;\n\t"
               "I0 += M0;\n\t"                                    /* cph += ci */
               "I1 += M1;\n\t")                                   /* ci += dci */
        FSIN_B                                                  /* cs */
        "A0 = R2.L * R3.H (IS);\n\t"
        "A0 = A0 >>> 10 || R1 = [%[c] + 52];\n\t"
        "R2.L = A0 (IS);\n"                                      /* cc = (cs gc) >> 10, saturated */
        "2:\tR3 = R3 + R1 (NS) || W [P1++] = R2;\n\t"           /* gc += dgc */
        "R0 = I0;\n\t[%[c] + 0] = R0;\n\t"
        "R0 = I1;\n\t[%[c] + 4] = R0;\n\t"
        "R0 = I2;\n\t[%[c] + 12] = R0;\n\t"
        "R0 = I3;\n\t[%[c] + 16] = R0;\n\t"
        "[%[c] + 24] = R4;\n\t"
        "[%[c] + 32] = R5;\n\t"
        "[%[c] + 40] = R6;\n\t"
        "[%[c] + 44] = R7;\n\t"
        "[%[c] + 48] = R3;\n\t"
        :
        : [c] "a"(cp)
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P0", "P1", "P2", "P4", "I0", "I1", "I2", "I3", "M0", "M1",
          "M2", "M3", "A0", "LC0", "LT0", "LB0", "cc", "memory");
    v->cph = c.cph;
    v->cinc = c.ci;
    v->mph = c.mph;
    v->minc = c.mi;
    v->gm = c.gm;
    v->gf = c.gf;
    v->gc = (int32_t)c.gc >> 16;
    v->m1 = c.m1;
    v->m2 = c.s - c.m1;
#else
    uint32_t cph = v->cph, mph = v->mph, ci = v->cinc, mi = v->minc;
    int32_t gm = v->gm, gf = v->gf, gc = v->gc, m1 = v->m1, m2 = v->m2;
    for (int i = 0; i < n; i++) {
        int32_t ms = fsin(mph + (uint32_t)(m1 + m2) * (uint32_t)gf);
        m2 = m1;
        m1 = ms;
        out[i] = (int16_t)sat16((fsin(cph + (uint32_t)ms * (uint32_t)gm) * gc) >> 10);
        cph += ci;
        mph += mi;
        ci += s->dci;
        mi += s->dmi;
        gm += s->dgm;
        gf += s->dgf;
        gc += s->dgc;
    }
    v->cph = cph;
    v->mph = mph;
    v->cinc = ci;
    v->minc = mi;
    v->gm = gm;
    v->gf = gf;
    v->gc = gc;
    v->m1 = m1;
    v->m2 = m2;
#endif
}

/* 2. The filter's source into fd_x[]: white noise x wx (+-16384), plus (SRC) METAL's squares (fd_src[]) and GRIND's
 * square (+-9459 x wsq: the same RMS as white noise), those two clamped to Q15 together */
#ifdef __bfin__
#define SRC_HEAD                                                                                                     \
    "R7 = [%[c] + 0];\n\tR6 = [%[c] + 4];\n\tR5 = [%[c] + 12];\n\tR4 = [%[c] + 16];\n\tR3 = [%[c] + 20];\n\t"           \
    "P1 = [%[c] + 28];\n\tP2 = [%[c] + 36];\n\tR0 = [%[c] + 32];\n\tI0 = R0;\n\t"                                  \
    "LSETUP (1f, 2f) LC0 = P2;\n"                                                                                   \
    "1:\tR7 *= R6;\n\t"                                                                                             \
    "R0 = [%[c] + 8];\n\t"                                                                                          \
    "R7 = R7 + R0 (NS) || R2 = [%[c] + 24];\n\t"                     /* seed = seed a + c; gi */                    \
    "R0 = R7 >>> 17;\n\t"                                                                                           \
    "R0.L = R0.L * R5.L (T);\n\t"                                    /* (wx (seed >> 17)) >> 15 */                  \
    "R0 = R0.L (X);\n\t"
#define SRC_TAIL                                                                                                     \
    "2:\t[P1++] = R0;\n\t"                                                                                          \
    "[%[c] + 0] = R7;\n\t[%[c] + 20] = R3;\n\t"                                                                     \
    : : [c] "a"(cp) : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P1", "P2", "I0", "LC0", "LT0", "LB0", "cc",  \
      "memory"
#endif

static void hv_src(struct fd_voice *v, int n, const struct flt *nf, int SRC)
{
    const int32_t w = (nf->wsq * 9459) >> 15;
#ifdef __bfin__
    struct {
        uint32_t seed, a, c;                /*  0  4  8: the LCG */
        int32_t wx, w;                      /* 12 16 */
        uint32_t g, gi;                     /* 20 24: GRIND's phase, increment */
        int32_t *x;                         /* 28 */
        const int16_t *d;                   /* 32 */
        int32_t n;                          /* 36 */
    } c;
    c.seed = v->seed;
    c.a = 1664525u;
    c.c = 1013904223u;
    c.wx = nf->wx;
    c.w = w;
    c.g = v->gph;
    c.gi = nf->gi;
    c.x = fd_x;
    c.d = fd_src;
    c.n = n;
    register void *cp __asm__("P5") = &c;
    if (!SRC)
        __asm__ volatile(SRC_HEAD SRC_TAIL);
    else if (!nf->wsq)
        __asm__ volatile(SRC_HEAD
                         "R1.L = W [I0++];\n\t"
                         "R1 = R1.L (X);\n\t"
                         "R0 = R0 + R1;\n"
                         SRC_TAIL);
    else                                    /* + sat16(d +- w): (w ^ s) - s, s = g >> 31 */
        __asm__ volatile(SRC_HEAD
                         "R3 = R3 + R2 (NS) || R1.L = W [I0++];\n\t"
                         "R1 = R1.L (X);\n\t"
                         "R2 = R3 >>> 31;\n\t"
                         "R4 = R4 ^ R2;\n\t"
                         "R1 = R1 + R4;\n\t"
                         "R4 = R4 ^ R2;\n\t"
                         "R1 = R1 - R2;\n\t"
                         "R1 = R1 << 16 (S);\n\t"
                         "R1 >>>= 16;\n\t"
                         "R0 = R0 + R1;\n"
                         SRC_TAIL);
    v->seed = c.seed;
    v->gph = c.g;
#else
    uint32_t seed = v->seed, g = v->gph;
    const int32_t wx = nf->wx;
    for (int i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        int32_t sq = 0;
        if (SRC) {
            g += nf->gi;
            sq = nf->wsq ? sat16(fd_src[i] + ((int32_t)g < 0 ? -w : w)) : fd_src[i];
        }
        fd_x[i] = ((wx * ((int32_t)seed >> 17)) >> 15) + sq;
    }
    v->seed = seed;
    v->gph = g;
#endif
}

/* 3. The filter (Chamberlin SVF, lp/bp/hp blended, x go), clamped to Q15: fd_x[] -> fd_src[] (the squares are used) */
static void hv_filt(struct fd_voice *v, int n, const struct flt *nf)
{
#ifdef __bfin__
    /* (x c) >> 15 for 32-bit x, 16-bit c, exactly: the high half's product (fractional: x 2) plus the low half's
     * (mixed mode: c signed, x.L unsigned) >> 15; q * bp as bp + (q - 32768) bp, so the coefficient fits 16 bits */
    struct {
        uint32_t fq, wlb, whg;              /*  0  4  8: f | (q - 32768) << 16, wl | wb << 16, wh | go << 16 */
        int32_t lp, bp;                     /* 12 16 */
        const int32_t *x;                   /* 20 */
        int16_t *nz;                        /* 24 */
        int32_t n;                          /* 28 */
    } c;
    c.fq = (uint32_t)nf->f | (uint32_t)(nf->q - 32768) << 16;
    c.wlb = (uint32_t)nf->wl | (uint32_t)nf->wb << 16;
    c.whg = (uint32_t)nf->wh | (uint32_t)nf->go << 16;
    c.lp = v->lp;
    c.bp = v->bp;
    c.x = fd_x;
    c.nz = fd_src;
    c.n = n;
    register void *cp __asm__("P5") = &c;
    __asm__ volatile(
        "R5 = [%[c] + 0];\n\t"
        "R4 = [%[c] + 4];\n\t"
        "R3 = [%[c] + 8];\n\t"
        "R7 = [%[c] + 12];\n\t"
        "R6 = [%[c] + 16];\n\t"
        "P0 = [%[c] + 20];\n\t"
        "P1 = [%[c] + 24];\n\t"
        "P2 = [%[c] + 28];\n\t"
        "LSETUP (1f, 2f) LC0 = P2;\n"
        "1:\tA1 = R5.L * R6.L (M), A0 = R5.L * R6.H || R2 = [P0++];\n\t"
        "A1 = A1 >>> 15;\n\t"
        "R0 = (A0 += A1);\n\t"
        "R7 = R7 + R0;\n\t"                                    /* lp += (f bp) >> 15 */
        "A1 = R5.H * R6.L (M), A0 = R5.H * R6.H;\n\t"
        "A1 = A1 >>> 15;\n\t"
        "R0 = (A0 += A1);\n\t"
        "R1 = R2 - R7;\n\t"
        "R1 = R1 - R6;\n\t"
        "R1 = R1 - R0;\n\t"                                    /* hp = x - lp - (q bp) >> 15 */
        "A1 = R5.L * R1.L (M), A0 = R5.L * R1.H;\n\t"
        "A1 = A1 >>> 15;\n\t"
        "R0 = (A0 += A1);\n\t"
        "R6 = R6 + R0;\n\t"                                    /* bp += (f hp) >> 15 */
        "A1 = R4.L * R7.L (M), A0 = R4.L * R7.H (IS);\n\t"
        "A1 += R4.H * R6.L (M), A0 += R4.H * R6.H (IS);\n\t"
        "A1 += R3.L * R1.L (M), A0 += R3.L * R1.H (IS);\n\t"
        "A1 = A1 >>> 12;\n\t"
        "A0 = A0 << 4;\n\t"
        "R0 = (A0 += A1);\n\t"                                 /* (wl lp + wb bp + wh hp) >> 12 */
        "A1 = R3.H * R0.L (M), A0 = R3.H * R0.H (IS);\n\t"
        "A1 = A1 >>> 8;\n\t"
        "A0 = A0 << 8;\n\t"
        "A0 += A1;\n\t"
        "R0.L = A0 (IS);\n"                                     /* x go >> 8, saturated */
        "2:\tW [P1++] = R0;\n\t"
        "[%[c] + 12] = R7;\n\t"
        "[%[c] + 16] = R6;\n\t"
        :
        : [c] "a"(cp)
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P0", "P1", "P2", "A0", "A1", "LC0", "LT0", "LB0", "cc",
          "memory");
    v->lp = c.lp;
    v->bp = c.bp;
#else
    int32_t lp = v->lp, bp = v->bp;
    const int32_t f = nf->f, q = nf->q, wl = nf->wl, wb = nf->wb, wh = nf->wh, go = nf->go;
    for (int i = 0; i < n; i++) {           /* (64-bit products: the accumulators are 40 bits) */
        lp += (int32_t)(((int64_t)f * bp) >> 15);
        int32_t hp = fd_x[i] - lp - (int32_t)(((int64_t)q * bp) >> 15);
        bp += (int32_t)(((int64_t)f * hp) >> 15);
        int64_t pre = ((int64_t)wl * lp + (int64_t)wb * bp + (int64_t)wh * hp) >> 12;
        pre = pre > 0x7FFFFFFF ? 0x7FFFFFFF : pre < -0x7FFFFFFF - 1 ? -0x7FFFFFFF - 1 : pre;
        int64_t y = (pre * go) >> 8;
        fd_src[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
    v->lp = lp;
    v->bp = bp;
#endif
}

/* 4. The mix: tone cc, the filtered source nz and their ring modulation (x2), each term halved before the sum (int32
 * headroom); the DC blocker and the soft knee: out[] in place */
static void hv_mix(struct fd_voice *v, int16_t *out, int n, const struct seg *s)
{
#ifdef __bfin__
    /* the gains as gt << 8, so their high halves are gt >> 8 (Q15) exactly; the DC blocker's qmul as gcc expands
     * __builtin_bfin_mult_fr1x32x32; the knee (dsp.h soft_knee) branches out of the loop only above 0.75 FS */
    struct {
        int32_t dcx, knee, top, one;        /*  0  4  8 12: dcx, KNEE_T, KNEE_T + 2 (32767 - KNEE_T), 32767 */
        int32_t dgt, dgn, dgr;              /* 16 20 24 (<< 8) */
        int32_t gt, gn, gr, dcy, dcr;       /* 28 32 36 40 44 */
        const int16_t *cc, *nz;             /* 48 52 */
        int16_t *out;                       /* 56 */
        int32_t n;                          /* 60 */
    } c;
    c.dcx = v->dcx;
    c.knee = KNEE_T;
    c.top = KNEE_T + 2 * (32767 - KNEE_T);
    c.one = 32767;
    c.dgt = s->dgt << 8;
    c.dgn = s->dgn << 8;
    c.dgr = s->dgr << 8;
    c.gt = v->gt << 8;
    c.gn = v->gn << 8;
    c.gr = v->gr << 8;
    c.dcy = v->dcy;
    c.dcr = DC_R;
    c.cc = out;
    c.nz = fd_src;
    c.out = out;
    c.n = n;
    register void *cp __asm__("P5") = &c;
    __asm__ volatile(
        "R7 = [%[c] + 28];\n\t"
        "R6 = [%[c] + 32];\n\t"
        "R5 = [%[c] + 36];\n\t"
        "R3 = [%[c] + 40];\n\t"
        "R4 = [%[c] + 44];\n\t"
        "R0 = [%[c] + 48];\n\tI0 = R0;\n\t"
        "R0 = [%[c] + 52];\n\tI1 = R0;\n\t"
        "R0 = [%[c] + 56];\n\tI2 = R0;\n\t"
        "P2 = [%[c] + 60];\n\t"
        "LSETUP (1f, 2f) LC0 = P2;\n"
        "1:\tMNOP || R0.L = W [I0++] || R1.L = W [I1++];\n\t"
        "A0 = R0.L * R7.H (IS);\n\t"                            /* cc gt >> 8 */
        "A1 = R1.L * R6.H (IS);\n\t"                            /* nz gn >> 8 */
        "A0 = A0 >>> 1;\n\t"
        "A1 = A1 >>> 1;\n\t"
        "A0 += A1;\n\t"
        "R2.L = R1.L * R0.L (T);\n\t"                           /* (nz cc) >> 15 */
        "A0 += R2.L * R5.H (IS);\n\t"
        "A0 = A0 >>> 6;\n\t"
        "R0 = A0 || R1 = [%[c] + 0];\n\t"                       /* y, Q23; dcx */
        "R1 = R0 - R1 (NS) || [%[c] + 0] = R0;\n\t"             /* dcx = y */
        "A1 = R3.L * R4.L (FU);\n\t"
        "A1 = A1 >> 16;\n\t"
        "A1 += R3.H * R4.L (M), A0 = R3.H * R4.H;\n\t"
        "A1 += R4.H * R3.L (M);\n\t"
        "A1 = A1 >>> 15;\n\t"
        "R2 = (A0 += A1);\n\t"                                  /* qmul(DC_R, dcy) */
        "R3 = R1 + R2;\n\t"                                     /* dcy */
        "R0 = R3 >>> 8;\n\t"
        "R1 = ABS R0 || R2 = [%[c] + 4];\n\t"
        "CC = R1 <= R2;\n\t"
        "IF !CC JUMP 4f;\n"
        "3:\tR1 = [%[c] + 16];\n\t"
        "R7 = R7 + R1 (NS) || W [I2++] = R0.L || R2 = [%[c] + 20];\n\t"
        "R6 = R6 + R2 (NS) || R1 = [%[c] + 24];\n"
        "2:\tR5 = R5 + R1;\n\t"
        "JUMP 5f;\n"
        "4:\tR2 = [%[c] + 8];\n\t"                             /* the knee: a = min(|x|, top), d = a - KNEE_T, */
        "R1 = MIN (R1, R2);\n\t"                                /* min(a - d^2 >> 15, 32767), x's sign */
        "R2 = [%[c] + 4];\n\t"
        "R2 = R1 - R2;\n\t"
        "R2 = R2.L * R2.L (IS);\n\t"
        "R2 >>>= 15;\n\t"
        "R1 = R1 - R2;\n\t"
        "R2 = [%[c] + 12];\n\t"
        "R1 = MIN (R1, R2);\n\t"
        "R2 = -R1;\n\t"
        "CC = R0 < 0;\n\t"
        "IF CC R1 = R2;\n\t"
        "R0 = R1;\n\t"
        "JUMP 3b;\n"
        "5:\t[%[c] + 28] = R7;\n\t"
        "[%[c] + 32] = R6;\n\t"
        "[%[c] + 36] = R5;\n\t"
        "[%[c] + 40] = R3;\n\t"
        :
        : [c] "a"(cp)
        : "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "P2", "I0", "I1", "I2", "A0", "A1", "LC0", "LT0", "LB0", "cc",
          "memory");
    v->gt = c.gt >> 8;
    v->gn = c.gn >> 8;
    v->gr = c.gr >> 8;
    v->dcx = c.dcx;
    v->dcy = c.dcy;
#else
    int32_t gt = v->gt, gn = v->gn, gr = v->gr, dcx = v->dcx, dcy = v->dcy;
    for (int i = 0; i < n; i++) {
        int32_t cc = out[i], nz = fd_src[i];
        int32_t y = (cc * (gt >> 8)) >> 1;
        y += (nz * (gn >> 8)) >> 1;
        y += ((nz * cc) >> 15) * (gr >> 8);
        y >>= 6;                                                    /* Q23 */
        dcy = y - dcx + fd_qmul(DC_R, dcy);
        dcx = y;
        out[i] = soft_knee(dcy >> 8);
        gt += s->dgt;
        gn += s->dgn;
        gr += s->dgr;
    }
    v->gt = gt;
    v->gn = gn;
    v->gr = gr;
    v->dcx = dcx;
    v->dcy = dcy;
#endif
}

static void run_hv(int SRC, struct fd_voice *v, int16_t *out, int n, const struct seg *s, const struct flt *nf)
{
    hv_core(v, out, n, s);
    if (SRC)
        squares(v, n, nf);
    hv_src(v, n, nf, SRC);
    hv_filt(v, n, nf);
    hv_mix(v, out, n, s);
}

static void run(struct fd_voice *v, int16_t *out, int n, const struct seg *s, const struct flt *nf)
{
    int harsh = v->gf || s->dgf || v->gc != 1024 || s->dgc || v->gr || s->dgr || nf->wm || nf->wsq || nf->wx != 32767;
    if (harsh) {
        run_hv(nf->wm || nf->wsq, v, out, n, s, nf);
        return;
    }
    v->m1 = v->m2 = 0;                      /* the clean path has no feedback (gf is 0 here) */
    int noise = v->gn || s->dgn, fm = v->gm || s->dgm;
    if (noise && fm)
        run_v(1, 1, v, out, n, s, nf);
    else if (noise)
        run_v(1, 0, v, out, n, s, nf);
    else if (fm)
        run_v(0, 1, v, out, n, s, nf);
    else
        run_v(0, 0, v, out, n, s, nf);
}

void fmdrum_render(struct tobj *o, int16_t *out, const int16_t *p)
{
    uint32_t t0 = cycles();
    unsigned vi = (unsigned)o->voice < FD_VOICES ? (unsigned)o->voice : 0;
    struct fd_voice *v = &fmdrum_voice[vi];
    fmdrum_diag.renders++;
    int fresh = v->trig != v->seen;
    if (fresh) {                            /* from silence: phase 0, gains from 0; a sounding voice (retrigger,
                                             * steal) keeps its phases and gains and ramps from them: no click */
        v->seen = v->trig;
        v->choke = 0;
        if (!v->age || !v->aenv) {          /* (a finished note's DC decay counts as silence; its dcy goes on) */
            v->cph = v->mph = 0;
            v->gt = v->gn = v->gm = v->gf = v->gr = 0;
            v->gc = 1024;
            v->m1 = v->m2 = 0;
            v->lp = v->bp = 0;
            v->mlp = 0;
            v->dcx = 0;
            if (!v->age)
                v->dcy = 0;                 /* a voice the slot init silenced mid-note left it loud */
        }
        v->age = 1;
        v->aenv = v->menv = v->senv = ENV1;
        if (!v->seed) {
            v->seed = 0x2545F491u + vi * 0x9E3779B9u;
            for (int j = 0; j < 6; j++)     /* METAL's squares free-run from spread phases */
                v->mt[j] = v->seed * (uint32_t)(2 * j + 1);
        }
    }
    if (!v->age) {
        for (int i = 0; i < BLOCK; i++)
            out[i] = 0;
        return;
    }
    if (!v->aenv) {                         /* the note is over: the DC blocker's own decay to 0 (as the voice
                                             * would go on with silence; a short, low note leaves up to ~-30 dBFS
                                             * there, which ending the voice at once cut: a click), then the voice
                                             * ends. Rounded toward 0 (a negative value would stick: qmul floors). */
        int32_t dcy = v->dcy, dcx = v->dcx;
        for (int i = 0; i < BLOCK; i++) {
            dcy = (dcy < 0 ? -fd_qmul(DC_R, -dcy) : fd_qmul(DC_R, dcy)) - dcx;
            dcx = 0;
            out[i] = soft_knee(dcy >> 8);
        }
        v->dcx = 0;
        v->dcy = dcy;
        v->age = dcy > -TAIL_END && dcy < TAIL_END ? 0 : v->age + 1;
        return;
    }

    /* envelopes at the end of this block */
    int32_t a1 = env_step(v->aenv, v->choke ? CHOKE_DEC : fd_amp_dec[fd_pos(p[FD_DECAY]) >> 7]);
    int32_t m1 = env_step(v->menv, fd_mod_dec[fd_pos(p[FD_FMDEC]) >> 7]);
    int32_t s1 = env_step(v->senv, fd_swp_dec[fd_pos(p[FD_SWDEC]) >> 7]);

    /* pitch: K0 plus the sweep; carrier and modulator increments at the block's end (and start, for a new note) */
    int32_t sw = fd_sweep_q(p[FD_SWEEP]);
    uint32_t r = fd_ratio[fd_pos(p[FD_RATIO]) >> 10];
    int32_t q1 = p[FD_PITCH] + ((sw * (s1 >> 15)) >> 15);
    uint32_t ci1 = inc_of(q1), mi1 = mul_ratio(ci1, r);
    if (fresh) {
        v->cinc = inc_of(p[FD_PITCH] + sw);
        v->minc = mul_ratio(v->cinc, r);
    }
    uint32_t fc = (v->cinc > ci1 ? v->cinc : ci1) >> 12;             /* the block's highest carrier, Q20 of fs */
    uint32_t fm = ((fc >> 4) * (r >> 4)) >> 8;
    int32_t mute = fm >= M_MUTE ? 0 : fm <= M_FADE ? 32767 : (int32_t)((M_MUTE - fm) >> 1);

    /* FEEDBACK and SQUARE (FM's top zones): feedback beta, the carrier's clip gain and its level */
    int32_t kf = fd_pos(p[FD_FM]), beta = 0, gc1 = 1024, comp = 32767, isc = 4096;
    if (kf >= FD_FB0) {
        const uint16_t *z = fd_fmz + ((kf - FD_FB0) >> 7) * FD_FMZ_W;
        beta = z[0];
        gc1 = z[1];
        comp = z[2];
        isc = z[3];
    }
    /* the limiter counts the fed-back modulator's harmonics: fm x (1 + beta / 4), beta up to 3 rad */
    uint32_t fml = fm + (((fm >> 4) * (uint32_t)(beta > 768 ? 768 : beta)) >> 6);

    /* index: FM x velocity (0.5 .. 1) x the modulator envelope, then the limits */
    int32_t ipk = (((fd_index_q8(p[FD_FM]) * isc) >> 12) * (16384 + (v->vel >> 1))) >> 15;
    int32_t i1 = limit_index((ipk * (m1 >> 15)) >> 15, mute, fc, fml);
    int32_t gm1 = (i1 * RAD) >> 8;
    int32_t gm0 = fresh ? (limit_index(ipk, mute, fc, fml) * RAD) >> 8 : 0;
    /* feedback depth (phase units per Q15 of m1 + m2: beta / 2 x RAD), following the index's envelope's square root
     * (so a short FMDEC still leaves the grit audible) */
    int32_t gf1 = beta ? (((beta * (int32_t)isqrt32((uint32_t)m1)) >> 15) * RAD) >> 9 : 0, gf0 = (beta * RAD) >> 9;

    /* noise: NOISE's 129 points, interpolated, one detent ahead (a zone's first detent is already in it) */
    int32_t kn = fd_noise_sound(p[FD_NOISE]);
    int32_t nf8 = kn & 255;
    const uint16_t *n0 = fd_noise + (kn >> 8) * FD_NOISE_W, *n1 = n0 + FD_NOISE_W;
#define LERP(j) ((int32_t)n0[j] + ((((int32_t)n1[j] - (int32_t)n0[j]) * nf8) >> 8))
    int32_t tone = LERP(0), nlev = LERP(1), clap = LERP(7), ring = LERP(11), gu = LERP(12);
    struct flt nf;                          /* (field by field: an initialiser would call memset) */
    nf.f = LERP(2);
    nf.q = LERP(3);
    nf.wl = LERP(4);
    nf.wb = LERP(5);
    nf.wh = LERP(6);
    nf.wx = LERP(8);
    nf.wm = LERP(9);
    nf.wsq = LERP(10);
    nf.gi = 0;
    nf.go = 256;
    nf.mh = LERP(13);
#undef LERP
    if (nf.wm) {                            /* METAL: the 808's ratios from PITCH / 2 (with the sweep), within
                                             * FD_METAL_LOW .. TOP: the band sits on the squares' harmonics */
        int32_t qm = q1 - 6144;
        qm = qm < FD_METAL_LOW ? FD_METAL_LOW : qm < FD_METAL_TOP ? qm : FD_METAL_TOP;
        uint32_t base = inc_of(qm);
        for (int j = 0; j < 6; j++)
            nf.mi[j] = mul_ratio(base, fd_metal_ratio[j]);
        /* level: fd_metal_gain by the cluster's octave and NOISE's position from METAL's start (512 apart) */
        int32_t oq = ((qm < FD_METAL_OCT0 ? 0 : qm - FD_METAL_OCT0) * 43691) >> 20;     /* octaves Q8 (/ 6144) */
        int32_t tq = (kn - 20480) >> 1;                                                  /* points Q8 */
        oq = oq > FD_METAL_OCTS << 8 ? FD_METAL_OCTS << 8 : oq;
        tq = tq < 0 ? 0 : tq > (FD_METAL_TN - 1) << 8 ? (FD_METAL_TN - 1) << 8 : tq;
        int32_t oi = oq >> 8, ti = tq >> 8, of = oq & 255, tf = tq & 255;
        int32_t oj = oi < FD_METAL_OCTS ? oi + 1 : oi, tj = ti < FD_METAL_TN - 1 ? ti + 1 : ti;
        const uint16_t *g0 = fd_metal_gain + oi * FD_METAL_TN, *g1 = fd_metal_gain + oj * FD_METAL_TN;
        int32_t ga = g0[ti] + (((g0[tj] - g0[ti]) * tf) >> 8), gb = g1[ti] + (((g1[tj] - g1[ti]) * tf) >> 8);
        int32_t gq = ga + (((gb - ga) * of) >> 8);                                       /* Q12 */
        nf.unit = (nf.wm * 3861) >> 15;
        int32_t w2 = (nf.wm * nf.wm) >> 15;                     /* the squares' share of the source's power, */
        w2 = (w2 * w2) >> 15;                                   /* squared: the boost follows the crossfades late */
        nf.go = (4096 + (((gq - 4096) * w2) >> 15)) >> 4;
    }
    if (nf.wsq) {                           /* GRIND: max(80 Hz, PITCH / 2) .. PITCH x sqrt 2, unsteady: +-10 %,
                                             * a new value every 8 blocks (12 ms), glided */
        int32_t g0 = q1 - 6144 > FD_GRIND_LOW ? q1 - 6144 : FD_GRIND_LOW;
        uint32_t gi = inc_of(g0 + (((q1 + 3072 - g0) * gu) >> 15));
        uint32_t s8 = (v->age >> 3) + vi * 977u;
        int32_t ja = (int32_t)((s8 * 2654435761u) >> 26) - 32, jb = (int32_t)(((s8 + 1) * 2654435761u) >> 26) - 32;
        int32_t jit = ja + (((jb - ja) * (int32_t)(v->age & 7)) >> 3);
        nf.gi = gi + (uint32_t)((((int32_t)(gi >> 8)) * jit * 13) >> 4);
    }
    uint32_t b = v->age - 1;
    int32_t cm = b < FD_CLAP_N ? fd_clap[b] : 32767;
    int32_t cmul = 32767 - clap + ((clap * cm) >> 15);

    /* gains at the end of the block */
    int32_t amp = ((a1 >> 15) * v->vel) >> 15;
    int32_t tlev = comp >= 32767 ? FD_TONE : (FD_TONE * comp) >> 15;
    int32_t gt1 = ((((amp * tone) >> 15) * tlev) >> 15) << 8;     /* Q23: ramps of small gains stay exact */
    int32_t gn1 = ((((amp * nlev) >> 15) * cmul) >> 15) << 8;
    int32_t gr1 = ((amp * ring) >> 15) << 8;

    struct seg s;
    if (fresh) {                            /* attack: to the start values over ATTACK samples, then the block */
        int32_t a0 = ((ENV1 >> 15) * v->vel) >> 15;
        int32_t gt0 = ((((a0 * tone) >> 15) * tlev) >> 15) << 8;
        int32_t gn0 = ((((a0 * nlev) >> 15) * (32767 - clap + ((clap * 32767) >> 15))) >> 15) << 8;
        int32_t gr0 = ((a0 * ring) >> 15) << 8;
        s.dgt = (gt0 - v->gt) >> 5;
        s.dgn = (gn0 - v->gn) >> 5;
        s.dgm = (gm0 - v->gm) >> 5;
        s.dgf = (gf0 - v->gf) >> 5;
        s.dgc = (gc1 - v->gc) >> 5;
        s.dgr = (gr0 - v->gr) >> 5;
        s.dci = s.dmi = 0;
        run(v, out, ATTACK, &s, &nf);
        v->gc = gc1;
        /* the remaining 96 samples: x 683 / 65536 = 1 / 96 */
        s.dgt = ((gt1 - v->gt) >> 8) * 683 >> 8;
        s.dgn = ((gn1 - v->gn) >> 8) * 683 >> 8;
        s.dgm = ((gm1 - v->gm) >> 8) * 683 >> 8;
        s.dgf = ((gf1 - v->gf) >> 8) * 683 >> 8;
        s.dgc = 0;
        s.dgr = ((gr1 - v->gr) >> 8) * 683 >> 8;
        s.dci = (uint32_t)((((int32_t)(ci1 - v->cinc)) >> 10) * 683 >> 6);
        s.dmi = (uint32_t)((((int32_t)(mi1 - v->minc)) >> 10) * 683 >> 6);
        run(v, out + ATTACK, BLOCK - ATTACK, &s, &nf);
    } else {
        s.dgt = (gt1 - v->gt) >> 7;
        s.dgn = (gn1 - v->gn) >> 7;
        s.dgm = (gm1 - v->gm) >> 7;
        s.dgf = (gf1 - v->gf) >> 7;
        s.dgc = (gc1 - v->gc) >> 7;
        s.dgr = (gr1 - v->gr) >> 7;
        s.dci = (uint32_t)((int32_t)(ci1 - v->cinc) >> 7);
        s.dmi = (uint32_t)((int32_t)(mi1 - v->minc) >> 7);
        run(v, out, BLOCK, &s, &nf);
    }
    /* land exactly on the block's end values (the ramps' truncation would drift) */
    v->gt = gt1;
    v->gn = gn1;
    v->gm = gm1;
    v->gf = gf1;
    v->gc = gc1;
    v->gr = gr1;
    v->cinc = ci1;
    v->minc = mi1;
    v->aenv = a1;
    v->menv = m1;
    v->senv = s1;
    v->index_q8 = i1;
    v->age++;
    if (a1 < SILENT) {                      /* silent: the DC blocker's decay next, or the voice ends */
        v->aenv = 0;
        if (v->dcy > -TAIL_END && v->dcy < TAIL_END)
            v->age = 0;
    }

    uint32_t dt = cycles() - t0;
    fmdrum_diag.cyc_max = dt > fmdrum_diag.cyc_max ? dt : fmdrum_diag.cyc_max;
}

static int choke_member(const int16_t *key_params, int32_t key)
{
    int z = fd_noise_zone_of(key_params[FD_NOISE]);
    return key >= FD_CHOKE_LO && key <= FD_CHOKE_HI && (z == NZ_HAT || z == NZ_METAL);
}

/* key_params = &params[slot][key][0] (TE's per-key array, 24 x 8 per slot); the key comes from TE's voice array */
void fmdrum_note_on(struct tobj *o, const int16_t *key_params)
{
    unsigned vi = (unsigned)o->voice < FD_VOICES ? (unsigned)o->voice : 0;
    struct fd_voice *v = &fmdrum_voice[vi];
    int32_t key = te_drum_key[vi], vel = te_drum_vel[vi];
    key = key < 0 ? 0 : key >= FD_KEYS ? FD_KEYS - 1 : key;
    if (key_params && choke_member(key_params, key))        /* hi-hat choke: release the group's other hats */
        for (int i = 0; i < FD_VOICES; i++) {
            struct fd_voice *w = &fmdrum_voice[i];
            if (i != (int)vi && w->age                     /* every other hat voice, the same key too: one hi-hat */
                && choke_member(key_params + (w->key - key) * 8, w->key))   /* same slot's row for w's key */
                w->choke = 1;
        }
    v->key = key;
    v->vel = vel < 0 ? 0 : vel;
    v->trig++;                              /* render restarts the voice at its next block */
    fmdrum_diag.noteons++;
    fmdrum_diag.last_voice = (int32_t)vi;
}

/* TE frees the voice when this returns 0 and steals the voice with the largest value: a finished note's DC decay
 * first, then the oldest. */
int32_t fmdrum_active(struct tobj *o)
{
    unsigned vi = (unsigned)o->voice < FD_VOICES ? (unsigned)o->voice : 0;
    const struct fd_voice *v = &fmdrum_voice[vi];
    return v->trig != v->seen ? 1 : v->age && !v->aenv ? 0x40000000 : (int32_t)v->age;
}

void fmdrum_note_off(struct tobj *o, const int16_t *key_params)
{
    (void)o;                                /* one-shot: drums ignore note-off */
    (void)key_params;
}

/* From the factory (slot.S _drum_factory) before the voices are built. The slot's per-key values stay as TE loaded
 * them (dbox_data); our voices start silent; our range row (ReadInterface normalises with it) = dbox's row 9 once TE
 * has filled it. */
void fmdrum_slot_init(int32_t slot)
{
    (void)slot;
    fmdrum_diag.inits++;
    for (int i = 0; i < FD_VOICES; i++) {
        fmdrum_voice[i].seen = fmdrum_voice[i].trig;
        fmdrum_voice[i].age = 0;
    }
    const int16_t *r9 = ranges + 9 * 32;
    int16_t *rn = ranges + fmdrum_id * 32;
    for (int i = 0; i < 8; i++)
        if (r9[4 * i] >= r9[4 * i + 1])
            return;
    for (int i = 0; i < 32; i++)
        rn[i] = r9[i];
    fmdrum_diag.row_copied = 1;
}
