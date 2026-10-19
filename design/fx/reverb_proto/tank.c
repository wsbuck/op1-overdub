/* Float prototype of the proposed reverb FX (notes/fx-reverb.md, after the design review):
 *   predelay + 8 early-reflection taps per side (the ER sum through one short allpass), 4 input allpasses per
 *   channel, an 8-line FDN (normalised Hadamard feedback; per line one short allpass, then a two-band absorbent
 *   filter; 4 modulated lines), 2nd-order low cut and level normalisation at the tank input, a delay-line pitch
 *   shifter (2 heads, jittered windows, biquad pre-filter) in the feedback path, FREEZE / INFINITE / frozen
 *   shimmer, and the envelope tail effects (level-keyed GATE, DUCK, SWELL on the wet output).
 * Not device code: it is there to hear the direction and to check RT60, diffusion, shimmer stability and the
 * 16-bit line-storage behaviour.
 *
 * Usage: tank PARAMS IN.f32 OUT.f32   (interleaved stereo float32, 44.1 kHz; PARAMS = "name [index] value" lines)
 * Built and driven by proto.py. Every FDN line is >= 64 samples, as the device's block-staged loop needs.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 44100.0
#define NL 8
#define NER 8
#define NAP 8                                   /* input allpasses: 0..3 L, 4..7 R */
#define MAXLINE 16384
#define PDLEN 32768
#define APLEN 4096
#define SHLEN 16384

static double P_predelay, P_er_t[NER], P_er_tr[NER], P_er_g[NER], P_er_level, P_er_send, P_direct_send;
static double P_ap_len[NAP], P_ap_g, P_er_ap_len[4], P_er_ap_g = 0.6, P_len[NL], P_lap_len[NL], P_lap_g = 0.5;
static double P_mod_depth, P_mod_rate, P_t60_lo, P_t60_hi, P_xover, P_in_hp, P_in_lp, P_in_gain = 1;
static double P_sh_ratio[2], P_sh_gain[2], P_sh_win, P_sh_jitter = 0.2, P_sh_hp, P_sh_lp, P_sh_limit;
static double P_freeze_on = 1e9, P_freeze_off = 1e9, P_dry = 1, P_wet = 0.3, P_late_level = 1;
static double P_infinite = 0;                   /* INFINITE: lossless-ish loop, input open, level-limited */
static double P_frz_shimmer = 0;                /* frozen shimmer: in freeze, loop x sqrt(1 - a^2), shimmer x a */
static double P_gate = 0, P_gate_hold = 0.3;    /* GATE: open while the input is above THR, then hold, close 30 ms */
static double P_duck = 0;                       /* DUCK: wet x (1 - depth * min(1, env / THR)) */
static double P_swell = 0;                      /* SWELL: the wet fades in over this many s after an onset */
static int P_quant = 0;                         /* 0 float, 1 round, 2 floor, 3 toward zero, 4 round + TPDF dither */
static double P_sh_norm = 1, P_sh_kmul = 1;

