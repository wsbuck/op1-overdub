/* Host build of src/slot/ui.c for previews: te_line prints each segment; preview.py draws them.
 *   preview_host WT_DATA.bin ENGINE K0 K1 K2 K3 > segments.txt   (one "x0 y0 x1 y1 width colour" per line)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pingpong.h"
#include "crush.h"
#include "fxpub.h"
#include "repeat.h"
#include "space.h"
#include "drumprobe.h"
#include "fmdrum.h"
#include "fmdrum_tables.h"
#include "metal.h"
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
int16_t ranges[32 * 32];
struct dp_diag drumprobe_diag = { 37, 35, 90000, 12700, 5, 0x7F00, 3, 0x10000, 1, 1,
                                  { 32767, 0, 0, 20000, 9000, 0, 0, 0, 0, 0, 3000, 0 },
                                  { 0, 16384, 12288, 20000, 8192, 0, 0, 0 } };
uint32_t dp_type_hits[2] = { 2, 5 };
/* Our drum engines in previews: 18 the probe (engine 300), 19 fmdrum (engine 301). fmdrum: FD_PARAMS = the slot's 24 x 8
 * per-key values (comma-separated), FD_KEY = the selected key, FD_PLAY = blocks since the selected key's note-on (a voice
 * playing it), FD_LIT = "key:level,..." other sounding keys (level Q15), FD_TURN = "knob:value" a knob turned on the
 * selected key since the frame before (its readout in the title). */
const int32_t drum_first = 18, drum_count = 3;
const uint8_t drum_kind[3] = { 1, 2, 3 };
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
        v->aenv = (int32_t)(1073741824.0 * pow(fd_amp_dec[fd_pos(k[FD_DECAY]) >> 7] / 1073741824.0, n));
        v->menv = (int32_t)(1073741824.0 * pow(fd_mod_dec[fd_pos(k[FD_FMDEC]) >> 7] / 1073741824.0, n));
        v->senv = (int32_t)(1073741824.0 * pow(fd_swp_dec[fd_pos(k[FD_SWDEC]) >> 7] / 1073741824.0, n));
        v->index_q8 = (int32_t)(fd_index_q8(k[FD_FM]) * (v->menv / 1073741824.0));
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
/* metal (engine 302 in previews, drum id 20): MT_PARAMS = the slot's 24 x 8 per-key values, MT_KEY = the selected key,
 * MT_PLAY = blocks since the selected key's note-on (metal.c itself renders them, its C loops: the voice the screen
 * reads is the engine's), MT_LIT = "key:level,..." other sounding keys, MT_TURN = "knob:value" a knob turned on the
 * selected key since the frame before. */
