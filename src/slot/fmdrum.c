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
 *            colour, level, tone level and clap bursts along one knob (OFF, AIR, SNARE, HAT, CLAP)
 *   amp      one exponential decay (DECAY) for tone and noise; velocity (MIDI; the OP-1's keys send 127) scales the
 *            level and the index (0.5 .. 1)
 * Knobs (per key, TE's dbox path, range row 9; fmdrum.h FD_*), read every block, so turning one changes a sounding note
 * smoothly; an amount on each plain knob, its time (or partner) on SHIFT:
 *   [0] blue   PITCH  14 Hz .. 3.6 kHz (512 per semitone, 0 = A3)   [4] SHIFT+blue    RATIO 15 harmonic, 17 inharmonic
 *   [1] green  SWEEP  0 .. +48 semitones                            [5] SHIFT+green   SWDEC sweep time 1 .. 300 ms
 *   [2] white  FM     index 0 .. 16 rad                             [6] SHIFT+white   FMDEC index decay 3 ms .. 3 s
 *   [3] orange DECAY  15 ms .. 4 s (to -60 dB)                      [7] SHIFT+orange  NOISE OFF/AIR/SNARE/HAT/CLAP
 * Hi-hat choke: a note-on on keys 10..13 whose NOISE is in the HAT zone releases the other sounding voices of the group
 * that are in the HAT zone too, in ~5 ms (open hat cut by the closed one).
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
 * Fixed point, no division (reciprocal constants, multiply-shift), no libgcc, no .bss.
 */
#include <stdint.h>
#include "dsp.h"
#include "sine_table.h"
#include "fmdrum.h"
#include "fmdrum_tables.h"

#define RAM __attribute__((section(".data")))
#define BLOCK 128
#define ENV1 (1 << 30)                      /* envelopes, Q30 */
#define SILENT 339547                       /* -70 dB of ENV1: the voice ends */
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
    return qmul(e, factor << 1);
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
    int32_t dgt, dgn, dgm;
    uint32_t dci, dmi;
};

struct flt {                                /* the block's noise filter */
    int32_t f, q, wl, wb, wh;
};

/* one run of n samples; NOISE and FM are constants at each call (run() below picks the variant), so a key without
 * noise or without FM skips that part of the loop */
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
        dcy = y - dcx + qmul(DC_R, dcy);
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

static void run(struct fd_voice *v, int16_t *out, int n, const struct seg *s, const struct flt *nf)
{
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
        if (!v->age) {
            v->cph = v->mph = 0;
            v->gt = v->gn = v->gm = 0;
            v->lp = v->bp = 0;
        }
        v->age = 1;
        v->aenv = v->menv = v->senv = ENV1;
        if (!v->seed)
            v->seed = 0x2545F491u + vi * 0x9E3779B9u;
    }
    if (!v->age) {
        for (int i = 0; i < BLOCK; i++)
            out[i] = 0;
        return;
    }

    /* envelopes at the end of this block */
    int32_t a1 = env_step(v->aenv, v->choke ? CHOKE_DEC : fd_amp_dec[fd_pos(p[FD_DECAY]) >> 7]);
    int32_t m1 = env_step(v->menv, fd_mod_dec[fd_pos(p[FD_FMDEC]) >> 7]);
    int32_t s1 = env_step(v->senv, fd_swp_dec[fd_pos(p[FD_SWDEC]) >> 7]);

    /* pitch: K0 plus the sweep; carrier and modulator increments at the block's end (and start, for a new note) */
    int32_t sw = fd_sweep_q(p[FD_SWEEP]);
    uint32_t r = fd_ratio[fd_pos(p[FD_RATIO]) >> 10];
    uint32_t ci1 = inc_of(p[FD_PITCH] + ((sw * (s1 >> 15)) >> 15)), mi1 = mul_ratio(ci1, r);
    if (fresh) {
        v->cinc = inc_of(p[FD_PITCH] + sw);
        v->minc = mul_ratio(v->cinc, r);
    }
    uint32_t fc = (v->cinc > ci1 ? v->cinc : ci1) >> 12;             /* the block's highest carrier, Q20 of fs */
    uint32_t fm = ((fc >> 4) * (r >> 4)) >> 8;
    int32_t mute = fm >= M_MUTE ? 0 : fm <= M_FADE ? 32767 : (int32_t)((M_MUTE - fm) >> 1);

    /* index: FM x velocity (0.5 .. 1) x the modulator envelope, then the limits */
    int32_t ipk = (fd_index_q8(p[FD_FM]) * (16384 + (v->vel >> 1))) >> 15;
    int32_t i1 = limit_index((ipk * (m1 >> 15)) >> 15, mute, fc, fm);
    int32_t gm1 = (i1 * RAD) >> 8;
    int32_t gm0 = fresh ? (limit_index(ipk, mute, fc, fm) * RAD) >> 8 : 0;

    /* noise: NOISE's 65 points, interpolated */
    int32_t kn = fd_pos(p[FD_NOISE]), nf9 = kn & 511;
    const int32_t *n0 = fd_noise + (kn >> 9) * FD_NOISE_W, *n1 = n0 + FD_NOISE_W;
#define LERP(j) (n0[j] + (((n1[j] - n0[j]) * nf9) >> 9))
    int32_t tone = LERP(0), nlev = LERP(1), clap = LERP(7);
    struct flt nf = { LERP(2), LERP(3), LERP(4), LERP(5), LERP(6) };
#undef LERP
    uint32_t b = v->age - 1;
    int32_t cm = b < FD_CLAP_N ? fd_clap[b] : 32767;
    int32_t cmul = 32767 - clap + ((clap * cm) >> 15);

    /* gains at the end of the block */
    int32_t amp = ((a1 >> 15) * v->vel) >> 15;
    int32_t gt1 = ((((amp * tone) >> 15) * FD_TONE) >> 15) << 8;   /* Q23: ramps of small gains stay exact */
    int32_t gn1 = ((((amp * nlev) >> 15) * cmul) >> 15) << 8;

    struct seg s;
    if (fresh) {                            /* attack: to the start values over ATTACK samples, then the block */
        int32_t a0 = ((ENV1 >> 15) * v->vel) >> 15;
        int32_t gt0 = ((((a0 * tone) >> 15) * FD_TONE) >> 15) << 8;
        int32_t gn0 = ((((a0 * nlev) >> 15) * (32767 - clap + ((clap * 32767) >> 15))) >> 15) << 8;
        s.dgt = (gt0 - v->gt) >> 5;
        s.dgn = (gn0 - v->gn) >> 5;
        s.dgm = (gm0 - v->gm) >> 5;
        s.dci = s.dmi = 0;
        run(v, out, ATTACK, &s, &nf);
        /* the remaining 96 samples: x 683 / 65536 = 1 / 96 */
        s.dgt = ((gt1 - v->gt) >> 8) * 683 >> 8;
        s.dgn = ((gn1 - v->gn) >> 8) * 683 >> 8;
        s.dgm = ((gm1 - v->gm) >> 8) * 683 >> 8;
        s.dci = (uint32_t)((((int32_t)(ci1 - v->cinc)) >> 10) * 683 >> 6);
        s.dmi = (uint32_t)((((int32_t)(mi1 - v->minc)) >> 10) * 683 >> 6);
        run(v, out + ATTACK, BLOCK - ATTACK, &s, &nf);
    } else {
        s.dgt = (gt1 - v->gt) >> 7;
        s.dgn = (gn1 - v->gn) >> 7;
        s.dgm = (gm1 - v->gm) >> 7;
        s.dci = (uint32_t)((int32_t)(ci1 - v->cinc) >> 7);
        s.dmi = (uint32_t)((int32_t)(mi1 - v->minc) >> 7);
        run(v, out, BLOCK, &s, &nf);
    }
    /* land exactly on the block's end values (the ramps' truncation would drift) */
    v->gt = gt1;
    v->gn = gn1;
    v->gm = gm1;
    v->cinc = ci1;
    v->minc = mi1;
    v->aenv = a1;
    v->menv = m1;
    v->senv = s1;
    v->index_q8 = i1;
    v->age = a1 < SILENT ? 0 : v->age + 1;

    uint32_t dt = cycles() - t0;
    fmdrum_diag.cyc_max = dt > fmdrum_diag.cyc_max ? dt : fmdrum_diag.cyc_max;
}

static int choke_member(const int16_t *key_params, int32_t key)
{
    return key >= FD_CHOKE_LO && key <= FD_CHOKE_HI && fd_noise_zone_of(key_params[FD_NOISE]) == 3;
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

/* TE frees the voice when this returns 0 and steals the voice with the largest value: the oldest. */
int32_t fmdrum_active(struct tobj *o)
{
    unsigned vi = (unsigned)o->voice < FD_VOICES ? (unsigned)o->voice : 0;
    const struct fd_voice *v = &fmdrum_voice[vi];
    return v->trig != v->seen ? 1 : (int32_t)v->age;
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
