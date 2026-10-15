/* Host build of src/slot/ui.c for previews: te_line prints each segment; preview.py draws them.
 *   preview_host WT_DATA.bin ENGINE K0 K1 K2 K3 > segments.txt   (one "x0 y0 x1 y1 width colour" per line)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pingpong.h"
#include "drumprobe.h"
#include "fmdrum.h"
#include "fmdrum_tables.h"
#include "sprobe.h"
#include "amp.h"
#include "amp_tune.h"
#include "harm.h"
#include "wt_import.h"
#include "harm_edit.h"
#include "fm4_edit.h"
#include <math.h>

int16_t host_knobs[8];   /* K0..K3 from the command line; K4..K7 set per preview */
uint8_t host_palette[20 * 4];
int16_t wt_data[8 * 12 * 4128];
int16_t grain_data[256 + 6 * 70560];
uint32_t wt_ram[(256 + 2 * 12 * 4128 * 2 + 40000) / 4];   /* SAMPLE bank (wt_user.h): nothing built */

int32_t host_matrix[9] = { 1024 };   /* not the rotated-panel form: vline_fast falls back to te_line (recorded) */
static int32_t host_raster[4] = { 0, -1, 0, 0 };   /* invalid geometry: fast lines fall back to te_line */
void *te_raster(void) { return host_raster; }
uint8_t voice_ram[8 * 4096];
volatile int32_t te_engine_ids[8];      /* no sampler slot: grain shows the built-in bank */
int32_t *te_ssamp[8];
volatile int16_t te_knobs[8][8];
volatile int32_t te_synth_slot;
const uint8_t ui_kind[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };   /* preview ids: 15 va, 16 wavetable, 17 modal, 18 grain,
                                                               * 19 inprobe, 20 sprobe, 21 amp, 22 harm, 23 fm4 */
/* harm and fm4 (engines 22, 23): K47="k4,k5,k6,k7" sets knobs 4..7 (the packed ones), DELTA=n the hook's last raw
 * delta, FM4_PLAY=1 a note sounding on fm4 (voice 0 renders every frame, envelopes full). harm's table is built on the
 * host as on the device. */
struct host_harm_s { struct harm_ram h; struct wti_work w; } host_harm;
struct harm_ui harm_ui = { .last = -1 };
struct fm4_ui fm4_ui = { .last = -1, .alg_shown = -1 };
/* Audio-input buffers (inprobe.h): a decaying guitar-like wave in TE IN and RAW (MSB-aligned), filled in main(). */
volatile int16_t te_audio_in[64];
volatile uint32_t te_rx_sel = 1;
volatile int32_t te_rx[256];
volatile int32_t te_alt_in[128];
/* Drum probe (engine 300 in previews): TE's per-key params, edit key, range row 9 and the engine's record. */
int16_t te_drum_params[8 * DP_KEYS * 8];
volatile int32_t te_drum_edit = 5;
int16_t ranges[20 * 32];
struct dp_diag drumprobe_diag = { 37, 35, 90000, 12700, 5, 0x7F00, 3, 0x10000, 1, 1,
                                  { 32767, 0, 0, 20000, 9000, 0, 0, 0, 0, 0, 3000, 0 },
                                  { 0, 16384, 12288, 20000, 8192, 0, 0, 0 } };
uint32_t dp_type_hits[2] = { 2, 5 };
/* Our drum engines in previews: 18 the probe (engine 300), 19 fmdrum (engine 301). fmdrum: FD_PARAMS = the slot's 24 x 8
 * per-key values (comma-separated), FD_KEY = the selected key, FD_PLAY = blocks since the selected key's note-on (a voice
 * playing it), FD_LIT = "key:level,..." other sounding keys (level Q15). */
