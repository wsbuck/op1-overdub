/* Knob probe: a minimal synth engine for the encoder hook and 8-knob storage test (notes/knob-hook.md). A test engine
 * (engines.json "test"), in SAVEPROBE.OP1 only. Its range row is type 3 (dsynth's), so SHIFT gives 4 more independent
 * knobs; src/slot/knobhook.c writes packed values into knobs 6..7 (sprobe.h).
 *
 * Sound: a 2-operator FM sine at the key's pitch.
 *   [0] blue    pitch, -12..+12 semitones          [4] SHIFT+blue    octave-up sine level (to hear TE's LFO on knob 4)
 *   [1] green   level                              [5] SHIFT+green   (shown only)
 *   [2] white   FM index 0 .. ~2 pi                [6] SHIFT+white   hook data (never read for sound)
 *   [3] orange  modulator ratio 1, 2, 3, 4         [7] SHIFT+orange  hook data (never read for sound)
 * Every call copies params[0..7] (TE's per-voice copy, after the LFO) and counts renders in sprobe_ram for the screen.
 *
 * State: bytes 0..3 carrier phase and 8..11 octave phase (TE resets both to 0x80000000 on note-on), 4..7 modulator.
 */
#include "engine.h"
#include "dsp.h"
#include "sine_table.h"
#include "sprobe.h"

struct state {
    uint32_t car;       /*  0 */
    uint32_t mod;       /*  4 */
    uint32_t oct;       /*  8 */
    uint32_t spare[5];
};

/* 2^(s / 12) in Q12, s = -12..+12 */
static const uint16_t semis_q12[25] = {
    2048, 2170, 2299, 2435, 2580, 2734, 2896, 3069, 3251, 3444, 3649, 3866, 4096, 4340, 4598, 4871, 5161, 5468,
    5793, 6137, 6502, 6889, 7298, 7732, 8192 };

static inline int32_t pos(int16_t k)
{
    return k < 0 ? 0 : k;
}

static inline int32_t sine(uint32_t ph)
{
    uint32_t j = ph >> 24, f = (ph >> 9) & 0x7FFF;
    return sine256[j] + (((sine256[j + 1] - sine256[j]) * (int32_t)f) >> 15);
}

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct state *s = st;
    struct sp_ram *r = &sprobe_ram;
    for (int i = 0; i < 8; i++)
        r->live[i] = params[i];
    r->renders++;

    uint32_t tune = semis_q12[(pos(params[0]) * 25) >> 15];
    int32_t level = pos(params[1]), index = pos(params[2]) >> 4, ratio = (pos(params[3]) >> 13) + 1;
    int32_t octave = pos(params[4]);
    uint32_t car = s->car, mod = s->mod, oct = s->oct;
    for (int i = 0; i < ENGINE_BLOCK; i++) {
        uint32_t inc = ((uint32_t)pitch[i] >> 12) * tune;
        int32_t m = sine(mod);
        int32_t y = sine(car + ((uint32_t)(m * index) << 6));
        y += (sine(oct) * octave) >> 15;
        out[i] = soft_knee((y * level) >> 15);
        car += inc;
        mod += inc * (uint32_t)ratio;
        oct += inc << 1;
    }
    s->car = car;
    s->mod = mod;
    s->oct = oct;
}
