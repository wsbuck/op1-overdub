/* Drum probe: the smallest drum-side engine, to learn how TE registers, renders and edits drum engines
 * (notes/drum-engine.md). A test engine (engines.json "test", "drum"), in DRUMPROBE.OP1 only.
 *
 * Runs as drum engine id N (after our synth ids) on TE's dbox voice objects (12 voices, 0x48 B each in L1 scratchpad)
 * with our copy of dbox's vtable (slot_gen.inc): render (+0x14), note-on (+0x1C), active (+0x24), note-off (+0x2C),
 * each through a C_ENTRY wrapper (entry.inc). `this` + 0x14 is the voice index TE's factory stored (FUN_01970da8).
 *
 * Sound: every key a sine with a fast pitch drop (kick/tom), pitched per key (E1 + key semitones); odd keys add a
 * noise burst. Velocity (TE's 0xFF8012EC, MIDI only: the OP-1's keys send 127) scales the level.
 * Knobs edit the last-pressed key's 8 params (dbox's encoder path, range-table row 9), read every block:
 *   [0] blue    tune, -12..+11 semitones (signed)        [4] SHIFT+blue    noise decay, 30 ms .. 2 s
 *   [1] green   decay, 30 ms .. 2 s (to -60 dB)          [5] SHIFT+green   (shown only)
 *   [2] white   pitch drop, 1x .. 8x at the start          [6] SHIFT+white   (shown only)
 *   [3] orange  noise level (odd keys)                    [7] SHIFT+orange  (shown only)
 * TE's per-key loader and saver treat our type like "dbox" (slot.S _drum_type_eq_load/_save), so the 24 x 8 values
 * load from and save to the patch's dbox_data. The factory preset carries the defaults (engines.json "dbox_default");
 * nothing here rewrites a slot's values (DRUMPROBE's marker did, which would fight loaded values).
 *
 * 128 samples per call at 88.2 kHz (the 2:1 decimator follows), Q15. No division, no libgcc, no .bss.
 */
#include <stdint.h>
#include "dsp.h"
#include "sine_table.h"
#include "drumprobe.h"

#define RAM __attribute__((section(".data")))
#define BLOCK 128
#define SILENT (1 << 20)                   /* envelope floor, Q30: -60 dB */

struct dvoice {                            /* TE's dbox object, 0x48 B; ours except the vptr and +0x14 */
    const void *vptr;                      /* 0x00: our vtable */
    uint32_t phase;                        /* 0x04 */
    uint32_t inc;                          /* 0x08: the key's base increment */
    int32_t env;                           /* 0x0C: amplitude, Q30 */
    uint32_t seed;                         /* 0x10: noise */
    int32_t voice;                         /* 0x14: TE's voice index (read only) */
    int32_t penv;                          /* 0x18: pitch drop, Q15 */
    int32_t nenv;                          /* 0x1C: noise amplitude, Q30 */
    int32_t key;                           /* 0x20 */
    uint32_t age;                          /* 0x24: blocks since note-on, 1.. while sounding */
};

struct dp_diag drumprobe_diag RAM = { 0, 0, 0, 0, -1, 0, -1, 0, 0, 0, { 0 }, { 0 } };
uint32_t dp_type_hits[2] RAM = { 0, 0 };   /* slot.S: TE's per-key loader / saver took the dbox path for our type */

/* E1 (41.2 Hz) * 2^(key / 12) at 88.2 kHz, 2^32 = one cycle */
static const uint32_t base_inc[DP_KEYS] = {
    2006431, 2125740, 2252143, 2386063, 2527945, 2678265, 2837523, 3006250, 3185011, 3374402, 3575054, 3787638,
    4012863, 4251480, 4504286, 4772125, 5055890, 5356529, 5675045, 6012501, 6370023, 6748804, 7150109, 7575276 };
/* 2^((s - 12) / 12), Q10 */
static const uint16_t tune_q10[25] = {
    512, 542, 575, 609, 645, 683, 724, 767, 813, 861, 912, 967, 1024, 1085, 1149, 1218, 1290, 1367, 1448, 1534,
    1625, 1722, 1825, 1933, 2048 };
/* per-block decay to -60 dB in 0.03 * (2 / 0.03)^(i / 31) s, Q15 */
static const uint16_t decay_q15[32] = {
    23460, 24474, 25396, 26230, 26980, 27652, 28254, 28789, 29265, 29687, 30061, 30391, 30683, 30939, 31165, 31364,
    31538, 31692, 31826, 31944, 32047, 32138, 32217, 32286, 32347, 32400, 32446, 32487, 32522, 32553, 32580, 32604 };
#define PENV_Q15 29746                     /* pitch drop: time constant 15 ms */

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile ("%0 = CYCLES;" : "=d"(c));
    return c;
}

static inline int32_t pos(int16_t k)
{
    return k < 0 ? 0 : k;
}

