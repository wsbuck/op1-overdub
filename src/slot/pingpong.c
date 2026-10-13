/* "pingpong" FX (src/slot/fx.json): a stereo ping-pong delay, echoes alternating L -> R -> L. Design, knob ranges
 * and memory layout: notes/fx-pingpong.md; FX contract: ensemble.c and notes/fx-slot.md.
 *
 * Master (stereo): (L + R) / 2 feeds the L line; L's output crosses into the R line and R's back into L, each
 * crossing through the feedback gain and the tone filter. So an input heard at t = 0 comes back on L at d, on R at
 * 2d (x g), on L at 3d (x g^2), ... Synth/drum (mono): one line, the same knobs.
 *   blue   time: 20 ms .. 1.28 s, exponential (6 octaves: 20 * 2^(6 k / 32768) ms). Changes glide like tape
 *          (the read position slews, pitch bends; at most 0.5 sample per sample), so they never click
 *   green  feedback: gain per echo 0 .. 0.95; a soft clip in the loop (linear up to -4 dBFS, ceiling -1.2 dBFS)
 *          keeps a hot loop bounded
 *   white  tone of the repeats: 0 = dark (one-pole low-pass at 1.2 kHz), 16384 = flat, max = bright (the
 *          matching high-pass); in the loop, so each repeat is darker (or thinner) than the last
 *   orange mix: dry .. wet (crossfade; 0 is bit exact dry)
 * Feedback, tone and mix glide linearly over each block.
 *
 * The lines live in a FREE SDRAM range (pingpong_ram, build_slot.py "ram"): not loaded, contents undefined at boot.
 * Each instance counts the samples written since boot (filled) and reads anything older as 0, so the stale
 * contents are never heard. The state survives the FX being switched off and on: re-enabled, it plays on from
 * where it stopped, like a tape that was paused.
 */
#include <stdint.h>
#include "pingpong.h"

#define RAM __attribute__((section(".data")))
#define BLOCK 64
#define SUB 8                               /* the time glide steps every SUB samples */
#define RING 65536                          /* samples per line, power of two; longest delay 56448 + 1 */
#define D_MIN 882                           /* 20 ms */
#define G_MAX 31130                         /* 0.95, Q15 */
#define TONE_B 5150                         /* one-pole coefficient 1 - exp(-2 pi 1200 / 44100), Q15 */
#define SLEW 16384                          /* Q15 samples per sample: the read glides at most 0.5x off pitch */
#define KNEE 20480                          /* soft clip: linear up to here (-4 dBFS) ... */
#define CEIL (KNEE + 8192)                  /* ... then a parabola up to 28672 (-1.2 dBFS), flat from KNEE + 16384 */

void pingpong_mono(void *self, int16_t *buf, const int16_t *p);
void pingpong_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);

/* FREE SDRAM, 3 lines: stereo L, stereo R, mono (3 * 128 KB of the 576 KB at 0x01E80000). */
extern int16_t pingpong_ram[];

struct pp pingpong_state[2] RAM = {{0, 0, 0, 0, 0, 0, {0, 0}}, {0, 0, 0, 0, 0, 0, {0, 0}}};
#define mono_st pingpong_state[0]
#define st_st pingpong_state[1]

static const uint32_t exp2_frac[17] = {   /* (2^(i/16) - 1) in Q16 */
    0, 2902, 5932, 9096, 12400, 15850, 19454, 23216, 27146, 31249, 35534, 40009, 44682, 49562, 54658, 59979, 65536,
};

static inline int32_t knob(int16_t k)
{
    return k < 0 ? 0 : k;
}

/* Delay for knob k, Q15 samples: D_MIN * 2^(6 k / 32768), 882 .. 56448 (fits: 56448 << 15 < 2^31). */
static int32_t time_q15(int32_t k)
{
    int32_t e = k * 6;                              /* Q15 octaves, 0 .. 6 */
    int32_t oct = e >> 15;                          /* 0..5 */
    int32_t f = e & 32767;
    int32_t i = f >> 11, t = f & 2047;              /* 16 table steps per octave */
    uint32_t m = exp2_frac[i] + (((exp2_frac[i + 1] - exp2_frac[i]) * t) >> 11);   /* Q16 mantissa - 1 */
    return (int32_t)(((D_MIN * (65536u + m)) >> 1) << oct);
}

/* Line read at d (Q15 samples behind the next write index w), linearly interpolated. Samples not written since
 * boot (filled = how many were) read as 0: the FREE SDRAM is undefined. */
static inline int32_t tap(const int16_t *ring, uint32_t w, uint32_t filled, int32_t d)
{
    uint32_t n = (uint32_t)d >> 15;
    if (n > filled)                                 /* a = x[t - n] and b = x[t - n - 1]; filled = t */
        return 0;
    int32_t a = ring[(w - n) & (RING - 1)], b = n < filled ? ring[(w - n - 1) & (RING - 1)] : 0;
    return a + (((b - a) * (d & 32767)) >> 15);
}

