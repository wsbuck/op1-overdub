/* CHORD v2 chord model: qualities, voicings, names, recognition of a played chord, voice leading, scale fill.
 * Design draft for notes/chord2.md (not linked into firmware). Written as device code would be: no division,
 * no floats, no libc, small tables; host-tested by design/chord2/chord2_preview.c ("model" checks) and
 * cross-compiled for Blackfin (no libgcc calls).
 *
 * A chord on a key = root (semitones from the key) + quality + TOP (a ladder: each step puts the next chord tone on
 * top, across octaves) + voicing family (close, drop 2, drop 3, spread), or a learned chord kept exactly as played
 * (CUSTOM) until TYPE is turned. Per map: BASS (the root 1 or 2 octaves under the chord, a 5th voice) and SMOOTH.
 * Intervals are what CHORD1's player already takes (key + interval), so octave keys keep working as today.
 */
#include <stdint.h>

#define C2_MAX 4                    /* chord tones (6 synth voices); + 1 optional bass note */
#define C2_NOTES (C2_MAX + 1)
#define C2_KEYS 24

enum { Q_MAJ, Q_MIN, Q_SUS2, Q_SUS4, Q_DIM, Q_AUG, Q_5,
       Q_MAJ7, Q_7, Q_MIN7, Q_HDIM, Q_DIM7, Q_MINMAJ7, Q_7SUS4, Q_6, Q_MIN6, Q_69,
       Q_ADD9, Q_MAJ9, Q_9, Q_MIN9, Q_9SUS4, Q_13, Q_MIN11, Q_7S9, Q_7B9, Q_7S11, Q_7B13,
       Q_COUNT, Q_CUSTOM = 0x7E, Q_NONE = 0x7F };

struct c2_quality {
    int8_t iv[C2_MAX];              /* close position, ascending, from the root (rootless voicings start above it) */
    uint8_t n;
    uint8_t rootless;
    char name[6];                   /* title: Real Book symbols ('-' minor, '^' maj7, '*' dim, '$' half-dim, '&' flat) */
    char tag[5];                    /* on a key: <= 3 glyphs */
};

static const struct c2_quality c2_q[Q_COUNT] = {
    { { 0, 4, 7 }, 3, 0, "", "" },          { { 0, 3, 7 }, 3, 0, "-", "-" },
    { { 0, 2, 7 }, 3, 0, "SUS2", "S2" },    { { 0, 5, 7 }, 3, 0, "SUS4", "S4" },
    { { 0, 3, 6 }, 3, 0, "*", "*" },        { { 0, 4, 8 }, 3, 0, "+", "+" },
    { { 0, 7, 12 }, 3, 0, "5", "5" },
    { { 0, 4, 7, 11 }, 4, 0, "^7", "^7" },  { { 0, 4, 7, 10 }, 4, 0, "7", "7" },
    { { 0, 3, 7, 10 }, 4, 0, "-7", "-7" },  { { 0, 3, 6, 10 }, 4, 0, "$7", "$" },
    { { 0, 3, 6, 9 }, 4, 0, "*7", "*7" },   { { 0, 3, 7, 11 }, 4, 0, "-^7", "-^" },
    { { 0, 5, 7, 10 }, 4, 0, "7SUS4", "7S4" }, { { 0, 4, 7, 9 }, 4, 0, "6", "6" },
    { { 0, 3, 7, 9 }, 4, 0, "-6", "-6" },   { { 0, 4, 9, 14 }, 4, 0, "6/9", "69" },
    { { 0, 4, 7, 14 }, 4, 0, "ADD9", "(9)" },
    { { 4, 7, 11, 14 }, 4, 1, "^9", "^9" }, { { 4, 7, 10, 14 }, 4, 1, "9", "9" },
    { { 3, 7, 10, 14 }, 4, 1, "-9", "-9" }, { { 5, 7, 10, 14 }, 4, 1, "9SUS4", "9S4" },
    { { 4, 10, 14, 21 }, 4, 1, "13", "13" }, { { 3, 10, 14, 17 }, 4, 1, "-11", "-11" },
    { { 0, 4, 10, 15 }, 4, 0, "7#9", "7#9" }, { { 4, 7, 10, 13 }, 4, 1, "7&9", "7&9" },
    { { 0, 4, 10, 18 }, 4, 0, "7#11", "#11" }, { { 4, 10, 14, 20 }, 4, 1, "7&13", "&13" },
};

enum { F_CLOSE, F_DROP2, F_DROP3, F_SPREAD, F_COUNT };
static const char *const c2_fam_name[F_COUNT] = { "CLOSE", "DROP 2", "DROP 3", "SPREAD" };

struct c2_chord {
    int8_t root;                    /* semitones from the key */
    uint8_t q;                      /* Q_*, Q_CUSTOM (learned, iv[] as played) or Q_NONE (key plays its note) */
    uint8_t fam;                    /* F_* */
    int8_t top;                     /* ladder: 0 = root position; +1 = next chord tone on top (inversion), n = +1 oct */
    int8_t iv[C2_MAX];              /* Q_CUSTOM: from the key, ascending */
    uint8_t n;
    uint8_t has_slash;              /* slash bass (semitones from the key) kept from a learned chord whose lowest note */
    int8_t slash;                   /* is not its root, so G/B survives a TYPE edit; zero-initialised = none */
};

