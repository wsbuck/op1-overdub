/* Guitar amp: plays TE's audio input (TE IN, 0xFF800CB4) through gate, drive, tone and cab. Design: notes/amp.md;
 * input research: notes/input.md. TE's note envelope still applies, so a held or latched note (SHIFT+T2 HOLD) opens
 * the amp; the presets use full sustain.
 *
 * Knobs (engine page, 0..0x7FFF, slewed 1024 per block):
 *   [0] blue    drive: pre-gain 0 .. +44 dB into a two-stage tube curve (clean -> crunch -> lead); also tightens the
 *               lows (high-pass 30 -> 160 Hz) and pushes the upper mids (+6 dB pre-emphasis); loudness compensated
 *   [1] green   tone: tilt, dark (+5 dB lows, -10 dB highs) .. flat (centre) .. bright
 *   [2] white   gate threshold: off at 0, then -84 .. -30 dBFS (opens above it, closes 6 dB below, 50 ms hold)
 *   [3] orange  cab: direct (no cab) -> 1x12 open back -> 2x12 -> 4x12 closed back
 *
 * Chain per block: TE IN (64 at 44.1 kHz) -> gate detector -> 2x half-band upsampler (24 taps, 88.2 kHz) -> gate gain ->
 * tight high-pass -> pre-emphasis -> drive -> tube (biased) -> 7 kHz interstage low-pass -> stage 2 -> low and high
 * shelf (tone) -> high-pass, mid bell, presence bell, two low-passes (cab) -> loudness -> limiter (-3 dBFS, gain from
 * the previous block's peak, 93 ms release) -> soft knee.
 *
 * One voice only plays the input (else a chord would stack copies of the guitar). Every voice calls render; the
 * shared state is amp_ram (amp.h). Per call:
 *   1. New TE block? TE IN differs from the last block's copy, or te_rx_sel toggled, or this voice rendered already in
 *      the current block while the input is digital silence. Then blk++ and the block is copied (and into the tuner's
 *      the output y[] is computed once per block, by the first voice that needs it.
 *   2. Owner: a voice takes over when it has a fresh note-on (TE's 0x80000000 in state bytes 0..3: newest note wins),
 *      when there is no owner, when the owner skipped a whole block (it stopped rendering: release ended or stolen),
 *      or when the owner is released and this voice isn't (iter's note-off sets state byte 28; Hypothesis).
 *   3. Each voice's gain (table in amp_ram) moves towards 1 (owner) or 0 by 1/8 per block, ramped per sample: a
 *      handover is an 11.6 ms crossfade of the same y[], so the level stays put. Non-owners at gain 0 output silence
 *      (the DSP runs once per block whoever renders first). A voice that skipped a block, or has a fresh note-on, fades in from 0.
 *
 * Fixed point: audio Q24 (1.0 = full scale), the shaper in Q20, coefficients Q31 / Q28 / Q29 (amp_tables.h, from
 * gen_amp_tables.py), qmul() for 32 x 32. No division operator (udiv() / recip_q31() loops, per block only).
 * State bytes: 0..3 note-on mark (TE writes 0x80000000), 4..7 the block of this voice's last render, 28 iter's
 * released flag; the rest is TE's (note-on writes 8..11, 16..27).
 */
#include "engine.h"
#include "dsp.h"
#include "inprobe.h"
#include "amp.h"
#include "amp_tables.h"

struct voice {
    uint32_t mark;                          /*  0: 0x80000000 after TE's note-on; we write 0 */
    uint32_t stamp;                         /*  4: amp_ram.blk at this voice's last render */
    uint32_t te8, te12, te16, te20, te24;   /*  8..27: TE's (note-on) */
    uint8_t released;                       /* 28: iter's note-off writes 1 (Hypothesis); cleared at our note-on */
    uint8_t pad[3];
};

#define HOLD 2205                           /* gate hold: 50 ms at 44.1 kHz */
#define LIM_T 11877049                      /* limiter threshold: -3 dBFS, Q24 */
#define SLEW 1024                           /* knob units per block */

/* One row of a knob table, interpolated: AMP_N points at 0, 1024, .. 32768 (k 0..32767). */
static void knob_row(const int32_t *t, int w, int32_t k, int32_t *row)
{
    int32_t i = k >> 10, f = k & 1023;
    const int32_t *a = t + i * w, *b = a + w;
    for (int j = 0; j < w; j++) {
        int32_t d = b[j] - a[j];
        row[j] = a[j] + (d >> 10) * f + (((d & 1023) * f) >> 10);
    }
}

