/* Simulator ABI test of our FX entry wrappers (entry.inc), as the firmware instantiates them per FX.
 * int fx_abi_call(void (*entry)(), void *this, void *a1, void *a2, void *a3)
 * Loads sentinels into every register a TE caller may rely on (R4-R7, P3-P5, FP, SP, LC0/LT0/LB0,
 * LC1/LT1/LB1, L0-L3), calls `entry` with R0 = this, R1 = a1, R2 = a2 and a3 at [SP + 12] (the stereo FX
 * call's 4th argument, as at 0xFFA017E6), and returns a bitmask of the registers that changed. The loop
 * registers point at _never, so armed counters can't fire. Assembled with --defsym NO_FX=1, only fx_abi_call
 * (drumprobe_check.py uses it for the drum probe's entries). */
    .include "entry.inc"

    .text
    .align 4
    .ifndef NO_FX
    FX_MONO_ENTRY _ensemble_mono_entry, _ensemble_mono
    FX_STEREO_ENTRY _ensemble_stereo_entry, _ensemble_stereo
    FX_MONO_ENTRY _pingpong_mono_entry, _pingpong_mono
    FX_STEREO_ENTRY _pingpong_stereo_entry, _pingpong_stereo
    FX_MONO_ENTRY _distortion_mono_entry, _distortion_mono
    FX_STEREO_ENTRY _distortion_stereo_entry, _distortion_stereo
    FX_MONO_ENTRY _crush_mono_entry, _crush_mono
    FX_STEREO_ENTRY _crush_stereo_entry, _crush_stereo
    FX_MONO_ENTRY _repeat_mono_entry, _repeat_mono
    FX_STEREO_ENTRY _repeat_stereo_entry, _repeat_stereo
    .endif

    .macro CHECK reg, hi, lo, bit
    R1 = \reg;
    R2.H = \hi;
    R2.L = \lo;
    CC = R1 == R2;
    IF CC JUMP 1f;
    BITSET(R0, \bit);
1:
    .endm

    .global _fx_abi_call
_fx_abi_call:
    [--SP] = RETS;
    [--SP] = (R7:4, P5:3);
    [--SP] = FP;
    P5.H = _args;
    P5.L = _args;
    [P5] = R0;                              /* entry */
    [P5 + 4] = R1;                          /* this */
    [P5 + 8] = R2;                          /* a1 */
    R3 = [SP + 0x30];                       /* a2: caller's [SP + 12], above our 9 saved words */
    [P5 + 12] = R3;
    R3 = [SP + 0x34];                       /* a3: caller's [SP + 16] */
    [P5 + 16] = R3;
    P1.H = _saved_sp; P1.L = _saved_sp;
    [P1] = SP;
    R4.H = 0x4444; R4.L = 0x4444;
    R5.H = 0x5555; R5.L = 0x5555;
    R6.H = 0x6666; R6.L = 0x6666;
    R7.H = 0x7777; R7.L = 0x7777;
    P3.H = 0x3333; P3.L = 0x3333;
    P4.H = 0x4444; P4.L = 0x4444;
    FP.H = 0xF0F0; FP.L = 0xF0F0;
    P1.H = _never; P1.L = _never;
    LT0 = P1; LB0 = P1; LT1 = P1; LB1 = P1;
    P1 = 7; LC0 = P1;
    P1 = 9; LC1 = P1;
    L0 = 0x10; L1 = 0x11; L2 = 0x12; L3 = 0x13;
    SP += -16;
    R3 = [P5 + 16];
    [SP + 12] = R3;
    P2 = [P5];
    R0 = [P5 + 4];
    R1 = [P5 + 8];
    R2 = [P5 + 12];
    P5.H = 0x5555; P5.L = 0x5555;
    CALL (P2);
    SP += 16;
    R0 = 0;
    CHECK R4, 0x4444, 0x4444, 0
    CHECK R5, 0x5555, 0x5555, 1
    CHECK R6, 0x6666, 0x6666, 2
    CHECK R7, 0x7777, 0x7777, 3
    CHECK P3, 0x3333, 0x3333, 4
    CHECK P4, 0x4444, 0x4444, 5
    CHECK P5, 0x5555, 0x5555, 6
    CHECK FP, 0xF0F0, 0xF0F0, 7
    CHECK LC0, 0, 7, 8
    CHECK LC1, 0, 9, 9
    CHECK LT0, _never, _never, 10
    CHECK LB0, _never, _never, 11
    CHECK LT1, _never, _never, 12
    CHECK LB1, _never, _never, 13
    CHECK L0, 0, 0x10, 14
    CHECK L1, 0, 0x11, 15
    CHECK L2, 0, 0x12, 16
    CHECK L3, 0, 0x13, 17
    P1.H = _saved_sp; P1.L = _saved_sp;
    R2 = [P1];
    R1 = SP;
    CC = R1 == R2;
    IF CC JUMP 2f;
    BITSET(R0, 18);
2:
    R1 = 0;                                 /* disarm */
    LC0 = R1;
    LC1 = R1;
    L0 = 0; L1 = 0; L2 = 0; L3 = 0;
    FP = [SP++];
    (R7:4, P5:3) = [SP++];
    RETS = [SP++];
    RTS;
_never:
    NOP;
    NOP;

    .data
    .align 4
_saved_sp:
    .long 0
_args:
    .long 0, 0, 0, 0, 0