volatile int32_t te_drum_key[DP_VOICES];
volatile int16_t te_drum_vel[DP_VOICES];
const int32_t metal_id = 20;
void metal_render(void *o, int16_t *out, const int16_t *p);
void metal_note_on(void *o, const int16_t *key_params);
void metal_slot_init(int32_t slot);
static void mt_setup(void)
{
    const char *s = getenv("MT_PARAMS");
    for (int i = 0; s && i < DP_KEYS * 8; i++) {
        te_drum_params[i] = (int16_t)atoi(s);
        s = strchr(s, ',');
        s = s ? s + 1 : 0;
    }
    te_drum_edit = getenv("MT_KEY") ? atoi(getenv("MT_KEY")) : 0;
    int key = te_drum_edit;
    int16_t *k = te_drum_params + key * 8;
    metal_slot_init(0);
    if (getenv("MT_PLAY")) {
        static uint32_t obj[18];
        static int16_t out[128];
        int n = atoi(getenv("MT_PLAY"));
        te_drum_key[0] = key;
        te_drum_vel[0] = 0x7F00;
        metal_note_on(obj, k);
        for (int b = 0; b < n; b++)
            metal_render(obj, out, k);
    }
    const char *lit = getenv("MT_LIT");
    for (int j = 1; lit && j < MT_VOICES; j++) {
        metal_voice[j].key = atoi(lit);
        lit = strchr(lit, ':');
        metal_voice[j].aenv = lit ? atoi(lit + 1) << 15 : 0;
        metal_voice[j].age = 5;
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

/* Our FX for previews: ids 8 ensemble, 9 pingpong, 10 distortion, 11 crush, 12 repeat, 13 space (fx.json order). */
const uint8_t fx_kind[6] = { 1, 2, 3, 4, 5, 6 };
const int32_t fx_count = 6;
/* crush: the real crush.c (linked by preview.py) runs on silence until its knob smoothing has settled, so crush_state
 * holds what the device would publish; then CRUSH_GR (0 .. 32767) sets the gain reduction and CRUSH_LEVEL the input
 * level (silence gives none of either), CRUSH_FLIPS adds that many bit flips per frame, CRUSH_TURN=frame:knob:value
 * turns a raw knob before that frame (the readout). */
void crush_mono(void *self, int16_t *buf, const int16_t *p);
void crush_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);
/* What the FX get as params (fxpub.h): FX_LIVE="k0,k1,k2,k3" stands for TE's LFO-modulated copy (the stored knobs stay
 * K0..K3, so the crush readout and the stored-knob fallback can be told apart); else the stored knobs. Every frame
 * (host_tick) the FX publish these as their last block's knobs, ~23 blocks a frame, like running FX (FX_OFF=1: they
 * don't, as when the FX is switched off). */
struct fx_pub ensemble_pub[2], pingpong_pub[2], distortion_pub[2];
extern struct fx_pub crush_pub[2];                                  /* the real crush.c's */
static const int16_t *fx_params(void)
{
    static int16_t p[8];
    const char *s = getenv("FX_LIVE");
    for (int i = 0; i < 8; i++)
        p[i] = host_knobs[i];
    for (int i = 0; s && i < 4; i++) {
        p[i] = (int16_t)atoi(s);
        s = strchr(s, ',');
        s = s ? s + 1 : 0;
    }
    return p;
}
static void crush_settle(void)
{
    int16_t l[64], r[64];
    for (int b = 0; b < 200; b++) {
        memset(l, 0, sizeof l);
        memset(r, 0, sizeof r);
        crush_mono(0, l, fx_params());
        crush_stereo(0, l, r, fx_params());
    }
    const char *g = getenv("CRUSH_GR"), *lv = getenv("CRUSH_LEVEL");
    crush_state[0].gr = crush_state[1].gr = g ? atoi(g) : 0;
    crush_state[0].level = crush_state[1].level = lv ? atoi(lv) : 0;
}

static void crush_frame(int i)
{
    const char *fl = getenv("CRUSH_FLIPS"), *t = getenv("CRUSH_TURN");
    if (fl)
        for (int s = 0; s < 2; s++)
            crush_state[s].flips += (uint32_t)atoi(fl);
    if (t && atoi(t) == i) {
        host_knobs[atoi(strchr(t, ':') + 1)] = (int16_t)atoi(strrchr(t, ':') + 1);
        uint32_t fl = crush_state[0].flips;
        crush_settle();                    /* the DSP follows the turn (the readout shows the new value) */
        crush_state[0].flips = crush_state[1].flips = fl;
    }
}
struct ens { uint32_t lfo, vib; int32_t depth, vdepth, ens, mix; int32_t d[2][3]; };
struct ens ensemble_state[2] = { { 0x30000000u, 0, 0, 0, 0, 0, { { 0 } } }, { 0x30000000u, 0, 0, 0, 0, 0, { { 0 } } } };

struct pp pingpong_state[2];
/* repeat (engines 112 / 212): the real repeat.c (linked by preview.py) runs on the host, 1470 samples (one 30 fps frame)
 * of a 441 Hz sine before each frame, on TE's words as the device has them: the tape playing from position 0 at REP_BPM
 * (default 120) or, REP_TAPE=0, stopped (the metronome's countdown); REP_SHIFT=1 holds SHIFT (the probe page);
 * REP_TURN=frame:knob:value turns a stored knob before that frame (the readout). With
 * FX_OFF=n the FX stops running for the last n frames (the screen's FX-off look; the clock stays where it was).
 * REP_FORCE="active=1,rep=3,ph=690,..." skips the DSP and sets repeat_state directly every frame (the clock at REP_BPM;
 * ph and grem in 1/1000 bar; the other fields as in struct rep): the design's preview states, exactly. */
int16_t pingpong_ram[3 * 65536];
uint32_t fx_line_owner[2];
volatile int32_t te_tempo_spb, te_tape_state, te_metro_count, te_play_slot, te_ppqn_tick;
volatile uint32_t te_tape_pos;
/* repeat.c's manual key: TE's block counter and the drum / mixer controllers' press and release (stand-ins) */
volatile uint32_t te_blocks;
int te_drum_press(void *self, int key) { (void)self; (void)key; return 0; }
int te_drum_release(void *self, int key) { (void)self; (void)key; return 0; }
int te_master_press(void *self, int key) { (void)self; (void)key; return 0; }
int te_master_release(void *self, int key) { (void)self; (void)key; return 0; }
uint8_t host_shift;
int32_t host_tape_speed = 0x08000000;
extern struct fx_pub repeat_pub[2];
void repeat_mono(void *self, int16_t *buf, const int16_t *p);
void repeat_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);
static uint64_t rep_t;                      /* samples run */
static uint32_t rep_ph;
static void repeat_frame(int i, int frames, int stereo)
{
    float bpm = getenv("REP_BPM") ? (float)atof(getenv("REP_BPM")) : 120.0f;
    int rolling = !getenv("REP_TAPE") || atoi(getenv("REP_TAPE"));
    int off = getenv("FX_OFF") ? atoi(getenv("FX_OFF")) : 0;
    host_shift = getenv("REP_SHIFT") && atoi(getenv("REP_SHIFT"));
    const char *turn = getenv("REP_TURN");                        /* "frame:knob:value": a stored-knob turn */
    if (turn && atoi(turn) == i)
        host_knobs[atoi(strchr(turn, ':') + 1)] = (int16_t)atoi(strrchr(turn, ':') + 1);
    if (off && i >= frames - off)
        return;
    int32_t spb = (int32_t)(2646000.0f / bpm * 16384.0f);
    const char *force = getenv("REP_FORCE");
    if (force) {
        struct rep *q = &repeat_state[stereo];
        char buf[256], *t;
        q->spb = spb;
        q->bar8 = (uint32_t)(((int64_t)spb * 4) >> 6);
        q->lock = RL_TAPE;
        q->clocked = 1;
        snprintf(buf, sizeof buf, "%s", force);
        for (t = strtok(buf, ","); t; t = strtok(0, ",")) {
            char *e = strchr(t, '=');
            if (!e)
                continue;
            *e++ = 0;
            long v = strtol(e, 0, 0);
            if (!strcmp(t, "active")) q->active = (uint8_t)v;
            else if (!strcmp(t, "hold")) q->hold = (uint8_t)v;
            else if (!strcmp(t, "key")) q->key = (uint8_t)v;
            else if (!strcmp(t, "kind")) q->kind = (uint8_t)v;
            else if (!strcmp(t, "grid")) q->grid = (uint8_t)v;
            else if (!strcmp(t, "blue")) q->blue = (uint8_t)v;
            else if (!strcmp(t, "rep")) q->rep = (int16_t)v;
            else if (!strcmp(t, "depth")) q->depth = (int16_t)v;
            else if (!strcmp(t, "bar")) q->bar = (uint32_t)v;
            else if (!strcmp(t, "lost")) q->lost = (uint32_t)v;
            else if (!strcmp(t, "turn")) q->turn = (uint32_t)v;
            else if (!strcmp(t, "ph")) q->ph8 = (uint32_t)((uint64_t)q->bar8 * (uint64_t)v / 1000);
            else if (!strcmp(t, "grem")) q->grem = (int32_t)((uint64_t)q->bar8 * (uint64_t)v / 1000);
        }
        q->vpos = q->bar * q->bar8 + q->ph8;
        fx_publish(&repeat_pub[stereo], fx_params());
        return;
    }
    while (rep_t < (uint64_t)(i + 1) * 1470) {
        int16_t l[64], r[64];
        for (int j = 0; j < 64; j++, rep_ph += 42949673u) {          /* 441 Hz */
            l[j] = (int16_t)(12000 * sin(rep_ph * 1.4629180792671596e-09));
            r[j] = l[j];
        }
        te_tempo_spb = spb;
        te_tape_state = rolling ? 2 : 0;
        te_tape_pos = (uint32_t)(rep_t * 64);
        uint64_t q14 = rep_t << 14, beat = (uint64_t)spb;
        te_metro_count = (int32_t)(beat - q14 % beat);              /* clicks on the beats from 0 */
        if (stereo)
            repeat_stereo(0, l, r, fx_params());
        else
            repeat_mono(0, l, fx_params());
        rep_t += 64;
    }
}
/* space (engines 113 / 213): the real space.c (linked by preview.py, its C reference kernels) runs on the host, one
 * 30 fps frame of audio per frame: a 220 Hz saw at -12 dBFS for the first SPACE_PLAY frames (default 6), then silence,
 * so the level meter shows a tail; on TE's words as the device has them (the stored white knob = K2). The first
 * frames clear the line set (a clean start), as on the device; SPACE_KEY=1 holds the green push; SPACE_SHIFT=1 holds
 * SHIFT (the CPU page in diagnostic builds); SPACE_TURN=frame:knob:value turns a stored knob before that frame (the
 * readout). SPACE_FORCE="zone=1,level=16000,duck=8000,frz=32768,..." skips the DSP and
 * sets space_ui directly every frame (t60, level, duck, frz, inf, zone, key, clearing as in struct space_ui): the
 * design's preview states, exactly. */
