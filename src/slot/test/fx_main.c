/* Simulator driver for one of our FX, src/slot/<source>.c (bfin-elf-run; see fxsim.py). Built with
 * -DFX=<source>, so it calls <source>_mono / <source>_stereo.
 *   fx_main MODE K0 K1 K2 K3 BLOCKS FREQ_MHZ [SWITCH_BLOCK J0 J1 J2 J3]
 * MODE m (mono) or s (stereo). Input: a sine at FREQ_MHZ / 1000 Hz, -6 dBFS, on every channel (R a quarter
 * period later, so dry L != R). FREQ_MHZ 0: a click instead, one sample of CLICK on every channel at frame 0, then
 * silence (impulse response, e.g. echo times). FREQ_MHZ < 0: a clean sine at -FREQ_MHZ (series in double, no
 * harmonics or aliases of its own: THD and aliasing figures). From SWITCH_BLOCK on the knobs are J0..J3 (knob-jump
 * click test).
 * stdout: one line per frame (L, or "L R"); stderr: "cycles max <n> mean <n>" per process call. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define FX_MONO CAT(FX, _mono)
#define FX_STEREO CAT(FX, _stereo)
#define CLICK 12000
void FX_MONO(void *self, int16_t *buf, const int16_t *p);
void FX_STEREO(void *self, int16_t *l, int16_t *r, const int16_t *p);

static int16_t sine_q15(uint32_t ph)              /* parabola pair: smooth, a few % THD */
{
    int32_t x = (int32_t)ph >> 16;                /* -32768 .. 32767 = -pi .. pi */
    int32_t y = (x * (32767 - (x < 0 ? -x : x))) >> 13;   /* 4 x (1 - |x|), Q15 */
    return (int16_t)y;
}

static int16_t sine_pure(uint32_t ph)              /* -6 dBFS, rounded */
{
    double x = (int32_t)ph / 2147483648.0 * 3.14159265358979;               /* -pi .. pi */
    x = x > 1.5707963267949 ? 3.14159265358979 - x : x < -1.5707963267949 ? -3.14159265358979 - x : x;
    double x2 = x * x;
    double s = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156))))));
    return (int16_t)(s * 16383.5 + (s >= 0 ? 0.5 : -0.5));
}

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

int main(int argc, char **argv)
{
    if (argc < 8)
        return 2;
    int stereo = argv[1][0] == 's';
    int16_t k[8] = {0}, j[8] = {0};
    for (int i = 0; i < 4; i++)
        k[i] = atoi(argv[2 + i]);
    int blocks = atoi(argv[6]);
    int click = atoi(argv[7]) == 0, pure = atoi(argv[7]) < 0;
    uint32_t inc = (uint32_t)((double)abs(atoi(argv[7])) / 1000.0 / 44100.0 * 4294967296.0);
    int sw = argc >= 13 ? atoi(argv[8]) : -1;
    for (int i = 0; i < 4 && sw >= 0; i++)
        j[i] = atoi(argv[9 + i]);
    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* CCEN: run CYCLES */
    uint32_t ph = 0, maxc = 0;
    uint64_t sum = 0;
    int16_t l[64], r[64];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 64; i++, ph += inc) {
            l[i] = sine_q15(ph) >> 1;
            r[i] = sine_q15(ph - 0x40000000u) >> 1;
            if (click)
                l[i] = r[i] = b == 0 && i == 0 ? CLICK : 0;
            if (pure) {
                l[i] = sine_pure(ph);
                r[i] = sine_pure(ph - 0x40000000u);
            }
        }
        const int16_t *p = sw >= 0 && b >= sw ? j : k;
        uint32_t t0 = cycles();
        if (stereo)
            FX_STEREO(0, l, r, p);
        else
            FX_MONO(0, l, p);
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