const int32_t drum_first = 18, drum_count = 2;
const uint8_t drum_kind[2] = { 1, 2 };
struct fd_voice fmdrum_voice[FD_VOICES];
static void fd_setup(void)
{
    const char *s = getenv("FD_PARAMS");
    for (int i = 0; s && i < DP_KEYS * 8; i++) {
        te_drum_params[i] = (int16_t)atoi(s);
        s = strchr(s, ',');
        s = s ? s + 1 : 0;
    }
    te_drum_edit = getenv("FD_KEY") ? atoi(getenv("FD_KEY")) : 0;
    int key = te_drum_edit;
    const int16_t *k = te_drum_params + key * 8;
    if (getenv("FD_PLAY")) {                 /* the engine's envelopes after n blocks, as fmdrum.c steps them */
        int n = atoi(getenv("FD_PLAY"));
        struct fd_voice *v = &fmdrum_voice[0];
        v->key = key;
        v->age = (uint32_t)n;
        v->aenv = (int32_t)(1073741824.0 * pow(fd_amp_dec[fd_pos(k[3]) >> 7] / 1073741824.0, n));
        v->menv = (int32_t)(1073741824.0 * pow(fd_mod_dec[fd_pos(k[7]) >> 7] / 1073741824.0, n));
        v->senv = (int32_t)(1073741824.0 * pow(fd_swp_dec[fd_pos(k[5]) >> 7] / 1073741824.0, n));
        v->index_q8 = (int32_t)(fd_index_q8(k[2]) * (v->menv / 1073741824.0));
    }
    const char *lit = getenv("FD_LIT");
    for (int j = 1; lit && j < FD_VOICES; j++) {
        fmdrum_voice[j].key = atoi(lit);
        lit = strchr(lit, ':');
        fmdrum_voice[j].aenv = lit ? atoi(lit + 1) << 15 : 0;
        fmdrum_voice[j].age = 5;
        lit = lit ? strchr(lit, ',') : 0;
        lit = lit ? lit + 1 : 0;
    }
}
/* Knob probe (engine 20 in previews): the hook's record and the engine's live params, set in main() */
struct kh_diag knobhook_diag;
struct sp_ram sprobe_ram;
/* amp (engine 21 in previews): AMP_DB = input peak dBFS (default -15), AMP_PLAY=1 a note sounds (the engine renders every
 * frame; else the tuner shows), AMP_HZ = the note TE IN carries (default 110 Hz + 7 cents); FRAMES animates. */
struct amp_ram amp_ram;
static double amp_t;
static void amp_fill(int16_t *dst, int n)
{
    double hz = getenv("AMP_HZ") ? atof(getenv("AMP_HZ")) : 110 * pow(2, 7 / 1200.0);
    double a = 32767 * pow(10, (getenv("AMP_DB") ? atof(getenv("AMP_DB")) : -15) / 20);
    for (int i = 0; i < n; i++, amp_t += 1 / 44100.0) {
        double v = 0;
        for (int h = 1; h <= 6; h++)
            v += sin(2 * M_PI * hz * h * amp_t + h) / h;
        dst[i] = (int16_t)(a * v / 1.9);
    }
}
int host_amp_capture(int16_t *dst)
{
    amp_fill(dst, AMP_TUNE_N);
    return 1;
}
static void amp_tick(void)
{
    int16_t b[64];
    amp_fill(b, 64);
    for (int i = 0; i < 64; i++)
        te_audio_in[i] = b[i];
    if (getenv("AMP_PLAY") && atoi(getenv("AMP_PLAY"))) {
        amp_ram.renders += 40;
        amp_ram.gate_open = 1;
    }
}
uint32_t hold_on = 1;   /* show the indicators in previews */
uint32_t chord_on = 1, chord_learning = 0;
int32_t chord_n = 3, chord_iv[4] = {0, 4, 16, 0};   /* root, major 3rd, major 10th */
uint32_t host_prof[6] = { 0, 0, 0, 0, 0, 1 };   /* [5] = render count: equal to ui.c's initial last_renders, so stored knobs are shown */

