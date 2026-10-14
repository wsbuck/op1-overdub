/* Simulator driver for drumprobe_check.py: plays the drum probe like TE's drum voice loop (notes/drum-engine.md),
 * every call through its firmware entry wrapper with the ABI sentinel check (fx_abi.S).
 *   drumprobe_main KEY VEL BLOCKS [P0 .. P7]   one note: slot init, note-on on voice 3, BLOCKS renders
 *   drumprobe_main init                        slot init cases (loaded values kept, range row copy)
 * stdout: one sample per line; stderr: "cycles max <n> guard <hex> active <a0> <a1> ... <aBLOCKS-1> noteons <n>"
 * (active() after each block). Exit 1 on an ABI mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "drumprobe.h"

typedef void (*entry_t)(void);
int fx_abi_call(entry_t entry, void *self, void *a1, void *a2, void *a3);
extern void dp_render_entry(void), dp_note_on_entry(void), dp_active_entry(void), dp_note_off_entry(void),
    dp_slot_init_entry(void);
extern int32_t drumprobe_active(void *voice);

/* TE's drum-side state (device: fixed addresses) */
volatile int32_t te_drum_key[DP_VOICES];
volatile int16_t te_drum_vel[DP_VOICES];
volatile int32_t te_drum_ratio[DP_VOICES];
volatile int32_t te_drum_edit;
int16_t te_drum_params[8 * DP_KEYS * 8];
int16_t ranges[20 * 32];
const int32_t drum_id = 18;

static uint32_t voices[DP_VOICES][18];      /* TE's dbox objects: vptr, ..., +0x14 = voice index */
static int mask;

static int32_t call(entry_t e, void *self, void *a1, void *a2)
{
    int m = fx_abi_call(e, self, a1, a2, 0);
    mask |= m;
    return m;
}

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c));
    return c;
}

static int init_cases(void)
{
    int16_t *k = te_drum_params + 2 * DP_KEYS * 8;
    for (int i = 0; i < DP_KEYS * 8; i++)
        k[i] = (int16_t)(i * 77 - 5000);                     /* the values TE's loader put there (dbox_data) */
    for (int i = 0; i < 8; i++) {                            /* row 9 as TE fills it: sane */
        ranges[9 * 32 + 4 * i] = (int16_t)(i ? 0 : -32767);
        ranges[9 * 32 + 4 * i + 1] = 32767;
        ranges[9 * 32 + 4 * i + 2] = 0x2000;
    }
    for (int i = 0; i < 32; i++)
        ranges[18 * 32 + i] = (int16_t)(i % 4 == 1 || i % 4 == 2 ? 0x7FFF : 0);
    call(dp_slot_init_entry, (void *)2, 0, 0);
    int ok_row = drumprobe_diag.row_copied == 1 && ranges[18 * 32] == -32767 && ranges[18 * 32 + 2] == 0x2000;
    call(dp_slot_init_entry, (void *)(2 + 8), 0, 0);         /* slot & 7 */
    int ok_kept = drumprobe_diag.inits == 2;
    for (int i = 0; i < DP_KEYS * 8; i++)                    /* loaded values never rewritten */
        ok_kept &= k[i] == (int16_t)(i * 77 - 5000);
    ranges[9 * 32 + 4 * 3] = ranges[9 * 32 + 4 * 3 + 1];     /* row 9 not filled yet: our row stays */
    drumprobe_diag.row_copied = 0;
    ranges[18 * 32] = 0;
    call(dp_slot_init_entry, (void *)0, 0, 0);
    int ok_norow = drumprobe_diag.row_copied == 0 && ranges[18 * 32] == 0;
    printf("kept %d row %d norow %d\n", ok_kept, ok_row, ok_norow);
    if (mask)
        fprintf(stderr, "abi mismatch mask %08x\n", mask);
    return mask ? 1 : 0;
}

int main(int argc, char **argv)
{
    static int16_t out[130];
    static int16_t params[8];
    __asm__ volatile ("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* CCEN: run CYCLES */
    for (int v = 0; v < DP_VOICES; v++) {
        voices[v][0] = 0x0190F8A4;
        voices[v][5] = (uint32_t)v;
        for (int i = 1; i < 18; i++)
            if (i != 5)
                voices[v][i] = 0xA5A5A5A5u * (uint32_t)(i + v);     /* dbox's constructor state: not ours */
    }
    if (argc == 2 && !strcmp(argv[1], "init"))
        return init_cases();
    if (argc < 4)
        return 2;
    int key = atoi(argv[1]), vel = atoi(argv[2]), blocks = atoi(argv[3]), v = 3;
    call(dp_slot_init_entry, (void *)0, 0, 0);
    int16_t *kp = te_drum_params + key * 8;
    for (int i = 0; i < 8 && 4 + i < argc; i++)
        kp[i] = (int16_t)atoi(argv[4 + i]);
    te_drum_key[v] = key;
    te_drum_vel[v] = (int16_t)(vel << 8);
    te_drum_ratio[v] = 0x10000;
    te_drum_edit = key;
    call(dp_note_on_entry, voices[v], kp, 0);
    out[128] = out[129] = 0x5a5a;
    uint32_t cmax = 0;
    char act[8192], *a = act;
    for (int b = 0; b < blocks; b++) {
        memcpy(params, kp, sizeof params);                   /* TE copies the key's params for each block */
        uint32_t t0 = cycles();
        call(dp_render_entry, voices[v], out, params);
        uint32_t dt = cycles() - t0;
        cmax = dt > cmax ? dt : cmax;
        int32_t r = fx_abi_call(dp_active_entry, voices[v], 0, 0, 0);   /* returns the mask; get the value directly */
        mask |= r;
        r = drumprobe_active(voices[v]);
        if (a < act + sizeof act - 16)
            a += sprintf(a, " %ld", (long)r);
        for (int i = 0; i < 128; i++)
            printf("%d\n", out[i]);
    }
    call(dp_note_off_entry, voices[v], kp, 0);
    fprintf(stderr, "cycles max %lu guard %04x%04x noteons %lu noteoffs %lu vptr %08lx active%s\n", (unsigned long)cmax,
            (uint16_t)out[128], (uint16_t)out[129], (unsigned long)drumprobe_diag.noteons,
            (unsigned long)drumprobe_diag.noteoffs, (unsigned long)voices[v][0], act);
    if (mask)
        fprintf(stderr, "abi mismatch mask %08x\n", mask);
    return mask ? 1 : 0;
}