static uint32_t freq(uint32_t inc, int32_t depth, int32_t penv)
{
    return inc + (inc >> 15) * (uint32_t)(depth * penv);
}

void drumprobe_render(struct dvoice *v, int16_t *out, const int16_t *p)
{
    uint32_t t0 = cycles();
    struct dp_diag *d = &drumprobe_diag;
    unsigned vi = (unsigned)v->voice < DP_VOICES ? (unsigned)v->voice : 0;
    int32_t e0 = v->env, n0 = v->nenv;
    d->renders++;
    if (e0 < SILENT && n0 < SILENT) {
        for (int i = 0; i < BLOCK; i++)
            out[i] = 0;
        v->env = v->nenv = 0;
        d->level[vi] = 0;
        return;
    }
    uint32_t inc = ((v->inc >> 4) * tune_q10[((p[0] * 12) >> 15) + 12]) >> 6;
    int32_t depth = pos(p[2]) >> 12;                              /* 0..7: start at 1 + depth times the pitch */
    int32_t pe0 = v->penv, pe1 = (pe0 * PENV_Q15) >> 15;
    uint32_t f = freq(inc, depth, pe0);
    int32_t df = ((int32_t)freq(inc, depth, pe1) - (int32_t)f) >> 7;
    int32_t e1 = (e0 >> 15) * decay_q15[pos(p[1]) >> 10];
    int32_t n1 = (n0 >> 15) * decay_q15[pos(p[4]) >> 10];
    int32_t nl = pos(p[3]);
    int32_t a = e0, da = (e1 - e0) >> 7, na = n0, dna = (n1 - n0) >> 7;
    uint32_t ph = v->phase, seed = v->seed;
    for (int i = 0; i < BLOCK; i++) {
        uint32_t j = ph >> 24, fr = (ph >> 9) & 0x7FFF;
        int32_t s = sine256[j] + (((sine256[j + 1] - sine256[j]) * (int32_t)fr) >> 15);
        int32_t x = (s * (a >> 15)) >> 15;
        seed = seed * 1664525u + 1013904223u;
        x += ((((int32_t)seed >> 16) * (na >> 15)) >> 15) * nl >> 15;
        out[i] = soft_knee(x);
        ph += f;
        f += (uint32_t)df;
        a += da;
        na += dna;
    }
    v->phase = ph;
    v->seed = seed;
    v->env = e1;
    v->nenv = n1;
    v->penv = pe1;
    v->age++;
    d->level[vi] = (e1 > n1 ? e1 : n1) >> 15;
    if (vi == (unsigned)d->voice)           /* the newest note's params, as rendered */
        for (int k = 0; k < 8; k++)
            d->live[k] = p[k];
    uint32_t dt = cycles() - t0;
    d->cyc_max = dt > d->cyc_max ? dt : d->cyc_max;
}

void drumprobe_note_on(struct dvoice *v, const int16_t *key_params)
{
    (void)key_params;                       /* = &params[slot][key][0]; the key comes from TE's voice array */
    struct dp_diag *d = &drumprobe_diag;
    unsigned vi = (unsigned)v->voice < DP_VOICES ? (unsigned)v->voice : 0;
    int32_t key = te_drum_key[vi], vel = te_drum_vel[vi];
    key = key < 0 ? 0 : key >= DP_KEYS ? DP_KEYS - 1 : key;
    vel = vel < 0 ? 0 : vel;
    v->key = key;
    v->inc = base_inc[key];
    v->phase = 0;
    v->penv = 32767;
    v->env = vel << 15;
    v->nenv = (key & 1) ? vel << 15 : 0;
    v->age = 1;
    d->noteons++;
    d->key = key;
    d->vel = vel;
    d->voice = (int32_t)vi;
    d->ratio = te_drum_ratio[vi];
}

/* TE frees the voice when this returns 0 and steals the voice with the largest value: the oldest. */
int32_t drumprobe_active(struct dvoice *v)
{
    return v->env >= SILENT || v->nenv >= SILENT ? (int32_t)v->age : 0;
}

void drumprobe_note_off(struct dvoice *v, const int16_t *key_params)
{
    (void)v;
    (void)key_params;
    drumprobe_diag.noteoffs++;              /* one-shot: drums ignore note-off */
}

/* From the factory (slot.S _drum_factory) before the voices are built, in TE's audio or UI thread. The slot's per-key
 * values are left as TE loaded them (from the patch's dbox_data). */
void drumprobe_slot_init(int32_t slot)
{
    (void)slot;
    drumprobe_diag.inits++;
    /* our range row (ReadInterface normalises with it) = dbox's row 9, filled by TE at boot, if it is sane */
    const int16_t *r9 = ranges + 9 * 32;
    int16_t *rn = ranges + drum_id * 32;
    for (int i = 0; i < 8; i++)
        if (r9[4 * i] >= r9[4 * i + 1])
            return;
    for (int i = 0; i < 32; i++)
        rn[i] = r9[i];
    drumprobe_diag.row_copied = 1;
}
