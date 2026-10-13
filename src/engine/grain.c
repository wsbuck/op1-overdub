/* Granular cloud: up to 8 overlapping Hann-windowed grains per voice. Source (grain_source.h): the OP-1's
 * own synth sampler sample when another sound slot holds the sampler engine (within the sampler's start/end
 * trim), else a built-in bank of six sounds (choir, strings, bells, glass, air, plucks; gen_grain_data.py),
 * all rooted at C4.
 *
 * Knobs (engine page, 0..0x7FFF, read once per block):
 *   [0] blue   position: scans the source (the bank: also picks the sound; neighbours blend at the seams)
 *   [1] green  size: grain length 10 ms .. 500 ms (exponential)
 *   [2] white  density: 2 .. 80 grains per second (exponential)
 *   [3] orange spray: random position (+-0.3 s), pitch (+-50 cents) and onset jitter; above 60 %, some
 *              grains an octave up (shimmer)
 * Pitch follows the key: grains play at inc / C4. Grains are scheduled once per block (2.9 ms; at
 * most 80 per second, so at most one per block) and rendered grain-outer with their state in registers.
 * Level: grain sum * 1 / sqrt(overlap) / 2, so density does not change loudness much. With the sampler as the
 * source, the sampler's own gain applies too: TE's sampler render uses (knob 8 / 8192)^2 (square in Q15,
 * then << 4 at 0x01a51c3c), so 8192 = unity and the maximum = x16.
 * Per-voice state in a voice_ram slot (grain_state.h); note-on (TE's 0x80000000 at state bytes 0..3)
 * clears the grains and starts one immediately.
 */
#include "engine.h"
#include "dsp.h"
#include "grain_layout.h"
#include "grain_state.h"
#include "modal_state.h"               /* voice_ram slot ownership */
#include "grain_source.h"

extern const int16_t engine_data[];     /* overview[GRAIN_OVERVIEW], then the bank */
extern uint8_t voice_ram[];             /* 8 x 4 KB (FREE region 0 at 0x00810000 on device) */

#define BANK (engine_data + GRAIN_OVERVIEW)

struct state {                          /* TE voice object +4 .. +0x23 */
    uint32_t note_on;                   /* 0x80000000 after TE's note-on */
    uint32_t spare[7];
};

/* 2^(i / 32), Q15 + 15 bits: 32768 .. 65536 */
static const uint16_t exp2_tab[33] = {
    32768, 33486, 34219, 34968, 35734, 36516, 37316, 38133, 38968, 39821, 40693, 41584, 42495, 43425,
    44376, 45348, 46341, 47356, 48393, 49452, 50535, 51642, 52773, 53928, 55109, 56316, 57549, 58809,
    60097, 61413, 62757, 64132, 65535,
};

/* 2^(y / 65536) * 65536 for 0 <= y < 15 * 65536 (Q16 in, Q16 out). */
static uint32_t exp2_q16(int32_t y)
{
    int n = y >> 16;
    int32_t f = y & 0xFFFF, i = f >> 11, fr = f & 0x7FF;
    uint32_t m = exp2_tab[i] + (((exp2_tab[i + 1] - exp2_tab[i]) * fr) >> 11);   /* Q15 */
    return (m << n) << 1;
}

static struct grain_voice *voice_of(void *st)
{
    uint32_t off = (uint32_t)st - 0xFFB00004u;      /* TE voice i: 0xFFB00000 + i * 0x24, state at +4 */
    uint32_t i = (off * 1821u) >> 16;
    if (off > 5 * 0x24 || i * 0x24 != off)
        i = 7;                                      /* not a TE voice (simulator) */
    return voice_slot_claim(voice_ram + i * VOICE_RAM_BYTES, OWNER_GRAIN, sizeof(struct grain_voice));
}

static inline uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* Uniform in [-32768, 32767] (Q15 bipolar) and [0, 32767]. */
static inline int32_t rnd_bi(uint32_t *s) { return (int32_t)xorshift(s) >> 16; }
static inline int32_t rnd_uni(uint32_t *s) { return (int32_t)(xorshift(s) >> 17); }

