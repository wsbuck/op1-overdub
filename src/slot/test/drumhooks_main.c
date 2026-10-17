/* Simulator driver for drumhooks_check.py: slot.S's DRUM_HOOKS for three drum engines (ids 23 .. 25: dprobe, fmdrum,
 * metal; drumhooks/slot_gen.inc)
 * against stand-ins for TE: the drum factory (TE's frame, dbox voice constructor and index setter, the assert path),
 * the validator's name lookup, and the dbox screen update. Every call goes through fx_abi.S's sentinel check (R4-R7,
 * P3-P5, FP, SP, loops, L preserved). Prints "ok/FAIL what" lines and "abi <mask>"; exit 1 on a failure. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef void (*entry_t)(void);
int fx_abi_call(entry_t entry, void *self, void *a1, void *a2, void *a3);
extern void factory_call(void), drum_validate_lookup(void), drum_ui_update(void);
extern uint32_t drum_vtable23[], drum_vtable24[], drum_vtable25[];
extern uint32_t dp_type_hits[2];

/* TE stand-ins */
uint32_t assert_r0;
static uint32_t voices[12][18];
static int built, indexed, init_calls[3], init_slot[3];
void *te_dbox_voice(int32_t i)                    /* FUN_0197189c: dbox voice i, built in place */
{
    built++;
    for (int j = 0; j < 18; j++)                   /* no memset: newlib's uses loop registers (TE's ctor: unknown) */
        voices[i][j] = 0;
    voices[i][0] = 0x0190F8A4;
    return voices[i];
}
void te_dbox_index(uint32_t *v, int32_t i)        /* FUN_01970da8: +0x14 = i */
{
    indexed++;
    v[5] = (uint32_t)i;
}
void drumA_slot_init(int32_t slot) { init_calls[0]++; init_slot[0] = slot; }
void drumB_slot_init(int32_t slot) { init_calls[1]++; init_slot[1] = slot; }
void drumC_slot_init(int32_t slot) { init_calls[2]++; init_slot[2] = slot; }
static int32_t lookup_ret;
int32_t te_engine_lookup(const char *name) { (void)name; return lookup_ret; }     /* FUN_019b9810 */
static int dbox_updates, draws, draw_slot, draw_id;
void dbox_page_update(void *page, int dt) { (void)page; (void)dt; dbox_updates++; }
void drum_ui_draw(int slot, int id) { draws++; draw_slot = slot; draw_id = id; }
/* the rest of slot.S's references */
void iter_update(void) {}
void ui_draw(int e, int s) { (void)e; (void)s; }
void filter_page_update(void) {}
void fx_ui_draw(int i, int m, int s) { (void)i; (void)m; (void)s; }
void hold_keyup(void) {}
void hold_shiftkey(void) {}
void chord_keydown(void) {}
void c2_press(void) {}                              /* CHORD v2's hooks (entry.inc C2_HOOKS) */
void c2_release(void) {}
void c2_knob(void) {}
void c2_frame(void) {}
void te_frame_render(void) {}
void c2_seq_tick(void) {}
void te_seq_tick(void) {}
int te_json_eq(void *a, void *b) { (void)a; (void)b; return 0; }
void *te_json_str(void *v, const char *s) { (void)s; return v; }
void te_json_free(void *v, int f) { (void)v; (void)f; }

static int mask, fails;
static void expect(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    fails += !ok;
}

static void factory(int id, int slot, uint32_t **out)
{
    memset(out, 0, 13 * sizeof *out);
    built = indexed = init_calls[0] = init_calls[1] = init_calls[2] = 0;
    init_slot[0] = init_slot[1] = init_slot[2] = -1;
    assert_r0 = 0;
    mask |= fx_abi_call(factory_call, (void *)(intptr_t)id, out, (void *)(intptr_t)slot, 0);
}

static int voices_ok(uint32_t **out, const uint32_t *vt)
{
    int ok = out[12] == 0;
    for (int i = 0; i < 12; i++)
        ok &= out[i] == voices[i] && voices[i][0] == (uint32_t)vt && voices[i][5] == (uint32_t)i;
    return ok;
}

