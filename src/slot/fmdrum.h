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
#define FD_CHOKE_LO 10                       /* hi-hat choke group: keys 10..13, voices whose NOISE is HAT or METAL */
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
    int32_t m1, m2;                         /* the modulator's last two outputs (FEEDBACK), Q15 */
    int32_t gf, gc, gr;                     /* feedback depth (phase units per Q15), carrier clip gain Q10 (1024 = clean),
                                             * ring-mod gain Q23 */
    uint32_t mt[6];                         /* METAL's six square phases */
    uint32_t gph;                           /* GRIND's square phase */
    int32_t mlp;                            /* METAL's squares: the one-pole high-pass's low-pass state */
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

/* FM zones (detents of 1024): the musical index up to 20480 (detent 20 = 16 rad), FEEDBACK 1..7 above it (detents
 * 21..27), SQUARE 1..5 above 27648 (detents 28..32). A value past a detent shows the next level. */
#define FD_FB0 20480
#define FD_SQ0 27648
enum { FZ_MUSIC, FZ_FEEDBACK, FZ_SQUARE };
static inline int fd_fm_zone(int16_t k2)
{
    return k2 <= FD_FB0 ? FZ_MUSIC : k2 <= FD_SQ0 ? FZ_FEEDBACK : FZ_SQUARE;
}

/* the readout's level: FEEDBACK 1..7, SQUARE 1..5 (0 below FEEDBACK) */
static inline int fd_fm_level(int16_t k2)
{
    int z = fd_fm_zone(k2);
    return z == FZ_MUSIC ? 0 : (k2 - (z == FZ_FEEDBACK ? FD_FB0 : FD_SQ0) + 1023) >> 10;
}

static inline int32_t fd_index_q8(int16_t k2)
{
    int32_t k = fd_pos(k2);
    k = k > FD_FB0 ? FD_FB0 : k;
    return (((k * k) >> 15) * 10486) >> 15;
}

/* SWEEP -> pitch offset at the start, PITCH units (512 per semitone): 48 semitones x (k / 32767)^2 */
static inline int32_t fd_sweep_q(int16_t k4)
{
    int32_t k = fd_pos(k4);
    return (((k * k) >> 15) * (48 * 512)) >> 15;
}

/* NOISE zone (fd_noise_zone[]), detents of 1024: OFF 0, AIR 1..4, SNARE 5..9, HAT 10..14, CLAP 15..19, METAL 20..25,
 * GRIND 26..32. The sound runs one detent ahead of the knob (FD_NOISE_SHIFT in fmdrum.c: a zone's first detent is
 * already 1 / n into it, its last is the zone at full), and the zone shown is the sound's. */
enum { NZ_OFF, NZ_AIR, NZ_SNARE, NZ_HAT, NZ_CLAP, NZ_METAL, NZ_GRIND };
static const int32_t fd_noise_edge[8] __attribute__((unused)) = { 0, 1024, 5120, 10240, 15360, 20480, 26624, 32768 };
/* the NOISE table point a knob value plays: one detent ahead (1023), the shift taken back across GRIND (x 1023 / 6143
 * above 26624) so the last detent is GRIND at full and the one before it differs (gen_fmdrum.py sound_k7) */
static inline int32_t fd_noise_sound(int16_t kn)
{
    int32_t k = fd_pos(kn), c = k + 1023 - (k > 26624 ? ((k - 26624) * 10914) >> 16 : 0);
    return c > 32767 ? 32767 : c;
}

static inline int fd_noise_zone_of(int16_t kn)
{
    int32_t k = fd_noise_sound(kn);
    int z = 0;
    while (z < NZ_GRIND && k >= fd_noise_edge[z + 1])
        z++;
    return z;
}

#endif