static void set_svf(struct amp_svf *f, const int32_t *c)
{
    struct svf s = svf_coefs(c[0], c[1]);
    f->a1 = s.a1;
    f->a2 = s.a2;
    f->a3 = s.a3;
    f->m0 = c[2];
    f->m1 = c[3];
    f->m2 = c[4];
}

/* (a * b) >> 31 without the low x low partial product (at most 1 LSB off): 4 instructions instead of the builtin's 6.
 * Not saturating: |a|, |b| < 2^31 and never both -2^31 here. */
#ifdef __bfin__
static inline int32_t fmul(int32_t a, int32_t b)
{
    int32_t r;
    __asm__("A1 = %1.H * %2.L (M), A0 = %1.H * %2.H;\n\tA1 += %2.H * %1.L (M);\n\tA1 = A1 >>> 15;\n\t%0 = (A0 += A1);"
            : "=d"(r) : "d"(a), "d"(b) : "A0", "A1");
    return r;
}
#else
#define fmul(a, b) qmul((a), (b))
#endif

/* Simper SVF over the block in place, state and coefficients in registers. The outputs (Q29 mix: products are Q22,
 * << 2): SHELF v0 + m1 band + m2 low (both shelves: the high shelf's m0 = A^2 is divided out here and multiplied
 * into the loudness), HP v0 + m1 band - low, BELL v0 + m1 band, LOW the low output. */
enum { SHELF, HP, BELL, LOW };
static void svf_block(struct amp_svf *f, int32_t *z, int kind)
{
    int32_t ic1 = f->ic1, ic2 = f->ic2;
    const int32_t a1 = f->a1, a2 = f->a2, a3 = f->a3, m1 = f->m1, m2 = f->m2;
#define SVF_CORE                                                    \
        int32_t v0 = z[n], v3 = v0 - ic2;                           \
        int32_t v1 = fmul(a1, ic1) + fmul(a2, v3);                  \
        int32_t v2 = ic2 + fmul(a2, ic1) + fmul(a3, v3);            \
        ic1 = 2 * v1 - ic1;                                         \
        ic2 = 2 * v2 - ic2;
    if (kind == SHELF)
        for (int n = 0; n < AMP_OUT; n++) {
            SVF_CORE
            z[n] = v0 + ((fmul(v1, m1) + fmul(v2, m2)) << 2);
        }
    else if (kind == HP)
        for (int n = 0; n < AMP_OUT; n++) {
            SVF_CORE
            z[n] = v0 + (fmul(v1, m1) << 2) - v2;
        }
    else if (kind == BELL)
        for (int n = 0; n < AMP_OUT; n++) {
            SVF_CORE
            z[n] = v0 + (fmul(v1, m1) << 2);
        }
    else
        for (int n = 0; n < AMP_OUT; n++) {
            SVF_CORE
            z[n] = v2;
        }
#undef SVF_CORE
    f->ic1 = ic1;
    f->ic2 = ic2;
}

static void amp_init(struct amp_ram *g, const int16_t *params)
{
    uint32_t *w = (uint32_t *)g;
    for (uint32_t i = 0; i < sizeof *g / 4; i++)
        w[i] = 0;
    for (int j = 0; j < 4; j++)
        g->knob[j] = amp_knob(params[j]);
    g->blk = 1;
    g->lim_gain = 32767;
    g->magic = AMP_MAGIC;
}

