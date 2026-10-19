/* CPU estimate for the proposed reverb (notes/fx-reverb.md): the whole per-block signal path in plain fixed-point C,
 * as a first device version would be written before any hand-written assembly. Not the FX: no knob mapping, no
 * glides, fixed lengths. Built for the Blackfin simulator by bench.sh (the op1-bfin image, -mcpu=bf524 -O2, as the
 * device objects), prints CYCLES per 64-frame stereo call for each stage. The simulator has no SDRAM latency.
 *
 * Path per frame: predelay write + 8 ER taps per channel, 2 allpasses per channel, FDN-8 (4 lines read with linear
 * interpolation, two-band filter per line, Hadamard 8 as 3 butterfly stages, writes rounded toward zero), stereo
 * outputs, one pitch-shifter voice (2 interpolated heads, sine-window table), dry / wet mix.
 */
#include <stdint.h>
#include <stdio.h>

#define BLOCK 64
#define NL 8
#define LLEN 8192
#define PDLEN 16384
#define APLEN 2048
#define SHLEN 8192

static int16_t line[NL][LLEN], pd[2][PDLEN], ap[4][APLEN], sh[SHLEN];
static int16_t win[1025];
static uint32_t lw, pw, apw, shw, shph;
static int32_t lpz[NL], modph[4], prev_mono;
static const int32_t len[NL] = {1381, 1621, 1847, 2087, 2377, 2609, 2939, 3229};
static const int32_t ertap[2][8] = {{353, 618, 927, 1279, 1676, 2073, 2558, 3087},
                                    {399, 562, 992, 1215, 1860, 1845, 2686, 2871}};