static const char *const c2_pc_sharp[12] = { "C", "C#", "D", "E&", "E", "F", "F#", "G", "A&", "A", "B&", "B" };
static const char *const c2_pc_flat[12] = { "C", "D&", "D", "E&", "E", "F", "G&", "G", "A&", "A", "B&", "B" };

static int c2_mod12(int x)          /* no % : no libgcc */
{
    while (x < 0)
        x += 12;
    while (x >= 12)
        x -= 12;
    return x;
}

/* The OP-1's 24 keys start on F (FUN_01a85014: note = key + 0x35 + 12 * octave). */
static int c2_key_pc(int key) { return c2_mod12(key + 5); }

static void c2_sort(int8_t *v, int n)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            int8_t t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
}

/* Voice a quality: TOP steps of inversion on the close voicing (each moves the lowest note up an octave; n steps =
 * one octave up; negative steps move the top note down), then the family's drop on the result: drops only move
 * notes below the top, so VOICING never changes the top note TOP set. Intervals from the root, ascending.
 * Triads: DROP 3 = DROP 2. */
static int c2_voice(const struct c2_quality *q, int fam, int top, int8_t *out)
{
    int n = q->n;
    for (int i = 0; i < n; i++)
        out[i] = q->iv[i];
    for (; top > 0; top--) {
        out[0] += 12;
        c2_sort(out, n);
    }
    for (; top < 0; top++) {
        out[n - 1] -= 12;
        c2_sort(out, n);
    }
    if (fam == F_DROP3 && n == 3)
        fam = F_DROP2;
    if (fam == F_DROP2)
        out[n - 2] -= 12;
    else if (fam == F_DROP3)
        out[n - 3] -= 12;
    else if (fam == F_SPREAD) {
        if (n == 4) {
            out[2] -= 12;           /* drop 2 + 4 */
            out[0] -= 12;
        } else {
            out[1] -= 12;           /* triad: drop 2, then the lowest down too: wide, top kept */
            out[0] -= 12;
        }
    }
    c2_sort(out, n);
    return n;
}

/* Intervals from the key, as the player plays them; bass 1..2 adds the root 1 or 2 octaves under the chord's root
 * (and under its lowest note): a 5th voice. Returns the count. */
static int c2_notes(const struct c2_chord *c, int bass, int8_t *out)
{
    int n = 0;
    if (c->q == Q_NONE)
        return 0;
    if (c->q == Q_CUSTOM) {
        for (int i = 0; i < c->n; i++)
            out[i] = c->iv[i];
        n = c->n;
    } else {
        n = c2_voice(&c2_q[c->q], c->fam, c->top, out);
        for (int i = 0; i < n; i++)
            out[i] += c->root;
    }
    if (n && c->q != Q_CUSTOM && c->has_slash) {   /* G/B: B under the chord */
        int b = c->slash;
        while (b >= out[0])
            b -= 12;
        for (int i = n; i > 0; i--)
            out[i] = out[i - 1];
        out[0] = (int8_t)b;
        n++;
    } else if (bass && n && c->q != Q_CUSTOM) {
        int b = c->root - 12 * bass;
        while (b >= out[0])
            b -= 12;
        for (int i = n; i > 0; i--)
            out[i] = out[i - 1];
        out[0] = (int8_t)b;
        n++;
    }
    return n;
}

static int c2_mask(const int8_t *iv, int n, int base)
{
    int m = 0;
    for (int i = 0; i < n; i++)
        m |= 1 << c2_mod12(iv[i] + base);
    return m;
}

/* Name a set of played notes (intervals from the key): the best (root, quality). Prefers the root in the bass, then
 * a voicing with the root, then the earlier quality. Returns the quality or Q_CUSTOM. */
static int c2_recognise(const int8_t *iv, int n, int *root_semis)
{
    int want = c2_mask(iv, n, 0), best = -1, best_q = Q_CUSTOM;
    for (int q = 0; q < Q_COUNT; q++) {
        if (c2_q[q].n != n)
            continue;
        for (int r = 0; r < 12; r++) {
            if (c2_mask(c2_q[q].iv, c2_q[q].n, r) != want)
                continue;
            int s = (r == c2_mod12(iv[0]) ? 8 : 0) + (c2_q[q].rootless ? 0 : 4);   /* ties: the earlier quality */
            if (s > best) {
                best = s;
                best_q = q;
                *root_semis = r;
            }
        }
    }
    return best_q;
}

/* SMOOTH (per map): the TOP of `c` (same family) whose notes move least from `prev` (the last chord played,
 * intervals from the same origin as `key`), kept near `centre`. After ~2 s of silence the caller passes pn = 0 and
 * the chord's own TOP is used, so a phrase always starts where the player set it: deterministic. */