/* The block's output y[] from in[] (the DSP; run once per block). */
static void amp_dsp(struct amp_ram *g, const int16_t *params)
{
    /* knobs, slewed */
    for (int j = 0; j < 4; j++) {
        int32_t t = amp_knob(params[j]), k = g->knob[j];
        k = t > k + SLEW ? k + SLEW : t < k - SLEW ? k - SLEW : t;
        g->knob[j] = k;
    }
    struct amp_svf *f = g->f;                                       /* in place: a copy would cost 224 B of stack */
    int32_t row[AMP_ORANGE_W], thr = 0;
    knob_row(amp_green, AMP_GREEN_W, g->knob[1], row);
    set_svf(&f[F_LOW], row);
    set_svf(&f[F_HIGH], row + 5);                                   /* m1, m2 already divided by m0 = A^2 */
    const int32_t hs_a2 = row[7];                                   /* the high shelf's A^2, Q29 */
    knob_row(amp_orange, AMP_ORANGE_W, g->knob[3], row);
    for (int j = 0; j < 5; j++)
        set_svf(&f[F_HP + j], row + 5 * j);
    if (g->knob[2])
        knob_row(amp_gate_thr, 1, g->knob[2], &thr);
    knob_row(amp_blue, AMP_BLUE_W, g->knob[0], row);
    const int32_t gain = row[0], tight_a = row[1], emph = row[2] << 15, bias = row[3], k2 = row[4];
    /* loudness x the high shelf's A^2, Q27 (<= 4.3 x 3.2); row[5] (Q12, up to 17675) << 16 so it can't wrap */
    const int32_t comp = fmul(row[5] << 16, hs_a2) << 1;
    /* drive gain (per input sample) and make-up (per output sample) ramp linearly from the last block's values, so a
     * knob step isn't a level step at the block edge */
    if (!g->dsp_blocks) {
        g->gain_prev = gain;
        g->comp_prev = comp;
    }
    const int32_t dgain = (gain - g->gain_prev) >> 6, dcomp = (comp - g->comp_prev) >> 7;
    int32_t gk = g->gain_prev, ck = g->comp_prev;
    const int32_t tb = amp_tube(bias);

    int16_t *b = g->buf;                                            /* 24 samples of history, then the block */

    int32_t env = g->env, hold = g->hold, open = g->gate_open, gg = g->gate_gain;
    int32_t thr_c = thr >> 1;
    int32_t tl = g->tight, el = g->emph, il = g->inter;
    int32_t lg = g->lim_gain, peak = 0, in_peak = 0;
    /* limiter: the gain for this block from the last block's peak (out_peak, Q15) */
    int32_t lt = 32767, p = g->out_peak;
    if (p > (LIM_T >> 9))
        lt = (int32_t)udiv((uint32_t)(LIM_T >> 9) << 15, (uint32_t)p);
    int32_t lnew = lt < lg ? lt : lg + ((32767 - lg) >> 6);
    if (lnew > lt)
        lnew = lt;
    int32_t ldelta = lnew - lg;
    int16_t *y = g->y;
    int32_t *zb = g->z;

    for (int i = 0; i < AMP_IN; i++) {
        /* gate detector on the raw input (it runs AMP_HB_K samples ahead of the delayed audio: a little lookahead) */
        int32_t x = b[AMP_HIST + i], ax = x < 0 ? -x : x;
        in_peak = ax > in_peak ? ax : in_peak;
        ax <<= 9;                                                   /* Q24 */
        env -= env >> 9;                                            /* 11.6 ms decay */
        env = ax > env ? ax : env;
        if (!thr) {
            open = 1;
        } else if (env > thr << 9) {
            open = 1;
            hold = HOLD;
        } else if (env > thr_c << 9) {
            if (open)
                hold = HOLD;
        } else if (hold > 0) {
            hold--;
        } else {
            open = 0;
        }
        gg = open ? gg + (((1 << 30) - gg) >> 5) : gg - (gg >> 12); /* 0.7 ms attack, 93 ms release */

        /* 2x up: the even sample is the input delayed by AMP_HB_K, the odd one the half-band interpolation */
        const int16_t *c = b + i + AMP_HB_K;
        int32_t acc = 0;
        for (int k = 0; k < AMP_HB_K; k++)
            acc += amp_hb[k] * (c[-k] + c[1 + k]);
        for (int h = 0; h < 2; h++) {
            int32_t s = fmul(h ? acc >> 6 : c[0] << 9, gg) << 1;    /* Q24, gated */
            tl += fmul(tight_a, s - tl);                            /* tight */
            s -= tl;
            el += fmul(AMP_EMPH_A, s - el);                         /* pre-emphasis */
            s += fmul(s - el, emph) << 1;
            int32_t u = qmul(s << 4, gk);                           /* drive, Q20 */
            u = amp_lim(u, -32 * AMP_U1, 32 * AMP_U1);
            int32_t y1 = amp_tube(u + bias) - tb;                   /* stage 1 */
            il += fmul(AMP_INTER_A, y1 - il);                       /* interstage */
            zb[2 * i + h] = amp_stage2(((il >> 4) * k2) >> 8) << 4; /* stage 2, Q24 */
        }
        gk += dgain;
    }
    svf_block(&f[F_LOW], zb, SHELF);                                /* tone */
    svf_block(&f[F_HIGH], zb, SHELF);
    svf_block(&f[F_HP], zb, HP);                                    /* cab */
    svf_block(&f[F_MID], zb, BELL);
    svf_block(&f[F_PRES], zb, BELL);
    svf_block(&f[F_LP1], zb, LOW);
    svf_block(&f[F_LP2], zb, LOW);
    for (int n = 0; n < AMP_OUT; n++) {
        int32_t z = fmul(zb[n] << 4, ck);                           /* loudness */
        ck += dcomp;
        int32_t az = z < 0 ? -z : z;
        peak = az > peak ? az : peak;
        int32_t lgi = lg + ((ldelta * n) >> 7);
        y[n] = soft_knee(qmul(z, lgi << 16) >> 9);                  /* limiter, knee */
    }

    for (int i = 0; i < AMP_HIST; i++)
        b[i] = b[AMP_IN + i];
    g->env = env;
    g->hold = hold;
    g->gate_open = open;
    g->gate_gain = gg;
    g->tight = tl;
    g->emph = el;
    g->inter = il;
    g->gain_prev = gk;                                              /* = gain and comp within the ramps' truncation */
    g->comp_prev = ck;
    g->lim_gain = lnew;
    g->out_peak = peak >> 9;
    g->in_peak = in_peak;
    g->dsp_blocks++;
}

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct amp_ram *g = &amp_ram;
    struct voice *s = st;
    uint32_t me = (uint32_t)st;
    (void)pitch;
    if (g->magic != AMP_MAGIC)
        amp_init(g, params);
    g->renders++;

    /* 1. a new TE block? (TE writes TE IN before the voice loop; it doesn't change during the callback) */
    int16_t *in = g->buf + AMP_HIST;
    int diff = 0, zero = 1;
    for (int i = 0; i < AMP_IN; i++) {
        int16_t v = te_audio_in[i];
        diff |= v != in[i];
        zero &= v == 0;
    }
    uint32_t sel = te_rx_sel & 1;
    if (diff || sel != g->sel || (s->stamp == g->blk && zero)) {
        g->blk++;
        g->sel = sel;
        g->cur_owner = g->owner;                              /* gains follow the owner as of the block start */
        for (int i = 0; i < AMP_IN; i++)
            in[i] = te_audio_in[i];
    }
    uint32_t blk = g->blk;
    s->stamp = blk;

    /* 2. ownership */
    int fresh = s->mark == 0x80000000u;
    if (fresh) {
        s->mark = 0;
        s->released = 0;
    }
    uint32_t own = g->owner;
    if (own != me && (fresh || !own || g->owner_blk + 1 < blk || (((struct voice *)own)->released && !s->released))) {
        g->owner = me;
        g->takeovers++;
    }
    if (g->owner == me)
        g->owner_blk = blk;
    /* the gains step towards the owner as of this block's start, so a takeover in the middle of the voice loop moves
     * every voice's gain from the next block on: the gains always sum to one */
    int target = g->cur_owner == me;

    /* 3. the block's output: computed by the first voice to render in it (every block, so the DSP state runs on
     * through handovers) */
    if (g->y_blk != blk) {
        amp_dsp(g, params);
        g->y_blk = blk;
    }

    /* 4. this voice's gain: find its entry (or the stalest one, if it needs one) */
    int e = -1, old = 0;
    for (int j = 0; j < AMP_VOICES; j++) {
        if (g->v[j].id == me)
            e = j;
        if (g->v[j].blk < g->v[old].blk)
            old = j;
    }
    if (e < 0 && !target) {
        for (int i = 0; i < AMP_OUT; i++)
            out[i] = 0;
        return;
    }
    if (e < 0) {
        e = old;
        g->v[e].id = me;
        g->v[e].gain = 0;
        g->v[e].blk = 0;
    }
    int32_t g1 = g->v[e].gain;
    if ((fresh && g->cur_owner != me) || g->v[e].blk + 1 < blk)   /* new note, back after a gap: fade in */
        g1 = 0;
    int32_t g0 = g1;
    if (g->v[e].blk != blk) {                                 /* one step per block */
        g1 = target ? g1 + 32768 / AMP_FADE : g1 - 32768 / AMP_FADE;
        g1 = g1 > 32767 ? 32767 : g1 < 0 ? 0 : g1;
        g->v[e].gain = g1;
        g->v[e].blk = blk;
    }                                                         /* (rendering again in the same block: hold) */
    if (!g0 && !g1) {
        for (int i = 0; i < AMP_OUT; i++)
            out[i] = 0;
        return;
    }
    const int16_t *y = g->y;
    int32_t d = g1 - g0;
    for (int i = 0; i < AMP_OUT; i++)
        out[i] = (int16_t)((y[i] * (g0 + ((d * i) >> 7))) >> 15);
}