static int32_t call1(entry_t e, const char *arg)        /* through a parameter, as knobhook_main.c's type_eq */
{
    return ((int32_t (*)(const char *))e)(arg);
}

static int32_t lookup(int32_t te_says)
{
    lookup_ret = te_says;
    mask |= fx_abi_call(drum_validate_lookup, (void *)"name", 0, 0, 0);
    return call1(drum_validate_lookup, "name");
}

volatile int32_t te_drum_slot, te_drum_ids[8];          /* 0xFF8016D8, 0xFF801704 on the device */
#define DRUM_SLOT te_drum_slot
#define DRUM_IDS te_drum_ids

static void ui(int slot, int id)
{
    DRUM_SLOT = slot;
    DRUM_IDS[slot] = id;
    dbox_updates = draws = 0;
    draw_slot = draw_id = -1;
    mask |= fx_abi_call(drum_ui_update, (void *)0x1234, (void *)17, 0, 0);
}

int main(void)
{
    static uint32_t *out[13];
    factory(23, 5, out);
    expect(voices_ok(out, drum_vtable23) && built == 12 && indexed == 12 && init_calls[0] == 1 && init_slot[0] == 5
           && init_calls[1] == 0 && !assert_r0, "factory id 23: engine A's slot init (slot 5), 12 dbox voices on vtable 23");
    factory(24, 2, out);
    expect(voices_ok(out, drum_vtable24) && built == 12 && init_calls[1] == 1 && init_slot[1] == 2 && init_calls[0] == 0
           && init_calls[2] == 0 && !assert_r0, "factory id 24: engine B's slot init (slot 2), 12 dbox voices on vtable 24");
    factory(25, 7, out);
    expect(voices_ok(out, drum_vtable25) && built == 12 && init_calls[2] == 1 && init_slot[2] == 7 && init_calls[0] == 0
           && init_calls[1] == 0 && !assert_r0, "factory id 25: engine C's slot init (slot 7), 12 dbox voices on vtable 25");
    int ids[] = { 22, 26, 10, 0, -1, 0x10017, 0x10019 };
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        factory(ids[i], 1, out);
        char what[80];
        snprintf(what, sizeof what, "factory id %d: TE's assert path (R0.L = 0x5A40), nothing built", ids[i]);
        expect((assert_r0 & 0xFFFF) == 0x5A40 && built == 0 && init_calls[0] + init_calls[1] + init_calls[2] == 0
               && !out[0], what);
    }
    expect(lookup(23) == 9 && lookup(24) == 9 && lookup(25) == 9, "validator lookup: ids 23, 24, 25 read as dbox (9)");
    expect(lookup(22) == 22 && lookup(26) == 26 && lookup(9) == 9 && lookup(8) == 8 && lookup(-1) == -1,
           "validator lookup: other ids unchanged");
    ui(3, 24);
    expect(draws == 1 && draw_slot == 3 && draw_id == 24 && !dbox_updates, "screen: slot 3 with id 24 -> drum_ui_draw(3, 24)");
    ui(0, 23);
    expect(draws == 1 && draw_slot == 0 && draw_id == 23 && !dbox_updates, "screen: id 23 -> drum_ui_draw(0, 23)");
    ui(6, 25);
    expect(draws == 1 && draw_slot == 6 && draw_id == 25 && !dbox_updates, "screen: id 25 -> drum_ui_draw(6, 25)");
    ui(7, 9);
    expect(dbox_updates == 1 && !draws, "screen: dbox (9) -> TE's update");
    ui(1, 26);
    expect(dbox_updates == 1 && !draws, "screen: id 26 -> TE's update");
    expect(dp_type_hits[0] == 0 && dp_type_hits[1] == 0, "dp_type_hits defined by DRUM_HOOKS, zero");
    printf("abi %08x\n", mask);
    return fails || mask;
}
