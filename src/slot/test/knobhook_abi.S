/* The knob hook and the drum type compare (entry.inc), instantiated as slot_gen.inc / slot.S do, for knobhook_main.c:
 * engine ids 20 (sprobe), 21 (harm), 22 (fm4) hooked, drum types "dprobe" and "fmdrum". */
    .include "entry.inc"

    KNOB_HOOK 20, 21, 22

    .text
    .align 4
    DRUM_TYPE_EQ _dname, _fname

    .data
    .align 4
_dname:
    .asciz "dprobe"
    .align 4
_fname:
    .asciz "fmdrum"
