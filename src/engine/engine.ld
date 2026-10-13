/* One flat code block: trampoline first (it is the vtable target), then C code and constants.
 * Anything needing .data/.bss fails the link: the image only carries this single block. */
/* Default: iter render 0x019B99D4-0x019B9B89, fw 246. build.sh passes a larger limit for free SDRAM. */
SLOT_SIZE = DEFINED(SLOT_SIZE) ? SLOT_SIZE : 438;

SECTIONS
{
  .text : {
    *(.text.entry)
    *(.text .text.*)
    *(.rodata .rodata.*)
  }
  .data : { *(.data .data.* .bss .bss.* COMMON) }
  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
ASSERT(SIZEOF(.data) == 0, "engine must not use .data/.bss")
ASSERT(SIZEOF(.text) <= SLOT_SIZE, "engine exceeds SLOT_SIZE")