static void set(const char *k, double v, int i)
{
#define S(name, var) if (!strcmp(k, name)) { var = v; return; }
#define A(name, arr) if (!strcmp(k, name)) { arr[i] = v; return; }
    S("predelay", P_predelay) S("er_level", P_er_level) S("er_send", P_er_send) S("direct_send", P_direct_send)
    S("ap_g", P_ap_g) S("er_ap_g", P_er_ap_g) S("lap_g", P_lap_g) S("mod_depth", P_mod_depth)
    S("mod_rate", P_mod_rate) S("t60_lo", P_t60_lo) S("t60_hi", P_t60_hi) S("xover", P_xover) S("in_hp", P_in_hp)
    S("in_lp", P_in_lp) S("in_gain", P_in_gain) S("sh_win", P_sh_win) S("sh_jitter", P_sh_jitter)
    S("sh_hp", P_sh_hp) S("sh_lp", P_sh_lp) S("sh_limit", P_sh_limit) S("freeze_on", P_freeze_on)
    S("freeze_off", P_freeze_off) S("dry", P_dry) S("wet", P_wet) S("late_level", P_late_level)
    S("infinite", P_infinite) S("frz_shimmer", P_frz_shimmer) S("gate", P_gate) S("gate_hold", P_gate_hold)
    S("duck", P_duck) S("swell", P_swell) S("sh_norm", P_sh_norm) S("sh_kmul", P_sh_kmul)
    if (!strcmp(k, "quant")) { P_quant = (int)v; return; }
    A("er_t", P_er_t) A("er_tr", P_er_tr) A("er_g", P_er_g) A("ap_len", P_ap_len) A("er_ap_len", P_er_ap_len)
    A("len", P_len) A("lap_len", P_lap_len) A("sh_ratio", P_sh_ratio) A("sh_gain", P_sh_gain)
    fprintf(stderr, "unknown param %s\n", k);
    exit(2);
}

static uint32_t rng = 12345;
static double urand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }

/* 16-bit storage, as on the device (lines in SDRAM as int16). Freeze switches toward-zero to round-to-nearest. */
static int qmode;
static double q16(double x)
{
    if (qmode == 0) return x;
    double s = x * 32768.0, r;
    switch (qmode) {
    case 1: r = floor(s + 0.5); break;
    case 2: r = floor(s); break;
    case 3: r = s < 0 ? ceil(s) : floor(s); break;
    default: r = floor(s + urand() - urand() + 0.5); break;
    }
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    return r / 32768.0;
}

static double line[NL][MAXLINE], lap[NL][APLEN], pd[2][PDLEN], ap[NAP][APLEN], erap[4][APLEN], sh[SHLEN];
static int lw, pw, apw, shw;

static double rd(const double *b, int len, int w, double d)    /* linear interpolation, d samples back */
{
    int n = (int)d;
    double f = d - n;
    double a = b[(w - n + len) % len], c = b[(w - n - 1 + len) % len];
    return a + (c - a) * f;
}

/* Schroeder allpass on ring b (length APLEN) with delay D: w = x + g z, y = z - g w. */
static double allpass(double *b, int w, int D, double g, double x)
{
    double z = b[(w - D + APLEN) % APLEN];
    double v = x + g * z;
    b[w % APLEN] = q16(v);
    return z - g * v;
}

struct op { double a, z; };                     /* one-pole low-pass */
static double lp1(struct op *o, double x) { o->z += o->a * (x - o->z); return o->z; }
static double coef(double hz) { return 1 - exp(-2 * M_PI * hz / FS); }

