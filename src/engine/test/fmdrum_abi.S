/* fmdrum's firmware entries (C_ENTRY, src/slot/entry.inc), instantiated as slot_gen.inc does, for fmdrum_main.c. */
    .include "entry.inc"

    .text
    .align 4
    C_ENTRY _fd_render_entry, _fmdrum_render
    C_ENTRY _fd_note_on_entry, _fmdrum_note_on
    C_ENTRY _fd_active_entry, _fmdrum_active
    C_ENTRY _fd_note_off_entry, _fmdrum_note_off
    C_ENTRY _fd_slot_init_entry, _fmdrum_slot_init
