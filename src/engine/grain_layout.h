/* Granular source bank layout, shared by grain.c and the screen (src/slot/ui.c). Must match
 * gen_grain_data.py: int16 overview[GRAIN_OVERVIEW] then int16 samples[GRAIN_TOTAL + 1] (guard = first sample),
 * 44.1 kHz, root C4. */
#ifndef GRAIN_LAYOUT_H
#define GRAIN_LAYOUT_H

#define GRAIN_SOURCES 6
#define GRAIN_SOURCE_LEN 70560                      /* 1.6 s */
#define GRAIN_TOTAL (GRAIN_SOURCES * GRAIN_SOURCE_LEN)
#define GRAIN_OVERVIEW 256
#define GRAIN_ROOT_INC 25480314u                    /* C4 phase increment, 2^32 per cycle at 44.1 kHz */

#endif
