/* Probe: are the candidate SDRAM regions really free?
 *
 * The image fills four 1 MB regions (_canary0.._canary3) with FILL_WORD at boot (ldr_patch.py
 * --fill). Every render call checks CHECK_WORDS words of each region at a rotating cursor, so each
 * region is swept fully every REGION_BYTES / (4 * CHECK_WORDS) calls (512 calls ~= 0.75 s at
 * 1 key = 689 calls/s). Any word != FILL_WORD sets the region's bit in the damage mask.
 *
 *   mask == 0: key-tracked tone (as psine.S)
 *   mask != 0: square wave, octave up = index of the lowest damaged region (0..3)
 *
 * Shared state in iter's private buffer (_iter_buf, zeroed when iter is selected):
 * [0] cursor (byte offset into every region), [4] damage mask. Reselecting iter clears the mask,
 * but a region that is still damaged is found again within one sweep.
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

    /* R3 = OR of (word ^ FILL_WORD) over CHECK_WORDS words of \sym at cursor P3; R7 |= damage bit */
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
    BITSET (R7, \bit);
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
    R2.H = FILL_WORD;          /* .H/.L of a constant take its halves */
    R2.L = FILL_WORD;
    CHECK _canary0, 0
    CHECK _canary1, 1
    CHECK _canary2, 2
    CHECK _canary3, 3

    /* advance cursor, wrap at REGION_BYTES */
    R0 = STEP_BYTES;
    R6 = R6 + R0;
    R0.H = REGION_BYTES;
    R0.L = REGION_BYTES;
    R1 = 0;
    CC = R6 < R0 (IU);
    IF !CC R6 = R1;
    [P2] = R6;
    [P2 + 4] = R7;

    R1 = [P5 + 4];              /* phase */
    P2 = 128;
    CC = R7 == 0;
    IF !CC JUMP 3f;

    /* clean: key-tracked tone (as psine.S) */
    R4.H = 0;
    R4.L = 0x7FFF;
    LSETUP (1f, 2f) LC0 = P2;
    P2.H = _pitch_buf;
    P2.L = _pitch_buf;
1:  R0 = R1 >>> 16;
    R3 = ABS R0;
    R3 = R4 - R3;
    R0.L = R0.L * R3.L;         /* y = x * (1 - |x|) */
    W[P1++] = R0;
    R2 = [P2++];
2:  R1 = R1 + R2;
    JUMP 6f;

    /* damaged: square wave at -12 dBFS, R6 = octave shift = lowest damaged region */
3:  R6 = 0;
    CC = BITTST (R7, 0);
    IF CC JUMP 4f;
    R6 = 1;
    CC = BITTST (R7, 1);
    IF CC JUMP 4f;
    R6 = 2;
    CC = BITTST (R7, 2);
    IF CC JUMP 4f;
    R6 = 3;
4:  R5 = 0x2000 (Z);
    LSETUP (5f, 7f) LC0 = P2;
    P2.H = _pitch_buf;
    P2.L = _pitch_buf;
5:  R0 = R1 >>> 31;             /* 0 or -1 */
    R0 = R0 ^ R5;               /* +0x2000 or -0x2001 */
    W[P1++] = R0;
    R2 = [P2++];
    R2 <<= R6;
7:  R1 = R1 + R2;

6:  [P5 + 4] = R1;
    (R7:4, P5:3) = [SP++];
    RTS;
