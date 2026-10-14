/* Simulator driver for engines that read all 8 knobs from params (fm4.c): calls _render (tramp.S) like TE's voice loop.
 *   fm4_main P0..P7 inc calls inc_end [noteon CALL]
 * Note-on state as TE writes it (state bytes 0..3 and 8..11 = 0x80000000, 16..23 cleared) at the start, and again at
 * render call CALL if given. The pitch increment glides linearly from inc to inc_end (0 = inc). stdout: one sample per
 * line. stderr: "cycles max <n> mean <n> guard <hex>" (CYCLES per render call, the out[128] overrun guard). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

uint32_t prof_stats[6];        /* tramp.S profiling */
uint8_t voice_ram[8 * 4096];   /* per-voice engine RAM (device: FREE region 0 at 0x00810000) */
int32_t pitch_buf[128];        /* TE's per-voice pitch buffer (0xFF801950 on device) */
extern void render(void *self, int16_t *out, const int16_t *params);

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c));
    return c;
}

int main(int argc, char **argv)
{
    static uint32_t voice[9] = { 0x019103CC, 0x80000000u, 0, 0x80000000u, 0, 0, 0 };
    static int16_t out[130];
    int16_t params[8];
    if (argc < 12)
        return 2;
    for (int i = 0; i < 8; i++)
        params[i] = (int16_t)atoi(argv[1 + i]);
    double inc0 = strtod(argv[9], 0), inc1 = strtod(argv[11], 0) > 0 ? strtod(argv[11], 0) : inc0;
    int calls = atoi(argv[10]), retrig = argc > 13 && !strcmp(argv[12], "noteon") ? atoi(argv[13]) : -1;
    /* voice_ram starts dirty, as on the device after boot or after another engine used the slot */
    for (unsigned i = 0, x = 0x12345678u; i < sizeof voice_ram; i++) {
        x = x * 1664525u + 1013904223u;
        voice_ram[i] = (uint8_t)(x >> 24);
    }
    __asm__ volatile ("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* CCEN: run CYCLES */
    uint32_t cmax = 0;
    double csum = 0;
    out[128] = out[129] = 0x5a5a;
    for (int c = 0; c < calls; c++) {
        if (c == retrig) {
            voice[1] = voice[3] = 0x80000000u;
            voice[5] = voice[6] = 0;
        }
        for (int i = 0; i < 128; i++)
            pitch_buf[i] = (int32_t)(inc0 + (inc1 - inc0) * (c * 128 + i) / (calls * 128.0));
        uint32_t t0 = cycles();
        render(voice, out, params);
        uint32_t dt = cycles() - t0;
        cmax = dt > cmax ? dt : cmax;
        csum += dt;
        for (int i = 0; i < 128; i++)
            printf("%d\n", out[i]);
    }
    fprintf(stderr, "cycles max %lu mean %.0f guard %04x%04x\n", (unsigned long)cmax, csum / calls, (uint16_t)out[128],
            (uint16_t)out[129]);
    return 0;
}
