/* Simulator driver for fx_abi_check.py: runs our FX through their firmware entry wrappers (fx_abi.S).
 *   fx_abi FX MODE BLOCKS     FX 0 ensemble, 1 pingpong, 2 distortion, 3 crush, 4 repeat, 5 space
 *   MODE m: mono only; s: stereo only; i: mono and stereo interleaved (as when a synth FX and the master FX run
 *   in one audio callback); d: stereo with mix 0 (must be the dry input: proves the 4th argument arrives; crush has
 *   no mix knob: every knob 0 is its bit-exact clean setting; repeat: FORTUNE OFF never fires: dry, bit exact).
 * stdout: per block "m" then 64 samples and/or "s" then 64 L and 64 R; stderr/exit 1 on any ABI mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

volatile int32_t te_tempo_spb = 22050 << 14;    /* pingpong.c: TE's tempo word (0x01AADAD8 on the device), 120 BPM */
/* repeat.c's bar clock: the tape playing (the playhead advances a block per call), the metronome, the slot */
volatile int32_t te_tape_state = 2, te_metro_count = 22050 << 14, te_play_slot = 0;
volatile uint32_t te_tape_pos = 0;
/* repeat.c's manual key: TE's block counter and the drum / mixer controllers' press and release (stand-ins) */
volatile uint32_t te_blocks;
int te_drum_press(void *self, int key) { (void)self; (void)key; return 0; }
int te_drum_release(void *self, int key) { (void)self; (void)key; return 0; }
int te_master_press(void *self, int key) { (void)self; (void)key; return 0; }
int te_master_release(void *self, int key) { (void)self; (void)key; return 0; }
/* space.c: the slots and stored FX knobs TE keeps (its TAIL zone on synth / drum) */
volatile int32_t te_fx_slot[2];
volatile int16_t te_fx_knobs_synth[64], te_fx_knobs_drum[64];

typedef void (*entry_t)(void);
int fx_abi_call(entry_t entry, void *self, void *a1, void *a2, void *a3);
extern void ensemble_mono_entry(void), ensemble_stereo_entry(void), pingpong_mono_entry(void),
    pingpong_stereo_entry(void), distortion_mono_entry(void), distortion_stereo_entry(void), crush_mono_entry(void),
    crush_stereo_entry(void), repeat_mono_entry(void), repeat_stereo_entry(void), space_mono_entry(void),
    space_stereo_entry(void);
static const entry_t mono[6] = { ensemble_mono_entry, pingpong_mono_entry, distortion_mono_entry, crush_mono_entry,
                                 repeat_mono_entry, space_mono_entry };
static const entry_t stereo[6] = { ensemble_stereo_entry, pingpong_stereo_entry, distortion_stereo_entry,
                                   crush_stereo_entry, repeat_stereo_entry, space_stereo_entry };

static int16_t sine(uint32_t ph)
{
    int32_t x = (int32_t)ph >> 16;
    return (int16_t)(((x * (32767 - (x < 0 ? -x : x))) >> 13) >> 1);
}

static void knobs(int b, int16_t *k, int dry, int fx)
{
    if (dry && fx == 3) {                        /* crush: every knob 0 = clean */
        k[0] = k[1] = k[2] = k[3] = k[4] = k[5] = k[6] = k[7] = 0;
        return;
    }
    k[0] = (int16_t)((b * 1777) & 0x7FFF);
    k[1] = (int16_t)((b * 911 + 9000) & 0x7FFF);
    k[2] = (int16_t)((b * 3001) & 0x7FFF);
    k[3] = dry ? 0 : 24000;
    k[4] = k[5] = k[6] = k[7] = 0;
    if (fx == 4) {                               /* repeat: FORTUNE through ALWAYS, CHANCE, VARY .., or OFF (dry) */
        k[1] = dry ? 1024 : (int16_t)((b / 40 * 5000) & 0x7FFF);
        k[3] = (int16_t)((b * 523) & 0x7FFF);
    }
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
        knobs(b, k, mode == 'd', fx);
        te_tape_pos += 64 * 64;
        te_blocks++;
        te_metro_count -= 64 << 14;
        if (te_metro_count <= 0)
            te_metro_count += te_tempo_spb;
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