static int host_drawing = 1;
void te_line(void *raster, const int32_t *a, const int32_t *b, int32_t width, const uint8_t *rgba)
{
    (void)raster;
    if (!host_drawing)
        return;
    printf("%d %d %d %d %d %d\n", a[0], a[1], b[0], b[1], width, (int)((rgba - host_palette) / 4));
}

/* Our FX for previews: ids 8 ensemble, 9 pingpong, 10 distortion (fx.json order). */
const uint8_t fx_kind[3] = { 1, 2, 3 };
const int32_t fx_count = 3;
struct ens { uint32_t lfo, vib; int32_t depth, vdepth, ens, mix; int32_t d[2][3]; };
struct ens ensemble_state[2] = { { 0x30000000u, 0, 0, 0, 0, 0, { { 0 } } }, { 0x30000000u, 0, 0, 0, 0, 0, { { 0 } } } };

struct pp pingpong_state[2];
/* One display frame of audio time (30 fps): the FX clocks the screens read. */
static void host_tick(int i)
{
    (void)i;
    for (int s = 0; s < 2; s++) {
        ensemble_state[s].vib += 0x0DDDDDDDu;               /* 6.5 Hz / 30 fps */
        ensemble_state[s].lfo += 0x01000000u;
        pingpong_state[s].w += 1470;
        if (!pingpong_state[s].d) {                         /* the delay the knob sets: 20 ms * 2^(6 k / 32768) */
            double ms = 20.0;
            for (int j = 0; j < (host_knobs[0] * 6) / 4096; j++)
                ms *= 1.0905077326652577;                   /* 2^(1/8) */
            pingpong_state[s].d = (int32_t)(ms * 44.1 * 32768);
        }
    }
}
/* distortion_curve / distortion_mode_name: the real ones (distortion.c built with -DDISTORTION_CURVE_ONLY). */

/* SVG screens on the host: with SVG_DIR and SVG_IDS set (preview.py), the documents "exist" and node_find/matrix/
 * visible/fill/stroke act on a node table; after drawing, every node's state is printed as
 * "N doc id visible a b c d e f fill stroke" (matrix Q10, -1 = untouched; colours rrggbb or -) for preview.py to
 * apply to the SVG and rasterise. SVG_IDS lines: "doc id fill stroke" (the SVG's own colours). */
#define NODES 512
struct hnode { uint8_t raw[0x60]; char doc[24], id[24]; int32_t m[6]; int has_m; uint8_t fill[4], stroke[4]; int has_f, has_s; };
static struct hnode nodes[NODES];
static int n_nodes;
static char docs[4][24];
static int n_docs;

static void load_ids(void)
{
    static int done;
    const char *p = getenv("SVG_IDS");
    if (done || !p)
        return;
    done = 1;
    FILE *f = fopen(p, "r");
    char doc[64], id[64], fl[16], st[16];
    while (f && fscanf(f, "%63s %63s %15s %15s", doc, id, fl, st) == 4 && n_nodes < NODES) {
        struct hnode *n = &nodes[n_nodes++];
        snprintf(n->doc, sizeof n->doc, "%s", doc);
        snprintf(n->id, sizeof n->id, "%s", id);
        n->raw[0x50] = 1;
        unsigned v;
        if ((n->has_f = sscanf(fl, "%x", &v) == 1 && fl[0] != '-'))
            n->fill[0] = v >> 16, n->fill[1] = v >> 8, n->fill[2] = v, n->fill[3] = 255;
        if ((n->has_s = sscanf(st, "%x", &v) == 1 && st[0] != '-'))
            n->stroke[0] = v >> 16, n->stroke[1] = v >> 8, n->stroke[2] = v, n->stroke[3] = 255;
    }
    if (f)
        fclose(f);
}

