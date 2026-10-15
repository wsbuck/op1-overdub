/* Extra synth engines (fw 246): engine ids 15, 16, ... that reuse iter's voice objects with our own vtables.
 *
 * Assembled by tools/build_slot.py into a FREE SDRAM region; the same tool patches TE's code to use the
 * relocated, grown tables in slot_gen.inc (see notes/engine-slot.md):
 *   _names     name -> id table, {char *name, int id}       (was 0x01DD4D98, 15 entries)
 *   _factory   producer factory jump table                    (was 0x018F5A04)
 *   _screens   engine -> screen jump table                    (was 0x018F803C; ours use iter's screen)
 *   _ranges    knob range table, 0x40 B per engine            (was 0x01DE5890; rows 0-14 filled at boot)
 *   _flagsN    six per-engine byte flag tables                (were 16 bytes each)
 * slot_gen.inc is generated from engines.json and the stock image (TE data: never committed).
 */

/* Factory case for one of our engines. Entered by JUMP from FUN_01a1b7d4 with its frame live:
 * P5 = Producer** out, SP already lowered by 12, R4-R7/P5 saved. Does what iter's case 14 does,
 * then points each voice at our vtable \vt. */
    .macro FACTORY_CASE vt
    R7 = 0;
    R6 = 6;
1:  R0 = R7;
    CALL _iter_get;
    P0 = R0;
    P1.L = \vt;
    P1.H = \vt;
    [P0] = P1;
    [P5++] = R0;
    R7 += 1;
    CC = R7 < R6;
    IF CC JUMP 1b;
    CALL _iter_init;
    SP += 0xc;
    P0 = [FP + 4];
    (R7:4, P5:5) = [SP++];
    UNLINK;
    JUMP (P0);
    .endm

/* Knob range row for our engines: 8 knobs x {min, max, scale, type}; full range and sensitivity,
 * type 0 (knobs 4-7 are the fine adjust of 0-3). */
    .macro RANGE_ROW
    .rept 8
    .short 0, 0x7FFF, 0x7FFF, 0
    .endr
    .endm

/* Screen hook: iter's screen page vtable slot +0x1C (update, called once per display frame while the
 * iter screen 0x14 is active) points here. R0 = page, R1 = dt. For TE's engines (id < 15) jump to iter's
 * own update untouched; for ours call ui_draw(engine, slot) (ui.c) instead, so iter's dials are not drawn.
 * Current synth slot = [0x01DF4E38], its engine id = [0xFF8016E4 + slot * 4] (notes/screen.md). */
    .text
    .align 4
    .global _ui_update
_ui_update:
    P0.L = 0x4E38;
    P0.H = 0x01DF;
    P1 = [P0];                          /* slot */
    P0.L = 0x16E4;
    P0.H = 0xFF80;
    P0 = P0 + (P1 << 2);
    R3 = [P0];                          /* engine id */
    R2 = 15;
    CC = R3 < R2;
    IF !CC JUMP 1f;
    JUMP.L _iter_update;                /* R0 (page) and R1 (dt) untouched */
1:  [--SP] = RETS;
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
    R0 = R3;                            /* engine */
    R1 = P1;                            /* slot */
    SP += -12;
    CALL _ui_draw;
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

/* FX screen hook: the filter FX screen page's update (vtable slot 0x01910CF0, stock 0x019E8308; our FX reuse
 * that screen, notes/fx-slot.md) points here. Edit mode [0x01DF4E3C] (0 synth, 1 drum, 3 master) and slot
 * [0x01DF4E38] give the FX id shown: synth [0xFF801734 + slot*4], drum [0xFF801754 + slot*4], master
 * [0xFF801090] (FUN_01a28da0). TE's FX (id < 8) and unknown modes: jump to the filter's own update with R0/R1
 * untouched; ours: fx_ui_draw(id, mode, slot) (ui.c). */
    .text
    .align 4
    .global _fx_ui_update