volatile int32_t te_fx_slot[2];
volatile int16_t te_fx_knobs_synth[64], te_fx_knobs_drum[64];
void space_mono(void *self, int16_t *buf, const int16_t *p);
void space_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);
extern struct fx_pub space_pub[2];
static uint64_t sp_t;
static void space_frame(int i, int frames, int stereo)
{
    int off = getenv("FX_OFF") ? atoi(getenv("FX_OFF")) : 0, play = getenv("SPACE_PLAY") ? atoi(getenv("SPACE_PLAY")) : 6;
    host_shift = getenv("SPACE_SHIFT") && atoi(getenv("SPACE_SHIFT"));
    const char *turn = getenv("SPACE_TURN");                      /* "frame:knob:value": a stored-knob turn */
    if (turn && atoi(turn) == i)
        host_knobs[atoi(strchr(turn, ':') + 1)] = (int16_t)atoi(strrchr(turn, ':') + 1);
    if (off && i >= frames - off)
        return;
    te_fx_knobs_synth[2] = te_fx_knobs_drum[2] = host_knobs[2];
    const char *force = getenv("SPACE_FORCE");
    if (force) {
        struct space_ui *q = &space_ui[stereo];
        char buf[256], *t;
        q->t60_ms = 2000;
        q->duck = 32768;
        q->blocks += 23;
        te_blocks += 23;
        snprintf(buf, sizeof buf, "%s", force);
        for (t = strtok(buf, ","); t; t = strtok(0, ",")) {
            char *e = strchr(t, '=');
            if (!e)
                continue;
            *e++ = 0;
            long v = strtol(e, 0, 0);
            if (!strcmp(t, "t60")) q->t60_ms = (int32_t)v;
            else if (!strcmp(t, "level")) q->level = (int32_t)v;
            else if (!strcmp(t, "duck")) q->duck = (int32_t)v;
            else if (!strcmp(t, "frz")) q->frz = (int32_t)v;
            else if (!strcmp(t, "inf")) q->inf = (int32_t)v;
            else if (!strcmp(t, "zone")) q->zone = (uint8_t)v;
            else if (!strcmp(t, "key")) q->key = (uint8_t)v;
            else if (!strcmp(t, "clearing")) q->clearing = (uint8_t)v;
        }
        fx_publish(&space_pub[stereo], fx_params());
        return;
    }
    if (getenv("SPACE_KEY") && atoi(getenv("SPACE_KEY"))) {
        rep_key.held[stereo ? RK_MASTER : RK_SYNTH] = 1;
        rep_key.screen_seen = te_blocks;
    }
    while (sp_t < (uint64_t)(i + 1) * 1470) {
        int16_t l[64], r[64];
        for (int j = 0; j < 64; j++) {
            uint64_t t = sp_t + (uint64_t)j;
            int on = t < (uint64_t)play * 1470 && t >= 64 * 40;
            l[j] = r[j] = on ? (int16_t)((int32_t)((t * 220 * 65536 / 44100) & 0xFFFF) / 8 - 4096) : 0;
        }
        te_blocks++;
        if (stereo)
            space_stereo(0, l, r, fx_params());
        else
            space_mono(0, l, fx_params());
        sp_t += 64;
    }
}
static int host_fx;
/* One display frame of audio time (30 fps): the FX clocks the screens read. */
static void host_tick(int i)
{
    (void)i;
    for (int s = 0; s < 2; s++) {
        struct fx_pub *pubs[3] = { &ensemble_pub[s], &pingpong_pub[s], &distortion_pub[s] };
        if (host_fx == 12 || host_fx == 13)                 /* repeat and space run themselves (repeat_frame, ..) */
            continue;
        for (int j = 0; j < 3 && !getenv("FX_OFF"); j++) {   /* FX_OFF: the FX not running (switched off) */
            fx_publish(pubs[j], fx_params());
            pubs[j]->blocks += 22;
        }
        if (!getenv("FX_OFF"))
            crush_pub[s].blocks += 23;                      /* its knobs: what crush.c was given (crush_settle) */
        ensemble_state[s].vib += 0x0DDDDDDDu;               /* 6.5 Hz / 30 fps */
        ensemble_state[s].lfo += 0x01000000u;
        pingpong_state[s].w += 1470;
        if (!pingpong_state[s].d) {                         /* the delay blue sets (pingpong.h) at PP_BPM (120) */
            struct pp *q = &pingpong_state[s];
            int div, fold;
            q->spb = (int32_t)(2646000.0f / (getenv("PP_BPM") ? (float)atof(getenv("PP_BPM")) : 120.0f) * 16384.0f);
            q->dt = q->d = pp_target(fx_params()[0] < 0 ? 0 : fx_params()[0], q->spb, &div, &fold);
            q->div = (int16_t)div;
            q->fold = (int16_t)fold;
        }
    }
}
/* distortion_curve / distortion_mode_name: the real ones (distortion.c built with -DDISTORTION_CURVE_ONLY). */

