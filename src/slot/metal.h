/* metal (metal.c, a drum engine) and its screen (ui_metal.h): the per-voice state the screen reads, and the knob
 * decoding both share. TE's drum-side arrays are declared in drumprobe.h. Design and knob mapping:
 * notes/drum-engine.md "metal", src/slot/gen_metal.py.
 */
#ifndef METAL_H
#define METAL_H
#include <stdint.h>
#include "drumprobe.h"

#define MT_VOICES DP_VOICES
#define MT_KEYS DP_KEYS
#define MT_PAIRS 4                          /* 8 modes, two per 32-bit word (vector ops) */

/* the 8 per-key values: the most played on the plain knobs (TE's LFO reaches K0..K3 only) */
enum { MT_PITCH, MT_MATERIAL, MT_DRIVE, MT_DECAY,     /* blue, green, white, orange */
       MT_STRIKE, MT_SPREAD, MT_GRIT, MT_NDECAY };     /* SHIFT + blue, green, white, orange */
#define MT_CHOKE_LO 10                       /* choke group: keys 10..13 (the hats) cut each other */
#define MT_CHOKE_HI 13

/* One voice. The audio thread (render) owns everything except `trig`, `key`, `vel`, which note-on (TE's caller thread)
 * writes before bumping `trig`; render restarts the voice when `trig` moved. The screen only reads. */
struct mt_voice {
    uint32_t trig, seen;                    /* note-ons written / taken by render */
    int32_t key, vel;                       /* key 0..23, velocity Q15 */
    uint32_t age;                           /* blocks since the note-on, 0 = silent (the voice is free) */
    int16_t x[8], y[8];                     /* the modes' magic-circle oscillators (pair j: modes 2j low, 2j + 1 high) */
    int16_t e[8];                           /* their coefficients this block: 2 sin(pi f / fs), Q15 */
    int16_t amp[8];                         /* their gains now, Q15 */
    int16_t ampq[8];                        /* their squares' gains (DRIVE's CLIP), Q15 */
    int32_t eacc[8];                        /* e's error diffusion (Q8 below e's LSB) */
    int32_t env[8];                         /* the modes' ring envelopes, Q30 */
    int32_t aenv;                           /* the loudest of env[] and exenv, Q30; 0 with age: the DC tail plays out */
    int32_t exenv;                          /* the exciter's envelope (NDECAY), Q30 */
    int32_t benv;                           /* BOOM's drop's envelope (its own decay, STRIKE), Q30 */
    int32_t atk, atkd;                      /* attack Q15 and its step per block */
    int32_t q;                              /* pitch at the end of the last block (K0 units, with the BOOM drop) */
    uint32_t seed;                          /* noise */
    int32_t lp1, lp2;                       /* the exciter's and the resonant noise's one-poles, Q15 << 8 */
    uint32_t oph, oinc;                     /* SCREAM's operator phase and increment */
    int32_t m1, m2, gf;                     /* its last two outputs Q15, feedback depth */
    int32_t gex, grn;                       /* exciter gain (Q12 << 16), resonant-noise mix (Q15) */
    int32_t mdv;                            /* the modes' frequency factor - 1 (BOOM's drop), Q15 << 16 */
    int32_t dcx, dcy;                       /* (unused), the DC blocker's low-pass, Q23 */
    uint32_t shph;                          /* GRIT's sample and hold: phase, held value */
    int32_t shv;
    uint32_t choke;                         /* set by another hat's note-on: release in ~5 ms */
    uint32_t rseed;                         /* RATTLE's impacts */
    int32_t rat;                            /* the last impact's burst, Q15 */
    int32_t level;                          /* (unused) */
    /* caches: by MATERIAL and SPREAD (cka): the log ratios (K0 units), levels, alpha; + DECAY (ckb): the ring
     * factors per block (Q30); + the pitch and STRIKE (ckq, cks): e's targets (Q23), the strike's levels, its gain */
    int32_t cka, ckb, ckq, cks, qr;
    int32_t lk[8], fk[8], et[8];
    int16_t ak[8], wk[8];
    int32_t alpha, g;
    int32_t gp;                             /* the post gain: the modes run at the strike's level (the drive and the
                                             * grit see it whole), this applies their envelope and DRIVE's level after,
                                             * Q12 << 16 */
};

