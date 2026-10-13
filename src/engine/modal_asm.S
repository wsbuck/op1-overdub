/* Resonator inner loops for modal.c: one mode over n samples, state and coefficients in registers,
 * hardware loop. GCC 4.3's loop for the same C is ~23 instructions per sample (each Q31 product is a
 * separate 6-instruction MAC sequence); here the products share the accumulators.
 *
 *   y[i] = 2 (c1 y[i-1] + c2h y[i-2] (+ gh x[i]));  sum[i] += y[i]
 *   c1 = r cos w (the bits of a1 = 2 r cos w in Q30), c2h = -r^2 / 2, gh = g / 2: Q31.  y, x, sum: Q24.
 * The half-scale sum is doubled with a saturating shift, so a large mode cannot wrap (a first version fed
 * 2 y[i-1] into the multiply and went unstable above 64.0; LOG.md).
 * Each Q31 product is formed like __builtin_bfin_mult_fr1x32x32: lo*lo (FU) >> 16, plus the two cross
 * terms (M), >>> 15, plus hi*hi (fractional) in A0; all products are summed before the shift.
 *
 * void modal_mode(int32_t *sum, int32_t c1, int32_t c2h, int32_t *state, int n);           state = {y1, y2}
 * void modal_mode_x(int32_t *sum, int32_t c1, int32_t c2h, int32_t *state, int32_t gh, const int32_t *x, int n);
 * Args R0-R2, then [SP+12], [SP+16], ... on entry. Saves R4-R7 and loop 0 (LC0/LT0/LB0). n >= 1.
 */
    .text
    .align 4
    .global _modal_mode
_modal_mode:
    [--SP] = (R7:4);
    [--SP] = LC0;
    [--SP] = LT0;
    [--SP] = LB0;
    P0 = R0;                    /* sum */
    R6 = R1;                    /* c1 */
    R7 = R2;                    /* c2h */
    P1 = [SP + 40];             /* state (12 + 7 pushed words) */
    P2 = [SP + 44];             /* n */
    R3 = [P1];                  /* y1 */
    R2 = [P1 + 4];              /* y2 */
    LSETUP (1f, 2f) LC0 = P2;
1:  A1 = R6.L * R3.L (FU);
    A1 += R7.L * R2.L (FU);
    A1 = A1 >> 16;
    A0 = R6.H * R3.H, A1 += R6.H * R3.L (M);
    A0 += R7.H * R2.H, A1 += R7.H * R2.L (M);
    A1 += R3.H * R6.L (M);
    A1 += R2.H * R7.L (M);
    A1 = A1 >>> 15;
    R0 = (A0 += A1);            /* y / 2 */
    R0 = R0 << 1 (S);           /* y */
    R1 = [P0];
    R1 = R1 + R0;
    R2 = R3;                    /* y2 = y1 */
    R3 = R0;                    /* y1 = y */
2:  [P0++] = R1;
    [P1] = R3;
    [P1 + 4] = R2;
    LB0 = [SP++];
    LT0 = [SP++];
    LC0 = [SP++];
    (R7:4) = [SP++];
    RTS;

    .align 4
    .global _modal_mode_x
_modal_mode_x:
    [--SP] = (R7:4);
    [--SP] = LC0;
    [--SP] = LT0;
    [--SP] = LB0;
    P0 = R0;                    /* sum */
    R6 = R1;                    /* c1 */
    R7 = R2;                    /* c2h */
    P1 = [SP + 40];             /* state */
    R4 = [SP + 44];             /* gh */
    P2 = [SP + 48];             /* x */
    R1 = [SP + 52];             /* n */
    LC0 = R1;
    R3 = [P1];
    R2 = [P1 + 4];
    LSETUP (3f, 4f) LC0;
3:  R5 = [P2++];                /* x */
    A1 = R6.L * R3.L (FU);
    A1 += R7.L * R2.L (FU);
    A1 += R4.L * R5.L (FU);
    A1 = A1 >> 16;
    A0 = R6.H * R3.H, A1 += R6.H * R3.L (M);
    A0 += R7.H * R2.H, A1 += R7.H * R2.L (M);
    A0 += R4.H * R5.H, A1 += R4.H * R5.L (M);
    A1 += R3.H * R6.L (M);
    A1 += R2.H * R7.L (M);
    A1 += R5.H * R4.L (M);
    A1 = A1 >>> 15;
    R0 = (A0 += A1);
    R0 = R0 << 1 (S);
    R1 = [P0];
    R1 = R1 + R0;
    R2 = R3;
    R3 = R0;
4:  [P0++] = R1;
    [P1] = R3;
    [P1 + 4] = R2;
    LB0 = [SP++];
    LT0 = [SP++];
    LC0 = [SP++];
    (R7:4) = [SP++];
    RTS;
