/* CHORD v2 factory maps (design draft). Keys: 0 = F .. 23 = E (two octaves, as the OP-1's keyboard).
 * White keys: F0 G2 A4 B6 C7 D9 E11 | F12 G14 A16 B18 C19 D21 E23. Black: F#1 G#3 A#5 C#8 D#10 | F#13 G#15 A#17 C#20 D#22.
 * Each entry: key, root (semitones from the key), quality, voicing family. Map settings (c2_factory_set): SMOOTH on for
 * II-V-I and NEO SOUL (neighbouring chords voice-lead), BASS -1 oct for NEO SOUL (its 9ths are rootless). */
struct c2_entry { int8_t key, root; uint8_t q, fam; };

/* ii-V-I in four keys, three adjacent white keys each: F, Bb, Eb, C; D E = the minor ii-V to C minor */
static const struct c2_entry c2_f_251[] = {
    { 0, 2, Q_MIN7, 0 }, { 2, 5, Q_7, 0 }, { 4, -4, Q_MAJ7, 0 },             /* F G A    = G-7 C7 F^7      (F)  */
    { 6, 1, Q_MIN7, 0 }, { 7, 5, Q_7, 0 }, { 9, -4, Q_MAJ7, 0 },             /* B C D    = C-7 F7 Bb^7     (Bb) */
    { 11, 1, Q_MIN7, 0 }, { 12, 5, Q_7, 0 }, { 14, -4, Q_MAJ7, 0 },          /* E F G    = F-7 Bb7 Eb^7    (Eb) */
    { 16, 5, Q_MIN7, 0 }, { 18, -4, Q_7, 0 }, { 19, 0, Q_MAJ7, 0 },          /* A B C    = D-7 G7 C^7      (C)  */
    { 21, 0, Q_HDIM, 0 }, { 22, 4, Q_7B9, 0 }, { 23, -4, Q_MIN6, 0 },       /* D D# E   = D$7 G7b9 C-6    (Cm) */
};

/* neo soul: m9 / maj9 / 13 / 7#9 colours around Eb major / C minor, flat-root chords on the black keys */
static const struct c2_entry c2_f_neo[] = {
    { 0, 0, Q_MIN9, 0 }, { 2, 0, Q_7S9, 0 }, { 3, 0, Q_MAJ9, 0 }, { 5, 0, Q_13, 0 }, { 7, 0, Q_MIN9, 0 },
    { 8, 0, Q_MAJ9, 0 }, { 9, 0, Q_MIN7, 0 }, { 10, 0, Q_MAJ9, 0 }, { 11, 0, Q_HDIM, 0 },
    { 12, 0, Q_MIN11, 0 }, { 14, 0, Q_7B9, 0 }, { 15, 0, Q_6, 0 }, { 17, 0, Q_7SUS4, 0 }, { 19, 0, Q_MIN11, 0 },
    { 20, 0, Q_MAJ7, 0 }, { 21, 0, Q_HDIM, 0 }, { 22, 0, Q_6, 0 },
};

/* pop: C major triads on the white keys, borrowed chords on the black ones (Bb, Ab, Eb, Fm, D = V of V) */
static const struct c2_entry c2_f_pop[] = {
    { 0, 0, Q_MAJ, 0 }, { 2, 0, Q_MAJ, 0 }, { 4, 0, Q_MIN, 0 }, { 6, 0, Q_DIM, 0 }, { 7, 0, Q_MAJ, 0 },
    { 9, 0, Q_MIN, 0 }, { 11, 0, Q_MIN, 0 }, { 12, 0, Q_MAJ, 0 }, { 14, 0, Q_MAJ, 0 }, { 16, 0, Q_MIN, 0 },
    { 18, 0, Q_DIM, 0 }, { 19, 0, Q_MAJ, 0 }, { 21, 0, Q_MIN, 0 }, { 23, 0, Q_MIN, 0 },
    { 1, -1, Q_MIN, 0 }, { 3, 0, Q_MAJ, 0 }, { 5, 0, Q_MAJ, 0 }, { 8, 1, Q_MAJ, 0 }, { 10, 0, Q_MAJ, 0 },
    { 13, -1, Q_MIN, 0 }, { 15, 0, Q_MAJ, 0 }, { 17, 0, Q_MAJ, 0 }, { 20, 1, Q_MAJ, 0 }, { 22, 0, Q_MAJ, 0 },
};

struct c2_set { uint8_t smooth, bass, flats; };
static const struct c2_set c2_factory_set[4] = { { 0, 0, 0 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 0, 1 } };

static void c2_load(struct c2_chord *map, const struct c2_entry *e, int n)
{
    for (int k = 0; k < C2_KEYS; k++)
        c2_clear(&map[k]);
    for (int i = 0; i < n; i++) {
        struct c2_chord *c = &map[e[i].key];
        c->root = e[i].root;
        c->q = e[i].q;
        c->fam = e[i].fam;
    }
}
