/* Simulator driver for the harmonic engine (harm.c): plays the UI thread's part too (harm_build.h into harm_ram, then
 * publish), and calls _render (tramp.S) like TE's voice loop.
 *   harm_main K0..K7 inc calls inc_end [switch CALL K0..K7 | nobuild | dump]
 * K0..K7: the sound's packed knobs (harm.h). The pitch increment glides linearly from inc to inc_end (0 = inc).
 *   switch CALL K0..K7  at render call CALL, build the second drawing into another frame and publish it (crossfade)
 *   nobuild             never publish (the engine's sine before the first build)
 *   dump                no render: write the built frame (FRAME_STRIDE int16, one per line) instead
 * stdout: one sample per line. stderr: "cycles max <n> mean <n> guard <hex> build <n>" (CYCLES per render call, the
 * out[128] overrun guard, CYCLES of the first table build). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../harm_build.h"

uint32_t prof_stats[6];        /* tramp.S profiling */
int32_t pitch_buf[128];        /* TE's per-voice pitch buffer (0xFF801950 on device) */
struct harm_ram harm_ram;      /* device: engines.json "ram", the builder's scratch right after it */
static struct wti_work work;
extern void render(void *self, int16_t *out, const int16_t *params);

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c));
    return c;
}

static uint32_t publish(const int16_t *k)
{
    struct harm_ram *h = &harm_ram;
    int target = 1;
    while (target == h->front || target == h->prev)
        target++;
    uint32_t t0 = cycles();
    harm_build(k, h->frame[target - 1], &work);
    uint32_t dt = cycles() - t0;
    h->uni = harm_uni(k) * HARM_UNI_SCALE;
    h->warp = harm_warp(k) * HARM_UNI_SCALE;
    h->prev = h->front;
    h->front = target;
    h->sig = harm_sig(k);
    return dt;
}

int main(int argc, char **argv)
{
    static uint32_t voice[9] = { 0x019103CC, 0x80000000u, 0, 0x80000000u, 0, 0, 0 };
    static int16_t out[130];
    int16_t k[8], k2[8], params[8] = { 0 };
    if (argc < 12)
        return 2;
    for (int i = 0; i < 8; i++)
        k[i] = (int16_t)atoi(argv[1 + i]);
    double inc0 = strtod(argv[9], 0), inc1 = strtod(argv[11], 0) > 0 ? strtod(argv[11], 0) : inc0;
    int calls = atoi(argv[10]), sw = -1, nobuild = 0, dump = 0;
    if (argc > 12 && !strcmp(argv[12], "switch") && argc >= 22) {
        sw = atoi(argv[13]);
        for (int i = 0; i < 8; i++)
            k2[i] = (int16_t)atoi(argv[14 + i]);
    }
    nobuild = argc > 12 && !strcmp(argv[12], "nobuild");
    dump = argc > 12 && !strcmp(argv[12], "dump");
    __asm__ volatile ("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* CCEN: run CYCLES */
    uint32_t build = nobuild ? 0 : publish(k);
    if (dump) {
        for (int i = 0; i < FRAME_STRIDE; i++)
            printf("%d\n", harm_ram.frame[harm_ram.front - 1][i]);
        fprintf(stderr, "cycles max 0 mean 0 guard 5a5a5a5a build %lu\n", (unsigned long)build);
        return 0;
    }
    uint32_t cmax = 0;
    double csum = 0;
    out[128] = out[129] = 0x5a5a;
    for (int c = 0; c < calls; c++) {
        if (c == sw)
            publish(k2);
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
    fprintf(stderr, "cycles max %lu mean %.0f guard %04x%04x build %lu\n", (unsigned long)cmax, csum / calls,
            (uint16_t)out[128], (uint16_t)out[129], (unsigned long)build);
    return 0;
}
