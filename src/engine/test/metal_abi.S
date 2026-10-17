/* metal's firmware entries (C_ENTRY, src/slot/entry.inc), instantiated as slot_gen.inc does, for metal_main.c. */
    .include "entry.inc"

    .text
    .align 4
    C_ENTRY _mt_render_entry, _metal_render
    C_ENTRY _mt_note_on_entry, _metal_note_on
    C_ENTRY _mt_active_entry, _metal_active
    C_ENTRY _mt_note_off_entry, _metal_note_off
    C_ENTRY _mt_slot_init_entry, _metal_slot_init
