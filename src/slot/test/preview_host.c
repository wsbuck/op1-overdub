/* Host build of src/slot/ui.c for previews: te_line prints each segment; preview.py draws them.
 *   preview_host WT_DATA.bin ENGINE K0 K1 K2 K3 > segments.txt   (one "x0 y0 x1 y1 width colour" per line)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pingpong.h"
#include "drumprobe.h"

int16_t host_knobs[4];
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
const uint8_t ui_kind[16] = { 1, 2, 3, 4, 5 };   /* preview ids: 15 va, 16 wavetable, 17 modal, 18 grain, 19 inprobe */
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
                                  { 0, 16384, 12288, 20000, 8192, 0, 0, DP_MAGIC } };
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
    static const int16_t dp_keys[8] = { -1200, 16384, 12288, 20000, 8192, 0, 0, DP_MAGIC };
    for (int i = 0; i < DP_KEYS * 8; i++)
        te_drum_params[i] = dp_keys[i & 7];
    for (int i = 0; i < 8; i++) {          /* a guess at dbox's row 9 (the device shows the real one) */
        ranges[9 * 32 + 4 * i] = (int16_t)(i ? 0 : -32767);
        ranges[9 * 32 + 4 * i + 1] = 32767;
        ranges[9 * 32 + 4 * i + 2] = 8192;
    }
    if (e == 300)
        drum_ui_draw(0, 18);
    else if (e >= 100)
        for (int i = 0; i < frames; i++) {
            if (i == frames - 1)
                host_drawing = 1;
            host_tick(i);
            fx_ui_draw(e % 100, e >= 200 ? 0 : 3, 0);
        }
    else
        ui_draw(e, 0);
    dump_nodes();
    return 0;
}
