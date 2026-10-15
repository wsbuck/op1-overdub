/* TE-side stand-ins for drumhooks_check.py: the drum factory's frame (TE's FUN_01a1b9c0 prologue, then the patched
 * JUMP to _drum_factory) and the assert path's continuation at 0x01A1BA3C (TE's epilogue). */
    .text
    .align 4
/* int factory_call(id R0, Producer **out R1, slot R2): TE's prologue as at FUN_01a1b9c0, then _drum_factory. */
    .global _factory_call
_factory_call:
    LINK 0;
    [--SP] = (R7:4, P5:5);
    SP += -12;
    P5 = R1;
    R6 = R2;
    JUMP.L _drum_factory;

/* 0x01A1BA3C: R0.H = 0x018F; CALL assert; then the epilogue. Records R0 (the message's low half). */
    .global _te_drum_assert
_te_drum_assert:
    P1.L = _assert_r0;
    P1.H = _assert_r0;
    [P1] = R0;
    SP += 0xc;
    P0 = [FP + 4];
    (R7:4, P5:5) = [SP++];
    UNLINK;
    JUMP (P0);
