/* Simulator driver for knobhook_check.py: the synth encoder hook (slot.S/entry.inc KNOB_HOOK -> knobhook.c) and the
 * drum per-key loader/saver type compare (entry.inc DRUM_TYPE_EQ), with stand-ins for TE's functions. Every call also
 * goes through fx_abi.S's sentinel check (R4-R7, P3-P5, FP, SP, loops, L preserved). Prints "name result" lines and
 * "abi <mask>"; exit 1 on a failed expectation. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sprobe.h"
#include "harm_edit.h"
#include "fm4_edit.h"

typedef void (*entry_t)(void);
int fx_abi_call(entry_t entry, void *self, void *a1, void *a2, void *a3);
extern void knob_hook(void), drum_type_eq_load(void), drum_type_eq_save(void);

/* build_slot.py's table: ids 20 sprobe, 21 harm, 22 fm4 (HOOK_KINDS), 23 not hooked */
const uint8_t hook_kind[47] = { [5] = 1, [6] = 2, [7] = 3 };
extern struct harm_ui harm_ui;
extern struct fm4_ui fm4_ui;

/* TE stand-ins */
int16_t te_knobs[8 * 8];                   /* 0xFF8013C4 */
uint32_t dp_type_hits[2];                  /* drumprobe.c */
static int applied, last_knob, last_id, last_delta;
static int16_t *last_knobs;
void te_knob_apply(int32_t knob, int16_t *delta, int32_t id, int16_t *knobs)    /* FUN_01a1dfec, type 3 */
{
    applied++;
    last_knob = knob;
    last_delta = *delta;
    last_id = id;
    last_knobs = knobs;
    int32_t v = knobs[knob] + *delta;
    int32_t lo = id == 21 || (id == 22 && knob >= 4) ? -32768 : 0;      /* harm's and fm4's rows (engines.json) */
    knobs[knob] = (int16_t)(v < lo ? lo : v > 32767 ? 32767 : v);
}
struct value { const char *s; uint32_t pad[3]; };   /* TE's 16-byte JSON value, as far as these calls go */
static int eqs, strs, frees, bad_free;
int te_json_eq(const struct value *a, const struct value *b)                    /* FUN_019bdea4 */
{
    eqs++;
    return a->s && b->s && !strcmp(a->s, b->s);
}
struct value *te_json_str(struct value *v, const char *s)                      /* FUN_019bdc4c */
{
    strs++;
    v->s = s;
    return v;
}
void te_json_free(struct value *v, int flag)                                     /* FUN_019bdd18 */
{
    frees++;
    bad_free |= flag != 2 || !v->s;
    v->s = 0;
}

static int mask, fails;
static void expect(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    fails += !ok;
}

static void turn(int knob, int delta, int id, int slot)
{
    static int16_t d;
    d = (int16_t)delta;
    mask |= fx_abi_call(knob_hook, (void *)(intptr_t)knob, &d, (void *)(intptr_t)id, te_knobs + slot * 8);
}

static int type_eq(entry_t e, const char *type, int *result)
{
    static struct value t, dbox;
    t.s = type;
    dbox.s = "dbox";
    eqs = strs = frees = 0;
    mask |= fx_abi_call(e, &t, &dbox, 0, 0);
    int n = eqs;
    eqs = strs = frees = 0;
    *result = ((int (*)(struct value *, struct value *))e)(&t, &dbox);
    return n;
}

