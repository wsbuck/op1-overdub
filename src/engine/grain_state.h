/* Per-voice state of the granular engine, shared with the screen (ui.c draws the live grains).
 * Lives in a 4 KB voice_ram slot (see modal_state.h; one engine renders at a time). */
#ifndef GRAIN_STATE_H
#define GRAIN_STATE_H
#include <stdint.h>

#define GRAINS 8

struct grain {
    uint32_t pos;       /* read position in the bank, Q12 samples */
    uint32_t wph;       /* window phase, 2^32 = whole grain */
    uint32_t wstep;     /* window phase step per sample; 0 = inactive */
    int32_t mul;        /* pitch multiplier, Q14 (1.0 = 16384; octave shimmer = 32768) */
};

struct grain_voice {
    uint32_t owner;     /* OWNER_GRAIN (modal_state.h) */
    struct grain g[GRAINS];
    int32_t next;       /* samples until the next grain */
    uint32_t rng;
    int32_t peak;       /* last block's output peak (Q15), for the screen */
    uint32_t src_key;   /* identity of the source the grains point into (grain_source.h); change = clear */
};

#endif
