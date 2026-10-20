/* space_check.py: every kernel of the space FX, asm (a_) against the C reference (c_), on random inputs (300 trials, a third at full scale); prints "N failures". */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
volatile uint32_t te_blocks; volatile int32_t te_fx_slot[2]; volatile int16_t te_fx_knobs_synth[64], te_fx_knobs_drum[64];
int16_t pingpong_ram[3*65536]; uint32_t fx_line_owner[2];
#define D(n, ...) void a_##n(__VA_ARGS__); void c_##n(__VA_ARGS__);
D(ap, int16_t *x, int16_t *ring, uint32_t mask, uint32_t t, int32_t dly, int32_t g, int32_t g2c, int n)
D(send, int16_t *u, const int16_t *pd, uint32_t pmask, uint32_t dpos, const int16_t *er, int32_t sd, int32_t dsd, int32_t se, int32_t dse, int n)
D(pole, int16_t *x, int32_t *s, int32_t a, int hp, int n)
D(taps, int16_t *er, const int16_t *ring, uint32_t mask, const uint32_t *pos, const int32_t *g, int n)
D(wht, int32_t F[8][64], int32_t *l, int32_t *r, int n)
D(inj, int32_t *P, int32_t *M, const int16_t *ul, const int16_t *ur, int n)
D(write, int16_t *ln, uint32_t mask, uint32_t t, const int32_t *row, const int32_t *inj, int32_t sgn, int near, int n)
D(mix, int16_t *x, const int16_t *y, int32_t g, int32_t dg, int32_t w, int32_t dw, int n)
D(head, int32_t *acc, const int16_t *ring, uint32_t mask, uint32_t p, uint32_t inc, int32_t w, int32_t dw, int n)
D(decim, int16_t *out, const int16_t *b)
D(interp, int16_t *out, const int16_t *c)
D(stage1, int32_t *F, const int16_t *ring, uint32_t mask, uint32_t p, uint32_t inc, int interp, int16_t *lap, uint32_t lmask, uint32_t t, int32_t dly, int32_t sgn, int32_t *lo, int32_t a, int32_t gh, int32_t gd, int near, int n)
int32_t a_wet(int16_t *wet, const int16_t *er, const int32_t *late, int32_t erlev, int n);
int32_t c_wet(int16_t *wet, const int16_t *er, const int32_t *late, int32_t erlev, int n);
static uint32_t rs = 12345;
static int32_t rnd(int32_t lo, int32_t hi) { rs = rs * 1664525u + 1013904223u; return lo + (int32_t)((rs >> 8) % (uint32_t)(hi - lo + 1)); }
static int16_t ra[8194] __attribute__((aligned(4))), rc[8194] __attribute__((aligned(4)));
static int16_t xa[128] __attribute__((aligned(4))), xc[128] __attribute__((aligned(4))), y16[128] __attribute__((aligned(4)));
static int32_t Fa[8][64], Fc[8][64], la[64], lc[64], ra2[64], rc2[64];
static int16_t lapa[1024] __attribute__((aligned(4))), lapc[1024] __attribute__((aligned(4)));
static int fails;
#define CMP(what, a, c, sz) do { if (memcmp(a, c, sz)) { fails++; printf("FAIL %s trial %d\n", what, tr); } } while (0)
int main(void)
{
    for (int tr = 0; tr < 300; tr++) {
        int big = tr % 3 == 0;
        int32_t A = big ? 32767 : 9000;
        for (int i = 0; i < 8194; i++) ra[i] = rc[i] = (int16_t)rnd(-A - (big ? 1 : 0), A);
        for (int i = 0; i < 128; i++) { xa[i] = xc[i] = (int16_t)rnd(-A, A); y16[i] = (int16_t)rnd(-A, A); }
        /* ap */
        { uint32_t m = 1023, t = (uint32_t)rnd(0, 100000) & ~63u; int32_t d = rnd(1, 1000), g = rnd(15000, 25000), g2c = 32768 - ((g * g + 16384) >> 15);
          a_ap(xa, ra, m, t, d, g, g2c, 64); c_ap(xc, rc, m, t, d, g, g2c, 64); CMP("ap", xa, xc, 128); CMP("ap ring", ra, rc, 2048); }
        /* send */
        { uint32_t m = 4095; int32_t sd = rnd(0, 17000), se = rnd(0, 24000), dsd = rnd(-60, 60), dse = rnd(-80, 80);
          a_send(xa, ra, m, rnd(0, 99999), y16, sd, dsd, se, dse, 64); c_send(xc, rc, m, 0, y16, sd, dsd, se, dse, 0);
          uint32_t dp = (uint32_t)rnd(0, 99999); a_send(xa, ra, m, dp, y16, sd, dsd, se, dse, 64); c_send(xc, rc, m, dp, y16, sd, dsd, se, dse, 64); CMP("send", xa, xc, 128); }
        /* pole */
        for (int hp = 0; hp < 2; hp++) { int32_t sa = rnd(-2000000000, 2000000000), sc_ = sa, a = rnd(100, 30000);
          for (int i = 0; i < 64; i++) xa[i] = xc[i] = (int16_t)rnd(-A, A);
          a_pole(xa, &sa, a, hp, 64); c_pole(xc, &sc_, a, hp, 64); CMP(hp ? "hp" : "lp", xa, xc, 128); if (sa != sc_) { fails++; printf("FAIL pole state %d %d\n", sa, sc_); } }
        /* taps */
        { uint32_t pos[8]; int32_t g[8]; for (int k = 0; k < 8; k++) { pos[k] = (uint32_t)rnd(0, 99999); g[k] = rnd(-11000, 11000); }
          a_taps(xa, ra, 8191, pos, g, 64); c_taps(xc, rc, 8191, pos, g, 64); CMP("taps", xa, xc, 128); }
        /* wht, inj, write */
        { for (int i = 0; i < 8; i++) for (int j = 0; j < 64; j++) Fa[i][j] = Fc[i][j] = rnd(-300000, 300000);
          a_wht(Fa, la, ra2, 64); c_wht(Fc, lc, rc2, 64); CMP("wht", Fa, Fc, sizeof Fa); CMP("wht late", la, lc, 256);
          a_inj(la, ra2, xa, y16, 64); c_inj(lc, rc2, xa, y16, 64); CMP("inj", la, lc, 256); CMP("inj M", ra2, rc2, 256);
          for (int k = 0; k < 4; k++) { uint32_t t = (uint32_t)rnd(0, 10000) & ~63u;
            a_write(ra, 8191, t, Fa[k], la, k & 1 ? -1 : 1, k >> 1, 64); c_write(rc, 8191, t, Fc[k], lc, k & 1 ? -1 : 1, k >> 1, 64); CMP("write", ra, rc, sizeof ra); } }
        /* wet, mix */
        { int32_t lev = rnd(0, 26000); int32_t sa = a_wet(xa, y16, Fa[0], lev, 64), s2 = c_wet(xc, y16, Fc[0], lev, 64); CMP("wet", xa, xc, 128); if (sa != s2) { fails++; printf("FAIL wet sum\n"); }
          int32_t g = rnd(0, 32767), w = rnd(0, 32767), dg = rnd(-511, 511) * (g + 64 * 511 < 32767 && g - 64 * 511 > 0), dw = 0;
          a_mix(xa, y16, g, dg, w, dw, 64); c_mix(xc, y16, g, dg, w, dw, 64); CMP("mix", xa, xc, 128); }
        /* mix knee at full scale */
        { int16_t ya[64]; for (int i = 0; i < 64; i++) { xa[i] = xc[i] = (int16_t)rnd(-32768, 32767); ya[i] = (int16_t)rnd(-32768, 32767); }
          a_mix(xa, ya, 32767, 0, 32767, 0, 64); c_mix(xc, ya, 32767, 0, 32767, 0, 64); CMP("mix knee", xa, xc, 128); }
        /* head */
        { int32_t acc_a[64] = {0}, acc_c[64] = {0}; uint32_t p = (uint32_t)rnd(0, 0x7FFFFFFF) * 2u, inc = (uint32_t)rnd(2, 3) << 16, wph = (uint32_t)rnd(0, 0x7FFFFFFF), dw = 1431655u;
          int32_t g = rnd(0, 32767);
          int32_t w0 = rnd(0, 32767), dw0 = rnd(-511, 511); if (w0 + 64 * dw0 > 32767 || w0 + 64 * dw0 < 0) dw0 = 0; (void)wph; (void)dw; (void)g;
          a_head(acc_a, ra, 4095, p, inc, w0, dw0, 64); c_head(acc_c, rc, 4095, p, inc, w0, dw0, 64); CMP("head", acc_a, acc_c, 256); }
        /* decim, interp */
        { static int16_t b[94] __attribute__((aligned(4))); for (int i = 0; i < 94; i++) b[i] = (int16_t)rnd(-A, A);
          a_decim(xa, b); c_decim(xc, b); CMP("decim", xa, xc, 64);
          a_interp(xa, b); c_interp(xc, b); CMP("interp", xa, xc, 128); }
        /* stage 1 */
        for (int k = 0; k < 8; k++) {
            uint32_t mask = 8191, lmask = 1023, t = (uint32_t)rnd(0, 100000) & ~63u;
            int interp = k >> 2, sgn = k & 2 ? -1 : 1, near = k & 1;
            int32_t st = interp ? rnd(-1200, 1200) : 0;
            uint32_t d = interp ? (uint32_t)rnd(500 << 16, 7000 << 16) : (uint32_t)rnd(500, 7000) << 16;
            uint32_t p0 = (t << 16) - 65536u - d, inc = (uint32_t)(65536 - st);
            for (int i = 0; i < 1024; i++) lapa[i] = lapc[i] = (int16_t)rnd(-A, A);
            int32_t loa = rnd(-2000000000, 2000000000), loc = loa;
            int32_t a = rnd(5000, 30000), gh = rnd(0, 11585), gd = rnd(0, 11585 - gh), dly = rnd(1, 1000);
            a_stage1(Fa[0], ra, mask, p0, inc, interp, lapa, lmask, t, dly, sgn, &loa, a, gh, gd, near, 64);
            c_stage1(Fc[0], rc, mask, p0, inc, interp, lapc, lmask, t, dly, sgn, &loc, a, gh, gd, near, 64);
            char nm[32]; sprintf(nm, "stage1 v%d", k);
            CMP(nm, Fa[0], Fc[0], 256); CMP("stage1 lap", lapa, lapc, sizeof lapa);
            if (loa != loc) { fails++; printf("FAIL stage1 v%d state %d %d\n", k, loa, loc); }
        }
    }
    printf("%d failures\n", fails);
    return fails != 0;
}
