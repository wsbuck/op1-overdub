/* Simulator driver for fx_lfo_check.py: our FX under a TE-style LFO, and what they publish for the screen (fxpub.h).
 *   fx_lfo FX MODE BLOCKS     FX 0 ensemble, 1 pingpong, 2 distortion, 3 crush, 4 repeat, 5 space; MODE m (mono) or s (stereo)
 * TE's LFO writes its modulated knobs into the per-block params copy the FX gets (0xFF801724), so here every block
 * gets a new params array: the stored knobs plus a triangle stepped once per block (each knob its own rate and phase,
 * full range, clamped 0 .. 32767 as TE's range row does); after block BLOCKS / 2 the params hold still.
 * After every call: <fx>_pub[instance] must hold exactly that block's params[0..3] and count the call (exit 1 if not).
 * stdout per block: "B p0 p1 p2 p3 u0 u1 u2 u3 c0 c1 c2 c3 peak" (u = published, c = crush_state's smoothed knobs or 0,
 * peak = the block's largest |output|). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "fxpub.h"
#include "crush.h"

volatile int32_t te_tempo_spb = 22050 << 14;    /* pingpong.c: TE's tempo word, 120 BPM */
volatile int32_t te_tape_state = 2, te_metro_count = 22050 << 14, te_play_slot = 0;   /* repeat.c: the tape plays */
volatile uint32_t te_tape_pos = 0;
/* repeat.c's manual key: TE's block counter and the drum / mixer controllers' press and release (stand-ins) */
volatile uint32_t te_blocks;
int te_drum_press(void *self, int key) { (void)self; (void)key; return 0; }
int te_drum_release(void *self, int key) { (void)self; (void)key; return 0; }
int te_master_press(void *self, int key) { (void)self; (void)key; return 0; }
int te_master_release(void *self, int key) { (void)self; (void)key; return 0; }
/* space.c: the slots and stored FX knobs TE keeps (its TAIL zone on synth / drum) */
volatile int32_t te_fx_slot[2];
volatile int16_t te_fx_knobs_synth[64], te_fx_knobs_drum[64];

#define DECL(f) void f##_mono(void *, int16_t *, const int16_t *); \
    void f##_stereo(void *, int16_t *, int16_t *, const int16_t *); extern struct fx_pub f##_pub[2];
DECL(ensemble) DECL(pingpong) DECL(distortion) DECL(crush) DECL(repeat) DECL(space)
typedef void (*mono_t)(void *, int16_t *, const int16_t *);
typedef void (*stereo_t)(void *, int16_t *, int16_t *, const int16_t *);
static const mono_t mono[6] = { ensemble_mono, pingpong_mono, distortion_mono, crush_mono, repeat_mono, space_mono };
static const stereo_t stereo[6] = { ensemble_stereo, pingpong_stereo, distortion_stereo, crush_stereo, repeat_stereo,
                                     space_stereo };
static struct fx_pub *const pubs[6] = { ensemble_pub, pingpong_pub, distortion_pub, crush_pub, repeat_pub, space_pub };
static const int16_t stored[6][4] = { { 15360, 21504, 0, 16384 }, { 28672, 14000, 12000, 10000 },
                                      { 16000, 16384, 7475, 32767 }, { 10240, 12288, 10240, 0 },
                                      { 12288, 5120, 9216, 16384 }, { 8192, 16384, 5120, 8192 } };   /* fx.json */

static int16_t sine(uint32_t ph)
{
    int32_t x = (int32_t)ph >> 16;
    return (int16_t)(((x * (32767 - (x < 0 ? -x : x))) >> 13) >> 1);
}

static int32_t tri(uint32_t ph)                 /* -16384 .. 16384 */
{
    int32_t x = (int32_t)(ph >> 16) - 32768;
    return (x < 0 ? -x : x) - 16384;
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    int fx = atoi(argv[1]), st = argv[2][0] == 's', blocks = atoi(argv[3]);
    static uint32_t self[64];
    int16_t l[64], r[64], p[8] = { 0 };
    uint32_t ph = 0, inc = 52000000u;            /* ~534 Hz */
    for (int b = 0; b < blocks; b++) {
        int moving = b < blocks / 2;
        for (int j = 0; j < 4; j++) {
            /* LFO rates 0.7 .. 2.8 Hz at 689 blocks a second: up to ~260 raw per block; depth +-16384 x 2 */
            uint32_t lph = (uint32_t)(moving ? b : blocks / 2) * (4200000u * (uint32_t)(j + 1)) + 0x40000000u * (uint32_t)j;
            int32_t v = stored[fx][j] + 2 * tri(lph);
            p[j] = (int16_t)(v < 0 ? 0 : v > 32767 ? 32767 : v);
        }
        for (int i = 0; i < 64; i++, ph += inc) {
            l[i] = sine(ph);
            r[i] = sine(ph + 0x40000000u);
        }
        te_tape_pos += 64 * 64;
        te_blocks++;
        uint32_t n0 = pubs[fx][st].blocks;
        if (st)
            stereo[fx](self, l, r, p);
        else
            mono[fx](self, l, p);
        const struct fx_pub *u = &pubs[fx][st];
        for (int j = 0; j < 4; j++)
            if (u->k[j] != p[j]) {
                fprintf(stderr, "block %d: published k%d %d != params %d\n", b, j, u->k[j], p[j]);
                return 1;
            }
        if (u->blocks != n0 + 1) {
            fprintf(stderr, "block %d: block count %u -> %u\n", b, (unsigned)n0, (unsigned)u->blocks);
            return 1;
        }
        if (pubs[fx][!st].blocks != 0) {
            fprintf(stderr, "block %d: the other instance published\n", b);
            return 1;
        }
        int peak = 0;
        for (int i = 0; i < 64; i++) {
            int a = l[i] < 0 ? -l[i] : l[i], c = r[i] < 0 ? -r[i] : r[i];
            peak = a > peak ? a : peak;
            if (st)
                peak = c > peak ? c : peak;
        }
        const struct crush *c = fx == 3 ? &crush_state[st] : 0;
        printf("B %d %d %d %d %d %d %d %d %d %d %d %d %d\n", p[0], p[1], p[2], p[3], u->k[0], u->k[1], u->k[2], u->k[3],
               c ? c->k[0] : 0, c ? c->k[1] : 0, c ? c->k[2] : 0, c ? c->k[3] : 0, peak);
    }
    return 0;
}
