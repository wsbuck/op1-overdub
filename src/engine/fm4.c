/* fm4: 4-operator phase-modulation synth, Ableton Operator style (notes/fm-operator.md; knobs and packing: fm4.h).
 *
 * Per sample, operators D, C, B, A: ph += inc x ratio; s = sin(ph + sum of its modulators' outputs [+ feedback on D]).
 * A modulator's output is s x its depth in phase units (uint32 arithmetic: the phase wraps, so any depth is exact);
 * carriers are summed (level x envelope / number of carriers), then a ~10 Hz DC blocker and the soft knee at -6 dB.
 * One inner loop per algorithm (the routing is a compile-time constant in each: switch outside the loop).
 *
 * Per block (128 samples):
 *   - operator increments from pitch[0] and pitch[127], ramped linearly (exact ratios at a steady pitch);
 *   - one envelope per operator, exponential towards its shape's target, time constant x TIME; the gains are ramped
 *     linearly across the block, so depth changes never zipper. Note-on (TE's 0x80000000 at state bytes 0..3)
 *     restarts phases, envelopes and feedback. Amp attack/release stay TE's (T2).
 *   - the "Tone" limiter (aliasing): bandwidth per operator, bottom-up through the algorithm: f_op + the sum over
 *     its modulators of (I + 3) x their bandwidth (Carson's I + 1 plus two more sidebands, which still reach -26 dB at
 *     small I; 4 I below 1 rad), D's own 1 + 6 x feedback rad. If a carrier's would pass 0.68 fs (60 kHz), every
 *     index and the feedback are scaled down (bisection, 8 steps) until it doesn't: what folds over then lands above
 *     28 kHz, which TE's 2:1 decimation removes. An operator above 0.5 fs (its own sine would alias) fades out from
 *     0.4375 fs.
 * State: TE's 32 B hold only the note-on flag; the rest is in a 4 KB voice_ram slot (OWNER_FM4, as grain/modal;
 * struct fm4_voice in fm4.h: the screen reads the envelopes of the voices that rendered since its last frame).
 */
#include "engine.h"
#include "dsp.h"
#include "sine_table.h"
#include "modal_state.h"
#include "fm4.h"

extern uint8_t voice_ram[];

#define DC_R 2145953825                     /* 1 - 2 pi 10 / 88200, Q31 */
#define RAD 20861                           /* phase units per radian per Q15 sample value: 2^32 / 2 pi / 32768 */
#define IMAX (8 * RAD)                      /* level 15 x INDEX 2x: 8 rad */
#define FBMAX 14603                         /* 1.4 rad, per (s1 + s2): 1.4 x RAD / 2 */
#define LIMIT 713032                        /* 0.68 fs in Q20 */
#define E_BASE 1413703                      /* log2(2^31 x 128 / 88200), Q16 */

struct state {                              /* TE voice object +4 .. +0x23 */
    uint32_t note_on;                       /* 0x80000000 after TE's note-on */
    uint32_t spare[7];
};

struct blk {
    uint32_t inc[FM4_OPS], dinc[FM4_OPS];
    int32_t dg[FM4_OPS], dfbg;
};

