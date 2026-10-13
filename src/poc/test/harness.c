/* Simulator harness for proof-of-life renders: calls render() like TE's voice loop and dumps samples.
 * Built with -msim and run under bfin-elf-run (see src/poc/test/run.sh). */
#include <stdio.h>
#include <stdint.h>

struct voice { uint32_t vptr, phase_a; uint8_t rest[28]; };
extern void render(struct voice *self, int16_t *out, const int16_t *params);
uint32_t prof_stats[6];        /* tramp.S profiling (device: FREE region 0) */
uint8_t voice_ram[8 * 4096];   /* per-voice engine RAM (device: FREE region 0 at 0x00810000) */
/* TE state that grain_source.h reads (device: fixed addresses). No sampler slot unless set up. */
volatile int32_t te_engine_ids[8];
int32_t *te_ssamp[8];
volatile int16_t te_knobs[8][8];
volatile int32_t te_synth_slot;
int32_t pitch_buf[128];   /* stands in for the firmware's per-voice pitch buffer at 0xFF801950 */
int32_t iter_buf[64];     /* stands in for iter's private buffer at 0x01DD4E18 */
/* Stand-ins for canary.S's SDRAM regions; run canary with --defsym REGION_BYTES=4096 */
#define CANARY_WORDS 1024
#ifndef CALLS
#define CALLS 40          /* render calls; the FSK loggers need ~400 per frame (-DCALLS=4000) */
#endif
int32_t canary0[CANARY_WORDS], canary1[CANARY_WORDS], canary2[CANARY_WORDS], canary3[CANARY_WORDS];

int main(void)
{
    static struct voice v = { 0, 0x80000000u };          /* note-on state */
    static struct voice v2 = { 0, 0x80000000u };         /* -DSILENT_VOICE: a second voice, output dropped */
    static int16_t out[128 + 2];
    static const int16_t params[8] = { 1516, 16704, 0, 15168 };
    /* Pitch buffer the firmware fills per voice: int32 phase increment per sample (2^32 = 1 cycle).
     * CALLS/2 calls at C4 (261.63 Hz), then the rest at C5 (523.25 Hz). Ignored by renders that don't read it. */
    volatile int32_t *pitch = pitch_buf;
    volatile int32_t *c2 = canary2;
    int32_t *regions[] = { canary0, canary1, canary2, canary3 };
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < CANARY_WORDS; i++) regions[r][i] = 0x5AA5C33C;
    out[128] = 0x5a5a; out[129] = 0x5a5a;                /* guard: render must write exactly 128 */
    for (int call = 0; call < CALLS; call++) {
        uint32_t inc = call < CALLS / 2 ? 25480314u : 50960628u; /* round(f/44100 * 2^32) */
        for (int i = 0; i < 128; i++) pitch[i] = (int32_t)inc;
        if (call == 25) c2[700] = 0;                    /* canary.S: damage region 2 mid-run */
        render(&v, out, params);
#ifdef SILENT_VOICE
        { static int16_t junk[128]; render(&v2, junk, params); }
#endif
        for (int i = 0; i < 128; i++) printf("%d\n", out[i]);
    }
    fprintf(stderr, "guard %04x %04x phase %08lx iter_buf %08lx %08lx\n", (uint16_t)out[128], (uint16_t)out[129],
            (unsigned long)v.phase_a, (unsigned long)iter_buf[0], (unsigned long)iter_buf[1]);
    fprintf(stderr, "damaged word canary2[700] at %08lx\n", (unsigned long)&canary2[700]);
    return 0;
}
