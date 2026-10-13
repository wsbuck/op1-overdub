/* Proof of life #2: iter render replacement that outputs a fixed ~440 Hz tone at -12 dBFS.
 *
 * Replaces the body of iter's render at 0x019B99D4 (firmware 246), see notes/engine-interface.md:
 *   void render(Iter* this /R0/, int16_t* out /R1/, const int16_t* params /R2/)
 * Per-voice phase accumulator: this+4 (iter's own phase A field; note-on resets it to 0x80000000).
 * Waveform: parabolic sine approximation y = x*(1-|x|), x = phase as Q15 in [-1,1) -> peak 0.25 (-12 dB).
 * Increment assumes fs = 44100 Hz: round(440/44100 * 2^32) = 0x028DDFB9. Measured pitch tells us fs.
 * Registers: R0-R3, P0-P2, LC0/LT0/LB0 (scratch); R4 saved/restored.
 */
    .text
    .global _render
_render:
    [--SP] = R4;
    P0 = R0;                    /* this */
    P1 = R1;                    /* out */
    R1 = [P0 + 4];              /* phase */
    R2.H = 0x028D;
    R2.L = 0xDFB9;              /* phase increment */
    R4.H = 0;
    R4.L = 0x7FFF;
    P2 = 128;
    LSETUP (1f, 2f) LC0 = P2;
1:  R0 = R1 >>> 16;            /* x (Q15, sign-extended) */
    R3 = ABS R0;                /* |x| (0..0x8000) */
    R3 = R4 - R3;               /* 1 - |x| (may be -1 at x = -1: harmless, product ~ +1 LSB) */
    R0.L = R0.L * R3.L;         /* y = x * (1 - |x|), Q15, rounded */
    W[P1++] = R0;
2:  R1 = R1 + R2;
    [P0 + 4] = R1;
    R4 = [SP++];
    RTS;
