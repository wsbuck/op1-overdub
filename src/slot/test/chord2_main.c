/* Simulator test of CHORD v2's hook entries (entry.inc C2_HOOKS) with hold.c built by the device compiler:
 * every entry preserves what a TE caller relies on (fx_abi.S), passes TE's arguments and result through unchanged
 * when CHORD v2 is left out (or the page is closed), and the frame hook calls TE's render with the frame buffer
 * first, then c2_frame. Built and run by chord2_check.py. */
#include <stdint.h>
#include <stdio.h>
#include "chord2.h"

typedef void (*entry_t)(void);
int fx_abi_call(entry_t entry, void *self, void *a1, void *a2, void *a3);
extern int c2_press_entry(void *self, int key);
extern int c2_release_entry(void *self, int key);
extern int c2_knob_entry(void *self, int knob, int16_t *delta);
extern void c2_frame_hook(void *buf);
extern struct c2_state c2;

uint32_t feature_mask = 7;
static int fails, order, render_at, frame_at, n_press, n_rel, n_knob;
static void *last_self, *last_buf;
static int last_key, last_knob;
static int16_t *last_delta;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } else { printf("PASS " __VA_ARGS__); printf("\n"); } } while (0)

/* TE stand-ins (the symbols the slot block gets as --defsym) */
void te_make_note(int32_t *n, int key, int vel) { n[0] = key + 0x35; n[1] = vel; }
void te_note_on(int32_t *n, int f) { (void)n; (void)f; }
void te_note_off(int32_t *n) { (void)n; }
int te_synth_shiftkey(void *s, int k) { (void)s; (void)k; return 0; }
int te_synth_press(void *s, int k) { last_self = s; last_key = k; n_press++; return 5; }
int te_synth_release(void *s, int k) { last_self = s; last_key = k; n_rel++; return 0; }
int te_synth_knob(void *s, int k, int16_t *d) { last_self = s; last_knob = k; last_delta = d; n_knob++; return 9; }
int te_yaffs_open(const char *p, int f, int m) { (void)p; (void)f; (void)m; return -1; }
int te_yaffs_read(int fd, void *b, unsigned n) { (void)fd; (void)b; (void)n; return -1; }
int te_yaffs_write(int fd, const void *b, unsigned n) { (void)fd; (void)b; (void)n; return -1; }
int te_yaffs_close(int fd) { (void)fd; return 0; }
int te_yaffs_mkdir(const char *p, int m) { (void)p; (void)m; return -1; }
void te_frame_render(void *buf) { last_buf = buf; render_at = ++order; }
void c2_frame(void) { frame_at = ++order; }

int main(void)
{
    int16_t d = -1024;
    uint32_t mask = 0;
    for (int fm = 3; fm <= 7; fm += 4) {
        feature_mask = (uint32_t)fm;
        c2.page = 0;
        n_press = n_rel = n_knob = 0;
        CHECK(c2_press_entry((void *)0x1234, 0xA) == 5 && last_self == (void *)0x1234 && last_key == 0xA && n_press == 1,
              "mask %d, page closed: LIFT reaches TE's press slot with its arguments and result", fm);
        CHECK(c2_release_entry((void *)0x4321, 0x26) == 0 && last_self == (void *)0x4321 && last_key == 0x26 && n_rel == 1,
              "mask %d: SHIFT release reaches TE's release slot", fm);
        CHECK(c2_knob_entry((void *)0x5555, 2, &d) == 9 && last_self == (void *)0x5555 && last_knob == 2 &&
              last_delta == &d && n_knob == 1, "mask %d, page closed: the encoder reaches TE with this, knob, delta", fm);
        mask |= (uint32_t)fx_abi_call((entry_t)c2_press_entry, (void *)0x1234, (void *)7, 0, 0);
        mask |= (uint32_t)fx_abi_call((entry_t)c2_release_entry, (void *)0x1234, (void *)0xC, 0, 0);
        mask |= (uint32_t)fx_abi_call((entry_t)c2_knob_entry, (void *)0x1234, (void *)1, &d, 0);
    }
    feature_mask = 7;
    c2.page = 1;
    n_press = n_knob = 0;
    uint32_t k0 = c2.dg.knobs;
    c2.loading = 1;                  /* the edit returns before reading TE's slot (an absolute address the sim lacks) */
    CHECK(c2_knob_entry((void *)0x5555, 1, &d) == 0 && n_knob == 0 && c2.dg.knobs == k0 + 1,
          "page open: the encoder is ours, TE not called");
    CHECK(c2_press_entry((void *)0x1234, 0xC) == 1 && n_press == 0 && c2.split_down, "page open: SPLIT is ours");
    CHECK(c2_press_entry((void *)0x1234, 6) == 5 && !c2.page && n_press == 1, "T1 closes the page and reaches TE");
    c2.page = 1;
    mask |= (uint32_t)fx_abi_call((entry_t)c2_knob_entry, (void *)0x1234, (void *)1, &d, 0);
    c2.loading = 0;
    feature_mask = 3;
    n_knob = 0;
    CHECK(c2_knob_entry((void *)0x5555, 1, &d) == 9 && n_knob == 1, "CHORD v2 left out: an open page flag never swallows");
    c2.page = 0;
    order = 0;
    c2_frame_hook((void *)0x02000000);
    CHECK(last_buf == (void *)0x02000000 && render_at == 1 && frame_at == 2, "frame hook: TE renders the buffer, then c2_frame");
    mask |= (uint32_t)fx_abi_call((entry_t)c2_frame_hook, (void *)0x03000000, 0, 0, 0);
    CHECK(last_buf == (void *)0x03000000, "frame hook under the ABI harness renders the right buffer");
    printf("abi %08x\n", (unsigned)mask);
    CHECK(mask == 0, "every entry preserves R4-R7, P3-P5, FP, SP, loops, L0-L3");
    printf(fails ? "FAILED\n" : "all passed\n");
    return fails != 0;
}
