/* Probe: which knobs arrive in render's params (R2)?
 *
 * Key-tracked tone (as psine.S) where
 *   params[0] (0..0x7FFF) = output level (full = -12 dBFS),
 *   params[1] >> 13       = octaves up (0..3).
 * Expected: T1 (engine page) blue/green encoders change level/octave. If T2 (envelope page)
 * encoders do instead, render receives the envelope knobs (see notes/params-path.md).
 * Registers: R0-R3, P0-P2, LC0/LT0/LB0 (scratch); R4-R6 saved/restored.
 */
    .text
    .global _render
_render:
    [--SP] = (R7:4);
    P0 = R0;                    /* this */
    P1 = R1;                    /* out */
    P2 = R2;                    /* params */
    R5 = W[P2] (Z);             /* level, Q15 */
    R6 = W[P2 + 2] (Z);
    R6 >>= 13;                  /* octave shift 0..3 */
    R1 = [P0 + 4];              /* phase */
    R4.H = 0;
    R4.L = 0x7FFF;
    P2 = 128;
    LSETUP (1f, 2f) LC0 = P2;
    P2.H = _pitch_buf;
    P2.L = _pitch_buf;
1:  R0 = R1 >>> 16;
    R3 = ABS R0;
    R3 = R4 - R3;
    R0.L = R0.L * R3.L;         /* y = x * (1 - |x|) */
    R0.L = R0.L * R5.L;         /* * level */
    W[P1++] = R0;
    R2 = [P2++];
    R2 <<= R6;                  /* octave up */
2:  R1 = R1 + R2;
    [P0 + 4] = R1;
    (R7:4) = [SP++];
    RTS;
