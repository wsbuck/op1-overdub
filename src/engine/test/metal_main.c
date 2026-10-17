/* Simulator driver for metal_check.py: plays the metal engine (src/slot/metal.c) like TE's drum voice loop
 * (notes/drum-engine.md), every call through its firmware entry wrapper with fx_abi.S's register sentinels.
 *   metal_main KEY VEL BLOCKS P0 .. P7 [chg BLOCK KNOB VALUE] ... [retrig BLOCK] ... [ramp B0 B1 KNOB V0 V1] ...
 *       one note on voice 3: slot init, note-on, BLOCKS x (render, active); "chg" sets a knob of the key before that
 *       block's render (TE copies the key's params every block), "retrig" sends another note-on before that block,
 *       "reinit" runs the slot init (TE's factory: a kit loaded) and then a note-on before that block,
 *       "ramp" moves a knob linearly from V0 at block B0 to V1 at B1 (a slow turn, or TE's LFO),
 *       "step N KNOB V0 DV COUNT" sets the knob to V0 + j DV and hits the key again every N blocks (a detent ladder)
 *   metal_main poly BLOCKS N P0..P7 x N
 *       N voices (0..N-1, keys 0..N-1) start together and all render every block: stdout their mix (sum / 4, clamped),
 *       stderr "block <worst sum of the N render calls' cycles in one block> call <worst single call>"
 *   metal_main init                              slot init cases (loaded values kept, range row copy)
 *   metal_main.choke K1 K2 BLOCK BLOCKS P0..P7 Q0..Q7
 *       voice 3 plays key K1 (values P), voice 4 key K2 (values Q) from BLOCK (hi-hat choke); both render every block;
 *       stdout: voice 3's samples; stderr: "end <block voice 3 went silent, or -1>"
 * stdout: the samples, int16 little-endian; stderr: "cycles max <n> guard <hex> vptr <hex> end <block> active <a> ..."
 * (active() after each of the first 64 blocks; end = the first block after which active() was 0, or -1). Exit 1 on an
 * ABI mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "metal.h"

typedef void (*entry_t)(void);
int fx_abi_call(entry_t entry, void *self, void *a1, void *a2, void *a3);
extern void mt_render_entry(void), mt_note_on_entry(void), mt_active_entry(void), mt_note_off_entry(void),
    mt_slot_init_entry(void);
extern int32_t metal_active(void *voice);

/* TE's drum-side state (device: fixed addresses) */
volatile int32_t te_drum_key[DP_VOICES];
volatile int16_t te_drum_vel[DP_VOICES];
volatile int32_t te_drum_ratio[DP_VOICES];
volatile int32_t te_drum_edit;
int16_t te_drum_params[8 * DP_KEYS * 8];
int16_t ranges[30 * 32];
const int32_t metal_id = 25;

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
        ranges[25 * 32 + i] = (int16_t)(i % 4 == 1 || i % 4 == 2 ? 0x7FFF : 0);
    call(mt_slot_init_entry, (void *)2, 0, 0);
    int ok_row = metal_diag.row_copied == 1 && ranges[25 * 32] == -24320 && ranges[25 * 32 + 2] == 0x2000;
    int ok_kept = metal_diag.inits == 1;
    for (int i = 0; i < DP_KEYS * 8; i++)                    /* loaded values never rewritten */
        ok_kept &= k[i] == (int16_t)(i * 77 - 5000);
    ranges[9 * 32 + 4 * 3] = ranges[9 * 32 + 4 * 3 + 1];     /* row 9 not filled yet: our row stays */
    metal_diag.row_copied = 0;
    ranges[25 * 32] = 0;
    call(mt_slot_init_entry, (void *)0, 0, 0);
    int ok_norow = metal_diag.row_copied == 0 && ranges[25 * 32] == 0;
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
    call(mt_note_on_entry, voices[v], kp, 0);
}

