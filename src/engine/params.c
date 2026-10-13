/* First C engine: same behaviour as src/poc/params.S (proven on device), so a CENGINE test isolates
 * the C toolchain + trampoline.  level = params[0], octave = params[1] >> 13, tone x * (1 - |x|). */
#include "engine.h"

struct state { uint32_t phase; };

static inline int16_t q15_mul(int16_t a, int16_t b)
{
    return (int16_t)(((int32_t)a * b + 0x4000) >> 15);
}

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct state *s = st;
    int16_t level = params[0];
    int oct = (params[1] >> 13) & 3;
    uint32_t phase = s->phase;

    for (int i = 0; i < ENGINE_BLOCK; i++) {
        int16_t x = (int16_t)(phase >> 16);
        int16_t shape = (int16_t)(0x7FFF - (x < 0 ? -x : x));
        out[i] = q15_mul(q15_mul(x, shape), level);
        phase += (uint32_t)pitch[i] << oct;
    }
    s->phase = phase;
}
