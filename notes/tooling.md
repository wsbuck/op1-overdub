# Tooling (Phase 0 evaluation, 2026-10-06)

## Recommendation
- **Disassembler: Ghidra 12.1.3 + `geepot/ghidra-blackfin`** (free).
  - It is the only option with a decompiler and DSP/MAC p-code.
  - Risk: it is 2 weeks old and unproven, so it gets cross-checked against objdump.
  - `sualk/ghidra-blackfin` is the fallback. Its disassembly is mature and it has an LDR loader, but its DSP p-code is weak.
  - We'll load from our own LDR→segments script, not from either project's loader.
- **Cross-check: GNU objdump** — `/opt/homebrew/opt/binutils/bin/objdump -D -b binary -m bfin`. Already installed (2.45.1, `--enable-targets=all`). **Confirmed** (`objdump -i` lists bfin).
- **Compiler: ADI's free `bfin-elf` GCC 4.3 toolchain (2014R1-RC2)** in an x86_64 Docker container (≈66 MB tarball).
  - The freetribe (Electribe 2) project uses the same setup.
  - Fallback: a native build of binutils + GCC 15/16 `--target=bfin-elf`. bfin is still upstream, but it uses old reload and may be dropped after GCC 16.
- Rejected:
  - CCES: v3 dropped Blackfin; 2.12.1 is the last with it, but it is paid, 0.7–0.9 GB and x86 only.
  - IDA: no Blackfin support.
  - Binary Ninja: paid; plugin from 2022.
  - r2/rizin/Capstone: no Blackfin.

## ABI notes (GCC bfin-elf vs ADI compiler) — from docs, not yet verified on this binary
- Arguments: R0–R2. The caller reserves a 12-byte arg area. Return value in R0; struct-return pointer in P0.
- Callee-saved: R4–R7, P3–P5, FP, SP.
- L0–L3 must be 0 on entry and exit.
- Leading-underscore symbol names in both.
- Pitfalls:
  - ADI `double` is 32-bit by default; avoid `double` across the boundary.
  - Don't leave a hardware loop (LC0/LC1) armed on return.

## Other relevant projects
- `op1emu/op1emu` (@3b926b1, 2026-09): WIP OP-1 emulator on the gdb bfin simulator.
  - It boots `te-boot.ldr`. It has no OTP/XTEA handling.
  - Possible future test bench before flashing.
- `tabascoeye/op1-decryptor`: XTEA decrypt of BFLAG_CALLBACK blocks (needs key).
- `Tolsi/op1dumps`: NAND dump, OTP dump, board photo with JTAG pads, ECC tools, patched te-boot.