static int poly_case(int argc, char **argv)
{
    static int16_t out[128], params[8], mix[128];
    int blocks = atoi(argv[2]), n = atoi(argv[3]);
    if (n < 1 || n > DP_VOICES || argc != 4 + 8 * n)
        return 2;
    for (int v = 0; v < n; v++)
        for (int i = 0; i < 8; i++)
            te_drum_params[v * 8 + i] = (int16_t)atoi(argv[4 + v * 8 + i]);
    call(mt_slot_init_entry, (void *)0, 0, 0);
    for (int v = 0; v < n; v++)
        note_on(v, v, 127, te_drum_params + v * 8);
    uint32_t bmax = 0, cmax = 0;
    for (int b = 0; b < blocks; b++) {
        int32_t acc[128];
        uint32_t sum = 0;
        for (int i = 0; i < 128; i++)
            acc[i] = 0;
        for (int v = 0; v < n; v++) {
            memcpy(params, te_drum_params + v * 8, sizeof params);
            uint32_t t0 = cycles();
            call(mt_render_entry, voices[v], out, params);
            uint32_t dt = cycles() - t0;
            sum += dt;
            cmax = dt > cmax ? dt : cmax;
            for (int i = 0; i < 128; i++)
                acc[i] += out[i];
        }
        bmax = sum > bmax ? sum : bmax;
        for (int i = 0; i < 128; i++) {
            int32_t m = acc[i] >> 2;
            mix[i] = (int16_t)(m > 32767 ? 32767 : m < -32767 ? -32767 : m);
        }
        fwrite(mix, 2, 128, stdout);
    }
    fprintf(stderr, "block %lu call %lu\n", (unsigned long)bmax, (unsigned long)cmax);
    return mask ? 1 : 0;
}