/* Hann window sin^2(pi x), 1025 points over the grain, Q15 (read without interpolation). */
static const uint16_t hann1025[1025] = {
    0, 0, 1, 3, 5, 8, 11, 15, 20, 25, 31, 37,
    44, 52, 60, 69, 79, 89, 100, 111, 123, 136, 149, 163,
    177, 192, 208, 224, 241, 259, 277, 295, 315, 335, 355, 376,
    398, 420, 443, 467, 491, 516, 541, 567, 593, 621, 648, 677,
    705, 735, 765, 796, 827, 859, 891, 924, 958, 992, 1027, 1062,
    1098, 1134, 1171, 1209, 1247, 1286, 1325, 1365, 1406, 1447, 1488, 1530,
    1573, 1616, 1660, 1704, 1749, 1795, 1841, 1887, 1935, 1982, 2030, 2079,
    2128, 2178, 2229, 2279, 2331, 2383, 2435, 2488, 2542, 2596, 2650, 2706,
    2761, 2817, 2874, 2931, 2989, 3047, 3105, 3165, 3224, 3284, 3345, 3406,
    3468, 3530, 3592, 3655, 3719, 3783, 3847, 3912, 3978, 4044, 4110, 4177,
    4244, 4312, 4380, 4449, 4518, 4587, 4657, 4728, 4799, 4870, 4942, 5014,
    5086, 5159, 5233, 5307, 5381, 5456, 5531, 5606, 5682, 5759, 5835, 5912,
    5990, 6068, 6146, 6225, 6304, 6383, 6463, 6543, 6624, 6705, 6786, 6868,
    6950, 7032, 7115, 7198, 7281, 7365, 7449, 7534, 7618, 7703, 7789, 7875,
    7961, 8047, 8134, 8221, 8308, 8396, 8484, 8572, 8660, 8749, 8838, 8928,
    9017, 9107, 9197, 9288, 9379, 9470, 9561, 9652, 9744, 9836, 9929, 10021,
    10114, 10207, 10300, 10393, 10487, 10581, 10675, 10770, 10864, 10959, 11054, 11149,
    11244, 11340, 11436, 11532, 11628, 11724, 11820, 11917, 12014, 12111, 12208, 12305,
    12403, 12500, 12598, 12696, 12794, 12892, 12990, 13089, 13187, 13286, 13385, 13484,
    13583, 13682, 13781, 13880, 13980, 14079, 14179, 14278, 14378, 14478, 14578, 14678,
    14778, 14878, 14978, 15078, 15178, 15279, 15379, 15479, 15580, 15680, 15780, 15881,
    15981, 16082, 16182, 16283, 16383, 16484, 16585, 16685, 16786, 16886, 16987, 17087,
    17187, 17288, 17388, 17488, 17589, 17689, 17789, 17889, 17989, 18089, 18189, 18289,
    18389, 18489, 18588, 18688, 18787, 18887, 18986, 19085, 19184, 19283, 19382, 19481,
    19580, 19678, 19777, 19875, 19973, 20071, 20169, 20267, 20364, 20462, 20559, 20656,
    20753, 20850, 20947, 21043, 21139, 21235, 21331, 21427, 21523, 21618, 21713, 21808,
    21903, 21997, 22092, 22186, 22280, 22374, 22467, 22560, 22653, 22746, 22838, 22931,
    23023, 23115, 23206, 23297, 23388, 23479, 23570, 23660, 23750, 23839, 23929, 24018,
    24107, 24195, 24283, 24371, 24459, 24546, 24633, 24720, 24806, 24892, 24978, 25064,
    25149, 25233, 25318, 25402, 25486, 25569, 25652, 25735, 25817, 25899, 25981, 26062,
    26143, 26224, 26304, 26384, 26463, 26542, 26621, 26699, 26777, 26855, 26932, 27008,
    27085, 27161, 27236, 27311, 27386, 27460, 27534, 27608, 27681, 27753, 27825, 27897,
    27968, 28039, 28110, 28180, 28249, 28318, 28387, 28455, 28523, 28590, 28657, 28723,
    28789, 28855, 28920, 28984, 29048, 29112, 29175, 29237, 29299, 29361, 29422, 29483,
    29543, 29602, 29662, 29720, 29778, 29836, 29893, 29950, 30006, 30061, 30117, 30171,
    30225, 30279, 30332, 30384, 30436, 30488, 30538, 30589, 30639, 30688, 30737, 30785,
    30832, 30880, 30926, 30972, 31018, 31063, 31107, 31151, 31194, 31237, 31279, 31320,
    31361, 31402, 31442, 31481, 31520, 31558, 31596, 31633, 31669, 31705, 31740, 31775,
    31809, 31843, 31876, 31908, 31940, 31971, 32002, 32032, 32062, 32090, 32119, 32146,
    32174, 32200, 32226, 32251, 32276, 32300, 32324, 32347, 32369, 32391, 32412, 32432,
    32452, 32472, 32490, 32508, 32526, 32543, 32559, 32575, 32590, 32604, 32618, 32631,
    32644, 32656, 32667, 32678, 32688, 32698, 32707, 32715, 32723, 32730, 32736, 32742,
    32747, 32752, 32756, 32759, 32762, 32764, 32766, 32767, 32767, 32767, 32766, 32764,
    32762, 32759, 32756, 32752, 32747, 32742, 32736, 32730, 32723, 32715, 32707, 32698,
    32688, 32678, 32667, 32656, 32644, 32631, 32618, 32604, 32590, 32575, 32559, 32543,
    32526, 32508, 32490, 32472, 32452, 32432, 32412, 32391, 32369, 32347, 32324, 32300,
    32276, 32251, 32226, 32200, 32174, 32146, 32119, 32090, 32062, 32032, 32002, 31971,
    31940, 31908, 31876, 31843, 31809, 31775, 31740, 31705, 31669, 31633, 31596, 31558,
    31520, 31481, 31442, 31402, 31361, 31320, 31279, 31237, 31194, 31151, 31107, 31063,
    31018, 30972, 30926, 30880, 30832, 30785, 30737, 30688, 30639, 30589, 30538, 30488,
    30436, 30384, 30332, 30279, 30225, 30171, 30117, 30061, 30006, 29950, 29893, 29836,
    29778, 29720, 29662, 29602, 29543, 29483, 29422, 29361, 29299, 29237, 29175, 29112,
    29048, 28984, 28920, 28855, 28789, 28723, 28657, 28590, 28523, 28455, 28387, 28318,
    28249, 28180, 28110, 28039, 27968, 27897, 27825, 27753, 27681, 27608, 27534, 27460,
    27386, 27311, 27236, 27161, 27085, 27008, 26932, 26855, 26777, 26699, 26621, 26542,
    26463, 26384, 26304, 26224, 26143, 26062, 25981, 25899, 25817, 25735, 25652, 25569,
    25486, 25402, 25318, 25233, 25149, 25064, 24978, 24892, 24806, 24720, 24633, 24546,
    24459, 24371, 24283, 24195, 24107, 24018, 23929, 23839, 23750, 23660, 23570, 23479,
    23388, 23297, 23206, 23115, 23023, 22931, 22838, 22746, 22653, 22560, 22467, 22374,
    22280, 22186, 22092, 21997, 21903, 21808, 21713, 21618, 21523, 21427, 21331, 21235,
    21139, 21043, 20947, 20850, 20753, 20656, 20559, 20462, 20364, 20267, 20169, 20071,
    19973, 19875, 19777, 19678, 19580, 19481, 19382, 19283, 19184, 19085, 18986, 18887,
    18787, 18688, 18588, 18489, 18389, 18289, 18189, 18089, 17989, 17889, 17789, 17689,
    17589, 17488, 17388, 17288, 17187, 17087, 16987, 16886, 16786, 16685, 16585, 16484,
    16384, 16283, 16182, 16082, 15981, 15881, 15780, 15680, 15580, 15479, 15379, 15279,
    15178, 15078, 14978, 14878, 14778, 14678, 14578, 14478, 14378, 14278, 14179, 14079,
    13980, 13880, 13781, 13682, 13583, 13484, 13385, 13286, 13187, 13089, 12990, 12892,
    12794, 12696, 12598, 12500, 12403, 12305, 12208, 12111, 12014, 11917, 11820, 11724,
    11628, 11532, 11436, 11340, 11244, 11149, 11054, 10959, 10864, 10770, 10675, 10581,
    10487, 10393, 10300, 10207, 10114, 10021, 9929, 9836, 9744, 9652, 9561, 9470,
    9379, 9288, 9197, 9107, 9017, 8928, 8838, 8749, 8660, 8572, 8484, 8396,
    8308, 8221, 8134, 8047, 7961, 7875, 7789, 7703, 7618, 7534, 7449, 7365,
    7281, 7198, 7115, 7032, 6950, 6868, 6786, 6705, 6624, 6543, 6463, 6383,
    6304, 6225, 6146, 6068, 5990, 5912, 5835, 5759, 5682, 5606, 5531, 5456,
    5381, 5307, 5233, 5159, 5086, 5014, 4942, 4870, 4799, 4728, 4657, 4587,
    4518, 4449, 4380, 4312, 4244, 4177, 4110, 4044, 3978, 3912, 3847, 3783,
    3719, 3655, 3592, 3530, 3468, 3406, 3345, 3284, 3224, 3165, 3105, 3047,
    2989, 2931, 2874, 2817, 2761, 2706, 2650, 2596, 2542, 2488, 2435, 2383,
    2331, 2279, 2229, 2178, 2128, 2079, 2030, 1982, 1935, 1887, 1841, 1795,
    1749, 1704, 1660, 1616, 1573, 1530, 1488, 1447, 1406, 1365, 1325, 1286,
    1247, 1209, 1171, 1134, 1098, 1062, 1027, 992, 958, 924, 891, 859,
    827, 796, 765, 735, 705, 677, 648, 621, 593, 567, 541, 516,
    491, 467, 443, 420, 398, 376, 355, 335, 315, 295, 277, 259,
    241, 224, 208, 192, 177, 163, 149, 136, 123, 111, 100, 89,
    79, 69, 60, 52, 44, 37, 31, 25, 20, 15, 11, 8,
    5, 3, 1, 0, 0,
};

