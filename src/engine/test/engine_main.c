/* Simulator driver for any engine: calls _render (trampoline) like TE's voice loop.
 *   engine_main p0 p1 p2 p3 inc calls [inc_end|0] [sampler[:gain knob]]
 * Note-on state as TE writes it (state bytes 0..3 and 8..11 = 0x80000000, 16..23 cleared). The pitch increment glides
 * linearly from inc to inc_end (default inc) over the run. Prints one sample per line, then on stderr
 * "cycles max <n> mean <n> guard <hex>": CYCLES per render call and the out[128] overrun guard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

uint32_t prof_stats[6];        /* tramp.S profiling (device: FREE region 0) */
uint8_t voice_ram[8 * 4096];   /* per-voice engine RAM (device: FREE region 0 at 0x00810000) */
/* TE state that grain_source.h reads (device: fixed addresses). No sampler slot unless set up. */
volatile int32_t te_engine_ids[8];
int32_t *te_ssamp[8];
volatile int16_t te_knobs[8][8];
volatile int32_t te_synth_slot;
uint32_t wt_ram[(256 + 2 * 12 * 4128 * 2) / 4];   /* wavetable SAMPLE bank (wt_user.h): nothing built */
/* Fake synth sampler for grain tests (argv[8] = "sampler"): slot 1 holds the sampler with a 440 Hz sine. */
static int16_t fake_sample[264608 + 16];
static int32_t fake_sb[0x103c / 4];
static int32_t fake_holder[2];
static void fake_sampler(void)
{
    /* 440 Hz sine, amplitude 16000, by the recursion s[n] = 2 cos(w) s[n-1] - s[n-2] */
    double w = 2 * 3.14159265358979 * 440 / 44100, c = 1 - w * w / 2 + w * w * w * w / 24 - w * w * w * w * w * w / 720;
    double s1 = 0, s2 = -16000 * (w - w * w * w / 6 + w * w * w * w * w / 120);   /* s[-1] = -A sin(w) */
    for (int i = 0; i < 264608; i++) {
        double v = i < 8 ? 0 : 2 * c * s1 - s2;
        if (i == 8)
            v = 0;
        if (i == 9)
            v = 16000 * (w - w * w * w / 6 + w * w * w * w * w / 120);
        if (i >= 8) {
            s2 = s1;
            s1 = v;
        }
        fake_sample[i] = (int16_t)v;
    }
    fake_sb[0] = (int32_t)(uintptr_t)fake_sample;
    fake_sb[0x1038 / 4] = 264608;
    fake_holder[0] = (int32_t)(uintptr_t)fake_sb;
    te_ssamp[1] = fake_holder;
    te_engine_ids[1] = 6;
    te_synth_slot = 0;
    te_knobs[1][0] = 0;
    te_knobs[1][3] = 32767;
    te_knobs[1][7] = 8192;                  /* the sampler's gain knob: unity */
}
int32_t pitch_buf[128];   /* stands in for the firmware's per-voice pitch buffer at 0xFF801950 */
extern void render(void *self, int16_t *out, const int16_t *params);

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c));
    return c;
}

int main(int argc, char **argv)
{
    /* vptr, then TE's note-on state: state bytes 0..3 and 8..11 = 0x80000000, 16..23 cleared */
    static uint32_t voice[9] = { 0x019103CC, 0x80000000u, 0, 0x80000000u, 0, 0, 0 };
    static int16_t out[130];
    int16_t params[4];
    if (argc < 7)
        return 2;
    for (int i = 0; i < 4; i++)
        params[i] = (int16_t)atoi(argv[1 + i]);
    double inc0 = strtod(argv[5], 0), inc1 = argc > 7 && strtod(argv[7], 0) > 0 ? strtod(argv[7], 0) : inc0;
    if (argc > 8 && !strncmp(argv[8], "sampler", 7)) {          /* "sampler" or "sampler:<gain knob>" */
        fake_sampler();
        if (argv[8][7] == ':')
            te_knobs[1][7] = (int16_t)atoi(argv[8] + 8);
    }
    int calls = atoi(argv[6]);
    uint32_t cmax = 0;
    double csum = 0;
    out[128] = out[129] = 0x5a5a;
    /* voice_ram starts dirty, as on the device after boot or after another engine used the slot */
    for (unsigned i = 0, x = 0x12345678u; i < sizeof voice_ram; i++) {
        x = x * 1664525u + 1013904223u;
        voice_ram[i] = (uint8_t)(x >> 24);
    }
    __asm__ volatile ("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* CCEN: run CYCLES */
    for (int c = 0; c < calls; c++) {
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
    fprintf(stderr, "cycles max %lu mean %.0f guard %04x%04x\n", (unsigned long)cmax, csum / calls,
            (uint16_t)out[128], (uint16_t)out[129]);
    return 0;
}