struct bq { double b0, b1, b2, a1, a2, x1, x2, y1, y2; };     /* RBJ biquad */
static void bq_lp(struct bq *q, double hz, double Q)
{
    double w = 2 * M_PI * hz / FS, al = sin(w) / (2 * Q), c = cos(w), a0 = 1 + al;
    q->b0 = (1 - c) / 2 / a0; q->b1 = (1 - c) / a0; q->b2 = q->b0; q->a1 = -2 * c / a0; q->a2 = (1 - al) / a0;
}
static void bq_hp(struct bq *q, double hz, double Q)
{
    double w = 2 * M_PI * hz / FS, al = sin(w) / (2 * Q), c = cos(w), a0 = 1 + al;
    q->b0 = (1 + c) / 2 / a0; q->b1 = -(1 + c) / a0; q->b2 = q->b0; q->a1 = -2 * c / a0; q->a2 = (1 - al) / a0;
}
static double bq(struct bq *q, double x)
{
    double y = q->b0 * x + q->b1 * q->x1 + q->b2 * q->x2 - q->a1 * q->y1 - q->a2 * q->y2;
    q->x2 = q->x1; q->x1 = x; q->y2 = q->y1; q->y1 = y;
    return y;
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: tank PARAMS IN OUT\n"); return 2; }
    FILE *pf = fopen(argv[1], "r");
    char k[64], lineb[256];
    while (fgets(lineb, sizeof lineb, pf)) {
        double a, b;
        int c = sscanf(lineb, "%63s %lf %lf", k, &a, &b);
        if (c == 3) set(k, b, (int)a);
        else if (c == 2) set(k, a, 0);
    }
    fclose(pf);
    FILE *fi = fopen(argv[2], "rb"), *fo = fopen(argv[3], "wb");
    fseek(fi, 0, SEEK_END);
    long n = ftell(fi) / 8;
    fseek(fi, 0, SEEK_SET);
    float *io = malloc(n * 8);
    if (fread(io, 8, n, fi) != (size_t)n) return 3;

    const int H[NL][NL] = {
        {1, 1, 1, 1, 1, 1, 1, 1}, {1, -1, 1, -1, 1, -1, 1, -1}, {1, 1, -1, -1, 1, 1, -1, -1},
        {1, -1, -1, 1, 1, -1, -1, 1}, {1, 1, 1, 1, -1, -1, -1, -1}, {1, -1, 1, -1, -1, 1, -1, 1},
        {1, 1, -1, -1, -1, -1, 1, 1}, {1, -1, -1, 1, -1, 1, 1, -1}};
    const double rn = 1 / sqrt(8.0);
    double glo[NL], ghi[NL], g_mean;
    struct op damp[NL], bwL = {coef(P_in_lp), 0}, bwR = {coef(P_in_lp), 0}, shh = {coef(P_sh_hp), 0};
    struct bq hpL = {0}, hpR = {0}, shl = {0};
    bq_hp(&hpL, P_in_hp, 0.7071);               /* 2nd-order low cut: no sub drone from kicks */
    bq_hp(&hpR, P_in_hp, 0.7071);
    bq_lp(&shl, P_sh_lp, 0.7071);               /* shimmer anti-alias (fs / 2r) */
    double xa = coef(P_xover), meanlen = 0;
    for (int i = 0; i < NL; i++) {
        double m = P_len[i] + P_lap_len[i];     /* the in-line allpass adds to the loop's delay */
        glo[i] = pow(10, -3 * m / (P_t60_lo * FS));
        ghi[i] = pow(10, -3 * m / (P_t60_hi * FS));
        damp[i].a = xa;
        damp[i].z = 0;
        meanlen += m / NL;
    }
    g_mean = pow(10, -3 * meanlen / (P_t60_lo * FS));
    double sh_k = P_sh_norm ? fmin(1, P_sh_kmul * sqrt(1 - g_mean * g_mean)) : 1;
    const double GFRZ = 1 - pow(2, -13);        /* freeze loop gain per pass: ~-1.3 dB per minute, bounded drift */
    double mod_ph[NL], mod_inc[NL];
    for (int i = 0; i < NL; i++) {
        mod_ph[i] = i * 0.37;
        mod_inc[i] = P_mod_rate * (1 + 0.23 * i) / FS;
    }
    double sh_ph[2] = {0, 0}, sh_w[2][2], prev_mono = 0, frz = 0, gate_g = P_gate > 0 ? 0 : 1, hold = 0;
    for (int s = 0; s < 2; s++) sh_w[s][0] = sh_w[s][1] = P_sh_win;
    double ef = 0, es = 0, duck_g = 1, swell_g = 1, armed = 1, tank_lv = 0, frz_target = 0;
    const double THR = 0.06;                    /* gate / duck / onset threshold, about -24 dBFS */
    const double GCLOSE = pow(10, -3 / (0.03 * FS));   /* gate release: 60 dB in 30 ms */
    double fbk_peak = 0;

    for (long t = 0; t < n; t++) {
        double ts = t / FS;
        double inL = io[2 * t], inR = io[2 * t + 1];
        int fz = ts >= P_freeze_on && ts < P_freeze_off;
        frz += ((fz ? 1.0 : 0.0) - frz) * (1 / (0.05 * FS));       /* 50 ms freeze crossfade */
        qmode = (P_quant == 3 && frz > 0.5) ? 1 : P_quant;          /* toward zero, but round while frozen */
        /* envelope: fast (1 ms / 60 ms) and slow (40 ms / 400 ms) peak followers */
        double ax = fmax(fabs(inL), fabs(inR));
        ef += (ax - ef) * (ax > ef ? 0.0226 : 0.00038);
        es += (ax - es) * (ax > es ? 0.00057 : 0.000057);
        int onset = 0;
        if (armed && ef > 1.5 * es + 0.25 * THR && ef > 0.5 * THR) { onset = 1; armed = 0; }
        if (ef < 1.1 * es) armed = 1;
        if (P_gate > 0) {                                           /* level-keyed: open while loud, then hold */
            if (ef > THR) hold = P_gate_hold * FS;
            if (hold > 0) { hold--; gate_g += (1 - gate_g) * 0.05; }
            else gate_g *= GCLOSE;
        }
        if (P_duck > 0) {
            double tg = 1 - P_duck * fmin(1, ef / THR);   /* P_duck = 1 - floor gain */
            duck_g += (tg - duck_g) * (tg < duck_g ? 0.01 : 0.00015);  /* duck fast, recover ~150 ms */
        }
        if (P_swell > 0) {                                          /* on the wet output: the tank stays fed */
            if (onset) swell_g = 0;
            swell_g += (1 - swell_g) * (1 / (P_swell * FS));
        }
        pd[0][pw] = inL;
        pd[1][pw] = inR;
        double dL = rd(pd[0], PDLEN, pw, P_predelay), dR = rd(pd[1], PDLEN, pw, P_predelay);
        double erL = 0, erR = 0;
        for (int e = 0; e < NER; e++) {
            erL += P_er_g[e] * rd(pd[0], PDLEN, pw, P_predelay + P_er_t[e]);
            erR += P_er_g[e] * rd(pd[1], PDLEN, pw, P_predelay + P_er_tr[e]);
        }
        pw = (pw + 1) % PDLEN;
        for (int e = 0; e < 2; e++) {                               /* smear the taps: 2 allpasses per side */
            erL = allpass(erap[e], apw, (int)P_er_ap_len[e], P_er_ap_g, erL);
            erR = allpass(erap[2 + e], apw, (int)P_er_ap_len[2 + e], P_er_ap_g, erR);
        }
        double uL = P_direct_send * dL + P_er_send * erL, uR = P_direct_send * dR + P_er_send * erR;
        uL = lp1(&bwL, bq(&hpL, uL)) * P_in_gain;
        uR = lp1(&bwR, bq(&hpR, uR)) * P_in_gain;
        for (int a = 0; a < NAP; a++) {
            double *x = a < NAP / 2 ? &uL : &uR;
            *x = allpass(ap[a], apw, (int)P_ap_len[a], P_ap_g, *x);
        }
        /* shimmer: pitch shifters on last sample's late output, fed back into the tank input */
        double shin = bq(&shl, prev_mono - lp1(&shh, prev_mono));
        sh[shw] = q16(shin);
        double sho = 0;
        for (int s = 0; s < 2; s++) {
            double r = P_sh_ratio[s];
            if (P_sh_gain[s] == 0 || r == 1) continue;
            sh_ph[s] += fabs(r - 1) / P_sh_win;
            if (sh_ph[s] >= 1) sh_ph[s] -= 1;
            for (int h = 0; h < 2; h++) {
                double ph = sh_ph[s] + 0.5 * h;
                ph -= floor(ph);
                if (ph < fabs(r - 1) / P_sh_win)               /* this head just reset: new jittered window */
                    sh_w[s][h] = P_sh_win * (1 + P_sh_jitter * (2 * urand() - 1));
                double W = sh_w[s][h], d0 = 64;
                double d = r > 1 ? d0 + W * (1 - ph) : d0 + W * ph;
                sho += P_sh_gain[s] * sin(M_PI * ph) * rd(sh, SHLEN, shw, d);   /* sine windows: constant power */
            }
        }
        shw = (shw + 1) % SHLEN;
        /* frozen shimmer: the frozen loop stays lossless and the shifter adds octaves on top; a servo trims the
         * return so the tank never rises more than ~2 dB above the level it was frozen at (a pure up-shift in a
         * lossless loop would drain everything out through the top band instead) */
        if (frz < 0.01) frz_target = 1.25 * tank_lv;
        double servo = frz_target / (tank_lv + 1e-9);
        servo = servo > 1 ? 1 : servo * servo * servo * servo;
        double a_fs = P_frz_shimmer * frz;
        double shf = sho * sh_k * (1 - frz) + sho * a_fs * servo;
        if (P_sh_limit > 0) shf = P_sh_limit * tanh(shf / P_sh_limit);
        double ing = 1 - frz;
        if (P_infinite > 0) ing *= 1 / (1 + tank_lv / 0.15);       /* INFINITE: input open, tank level held */
        uL = uL * ing + shf;
        uR = uR * ing + shf;

        double y[NL], f[NL];
        double gloop = 1;
        for (int i = 0; i < NL; i++) {
            double d = P_len[i];
            if (i == 0 || i == 3 || i == 5 || i == 6) {
                mod_ph[i] += mod_inc[i];
                mod_ph[i] -= floor(mod_ph[i]);
                double tri = mod_ph[i] < 0.5 ? 4 * mod_ph[i] - 1 : 3 - 4 * mod_ph[i];
                d += P_mod_depth * (1 - 0.5 * frz) * tri;   /* depth halves in freeze */
            }
            y[i] = rd(line[i], MAXLINE, lw, d);
            if (P_lap_len[i] > 0)                           /* in-line allpass: density, breaks up modes */
                y[i] = allpass(lap[i], apw, (int)P_lap_len[i], (i & 1) ? -P_lap_g : P_lap_g, y[i]);
            double lo = lp1(&damp[i], y[i]);
            double gf = P_infinite > 0 ? GFRZ : 1;
            double gl = glo[i] + (GFRZ - glo[i]) * frz, gh = ghi[i] + (GFRZ - ghi[i]) * frz;
            if (P_infinite > 0) gl = gh = gf;
            f[i] = gloop * (gl * lo + gh * (y[i] - lo));
        }
        double lateL = 0, lateR = 0, e2 = 0;
        for (int i = 0; i < NL; i++) {
            double s = 0;
            for (int j = 0; j < NL; j++) s += H[i][j] * f[j];
            s = s * rn + rn * (H[3][i] * uL + H[4][i] * uR);
            if (fabs(s) > fbk_peak) fbk_peak = fabs(s);
            line[i][lw] = q16(s);
            e2 += s * s;
            lateL += H[1][i] * y[i];
            lateR += H[2][i] * y[i];
        }
        tank_lv += (sqrt(e2 / NL) - tank_lv) * 0.0005;
        lw = (lw + 1) % MAXLINE;
        apw++;
        lateL *= rn * P_late_level;                 /* level normalisation is at the tank input (in_gain) */
        lateR *= rn * P_late_level;
        prev_mono = 0.5 * (lateL + lateR) / P_late_level;   /* the shimmer taps the tank before the make-up */
        double wL = P_er_level * erL + lateL, wR = P_er_level * erR + lateR;
        double eg = gate_g * duck_g * swell_g * swell_g;
        io[2 * t] = (float)(P_dry * inL + P_wet * wL * eg);
        io[2 * t + 1] = (float)(P_dry * inR + P_wet * wR * eg);
    }
    fwrite(io, 8, n, fo);
    fclose(fo);
    fprintf(stderr, "fbk_peak %.4f g_mean %.5f sh_k %.4f\n", fbk_peak, g_mean, sh_k);
    return 0;
}
