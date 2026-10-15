/* fmdrum (fmdrum.c, a drum engine) and its screen (ui_fmdrum.h): the per-voice state the screen reads, and the knob
 * decoding both share. TE's drum-side arrays (te_drum_key, te_drum_vel, te_drum_edit, te_drum_params, ranges) are
 * declared in drumprobe.h. Design and knob mapping: notes/drum-engine.md "fmdrum", src/slot/gen_fmdrum.py.
 */
#ifndef FMDRUM_H
#define FMDRUM_H
#include <stdint.h>
#include "drumprobe.h"

#define FD_VOICES DP_VOICES
#define FD_KEYS DP_KEYS

/* the 8 per-key values: an amount on each plain knob, its time (or partner) on SHIFT */
enum { FD_PITCH, FD_SWEEP, FD_FM, FD_DECAY,         /* blue, green, white, orange */
       FD_RATIO, FD_SWDEC, FD_FMDEC, FD_NOISE };    /* SHIFT + blue, green, white, orange */
#define FD_CHOKE_LO 10                       /* hi-hat choke group: keys 10..13, voices whose NOISE is in the HAT zone */
#define FD_CHOKE_HI 13

/* One voice. The audio thread (render) owns everything except `trig`, `key`, `vel`, which note-on (TE's caller thread)
 * writes before bumping `trig`; render restarts the voice when `trig` moved. The screen only reads. */
struct fd_voice {
    uint32_t trig, seen;                    /* note-ons written / taken by render */
    int32_t key, vel;                       /* key 0..23, velocity Q15 */
    uint32_t age;                           /* blocks since the note-on, 0 = silent */
    uint32_t cph, mph;                      /* carrier, modulator phase */
    int32_t aenv, menv, senv;               /* amplitude, modulator, sweep envelopes, Q30 (start 1.0) */
    int32_t gt, gn, gm;                     /* tone gain Q23, noise gain Q23, modulation depth (phase units per Q15) */
    uint32_t cinc, minc;                    /* increments at the end of the last block (the next block's start) */
    uint32_t seed;
    int32_t lp, bp;                         /* noise filter */
    int32_t dcx, dcy;                       /* DC blocker, Q23 */
    int32_t index_q8;                       /* index (rad, Q8) at the end of the last block, after the limiter */
    uint32_t choke;                         /* set by another hat's note-on (choke group): release in ~5 ms */
};

struct fd_diag {
    uint32_t noteons, renders, cyc_max, inits, row_copied;
    int32_t last_voice;                     /* the newest note-on's voice */
};

extern struct fd_voice fmdrum_voice[FD_VOICES];
extern struct fd_diag fmdrum_diag;
extern const int32_t fmdrum_id;             /* our drum engine id (slot_gen.inc) */

static inline int32_t fd_pos(int16_t k)
{
    return k < 0 ? 0 : k;
}

/* FM -> peak index, rad Q8: 16 rad x (k / 32767)^2 */
static inline int32_t fd_index_q8(int16_t k2)
{
    int32_t k = fd_pos(k2);
    return (((k * k) >> 15) * 4096) >> 15;
}

/* SWEEP -> pitch offset at the start, PITCH units (512 per semitone): 48 semitones x (k / 32767)^2 */
static inline int32_t fd_sweep_q(int16_t k4)
{
    int32_t k = fd_pos(k4);
    return (((k * k) >> 15) * (48 * 512)) >> 15;
}

/* NOISE zone: 0 OFF, 1 AIR, 2 SNARE, 3 HAT, 4 CLAP (fd_noise_zone[]) */
static inline int fd_noise_zone_of(int16_t kn)
{
    int32_t k = fd_pos(kn);
    return k < 1024 ? 0 : 1 + (k >> 13 > 3 ? 3 : k >> 13);
}

#endif