static struct fm4_voice *voice_of(void *st)
{
    uint32_t off = (uint32_t)st - 0xFFB00004u;      /* TE voice i: 0xFFB00000 + i * 0x24, state at +4 */
    uint32_t i = (off * 1821u) >> 16;
    if (off > 5 * 0x24 || i * 0x24 != off)
        i = 7;                                      /* not a TE voice (simulator) */
    return voice_slot_claim(voice_ram + i * VOICE_RAM_BYTES, OWNER_FM4, sizeof(struct fm4_voice));
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

/* 2^(y / 65536) for 18 <= y / 65536 < 31 */
static inline int32_t exp2_hi(int32_t y)
{
    static const uint16_t tab[33] = {
        32768, 33486, 34219, 34968, 35734, 36516, 37316, 38133, 38968, 39821, 40693, 41584, 42495, 43425, 44376,
        45348, 46341, 47356, 48393, 49452, 50535, 51642, 52773, 53928, 55109, 56316, 57549, 58809, 60097, 61413,
        62757, 64132, 65535,
    };
    int n = y >> 16;
    uint32_t f = (uint32_t)y & 0xFFFF, i = f >> 11, fr = f & 0x7FF;
    uint32_t m = tab[i] + (((tab[i + 1] - tab[i]) * fr) >> 11);
    return (int32_t)(m << (n - 15));
}

/* fm4_mods / fm4_car as constant expressions (one nibble per operator, A lowest), so the routing folds away in run() */
#define MODS(a) ((a) == 0 ? 0x0842 : (a) == 1 ? 0x00C2 : (a) == 2 ? 0x004A : (a) == 3 ? 0x0806 : (a) == 4 ? 0x0802 : \
                 (a) == 5 ? 0x000E : (a) == 6 ? 0x0840 : (a) == 7 ? 0x0844 : (a) == 8 ? 0x0888 : (a) == 9 ? 0x0800 : 0)
#define CARS(a) ((a) == 4 ? 5 : (a) == 6 || (a) == 7 ? 3 : (a) == 8 || (a) == 9 ? 7 : (a) == 10 ? 15 : 1)

/* The inner loop for algorithm ALG (a constant at every call). */
static inline __attribute__((always_inline)) void run(const int ALG, struct fm4_voice *v, const struct blk *b,
                                                      int16_t *out)
{
    const unsigned mA = MODS(ALG) & 15, mB = MODS(ALG) >> 4 & 15, mC = MODS(ALG) >> 8 & 15, car = CARS(ALG);
    uint32_t pa = v->ph[0], pb = v->ph[1], pc = v->ph[2], pd = v->ph[3];
    uint32_t ia = b->inc[0], ib = b->inc[1], ic = b->inc[2], id = b->inc[3];
    int32_t ga = v->g[0], gb = v->g[1], gc = v->g[2], gd = v->g[3], fbg = v->fbg;
    int32_t s1 = v->s1, s2 = v->s2, dc_x = v->dc_x, dc_y = v->dc_y;
    for (int i = 0; i < ENGINE_BLOCK; i++) {
        int32_t acc = 0;
        int32_t sd = fsin(pd + (uint32_t)((s1 + s2) * fbg));
        s2 = s1;
        s1 = sd;
        uint32_t yd = (uint32_t)sd * (uint32_t)gd;
        if (car & 8)
            acc += sd * gd;
        uint32_t m = (mC & 8 ? yd : 0);
        int32_t sc = fsin(pc + m);
        uint32_t yc = (uint32_t)sc * (uint32_t)gc;
        if (car & 4)
            acc += sc * gc;
        m = (mB & 4 ? yc : 0) + (mB & 8 ? yd : 0);
        int32_t sb = fsin(pb + m);
        uint32_t yb = (uint32_t)sb * (uint32_t)gb;
        if (car & 2)
            acc += sb * gb;
        m = (mA & 2 ? yb : 0) + (mA & 4 ? yc : 0) + (mA & 8 ? yd : 0);
        acc += fsin(pa + m) * ga;
        int32_t x = acc >> 6;                       /* Q30 -> Q24 */
        dc_y = x - dc_x + qmul(DC_R, dc_y);
        dc_x = x;
        out[i] = soft_knee(dc_y >> 10);             /* Q24 -> Q15 at -6 dB */
        pa += ia;
        pb += ib;
        pc += ic;
        pd += id;
        ia += b->dinc[0];
        ib += b->dinc[1];
        ic += b->dinc[2];
        id += b->dinc[3];
        ga += b->dg[0];
        gb += b->dg[1];
        gc += b->dg[2];
        gd += b->dg[3];
        fbg += b->dfbg;
    }
    v->ph[0] = pa;
    v->ph[1] = pb;
    v->ph[2] = pc;
    v->ph[3] = pd;
    v->s1 = s1;
    v->s2 = s2;
    v->dc_x = dc_x;
    v->dc_y = dc_y;
}

