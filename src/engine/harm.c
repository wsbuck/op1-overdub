/* Harmonic engine: plays the single-cycle table the screen builds from the drawn harmonics (notes/harmonic-synth.md).
 *
 * The UI thread (src/slot/ui_harm.h) unpacks the sound's stored knobs (harm.h), builds a band-limited frame
 * (harm_build.h) into one of 3 buffers in harm_ram and publishes it with unison and warp. The engine reads nothing from
 * params (TE's LFO would scramble the packed values) and plays:
 *   - the published frame with wt.c's oscillator core (wt_osc.h): 9 band-limited levels chosen from the block's pitch
 *     (raised by warp's speed-up), 8x oversampled linear interpolation, crossfaded between adjacent levels;
 *   - warp: wt.c's smooth phase bend (warp.h); unison: a second oscillator fading in, detuned up to +30 cents;
 *   - a crossfade from the frame a voice was playing to a newly published one over HARM_FADE samples (8 blocks,
 *     11.6 ms at 88.2 kHz), so edits never click. A voice fades only from a frame that is still intact (the sine, or
 *     the one published just before front: the UI never rebuilds those two); otherwise it switches at once.
 *   - before the first build (front = 0): a sine (sine256, the same at every level).
 * Then a ~10 Hz DC blocker (warp makes the halves of the cycle unequal) and the soft knee at -6 dB, as wt.c.
 * State: phases at bytes 0/8 and the DC blocker at 16/20 (reset by TE's note-on, engine.h); the voice's frame and
 * fade at bytes 4..7, which TE never writes (tagged, so a stale value from another engine is ignored).
 */
#include "engine.h"
#include "dsp.h"
#include "warp.h"
#include "wt_osc.h"
#include "harm.h"

#define TAG 0x4Au
#define DC_R 2145953825                         /* 1 - 2 pi 10 / 88200, Q31 */

struct state {
    uint32_t phase1;    /*  0: reset to 0x80000000 on note-on */
    uint32_t fade;      /*  4: TAG << 24 | samples left << 4 | prev << 2 | cur (frames 1..3, 0 = sine) */
    uint32_t phase2;    /*  8: reset to 0x80000000 on note-on */
    uint32_t spare3;
    int32_t dc_x;       /* 16: DC blocker, cleared on note-on */
    int32_t dc_y;       /* 20 */
    uint32_t spare6, spare7;
};

static inline void looks(struct wo_look *lk, const struct harm_ram *h, int f, int lvl, int lvl2)
{
    if (!f) {
        lk[0].t = sine256;
        lk[0].shift = 24;
        lk[0].mask = 255;
        lk[1] = lk[0];
        return;
    }
    lk[0] = wo_level(h->frame[f - 1], lvl);
    lk[1] = wo_level(h->frame[f - 1], lvl2);
}

/* One oscillator: the voice's frame, or (fading) the old frame blended into it by ff (Q15). */
static inline __attribute__((always_inline)) int32_t sample(const struct wo_look *lk, const struct wo_look *lp,
                                                            int fading, int32_t ff, int32_t w, uint32_t ph)
{
    int32_t b = wo_osc(lk, w, ph);
    if (fading) {
        int32_t a = wo_osc(lp, w, ph);
        b = a + (((b - a) * ff) >> 15);
    }
    return b;
}

struct run {
    const struct wo_look *lk, *lp;
    int32_t w, ff, g2, det12, norm, warp;
    struct warp wp;
};

static inline __attribute__((always_inline)) void loop(struct state *s, int16_t *out, const int32_t *pitch,
                                                       const struct run *r, int fading)
{
    uint32_t p1 = s->phase1, p2 = s->phase2;
    int32_t dc_x = s->dc_x, dc_y = s->dc_y, ff = r->ff;
    const int32_t g2 = r->g2, w = r->w;
    for (int i = 0; i < ENGINE_BLOCK; i++) {
        uint32_t inc = (uint32_t)pitch[i];
        int32_t v = sample(r->lk, r->lp, fading, ff, w, r->warp ? warp_phase(&r->wp, p1) : p1);
        if (g2) {
            v += (sample(r->lk, r->lp, fading, ff, w, r->warp ? warp_phase(&r->wp, p2) : p2) * g2) >> 15;
            p2 += inc + ((inc >> 10) * (uint32_t)r->det12 >> 2);
        }
        int32_t x = qmul(r->norm, v << 9);              /* Q15 -> Q24, unison-normalised */
        dc_y = x - dc_x + qmul(DC_R, dc_y);
        dc_x = x;
        out[i] = soft_knee(dc_y >> 10);                 /* Q24 -> Q15 at -6 dB */
        p1 += inc;
        if (fading)
            ff += 32768 / HARM_FADE;
    }
    s->phase1 = p1;
    s->phase2 = p2;
    s->dc_x = dc_x;
    s->dc_y = dc_y;
}

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct state *s = st;
    const struct harm_ram *h = &harm_ram;
    (void)params;

    int f = h->front, pv = h->prev;
    if ((uint32_t)f > HARM_FRAMES)
        f = 0;
    int32_t uni = h->uni, warp_knob = h->warp;
    uni = uni < 0 ? 0 : uni > 32767 ? 32767 : uni;
    warp_knob = warp_knob < 0 ? 0 : warp_knob > 32767 ? 32767 : warp_knob;

    /* the voice's frame and fade */
    uint32_t vw = s->fade;
    int cur = f, prev = f, left = 0;
    if ((vw >> 24) == TAG) {
        cur = vw & 3;
        prev = (vw >> 2) & 3;
        left = (vw >> 4) & 0x7FF;
    }
    if (cur != f) {
        left = cur == 0 || cur == pv ? HARM_FADE : 0;
        prev = cur;
        cur = f;
    }

    struct run r;
    /* unison: osc 2 gain g2 (Q15) fades in over the first quarter, detune up to +30 cents (Q12), as wt.c */
    r.g2 = uni >= 8192 ? 32767 : uni << 2;
    r.det12 = (((uni * uni) >> 15) * 72) >> 15;
    r.norm = 0x7FFFFFFF - (r.g2 << 15);                 /* ~1 / (1 + g2), Q31 */
    r.warp = warp_knob;
    r.wp = warp_setup(warp_knob);
    int lvl = wo_pick_level((uint32_t)pitch[0], warp_knob ? r.wp.level_q8 : 0, &r.w);
    int lvl2 = lvl + 1 < LEVELS ? lvl + 1 : lvl;
    struct wo_look lk[2], lp[2];
    looks(lk, h, cur, lvl, lvl2);
    r.lk = lk;
    r.lp = lp;
    if (left) {
        looks(lp, h, prev, lvl, lvl2);
        r.ff = (HARM_FADE - left) * (32768 / HARM_FADE);
        loop(s, out, pitch, &r, 1);
        left -= ENGINE_BLOCK;
        if (left < 0)
            left = 0;
    } else {
        r.ff = 0;
        loop(s, out, pitch, &r, 0);
    }
    s->fade = TAG << 24 | (uint32_t)left << 4 | (uint32_t)prev << 2 | (uint32_t)cur;
}
