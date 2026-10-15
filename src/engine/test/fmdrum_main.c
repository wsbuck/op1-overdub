/* Simulator driver for fmdrum_check.py: plays the fmdrum engine (src/slot/fmdrum.c) like TE's drum voice loop
 * (notes/drum-engine.md), every call through its firmware entry wrapper with fx_abi.S's register sentinels.
 *   fmdrum_main KEY VEL BLOCKS P0 .. P7 [chg BLOCK KNOB VALUE] ... [retrig BLOCK] ...
 *       one note on voice 3: slot init, note-on, BLOCKS x (render, active); "chg" sets a knob of the key before that
 *       block's render (TE copies the key's params every block), "retrig" sends another note-on before that block
 *   fmdrum_main init                              slot init cases (loaded values kept, range row copy)
 * stdout: the samples, int16 little-endian; stderr: "cycles max <n> guard <hex> vptr <hex> end <block> active <a> ..."
 * (active() after each of the first 64 blocks; end = the first block after which active() was 0, or -1). Exit 1 on an
 * ABI mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fmdrum.h"

typedef void (*entry_t)(void);
int fx_abi_call(entry_t entry, void *self, void *a1, void *a2, void *a3);
extern void fd_render_entry(void), fd_note_on_entry(void), fd_active_entry(void), fd_note_off_entry(void),
    fd_slot_init_entry(void);
extern int32_t fmdrum_active(void *voice);

/* TE's drum-side state (device: fixed addresses) */
volatile int32_t te_drum_key[DP_VOICES];
volatile int16_t te_drum_vel[DP_VOICES];
volatile int32_t te_drum_ratio[DP_VOICES];
volatile int32_t te_drum_edit;
int16_t te_drum_params[8 * DP_KEYS * 8];
int16_t ranges[30 * 32];
const int32_t fmdrum_id = 24;

static uint32_t voices[DP_VOICES][18];      /* TE's dbox objects: vptr, ..., +0x14 = voice index */
static int mask;

static void call(entry_t e, void *self, void *a1, void *a2)
{
    mask |= fx_abi_call(e, self, a1, a2, 0);
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
        ranges[9 * 32 + 4 * i] = (int16_t)(i ? 0 : -24320);
        ranges[9 * 32 + 4 * i + 1] = 24832;
        ranges[9 * 32 + 4 * i + 2] = 0x2000;
    }
    for (int i = 0; i < 32; i++)
        ranges[24 * 32 + i] = (int16_t)(i % 4 == 1 || i % 4 == 2 ? 0x7FFF : 0);
    call(fd_slot_init_entry, (void *)2, 0, 0);
    int ok_row = fmdrum_diag.row_copied == 1 && ranges[24 * 32] == -24320 && ranges[24 * 32 + 2] == 0x2000;
    int ok_kept = fmdrum_diag.inits == 1;
    for (int i = 0; i < DP_KEYS * 8; i++)                    /* loaded values never rewritten */
        ok_kept &= k[i] == (int16_t)(i * 77 - 5000);
    ranges[9 * 32 + 4 * 3] = ranges[9 * 32 + 4 * 3 + 1];     /* row 9 not filled yet: our row stays */
    fmdrum_diag.row_copied = 0;
    ranges[24 * 32] = 0;
    call(fd_slot_init_entry, (void *)0, 0, 0);
    int ok_norow = fmdrum_diag.row_copied == 0 && ranges[24 * 32] == 0;
    printf("kept %d row %d norow %d\n", ok_kept, ok_row, ok_norow);
    if (mask)
        fprintf(stderr, "abi mismatch mask %08x\n", mask);
    return mask ? 1 : 0;
}

static void note_on(int v, int key, int vel, int16_t *kp)
{
    te_drum_key[v] = key;
    te_drum_vel[v] = (int16_t)(vel << 8);
    te_drum_ratio[v] = 0x10000;
    te_drum_edit = key;
    call(fd_note_on_entry, voices[v], kp, 0);
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
    if (argc < 12)
        return 2;
    int key = atoi(argv[1]), vel = atoi(argv[2]), blocks = atoi(argv[3]), v = 3;
    int chg_b[16], chg_k[16], chg_v[16], nchg = 0, rt_b[8], nrt = 0;
    for (int i = 12; i < argc; i++) {
        if (!strcmp(argv[i], "chg") && i + 3 < argc && nchg < 16) {
            chg_b[nchg] = atoi(argv[i + 1]);
            chg_k[nchg] = atoi(argv[i + 2]);
            chg_v[nchg++] = atoi(argv[i + 3]);
            i += 3;
        } else if (!strcmp(argv[i], "retrig") && i + 1 < argc && nrt < 8) {
            rt_b[nrt++] = atoi(argv[++i]);
        }
    }
    call(fd_slot_init_entry, (void *)0, 0, 0);
    int16_t *kp = te_drum_params + key * 8;
    for (int i = 0; i < 8; i++)
        kp[i] = (int16_t)atoi(argv[4 + i]);
    note_on(v, key, vel, kp);
    out[128] = out[129] = 0x5a5a;
    uint32_t cmax = 0;
    int end = -1;
    char act[2048], *a = act;
    for (int b = 0; b < blocks; b++) {
        for (int j = 0; j < nchg; j++)
            if (chg_b[j] == b)
                kp[chg_k[j]] = (int16_t)chg_v[j];
        for (int j = 0; j < nrt; j++)
            if (rt_b[j] == b)
                note_on(v, key, vel, kp);
        memcpy(params, kp, sizeof params);                   /* TE copies the key's params for each block */
        uint32_t t0 = cycles();
        call(fd_render_entry, voices[v], out, params);
        uint32_t dt = cycles() - t0;
        cmax = dt > cmax ? dt : cmax;
        call(fd_active_entry, voices[v], 0, 0);
        int32_t r = fmdrum_active(voices[v]);
        if (r == 0 && end < 0)
            end = b;
        if (b < 64)
            a += sprintf(a, " %ld", (long)r);
        fwrite(out, 2, 128, stdout);
    }
    call(fd_note_off_entry, voices[v], kp, 0);
    fprintf(stderr, "cycles max %lu guard %04x%04x vptr %08lx end %d noteons %lu active%s\n", (unsigned long)cmax,
            (uint16_t)out[128], (uint16_t)out[129], (unsigned long)voices[v][0], end,
            (unsigned long)fmdrum_diag.noteons, act);
    if (mask)
        fprintf(stderr, "abi mismatch mask %08x\n", mask);
    return mask ? 1 : 0;
}