/* Highest carrier bandwidth (Q20 of fs) with every index (Q8 rad) and the feedback scaled by sc (Q8). */
static int32_t bandwidth(int alg, const int32_t *f, const int32_t *iq8, int32_t fbq8, int32_t sc)
{
    int32_t bw[FM4_OPS], top = 0;
    for (int op = FM4_OPS - 1; op >= 0; op--) {
        int32_t w = f[op];
        if (op == FM4_OPS - 1)
            w += (int32_t)(((uint32_t)w >> 8) * (uint32_t)((6 * fbq8 * sc) >> 8));
        unsigned mods = fm4_mods[alg][op];
        for (int j = op + 1; j < FM4_OPS; j++)
            if (mods >> j & 1) {
                int32_t is = (iq8[j] * sc) >> 8;    /* (I + 3) f_m, easing to 4 I f_m below 1 rad: sidebands */
                int32_t ex = 3 * is < 768 ? 3 * is : 768;   /* past Carson's I + 1 still reach -26 dB at small I */
                w += (int32_t)((uint32_t)(is + ex) * (uint32_t)(bw[j] >> 4) >> 4);
            }
        bw[op] = w > (1 << 23) ? 1 << 23 : w;
        if ((fm4_car[alg] >> op & 1) && bw[op] > top)
            top = bw[op];
    }
    return top;
}

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct state *s = st;
    struct fm4_voice *v = voice_of(st);
    struct fm4_patch p;
    fm4_decode(params, &p);
    const int alg = p.alg;
    int fresh = s->note_on == 0x80000000u || !v->alg;
    s->note_on = 0;
    v->blocks++;                                    /* the screen: this voice rendered since its last frame */

    /* operator increments, ramped from pitch[0] to pitch[127] (inc x ratio mod 2^32, without a 64-bit multiply) */
    uint32_t i0 = (uint32_t)pitch[0], i1 = (uint32_t)pitch[ENGINE_BLOCK - 1];
    struct blk b;
    int32_t fq[FM4_OPS], mute[FM4_OPS];
    uint32_t fkey = i0 >> 12;                       /* Q20 of fs */
    if (fkey > 1 << 17)
        fkey = 1 << 17;
    for (int op = 0; op < FM4_OPS; op++) {
        uint32_t r = p.ratio[op], r6 = r >> 6;
        uint32_t a = (i0 >> 16) * r + (((i0 & 0xFFFF) * r6) >> 10);
        uint32_t e = (i1 >> 16) * r + (((i1 & 0xFFFF) * r6) >> 10);
        b.inc[op] = a;
        b.dinc[op] = (uint32_t)((int32_t)(e - a) >> 7);
        fq[op] = (int32_t)((fkey * (r >> 8)) >> 8);
        mute[op] = fq[op] >= 1 << 19 ? 0 : fq[op] <= (1 << 19) - (1 << 16) ? 32767 : ((1 << 19) - fq[op]) >> 1;
        if (!mute[op])
            fq[op] = 0;                             /* silent: no bandwidth */
    }

    /* envelopes: start on note-on, then one exponential step per block towards the target */
    if (fresh) {
        for (int op = 0; op < FM4_OPS; op++) {
            v->ph[op] = 0;
            v->env[op] = fm4_env_start[p.env[op]] << 15;
        }
        v->s1 = v->s2 = 0;
        v->dc_x = v->dc_y = 0;
    }
    int32_t env1[FM4_OPS];
    for (int op = 0; op < FM4_OPS; op++) {
        int sh = p.env[op];
        int32_t target = fm4_env_target[sh] << 15, e = v->env[op];
        if (sh) {
            int32_t d = exp2_hi(E_BASE - fm4_env_tau[sh] - p.time_q16);       /* 128 / (tau fs), Q31 */
            e -= qmul(e - target, d);
        }
        env1[op] = e;
    }

    /* depths: modulators in phase units, I = level x env x INDEX x 8 rad; carriers Q15, scaled by 1 / (sum of the
     * carriers' levels) when that passes 1, so one carrier alone plays at full level and four at 15 don't clip */
    unsigned car = fm4_car[alg];
    int32_t lv[FM4_OPS], lsum = 0;
    for (int op = 0; op < FM4_OPS; op++) {
        lv[op] = (fm4_level[p.level[op]] * mute[op]) >> 15;
        if (car >> op & 1)
            lsum += lv[op];
    }
    int32_t norm = lsum > 32767 ? (int32_t)udiv(32767u << 15, (uint32_t)lsum) : 32767;
    int32_t iq8[FM4_OPS], gend[FM4_OPS], gnow[FM4_OPS];
    for (int op = 0; op < FM4_OPS; op++) {
        int32_t a1 = (lv[op] * (env1[op] >> 15)) >> 15, a0 = (lv[op] * (v->env[op] >> 15)) >> 15;
        if (car >> op & 1) {
            gend[op] = (a1 * norm) >> 15;
            gnow[op] = (a0 * norm) >> 15;
            iq8[op] = 0;
        } else {
            int32_t d1 = (a1 * p.index) >> 15, d0 = (a0 * p.index) >> 15;
            iq8[op] = (d1 * 2048) >> 15;            /* 8 rad = 2048 in Q8 */
            gend[op] = (d1 * (IMAX >> 3)) >> 12;
            gnow[op] = (d0 * (IMAX >> 3)) >> 12;
        }
    }
    int32_t fb = (p.fb * mute[3]) >> 15;
    int32_t fbq8 = (fb * 358) >> 15;                /* 1.4 rad in Q8 */
    /* Tone: scale every index and the feedback until the widest carrier fits under LIMIT */
    int32_t sc = 256;
    if (bandwidth(alg, fq, iq8, fbq8, 256) > LIMIT) {
        int32_t lo = 0, hi = 256;
        for (int it = 0; it < 8; it++) {
            int32_t mid = (lo + hi) >> 1;
            if (bandwidth(alg, fq, iq8, fbq8, mid) > LIMIT)
                hi = mid;
            else
                lo = mid;
        }
        sc = lo;
    }
    int32_t fbend = (((fb * FBMAX) >> 15) * sc) >> 8;
    for (int op = 0; op < FM4_OPS; op++)
        if (!(car >> op & 1))
            gend[op] = (gend[op] >> 1) * sc >> 7;

    /* ramps; a note-on or an algorithm change starts from the new values (an operator may have changed role) */
    if (fresh || v->alg != alg + 1) {
        for (int op = 0; op < FM4_OPS; op++)
            v->g[op] = car >> op & 1 ? gnow[op] : (int32_t)((gnow[op] >> 1) * sc >> 7);
        v->fbg = fbend;
    }
    for (int op = 0; op < FM4_OPS; op++)
        b.dg[op] = (gend[op] - v->g[op]) >> 7;
    b.dfbg = (fbend - v->fbg) >> 7;

    switch (alg) {
    case 0: run(0, v, &b, out); break;
    case 1: run(1, v, &b, out); break;
    case 2: run(2, v, &b, out); break;
    case 3: run(3, v, &b, out); break;
    case 4: run(4, v, &b, out); break;
    case 5: run(5, v, &b, out); break;
    case 6: run(6, v, &b, out); break;
    case 7: run(7, v, &b, out); break;
    case 8: run(8, v, &b, out); break;
    case 9: run(9, v, &b, out); break;
    default: run(10, v, &b, out); break;
    }
    for (int op = 0; op < FM4_OPS; op++) {
        v->env[op] = env1[op];
        v->g[op] = gend[op];
    }
    v->fbg = fbend;
    v->alg = alg + 1;
}
