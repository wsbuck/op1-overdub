/* Simulator driver for any engine: calls _render (trampoline) like TE's voice loop.
 *   engine_main p0 p1 p2 p3 inc calls [inc_end|0] [sampler[:gain knob] | k4:p4,p5,p6,p7 | turn:k,step,every,first |
 *               lfo:k,period] ...   (k4:, turn: and lfo: may be given together)
 * turn: knob k moves by step (raw, clamped to 0..32767) before call first and every `every` calls after it, as the
 * encoder's detents do (once per block). lfo: knob k follows a triangle 0 -> 32767 -> 0 of `period` calls, set once
 * per call as TE's LFO modulates the voice's knob copy.
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
/* Audio-input buffers that inprobe.h reads (device: TE's addresses, notes/input.md). Refilled before every render
 * call with integer triangles that inprobe_check.py recomputes: TE IN period 44, amplitude 8000; RAW (the half
 * te_rx_sel names) L period 88 / 12000 and R period 30 / 6000, MSB-aligned; the other half 0x00123400; ALT L period
 * 60 / 10000, R = -L. n counts 44.1 kHz frames from the first call. */
volatile int16_t te_audio_in[64];
volatile uint32_t te_rx_sel;
volatile int32_t te_rx[256];
volatile int32_t te_alt_in[128];
uint32_t sprobe_ram[16];   /* knob probe's screen record (sprobe.h: renders, live[8]); printed on stderr */
static int32_t tri(int n, int p, int a)
{
    int t = n % p;
    return 2 * t < p ? -a + 4 * a * t / p : 3 * a - 4 * a * t / p;
}
static void fill_input(int c)
{
    te_rx_sel = c & 1;
    for (int i = 0; i < 64; i++) {
        int n = c * 64 + i;
        te_audio_in[i] = (int16_t)tri(n, 44, 8000);
        te_rx[128 * (c & 1) + 2 * i] = tri(n, 88, 12000) << 16;
        te_rx[128 * (c & 1) + 2 * i + 1] = tri(n, 30, 6000) << 16;
        te_rx[128 * (~c & 1) + 2 * i] = te_rx[128 * (~c & 1) + 2 * i + 1] = 0x00123400;
        te_alt_in[2 * i] = tri(n, 60, 10000) << 16;
        te_alt_in[2 * i + 1] = -te_alt_in[2 * i];
    }
}
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
    int16_t params[8] = { 0 };
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
    int turn_k = -1, turn_step = 0, turn_every = 1, turn_first = 0, lfo_k = -1, lfo_period = 1;
    for (int a = 8; a < argc; a++) {        /* k4: / turn: / lfo: may be combined (va_check: a shape, then a turn) */
        if (!strncmp(argv[a], "k4:", 3))    /* SHIFT knobs 4..7 */
            sscanf(argv[a] + 3, "%hd,%hd,%hd,%hd", &params[4], &params[5], &params[6], &params[7]);
        if (!strncmp(argv[a], "turn:", 5))
            sscanf(argv[a] + 5, "%d,%d,%d,%d", &turn_k, &turn_step, &turn_every, &turn_first);
        if (!strncmp(argv[a], "lfo:", 4))
            sscanf(argv[a] + 4, "%d,%d", &lfo_k, &lfo_period);
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
        fill_input(c);
        if (turn_k >= 0 && c >= turn_first && (c - turn_first) % turn_every == 0) {
            int v = params[turn_k] + turn_step;
            params[turn_k] = (int16_t)(v < 0 ? 0 : v > 32767 ? 32767 : v);
        }
        if (lfo_k >= 0) {
            int t = c % lfo_period;
            params[lfo_k] = (int16_t)(2 * t < lfo_period ? 65534 * t / lfo_period : 65534 * (lfo_period - t) / lfo_period);
        }
        uint32_t t0 = cycles();
        render(voice, out, params);
        uint32_t dt = cycles() - t0;
        cmax = dt > cmax ? dt : cmax;
        csum += dt;
        for (int i = 0; i < 128; i++)
            printf("%d\n", out[i]);
    }
    const int16_t *live = (const int16_t *)&sprobe_ram[1];
    fprintf(stderr, "cycles max %lu mean %.0f guard %04x%04x sprobe %lu %d %d %d %d %d %d %d %d\n", (unsigned long)cmax,
            csum / calls, (uint16_t)out[128], (uint16_t)out[129], (unsigned long)sprobe_ram[0], live[0], live[1], live[2],
            live[3], live[4], live[5], live[6], live[7]);
    return 0;
}
