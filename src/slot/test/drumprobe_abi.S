/* The drum probe's firmware entries (C_ENTRY, entry.inc), instantiated as slot_gen.inc does, for drumprobe_main.c. */
    .include "entry.inc"

    .text
    .align 4
    C_ENTRY _dp_render_entry, _drumprobe_render
    C_ENTRY _dp_note_on_entry, _drumprobe_note_on
    C_ENTRY _dp_active_entry, _drumprobe_active
    C_ENTRY _dp_note_off_entry, _drumprobe_note_off
    C_ENTRY _dp_slot_init_entry, _drumprobe_slot_init
