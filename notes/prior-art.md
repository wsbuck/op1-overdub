# Prior art and hardware (Phase 0, 2026-10-06)
Sources are linked. "Community" means a third-party claim we haven't checked ourselves.

## Hardware
- DSP: **ADSP-BF524C** (BF52x with an on-chip codec), 400 MHz.
  - Community: op1hacks/docs hardware.md and the Tolsi OTP dump "TE-OP-1-BF524-OTP-dump".
  - Speed grade not verified.
- 64 MB SDRAM. Community; consistent with SDRAM LDR addresses up to ~0x01Axxxxx.
- 512 MB NAND (Micron MT29F4G08ABBDA, BGA63) holding YAFFS2. Community; the YAFFS2 part is **Confirmed** from te-boot strings.
- JTAG test pads on the bottom of the DSP board (EMU, TMS, TCK, TRST, TDI, TDO, plus VPPOTP).
  - Source: photo in `third_party/op1dumps/TE-OP1-board-useful-pins.jpg`.
  - No public report of an ICE session. No UART found.

## Boot chain
- BF524 boot ROM → te-boot from the NAND boot blocks → te-boot loads `/yaffs2/OP1_vdk.ldr`.
  - Community; the te-boot strings are consistent with this.
- te-boot is reflashed from an update image when the bundled te-boot's version differs from the installed one.
  - The op1dumps README warns that a bad te-boot requires an external NAND programmer.
  - **This contradicts the PROMPT.md assumption** that the bootloader is untouched by updates.
- OTP pages 0x10–0x12, 0xD0–0xD3, 0xDF, 0xD8 are programmed by TE.
  - The XTEA key for encrypted app blocks is believed to be at 0xD0 (community).

## Community mods
- op1repacker: data-only (DB rows, SVGs). It explicitly cannot change code.
- iter/filter can also be used on stock firmware by loading a patch .aif that references them (community, op-forums). This gives a way to exercise iter without reflashing.
- Libraries found in the firmware: SQLite3, YAFFS2, libaiff, Box2D, libjson.
- **No one has published a code patch to OP1_vdk.ldr, a custom engine, or a decrypted/annotated disassembly.** As of 2023, TabascoEye called the LDR round-trip unsolved.
- The RTOS is presumed to be VisualDSP++ VDK (from the file name). **Hypothesis**.
- No TE SDK or source exists. TE has historically tolerated mods.

## Links
- https://github.com/op1hacks/docs · https://github.com/op1hacks/op1repacker · https://github.com/op1hacks/op1-fw-archive
- https://github.com/Tolsi/op1dumps · https://github.com/tabascoeye/op1-decryptor · https://github.com/op1emu/op1emu
- https://op-forums.com/t/custom-firmware-on-the-op-1/4283
- https://teenage.engineering/guides/op-1/original/te-boot
