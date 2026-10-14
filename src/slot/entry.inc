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

/* Drum engine: TE's per-key loader/saver type compare (slot.S DRUM_HOOKS, notes/drum-engine.md). Replaces
 * CALL FUN_019bdea4 (JSON value ==, R0 = the patch's "type", R1 = Value("dbox")): TE's result first, unchanged for
 * "drum"/"dbox"; only when it is 0, compare with Value(\name) built the way TE builds its own (FUN_019bdc4c in a
 * 16-byte stack slot, destroyed by FUN_019bdd18(v, 2)). Counts our hits in dp_type_hits[0] (load) / [1] (save). */
    .macro DRUM_TYPE_EQ name
    .global _drum_type_eq_load
    .global _drum_type_eq_save
_drum_type_eq_load:
    R3 = 0;
    JUMP.S 5f;
_drum_type_eq_save:
    R3 = 4;
5:  LINK 16;                            /* [FP - 16]: a 16-byte Value, as TE's own temporaries */
    [--SP] = (R7:5);
    SP += -12;
    R7 = R0;                            /* the patch's "type" value */
    R5 = R3;
    CALL _te_json_eq;                   /* TE's compare: R0 = type, R1 = Value("dbox") */
    CC = R0 == 0;
    IF !CC JUMP 6f;
    R0 = FP;
    R0 += -16;
    R1.L = \name;
    R1.H = \name;
    CALL _te_json_str;                  /* Value(const char *) in place; returns it */
    R1 = R0;
    R0 = R7;
    CALL _te_json_eq;
    R6 = R0;
    R0 = FP;
    R0 += -16;
    R1 = 2;
    CALL _te_json_free;
    R0 = R6;
    CC = R0 == 0;
    IF CC JUMP 6f;
    P0.L = _dp_type_hits;
    P0.H = _dp_type_hits;
    P1 = R5;
    P0 = P0 + P1;
    R1 = [P0];
    R1 += 1;
    [P0] = R1;
6:  SP += 12;
    (R7:5) = [SP++];
    UNLINK;
    RTS;

    .endm

/* Synth encoder hook (knobhook.c, notes/knob-hook.md): replaces CALL FUN_01a1dfec at 0x01A5063E in TE's engine-page
 * encoder handler (R0 knob, R1 int16 *delta, R2 engine id, [SP + 12] the slot's knobs). Engine \id goes to
 * knobhook_turn(knob, delta, id, knobs) (FX_STEREO_ENTRY moves the 4th argument); every other id jumps to TE's function
 * with all four arguments untouched. */
    .macro KNOB_HOOK id
    .text
    .align 4
    .global _knob_hook
_knob_hook:
    R3 = \id;
    CC = R2 == R3;
    IF CC JUMP 1f;
    JUMP.L _te_knob_apply;
1:  JUMP.L _knob_hook_entry;
    FX_STEREO_ENTRY _knob_hook_entry, _knobhook_turn
    .endm

