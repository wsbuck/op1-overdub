/* Modal synthesis: exciter -> bank of MODES two-pole resonators. Fixed-point port of krystal's Modal
 * engine (krystal/firmware/engines/modal/modal_engine.cpp): same materials, exciters, ring-time curve,
 * per-mode damping slope, strike weighting and brightness tilt, with these changes for the OP-1:
 *   - 20 modes (krystal: 64) for the CPU budget; mono (no spread/pan); position 0.30 and density fixed;
 *     no lattice yet. Velocity fixed at 0.8; no note-off, so bow/breath/rain excite while the voice
 *     renders and TE's envelope shapes the release.
 *   - Knobs: [0] blue material (8, morphing), [1] green exciter (mallet, pluck, bow, breath, rain),
 *     [2] white ring (T60 0.05 s .. 20 s, infinite above 95%), [3] orange brightness.
 * Fixed point: resonator states Q24, a1 = 2 r cos w (Q30), a2 = -r^2 (Q31), gains Q31; mode frequencies
 * f0 * 2^log2ratio from the per-voice phase increment, cos w = 1 - 2 sin^2(w/2) from a Q30 sine table
 * (accurate for low modes). Coefficients are computed once per block.
 * Per-voice memory: resonator and exciter state (~220 B) exceed TE's 32-byte voice state, so each voice
 * uses a 4 KB slot of voice_ram (FREE region 0 on device), found from its voice index.
 * Resonator loops are hand-written (modal_asm.S). Bursts are generated as they play.
 * Note-on: TE writes 0x80000000 to state bytes 0..3; we strike and clear it.
 */
#include "engine.h"
#include "dsp.h"
#include "modal_tables.h"
#include "modal_state.h"

extern uint8_t voice_ram[];            /* 8 x 4 KB: 0x00810000 on device (build.sh), an array in the sim */

/* modal_asm.S: one mode over n samples, sum[i] += y (with input x when given); c2h = a2 / 2, gh = g / 2. */
void modal_mode(int32_t *sum, int32_t c1, int32_t c2h, int32_t *state, int n);
void modal_mode_x(int32_t *sum, int32_t c1, int32_t c2h, int32_t *state, int32_t gh, const int32_t *x, int n);

#define BOW_CHUNK 32    /* bow friction is updated every 32 samples (0.73 ms) so the fast loops can run */

enum { MALLET, PLUCK, BOW, BREATH, RAIN };

struct state {                         /* TE voice object +4 .. +0x23 */
    uint32_t note_on;                  /* 0: 0x80000000 after TE's note-on */
    uint32_t spare[7];
};

#define vram modal_voice
#if MODES != MODAL_MODES
#error modal_tables.h and modal_state.h disagree on MODES
#endif

static struct vram *voice_of(void *st)
{
    uint32_t off = (uint32_t)st - 0xFFB00004u;     /* TE voice i: 0xFFB00000 + i * 0x24, state at +4 */
    uint32_t i = (off * 1821u) >> 16;              /* off / 36 for off <= 180 */
    if (off > 5 * 0x24 || i * 0x24 != off)
        i = 7;                                     /* not a TE voice (simulator) */
    return voice_slot_claim(voice_ram + i * VOICE_RAM_BYTES, OWNER_MODAL, sizeof(struct vram));
}

/* All modes over n samples: sum[0..n) = sum of mode outputs (x may be NULL). */
static void run_modes(struct vram *v, const int32_t *a1, const int32_t *a2, const int32_t *g,
                      int32_t *sum, const int32_t *x, int n)
{
    int32_t ys[2];
    for (int i = 0; i < n; i++)
        sum[i] = 0;
    for (int k = 0; k < MODES; k++) {
        if (!a1[k] && !a2[k])
            continue;
        ys[0] = v->y1[k];
        ys[1] = v->y2[k];
        if (x && g[k])
            modal_mode_x(sum, a1[k], a2[k] >> 1, ys, g[k] >> 1, x, n);
        else
            modal_mode(sum, a1[k], a2[k] >> 1, ys, n);
        v->y1[k] = ys[0];
        v->y2[k] = ys[1];
    }
}

