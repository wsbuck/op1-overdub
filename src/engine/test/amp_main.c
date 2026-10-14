/* Simulator driver for the amp engine (src/engine/amp.c): several voices, a synthetic input, scripted notes.
 *   amp_main k0 k1 k2 k3 blocks INPUT [EVENTS] [FLAGS]
 * INPUT   a file of int16 samples (44.1 kHz; amp_check.py writes it): 64 go into TE IN before each callback (zeros
 *         after its end).
 * EVENTS  comma-separated <block><action><voice>: n note-on (TE's note-on writes, the voice renders from then on),
 *         r release (byte 28 = 1: iter's note-off; the voice keeps rendering), s stop (no longer rendered).
 *         Default: "0n0" (one voice from the start).
 * FLAGS   nosel  te_rx_sel stays 0 (block detection by content only)
 *         dbl    every active voice is rendered twice per callback (TE's unexplained 2 calls per block, LOG.md)
 *         rev    voices render in descending index order
 * Prints per sample "mix mask gate": the sum of all voices' outputs, a bitmask of voices with nonzero output, the gate
 * state after the block. On stderr: "cycles max <n> mean <n> dspmax <n> guard <ok|bad> takeovers <n> owner <v> renders <n>
 * blocks <n> dsp <n>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../amp.h"

uint32_t prof_stats[6];
int32_t pitch_buf[128];
volatile int16_t te_audio_in[64];
volatile uint32_t te_rx_sel;
volatile int32_t te_rx[256];
volatile int32_t te_alt_in[128];
struct amp_ram amp_ram;

#define VOICES 6
static uint32_t voice[VOICES][9];          /* 36 B each, as TE's L1 voice objects: vptr, then 32 B of state */
static int16_t out[VOICES][130];
extern void render(void *self, int16_t *out, const int16_t *params);

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c));
    return c;
}

int main(int argc, char **argv)
{
    if (argc < 7)
        return 2;
    int16_t params[8] = { 0 };
    for (int i = 0; i < 4; i++)
        params[i] = (int16_t)atoi(argv[1 + i]);
    int blocks = atoi(argv[5]);
    FILE *in = fopen(argv[6], "rb");
    if (!in)
        return 3;
    const char *events = argc > 7 ? argv[7] : "0n0";
    const char *flags = argc > 8 ? argv[8] : "";
    int nosel = strstr(flags, "nosel") != 0, dbl = strstr(flags, "dbl") != 0, rev = strstr(flags, "rev") != 0;
    int active[VOICES] = { 0 };
    for (int v = 0; v < VOICES; v++) {
        voice[v][0] = 0x019103CC;
        for (int j = 1; j < 9; j++)                 /* state as another engine left it */
            voice[v][j] = 0x5A5A5A5Au * (j + v);
        out[v][128] = out[v][129] = 0x5a5a;
    }
    memset(&amp_ram, 0xA5, sizeof amp_ram);         /* FREE SDRAM after boot: the image zeroes the first 256 B only */
    memset(&amp_ram, 0, 256);
    uint32_t cmax = 0, dspmax = 0, calls = 0;
    double csum = 0;
    int guard_ok = 1;
    __asm__ volatile ("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");
    for (int c = 0; c < blocks; c++) {
        /* events at this block */
        for (const char *e = events; *e;) {
            int b = atoi(e);
            while (*e >= '0' && *e <= '9')
                e++;
            char a = *e++;
            int v = atoi(e);
            while (*e >= '0' && *e <= '9')
                e++;
            if (*e == ',')
                e++;
            if (b != c || v < 0 || v >= VOICES)
                continue;
            uint8_t *st = (uint8_t *)&voice[v][1];
            if (a == 'n') {                             /* TE's note-on: 0..3, 8..11 = 0x80000000, 16..23 = 0, 24..27 gain */
                ((uint32_t *)st)[0] = 0x80000000u;
                ((uint32_t *)st)[2] = 0x80000000u;
                ((uint32_t *)st)[4] = ((uint32_t *)st)[5] = 0;
                ((uint32_t *)st)[6] = 0x12341234u;
                active[v] = 1;
            } else if (a == 'r') {
                st[28] = 1;
            } else if (a == 's') {
                active[v] = 0;
            }
        }
        /* the input block */
        te_rx_sel = nosel ? 0 : (uint32_t)(c & 1);
        int16_t blk[64];
        size_t got = fread(blk, 2, 64, in);
        for (int i = 0; i < 64; i++)
            te_audio_in[i] = i < (int)got ? blk[i] : 0;
        /* the voice loop */
        int mix[128] = { 0 }, mask[128] = { 0 };
        for (int pass = 0; pass < (dbl ? 2 : 1); pass++) {
            for (int n = 0; n < VOICES; n++) {
                int v = rev ? VOICES - 1 - n : n;
                if (!active[v])
                    continue;
                uint32_t d0 = amp_ram.dsp_blocks, t0 = cycles();
                render(voice[v], out[v], params);
                uint32_t dt = cycles() - t0;
                cmax = dt > cmax ? dt : cmax;
                if (amp_ram.dsp_blocks != d0)
                    dspmax = dt > dspmax ? dt : dspmax;
                csum += dt;
                calls++;
                if (out[v][128] != 0x5a5a || out[v][129] != 0x5a5a)
                    guard_ok = 0;
                if (pass == 0)
                    for (int i = 0; i < 128; i++) {
                        mix[i] += out[v][i];
                        mask[i] |= (out[v][i] != 0) << v;
                    }
            }
        }
        for (int i = 0; i < 128; i++)
            printf("%d %d %d\n", mix[i], mask[i], (int)amp_ram.gate_open);
    }
    int owner = -1;
    for (int v = 0; v < VOICES; v++)
        if (amp_ram.owner == (uint32_t)&voice[v][1])
            owner = v;
    fprintf(stderr, "cycles max %lu mean %.0f dspmax %lu guard %s takeovers %lu owner %d renders %lu blocks %lu dsp %lu\n",
            (unsigned long)cmax, calls ? csum / calls : 0, (unsigned long)dspmax, guard_ok ? "ok" : "bad",
            (unsigned long)amp_ram.takeovers, owner, (unsigned long)amp_ram.renders, (unsigned long)amp_ram.blk,
            (unsigned long)amp_ram.dsp_blocks);
    return 0;
}
