/* Input probe (inprobe.c engine and its screen in src/slot/ui.c): which audio-input buffers an engine can read.
 * Research: notes/input.md. Both sides call inp_fetch(), so the screen shows exactly what the engine plays.
 *
 * Sources (green):
 *   SILENCE    nothing: the reference
 *   TONE       a sine at the key's pitch, made by the engine (proves the engine is heard at all)
 *   TE IN      0xFF800CB4: TE's mono int16 input, 64 samples, written at the start of each audio callback
 *   RAW        the SPORT0 RX half the callback converts: int32 L/R x 64 at 0xFF800370 or 0xFF800570
 *   RAW OTHER  the other RX half (the one the DMA is filling while the callback runs)
 *   ALT        0x01AB2E2C: int32 L/R x 64, the stream TE uses instead of the codec when 0xFF8010B4 = 3
 * White picks L+R / L / R for the int32 sources; orange shifts them left 0..8 bits before taking the top 16
 * (0 = the word is MSB-aligned, as TE treats it; 8 = 24-bit data in the low bits).
 *
 * The buffers are linker symbols: TE's addresses on the device (src/engine/build.sh, tools/build_slot.py),
 * arrays in the simulator and the host preview.
 */
#ifndef INPROBE_H
#define INPROBE_H
#include <stdint.h>

#define INP_N 64                           /* samples per codec block (44.1 kHz) */
enum { INP_SILENCE, INP_TONE, INP_TE, INP_RAW, INP_RAW_OTHER, INP_ALT, INP_SOURCES };
#define INP_CHANNELS 3                     /* L+R, L, R */
#define INP_SHIFTS 9                       /* 0..8 */
#define INP_DB_MAX 24                      /* blue: 0 .. +24 dB in 1 dB steps */

extern volatile int16_t te_audio_in[];     /* 0xFF800CB4, int16 x 64 */
extern volatile uint32_t te_rx_sel;        /* 0xFF800364: 1 = the callback got the half at 0xFF800570 */
extern volatile int32_t te_rx[];           /* 0xFF800370, two halves of int32 x 128 (L, R interleaved) */
extern volatile int32_t te_alt_in[];       /* 0x01AB2E2C, int32 x 128 (L, R interleaved) */

/* 10^(dB / 20) in Q10, dB = 0..24 */
static const uint16_t inp_gain_q10[INP_DB_MAX + 1] = {
    1024, 1149, 1289, 1446, 1623, 1821, 2043, 2292, 2572, 2886, 3238, 3633, 4077, 4574, 5132, 5758,
    6461, 7249, 8134, 9126, 10240, 11489, 12891, 14464, 16229 };

/* Knob 0..0x7FFF -> one of n positions */
static inline int inp_pos(int16_t k, int n)
{
    int p = ((k < 0 ? 0 : k) * n) >> 15;
    return p < n ? p : n - 1;
}

static inline int16_t inp_sat16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* The int32 L/R block a stereo source reads (0 for the others). */
static inline const volatile int32_t *inp_words(int src)
{
    if (src == INP_RAW || src == INP_RAW_OTHER)
        return te_rx + ((((te_rx_sel & 1) ^ (src == INP_RAW_OTHER))) << 7);   /* & 1: never outside the halves */
    return src == INP_ALT ? te_alt_in : 0;
}

/* The source's 64 samples as int16 into x; returns the first raw word (int16 for TE IN, the L word else). */
static inline uint32_t inp_fetch(int src, int ch, int shift, int16_t *x)
{
    const volatile int32_t *w = inp_words(src);
    if (w) {
        for (int i = 0; i < INP_N; i++) {
            int32_t l = w[2 * i], r = w[2 * i + 1];
            int32_t v = ch == 1 ? l >> (16 - shift) : ch == 2 ? r >> (16 - shift)
                      : ((l >> 1) + (r >> 1)) >> (15 - shift);      /* L+R as TE sums them, without overflow */
            x[i] = inp_sat16(v);
        }
        return (uint32_t)w[0];
    }
    for (int i = 0; i < INP_N; i++)
        x[i] = src == INP_TE ? te_audio_in[i] : 0;
    return (uint16_t)x[0];
}

#endif