static const int16_t erg[8] = {8000, 7000, 6200, 5600, 4800, 4200, 3600, 3000};
static const int32_t aplen[4] = {211, 159, 187, 139};

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static inline int16_t sat16(int32_t x)
{
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

static inline int16_t tz(int32_t acc)      /* Q30 -> Q15, rounded toward zero (no limit cycles), saturated */
{
    acc += (acc >> 31) & 0x7FFF;
    return sat16(acc >> 15);
}

static inline int32_t rdi(const int16_t *b, uint32_t mask, uint32_t w, uint32_t d16)   /* d in Q16 samples */
{
    uint32_t n = d16 >> 16, f = (d16 >> 1) & 0x7FFF;
    int32_t a = b[(w - n) & mask], c = b[(w - n - 1) & mask];
    return a + (((c - a) * (int32_t)f) >> 15);
}

static uint32_t t_er, t_ap, t_fdn, t_sh, t_mix;

void reverb_block(int16_t *L, int16_t *R)
{
    int32_t uL[BLOCK], uR[BLOCK], eL[BLOCK], eR[BLOCK], wL[BLOCK], wR[BLOCK];
    static int32_t shf[BLOCK];          /* last block's shimmer (the loop's one-block delay) */
    uint32_t c0 = cycles();
    for (int i = 0; i < BLOCK; i++) {                  /* predelay + ERs */
        pd[0][(pw + i) & (PDLEN - 1)] = L[i];
        pd[1][(pw + i) & (PDLEN - 1)] = R[i];
        uint32_t w = pw + i - 882;                     /* 20 ms predelay */
        int32_t a = 0, b = 0;
        for (int k = 0; k < 8; k++) {
            a += pd[0][(w - ertap[0][k]) & (PDLEN - 1)] * erg[k];
            b += pd[1][(w - ertap[1][k]) & (PDLEN - 1)] * erg[k];
        }
        eL[i] = a >> 15;
        eR[i] = b >> 15;
        uL[i] = (pd[0][w & (PDLEN - 1)] + eL[i]) >> 1;
        uR[i] = (pd[1][w & (PDLEN - 1)] + eR[i]) >> 1;
    }
    pw += BLOCK;
    uint32_t c1 = cycles();
    for (int a = 0; a < 4; a++) {                      /* 2 allpasses per channel, g = 0.7 */
        int32_t *x = a < 2 ? uL : uR;
        for (int i = 0; i < BLOCK; i++) {
            uint32_t w = apw + i;
            int32_t z = ap[a][(w - aplen[a]) & (APLEN - 1)];
            int32_t v = x[i] + ((z * 22938) >> 15);
            ap[a][w & (APLEN - 1)] = sat16(v);
            x[i] = z - ((v * 22938) >> 15);
        }
    }
    apw += BLOCK;
    uint32_t c2 = cycles();
#ifndef STAGED
    for (int i = 0; i < BLOCK; i++) {                  /* FDN-8, per frame (naive) */
        int32_t y[NL];
        uint32_t w = lw + i;
        for (int k = 0; k < NL; k++) {
            if (k < 4) {
                modph[k] += 1200 + 300 * k;
                int32_t tri = (modph[k] >> 15) ^ (modph[k] >> 31);   /* rough triangle, Q16 */
                y[k] = rdi(line[k], LLEN - 1, w, ((uint32_t)len[k] << 16) + (tri & 0x7FFFF));
            } else {
                y[k] = line[k][(w - len[k]) & (LLEN - 1)];
            }
        }
        int32_t f[NL];
        for (int k = 0; k < NL; k++) {                 /* two-band: g_lo LP + g_hi (y - LP) */
            lpz[k] += ((y[k] - lpz[k]) * 12000) >> 15;
            f[k] = (lpz[k] * 30000 + (y[k] - lpz[k]) * 26000) >> 15;
        }
        for (int s = 1; s < NL; s <<= 1)               /* Hadamard: 3 butterfly stages */
            for (int k = 0; k < NL; k++)
                if (!(k & s)) {
                    int32_t a = f[k], b = f[k + s];
                    f[k] = a + b;
                    f[k + s] = a - b;
                }
        int32_t inl = uL[i] + shf[i], inr = uR[i] + shf[i];
        static const int8_t sl[NL] = {1, -1, -1, 1, 1, -1, -1, 1}, sr[NL] = {1, 1, 1, 1, -1, -1, -1, -1};
        for (int k = 0; k < NL; k++)                   /* x 1/sqrt 8 (Q15 11585) and the input */
            line[k][w & (LLEN - 1)] = tz(f[k] * 11585 + (sl[k] * inl + sr[k] * inr) * 11585);
        wL[i] = (y[0] - y[1] + y[2] - y[3] + y[4] - y[5] + y[6] - y[7]) * 11585 >> 15;
        wR[i] = (y[0] + y[1] - y[2] - y[3] + y[4] + y[5] - y[6] - y[7]) * 11585 >> 15;
    }
#else
    /* FDN-8, block-staged: every line is longer than BLOCK, so all 64 outputs of a line are read before any of
     * this block's writes. Stage 1 per line (read, filter), stage 2 per frame (Hadamard, outputs), stage 3 per
     * line (write). Each loop is short and regular: hardware loops, sequential addresses (cache-friendly). */
    static int32_t Y[NL][BLOCK];
    for (int k = 0; k < NL; k++) {
        int32_t *yk = Y[k], z = lpz[k];
        const int16_t *ln = line[k];
        uint32_t w = lw;
        if (k < 4) {
            uint32_t d = ((uint32_t)len[k] << 16) + ((modph[k] >> 13) & 0x7FFFF);
            int32_t dst = 37 * (k + 1);                /* per-sample delay step (the LFO, linear per block) */
            modph[k] += 64 * (1200 + 300 * k);
            for (int i = 0; i < BLOCK; i++, w++, d += dst) {
                int32_t x = rdi(ln, LLEN - 1, w, d);
                z += ((x - z) * 12000) >> 15;
                yk[i] = x;
                yk[i] = (z * 30000 + (x - z) * 26000) >> 15;
            }
        } else {
            uint32_t r = lw - len[k];
            for (int i = 0; i < BLOCK; i++, r++) {
                int32_t x = ln[r & (LLEN - 1)];
                z += ((x - z) * 12000) >> 15;
                yk[i] = (z * 30000 + (x - z) * 26000) >> 15;
            }
        }
        lpz[k] = z;
    }
    for (int i = 0; i < BLOCK; i++) {
        int32_t a0 = Y[0][i], a1 = Y[1][i], a2 = Y[2][i], a3 = Y[3][i];
        int32_t a4 = Y[4][i], a5 = Y[5][i], a6 = Y[6][i], a7 = Y[7][i];
        wL[i] = (a0 - a1 + a2 - a3 + a4 - a5 + a6 - a7) * 11585 >> 15;   /* (filtered: fine for a cost estimate) */
        wR[i] = (a0 + a1 - a2 - a3 + a4 + a5 - a6 - a7) * 11585 >> 15;
        int32_t b0 = a0 + a1, b1 = a0 - a1, b2 = a2 + a3, b3 = a2 - a3;
        int32_t b4 = a4 + a5, b5 = a4 - a5, b6 = a6 + a7, b7 = a6 - a7;
        int32_t c0 = b0 + b2, c1 = b1 + b3, c2 = b0 - b2, c3 = b1 - b3;
        int32_t c4 = b4 + b6, c5 = b5 + b7, c6 = b4 - b6, c7 = b5 - b7;
        int32_t inl = uL[i] + shf[i], inr = uR[i] + shf[i];
        int32_t p = inl + inr, m = inl - inr;
        Y[0][i] = c0 + c4 + p;
        Y[1][i] = c1 + c5 - m;
        Y[2][i] = c2 + c6 - m;
        Y[3][i] = c3 + c7 + p;
        Y[4][i] = c0 - c4 + m;
        Y[5][i] = c1 - c5 - p;
        Y[6][i] = c2 - c6 - p;
        Y[7][i] = c3 - c7 + m;
    }
    for (int k = 0; k < NL; k++) {
        int16_t *ln = line[k];
        const int32_t *yk = Y[k];
        uint32_t w = lw;
        for (int i = 0; i < BLOCK; i++, w++)
            ln[w & (LLEN - 1)] = tz(yk[i] * 11585);
    }
#endif
    lw += BLOCK;
    uint32_t c3 = cycles();
    for (int i = 0; i < BLOCK; i++) {                  /* shimmer: one voice, 2 heads */
        uint32_t w = shw + i;
        sh[w & (SHLEN - 1)] = sat16((wL[i] + wR[i]) >> 1);
        shph += 1431655;                               /* (ratio - 1) / W in Q32, W = 3000 */
        int32_t acc = 0;
        for (int h = 0; h < 2; h++) {
            uint32_t ph = shph + h * 0x80000000u;
            uint32_t d = (64u << 16) + (uint32_t)(((uint64_t)(0xFFFFFFFFu - ph) * 3000) >> 16);
            acc += rdi(sh, SHLEN - 1, w, d) * win[ph >> 22];
        }
        shf[i] = acc >> 15;
    }
    shw += BLOCK;
    uint32_t c4 = cycles();
    for (int i = 0; i < BLOCK; i++) {                  /* mix: dry + wet x k */
        L[i] = sat16(L[i] + (((wL[i] + eL[i]) * 12000) >> 15));
        R[i] = sat16(R[i] + (((wR[i] + eR[i]) * 12000) >> 15));
    }
    uint32_t c5 = cycles();
    t_er += c1 - c0;
    t_ap += c2 - c1;
    t_fdn += c3 - c2;
    t_sh += c4 - c3;
    t_mix += c5 - c4;
    (void)prev_mono;
}

int main(void)
{
    static int16_t L[BLOCK], R[BLOCK];
    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");   /* CCEN: run CYCLES */
    for (int i = 0; i <= 1024; i++)
        win[i] = (int16_t)(32767.0 * __builtin_sin(3.14159265 * i / 1024));
    uint32_t seed = 1, tot = 0, worst = 0;
    const int N = 200;
    for (int b = 0; b < N; b++) {
        for (int i = 0; i < BLOCK; i++) {
            seed = seed * 1664525u + 1013904223u;
            L[i] = (int16_t)(seed >> 18);
            R[i] = (int16_t)(seed >> 19);
        }
        uint32_t c = cycles();
        reverb_block(L, R);
        c = cycles() - c;
        tot += c;
        if (c > worst)
            worst = c;
    }
    printf("total mean %lu max %lu | er %lu ap %lu fdn %lu shimmer %lu mix %lu (cycles per 64-frame stereo call)\n",
           (unsigned long)(tot / N), (unsigned long)worst, (unsigned long)(t_er / N), (unsigned long)(t_ap / N),
           (unsigned long)(t_fdn / N), (unsigned long)(t_sh / N), (unsigned long)(t_mix / N));
    return 0;
}