struct mt_diag {
    uint32_t noteons, renders, cyc_max, inits, row_copied;
    int32_t last_voice;
};

extern struct mt_voice metal_voice[MT_VOICES];
extern struct mt_diag metal_diag;
extern const int32_t metal_id;              /* our drum engine id (slot_gen.inc) */

static inline int32_t mt_pos(int16_t k)
{
    return k < 0 ? 0 : k;
}

/* zones on detents (1024 per detent): the first detent of each zone (the last entry ends the knob) */
static const uint8_t mt_strike_edge[6] __attribute__((unused)) = { 0, 8, 16, 24, 29, 33 };
static const uint8_t mt_drive_edge[5] __attribute__((unused)) = { 0, 1, 11, 21, 33 };
static const uint8_t mt_grit_edge[4] __attribute__((unused)) = { 0, 1, 13, 33 };
static const char *const mt_strike_zone[5] __attribute__((unused)) = { "BOOM", "MALLET", "HAMMER", "SCRAPE", "RATTLE" };
static const char *const mt_drive_zone[4] __attribute__((unused)) = { "CLEAN", "FOLD", "CLIP", "SCREAM" };
static const char *const mt_grit_zone[3] __attribute__((unused)) = { "CLEAN", "CRUSH", "ALIAS" };
enum { SZ_BOOM, SZ_MALLET, SZ_HAMMER, SZ_SCRAPE, SZ_RATTLE };
enum { DZ_CLEAN, DZ_FOLD, DZ_CLIP, DZ_SCREAM };
enum { GZ_CLEAN, GZ_CRUSH, GZ_ALIAS };

/* What a zoned knob (STRIKE, DRIVE, GRIT) plays: its table one detent ahead (as fmdrum's NOISE), so a zone's first
 * detent is already in it and its last is the zone at full; the zones join between two detents, continuously. The
 * shift is taken back across the last zone (from its first detent, a) so the knob's top detent is the table's top and
 * the one before it differs. */
static inline int32_t mt_sound(int16_t k, int32_t a, int32_t mul)
{
    int32_t x = mt_pos(k);
    x = x + 1023 - (x > a ? ((x - a) * mul) >> 16 : 0);
    return x > 32767 ? 32767 : x;
}
#define mt_strike_pos(k) mt_sound((k), 29 * 1024, 21831)     /* RATTLE from 29: 1023 x 65536 / (32767 - a) */
#define mt_drive_pos(k) mt_sound((k), 21 * 1024, 5952)       /* SCREAM from 21 */
#define mt_grit_pos(k) mt_sound((k), 13 * 1024, 3446)        /* ALIAS from 13 */

/* the zone of a table position (mt_*_pos: what the knob plays) or of a plain knob value */
static inline int mt_zone(const uint8_t *edge, int n, int32_t k)
{
    int32_t d = (k < 0 ? 0 : k) >> 10;
    int z = 0;
    while (z < n - 1 && d >= edge[z + 1])
        z++;
    return z;
}

/* a knob's position inside its zone, Q15 (the knob's last detent, 32, is the top zone's 1.0); no division */
static inline int32_t mt_zone_t(const uint8_t *edge, int n, int32_t k)
{
    static const uint16_t inv[33] = { 0, 32767, 16384, 10923, 8192, 6554, 5461, 4681, 4096, 3641, 3277, 2979, 2731, 2521, 2341, 2185, 2048, 1928, 1820, 1725, 1638, 1560, 1489, 1425, 1365, 1311, 1260, 1214, 1170, 1130, 1092, 1057, 1024 };   /* 32768 / w */
    int z = mt_zone(edge, n, k);
    int32_t w = (edge[z + 1] > 32 ? 32 : edge[z + 1]) - edge[z];
    if (w <= 0)
        return 32767;
    int32_t t = (((k < 0 ? 0 : k) - (edge[z] << 10)) * inv[w]) >> 10;
    return t > 32767 ? 32767 : t;
}

#endif
