/* space_check.py: the space FX kernels, built twice (asm, and -DKT_C the C reference) with prefixes a_ / c_ for space_kernels_main.c. */
#include <stdint.h>
#ifdef KT_C
#define SPACE_C
#define space_ui space_ui_c
#define space_pub space_pub_c
#define space_mono space_mono_c
#define space_stereo space_stereo_c
#define W(n) c_##n
#else
#define W(n) a_##n
#endif
#include "space.c"
void W(ap)(int16_t *x, int16_t *ring, uint32_t mask, uint32_t t, int32_t dly, int32_t g, int32_t g2c, int n) { int16_t *r[1] = { ring }; k_aps(x, r, &mask, t, &dly, g, g2c, 1, n); }
void W(send)(int16_t *u, const int16_t *pd, uint32_t pmask, uint32_t dpos, const int16_t *er, int32_t sd, int32_t dsd, int32_t se, int32_t dse, int n) { k_send(u, pd, pmask, dpos, er, sd, dsd, se, dse, n); }
void W(pole)(int16_t *x, int32_t *s, int32_t a, int hp, int n) { k_pole(x, s, a, hp, n); }
void W(taps)(int16_t *er, const int16_t *ring, uint32_t mask, const uint32_t *pos, const int32_t *g, int n) { k_taps(er, ring, mask, pos, g, n); }
void W(wht)(int32_t F[8][64], int32_t *l, int32_t *r, int n) { k_wht(F, l, r, n); }
void W(inj)(int32_t *P, int32_t *M, const int16_t *ul, const int16_t *ur, int n) { k_inj(P, M, ul, ur, n); }
void W(write)(int16_t *const *ln, uint32_t mask, uint32_t t, int32_t F[8][64], int32_t inj[2][64], int near, int n) { k_write_all(ln, mask, t, F, inj, near, n); }
int32_t W(wet)(int16_t *wet, const int16_t *er, const int32_t *late, int32_t erlev, int n) { return k_wet(wet, er, late, erlev, n); }
void W(mix)(int16_t *x, const int16_t *y, int32_t g, int32_t dg, int32_t w, int32_t dw, int n) { k_mix(x, y, g, dg, w, dw, n); }
void W(head)(int32_t *acc, const int16_t *ring, uint32_t mask, uint32_t p, uint32_t inc, int32_t w, int32_t dw, int n) { struct hseg g[2] = { { acc, p, inc, w, dw, n / 2 }, { acc + n / 2, p + (uint32_t)(n / 2) * inc, inc, w + (n / 2) * dw, dw, n - n / 2 } }; k_heads(g, 2, ring, mask); }
void W(bq)(int16_t *x, int32_t *st, const int16_t *q, int n) { k_bq(x, st, q, n); }
void W(decim)(int16_t *out, const int16_t *in, int16_t *bo, int16_t *be, int16_t *bs) { k_decimate(out, in, bo, be, bs); }
void W(interp)(int16_t *out, const int16_t *in, int16_t *c, int16_t *cs) { k_interpolate(out, in, c, cs); }
void W(stage1)(const struct l1 *L, uint32_t mask, uint32_t lmask, uint32_t t, int32_t a, int near, int n) { k_stage1_all(L, mask, lmask, t, a, near, n); }
void W(pdw)(int16_t *pd, uint32_t pmask, uint32_t t, const int16_t *x, int n) { k_pdw(pd, pmask, t, x, n); }
void W(add)(int16_t *u, const int16_t *a, int n) { k_add(u, a, n); }
void W(shin)(int16_t *sh, const int32_t *l0, const int32_t *l1, int stereo, int n) { k_shin(sh, l0, l1, stereo, n); }
int32_t W(peak)(const int16_t *a, const int16_t *b, int n) { return k_peak(a, b, n); }
void W(zero)(uint32_t *w, uint32_t nw) { k_zero(w, nw); }
void W(pole2)(int16_t *x, int32_t *s1, int32_t a1, int32_t *s2, int32_t a2, int hp2, int n) { k_pole2(x, s1, a1, s2, a2, hp2, n); }
void W(shret)(int16_t *out, const int32_t *acc, int32_t g, int32_t *prev, int n) { k_shret(out, acc, g, prev, n); }
void W(aps)(int16_t *x, int16_t *const *ring, const uint32_t *mask, uint32_t t, const int32_t *dly, int32_t g, int32_t g2c, int na, int n) { k_aps(x, ring, mask, t, dly, g, g2c, na, n); }
void W(ringw)(int16_t *ring, uint32_t mask, uint32_t t, const int16_t *in, int n2) { k_ringw(ring, mask, t, in, n2); }
