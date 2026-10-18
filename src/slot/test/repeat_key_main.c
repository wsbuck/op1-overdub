/* Simulator test of the repeat FX's manual key (repeat.h; repeat_key_check.py): the green encoder's push on the drum
 * and mixer controllers' slots (through their firmware entries: registers kept, TE's function called with the same
 * arguments for every other key and context), rep_key_event's rules, and the DSP's latch held by it.
 * stdout: "PASS ..." / "FAIL ..." lines. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "repeat.h"
#include "pingpong.h"

volatile int32_t te_tempo_spb = 22050 << 14, te_tape_state = 0, te_metro_count = 22050 << 14, te_play_slot;
volatile uint32_t te_tape_pos, te_blocks;
static int calls, last_fn, last_key;
static uint32_t last_self;
#define STUB(name, id) int name(void *self, int key) { calls++; last_fn = id; last_key = key; \
    last_self = (uint32_t)self; return key ^ 0x55; }
STUB(te_drum_press, 1) STUB(te_drum_release, 2) STUB(te_master_press, 3) STUB(te_master_release, 4)
void repeat_mono(void *self, int16_t *buf, const int16_t *p);
void repeat_stereo(void *self, int16_t *l, int16_t *r, const int16_t *p);
typedef void (*entry_t)(void);
int fx_abi_call(entry_t entry, void *self, void *a1, void *a2, void *a3);
extern void rk_drum_press_entry(void), rk_drum_release_entry(void), rk_master_press_entry(void),
    rk_master_release_entry(void);
static const entry_t entries[4] = { rk_drum_press_entry, rk_drum_release_entry, rk_master_press_entry,
                                    rk_master_release_entry };
static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* one call through entry e; returns the entry's R0 (fx_abi_call returns the register mask; R0 is read back) */
extern int fx_abi_ret;
static int call(int e, int key, int *mask)
{
    *mask = fx_abi_call(entries[e], (void *)0x1234u, (void *)(intptr_t)key, 0, 0);
    return fx_abi_ret;
}

