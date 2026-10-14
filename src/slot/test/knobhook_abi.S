/* The knob hook and the drum type compare (entry.inc), instantiated as slot_gen.inc / slot.S do, for knobhook_main.c:
 * engine id 20 hooked, drum type "dprobe". */
    .include "entry.inc"

    KNOB_HOOK 20

    .text
    .align 4
    DRUM_TYPE_EQ _dname

    .data
    .align 4
_dname:
    .asciz "dprobe"