int te_yaffs_open(const char *path, int flags, int mode)
{
    (void)flags; (void)mode;
    char buf[512];
    const char *d = getenv("SVG_DIR"), *slash = strrchr(path, '/');
    if (!d || !slash)
        return -1;
    snprintf(buf, sizeof buf, "%s%s", d, slash);
    FILE *f = fopen(buf, "r");
    if (!f)
        return -1;
    fclose(f);
    return 3;
}
int te_yaffs_close(int fd) { (void)fd; return 0; }
void *te_svg_load(const char *name)
{
    load_ids();
    snprintf(docs[n_docs], sizeof docs[0], "%s", name);
    return (void *)(intptr_t)(0x1000 + n_docs++);
}
static struct hnode *lookup(void *doc, const char *id)
{
    int di = (int)((intptr_t)doc - 0x1000);
    if (di < 0 || di >= n_docs)
        return 0;
    for (int i = 0; i < n_nodes; i++)
        if (!strcmp(nodes[i].doc, docs[di]) && !strcmp(nodes[i].id, id))
            return &nodes[i];
    return 0;
}
void *te_node_find(void *node, const char *id, int must)
{
    (void)must;
    struct hnode *n = lookup(node, id);
    return n ? n->raw : 0;
}
void te_node_matrix(void *node, const int32_t *m)
{
    struct hnode *n = (struct hnode *)node;
    n->m[0] = m[0], n->m[1] = m[1], n->m[2] = m[3], n->m[3] = m[4], n->m[4] = m[6], n->m[5] = m[7];
    n->has_m = 1;
}
void te_svg_render(void *screen) { (void)screen; }
/* Like TE's getters (FUN_01a3b854 / FUN_01a3b96c): assert when the element has no such style. */
uint8_t *te_svg_stroke(void *screen, const char *id)
{
    struct hnode *n = lookup(*(void **)screen, id);
    if (!n || !n->has_s) {
        fprintf(stderr, "ASSERT Screen.cpp linestyle_id: %s has no stroke\n", id);
        exit(5);
    }
    return n->stroke;
}
uint8_t *te_svg_fill(void *screen, const char *id)
{
    struct hnode *n = lookup(*(void **)screen, id);
    if (!n || !n->has_f) {
        fprintf(stderr, "ASSERT Screen.cpp fill: %s has no fill\n", id);
        exit(5);
    }
    return n->fill;
}

static void dump_nodes(void)
{
    for (int i = 0; i < n_nodes; i++) {
        struct hnode *n = &nodes[i];
        int loaded = 0;
        for (int d = 0; d < n_docs; d++)
            loaded |= !strcmp(n->doc, docs[d]);
        if (!loaded)
            continue;
        printf("N %s %s %d", n->doc, n->id, n->raw[0x50]);
        for (int j = 0; j < 6; j++)
            printf(" %d", n->has_m ? n->m[j] : -1);
        if (n->has_f) printf(" %02x%02x%02x", n->fill[0], n->fill[1], n->fill[2]); else printf(" -");
        if (n->has_s) printf(" %02x%02x%02x\n", n->stroke[0], n->stroke[1], n->stroke[2]); else printf(" -\n");
    }
}

void ui_draw(int engine, int slot);
void fx_ui_draw(int id, int mode, int slot);
void drum_ui_draw(int slot, int id);