int main(void)
{
    int mask, bad = 0, ok;
    /* 1. no repeat screen anywhere: every key on every slot goes to TE's function, registers kept */
    rep_key.screen_ctl = RK_SYNTH;
    rep_key.screen_seen = te_blocks = 1000;
    for (int e = 0; e < 4; e++)
        for (int k = 0; k < 0x46; k++) {
            calls = 0;
            int r = call(e, k, &mask);
            if (mask || calls != 1 || last_fn != e + 1 || last_key != k || last_self != 0x1234u || r != (k ^ 0x55))
                bad++;
        }
    check(!bad, "drum / mixer press and release entries: 4 x 70 keys with the repeat screen elsewhere: TE's function "
          "with the same this and key, its return value, callee-saved / loop / L registers kept");
    /* 2. the drum context's repeat screen: green's press is taken, others pass */
    rep_key.screen_ctl = RK_DRUM;
    calls = 0;
    ok = call(0, REP_KEY_GREEN, &mask) == 1 && !calls && !mask && rep_key.held[RK_DRUM] == 1;
    calls = 0;
    ok = ok && call(2, REP_KEY_GREEN, &mask) == (REP_KEY_GREEN ^ 0x55) && calls == 1 && !rep_key.held[RK_MASTER];
    calls = 0;
    ok = ok && call(0, 0x3B, &mask) == (0x3B ^ 0x55) && calls == 1 && call(0, 0x0D, &mask) == (0x0D ^ 0x55);
    check(ok, "drum repeat screen: green's press taken (TE not called, held), blue's and a note key go to TE, the "
          "mixer's green press goes to TE");
    calls = 0;
    ok = call(1, REP_KEY_GREEN, &mask) == (REP_KEY_GREEN ^ 0x55) && calls == 1 && !rep_key.held[RK_DRUM] && !mask;
    check(ok, "green's release: held cleared, TE's release still called");
    /* 3. the screen gone (no draw for > REP_KEY_STALE blocks): TE's again */
    te_blocks = rep_key.screen_seen + REP_KEY_STALE + 1;
    calls = 0;
    ok = call(0, REP_KEY_GREEN, &mask) == (REP_KEY_GREEN ^ 0x55) && calls == 1 && !rep_key.held[RK_DRUM];
    check(ok, "the repeat screen not drawn for 101 blocks: green's press goes to TE");
    rep_key.screen_seen = te_blocks;
    rep_key.screen_ctl = RK_MASTER;
    ok = call(2, REP_KEY_GREEN, &mask) == 1 && rep_key.held[RK_MASTER] && call(3, REP_KEY_GREEN, &mask) == (REP_KEY_GREEN ^ 0x55)
         && !rep_key.held[RK_MASTER];
    check(ok, "mixer repeat screen: green's press taken, its release clears");
    char buf[200];
    snprintf(buf, sizeof buf, "counters: %u presses, %u releases, %u taken, last 0x%02X on %d", (unsigned)rep_key.presses,
             (unsigned)rep_key.releases, (unsigned)rep_key.taken, rep_key.last, rep_key.last_ctl);
    check(rep_key.presses == 4 * 2 + 5 && rep_key.releases == 4 * 2 + 2 && rep_key.taken == 2 && rep_key.last == 0x3C
          && rep_key.last_ctl == RK_MASTER, buf);
    /* 4. the DSP (mono, the synth context, tape stopped, the metronome running, FORTUNE OFF, 1/16): held on the screen
     * = the latch on blue's grid line; release = dry within one crossfade; the screen gone = dry too */
    int16_t k[8] = { 12288, 1024, 9216, 16384 };
    int16_t b[64];
    rep_key.screen_ctl = RK_SYNTH;
    long t = 0, first = -1, back = -1, gone = -1;
    int32_t c = 7000 << 14;
    for (int blk = 0; blk < 2400; blk++) {
        te_blocks++;
        te_metro_count = c;
        c -= 64 << 14;
        while (c <= 0)
            c += 22050 << 14;
        if (blk < 1200 || (blk >= 1600 && blk < 2000))
            rep_key.screen_seen = te_blocks;            /* the screen draws ... */
        if (blk == 300 || blk == 1700)
            rep_key.held[RK_SYNTH] = 1;                 /* green pressed */
        if (blk == 900)
            rep_key.held[RK_SYNTH] = 0;                 /* released (the second press is never released: the screen goes) */
        for (int i = 0; i < 64; i++, t++)
            b[i] = (int16_t)lround(12000 * sin(t * 0.0731));
        repeat_mono(0, b, k);
        for (int i = 0; i < 64; i++) {
            long u = blk * 64L + i;
            int16_t dry = (int16_t)lround(12000 * sin(u * 0.0731));
            if (b[i] != dry && first < 0)
                first = u;
            if (blk >= 900 && blk < 1000 && b[i] != dry)
                back = u;
            if (blk >= 2000 && b[i] != dry)
                gone = u;
        }
    }
    /* our clock: the next click (7000 samples on) is beat 1; 1/16 lines every 5512.5 samples from there */
    double six = 22050 / 4.0, line0 = 7000.0;
    long press = 300 * 64;
    double ph = fmod((press - line0) + 1e9 * six, six), lag = ph <= 1323 ? 0 : six - ph;
    snprintf(buf, sizeof buf, "held on the synth screen: the latch starts at %ld (pressed at %ld, %.1f ms after a 1/16 line: "
             "expected %s)", first, press, ph / 44.1, ph <= 1323 ? "at once" : "on the next line");
    check(first >= press && first <= press + (long)lag + 2, buf);
    snprintf(buf, sizeof buf, "released: dry again within one crossfade (last repeat sample %ld, release at %d)", back, 900 * 64);
    check(back < 900 * 64 + 33, buf);
    snprintf(buf, sizeof buf, "held but the screen stops drawing: dry within %d blocks and a crossfade (last %ld)", REP_KEY_STALE,
             gone);
    check(gone < 2000 * 64 + (REP_KEY_STALE + 1) * 64 + 33, buf);
    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