/* SVG screens on the host: with SVG_DIR and SVG_IDS set (preview.py), the documents "exist" and node_find/matrix/
 * visible/fill/stroke act on a node table; after drawing, every node's state is printed as
 * "N doc id visible a b c d e f fill stroke" (matrix Q10, -1 = untouched; colours rrggbb or -) for preview.py to
 * apply to the SVG and rasterise. SVG_IDS lines: "doc id fill stroke" (the SVG's own colours). */
#define NODES 4096
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
    if (n_nodes == NODES) {                         /* a silently short table hides nodes the C looks for */
        fprintf(stderr, "preview_host: more than %d SVG ids, raise NODES\n", NODES);
        exit(1);
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
/* TE's FUN_019ead58 (notes/svg-route.md, Verified): copies m[9] into the node as its local matrix (replacing the SVG
 * transform; row vector, {a, b, 0, c, d, 0, e, f, 1} x 1024). Anything else in m[2], m[5], m[8] would make TE's
 * transform projective: refuse it here, so the preview cannot accept a layout the device would misread. */
void te_node_matrix(void *node, const int32_t *m)
{
    struct hnode *n = (struct hnode *)node;
    if (m[2] || m[5] || m[8] != 1024) {
        fprintf(stderr, "te_node_matrix %s: not TE's layout {a, b, 0, c, d, 0, e, f, 1024}\n", n->id);
        exit(5);
    }
    n->m[0] = m[0], n->m[1] = m[1], n->m[2] = m[3], n->m[3] = m[4], n->m[4] = m[6], n->m[5] = m[7];
    n->has_m = 1;
}
static int rendered = -1;                   /* the document of the last render (one screen per frame) */
void te_svg_render(void *screen) { rendered = (int)((intptr_t)*(void **)screen - 0x1000); }
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
        if (rendered < 0 || rendered >= n_docs || strcmp(n->doc, docs[rendered]))
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
    if (e == 15 && getenv("VA_SUB")) {      /* va: the sub's shape (knob 7); VA_TURN=1: a frame before it was turned */
        host_knobs[7] = (int16_t)atoi(getenv("VA_SUB"));
        if (getenv("VA_TURN")) {
            int16_t k7 = host_knobs[7];
            host_knobs[7] = 0;
            host_drawing = 0;
            ui_draw(e, 0);
            host_knobs[7] = k7;
            host_drawing = 1;
        }
    }
    if (e == 300)
        drum_ui_draw(0, 18);
    else if (e == 301) {
        fd_setup();
        if (getenv("FD_TURN")) {           /* a frame before the turn, then the turn */
            const char *t = getenv("FD_TURN");
            host_drawing = 0;
            drum_ui_draw(0, 19);
            te_drum_params[te_drum_edit * 8 + atoi(t)] = (int16_t)atoi(strchr(t, ':') + 1);
            host_drawing = 1;
        }
        drum_ui_draw(0, 19);
    }
    else if (e == 302) {
        mt_setup();
        if (getenv("MT_TURN")) {
            const char *t = getenv("MT_TURN");
            host_drawing = 0;
            drum_ui_draw(0, 20);
            te_drum_params[te_drum_edit * 8 + atoi(t)] = (int16_t)atoi(strchr(t, ':') + 1);
            host_drawing = 1;
        }
        drum_ui_draw(0, 20);
    }
    else if (e % 100 == 11 && getenv("CRUSH_FUZZ")) {
        /* crush screen fuzz (crush_fuzz_check.py): frame 0 with the state as the image loads it (before any DSP
         * block), then per frame extreme or random crush_state and raw knobs, mono and master alternating; after each
         * frame "F i" and every node's visibility and matrix (te_line segments print as they are drawn). */
        int n = atoi(getenv("CRUSH_FUZZ"));
        uint32_t seed = 12345;
        host_drawing = 1;
        for (int i = 0; i < n; i++) {
            printf("F %d\n", i);
            int pat = i % 7;
            for (int st = 0; st < 2 && i; st++) {
                struct crush *c = &crush_state[st];
                for (int j = 0; j < 4; j++) {
                    seed = seed * 1103515245u + 12345u;
                    c->k[j] = pat == 1 ? 0 : pat == 2 ? -1 : pat == 3 ? 32767 : pat == 4 ? -32768 : (int16_t)(seed >> 16);
                }
                int32_t v[5];
                for (int j = 0; j < 5; j++) {
                    seed = seed * 1103515245u + 12345u;
                    v[j] = pat == 1 ? 0 : pat == 2 ? -1 : pat == 3 ? 0x7FFFFFFF : pat == 4 ? (int32_t)0x80000000 :
                           pat == 5 ? (int32_t)seed : (int32_t)(seed >> 17);
                }
                c->gr = v[0], c->rate_hz = v[1], c->bits_q12 = v[2], c->mu = v[3], c->level = v[4];
                if (getenv("CRUSH_FUZZ_REACHABLE")) {      /* only what crush.c can publish */
                    for (int j = 0; j < 4; j++)
                        c->k[j] &= 0x7FFF;
                    c->gr = (uint32_t)v[0] % 32768, c->rate_hz = 399 + (uint32_t)v[1] % 43702;
                    c->bits_q12 = 4097 + (uint32_t)v[2] % 61440, c->mu = (uint32_t)v[3] % 32769;
                    c->level = (uint32_t)v[4] % 32768;
                }
                seed = seed * 1103515245u + 12345u;
                c->flips += pat == 6 ? seed >> 8 : seed >> 28;
            }
            for (int st = 0; st < 2 && i; st++)
                crush_pub[st].blocks += 23;                /* crush runs: the screen takes crush_state's knobs */
            for (int j = 0; j < 4; j++) {
                seed = seed * 1103515245u + 12345u;
                host_knobs[j] = i % 11 == 3 ? (int16_t)(seed >> 16) : i % 11 == 5 ? -32768 : (int16_t)((seed >> 17) & 0x7FFF);
            }
            fx_ui_draw(11, i & 1 ? 3 : 0, 0);
            dump_nodes();
        }
        return 0;
    }
    else if (e >= 100) {
        host_fx = e % 100;
        if (e % 100 == 11)
            crush_settle();
        for (int i = 0; i < frames; i++) {
            if (i == frames - 1)
                host_drawing = 1;
            host_tick(i);
            if (e % 100 == 12)
                repeat_frame(i, frames, e < 200);
            if (e % 100 == 13)
                space_frame(i, frames, e < 200);
            if (e % 100 == 11)
                crush_frame(i);
            fx_ui_draw(e % 100, e >= 200 ? 0 : 3, 0);
        }
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
