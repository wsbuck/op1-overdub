/* Simulator driver for the distortion FX's screen functions (distortion_check.py; bfin-elf-run).
 *   distortion_curve K0 K1 K2 K3 [K0 K1 K2 K3 ...]
 * stdout, one line per knob set: "<mode name> <mean CYCLES per distortion_curve call> y0 .. y64", where y_i is the
 * curve at x = -32768 + 1024 i (the last point at 32767). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int32_t distortion_curve(const int16_t *p, int32_t x);
const char *distortion_mode_name(const int16_t *p);

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

int main(int argc, char **argv)
{
    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* CCEN: run CYCLES */
    for (int a = 1; a + 3 < argc; a += 4) {
        int16_t p[8] = {0};
        int32_t y[65];
        for (int i = 0; i < 4; i++)
            p[i] = atoi(argv[a + i]);
        uint32_t t0 = cycles();
        for (int i = 0; i < 65; i++)
            y[i] = distortion_curve(p, i == 64 ? 32767 : -32768 + 1024 * i);
        uint32_t c = cycles() - t0;
        printf("%s %u", distortion_mode_name(p), (unsigned)(c / 65));
        for (int i = 0; i < 65; i++)
            printf(" %d", (int)y[i]);
        printf("\n");
    }
    return 0;
}
