/* Canary probe with FSK log output: canary.S's region checks, reported as data instead of tones.
 *
 * Same regions, fill and sweep as canary.S. On a region's first damage, a second pass over the
 * checked window records the lowest damaged address and the value found there.
 * Output is fsk.inc frames (decode with tools/fsk_decode.py) of _iter_buf[4..43], 10 words:
 *   mask, then per region r: first damaged address, value; then completed sweeps.
 *
 * Shared state in _iter_buf (zeroed when iter is selected):
 *   [0] cursor, [4] damage mask, [8 + 8r] address, [12 + 8r] value, [40] sweeps.
 * Registers: R0-R3, P0-P2, LC0/LT0/LB0 (scratch); R4-R7, P3-P5 saved/restored.
 */
    .ifndef CHECK_WORDS
    .set CHECK_WORDS, 512
    .endif
    .ifndef REGION_BYTES
    .set REGION_BYTES, 0x100000
    .endif
    .ifndef FILL_WORD
    .set FILL_WORD, 0x5AA5C33C
    .endif
    .set STEP_BYTES, CHECK_WORDS * 4

    .include "fsk.inc"

    /* R3 = OR of (word ^ FILL_WORD) over CHECK_WORDS words of \sym at cursor P3. On damage: set
     * R7's bit; if it was clear, scan the window downwards and record the lowest mismatch. */
    .macro CHECK sym, bit
    P0.H = \sym;
    P0.L = \sym;
    P0 = P0 + P3;
    R3 = 0;
    LSETUP (8f, 9f) LC0 = P4;
8:  R0 = [P0++];
    R0 = R0 ^ R2;
9:  R3 = R3 | R0;
    CC = R3 == 0;
    IF CC JUMP 8f;
    CC = BITTST (R7, \bit);
    IF CC JUMP 7f;
    P0 += -4;
    LSETUP (5f, 6f) LC0 = P4;
5:  R0 = [P0--];
    CC = R0 == R2;
    IF !CC R4 = R0;
6:  IF !CC R5 = P0;             /* = mismatch address - 4 */
    R5 += 4;
    [P2 + (8 + 8 * \bit)] = R5;
    [P2 + (12 + 8 * \bit)] = R4;
7:  BITSET (R7, \bit);
8:
    .endm

    .text
    .global _render
_render:
    [--SP] = (R7:4, P5:3);
    P5 = R0;                    /* this */
    P1 = R1;                    /* out */
    P2.H = _iter_buf;
    P2.L = _iter_buf;
    R6 = [P2];                  /* cursor */
    R7 = [P2 + 4];              /* damage mask */
    P3 = R6;
    P4 = CHECK_WORDS;
    R2.H = FILL_WORD;
    R2.L = FILL_WORD;
    CHECK _canary0, 0
    CHECK _canary1, 1
    CHECK _canary2, 2
    CHECK _canary3, 3
    [P2 + 4] = R7;

    /* advance cursor; at wrap, cursor = 0 and sweeps += 1 */
    R0 = STEP_BYTES;
    R6 = R6 + R0;
    R0.H = REGION_BYTES;
    R0.L = REGION_BYTES;
    CC = R6 < R0 (IU);
    IF CC JUMP 1f;
    R6 = 0;
    R0 = [P2 + 40];
    R0 += 1;
    [P2 + 40] = R0;
1:  [P2] = R6;

    P0 = P2;
    P0 += 4;
    FSK 10
    (R7:4, P5:3) = [SP++];
    RTS;
