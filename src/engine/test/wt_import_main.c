/* Simulator/host driver for src/engine/wt_import.h (custom wavetable bank from a sample).
 *   wt_import_main START LEN < sample.raw > bank.raw
 * stdin: int16 little-endian sample (up to 264600 = the synth sampler's 6 s). Builds the bank from x[START ..
 * START + LEN) with wti_step, writes FRAMES * FRAME_STRIDE int16 to stdout and on stderr
 * "period <p_q16> pitched <0|1|2> cycles detect <n> frame_max <n> total <n>" (CYCLES on Blackfin, 0 on the host). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../wt_import.h"

static int16_t sample[264600 + 16];
static int16_t bank[FRAMES * FRAME_STRIDE];
static struct wti_work work;

static inline uint32_t cycles(void)
{
#ifdef __bfin__
    uint32_t c;
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c));
    return c;
#else
    return 0;
#endif
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    size_t n = fread(sample, 2, 264600, stdin);
    uint32_t start = (uint32_t)atol(argv[1]), len = (uint32_t)atol(argv[2]);
    if (start + len > n)
        len = n > start ? (uint32_t)(n - start) : 0;
#ifdef __bfin__
    __asm__ volatile ("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* CCEN: run CYCLES */
#endif
    struct wti_job job;
    wti_begin(&job, sample + start, len, start, bank);
    uint32_t detect = 0, fmax = 0, total = 0;
    int r;
    do {
        int stage = job.stage;
        uint32_t t0 = cycles();
        r = wti_step(&job, &work);
        uint32_t dt = cycles() - t0;
        total += dt;
        if (stage == 0)
            detect = dt;
        else if (dt > fmax)
            fmax = dt;
    } while (r > 0);
    fwrite(bank, 2, sizeof bank / 2, stdout);
    fprintf(stderr, "period %lu pitched %d cycles detect %lu frame_max %lu total %lu status %d\n",
            (unsigned long)job.p_q16, (int)job.pitched, (unsigned long)detect, (unsigned long)fmax,
            (unsigned long)total, r);
    return r < 0;
}
