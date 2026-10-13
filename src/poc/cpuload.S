/* Measurement #1: CPU budget of the render call.
 *
 * Plays the key-tracked tone of psine.S, but before rendering burns step * 20000 cycles
 * (single-instruction hardware loop: 1 cycle per iteration). Every TICK_CALLS render calls (all
 * voices together) the step increments and that call outputs a silent block (an audible tick).
 * Step wraps to 0 after STEP_MAX so an overload recovers by itself.
 *
 *   tick interval: 1378 calls = 4.0 s if 1 voice renders per 2.9 ms block, 0.67 s if 6 do
 *   budget: ticks counted until the audio breaks up -> cycles per call ~= ticks * 20000
 *
 * Shared state lives in iter's private buffer (_iter_buf = 0x01DD4E18, zeroed when iter is
 * selected; only iter code references it): [0] call counter, [4] step.
 * Registers: R0-R3, P0-P2, LC0/LT0/LB0 (scratch); R4 saved/restored.
 */
    .ifndef TICK_CALLS
    .set TICK_CALLS, 1378
    .endif
    .ifndef STEP_CYCLES
    .set STEP_CYCLES, 20000
    .endif
    .ifndef STEP_MAX
    .set STEP_MAX, 70
    .endif

    .text
    .global _render
_render:
    [--SP] = R4;
    P0 = R0;                    /* this */
    P1 = R1;                    /* out */
    P2.H = _iter_buf;
    P2.L = _iter_buf;

    /* tick bookkeeping: R3 = 1 if this call ticks */
    R0 = [P2];
    R0 += 1;
    R3 = 0;
    R2.H = 0;
    R2.L = TICK_CALLS;
    CC = R0 < R2;
    IF CC JUMP 1f;
    R0 = 0;
    R2 = [P2 + 4];
    R2 += 1;
    R1 = STEP_MAX;
    CC = R2 < R1;
    IF !CC R2 = R3;             /* wrap step to 0 (R3 is 0 here) */
    [P2 + 4] = R2;
    R3 = 1;
1:  [P2] = R0;

    /* burn step * STEP_CYCLES cycles */
    R0 = [P2 + 4];
    R2.H = 0;
    R2.L = STEP_CYCLES;
    R0 *= R2;
    CC = R0 == 0;
    IF CC JUMP 3f;
    P2 = R0;
    LSETUP (2f, 2f) LC0 = P2;
2:  NOP;
3:
    /* tick: one silent block */
    CC = R3 == 0;
    IF CC JUMP 4f;
    R0 = 0;
    P2 = 128;
    LSETUP (5f, 5f) LC0 = P2;
5:  W[P1++] = R0;
    R4 = [SP++];
    RTS;

    /* key-tracked tone (as psine.S) */
4:  R1 = [P0 + 4];              /* phase */
    R4.H = 0;
    R4.L = 0x7FFF;
    P2 = 128;
    LSETUP (6f, 7f) LC0 = P2;
    P2.H = _pitch_buf;
    P2.L = _pitch_buf;
6:  R0 = R1 >>> 16;
    R3 = ABS R0;
    R3 = R4 - R3;
    R0.L = R0.L * R3.L;
    W[P1++] = R0;
    R2 = [P2++];
7:  R1 = R1 + R2;
    [P0 + 4] = R1;
    R4 = [SP++];
    RTS;
