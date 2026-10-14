/* Simulator driver for knobhook_check.py: the synth encoder hook (slot.S/entry.inc KNOB_HOOK -> knobhook.c) and the
 * drum per-key loader/saver type compare (entry.inc DRUM_TYPE_EQ), with stand-ins for TE's functions. Every call also
 * goes through fx_abi.S's sentinel check (R4-R7, P3-P5, FP, SP, loops, L preserved). Prints "name result" lines and
 * "abi <mask>"; exit 1 on a failed expectation. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sprobe.h"

typedef void (*entry_t)(void);
int fx_abi_call(entry_t entry, void *self, void *a1, void *a2, void *a3);
extern void knob_hook(void), drum_type_eq_load(void), drum_type_eq_save(void);

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
    knobs[knob] = (int16_t)(v < 0 ? 0 : v > 32767 ? 32767 : v);
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

    /* drum type compare */
    int r, n;
    n = type_eq(drum_type_eq_load, "dbox", &r);
    expect(r == 1 && n == 1 && strs == 0 && frees == 0 && dp_type_hits[0] == 0, "dbox: TE's result, one compare, nothing else");
    n = type_eq(drum_type_eq_load, "drum", &r);
    expect(r == 0 && n == 2 && strs == 1 && frees == 1 && !bad_free && dp_type_hits[0] == 0,
           "drum (not reached by TE, but harmless): 0, our Value built and freed");
    n = type_eq(drum_type_eq_load, "dprobe", &r);
    expect(r == 1 && n == 2 && strs == 1 && frees == 1 && !bad_free && dp_type_hits[0] == 2,
           "dprobe on load: 1 (dbox path), Value freed with 2, load hits counted");
    n = type_eq(drum_type_eq_save, "dprobe", &r);
    expect(r == 1 && dp_type_hits[1] == 2 && dp_type_hits[0] == 2, "dprobe on save: 1, save hits counted");
    n = type_eq(drum_type_eq_save, "sampler", &r);
    expect(r == 0 && dp_type_hits[1] == 2, "other types: 0");
    printf("abi %08x\n", mask);
    return fails || mask;
}
