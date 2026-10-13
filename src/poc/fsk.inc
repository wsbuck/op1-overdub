/* FSK logger: sends payload words as audio, one bit per render call (128 samples).
 *
 * Frame = FSK_SYNC, then n payload words (MSB first), then a check word. All 32-bit.
 *   check: Fletcher sum over the payload, a = 1, b = 0; per word a += w, b += a; check = b.
 * Bit 0 = 8 square cycles per block (2756 Hz), bit 1 = 16 cycles per block (5513 Hz), at -12 dBFS.
 * Whole cycles per block, so each block holds exactly one symbol.
 *
 * The bit position is per voice, in voice +0x14 (iter's note-on clears it), so every note starts a
 * fresh frame and a second rendered voice doesn't advance this one's stream.
 * Payload words are read live; a word that changes mid-frame fails the check and the decoder drops
 * that frame.
 *
 * In:  P0 = payload, P1 = out, P5 = this. Clobbers R0-R3, P0, P2, LC0/LT0/LB0. Advances P1 by 256.
 * Decoder: tools/fsk_decode.py.
 */
    .ifndef FSK_SYNC
    .set FSK_SYNC, 0x1ACFFC1D           /* CCSDS attached sync marker */
    .endif

    .macro FSK n
    R3 = [P5 + 0x14];                   /* bit position in frame */
    R2 = R3 >> 5;                       /* word index */
    R0.H = FSK_SYNC;
    R0.L = FSK_SYNC;
    CC = R2 == 0;
    IF CC JUMP 3f;
    R1 = \n (Z);
    CC = R1 < R2;
    IF CC JUMP 1f;
    R1 = R2 << 2;                       /* payload word idx-1 */
    P2 = R1;
    P0 = P0 + P2;
    R0 = [P0 + -4];
    JUMP 3f;
1:  R1 = 1;                             /* check word */
    R0 = 0;
    P2 = \n;
    LSETUP (2f, 6f) LC0 = P2;
2:  R2 = [P0++];
    R1 = R1 + R2;
6:  R0 = R0 + R1;
3:  R1 = 31;                            /* bit = R0 >> (31 - pos % 32) & 1 */
    R1 = R3 & R1;
    R0 <<= R1;
    R0 >>= 31;
    R2.H = 0x1000;                      /* phase step: 8 << bit cycles per 128 samples */
    R2.L = 0;
    R2 <<= R0;
    R3 += 1;
    R0 = (\n + 2) * 32 (Z);
    R1 = 0;
    CC = R3 < R0;
    IF !CC R3 = R1;
    [P5 + 0x14] = R3;
    R3 = 0x2000 (Z);
    P2 = 128;
    LSETUP (4f, 5f) LC0 = P2;
4:  R0 = R1 >>> 31;                     /* 0 or -1 */
    R0 = R0 ^ R3;                       /* +0x2000 or -0x2001 */
    W[P1++] = R0;
5:  R1 = R1 + R2;
    .endm
