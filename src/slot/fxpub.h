/* What each of our FX publishes for its screen (ui.c fx_live): the knobs its process call used for the latest
 * 64-frame block, and a block count. Those are the params TE passed: for a synth / drum FX, TE's per-block copy
 * 0xFF801724 after the sound's LFO has modulated it (the stored knobs 0xFF801444 / 0xFF8015C4 never move with the
 * LFO); for the master FX, the stored 0xFF8016C4 (no LFO reaches it). notes/fx-slot.md "LFO". */
#ifndef FXPUB_H
#define FXPUB_H
#include <stdint.h>

struct fx_pub {
    int16_t k[4];                       /* params[0..3] of the latest block (TE's LFO targets knobs 1-4 only) */
    uint32_t blocks;                    /* process calls so far (wraps); the screen trusts k only while it moves */
};

/* <source>_pub[0]: mono (synth / drum FX), [1]: stereo (master FX). */
static inline void fx_publish(struct fx_pub *u, const int16_t *p)
{
    u->k[0] = p[0];
    u->k[1] = p[1];
    u->k[2] = p[2];
    u->k[3] = p[3];
    u->blocks++;
}

#endif
