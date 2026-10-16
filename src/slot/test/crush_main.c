/* Simulator driver for the crush FX (crush_check.py; bfin-elf-run).
 *   crush_main MODE K0 K1 K2 K3 BLOCKS [SCHED ...] < input
 * MODE m (mono) or s (stereo). input: BLOCKS * 64 frames of raw int16 (little endian), one per frame (mono) or L R
 * interleaved (stereo); crush_check.py makes the signals.
 * SCHED (knob changes at block starts, applied in order):
 *   j:B:J0:J1:J2:J3          from block B the knobs are J0..J3
 *   t:K:STEP:EVERY:FIRST:N   knob K moves by STEP every EVERY blocks from block FIRST, N times (encoder detents)
 * stdout per frame: "L [R] GR" (GR = crush_state[].gr after that block); stderr: "cycles max <n> mean <n> flips <n> rate <hz>
 * bits <q12> mu <q15>" (bit flips over the run; the state after the last block). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../crush.h"

void crush_mono(void *self, int16_t *buf, const int16_t *p);
void crush_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

int main(int argc, char **argv)
{
    if (argc < 7)
        return 2;
    int stereo = argv[1][0] == 's', nch = stereo ? 2 : 1;
    int16_t k[8] = {0};
    for (int i = 0; i < 4; i++)
        k[i] = atoi(argv[2 + i]);
    int blocks = atoi(argv[6]);
    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");
    uint32_t maxc = 0;
    uint64_t sum = 0;
    int16_t in[128], l[64], r[64];
    for (int b = 0; b < blocks; b++) {
        for (int a = 7; a < argc; a++) {
            int v[6] = {0};
            sscanf(argv[a] + 2, "%d:%d:%d:%d:%d:%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
            if (argv[a][0] == 'j' && b == v[0])
                for (int i = 0; i < 4; i++)
                    k[i] = v[1 + i];
            if (argv[a][0] == 't' && b >= v[3] && (b - v[3]) % v[2] == 0 && (b - v[3]) / v[2] < v[4]) {
                int x = k[v[0]] + v[1];
                k[v[0]] = x > 32767 ? 32767 : x < 0 ? 0 : x;
            }
        }
        if (fread(in, 2, 64 * nch, stdin) != (size_t)(64 * nch))
            return 3;
        for (int i = 0; i < 64; i++) {
            l[i] = in[i * nch];
            r[i] = in[i * nch + nch - 1];
        }
        uint32_t t0 = cycles();
        if (stereo)
            crush_stereo(0, l, r, k);
        else
            crush_mono(0, l, k);
        uint32_t c = cycles() - t0;
        maxc = c > maxc ? c : maxc;
        sum += c;
        int gr = crush_state[stereo].gr;
        for (int i = 0; i < 64; i++)
            if (stereo)
                printf("%d %d %d\n", l[i], r[i], gr);
            else
                printf("%d %d\n", l[i], gr);
    }
    const struct crush *st = &crush_state[stereo];
    fprintf(stderr, "cycles max %u mean %u flips %u rate %d bits %d mu %d\n", (unsigned)maxc, (unsigned)(sum / blocks),
            (unsigned)st->flips, (int)st->rate_hz, (int)st->bits_q12, (int)st->mu);
    return 0;
}
