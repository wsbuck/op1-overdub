/* A test slot_gen.inc for drumhooks_check.py: what build_slot.py generates for two drum engines, ids 23 and 24, with
 * marker vtables. Assembled with slot.S (-I this directory first). */
    .text
    .align 4
    DRUM_HOOKS 23, 2, _name23, _name24
    C_ENTRY _drum23_slot_init_entry, _drumA_slot_init
    C_ENTRY _drum24_slot_init_entry, _drumB_slot_init
    .align 4
    .global _drum_vtable23, _drum_vtable24
_drum_vtable23:
    .rept 12
    .long 0x23232323
    .endr
_drum_vtable24:
    .rept 12
    .long 0x24242424
    .endr
_drum_vtables:
    .long _drum_vtable23, _drum_vtable24
_drum_inits:
    .long _drum23_slot_init_entry, _drum24_slot_init_entry
    .align 4
_name23:
    .asciz "dprobe"
    .align 4
_name24:
    .asciz "fmdrum"
    .align 4
