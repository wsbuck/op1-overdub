/* The bfin GCC 4.3 -O2 hardware-loop CC miscompile (src/slot/test/loopcc.py), as CHORD v2's first factory restore
 * had it: a counted loop with a branch on a loop-invariant test. loopcc_check.py builds this at -O2 and runs it in
 * the simulator: restore_bad must come out wrong (the toolchain still has the bug, and loopcc.py must flag its
 * assembly), restore_flat and restore_mask (the forms we use instead) right. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct ch { int8_t root; uint8_t q, fam; int8_t top; int8_t iv[4]; uint8_t n, hs; int8_t sl; uint8_t pad; };
#define FN 4
#define KEYS 24
static const struct ch fac[FN][KEYS] = {
    [2] = { [0] = { 0, 21 }, [2] = { 0, 26 }, [3] = { 0, 19 }, [5] = { 0, 23 }, [11] = { 0, 6 }, [23] = { 0, 9 } },
};
struct ch g[8][KEYS];

static void cp(void *dst, const void *src, int n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

static void clr(struct ch *c)
{
    c->root = 0; c->q = 0; c->fam = 0; c->top = 0;
    for (int i = 0; i < 4; i++)
        c->iv[i] = 0;
    c->n = 0; c->hs = 0; c->sl = 0; c->pad = 0;
}

/* the trigger: `m < FN` is hoisted into CC; the outer loop is not a hardware loop (two latches), so its counter test
 * becomes `P4 += -1; cc =P4==0; if !cc jump` and every key after the first takes the clr branch */
__attribute__((noinline)) void restore_bad(int m)
{
    for (int k = 0; k < KEYS; k++) {
        if (m < FN)
            cp(&g[m][k], &fac[m][k], sizeof g[m][k]);
        else
            clr(&g[m][k]);
    }
}

/* CHORD v2's form since f9a12f8: the test outside, one flat copy */
__attribute__((noinline)) void restore_flat(int m)
{
    if (m < FN)
        cp(g[m], fac[m], sizeof g[m]);
    else
        for (int k = 0; k < KEYS; k++)
            clr(&g[m][k]);
}

/* modal.c strike's form: a select inside the loop as a mask (no branch, no CC) */
__attribute__((noinline)) int32_t energy_mask(int plain, int len)
{
    uint32_t sd = 1;
    int32_t lp = 0, e = 0, pm = -plain;
    for (int n = 0; n < len; n++) {
        sd = sd * 1103515245u + 12345u;
        int32_t w = (int32_t)(sd >> 17) - 16384;
        lp += (w - lp) >> 3;
        int32_t x = lp ^ ((w ^ lp) & pm);
        e += (x >> 8) * (x >> 8);
    }
    return e;
}

__attribute__((noinline)) int32_t energy_ref(int plain, int len)
{
    uint32_t sd = 1;
    int32_t lp = 0, e = 0;
    for (int n = 0; n < len; n++) {
        sd = sd * 1103515245u + 12345u;
        int32_t w = (int32_t)(sd >> 17) - 16384;
        lp += (w - lp) >> 3;
        volatile int32_t x = plain ? w : lp;               /* volatile: CC is set right here, every time */
        e += (x >> 8) * (x >> 8);
    }
    return e;
}

static int bad_keys(int m)
{
    int bad = 0;
    for (int k = 0; k < KEYS; k++)
        bad += memcmp(&g[m][k], &fac[m][k], sizeof g[m][k]) != 0;
    return bad;
}

int main(void)
{
    restore_bad(2);
    printf("restore_bad %d\n", bad_keys(2));
    memset(g, 0x55, sizeof g);
    restore_flat(2);
    int flat = bad_keys(2);
    restore_flat(6);
    int empty = 0;
    for (int k = 0; k < KEYS; k++)
        empty += g[6][k].q != 0 || g[6][k].iv[3] != 0;
    printf("restore_flat %d %d\n", flat, empty);
    printf("energy_mask %d\n", energy_mask(0, 100) != energy_ref(0, 100) || energy_mask(1, 100) != energy_ref(1, 100));
    return 0;
}