int main(int argc, char **argv)
{
    if (argc != 7)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(wt_data, 2, sizeof wt_data / 2, f) != sizeof wt_data / 2)
        return 3;
    fclose(f);
    f = fopen(getenv("GRAIN_DATA") ? getenv("GRAIN_DATA") : "/dev/null", "rb");
    if (f) {
        size_t n = fread(grain_data, 2, sizeof grain_data / 2, f);
        (void)n;
        fclose(f);
    }
    for (int i = 0; i < 4; i++)
        host_knobs[i] = (int16_t)atoi(argv[3 + i]);
    int e = atoi(argv[2]);
    for (int i = 0; i < 64; i++) {      /* a parabolic sine (one period, peak ~16000) plus a ragged overtone */
        int t = i * 6 - 192;
        int v = t * (192 - (t < 0 ? -t : t)) * 16 / 9 + ((i * 37) % 64 - 32) * 60;
        te_audio_in[i] = (int16_t)v;
        te_rx[128 + 2 * i] = v << 16;
        te_rx[128 + 2 * i + 1] = (v / 2) << 16;
    }
    if (getenv("CLEAN"))
        hold_on = chord_on = 0;   /* screenshots: no HOLD/CHORD badges */
    int frames = getenv("FRAMES") ? atoi(getenv("FRAMES")) : 1;
    host_drawing = frames <= 1;
    static const int16_t dp_keys[8] = { -1200, 16384, 12288, 20000, 8192, 0, 0, 0 };
    for (int i = 0; i < DP_KEYS * 8; i++)
        te_drum_params[i] = dp_keys[i & 7];
    for (int i = 0; i < 8; i++) {          /* a guess at dbox's row 9 (the device shows the real one) */
        ranges[9 * 32 + 4 * i] = (int16_t)(i ? 0 : -32767);
        ranges[9 * 32 + 4 * i + 1] = 32767;
        ranges[9 * 32 + 4 * i + 2] = 8192;
    }
    if (e == 20) {                         /* knob probe: 37 edits, the last on knob 5; the LFO moving knob 4 */
        host_knobs[4] = 12000;
        host_knobs[5] = 3000;
        host_knobs[6] = sp_k6(37, 5);
        host_knobs[7] = sp_k7(host_knobs[6]);
        knobhook_diag = (struct kh_diag){ 41, 5, -3, host_knobs[6], host_knobs[7], 0 };
        if (getenv("CLAMPED"))             /* what a clamp on load would look like */
            host_knobs[7] = 32767;
        for (int i = 0; i < 8; i++)
            sprobe_ram.live[i] = host_knobs[i];
        sprobe_ram.live[4] = 17500;
        sprobe_ram.renders = 90;
    }
    if (e == 22 || e == 23) {
        const char *k47 = getenv("K47");
        for (int i = 0; k47 && i < 4; i++) {
            host_knobs[4 + i] = (int16_t)atoi(k47);
            k47 = strchr(k47, ',');
            k47 = k47 ? k47 + 1 : 0;
        }
        if (getenv("DELTA")) {
            int d = atoi(getenv("DELTA"));
            knobhook_diag = (struct kh_diag){ 7, 1, d, 0, 0, 0, d < 0 ? -d : d };
        }
        if (e == 23 && getenv("PAGE"))
            fm4_ui.page = atoi(getenv("PAGE"));
    }
    if (e == 300)
        drum_ui_draw(0, 18);
    else if (e == 301) {
        fd_setup();
        drum_ui_draw(0, 19);
    }
    else if (e >= 100)
        for (int i = 0; i < frames; i++) {
            if (i == frames - 1)
                host_drawing = 1;
            host_tick(i);
            fx_ui_draw(e % 100, e >= 200 ? 0 : 3, 0);
        }
    else if (e == 21)
        for (int i = 0; i < frames; i++) {
            if (i == frames - 1)
                host_drawing = 1;
            amp_tick();
            ui_draw(e, 0);
        }
    else if (e == 23)
        for (int i = 0; i < frames; i++) {
            if (i == frames - 1)
                host_drawing = 1;
            if (getenv("FM4_PLAY")) {      /* voice 0 renders, envelopes full */
                struct fm4_voice *v = (struct fm4_voice *)voice_ram;
                v->owner = OWNER_FM4;
                v->blocks += 23;
                for (int op = 0; op < FM4_OPS; op++)
                    v->env[op] = 32767 << 15;
            }
            ui_draw(e, 0);
        }
    else
        ui_draw(e, 0);
    dump_nodes();
    return 0;
}