/* Soft clip: linear to KNEE, then y = KNEE + t - t^2 / 32768 (slope 1 -> 0) up to CEIL; never above. */
static inline int32_t clip(int32_t x)
{
    int32_t a = x < 0 ? -x : x;
    if (a > KNEE) {
        int32_t t = a - KNEE;
        if (t > 2 * (CEIL - KNEE))
            t = 2 * (CEIL - KNEE);
        a = KNEE + t - ((t * t) >> 15);
    }
    return x < 0 ? -a : a;
}

/* The repeats' tone: blend towards the low-pass (dark, tn < 0) or away from it (bright: x - lp = high-pass).
 * Both blends have gain <= 1 at every frequency, so the loop gain stays <= g. */
static inline int32_t tone(int32_t x, int32_t *lp, int32_t tn)
{
    int32_t s = *lp + (((x - *lp) * TONE_B) >> 15);
    *lp = s;
    return tn < 0 ? x + (((s - x) * -tn) >> 15) : x - ((s * tn) >> 15);
}

/* Knob targets; the first block starts on them (no glide in from the zeroed state). */
static void targets(struct pp *s, const int16_t *p, int32_t *dt, int32_t *gt, int32_t *tt, int32_t *mt)
{
    *dt = time_q15(knob(p[0]));
    *gt = (knob(p[1]) * G_MAX) >> 15;
    *tt = (knob(p[2]) - 16384) * 2;
    *mt = knob(p[3]);
    *mt += *mt >> 14;                               /* 32767 -> 32768: full wet, no dry LSB left */
    if (!s->d) {
        s->d = *dt;
        s->g = *gt;
        s->tone = *tt;
        s->mix = *mt;
    }
}

/* Time glide for the next SUB samples: an exponential approach (~93 ms) limited to SLEW. */
static inline int32_t dstep(int32_t d, int32_t dt)
{
    int32_t st = (dt - d) >> 12;
    return st > SLEW ? SLEW : st < -SLEW ? -SLEW : st;
}

void pingpong_mono(void *self, int16_t *buf, const int16_t *p)
{
    (void)self;
    struct pp *s = &mono_st;
    int16_t *ring = pingpong_ram + 2 * RING;
    int32_t dt, gt, tt, mt;
    targets(s, p, &dt, &gt, &tt, &mt);
    int32_t dg = (gt - s->g) / BLOCK, dtn = (tt - s->tone) / BLOCK, dm = (mt - s->mix) / BLOCK;
    int32_t d = s->d, g = s->g, tn = s->tone, mix = s->mix, lp = s->lp[0];
    uint32_t w = s->w, filled = s->filled;
    for (int i0 = 0; i0 < BLOCK; i0 += SUB) {
        int32_t st = dstep(d, dt);
        for (int i = i0; i < i0 + SUB; i++) {
            d += st;
            g += dg;
            tn += dtn;
            mix += dm;
            int32_t in = buf[i];
            int32_t y = tap(ring, w, filled, d);
            ring[w & (RING - 1)] = clip(in + tone((y * g) >> 15, &lp, tn));
            w++;
            filled += filled < RING;
            buf[i] = in + (((y - in) * mix) >> 15);
        }
    }
    s->d = d;
    s->g = gt;                                      /* exact at the block end (the steps truncate) */
    s->tone = tt;
    s->mix = mt;
    s->lp[0] = lp;
    s->w = w;
    s->filled = filled;
}

void pingpong_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p)
{
    (void)self;
    struct pp *s = &st_st;
    int16_t *rl = pingpong_ram, *rr = pingpong_ram + RING;
    int32_t dt, gt, tt, mt;
    targets(s, p, &dt, &gt, &tt, &mt);
    int32_t dg = (gt - s->g) / BLOCK, dtn = (tt - s->tone) / BLOCK, dm = (mt - s->mix) / BLOCK;
    int32_t d = s->d, g = s->g, tn = s->tone, mix = s->mix, lpl = s->lp[0], lpr = s->lp[1];
    uint32_t w = s->w, filled = s->filled;
    for (int i0 = 0; i0 < BLOCK; i0 += SUB) {
        int32_t st = dstep(d, dt);
        for (int i = i0; i < i0 + SUB; i++) {
            d += st;
            g += dg;
            tn += dtn;
            mix += dm;
            int32_t xl = l[i], xr = r[i];
            int32_t yl = tap(rl, w, filled, d), yr = tap(rr, w, filled, d);
            uint32_t wi = w & (RING - 1);
            rl[wi] = clip(((xl + xr) >> 1) + tone((yr * g) >> 15, &lpl, tn));   /* input and R's echo */
            rr[wi] = clip(tone((yl * g) >> 15, &lpr, tn));                       /* L's echo crosses over */
            w++;
            filled += filled < RING;
            l[i] = xl + (((yl - xl) * mix) >> 15);
            r[i] = xr + (((yr - xr) * mix) >> 15);
        }
    }
    s->d = d;
    s->g = gt;
    s->tone = tt;
    s->mix = mt;
    s->lp[0] = lpl;
    s->lp[1] = lpr;
    s->w = w;
    s->filled = filled;
}