int main(void)
{
    /* another engine (id 5): TE's function gets exactly the four arguments, the hook does nothing */
    te_knobs[2 * 8 + 6] = 1234;
    turn(3, 7, 5, 2);
    expect(applied == 1 && last_knob == 3 && last_delta == 7 && last_id == 5 && last_knobs == te_knobs + 16
           && te_knobs[16 + 3] == 7 && te_knobs[16 + 6] == 1234 && knobhook_diag.calls == 0, "other engine: TE's call untouched");
    /* the probe (id 20), slot 3: knobs 0..5 still through TE, then 6..7 packed */
    int16_t *k = te_knobs + 3 * 8;
    turn(1, 100, 20, 3);
    expect(applied == 2 && last_knob == 1 && last_id == 20 && k[1] == 100, "probe knob 1: TE's type-3 call made");
    expect(k[6] == sp_k6(1, 1) && k[7] == sp_k7(k[6]) && k[6] < 0 && (uint16_t)k[7] >= 0x8000,
           "probe: K6 negative (edits 1, knob 1), K7 raw >= 0x8000 = f(K6)");
    expect(knobhook_diag.calls == 1 && knobhook_diag.slot == 3 && knobhook_diag.knob == 1 && knobhook_diag.delta == 100
           && knobhook_diag.w6 == k[6] && knobhook_diag.w7 == k[7], "probe: diag records calls, slot, knob, delta, writes");
    turn(6, -3, 20, 3);
    turn(7, 5, 20, 3);
    expect(applied == 2 && sp_edits(k[6]) == 3 && (k[6] & 7) == 7 && k[7] == sp_k7(k[6]),
           "probe knobs 6, 7: not passed to TE; edits 3, last knob 7");
    k[6] = sp_k6(37, 2);                   /* a loaded sound with 37 edits */
    k[7] = sp_k7(k[6]);
    turn(4, 1, 20, 3);
    expect(applied == 3 && sp_edits(k[6]) == 38 && k[4] == 1, "probe: edits continue from the loaded K6; knob 4 via TE");
    k[6] = 500;                            /* not ours (positive): starts at 1 */
    turn(0, 1, 20, 3);
    expect(sp_edits(k[6]) == 1, "probe: a K6 without the mark restarts the count");
    uint32_t e = 4095;
    expect(sp_edits(sp_k6(e + 1, 0)) == 0 && sp_k7(sp_k6(0, 0)) < 0, "packing wraps at 4096 edits, K7 always >= 0x8000");

    /* harm (id 21), slot 4, with a delta of 512 per detent: TE gets delta 0 after harm_edit */
    int16_t *h = te_knobs + 4 * 8;
    for (int i = 0; i < 8; i++)
        h[i] = 0;
    h[0] = harm_k0(0, 0);
    knobhook_diag.dmin = 0;                /* the probe's turns above taught it 1: start as after a boot */
    int before = applied;
    turn(1, 512, 21, 4);
    expect(applied == before + 1 && last_delta == 0 && last_knob == 1 && last_id == 21 && harm_level(h, 0) == 1,
           "harm level: one detent = one step, TE called with delta 0");
    expect(knobhook_diag.delta == 512 && knobhook_diag.dmin == 512 && knobhook_diag.slot == 4,
           "harm: diag records the raw delta and dmin");
    turn(0, 2048, 21, 4);
    turn(0, 2048, 21, 4);
    expect(harm_ui.cursor == 2, "harm cursor: a fast turn still moves one bar per call");
    turn(1, 4 * 512, 21, 4);
    expect(harm_level(h, 2) == 4 && h[0] == harm_k0(0, 0), "harm level: 4 x the slow detent = 4 steps");
    turn(7, -512, 21, 4);
    turn(3, 1024, 21, 4);
    expect(harm_uni(h) == 2 && harm_warp(h) == 0, "harm unison +2, warp clamps at 0");
    turn(9, 512, 21, 4);
    expect(last_knob == 9 && last_delta == 512, "harm: an unknown knob index goes to TE untouched");

    /* fm4 (id 22), slot 5: MAIN plain knobs pass to TE with TE's delta */
    int16_t *f = te_knobs + 5 * 8;
    int16_t v0[8] = { 11585, 13405, 6000, 0, 1536, 1536, 14848, 6000 };
    for (int i = 0; i < 8; i++)
        f[i] = v0[i];
    turn(0, 512, 22, 5);
    expect(last_delta == 512 && last_knob == 0 && f[0] == 11585 + 512 && fm4_ui.page == 0, "fm4 MAIN: blue passes to TE");
    turn(4, 512, 22, 5);
    expect(last_delta == 0 && fm4_ui.page == 1 && fm4_is_packed(f), "fm4 SHIFT blue: page A, the sound packed, TE delta 0");
    struct fm4_patch p;
    fm4_decode(f, &p);
    int lvl = p.level[0];
    turn(2, -512, 22, 5);
    fm4_decode(f, &p);
    expect(last_delta == 0 && p.level[0] == lvl - 1 && f[2] == 6000, "fm4 page A: white = A's level -1, knob 2 untouched");
    turn(4, 4096, 22, 5);
    expect(fm4_ui.page == 2, "fm4 page: one page per call however fast");
    /* the device's delta: 1024 per detent (SYNTH3: D1024 M1024); page B */
    knobhook_diag.dmin = 0;
    fm4_decode(f, &p);
    int c0 = p.coarse[1], f0 = p.fine[1], l0 = p.level[1], e0 = p.env[1];
    turn(0, 1024, 22, 5);
    turn(1, 1024, 22, 5);
    turn(2, -1024, 22, 5);
    turn(3, 1024, 22, 5);
    fm4_decode(f, &p);
    expect(knobhook_diag.dmin == 1024 && p.coarse[1] == c0 + 1 && p.fine[1] == f0 + 1 && p.level[1] == l0 - 1 &&
           p.env[1] == e0 + 1, "1024 per detent: coarse, fine, level, envelope one step each");
    turn(0, 2048, 22, 5);
    turn(1, 3072, 22, 5);
    turn(3, 4096, 22, 5);
    fm4_decode(f, &p);
    expect(p.coarse[1] == c0 + 3 && p.fine[1] == f0 + 2 && p.env[1] == e0 + 2,
           "accelerated 2048..4096: coarse 2 steps, fine and envelope still one");
    turn(4, -1024, 22, 5);
    expect(fm4_ui.page == 1, "1024: one page back");
    turn(4, -512, 22, 5);
    turn(4, -512, 22, 5);
    expect(fm4_ui.page == 0, "fm4 back to MAIN");
    turn(1, 300, 22, 5);
    expect(last_delta == 300 && f[1] == 13405 + 300, "fm4 MAIN green passes (TE's own delta)");

    /* not hooked (id 23) */
    turn(2, 77, 23, 6);
    expect(last_id == 23 && last_delta == 77 && last_knob == 2, "id 23 (not hooked): TE's call untouched");

    /* drum type compare */
    int r, n;
    n = type_eq(drum_type_eq_load, "dbox", &r);
    expect(r == 1 && n == 1 && strs == 0 && frees == 0 && dp_type_hits[0] == 0, "dbox: TE's result, one compare, nothing else");
    n = type_eq(drum_type_eq_load, "drum", &r);
    expect(r == 0 && n == 4 && strs == 3 && frees == 3 && !bad_free && dp_type_hits[0] == 0,
           "drum (not reached by TE, but harmless): 0, each of our Values built and freed");
    n = type_eq(drum_type_eq_load, "dprobe", &r);
    expect(r == 1 && n == 2 && strs == 1 && frees == 1 && !bad_free && dp_type_hits[0] == 2,
           "dprobe on load: 1 (dbox path), Value freed with 2, load hits counted");
    n = type_eq(drum_type_eq_save, "dprobe", &r);
    expect(r == 1 && dp_type_hits[1] == 2 && dp_type_hits[0] == 2, "dprobe on save: 1, save hits counted");
    n = type_eq(drum_type_eq_load, "fmdrum", &r);
    expect(r == 1 && n == 3 && strs == 2 && frees == 2 && !bad_free && dp_type_hits[0] == 4,
           "fmdrum (second name) on load: 1, both Values freed, load hits counted");
    n = type_eq(drum_type_eq_save, "fmdrum", &r);
    expect(r == 1 && dp_type_hits[1] == 4, "fmdrum on save: 1, save hits counted");
    n = type_eq(drum_type_eq_load, "metal", &r);
    expect(r == 1 && n == 4 && strs == 3 && frees == 3 && !bad_free && dp_type_hits[0] == 6,
           "metal (third name) on load: 1, all three Values freed, load hits counted");
    n = type_eq(drum_type_eq_save, "metal", &r);
    expect(r == 1 && dp_type_hits[1] == 6, "metal on save: 1, save hits counted");
    n = type_eq(drum_type_eq_save, "sampler", &r);
    expect(r == 0 && dp_type_hits[1] == 6 && frees == 3, "other types: 0, nothing counted, every Value freed");
    printf("abi %08x\n", mask);
    return fails || mask;
}