static inline uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static inline int32_t noise_q15(uint32_t *s)
{
    return (int32_t)xorshift(s) >> 16;
}

/* 2^(-y), y in Q16 (>= 0), Q31. */
static int32_t exp2neg(int32_t y)
{
    if (y < 0)
        y = 0;
    int n = y >> 16;
    if (n >= 31)
        return 0;
    int i = (y >> 8) & 255;
    int32_t a = exp2neg_q31[i], b = exp2neg_q31[i + 1];
    return (a + (((b - a) >> 8) * (y & 255))) >> n;
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* Start an exciter burst (krystal strike()): decaying noise, low-passed except for pluck, normalised to
 * a target RMS. krystal pre-renders and measures the burst; here a dry run measures its energy (the burst
 * is deterministic) and it is then generated as it plays, so note-on costs ~12 cycles per burst sample
 * instead of a full render. (An expected-RMS formula was tried first: up to 7 dB off for short bursts.)
 * c = burst low-pass coefficient, Q15. */
static void strike(struct vram *v, int ex, int32_t bright, int32_t c, int32_t amount)
{
    int32_t len;
    if (ex == MALLET)
        len = 132 + ((529 * (32767 - bright)) >> 15);        /* 3 + 12 (1 - b) ms */
    else if (ex == PLUCK)
        len = 26;                                           /* 0.6 ms */
    else if (ex == RAIN)
        len = 44 + ((176 * (32767 - bright)) >> 15);        /* 1 + 4 (1 - b) ms */
    else
        len = 88;                                           /* 2 ms */
    int32_t step = (int32_t)udiv(32767u << 8, (uint32_t)len);
    int plain = ex == PLUCK;
    int32_t key = (plain << 30) | (len << 16) | c;
    if (key != v->gkey) {
    /* dry run: energy of the burst, sum of (x >> 4)^2 (cached: rain and repeated notes skip it) */
    uint32_t sd = 0x9E3779B9u, e = 0;                       /* same burst every strike */
    int32_t lp = 0, env = 32767 << 8;
    for (int n = 0; n < len; n++) {
        int32_t w = noise_q15(&sd);
        lp += ((w - lp) * c) >> 15;
        int32_t x = ((plain ? w : lp) * (env >> 8)) >> 15;
        e += (uint32_t)((x >> 4) * (x >> 4));
        env -= step;
    }
    uint32_t rms = isqrt(udiv(e, (uint32_t)len)) << 4;      /* Q15 */
    int32_t target = plain ? 16384 : 9175;                  /* 0.5 : 0.28 */
    v->gval = rms ? (int32_t)udiv((uint32_t)target << 12, rms) : 4096;
    v->gkey = key;
    }
    v->bgain = v->gval;
    v->brng = 0x9E3779B9u;
    v->blp = 0;
    v->benv = 32767 << 8;
    v->bstep = step;
    v->bplain = plain;
    v->exc_left = len;
    v->exc_amp = amount;
}

/* Next burst sample, Q13 (gain applied). */
static inline int32_t burst_next(struct vram *v, int32_t c)
{
    int32_t w = noise_q15(&v->brng);
    v->blp += ((w - v->blp) * c) >> 15;
    int32_t x = ((v->bplain ? w : v->blp) * (v->benv >> 8)) >> 15;
    v->benv -= v->bstep;
    v->exc_left--;
    return (x * v->bgain) >> 14;                            /* Q15 * Q12 -> Q13 */
}

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct state *s = st;
    struct vram *v = voice_of(st);
    int32_t mat = clamp_knob(params[0]) * 7;
    int ex = (clamp_knob(params[1]) * 5) >> 15;
    int32_t ring = clamp_knob(params[2]);
    int32_t bright = clamp_knob(params[3]);
    if (ex > 4)
        ex = 4;
    int i0 = mat >> 15;
    int32_t t = mat & 0x7FFF;
    if (i0 > 6) {
        i0 = 6;
        t = 0x7FFF;
    }

    /* Burst / breath low-pass: c = 1 - exp(-2 pi (200 + 14000 b^2) / fs). */
    int32_t hz = 200 + ((14000 * ((bright * bright) >> 15)) >> 15);
    int32_t c = 32767 - (exp2neg((hz * 862) >> 6) >> 16);

    if (s->note_on == 0x80000000u) {          /* TE note-on */
        s->note_on = 0;
        if (v->rng == 0)
            v->rng = 0x9E3779B9u;
        v->breath_env = 0;
        v->bow_env = 0;
        v->rain_next = 0;
        strike(v, ex, bright, c, 32767);
    }

    /* Mode coefficients. x0 = 6.91 / (T60 fs), T60 = 0.05 * 400^ring; x_k = x0 (1 + 0.035 k). */
    uint32_t inc0 = (uint32_t)pitch[0];
    int32_t x0 = ring > 31129 ? 0 : qmul(6730216, exp2neg((ring * 8851) >> 9));   /* 0.003134 Q31 */
    int32_t tilt = 0x7FFFFFFF, tq = exp2neg(((32767 - bright) * 9455) >> 15);       /* exp(-0.1 (1 - b)) */
    int32_t a1[MODES], a2[MODES], g[MODES];
    for (int k = 0; k < MODES; k++, tilt = qmul(tilt, tq)) {
        a1[k] = a2[k] = g[k] = 0;                                        /* inactive unless set below */
        int32_t l0 = modal_log2r[i0][k], l1 = modal_log2r[i0 + 1][k];
        int32_t L = l0 + (((l1 - l0) >> 4) * t >> 11);                 /* Q16 */
        int sh = 2 + (L >> 16);
        if (sh >= 31) {
            v->y1[k] = v->y2[k] = 0;
            continue;
        }
        uint32_t e2 = (uint32_t)exp2neg(65536 - (L & 0xFFFF));           /* 2^frac, Q30 */
        uint32_t q = (uint32_t)qmul((int32_t)(inc0 >> 1), (int32_t)e2);  /* inc0 2^frac / 4 */
        uint32_t inc = q << sh;
        if (q > (1932735283u >> sh) || inc < 1947800u) {                 /* >= 0.45 fs or < 20 Hz */
            v->y1[k] = v->y2[k] = 0;
            continue;
        }
        /* sin(w/2) from the Q30 table at phase inc / 2; cos w = 1 - 2 sin^2(w/2). */
        uint32_t ph = inc >> 1;
        int idx = (int)(ph >> 22);
        int32_t fr = (int32_t)((ph >> 7) & 0x7FFF);
        int32_t sn = sin_q30[idx] + ((((sin_q30[idx + 1] - sin_q30[idx]) >> 8) * fr) >> 7);
        int32_t cw = (1 << 30) - (qmul(sn, sn) << 2);                   /* Q30 */
        int32_t xk = x0 + qmul(x0, (int32_t)(75161928u * (uint32_t)k)); /* 0.035 k, Q31 */
        if (xk < 2147)
            xk = 2147;                                                   /* r <= 1 - 1e-6 (krystal) */
        int32_t r = 0x7FFFFFFF - xk + (qmul(xk, xk) >> 1);               /* exp(-x), Q31 */
        a1[k] = qmul(r, cw) << 1;
        a2[k] = -qmul(r, r);
        g[k] = qmul(qmul(xk, modal_weight[k]) << 7, tilt);
    }

    /* Exciter drive: 0.8 velocity * (mallet 1.2 : 2), Q14. */
    int32_t drive = ex == MALLET ? 15729 : 26214;
    int32_t amt = (drive * v->exc_amp) >> 15;
    int32_t pressure = (14746 * (8192 + ((24576 * bright) >> 15))) >> 14;          /* 0.9 (0.5 + 1.5 b), Q14 */
    /* body-velocity coupling: krystal kBowCoupling = 3 with ~50 active modes; 8 here for 20 modes (6 still
     * clipped on low, bright, short-ring notes). bowk = 8 fs / (2 pi f0) in Q8 = 2^43 / (2 pi inc). */
    int32_t bowk = inc0 >= 512 ? (int32_t)udiv(2734261100u, inc0 >> 9) : 0;
    int32_t peak = 0;
    int32_t xin[ENGINE_BLOCK], sum[ENGINE_BLOCK];

    if (ex != BOW) {
        /* Exciter for the whole block first, then each mode over the block with its state and coefficients
         * in registers (mode-outer: ~3x cheaper than sample-outer). */
        int any = 0;
        for (int i = 0; i < ENGINE_BLOCK; i++) {
            int32_t x = 0;                                               /* Q24 */
            if (v->exc_left > 0)
                x = (burst_next(v, c) * amt) >> 3;                       /* Q13 * Q14 -> Q24 */
            if (ex == BREATH) {
                if (v->breath_env < (1 << 24))
                    v->breath_env += 3355;                               /* 0.0002 per sample */
                v->lp += ((noise_q15(&v->rng) - v->lp) * c) >> 15;
                x += ((v->lp * (v->breath_env >> 9)) >> 15) * 2097 >> 6; /* * 0.8 * 0.08, Q15 -> Q24 */
            } else if (ex == RAIN && --v->rain_next <= 0) {
                uint32_t u1 = xorshift(&v->rng) >> 17, u2 = xorshift(&v->rng) >> 17;   /* Q15 */
                strike(v, RAIN, bright, c, (19661 * (6554 + ((26214 * ((u1 * u2) >> 15)) >> 15))) >> 15);
                amt = (drive * v->exc_amp) >> 15;
                int32_t rate = 4 + ((40 * bright) >> 15);
                v->rain_next = (int32_t)((udiv(44100u, (uint32_t)rate) *
                                          (9830 + ((45875 * (xorshift(&v->rng) >> 17)) >> 15))) >> 15);
            }
            xin[i] = x;
            any |= x;
        }
        run_modes(v, a1, a2, g, sum, any ? xin : 0, ENGINE_BLOCK);
    } else {
        /* Bow: friction depends on the body velocity (from the summed output), recomputed every chunk. */
        for (int i0 = 0; i0 < ENGINE_BLOCK; i0 += BOW_CHUNK) {
            int32_t vb = ((v->ysum - v->ysum_prev) >> 8) * bowk;         /* body velocity, Q24 */
            int32_t dv = 8724152 - vb;                                   /* 0.4 (0.5 + 0.8) = 0.52 */
            int32_t d12 = dv >> 12, fr = 0;
            if (d12 > -46341 && d12 < 46341) {
                int32_t u = d12 * d12;                                   /* dv^2, Q24 */
                int32_t e = exp2neg((int32_t)(((uint32_t)u >> 6) * 18)); /* exp(-50 dv^2) = 2^(-72.1 dv^2) */
                fr = qmul(qmul(dv, e), pressure << 16) << 1;             /* dv exp(..) pressure, Q24 */
            }
            for (int j = i0; j < i0 + BOW_CHUNK; j++) {
                int32_t x = 0;
                if (v->exc_left > 0)
                    x = (burst_next(v, c) * amt) >> 3;
                if (v->bow_env < 32767)
                    v->bow_env += 16;                                    /* 0.0005 per sample */
                xin[j] = x + qmul(fr, v->bow_env << 16);
            }
            run_modes(v, a1, a2, g, sum + i0, xin + i0, BOW_CHUNK);
            v->ysum_prev = sum[i0 + BOW_CHUNK - 2];
            v->ysum = sum[i0 + BOW_CHUNK - 1];
        }
    }

    for (int i = 0; i < ENGINE_BLOCK; i++) {
        int16_t o = soft_clip(qmul(sum[i] >> 9, 1503238554));           /* krystal tanh(0.7 sum): Q24 -> Q15 */
        out[i] = o;
        int32_t a = o < 0 ? -o : o;
        if (a > peak)
            peak = a;
    }
    v->peak = peak;
}
