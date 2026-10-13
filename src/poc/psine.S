/* Proof of life #3: iter render replacement: key-tracked tone at -12 dBFS.
 *
 * Same as sine.S, but the per-sample phase increment comes from the voice's pitch buffer
 * int32[128] at 0xFF801950 (stock iter reads it the same way: I1, [I1++], added to phase A).
 * Replaces the body of iter's render at 0x019B99D4 (firmware 246), see notes/engine-interface.md.
 * _pitch_buf is resolved at link time: 0xFF801950 for the firmware (build.sh --defsym), an ordinary
 * array in the simulator harness.
 * Registers: R0-R3, P0-P2, LC0/LT0/LB0 (scratch); R4 saved/restored.
 */
    .text
    .global _render
_render:
    [--SP] = R4;
    P0 = R0;                    /* this */
    P1 = R1;                    /* out */
    R1 = [P0 + 4];              /* phase */
    R4.H = 0;
    R4.L = 0x7FFF;
    P2 = 128;
    LSETUP (1f, 2f) LC0 = P2;
    P2.H = _pitch_buf;
    P2.L = _pitch_buf;          /* pitch buffer: per-sample phase increments */
1:  R0 = R1 >>> 16;            /* x (Q15, sign-extended) */
    R3 = ABS R0;                /* |x| */
    R3 = R4 - R3;               /* 1 - |x| */
    R0.L = R0.L * R3.L;         /* y = x * (1 - |x|) */
    W[P1++] = R0;
    R2 = [P2++];                /* this sample's increment */
2:  R1 = R1 + R2;
    [P0 + 4] = R1;
    R4 = [SP++];
    RTS;
