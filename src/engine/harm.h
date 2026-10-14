/* Harmonic (drawn additive) engine: shared by the engine (harm.c), the table builder (harm_build.h, UI thread), the
 * edit function (src/slot/harm_edit.h, encoder hook) and the screen (src/slot/ui_harm.h). Design:
 * notes/harmonic-synth.md; implementation notes and presets: notes/harm-fm4.md.
 *
 * Storage: the sound's 8 knob values (patch "knobs", int16; the range rows are -32768..32767, type 3):
 *   knob 0  bits 0..5 unison, 6..11 warp (0..63 each), 12..15 format version (HARM_VERSION)
 *   knob 1  bits 0..4 tilt (two's complement, -16..15, 0.75 dB/octave per step), 5..8 extend (0..15), 9..15 zero
 *   knob 2+j  harmonics 4j+1 .. 4j+4, 4 bits each, lowest nibble first (0 = off, 1..15 = -42 .. 0 dB, 3 dB steps)
 * The engine never reads params: TE's LFO modulates the per-voice params copy, which would scramble packed values.
 * The UI thread unpacks the stored (unmodulated) knobs, builds the table and publishes it with unison and warp.
 */
#ifndef HARM_H
#define HARM_H
#include <stdint.h>
#include "wt_layout.h"

#define HARM_N 24                           /* drawn harmonics */
#define HARM_MAXLEVEL 15
#define HARM_VERSION 1
#define HARM_FRAMES 3                       /* published single-frame tables (3: the UI never writes one a voice
                                             * may still fade from) */
#define HARM_FADE 1024                      /* crossfade old -> new table: samples (11.6 ms at 88.2 kHz) */
#define HARM_RAM_HDR 256
#define HARM_UNI_SCALE 520                  /* 6-bit unison/warp -> wt.c's 0..32767 knob scale (63 -> 32760) */

/* Engine <-> UI, in FREE SDRAM (engines.json "ram"; the first 256 B are zero at boot, so front = 0: the engine plays
 * a sine until the screen has built a table). Then struct wti_work (the builder's scratch). */
struct harm_ram {
    volatile int32_t front;                 /* 0: nothing built (sine); 1..3: frame[front - 1] plays */
    volatile int32_t prev;                  /* the frame published before front (0: none): voices may fade from it */
    volatile int32_t uni, warp;             /* 0..32767 (wt.c's scale), from the stored knobs */
    uint32_t sig;                           /* signature of the drawing in front (0: none) */
    uint32_t builds;                        /* tables built since boot */
    uint32_t build_cycles;                  /* CYCLES of the last build (device diagnostics) */
    int32_t spare[HARM_RAM_HDR / 4 - 7];
    int16_t frame[HARM_FRAMES][FRAME_STRIDE];
};

extern struct harm_ram harm_ram __attribute__((weak));     /* absent in builds without harm */

/* ---- packing ---- */

static inline int harm_level(const int16_t *k, int i)
{
    return ((uint16_t)k[2 + (i >> 2)] >> ((i & 3) << 2)) & 15;
}

static inline void harm_set_level(int16_t *k, int i, int v)
{
    int sh = (i & 3) << 2;
    uint16_t w = (uint16_t)k[2 + (i >> 2)];
    w = (uint16_t)((w & ~(15u << sh)) | ((uint32_t)(v & 15) << sh));
    k[2 + (i >> 2)] = (int16_t)w;
}

static inline int harm_uni(const int16_t *k) { return (uint16_t)k[0] & 63; }
static inline int harm_warp(const int16_t *k) { return ((uint16_t)k[0] >> 6) & 63; }
static inline int harm_tilt(const int16_t *k) { return (int)(((uint16_t)k[1] & 31) ^ 16) - 16; }
static inline int harm_extend(const int16_t *k) { return ((uint16_t)k[1] >> 5) & 15; }

static inline int16_t harm_k0(int uni, int warp)
{
    return (int16_t)(uint16_t)((uni & 63) | (warp & 63) << 6 | HARM_VERSION << 12);
}

static inline int16_t harm_k1(int tilt, int extend)
{
    return (int16_t)(uint16_t)((tilt & 31) | (extend & 15) << 5);
}

/* Signature of what the table depends on (knob 1's tilt/extend and the 24 levels); never 0. */
static inline uint32_t harm_sig(const int16_t *k)
{
    uint32_t s = 0x811C9DC5u;
    for (int i = 1; i < 8; i++)
        s = (s ^ (uint16_t)(i == 1 ? k[1] & 0x1FF : k[i])) * 16777619u;
    return s | 1;
}

#endif
