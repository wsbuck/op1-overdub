/* Simulator ABI check for the trampoline: see abi_check.S. Prints the mismatch mask (0 = clean). */
#include <stdio.h>
#include <stdint.h>

uint32_t prof_stats[6];        /* tramp.S profiling (device: FREE region 0) */
uint8_t voice_ram[8 * 4096];   /* per-voice engine RAM (device: FREE region 0 at 0x00810000) */
/* TE state that grain_source.h reads (device: fixed addresses). No sampler slot unless set up. */
volatile int32_t te_engine_ids[8];
int32_t *te_ssamp[8];
volatile int16_t te_knobs[8][8];
volatile int32_t te_synth_slot;
int32_t pitch_buf[128];   /* stands in for the firmware's per-voice pitch buffer at 0xFF801950 */
/* Audio-input buffers that inprobe.h reads (device: TE's addresses, notes/input.md); zero here. */
volatile int16_t te_audio_in[64];
volatile uint32_t te_rx_sel;
volatile int32_t te_rx[256];
volatile int32_t te_alt_in[128];
uint32_t sprobe_ram[16];   /* knob probe's screen record (sprobe.h; device: engines.json "ram") */
uint32_t amp_ram[4096];    /* amp's shared record (amp.h; device: engines.json "ram"), zero as after boot */
extern int abi_check(void *self, int16_t *out, const int16_t *params);

int main(void)
{
    static uint32_t voice[9] = { 0x019103CC, 0x80000000u };
    static int16_t out[128];
    static const int16_t params[8] = { 1516, 16704, 0, 15168, 0, 0, -1, -32768 };
    for (int i = 0; i < 128; i++) pitch_buf[i] = 25480314;
    int mask = abi_check(voice, out, params);
    printf("abi mask %08x vptr %08lx\n", mask, (unsigned long)voice[0]);
    return mask != 0 || voice[0] != 0x019103CC;
}
