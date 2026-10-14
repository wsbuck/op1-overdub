/* Knob probe (sprobe.c engine, src/slot/knobhook.c encoder hook, its screen in src/slot/ui.c): shared state and the
 * packing the hook writes into synth knobs 6..7. Research and route: notes/knob-hook.md.
 *
 * Range row (engines.json "ranges"): type 3 for every knob (dsynth's: knob k clamps against its own row, no SHIFT
 * fine remap), knobs 0..5 0..0x7FFF, knobs 6..7 -32768..32767 so every packed value passes TE's patch validator
 * (it rejects knob values outside the row, error 15).
 *
 * Packing, written by the hook on every encoder turn while sprobe is the slot's engine:
 *   K6 = 0x8000 | (edits & 0xFFF) << 3 | last knob   always negative as int16: tests a signed value
 *   K7 = 0x8000 | (K6 * 977 + 677) & 0x7FFF          a raw bit pattern above 0x7FFF, derived from K6
 * edits continues from the K6 the sound holds, so it counts the sound's edits across save and load. K7 = f(K6) lets
 * the screen check a loaded pair without any RAM state (a clamp or rewrite on load breaks the pair).
 */
#ifndef SPROBE_H
#define SPROBE_H
#include <stdint.h>

#define SP_K6 6
#define SP_K7 7

static inline uint32_t sp_edits(int16_t k6)
{
    return (uint16_t)k6 & 0x8000 ? ((uint16_t)k6 >> 3) & 0xFFF : 0;
}

static inline int16_t sp_k6(uint32_t edits, int32_t knob)
{
    return (int16_t)(uint16_t)(0x8000 | (edits & 0xFFF) << 3 | (knob & 7));
}

static inline int16_t sp_k7(int16_t k6)
{
    return (int16_t)(uint16_t)(0x8000 | (((uint32_t)(uint16_t)k6 * 977 + 677) & 0x7FFF));
}

/* Engine -> screen (FREE SDRAM, engines.json "ram"; the first 256 B are zero at boot). */
struct sp_ram {
    uint32_t renders;                      /* render calls */
    int16_t live[8];                       /* the last render call's params (TE's per-voice copy, after the LFO) */
};
extern struct sp_ram sprobe_ram;

/* Hook -> screen (slot block .data). */
struct kh_diag {
    uint32_t calls;                        /* hook calls since boot (encoder turns on sprobe's engine page) */
    int32_t knob, delta;                   /* the last turn */
    int16_t w6, w7;                        /* what the hook last wrote into knobs 6, 7 */
    int32_t slot;                          /* the slot it wrote */
};
extern struct kh_diag knobhook_diag;

#endif
