/* Where grain reads its audio: the OP-1's own synth sampler sample if a sound slot holds the sampler engine,
 * else the built-in bank. Shared by grain.c (audio) and src/slot/ui.c (screen). fw 246, notes/grain.md.
 *
 * Synth sampler (engine id 6): per sound slot a holder at te_ssamp[slot] (0x01DF6F78, 8 x SynthSampler*,
 * created once at boot, never moved or freed): holder[0] = SampleBuffer*. SampleBuffer +0 = int16 *data
 * (calloc'd), +4 = int16 peak per 256 samples, +0x1038 = length 264608 (8 pad samples + 264600 = 6 s).
 * Audio is data[8 ..]. The sampler's own knobs (te_knobs[slot][0] start, [3] end, Q15 of the 6 s) give the
 * trimmed window. Loads and recording write the buffer without locking: glitches are possible, faults not.
 * TE addresses are link-time symbols (src/engine/build.sh, tools/build_slot.py) so the simulator can fake them.
 */
#ifndef GRAIN_SOURCE_H
#define GRAIN_SOURCE_H
#include <stdint.h>

#define ENGINE_SAMPLER 6
#define SAMPLER_PAD 8
#define SAMPLER_LEN 264600u                 /* 6.000 s */
#define SAMPLER_BUF_LEN 264608              /* +0x1038 */

extern volatile int32_t te_engine_ids[8];  /* 0xFF8016E4: engine id per sound slot */
extern int32_t *te_ssamp[8];               /* 0x01DF6F78: synth sampler holders */
extern volatile int16_t te_knobs[8][8];    /* 0xFF8013C4: knobs per sound slot */
extern volatile int32_t te_synth_slot;     /* 0x01AAAC0C: the slot our producers belong to */

struct grain_src {
    const int16_t *audio;                   /* sample 0 of the source */
    uint32_t start, len;                    /* window in samples; audio[start + len] is readable */
    const int16_t *peaks;                   /* sampler: peak per 256 samples (from audio[0]); 0 for the bank */
    int slot;                               /* sampler slot (0..7), or -1 for the built-in bank */
};

/* The first other slot holding the sampler with a valid buffer, else -1. */
static inline int sampler_slot(const int32_t **sb_out)
{
    int own = te_synth_slot;
    for (int s = 0; s < 8; s++) {
        if (s == own || te_engine_ids[s] != ENGINE_SAMPLER)
            continue;
        const int32_t *h = te_ssamp[s];
        if (!h || ((uint32_t)h & 3))
            continue;
        const int32_t *sb = (const int32_t *)h[0];
        if (!sb || ((uint32_t)sb & 3) || !sb[0] || (sb[0] & 1) || sb[0x1038 / 4] != SAMPLER_BUF_LEN)
            continue;
        *sb_out = sb;
        return s;
    }
    return -1;
}

static inline struct grain_src grain_source(const int16_t *bank, uint32_t bank_len)
{
    struct grain_src g;
    const int32_t *sb = 0;
    g.slot = sampler_slot(&sb);
    if (g.slot < 0) {
        g.audio = bank;
        g.start = 0;
        g.len = bank_len;
        g.peaks = 0;
        return g;
    }
    g.audio = (const int16_t *)sb[0] + SAMPLER_PAD;
    g.peaks = (const int16_t *)(sb + 1);
    int32_t k0 = te_knobs[g.slot][0], k3 = te_knobs[g.slot][3];
    /* knob * 264600 / 32768 without overflowing 32 bits: 264600 = 33075 * 8 */
    uint32_t a = ((uint32_t)(k0 < 0 ? 0 : k0) * (SAMPLER_LEN >> 3)) >> 12;
    uint32_t b = ((uint32_t)(k3 < 0 ? 0 : k3) * (SAMPLER_LEN >> 3)) >> 12;
    if (b > SAMPLER_LEN - 1)
        b = SAMPLER_LEN - 1;                /* audio[b] then audio[b + 1] stays inside the buffer */
    if (b < a + 2205) {                     /* no usable trim (< 50 ms): the whole buffer */
        a = 0;
        b = SAMPLER_LEN - 1;
    }
    g.start = a;
    g.len = b - a;
    return g;
}

#endif
