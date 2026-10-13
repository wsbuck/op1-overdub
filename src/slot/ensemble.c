/* "ensemble" FX (src/slot/fx.json; first of our FX ids 8+): a Juno/Solina-style chorus ensemble. Design and
 * addresses: notes/fx-slot.md. Contract for every FX source (<source>.c exports <source>_mono/_stereo):
 *
 * TE calls process through the vtable (+0x14) of a filter FX object whose vptr build_slot.py's factory case
 * swapped for ours, from the L1 audio callback, once per 64-frame block (44.1 kHz, Q15):
 *   synth/drum (mono): process(this, int16 buf[64] in place, const int16 params[8])
 *   master (stereo):   process(this, int16 L[64], int16 R[64], const int16 params[8])
 * slot_gen.inc wraps both (FX_MONO_ENTRY/FX_STEREO_ENTRY in slot.S: save the hardware loops, zero L0-L3).
 * The object itself (TE's filter state) is not touched; state lives in the slot block (.data) or a FREE
 * SDRAM buffer. Keep one mono and one stereo instance: both can run at once (synth FX + master FX).
 *
 * Three delay taps swept by one triangle LFO at 0, 120 and 240 degrees (on R: +180, Juno's anti-phase).
 *   blue   rate: 0.05 .. 12.8 Hz, exponential (Juno I 0.5 Hz at 13600, II 0.83 Hz at 16000)
 *   green  depth: sweep 0 .. +-3 ms around 2 ms + depth
 *   white  ensemble: 0 = one tap per channel (Juno chorus); up = the 120/240 degree taps fade in and a 6.5 Hz
 *          vibrato is added (string ensemble shimmer)
 *   orange mix: dry .. wet (16384 = Juno's 50/50)
 * Delays are linearly interpolated. Depth glides (slew-limited) and the ensemble amount over each block, so knob
 * moves and LFO modulation do not click. Summed, the three 120-degree taps cancel each other's pitch wobble at
 * the LFO rate (what remains is at 3x, plus the moving comb): the string-ensemble sound.
 */
#include <stdint.h>
#include "ensemble.h"

#define RAM __attribute__((section(".data")))
#define BLOCK 64
#define RING 1024                       /* power of two; longest tap 88 + 2*132 + 9 + 1 = 362 samples */
#define BASE_Q16 (88 << 16)             /* 2 ms */
#define DEPTH_MAX 132                   /* samples: 3 ms */
#define VIB_MAX 9                       /* samples: 0.2 ms */
#define VIB_INC 633008u                 /* 6.5 Hz: 6.5 / 44100 * 2^32 */
#define THIRD 0x55555555u               /* 120 degrees of phase */
#define HALF 0x80000000u
#define SLEW 4096                       /* Q16: depth glides at most 1/16 sample per sample (~1 semitone) */

void ensemble_mono(void *self, int16_t *buf, const int16_t *p);
void ensemble_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);

struct chan {
    int16_t ring[RING];
    uint32_t w;
};


static struct chan mono_ch RAM = {{0}, 0};
static struct chan st_ch[2] RAM = {{{0}, 0}, {{0}, 0}};
struct ens ensemble_state[2] RAM = {{0, 0, 0, 0, 0, 0, {{0}}}, {0, 0, 0, 0, 0, 0, {{0}}}};   /* mono, stereo */

static const uint32_t exp2_frac[17] = {   /* (2^(i/16) - 1) in Q16 */
    0, 2902, 5932, 9096, 12400, 15850, 19454, 23216, 27146, 31249, 35534, 40009, 44682, 49562, 54658, 59979, 65536,
};

