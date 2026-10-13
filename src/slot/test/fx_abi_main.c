/* Simulator driver for fx_abi_check.py: runs our FX through their firmware entry wrappers (fx_abi.S).
 *   fx_abi FX MODE BLOCKS     FX 0 ensemble, 1 pingpong, 2 distortion
 *   MODE m: mono only; s: stereo only; i: mono and stereo interleaved (as when a synth FX and the master FX run
 *   in one audio callback); d: stereo with mix 0 (must be the dry input: proves the 4th argument arrives).
 * stdout: per block "m" then 64 samples and/or "s" then 64 L and 64 R; stderr/exit 1 on any ABI mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef void (*entry_t)(void);
int fx_abi_call(entry_t entry, void *self, void *a1, void *a2, void *a3);
extern void ensemble_mono_entry(void), ensemble_stereo_entry(void), pingpong_mono_entry(void),
    pingpong_stereo_entry(void), distortion_mono_entry(void), distortion_stereo_entry(void);
static const entry_t mono[3] = { ensemble_mono_entry, pingpong_mono_entry, distortion_mono_entry };
static const entry_t stereo[3] = { ensemble_stereo_entry, pingpong_stereo_entry, distortion_stereo_entry };

static int16_t sine(uint32_t ph)
{
    int32_t x = (int32_t)ph >> 16;
    return (int16_t)(((x * (32767 - (x < 0 ? -x : x))) >> 13) >> 1);
}

static void knobs(int b, int16_t *k, int dry)
{
    k[0] = (int16_t)((b * 1777) & 0x7FFF);
    k[1] = (int16_t)((b * 911 + 9000) & 0x7FFF);
    k[2] = (int16_t)((b * 3001) & 0x7FFF);
    k[3] = dry ? 0 : 24000;
    k[4] = k[5] = k[6] = k[7] = 0;
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    int fx = atoi(argv[1]), blocks = atoi(argv[3]);
    char mode = argv[2][0];
    static uint32_t self[64];                    /* stands in for TE's FX object (our code must not need it) */
    int16_t m[64], l[64], r[64], k[8];
    uint32_t ph = 0, inc = 52000000u;            /* ~534 Hz */
    for (int b = 0; b < blocks; b++) {
        knobs(b, k, mode == 'd');
        for (int i = 0; i < 64; i++, ph += inc) {
            m[i] = sine(ph);
            l[i] = sine(ph);
            r[i] = sine(ph + 0x40000000u);
        }
        int mask = 0;
        if (mode == 'm' || mode == 'i')
            mask |= fx_abi_call(mono[fx], self, m, k, 0);
        if (mode == 's' || mode == 'i' || mode == 'd')
            mask |= fx_abi_call(stereo[fx], self, l, r, k);
        if (mask) {
            fprintf(stderr, "abi mismatch mask %08x at block %d\n", mask, b);
            return 1;
        }
        if (mode == 'm' || mode == 'i') {
            printf("m");
            for (int i = 0; i < 64; i++)
                printf(" %d", m[i]);
            printf("\n");
        }
        if (mode != 'm') {
            printf("s");
            for (int i = 0; i < 64; i++)
                printf(" %d %d", l[i], r[i]);
            printf("\n");
        }
    }
    return 0;
}