_fx_ui_update:
    P0.L = 0x4E3C;
    P0.H = 0x01DF;
    R3 = [P0];                          /* mode */
    P0.L = 0x4E38;
    P1 = [P0];                          /* slot */
    R2 = 3;
    CC = R3 == R2;
    IF CC JUMP 2f;
    P0.L = 0x1734;
    P0.H = 0xFF80;
    CC = R3 == 0;
    IF CC JUMP 1f;
    P0.L = 0x1754;
    R2 = 1;
    CC = R3 == R2;
    IF !CC JUMP 3f;
1:  P0 = P0 + (P1 << 2);
    JUMP 4f;
2:  P0.L = 0x1090;
    P0.H = 0xFF80;
4:  R2 = [P0];                          /* FX id */
    P2 = R3;                            /* mode (R0/R1 must survive for TE's update) */
    R3 = 8;
    CC = R2 < R3;
    IF !CC JUMP 5f;
3:  JUMP.L _filter_page_update;         /* R0 (page) and R1 (dt) untouched */
5:  [--SP] = RETS;
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
    R0 = R2;                            /* id */
    R1 = P2;                            /* mode */
    R2 = P1;                            /* slot */
    SP += -12;
    CALL _fx_ui_draw;
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

    .include "entry.inc"

/* HOLD and CHORD hooks (hold.c; installed by build_slot.py --hold / --chord). */
    .text
    .align 4
    C_ENTRY _hold_keyup_entry, _hold_keyup
    C_ENTRY _hold_shiftkey_entry, _hold_shiftkey
    C_ENTRY _chord_keydown_entry, _chord_keydown       /* CHORD (build_slot.py --chord) */

/* FX factory case for one of our ids. Entered by JUMP from TE's mono (FUN_01a1b6d0) or stereo (FUN_01a1b5f0) FX
 * factory with its frame live: R7 = the fixed placement address, saved R7 at [SP + 12]. Constructs TE's
 * filter FX there (\ctor), points it at our vtable \vt, returns the object like TE's cases. */
    .macro FX_CASE ctor, vt
    R0 = R7;
    CALL \ctor;
    P0 = R7;
    P1.L = \vt;
    P1.H = \vt;
    [P0] = P1;
    R0 = R7;
    P0 = [FP + 4];
    R7 = [SP + 12];
    UNLINK;
    JUMP (P0);
    .endm

/* Drum engines (notes/drum-engine.md): ids \first .. \first + \count - 1 after our synth ids, on TE's dbox voice objects
 * with our vtables (slot_gen.inc: _drum_vtables[i], _drum_inits[i] = engine i's vtable and C_ENTRY slot init).
 * build_slot.py instantiates DRUM_HOOKS once and patches TE to use it:
 *   _drum_factory           the drum factory FUN_01a1b9c0 sends ids other than 8/9 to an assert at 0x01A1BA38; that
 *                           instruction (R0.L = 0x5A40, the message) becomes JUMP.L here. Frame live: P5 = Producer**
 *                           out, R6 = drum slot, SP lowered by 12, R4-R7/P5 saved (R4 holds the vtable here). Our ids:
 *                           the engine's slot init (slot), then dbox's 12 voices (as TE's case 9) pointed at its vtable;
 *                           other ids: the assert.
 *   _drum_validate_lookup   replaces CALL FUN_019b9810 (name -> id) in the patch validator FUN_01a037a8: our ids read
 *                           as 9, so our drum patches validate as dbox ones (other ids assert in drum context).
 *   _drum_ui_update         dbox screen page update (slot 0x0190F8F0, stock 0x01972470): our ids draw
 *                           drum_ui_draw(slot, id) (ui.c) instead; R0/R1 untouched for TE's. Drum slot [0xFF8016D8],
 *                           its id [0xFF801704 + slot * 4] (as the drum ScreenId's caller 0x01977394).
 *   _drum_type_eq_load/save replace CALL FUN_019bdea4 (JSON value ==) where TE's per-key loader FUN_01a0036c (0x01A00630)
 *                           and saver FUN_019ffc50 (0x019FFFBA) compare the patch's "type" with Value("dbox"), so our
 *                           types (\names) load and save dbox_data too (DRUM_TYPE_EQ, entry.inc).
 *   _dp_type_hits           how often the loader [0] / saver [1] took the dbox path for one of our types (screens). */
    .macro DRUM_HOOKS first, count, names:vararg
    .text
    .align 4
    .global _drum_factory
