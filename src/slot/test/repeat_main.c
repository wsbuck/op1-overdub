/* Simulator driver for the repeat FX (bfin-elf-run; repeat_check.py), with pingpong linked for the shared lines.
 *   repeat_main CTRL IN OUT STATE
 * CTRL: one text line per 64-frame block: "FX MODE K0 K1 K2 K3 SPB TAPE POS METRO SLOT"
 *   FX r = repeat, R = repeat without counting its cycles (a lead-in), p = pingpong
 *   MODE m (mono, the L channel) or s (stereo); K0..K3 the params (TE's per-block copy); SPB 0x01AADAD8, TAPE
 *   0xFF802960, POS 0xFF802974 (unsigned), METRO 0x01AADADC, SLOT 0xFF8016D4 (TE's words, as the device has them).
 * IN / OUT: int16 frames, L R interleaved (mono uses L; R passes through).
 * STATE: per block "B active hold kind rep grid blue fold depth dir fires lost turn bar ph8 bar8 lock w filled owner0
 *   owner1 pub_blocks" of repeat_state[mono or stereo] after the call.
 * stderr: "cycles max <n> mean <n> calls <n>" over the repeat calls. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "repeat.h"
#include "pingpong.h"
#include "fxpub.h"

volatile int32_t te_tempo_spb, te_tape_state, te_metro_count, te_play_slot;
volatile uint32_t te_tape_pos;
/* repeat.c's manual key: TE's block counter and the drum / mixer controllers' press and release (stand-ins) */
volatile uint32_t te_blocks;
int te_drum_press(void *self, int key) { (void)self; (void)key; return 0; }
int te_drum_release(void *self, int key) { (void)self; (void)key; return 0; }
int te_master_press(void *self, int key) { (void)self; (void)key; return 0; }
int te_master_release(void *self, int key) { (void)self; (void)key; return 0; }
void repeat_mono(void *self, int16_t *buf, const int16_t *p);
void repeat_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);
void pingpong_mono(void *self, int16_t *buf, const int16_t *p);
void pingpong_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);
extern struct fx_pub repeat_pub[2];

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

int main(int argc, char **argv)
{
    if (argc != 5)
        return 2;
    FILE *ctl = fopen(argv[1], "r"), *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb"), *st = fopen(argv[4], "w");
    if (!ctl || !in || !out || !st)
        return 3;
    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* CCEN: run CYCLES */
    char fx, mode;
    int k[4];
    long spb, tape, metro, slot;
    unsigned long pos;
    uint32_t maxc = 0, calls = 0;
    uint64_t sum = 0;
    static uint32_t self[64];
    while (fscanf(ctl, " %c %c %d %d %d %d %ld %ld %lu %ld %ld", &fx, &mode, &k[0], &k[1], &k[2], &k[3], &spb, &tape,
                  &pos, &metro, &slot) == 11) {
        int16_t fr[128], l[64], r[64], p[8] = { 0 };
        if (fread(fr, 2, 128, in) != 128)
            break;
        for (int i = 0; i < 64; i++)
            l[i] = fr[2 * i], r[i] = fr[2 * i + 1];
        for (int i = 0; i < 4; i++)
            p[i] = (int16_t)k[i];
        te_tempo_spb = (int32_t)spb;
        te_tape_state = (int32_t)tape;
        te_tape_pos = (uint32_t)pos;
        te_metro_count = (int32_t)metro;
        te_play_slot = (int32_t)slot;
        int stereo = mode == 's';
        if (fx == 'p') {
            if (stereo)
                pingpong_stereo(self, l, r, p);
            else
                pingpong_mono(self, l, p);
        } else {
            uint32_t t0 = cycles();
            if (stereo)
                repeat_stereo(self, l, r, p);
            else
                repeat_mono(self, l, p);
            uint32_t c = cycles() - t0;
            if (fx == 'r') {
                maxc = c > maxc ? c : maxc;
                sum += c;
                calls++;
            }
        }
        for (int i = 0; i < 64; i++)
            fr[2 * i] = l[i], fr[2 * i + 1] = r[i];
        fwrite(fr, 2, 128, out);
        const struct rep *s = &repeat_state[stereo];
        fprintf(st, "B %d %d %d %d %d %d %d %d %d %u %u %u %u %u %u %d %u %u %u %u %u\n", s->active, s->hold, s->kind,
                s->rep, s->grid, s->blue, s->fold, s->depth, s->dir, (unsigned)s->fires, (unsigned)s->lost,
                (unsigned)s->turn, (unsigned)s->bar, (unsigned)s->ph8, (unsigned)s->bar8, s->lock, (unsigned)s->w,
                (unsigned)s->filled, (unsigned)fx_line_owner[0], (unsigned)fx_line_owner[1],
                (unsigned)repeat_pub[stereo].blocks);
    }
    fclose(out);
    fclose(st);
    fprintf(stderr, "cycles max %u mean %u calls %u\n", (unsigned)maxc, (unsigned)(calls ? sum / calls : 0),
            (unsigned)calls);
    return 0;
}