static int c2_smooth(const struct c2_chord *c, int key, const int8_t *prev, int pn, int centre)
{
    if (!pn || c->q >= Q_COUNT)
        return c->top;
    int best = 1 << 30, best_t = c->top;
    for (int t = -4; t <= 6; t++) {
        int8_t v[C2_MAX];
        int n = c2_voice(&c2_q[c->q], c->fam, t, v), cost = 0, sum = 0;
        for (int i = 0; i < n; i++) {
            v[i] += key + c->root;
            sum += v[i];
        }
        for (int i = 0; i < n; i++) {
            int j = i < pn ? i : pn - 1, d = v[i] - prev[j];
            cost += d < 0 ? -d : d;
        }
        int reg = sum - n * centre;
        cost += (reg < 0 ? -reg : reg) >> 2;
        if (cost < best) {
            best = cost;
            best_t = t;
        }
    }
    return best_t;
}

/* "F#-7": root name + quality symbol (or tag). Returns the end of the string. */
static char *c2_name(char *p, const struct c2_chord *c, int key, int flats, int tag)
{
    int root = c->root;
    int q = c->q;
    if (q == Q_CUSTOM) {
        q = c2_recognise(c->iv, c->n, &root);
        if (q == Q_CUSTOM)
            root = c->iv[0];
    }
    const char *s = (flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(key + root)];
    while (*s)
        *p++ = *s++;
    s = q == Q_CUSTOM ? "?" : tag ? c2_q[q].tag : c2_q[q].name;
    while (*s)
        *p++ = *s++;
    int low = c->q == Q_CUSTOM ? c->iv[0] : c->slash;   /* slash: a bass that is not the root */
    if (!tag && (c->q == Q_CUSTOM || c->has_slash) && q != Q_CUSTOM && c2_mod12(low - root) != 0) {
        *p++ = '/';
        for (s = (flats ? c2_pc_flat : c2_pc_sharp)[c2_key_pc(key + low)]; *s;)
            *p++ = *s++;
    }
    *p = 0;
    return p;
}

/* Scale fill: every key whose pitch is in the scale gets the diatonic chord stacked in thirds from it (3 or 4
 * notes); other keys stay plain. `tonic_pc` 0..11 (C = 0). */
enum { S_MAJOR, S_MINOR, S_DORIAN, S_MIXO, S_HARM, S_COUNT };
static const uint8_t c2_scale[S_COUNT][7] = {
    { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 3, 5, 7, 9, 10 }, { 0, 2, 4, 5, 7, 9, 10 },
    { 0, 2, 3, 5, 7, 8, 11 },
};
static const char *const c2_scale_name[S_COUNT] = { "MAJOR", "MINOR", "DORIAN", "MIXOLYD", "HARM MIN" };

static int c2_quality_of(const int8_t *iv, int n)   /* iv from the root, close */
{
    int m = c2_mask(iv, n, 0);
    for (int q = 0; q < Q_COUNT; q++)
        if (!c2_q[q].rootless && c2_q[q].n == n && c2_mask(c2_q[q].iv, n, 0) == m)
            return q;
    return Q_CUSTOM;
}

static void c2_clear(struct c2_chord *c)
{
    c->q = Q_NONE;
    c->root = 0;
    c->fam = F_CLOSE;
    c->top = 0;
    c->n = 0;
    c->has_slash = 0;
}

/* TYPE turned on a learned chord: it becomes structured (recognised root, the next quality), keeping a learned bass
 * that is not the root as the slash bass. */
static void c2_structure(struct c2_chord *c, int q)
{
    int root = 0, low = c->iv[0];
    int got = c2_recognise(c->iv, c->n, &root);
    if (got == Q_CUSTOM)
        root = low;
    while (root > low + 11)
        root -= 12;
    while (root < low)
        root += 12;
    c->q = (uint8_t)q;
    c->root = (int8_t)(root > 12 ? root - 12 : root);
    c->fam = F_CLOSE;
    c->top = 0;
    c->has_slash = c2_mod12(low - root) != 0;
    c->slash = (int8_t)low;
}

static void c2_fill(struct c2_chord *map, int tonic_pc, int scale, int sevenths)
{
    for (int k = 0; k < C2_KEYS; k++) {
        struct c2_chord *c = &map[k];
        c2_clear(c);
        int pc = c2_key_pc(k), deg = -1;
        for (int d = 0; d < 7; d++)
            if (c2_mod12(tonic_pc + c2_scale[scale][d]) == pc)
                deg = d;
        if (deg < 0)
            continue;
        int8_t iv[4];
        int n = sevenths ? 4 : 3;
        for (int i = 0; i < n; i++) {
            int d = deg + 2 * i, up = 0;
            while (d >= 7) {
                d -= 7;
                up += 12;
            }
            iv[i] = (int8_t)(c2_scale[scale][d] + up - c2_scale[scale][deg]);
        }
        c->q = (uint8_t)c2_quality_of(iv, n);
        if (c->q == Q_CUSTOM) {
            for (int i = 0; i < n; i++)
                c->iv[i] = iv[i];
            c->n = (uint8_t)n;
        }
    }
}
