/* Simulator (and host) driver for the space FX (space_check.py, the sound review's renders), with pingpong and repeat
 * linked for the shared lines (the simulator build; the host build gets stand-ins).
 *   space_main CTRL IN OUT STATE
 * CTRL: one line per 64-frame block: "F MODE K0 K1 K2 K3 STORED GAP KEY"
 *   F s = space, S = space without counting its cycles, p = pingpong, r = repeat, - = no FX this block (off: the
 *   input passes, TE's block counter still advances)
 *   MODE m (mono, the L channel) or s (stereo); K0..K3 the params (TE's per-block copy, after its LFO)
 *   STORED: the stored white knob TE keeps for the synth slot (-1: K2), for the TAIL zone on synth / drum
 *   GAP: blocks TE ran without calling the FX before this one (te_blocks advances by 1 + GAP)
 *   KEY: 1 = the green push held on this context's space screen (repeat.h rep_key, refreshed each block)
 * IN / OUT: int16 frames, L R interleaved (mono uses L; R passes through).
 * STATE: per block "B level t60 frz inf zone clearing duck owner0 owner1 cycles".
 * stderr: "cycles max <n> mean <n> calls <n>" over the space calls. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "space.h"
#include "repeat.h"
#include "pingpong.h"
#include "fxpub.h"

volatile uint32_t te_blocks;
volatile int32_t te_fx_slot[2];
volatile int16_t te_fx_knobs_synth[64], te_fx_knobs_drum[64];
#ifdef SPACE_HOST
int16_t pingpong_ram[3 * 65536];
uint32_t fx_line_owner[2];
#else
volatile int32_t te_tempo_spb = 22050 << 14, te_tape_state = 2, te_metro_count = 22050 << 14, te_play_slot = 0;
volatile uint32_t te_tape_pos = 0;
int te_drum_press(void *self, int key) { (void)self; (void)key; return 0; }
int te_drum_release(void *self, int key) { (void)self; (void)key; return 0; }
int te_master_press(void *self, int key) { (void)self; (void)key; return 0; }
int te_master_release(void *self, int key) { (void)self; (void)key; return 0; }
void repeat_mono(void *self, int16_t *buf, const int16_t *p);
void repeat_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);
void pingpong_mono(void *self, int16_t *buf, const int16_t *p);
void pingpong_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);
#endif
void space_mono(void *self, int16_t *buf, const int16_t *p);
void space_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);

#ifdef __bfin__
static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}
#else
static inline uint32_t cycles(void) { return 0; }
#endif

#ifdef SPACE_SHTEST
void space_shtest(int half, int voice, const int16_t *in, int16_t *out, int nblocks);
#endif

int main(int argc, char **argv)
{
#ifdef SPACE_SHTEST
    if (argc == 7) {                        /* space_main shtest HALF VOICE IN OUT NBLOCKS: the shifter alone */
        int half = atoi(argv[2]), voice = atoi(argv[3]), nb = atoi(argv[6]), n = 64 >> half;
        int16_t *x = calloc((size_t)(nb * n), 2), *y = calloc((size_t)(nb * n), 2);
        FILE *fi = fopen(argv[4], "rb"), *fo = fopen(argv[5], "wb");
        if (!x || !y || !fi || !fo || fread(x, 2, (size_t)(nb * n), fi) != (size_t)(nb * n))
            return 3;
        space_shtest(half, voice, x, y, nb);
        fwrite(y, 2, (size_t)(nb * n), fo);
        fclose(fo);
        return 0;
    }
#endif
    if (argc != 5)
        return 2;
    FILE *ctl = fopen(argv[1], "r"), *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb"), *st = fopen(argv[4], "w");
    if (!ctl || !in || !out || !st)
        return 3;
#ifdef __bfin__
    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* CCEN: run CYCLES */
#endif
    char fx, mode;
    int k[4], stored, gap, key;
    uint32_t maxc = 0, calls = 0;
    uint64_t sum = 0;
    static uint32_t self[64];
    while (fscanf(ctl, " %c %c %d %d %d %d %d %d %d", &fx, &mode, &k[0], &k[1], &k[2], &k[3], &stored, &gap, &key)
           == 9) {
        int16_t fr[128], l[64], r[64], p[8] = { 0 };
        if (fread(fr, 2, 128, in) != 128)
            break;
        for (int i = 0; i < 64; i++)
            l[i] = fr[2 * i], r[i] = fr[2 * i + 1];
        for (int i = 0; i < 4; i++)
            p[i] = (int16_t)k[i];
        te_blocks += 1 + (uint32_t)gap;
        te_fx_slot[0] = 3;
        te_fx_slot[1] = 5;
        te_fx_knobs_synth[3 * 8 + 2] = (int16_t)(stored < 0 ? k[2] : stored);
        te_fx_knobs_drum[5 * 8 + 2] = (int16_t)(stored < 0 ? k[2] : stored);
        int stereo = mode == 's';
#ifndef SPACE_HOST
        rep_key.screen_ctl = stereo ? RK_MASTER : RK_SYNTH;
        rep_key.screen_seen = te_blocks;
        rep_key.held[stereo ? RK_MASTER : RK_SYNTH] = (uint8_t)key;
        te_tape_pos += 64 * 64;
        te_metro_count -= 64 << 14;
        if (te_metro_count <= 0)
            te_metro_count += te_tempo_spb;
#else
        (void)key;
#endif
        uint32_t c = 0;
        if (fx == 's' || fx == 'S') {
            uint32_t t0 = cycles();
            if (stereo)
                space_stereo(self, l, r, p);
            else
                space_mono(self, l, p);
            c = cycles() - t0;
            if (fx == 's') {
                maxc = c > maxc ? c : maxc;
                sum += c;
                calls++;
            }
        }
#ifndef SPACE_HOST
        else if (fx == 'p') {
            if (stereo)
                pingpong_stereo(self, l, r, p);
            else
                pingpong_mono(self, l, p);
        } else if (fx == 'r') {
            if (stereo)
                repeat_stereo(self, l, r, p);
            else
                repeat_mono(self, l, p);
        }
#endif
        for (int i = 0; i < 64; i++)
            fr[2 * i] = l[i], fr[2 * i + 1] = r[i];
        fwrite(fr, 2, 128, out);
        const struct space_ui *u = &space_ui[stereo];
        fprintf(st, "B %d %d %d %d %d %d %d %u %u %u\n", (int)u->level, (int)u->t60_ms, (int)u->frz, (int)u->inf,
                (int)u->zone, (int)u->clearing, (int)u->duck, (unsigned)fx_line_owner[0], (unsigned)fx_line_owner[1],
                (unsigned)c);
    }
    fclose(out);
    fclose(st);
    fprintf(stderr, "cycles max %u mean %u calls %u\n", (unsigned)maxc, (unsigned)(calls ? sum / calls : 0),
            (unsigned)calls);
#ifdef SPACE_PROF
    {
        extern uint32_t space_prof[12];
        fprintf(stderr, "prof");
        for (int i = 0; i < 12; i++)
            fprintf(stderr, " %u", (unsigned)(space_prof[i] / (calls ? calls : 1)));
        fprintf(stderr, "\n");
    }
#endif
#ifdef SPACE_DIAG
    {
        extern int32_t space_diag_peak;
        fprintf(stderr, "peak %d\n", (int)space_diag_peak);
    }
#endif
    return 0;
}
