/* The SAMPLE bank: a 9th wavetable bank built from the sampler slot's sample (notes/wavetable-custom.md).
 * Shared by wt.c (audio: reads `front` once per block) and src/slot/ui.c (UI thread: builds with wt_import.h).
 * Lives in FREE region 2 at _wt_ram (engines.json "ram"); the header is zero-filled by the image at boot, so
 * front = 0 (nothing built) until the UI publishes a bank. Double-buffered: the UI builds into the bank the
 * engine is not playing, then publishes it with one aligned 32-bit store. */
#ifndef WT_USER_H
#define WT_USER_H
#include <stdint.h>
#include "wt_layout.h"

#define WT_SAMPLE_BANK BANKS                /* bank index 8 */
#define WT_SAMPLE_KNOB 31744                /* green at or above this: SAMPLE (presets sit at b * 4096 + 2048) */
#define WT_BANK_SAMPLES (FRAMES * FRAME_STRIDE)
#define WT_USER_HDR 256

struct wt_user {
    volatile int32_t front;                 /* 0: nothing built; 1 or 2: bank[front - 1] is the SAMPLE bank */
    int32_t building;                       /* 0 idle, else 1 + the bank being built */
    uint32_t sig, built_sig;                /* source signature now / of the last build (0: none) */
    int32_t stable;                         /* UI frames the signature has held */
    int32_t pitched;                        /* of the published bank: 2 grid, 1 detected period, 0 slices */
    uint32_t period;                        /* of the published bank, samples Q16 */
    int32_t slot;                           /* sampler slot of the published bank, -1 none */
    int32_t spare[WT_USER_HDR / 4 - 8];
    int16_t bank[2][WT_BANK_SAMPLES];       /* then struct wti_work (ui.c) */
};

extern struct wt_user wt_ram __attribute__((weak));   /* absent in builds without wavetable */

/* Wavetable knob [1] -> bank 0..8 */
static inline int wt_bank_of(int32_t k)
{
    if (k >= WT_SAMPLE_KNOB)
        return WT_SAMPLE_BANK;
    return k < 0 ? 0 : k >> 12;
}

/* First sample of bank b in `tables` (the built-in banks); SAMPLE plays the published bank, or bank 0 before one
 * exists. */
static inline const int16_t *wt_bank_base(const int16_t *tables, int b)
{
    if (b == WT_SAMPLE_BANK) {
        int32_t f = wt_ram.front;
        return f == 1 || f == 2 ? wt_ram.bank[f - 1] : tables;
    }
    return tables + b * WT_BANK_SAMPLES;
}

#endif
