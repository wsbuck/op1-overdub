# Project: Custom synth engine on the original OP-1

## Goal
Run my own synth engine (DSP) inside the stock Teenage Engineering OP-1 (original, 2011) firmware.
I'm not replacing the firmware. I'm patching it: hijack one existing engine's render path
(the hidden `iter` engine, ideally) and point it at my own code, while TE's code keeps handling
the UI, encoders, presets and voice allocation.

I own the device. This is a personal research project.

## Context: verify all of this, treat it as hypotheses
- The OP-1 uses an Analog Devices Blackfin DSP (I believe the BF52x family). Confirm the exact part.
- Firmware ships as a `.op1` file: a compressed archive of a code binary plus an asset filesystem
  (SVGs, presets, config), with a checksum. As far as I know there's no cryptographic signature.
- The community tool `op1repacker` (GitHub: op1hacks) unpacks and repacks images and enables hidden
  features (`iter` synth, `filter` effect) by changing config flags, which means that code is already
  compiled into the firmware.
- The update path: power on holding COM to enter TE's bootloader, connect USB, the OP-1 mounts as a
  disk, drag the `.op1` file onto it. That bootloader is believed to survive firmware updates, so a
  bad image can be recovered by reflashing stock.
- The code binary is probably in Blackfin LDR format (blocks of "load these bytes at this address").
- Hypothesis: engines are registered in a table of function pointers (init / render / param / UI).
  Tracing the flag that enables `iter` may be the fastest route to that table.
- Render loops should be dense with fixed-point multiply-accumulate instructions (e.g. `A0 += R0.L * R1.L`).
- I also have a Daisy Seed (STM32H750), which I may use to prototype DSP first.

## Hard rules
1. **Never produce an image that writes to the bootloader region.** Before any flash, list exactly which
   address ranges the image touches and confirm none of them overlap the bootloader. If you can't
   rule that out, stop and tell me.
2. **You don't flash anything. I do.** For every image you build, give me: what changed, the expected
   result, what failure looks like, and the recovery step (COM menu, reflash stock).
3. **Keep a pristine copy of stock firmware** with its hash recorded. Never modify it in place.
4. **Don't commit or redistribute TE firmware or TE-derived binaries.** Add them to `.gitignore`.
   Commit only my tools, scripts, notes and my own source code.
5. **Separate facts from guesses.** Every claim in the notes is marked **Confirmed** (with evidence:
   address, disassembly, test result) or **Hypothesis**.
6. **Small steps.** One change per test image. If something fails twice, stop and reassess rather
   than stacking guesses.

## Phases. Stop at the end of each one and report to me before continuing.

### Phase 0: Setup and research
- Set up the repo: `firmware/` (gitignored), `tools/`, `notes/`, `src/`.
- Get the latest official firmware for the original OP-1, record its hash, and find and evaluate
  `op1repacker`.
- Find the current state of Blackfin tooling: Ghidra processor module support (official or community),
  IDA support, a working compiler (ADI CrossCore Embedded Studio, or a usable `bfin-elf-gcc`). Recommend
  one disassembler and one compiler, with reasons.
- Search for prior work: OP-1 teardowns, chip IDs, JTAG pads, any reverse-engineering of the firmware
  format or engines. Summarize what exists and what doesn't.
- Report: tools chosen, prior art, and anything that contradicts the context above.

### Phase 1: Extract and map
- Unpack the image. Write `tools/ldr_parse.py` to list every LDR block: target address, size, flags.
- Produce `notes/memory-map.md`: which blocks go to on-chip L1 memory and which to SDRAM, where the
  bootloader is believed to live, and candidate unused regions for my code.
- Load the image into the disassembler at the correct addresses, and check the load is right by
  confirming code references resolve.
- Report: memory map, confidence level, open questions.

### Phase 2: Find the engines
- Find strings for engine names (`fm`, `cluster`, `digital`, `dimension`, `phase`, `pulse`, `string`,
  `dna`, `iter`, ...) and the code that references them.
- Trace the `iter` enable flag to find where engines are registered.
- Find the audio DMA interrupt handler (SPORT) and walk up to the per-voice render calls.
- Document the render interface as far as you can: arguments, buffer format, sample count,
  fixed-point format, where parameters and voice state come from, the calling convention.
- Produce `notes/engine-interface.md`.
- Report: the interface, confidence for each field, and a proposed hook point.

### Phase 3: Proof-of-life hook (the go/no-go point)
- Build the smallest possible patch: redirect `iter`'s render to my code that outputs **silence**.
  Next test: a fixed **sine wave**.
- My code goes in as an added LDR block in a region shown to be unused, compiled for a fixed address
  with no standard library, using TE's calling convention.
- Repack with a correct checksum. Hand me the image with the flash and test checklist from rule 2.
- If silence and sine both work, the project is viable. Write up how to continue.
  If not, write up what blocked it and whether it can be overcome.

## Working style
- Keep `notes/LOG.md` with dated entries: what you tried, what happened, what's next.
- Prefer scripts over manual steps, so everything can be reproduced from the stock image.
- Ask me before installing large toolchains or anything paid.
- Start with Phase 0.