_drum_factory:
    R1 = \first;
    R1 = R0 - R1;
    R2 = \count;
    CC = R1 < R2 (IU);
    IF CC JUMP 1f;
    R0.L = 0x5A40;                      /* the replaced instruction */
    JUMP.L _te_drum_assert;             /* 0x01A1BA3C: R0.H = 0x018F; CALL assert */
1:  P1 = R1;
    P0.L = _drum_vtables;
    P0.H = _drum_vtables;
    P0 = P0 + (P1 << 2);
    R4 = [P0];                          /* the engine's vtable (R4 is saved in TE's frame) */
    P0.L = _drum_inits;
    P0.H = _drum_inits;
    P0 = P0 + (P1 << 2);
    P2 = [P0];
    R0 = R6;
    CALL (P2);                          /* <engine>_slot_init(slot) through its C_ENTRY */
    R7 = 0;
    R5 = 12;                            /* kDrumVoiceCount */
2:  R0 = R7;
    CALL _te_dbox_voice;                /* FUN_0197189c(i): dbox voice i, built in place; returns it */
    P0 = R0;
    [P0] = R4;
    [P5++] = R0;
    R1 = R7;
    CALL _te_dbox_index;                /* FUN_01970da8(voice, i): +0x14 = i */
    R7 += 1;
    CC = R7 < R5;
    IF CC JUMP 2b;
    SP += 0xc;
    P0 = [FP + 4];
    (R7:4, P5:5) = [SP++];
    UNLINK;
    JUMP (P0);

    DRUM_TYPE_EQ \names

    .global _drum_validate_lookup
_drum_validate_lookup:
    [--SP] = RETS;
    SP += -12;
    CALL _te_engine_lookup;             /* FUN_019b9810(R0 = name) */
    SP += 12;
    RETS = [SP++];
    R1 = \first;
    R1 = R0 - R1;
    R2 = \count;
    CC = R1 < R2 (IU);
    IF !CC JUMP 3f;
    R0 = 9;
3:  RTS;

    .global _drum_ui_update
_drum_ui_update:
    P0.L = _te_drum_slot;               /* 0xFF8016D8 (symbols: the simulator test maps them) */
    P0.H = _te_drum_slot;
    P1 = [P0];                          /* drum slot */
    P0.L = _te_drum_ids;                /* 0xFF801704 */
    P0.H = _te_drum_ids;
    P0 = P0 + (P1 << 2);
    R3 = [P0];                          /* its drum engine id */
    R2 = \first;
    R2 = R3 - R2;
    P2 = R3;
    R3 = \count;
    CC = R2 < R3 (IU);
    IF CC JUMP 4f;
    JUMP.L _dbox_page_update;           /* R0 (page) and R1 (dt) untouched */
4:  [--SP] = RETS;
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
    R0 = P1;                            /* slot */
    R1 = P2;                            /* id */
    SP += -12;
    CALL _drum_ui_draw;
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

    .data
    .align 4
    .global _dp_type_hits
_dp_type_hits:
    .long 0, 0
    .text
    .endm

/* FX knob range row: 8 knobs x {min, max, scale}; full range and sensitivity. */
    .macro FX_RANGE_ROW
    .rept 8
    .short 0, 0x7FFF, 0x7FFF
    .endr
    .endm

    .include "slot_gen.inc"
