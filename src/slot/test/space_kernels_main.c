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
D(write, int16_t *const *ln, uint32_t mask, uint32_t t, int32_t F[8][64], int32_t inj[2][64], int near, int n)
static int16_t wla[8][1024 + 2] __attribute__((aligned(4))), wlc[8][1024 + 2] __attribute__((aligned(4))); static int32_t wij[2][64];
D(mix, int16_t *x, const int16_t *y, int32_t g, int32_t dg, int32_t w, int32_t dw, int n)
D(head, int32_t *acc, const int16_t *ring, uint32_t mask, uint32_t p, uint32_t inc, int32_t w, int32_t dw, int n)
D(bq, int16_t *x, int32_t *st, const int16_t *q, int n)
D(decim, int16_t *out, const int16_t *in, int16_t *bo, int16_t *be, int16_t *bs)
D(interp, int16_t *out, const int16_t *in, int16_t *c, int16_t *cs)
D(pdw, int16_t *pd, uint32_t pmask, uint32_t t, const int16_t *x, int n)
D(add, int16_t *u, const int16_t *a, int n)
D(zero, uint32_t *w, uint32_t nw)
D(ringw, int16_t *ring, uint32_t mask, uint32_t t, const int16_t *in, int n2)
D(aps, int16_t *x, int16_t *const *ring, const uint32_t *mask, uint32_t t, const int32_t *dly, int32_t g, int32_t g2c, int na, int n)
D(shret, int16_t *out, const int32_t *acc, int32_t g, int32_t *prev, int n)
D(pole2, int16_t *x, int32_t *s1, int32_t a1, int32_t *s2, int32_t a2, int hp2, int n)
int32_t a_peak(const int16_t *a, const int16_t *b, int n); int32_t c_peak(const int16_t *a, const int16_t *b, int n);
D(shin, int16_t *sh, const int32_t *l0, const int32_t *l1, int stereo, int n)
struct l1 { int32_t *F; const int16_t *ring; uint32_t p, inc; int interp; int16_t *lap; int32_t dly, sgn, *lo, gh, gd; uint32_t *job; };
static uint32_t k1job[8][20];
D(stage1, const struct l1 *L, uint32_t mask, uint32_t lmask, uint32_t t, int32_t a, int near, int n)
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
        for (int hp = 0; hp < 2; hp++) { int32_t s1a = rnd(-2000000000, 2000000000), s1c = s1a, s2a = rnd(-2000000000, 2000000000), s2c = s2a, a1 = rnd(100, 30000), a2 = rnd(100, 30000);
          for (int i = 0; i < 64; i++) xa[i] = xc[i] = (int16_t)rnd(-A, A);
          a_pole2(xa, &s1a, a1, &s2a, a2, hp, 64); c_pole2(xc, &s1c, a1, &s2c, a2, hp, 64); CMP("pole2", xa, xc, 128);
          if (s1a != s1c || s2a != s2c) { fails++; printf("FAIL pole2 state\n"); } }
        /* taps */
        { uint32_t pos[8]; int32_t g[8]; for (int k = 0; k < 8; k++) { pos[k] = (uint32_t)rnd(0, 99999); g[k] = rnd(-11000, 11000); }
          a_taps(xa, ra, 8191, pos, g, 64); c_taps(xc, rc, 8191, pos, g, 64); CMP("taps", xa, xc, 128); }
        /* wht, inj, write */
        { for (int i = 0; i < 8; i++) for (int j = 0; j < 64; j++) Fa[i][j] = Fc[i][j] = rnd(-300000, 300000);
          a_wht(Fa, la, ra2, 64); c_wht(Fc, lc, rc2, 64); CMP("wht", Fa, Fc, sizeof Fa); CMP("wht late", la, lc, 256);
          a_inj(la, ra2, xa, y16, 64); c_inj(lc, rc2, xa, y16, 64); CMP("inj", la, lc, 256); CMP("inj M", ra2, rc2, 256);
          memcpy(wij[0], la, 256); memcpy(wij[1], ra2, 256);
          for (int k = 0; k < 2; k++) { uint32_t t = (uint32_t)rnd(0, 10000) & ~63u; int16_t *pa[8], *pc[8];
            for (int i = 0; i < 8; i++) { pa[i] = wla[i]; pc[i] = wlc[i]; }
            a_write(pa, 1023, t, Fa, wij, k, 64); c_write(pc, 1023, t, Fc, wij, k, 64); CMP("write", wla, wlc, sizeof wla); } }
        /* wet, mix */
        { int32_t lev = rnd(0, 26000); int32_t sa = a_wet(xa, y16, Fa[0], lev, 64), s2 = c_wet(xc, y16, Fc[0], lev, 64); CMP("wet", xa, xc, 128); if (sa != s2) { fails++; printf("FAIL wet sum\n"); }
          int32_t g = rnd(0, 32767), w = rnd(0, 32767), dg = rnd(-511, 511) * (g + 64 * 511 < 32767 && g - 64 * 511 > 0), dw = 0;
          a_mix(xa, y16, g, dg, w, dw, 64); c_mix(xc, y16, g, dg, w, dw, 64); CMP("mix", xa, xc, 128); }
        /* mix knee at full scale */
        { int16_t ya[64]; for (int i = 0; i < 64; i++) { xa[i] = xc[i] = (int16_t)rnd(-32768, 32767); ya[i] = (int16_t)rnd(-32768, 32767); }
          a_mix(xa, ya, 32767, 0, 32767, 0, 64); c_mix(xc, ya, 32767, 0, 32767, 0, 64); CMP("mix knee", xa, xc, 128); }
        /* biquad */
        { static const int16_t q[5] = { 2351, 4702, 2351, -11153, 4173 }; int32_t sa[4], sc_[4];
          for (int i = 0; i < 4; i++) sa[i] = sc_[i] = rnd(-A, A);
          for (int i = 0; i < 64; i++) xa[i] = xc[i] = (int16_t)rnd(-A, A);
          a_bq(xa, sa, q, 64); c_bq(xc, sc_, q, 64); CMP("bq", xa, xc, 128); CMP("bq state", sa, sc_, 16); }
        /* head */
        { int32_t acc_a[64] = {0}, acc_c[64] = {0}; uint32_t p = (uint32_t)rnd(0, 0x7FFFFFFF) * 2u, inc = (uint32_t)rnd(2, 3) << 16, wph = (uint32_t)rnd(0, 0x7FFFFFFF), dw = 1431655u;
          int32_t g = rnd(0, 32767);
          int32_t w0 = rnd(0, 32767), dw0 = rnd(-511, 511); if (w0 + 64 * dw0 > 32767 || w0 + 64 * dw0 < 0) dw0 = 0; (void)wph; (void)dw; (void)g;
          a_head(acc_a, ra, 4095, p, inc, w0, dw0, 64); c_head(acc_c, rc, 4095, p, inc, w0, dw0, 64); CMP("head", acc_a, acc_c, 256); }
        /* an allpass chain */
        { static int16_t r4a[4][1024] __attribute__((aligned(4))), r4c[4][1024] __attribute__((aligned(4)));
          int16_t *pa[4], *pc[4]; uint32_t mk[4]; int32_t dl[4]; int na = rnd(1, 4);
          for (int a = 0; a < 4; a++) { pa[a] = r4a[a]; pc[a] = r4c[a]; mk[a] = (256u << (a & 1)) - 1; dl[a] = rnd(1, (int32_t)mk[a]); }
          for (int i = 0; i < 64; i++) xa[i] = xc[i] = (int16_t)rnd(-A, A);
          uint32_t t = (uint32_t)rnd(0, 100000) & ~63u; int32_t g = rnd(1000, 26000), g2c = 32768 - ((g * g + 16384) >> 15);
          memcpy(r4c, r4a, sizeof r4a);
          a_aps(xa, pa, mk, t, dl, g, g2c, na, 64); c_aps(xc, pc, mk, t, dl, g, g2c, na, 64);
          CMP("aps", xa, xc, 128); CMP("aps rings", r4a, r4c, sizeof r4a); }
        /* the shimmer's return, into the limit */
        { int32_t acc[32], pa = rnd(-20000, 20000), pc = pa; int16_t oa[64], oc[64]; int32_t g = rnd(0, 32768), big = tr & 1 ? 45000 : 12000;
          for (int i = 0; i < 32; i++) acc[i] = rnd(-big, big);
          if (tr % 5 == 0) { acc[3] = 45000; acc[7] = -45000; g = 32768; }
          a_shret(oa, acc, g, &pa, 16 << (tr & 1)); c_shret(oc, acc, g, &pc, 16 << (tr & 1)); CMP("shret", oa, oc, 4 * (16 << (tr & 1)));
          if (pa != pc) { fails++; printf("FAIL shret prev\n"); } }
        /* the block moves */
        { static int16_t pa[1024] __attribute__((aligned(4))), pc[1024] __attribute__((aligned(4)));
          int16_t ua[64] __attribute__((aligned(4))), uc[64] __attribute__((aligned(4))), ad[64] __attribute__((aligned(4)));
          static int32_t l0[64], l1[64];
          for (int i = 0; i < 64; i++) { ua[i] = uc[i] = (int16_t)rnd(-32768, 32767); ad[i] = (int16_t)rnd(-32768, 32767); l0[i] = rnd(-3000000, 3000000); l1[i] = rnd(-3000000, 3000000); }
          uint32_t t = (uint32_t)rnd(0, 100000) & ~63u;     /* (n divides t) */
          a_pdw(pa, 1023, t, ua, 32 << (tr & 1)); c_pdw(pc, 1023, t, ua, 32 << (tr & 1)); CMP("pdw", pa, pc, sizeof pa);
          { uint32_t t2 = (uint32_t)rnd(0, 100000); a_ringw(pa, 255, t2, ua, 32); c_ringw(pc, 255, t2, ua, 32); CMP("ringw", pa, pc, sizeof pa); }
          a_add(ua, ad, 64); c_add(uc, ad, 64); CMP("add", ua, uc, 128);
          a_shin(ua, l0, l1, tr & 1, 64); c_shin(uc, l0, l1, tr & 1, 64); CMP("shin", ua, uc, 128);
          if (tr % 7 == 0) { ua[rnd(0, 63)] = -32768; ad[rnd(0, 63)] = (tr & 8) ? -32768 : 32767; }
          int32_t qa = a_peak(ua, ad, 64), qc = c_peak(ua, ad, 64); if (qa != qc) { fails++; printf("FAIL peak %d %d\n", qa, qc); }
          uint32_t za[40], zc[40]; for (int i = 0; i < 40; i++) za[i] = zc[i] = (uint32_t)rnd(0, 99999); uint32_t nz = (uint32_t)rnd(0, 39);
          a_zero(za, nz); c_zero(zc, nz); CMP("zero", za, zc, sizeof za); }
        /* decim, interp */
        { /* (the state carries over from trial to trial: one stream) */
          static int16_t boa[48] __attribute__((aligned(4))), bea[48] __attribute__((aligned(4))), bsa[48] __attribute__((aligned(4)));
          static int16_t boc[48] __attribute__((aligned(4))), bec[48] __attribute__((aligned(4))), bsc[48] __attribute__((aligned(4)));
          static int16_t ca[48] __attribute__((aligned(4))), csa[48] __attribute__((aligned(4))), cc_[48] __attribute__((aligned(4))), csc[48] __attribute__((aligned(4)));
          int16_t in[64] __attribute__((aligned(4))), oa[64] __attribute__((aligned(4))), oc[64] __attribute__((aligned(4)));
          for (int i = 0; i < 64; i++) in[i] = (int16_t)rnd(-A, A);
          a_decim(oa, in, boa, bea, bsa); c_decim(oc, in, boc, bec, bsc); CMP("decim", oa, oc, 64);
          a_interp(oa, in, ca, csa); c_interp(oc, in, cc_, csc); CMP("interp", oa, oc, 128); }
        /* stage 1: all 8 lines, a mix of variants; glides big enough to split segments */
        for (int near = 0; near < 2; near++) {
            static int16_t ringa[8][4098] __attribute__((aligned(4))), ringc[8][4098] __attribute__((aligned(4)));
            static int16_t lapa8[8][512] __attribute__((aligned(4))), lapc8[8][512] __attribute__((aligned(4)));
            static int32_t Fa8[8][64], Fc8[8][64];
            int32_t loa[8], loc[8];
            struct l1 La[8], Lc[8];
            uint32_t mask = 4095, lmask = 511, t = (uint32_t)rnd(0, 100000) & ~63u;
            int32_t a = rnd(5000, 30000);
            for (int i = 0; i < 8; i++) {
                for (int k = 0; k < 4098; k++) ringa[i][k] = ringc[i][k] = (int16_t)rnd(-A, A);
                for (int k = 0; k < 512; k++) lapa8[i][k] = lapc8[i][k] = (int16_t)rnd(-A, A);
                loa[i] = loc[i] = rnd(-2000000000, 2000000000);
                int interp = rnd(0, 2) > 0;
                int32_t st = interp ? (rnd(0, 3) == 0 ? rnd(-1100, 1100) : rnd(-40, 40)) : 0;
                uint32_t d = interp ? (uint32_t)rnd(300 << 16, 3500 << 16) : (uint32_t)rnd(300, 3500) << 16;
                struct l1 l = { 0, 0, (t << 16) - 65536u - d, (uint32_t)(65536 - st), interp, 0, rnd(1, 400),
                                (i & 1) ? -1 : 1, 0, rnd(0, 11585), 0 };
                l.gd = rnd(0, 11585 - l.gh);
                La[i] = Lc[i] = l;
                La[i].F = Fa8[i]; Lc[i].F = Fc8[i];
                La[i].ring = ringa[i]; Lc[i].ring = ringc[i];
                La[i].lap = lapa8[i]; Lc[i].lap = lapc8[i];
                La[i].lo = &loa[i]; Lc[i].lo = &loc[i];
                La[i].job = k1job[i];
            }
            for (int blk = 0; blk < 2; blk++) {     /* two blocks: the asm's own state carries over */
                int nn = 32 << (tr & 1);
                a_stage1(La, mask, lmask, t, a, near, nn);
                c_stage1(Lc, mask, lmask, t, a, near, nn);
                t += (uint32_t)nn;
                for (int i = 0; i < 8; i++) { La[i].p += (uint32_t)nn * La[i].inc; Lc[i].p = La[i].p; }
            }
            CMP(near ? "stage1 near" : "stage1 tz", Fa8, Fc8, sizeof Fa8);
            CMP("stage1 lap", lapa8, lapc8, sizeof lapa8);
            CMP("stage1 state", loa, loc, sizeof loa);
        }
    }
    printf("%d failures\n", fails);
    return fails != 0;
}
