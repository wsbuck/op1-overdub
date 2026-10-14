/* Input probe: plays one of the OP-1's input buffers, to find out whether an engine can hear the mic/line input
 * (the first step towards a guitar-amp engine). A test engine, not in release builds (engines.json "test").
 * Sources and buffers: inprobe.h; research: notes/input.md. Every sounding voice plays the input; latch one key
 * with HOLD (SHIFT+T2).
 *
 * Knobs (engine page, 0..0x7FFF, read once per block):
 *   [0] blue   gain 0 .. +24 dB in 1 dB steps, then the soft knee
 *   [1] green  source: SILENCE, TONE, TE IN, RAW, RAW OTHER, ALT
 *   [2] white  channel of the int32 sources: L+R, L, R
 *   [3] orange shift of the int32 sources: 0..8 bits up before taking the top 16
 *
 * A codec block is 64 samples at 44.1 kHz; an engine block is 128 at 88.2 kHz (notes/fx-slot.md "Audio graph"),
 * so each block is upsampled 2x linearly: out[2i] = (x[i-1] + x[i]) / 2, out[2i+1] = x[i], with x[-1] the last
 * sample of the previous block (half a sample late, no lookahead needed).
 *
 * State: bytes 0..3 the tone's phase (TE resets it on note-on), 4..7 the previous block's last sample.
 */
#include "engine.h"
#include "dsp.h"
#include "sine_table.h"
#include "inprobe.h"

struct state {
    uint32_t phase;     /*  0: tone phase, reset to 0x80000000 on note-on */
    int32_t prev;       /*  4: last input sample of the previous block */
    uint32_t spare[6];
};

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct state *s = st;
    int32_t g = inp_gain_q10[inp_pos(params[0], INP_DB_MAX + 1)];
    int src = inp_pos(params[1], INP_SOURCES);

    if (src == INP_TONE) {                  /* half-scale sine at the key's pitch (an octave up at 88.2 kHz) */
        uint32_t ph = s->phase;
        for (int i = 0; i < ENGINE_BLOCK; i++) {
            ph += (uint32_t)pitch[i];
            uint32_t j = ph >> 24, f = (ph >> 9) & 0x7FFF;
            int32_t v = sine256[j] + (((sine256[j + 1] - sine256[j]) * (int32_t)f) >> 15);
            out[i] = soft_knee(((v >> 1) * g) >> 10);
        }
        s->phase = ph;
        return;
    }

    int16_t x[INP_N];
    inp_fetch(src, inp_pos(params[2], INP_CHANNELS), inp_pos(params[3], INP_SHIFTS), x);
    int32_t prev = s->prev;
    if (prev > 32767 || prev < -32768)      /* state not ours yet (another engine's bytes) */
        prev = 0;
    for (int i = 0; i < INP_N; i++) {
        int32_t v = x[i];
        out[2 * i] = soft_knee((((prev + v) >> 1) * g) >> 10);
        out[2 * i + 1] = soft_knee((v * g) >> 10);
        prev = v;
    }
    s->prev = prev;
}
