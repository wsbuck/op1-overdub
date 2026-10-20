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
void W(ap)(int16_t *x, int16_t *ring, uint32_t mask, uint32_t t, int32_t dly, int32_t g, int32_t g2c, int n) { k_ap(x, ring, mask, t, dly, g, g2c, n); }
void W(send)(int16_t *u, const int16_t *pd, uint32_t pmask, uint32_t dpos, const int16_t *er, int32_t sd, int32_t dsd, int32_t se, int32_t dse, int n) { k_send(u, pd, pmask, dpos, er, sd, dsd, se, dse, n); }
void W(pole)(int16_t *x, int32_t *s, int32_t a, int hp, int n) { k_pole(x, s, a, hp, n); }
void W(taps)(int16_t *er, const int16_t *ring, uint32_t mask, const uint32_t *pos, const int32_t *g, int n) { k_taps(er, ring, mask, pos, g, n); }
void W(wht)(int32_t F[8][64], int32_t *l, int32_t *r, int n) { k_wht(F, l, r, n); }
void W(inj)(int32_t *P, int32_t *M, const int16_t *ul, const int16_t *ur, int n) { k_inj(P, M, ul, ur, n); }
void W(write)(int16_t *ln, uint32_t mask, uint32_t t, const int32_t *row, const int32_t *inj, int32_t sgn, int near, int n) { k_write(ln, mask, t, row, inj, sgn, near, n); }
int32_t W(wet)(int16_t *wet, const int16_t *er, const int32_t *late, int32_t erlev, int n) { return k_wet(wet, er, late, erlev, n); }
void W(mix)(int16_t *x, const int16_t *y, int32_t g, int32_t dg, int32_t w, int32_t dw, int n) { k_mix(x, y, g, dg, w, dw, n); }
void W(head)(int32_t *acc, const int16_t *ring, uint32_t mask, uint32_t p, uint32_t inc, int32_t w, int32_t dw, int n) { k_head(acc, ring, mask, p, inc, w, dw, n); }
void W(decim)(int16_t *out, const int16_t *b) { k_decim(out, b, b + 47); }
void W(interp)(int16_t *out, const int16_t *c) { k_interp(out, c); }
void W(stage1)(int32_t *F, const int16_t *ring, uint32_t mask, uint32_t p, uint32_t inc, int interp, int16_t *lap, uint32_t lmask, uint32_t t, int32_t dly, int32_t sgn, int32_t *lo, int32_t a, int32_t gh, int32_t gd, int near, int n) { k_stage1(F, ring, mask, p, inc, interp, lap, lmask, t, dly, sgn, lo, a, gh, gd, near, n); }
