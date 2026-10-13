/* C entry wrappers shared by slot.S and the simulator ABI test (src/slot/test/fx_abi_check.py). */

/* C entry wrapper: saves RETS and both hardware loops, zeroes L0-L3 (GCC code may use loop 1 and assumes
 * L = 0; TE's callers are not known to tolerate either), reserves the 12-byte argument area, calls \fn
 * with R0-R2 untouched and returns its R0. */
    .macro C_ENTRY name, fn
    .global \name
\name:
    [--SP] = RETS;
    [--SP] = LC0;
    [--SP] = LT0;
    [--SP] = LB0;
    [--SP] = LC1;
    [--SP] = LT1;
    [--SP] = LB1;
    [--SP] = L0;
    [--SP] = L1;
    [--SP] = L2;
    [--SP] = L3;
    L0 = 0;
    L1 = 0;
    L2 = 0;
    L3 = 0;
    SP += -12;
    CALL \fn;
    SP += 12;
    L3 = [SP++];
    L2 = [SP++];
    L1 = [SP++];
    L0 = [SP++];
    LB1 = [SP++];
    LT1 = [SP++];
    LC1 = [SP++];
    LB0 = [SP++];
    LT0 = [SP++];
    LC0 = [SP++];
    RETS = [SP++];
    RTS;
    .endm

/* Our FX (src/slot/fx.json, <source>.c; build_slot.py --fx). Mono process(this, buf, params) passes straight
 * through; stereo process(this, L, R, [SP + 12] params) moves its 4th argument to the C call's. The entries
 * themselves are instantiated in slot_gen.inc, one pair per FX. */
    .macro FX_MONO_ENTRY name, fn
    C_ENTRY \name, \fn
    .endm

    .macro FX_STEREO_ENTRY name, fn
    .global \name
\name:
    P0 = [SP + 12];                     /* params (4th argument) */
    [--SP] = RETS;
    [--SP] = LC0;
    [--SP] = LT0;
    [--SP] = LB0;
    [--SP] = LC1;
    [--SP] = LT1;
    [--SP] = LB1;
    [--SP] = L0;
    [--SP] = L1;
    [--SP] = L2;
    [--SP] = L3;
    L0 = 0;
    L1 = 0;
    L2 = 0;
    L3 = 0;
    SP += -16;
    [SP + 12] = P0;
    CALL \fn;
    SP += 16;
    L3 = [SP++];
    L2 = [SP++];
    L1 = [SP++];
    L0 = [SP++];
    LB1 = [SP++];
    LT1 = [SP++];
    LC1 = [SP++];
    LB0 = [SP++];
    LT0 = [SP++];
    LC0 = [SP++];
    RETS = [SP++];
    RTS;
    .endm