/* LFO phase increment for knob k: 0.05 Hz * 2^(8 k / 32768). 0.05 Hz = 4869.6 per sample (2^32 / 44100 / 20). */
static uint32_t rate_inc(int32_t k)
{
    int32_t oct = k >> 12;                          /* 0..7 */
    int32_t f = k & 4095;                           /* fraction of an octave, Q12 */
    int32_t i = f >> 8, t = f & 255;               /* 16 table steps per octave */
    uint32_t m = exp2_frac[i] + (((exp2_frac[i + 1] - exp2_frac[i]) * t) >> 8);   /* Q16 mantissa - 1 */
    return (4870u * (65536u + m) >> 16) << oct;
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

/* Delay-line read at d (Q16 samples behind write position w, the newest sample being w - 1). */
static inline int32_t tap(const int16_t *ring, uint32_t w, int32_t d)
{
    uint32_t pos = (w << 16) - (uint32_t)d;
    uint32_t i = (pos >> 16) & (RING - 1);
    int32_t a = ring[(i - 1) & (RING - 1)], b = ring[i];
    int32_t f = (pos >> 1) & 0x7FFF;               /* Q15 weight of b */
    return a + (((b - a) * f) >> 15);
}

/* A delay that jumps would click; moving it faster than SLEW would swoop the pitch. */
static inline int32_t slew(int32_t d, int32_t max)
{
    return d > max ? max : d < -max ? -max : d;
}

static inline int16_t sat(int32_t x)
{
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

/* One block for nch channels sharing the LFO. The LFO is evaluated every SUB samples and the tap delays move
 * linearly in between; channel 1's taps sit at +180 degrees, i.e. mirrored around the centre delay. */
#define SUB 8
static void ensemble(struct ens *s, struct chan *ch, int16_t **io, int nch, const int16_t *p)
{
    uint32_t inc = rate_inc(knob(p[0])) * SUB;
    int32_t depth_t = knob(p[1]) * DEPTH_MAX * 2;  /* Q16 samples: k / 32768 * DEPTH_MAX << 16 */
    int32_t ens_t = knob(p[2]);
    int32_t vdepth_t = ens_t * VIB_MAX * 2;
    int32_t mix_t = knob(p[3]);
    int32_t dens = (ens_t - s->ens) / (BLOCK / SUB), dmix = (mix_t - s->mix) / (BLOCK / SUB);
    for (int i0 = 0; i0 < BLOCK; i0 += SUB) {
        s->depth += slew(depth_t - s->depth, SLEW * SUB);
        s->vdepth += slew(vdepth_t - s->vdepth, SLEW * SUB);
        s->ens += dens;
        s->lfo += inc;
        s->vib += VIB_INC * SUB;
        int32_t centre = BASE_Q16 + s->depth + s->vdepth + 65536;
        /* Weights: tap 0 = 1 - 2e/3, taps 1 and 2 = e/3 each (sum 1); e = ensemble amount. */
        int32_t w12 = (s->ens * 10923) >> 15, w0 = 32768 - 2 * w12;
        int32_t mix = s->mix, mstep = dmix / SUB;
        s->mix += dmix;
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
            struct chan *h = &ch[c];
            int16_t *x = io[c] + i0;
            int32_t d0 = s->d[c][0], d1 = s->d[c][1], d2 = s->d[c][2];
            int32_t s0 = step[c][0], s1 = step[c][1], s2 = step[c][2];
            uint32_t w = h->w;
            int32_t mx = mix;
            for (int i = 0; i < SUB; i++) {
                int32_t in = x[i];
                h->ring[w & (RING - 1)] = in;
                w++;
                d0 += s0;
                d1 += s1;
                d2 += s2;
                int32_t wet = tap(h->ring, w, d0) * w0 + (tap(h->ring, w, d1) + tap(h->ring, w, d2)) * w12;
                wet >>= 15;
                mx += mstep;
                x[i] = sat(in + (((wet - in) * mx) >> 15));
            }
            h->w = w;
            s->d[c][0] = d0;
            s->d[c][1] = d1;
            s->d[c][2] = d2;
        }
    }
    s->ens = ens_t;                                 /* exact at the block end (the steps truncate) */
    s->mix = mix_t;
}

void ensemble_mono(void *self, int16_t *buf, const int16_t *p)
{
    (void)self;
    int16_t *io[1] = {buf};
    ensemble(&ensemble_state[0], &mono_ch, io, 1, p);
}

void ensemble_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p)
{
    (void)self;
    int16_t *io[2] = {l, r};
    ensemble(&ensemble_state[1], st_ch, io, 2, p);
}