static int choke_case(char **argv)
{
    static int16_t out[128], params[8];
    int k1 = atoi(argv[2]), k2 = atoi(argv[3]), at = atoi(argv[4]), blocks = atoi(argv[5]), end = -1;
    int16_t *p1 = te_drum_params + k1 * 8, *p2 = te_drum_params + k2 * 8;
    for (int i = 0; i < 8; i++) {
        p1[i] = (int16_t)atoi(argv[6 + i]);
        p2[i] = (int16_t)atoi(argv[14 + i]);
    }
    call(mt_slot_init_entry, (void *)0, 0, 0);
    note_on(3, k1, 127, p1);
    for (int b = 0; b < blocks; b++) {
        if (b == at)
            note_on(4, k2, 127, p2);
        memcpy(params, p1, sizeof params);
        call(mt_render_entry, voices[3], out, params);
        fwrite(out, 2, 128, stdout);
        if (end < 0 && !metal_active(voices[3]))
            end = b;
        if (b >= at) {
            memcpy(params, p2, sizeof params);
            call(mt_render_entry, voices[4], out, params);
        }
    }
    fprintf(stderr, "end %d\n", end);
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
    if (argc == 22 && !strcmp(argv[1], "choke"))
        return choke_case(argv);
    if (argc >= 4 && !strcmp(argv[1], "poly"))
        return poly_case(argc, argv);
    if (argc < 12)
        return 2;
    int key = atoi(argv[1]), vel = atoi(argv[2]), blocks = atoi(argv[3]), v = 3;
    int chg_b[16], chg_k[16], chg_v[16], nchg = 0, rt_b[8], nrt = 0, ri_b[4], nri = 0;
    int rp_b0[4], rp_b1[4], rp_k[4], rp_v0[4], rp_v1[4], nrp = 0;
    int st_n = 0, st_k = 0, st_v0 = 0, st_dv = 0, st_cnt = 0;      /* "step N KNOB V0 DV COUNT": a ladder */
    for (int i = 12; i < argc; i++) {
        if (!strcmp(argv[i], "chg") && i + 3 < argc && nchg < 16) {
            chg_b[nchg] = atoi(argv[i + 1]);
            chg_k[nchg] = atoi(argv[i + 2]);
            chg_v[nchg++] = atoi(argv[i + 3]);
            i += 3;
        } else if (!strcmp(argv[i], "retrig") && i + 1 < argc && nrt < 8) {
            rt_b[nrt++] = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "reinit") && i + 1 < argc && nri < 4) {
            ri_b[nri++] = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "step") && i + 5 < argc) {
            st_n = atoi(argv[i + 1]);
            st_k = atoi(argv[i + 2]);
            st_v0 = atoi(argv[i + 3]);
            st_dv = atoi(argv[i + 4]);
            st_cnt = atoi(argv[i + 5]);
            i += 5;
        } else if (!strcmp(argv[i], "ramp") && i + 5 < argc && nrp < 4) {
            rp_b0[nrp] = atoi(argv[i + 1]);
            rp_b1[nrp] = atoi(argv[i + 2]);
            rp_k[nrp] = atoi(argv[i + 3]);
            rp_v0[nrp] = atoi(argv[i + 4]);
            rp_v1[nrp++] = atoi(argv[i + 5]);
            i += 5;
        }
    }
    call(mt_slot_init_entry, (void *)0, 0, 0);
    int16_t *kp = te_drum_params + key * 8;
    for (int i = 0; i < 8; i++)
        kp[i] = (int16_t)atoi(argv[4 + i]);
    note_on(v, key, vel, kp);
    out[128] = out[129] = 0x5a5a;
    uint32_t cmax = 0, csum = 0;
    int end = -1;
    char act[2048], *a = act;
    for (int b = 0; b < blocks; b++) {
        for (int j = 0; j < nchg; j++)
            if (chg_b[j] == b)
                kp[chg_k[j]] = (int16_t)chg_v[j];
        for (int j = 0; j < nrp; j++)
            if (b >= rp_b0[j] && b <= rp_b1[j])
                kp[rp_k[j]] = (int16_t)(rp_v0[j] + (int)((long long)(rp_v1[j] - rp_v0[j]) * (b - rp_b0[j])
                                                          / (rp_b1[j] > rp_b0[j] ? rp_b1[j] - rp_b0[j] : 1)));
        for (int j = 0; j < nrt; j++)
            if (rt_b[j] == b)
                note_on(v, key, vel, kp);
        if (st_n > 0 && b % st_n == 0 && b / st_n < st_cnt) {   /* the ladder: the knob's next value, a new hit */
            int32_t val = st_v0 + st_dv * (b / st_n);
            kp[st_k] = (int16_t)(val > 32767 ? 32767 : val);
            if (b)
                note_on(v, key, vel, kp);
        }
        for (int j = 0; j < nri; j++)
            if (ri_b[j] == b) {                              /* the slot reloaded mid-note (factory: slot init), */
                call(mt_slot_init_entry, (void *)0, 0, 0);   /* then the key again */
                note_on(v, key, vel, kp);
            }
        memcpy(params, kp, sizeof params);                   /* TE copies the key's params for each block */
        uint32_t t0 = cycles();
        call(mt_render_entry, voices[v], out, params);
        uint32_t dt = cycles() - t0;
        cmax = dt > cmax ? dt : cmax;
        csum += dt;
        call(mt_active_entry, voices[v], 0, 0);
        int32_t r = metal_active(voices[v]);
        if (r == 0 && end < 0)
            end = b;
        if (b < 64)
            a += sprintf(a, " %ld", (long)r);
        fwrite(out, 2, 128, stdout);
    }
    call(mt_note_off_entry, voices[v], kp, 0);
    fprintf(stderr, "cycles max %lu mean %lu guard %04x%04x vptr %08lx end %d noteons %lu active%s\n", (unsigned long)cmax,
            (unsigned long)(csum / (blocks ? blocks : 1)), (uint16_t)out[128], (uint16_t)out[129], (unsigned long)voices[v][0], end,
            (unsigned long)metal_diag.noteons, act);
    if (mask)
        fprintf(stderr, "abi mismatch mask %08x\n", mask);
    return mask ? 1 : 0;
}
