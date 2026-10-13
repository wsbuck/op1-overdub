/* Simulator driver for the distortion FX with inputs fx_main.c does not make (distortion_check.py; bfin-elf-run).
 *   distortion_sig MODE K0 K1 K2 K3 BLOCKS
 * MODE d: a 200 Hz sine at -6 dBFS for 0.2 s, then decaying 60 dB per second (a plucked note);
 * MODE n: white noise, -12 dBFS rms (uniform, fixed seed).
 * Mono instance. stdout: one output sample per line. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

void distortion_mono(void *self, int16_t *buf, const int16_t *p);

int main(int argc, char **argv)
{
    if (argc < 7)
        return 2;
    int16_t k[8] = {0}, b[64];
    for (int i = 0; i < 4; i++)
        k[i] = atoi(argv[2 + i]);
    int blocks = atoi(argv[6]), noise = argv[1][0] == 'n';
    double ph = 0, a = 16383, dec = pow(10, -60.0 / 20 / 44100);
    uint32_t seed = 12345;
    for (int n = 0, blk = 0; blk < blocks; blk++) {
        for (int i = 0; i < 64; i++, n++) {
            if (noise) {
                seed = seed * 1664525u + 1013904223u;
                b[i] = (int16_t)((int32_t)(seed >> 16) - 32768) * 0.4335;   /* uniform: rms = 0.25 FS */
            } else {
                ph += 2 * M_PI * 200 / 44100;
                if (n >= 8820)
                    a *= dec;
                b[i] = (int16_t)lrint(a * sin(ph));
            }
        }
        distortion_mono(0, b, k);
        for (int i = 0; i < 64; i++)
            printf("%d\n", b[i]);
    }
    return 0;
}
