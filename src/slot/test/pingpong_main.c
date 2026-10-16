/* Simulator driver for the ping-pong FX (bfin-elf-run; pingpong_check.py): fx_main.c's cases plus the tempo word and
 * knob ramps.
 *   pingpong_main MODE K0 K1 K2 K3 BLOCKS FREQ_MHZ SWITCH J0 J1 J2 J3 SPB0 TSW SPB1 RAMP
 * As fx_main.c (SWITCH -1: none). te_tempo_spb (TE's samples per beat, Q14) is SPB0, and SPB1 from block TSW on
 * (TSW -1: never). RAMP > 0: from SWITCH on, K0 walks to J0 one encoder detent (1024 raw) every RAMP blocks
 * instead of jumping (the other knobs switch at SWITCH as usual).
 * stdout: one line per frame (L, or "L R"); stderr: "cycles max <n> mean <n>" per process call. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define CLICK 12000
void pingpong_mono(void *self, int16_t *buf, const int16_t *p);
void pingpong_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);
volatile int32_t te_tempo_spb;              /* 0x01AADAD8 on the device */

static int16_t sine_q15(uint32_t ph)        /* = fx_main.c */
{
    int32_t x = (int32_t)ph >> 16;
    int32_t y = (x * (32767 - (x < 0 ? -x : x))) >> 13;
    return (int16_t)y;
}

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

int main(int argc, char **argv)
{
    if (argc != 17)
        return 2;
    int stereo = argv[1][0] == 's';
    int16_t k[8] = {0}, j[8] = {0}, q[8] = {0};
    for (int i = 0; i < 4; i++) {
        k[i] = atoi(argv[2 + i]);
        j[i] = atoi(argv[9 + i]);
    }
    int blocks = atoi(argv[6]), click = atoi(argv[7]) == 0;
    uint32_t inc = (uint32_t)((double)atoi(argv[7]) / 1000.0 / 44100.0 * 4294967296.0);
    int sw = atoi(argv[8]), tsw = atoi(argv[14]), ramp = atoi(argv[16]);
    int32_t spb0 = atoi(argv[13]), spb1 = atoi(argv[15]);
    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* CCEN: run CYCLES */
    uint32_t ph = 0, maxc = 0;
    uint64_t sum = 0;
    int16_t l[64], r[64];
    int32_t walk = k[0];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 64; i++, ph += inc) {
            l[i] = sine_q15(ph) >> 1;
            r[i] = sine_q15(ph - 0x40000000u) >> 1;
            if (click)
                l[i] = r[i] = b == 0 && i == 0 ? CLICK : 0;
        }
        te_tempo_spb = tsw >= 0 && b >= tsw ? spb1 : spb0;
        for (int i = 0; i < 8; i++)
            q[i] = sw >= 0 && b >= sw ? j[i] : k[i];
        if (ramp > 0 && sw >= 0 && b >= sw) {
            if ((b - sw) % ramp == 0 && walk != j[0])
                walk += walk < j[0] ? (j[0] - walk < 1024 ? j[0] - walk : 1024) : (walk - j[0] < 1024 ? j[0] - walk : -1024);
            q[0] = (int16_t)walk;
        }
        uint32_t t0 = cycles();
        if (stereo)
            pingpong_stereo(0, l, r, q);
        else
            pingpong_mono(0, l, q);
        uint32_t c = cycles() - t0;
        maxc = c > maxc ? c : maxc;
        sum += c;
        for (int i = 0; i < 64; i++)
            if (stereo)
                printf("%d %d\n", l[i], r[i]);
            else
                printf("%d\n", l[i]);
    }
    fprintf(stderr, "cycles max %u mean %u\n", (unsigned)maxc, (unsigned)(sum / blocks));
    return 0;
}
