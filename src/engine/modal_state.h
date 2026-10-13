/* Per-voice state of the modal engine, shared with the screen (src/slot/ui.c reads `peak` for the crystal's
 * spin). One 4 KB slot of voice_ram per TE voice (FREE region 0 at 0x00810000 on device). */
#ifndef MODAL_STATE_H
#define MODAL_STATE_H
#include <stdint.h>

#define MODAL_MODES 20
#define VOICE_RAM_BYTES 4096
#define VOICE_RAM_SLOTS 8                  /* TE voices 0..5; slot 7 = not a TE voice (simulator) */
/* Slots are shared by every engine that keeps per-voice RAM; the first word says whose data it is. An engine
 * that finds another owner clears the slot first (modal used to resume grain's data as resonator states and
 * played loud noise; LOG.md 2026-10-08). */
#define OWNER_MODAL 0x4D4F444Cu            /* "MODL" */
#define OWNER_GRAIN 0x4752414Eu            /* "GRAN" */

/* Claim a voice_ram slot for `owner`, clearing it if it holds another engine's state. */
static inline void *voice_slot_claim(void *slot, uint32_t owner, unsigned bytes)
{
    uint32_t *w = slot;
    if (w[0] != owner) {
        for (unsigned i = 0; i < bytes / 4; i++)
            w[i] = 0;
        w[0] = owner;
    }
    return slot;
}

struct modal_voice {
    uint32_t owner;                    /* OWNER_MODAL */
    int32_t y1[MODAL_MODES], y2[MODAL_MODES];   /* resonator states, Q24 */
    int32_t exc_left, exc_amp;         /* burst samples left, amount Q15 */
    uint32_t brng;                     /* burst noise generator */
    int32_t blp, benv, bstep, bgain;   /* burst low-pass Q15, envelope Q23, step, gain Q12 */
    int32_t bplain;                    /* 1: plain noise (pluck), 0: low-passed */
    int32_t gkey, gval;                /* cached burst gain for (exciter, length, coefficient) */
    uint32_t rng;
    int32_t lp, breath_env, bow_env, rain_next, ysum, ysum_prev;
    int32_t peak;                      /* last block's output peak (Q15), for the screen */
};

#endif
