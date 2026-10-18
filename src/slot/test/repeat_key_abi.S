/* Simulator ABI test of the repeat FX's manual-key entries (repeat_key_check.py): the drum and mixer controllers'
 * press / release slots as build_slot.py instantiates them (slot_gen.inc). fx_abi_call comes from fx_abi.S (NO_FX). */
    .include "entry.inc"
    .text
    .align 4
    C_ENTRY _rk_drum_press_entry, _rk_drum_press
    C_ENTRY _rk_drum_release_entry, _rk_drum_release
    C_ENTRY _rk_master_press_entry, _rk_master_press
    C_ENTRY _rk_master_release_entry, _rk_master_release