void engine_render(void *st, int16_t *out, const int16_t *params, const int32_t *pitch)
{
    struct state *s = st;
    struct grain_voice *v = voice_of(st);
    int32_t pos = clamp_knob(params[0]), size = clamp_knob(params[1]);
    int32_t dens = clamp_knob(params[2]), spray = clamp_knob(params[3]);

    /* Source: the sampler's sample or the bank; grains in flight are dropped when it changes. */
    struct grain_src src = grain_source(BANK, GRAIN_TOTAL);
    uint32_t key = (uint32_t)src.audio ^ (src.start << 3) ^ (src.len << 11);
    if (key != v->src_key) {
        for (int i = 0; i < GRAINS; i++)
            v->g[i].wstep = 0;
        v->src_key = key;
    }
    const uint32_t span_q12 = src.len << 12, hi_q12 = (src.start + src.len) << 12;

    if (s->note_on == 0x80000000u) {               /* TE note-on */
        s->note_on = 0;
        for (int i = 0; i < GRAINS; i++)
            v->g[i].wstep = 0;
        v->next = 0;
        if (v->rng == 0)
            v->rng = 0x2545F491u ^ (uint32_t)st;
    }

    /* Block parameters. Lengths in samples: size 441 * 50^k (10 .. 500 ms), interval 22050 / 40^k. */
    uint32_t len = (441u * exp2_q16((size * 11559) >> 10)) >> 16;      /* 2^(5.644 k): log2(50) */
    uint32_t ival = udiv(22050u << 16, exp2_q16((dens * 10900) >> 10)); /* 2^(5.322 k): log2(40) */
    if (len < 64)
        len = 64;
    /* Pitch: rate = inc / inc(C4), Q15, from a fixed reciprocal 2^47 / GRAIN_ROOT_INC. */
    int32_t rate = qmul((int32_t)((uint32_t)pitch[0] >> 1), 5523329);
    /* Level: 1 / sqrt(overlap), overlap = len / interval. */
    int32_t gain = 32767;
    if (len > ival)
        gain = (int32_t)isqrt32(udiv(ival << 15, len) << 15);           /* sqrt(ratio), Q15 */

    /* Schedule: at most one new grain per block. */
    v->next -= ENGINE_BLOCK;
    if (v->next <= 0) {
        int slot = -1;
        for (int i = 0; i < GRAINS; i++)
            if (!v->g[i].wstep) {
                slot = i;
                break;
            }
        if (slot >= 0) {
            struct grain *g = &v->g[slot];
            int32_t base = (int32_t)(((uint32_t)pos * ((src.len - 1u) >> 3)) >> 12);    /* within the window */
            int32_t jit = (rnd_bi(&v->rng) * ((spray * 13230) >> 15)) >> 15;   /* +-0.3 s */
            int32_t p = base + jit;
            while (p < 0)
                p += (int32_t)src.len;
            while (p >= (int32_t)src.len)
                p -= (int32_t)src.len;
            g->pos = (src.start + (uint32_t)p) << 12;
            g->wph = 0;
            g->wstep = udiv(0xFFFFFFFFu, len);
            g->mul = 16384 + ((rnd_bi(&v->rng) * ((spray * 474) >> 15)) >> 15);  /* +-50 cents ~ +-2.9 % */
            if (spray > 19661 && rnd_uni(&v->rng) < ((spray - 19661) * 5) >> 3)
                g->mul <<= 1;                                                   /* octave shimmer */
        }
        v->next += (int32_t)ival + (int32_t)(((int32_t)ival * ((rnd_bi(&v->rng) * (spray >> 1)) >> 15)) >> 15);
        if (v->next < ENGINE_BLOCK)
            v->next = ENGINE_BLOCK;
    }

    /* Render: each grain over the block. step (Q12 samples per output sample) = rate * mul. */
    int32_t sum[ENGINE_BLOCK];
    for (int i = 0; i < ENGINE_BLOCK; i++)
        sum[i] = 0;
    for (int gi = 0; gi < GRAINS; gi++) {
        struct grain *g = &v->g[gi];
        if (!g->wstep)
            continue;
        uint32_t step = ((uint32_t)(rate >> 3) * (uint32_t)(g->mul >> 2)) >> 12;
        uint32_t p = g->pos, w = g->wph, ws = g->wstep;
        /* samples left in the window: the loop then needs no end test */
        uint32_t left = udiv(~w, ws) + 1;
        int n = left < ENGINE_BLOCK ? (int)left : ENGINE_BLOCK;
        for (int i = 0; i < n; i++) {
            uint32_t idx = p >> 12;
            const int16_t *b = src.audio + idx;                  /* b[1] exists (bank guard / clamped trim) */
            /* 16-bit multiplies (one cycle on the BF524): the delta is halved to fit (|delta| <= 2 * 32767);
             * x and the window both fit 16 bits */
            int32_t x = b[0] + (((int32_t)(int16_t)((b[1] - b[0]) >> 1) * (int16_t)(p & 0xFFF)) >> 11);
            sum[i] += ((int32_t)(int16_t)x * (int16_t)hann1025[w >> 22]) >> 15;
            p += step;
            if (p >= hi_q12)
                p -= span_q12;
            w += ws;
        }
        g->pos = p;
        g->wph = w;
        g->wstep = left <= ENGINE_BLOCK ? 0 : ws;                  /* window finished in this block */
    }

    /* sampler gain (k7 / 8192)^2 in Q12: 8192 -> 4096 (unity), max 65532 (x16); the bank uses unity */
    int32_t sgain = 4096;
    if (src.slot >= 0) {
        int32_t k7 = te_knobs[src.slot][7];
        sgain = k7 <= 0 ? 0 : (k7 * k7) >> 14;
    }
    int32_t peak = 0;
    for (int i = 0; i < ENGINE_BLOCK; i++) {
        int32_t x = ((sum[i] >> 1) * (gain >> 1)) >> 14;              /* sum * gain / 2 (-6 dB) */
        x = ((x >> 2) * (sgain >> 2)) >> 8;                            /* * sgain / 4096 */
        int16_t o = soft_clip(x);
        out[i] = o;
        int32_t a = o < 0 ? -o : o;
        if (a > peak)
            peak = a;
    }
    v->peak = peak;
}
