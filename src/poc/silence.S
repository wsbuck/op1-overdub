/* Proof of life #1: iter render replacement that outputs silence.
 *
 * Replaces the body of iter's render at 0x019B99D4 (firmware 246), see notes/engine-interface.md:
 *   void render(Iter* this /R0/, int16_t* out /R1/, const int16_t* params /R2/)
 * Writes 128 zero samples. Touches only R0, P0, P1, LC0/LT0/LB0 (all scratch; TE's own render clobbers LC0).
 */
    .text
    .global _render
_render:
    P0 = R1;                    /* out */
    R0 = 0;
    P1 = 128;
    LSETUP (1f, 1f) LC0 = P1;
1:  W[P0++] = R0;              /* 16-bit store of R0.L */
    RTS;
