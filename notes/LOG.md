# Log

## 2026-10-06 — Phase 0 start
- Initialized repo: `firmware/` (gitignored, TE binaries only), `tools/`, `notes/`, `src/`, `third_party/` (gitignored).
- OP-1 connected in normal disk mode: USB mass storage "OP-1 Disk", FAT16, 402.7 MB
  (album/, drum/user, synth/user, tape/). **Confirmed** (`diskutil info`). This is not the update/bootloader mode.
- Started research: stock firmware + op1repacker, Blackfin tooling, OP-1 hardware/prior RE.
- Downloaded stock 246 (latest) and 245; recorded hashes; files are read-only. CRC-32 container format verified by recomputation (see firmware-format.md).
- Found that `te-boot.ldr` ships inside every image and can rewrite the NAND boot blocks. New rule: every built image must carry te-boot.ldr byte-identical to stock.
- Found that 46 of 801 blocks in OP1_vdk.ldr (80 KB, all L1 SRAM) are XTEA-encrypted. All SDRAM content is plaintext.
- Tooling chosen (tooling.md); prior art summarized (prior-art.md). Cloned references into third_party/: op1repacker@390b18e, op1dumps@a353df4, op1-decryptor@2524f94, op1emu@3b926b1.
- Next: user decisions (key, installs) → Phase 1.

## 2026-10-06 — Phase 1: extract and map
- Installed (user-approved):
  - JDK 21 and Ghidra 12.1.3 + geepot/ghidra-blackfin v1.1.0 in `~/tools`. Built the decompiler natively for arm64 (`make ghidra_opt ARCH_TYPE="-arch arm64"`), since Ghidra ships no mac_arm_64 decompiler.
  - Docker image `op1-bfin` (`tools/docker/Dockerfile`) with ADI bfin-elf GCC 4.3.5. It includes `bfin-elf-run` (a simulator) and `bfin-ldr`.
- Tools:
  - `tools/ldr.py` (parse/serialize, round-trips both LDRs byte-for-byte).
  - `tools/ldr_parse.py` (list/summary/extract).
  - `tools/ldr2elf.py` (LDR → Blackfin ELF for Ghidra/objdump).
  - `tools/op1pack.py` (unpack/build). A no-op rebuild reproduces the stock tar byte-for-byte. It refuses to touch te-boot.ldr.
  - `tools/decompile.sh` + `tools/ghidra_scripts/Decompile.java`.
- Mapped the boot chain from te-boot's disassembly:
  - The ROM boots te-boot's init DXE from NAND block 0.
  - Without COM held, the init DXE redirects the ROM to boot OP1_vdk.ldr from NAND block 4, with an XTEA callback.
  - Updates write OP1_vdk.ldr raw to NAND blocks 4+.
  - te-boot is rewritten only if its version string differs or the file name contains `FRC`.
  - A `.KEY` file name triggers OTP programming.
  See memory-map.md.
- Verified the app load: 97.3% of call targets land on function starts; string xrefs resolve.
- Found the CPLB tables: all 64 MB of SDRAM is mapped. Still unknown which unloaded SDRAM ranges are used at runtime.
- Next: Phase 2 — find the engine table and iter's render code. Check whether it's in plaintext SDRAM.

## 2026-10-06 — Test image 0: REPACK.OP1 (pipeline test, no code change)
- Built with `tools/op1pack.py build firmware/stock/op1_246.op1 firmware/images/REPACK.OP1`.
  - sha256 b30cfb2b478c3f639ab39cbefb929d97480f0dcac33b4551d5b5129148b76a2b, 13,052,053 B.
- Verification:
  - The decompressed tar is byte-identical to stock 246, checked with an independent decoder (`xz --format=lzma`).
  - CRC-32 OK.
  - te-boot.ldr is identical to stock.
- The only difference from stock is the LZMA stream (our encoder).
- Result (user, 2026-10-06): **Confirmed working.** The update screen matched the expected lines, including "te_boot skipped". The OP-1 boots and works normally. Pack pipeline validated on device.

## 2026-10-06 — Phase 2: find the engines
- Seeded Ghidra functions at all 3,685 direct call targets (`tools/ghidra_scripts/SeedFunctions.java`). Ghidra created 1,108 new functions.
- Assert strings give source paths: `src\dsp\Iter.cpp`, `producer_factory.cpp`, `Core.cpp`, `VoiceCtl.cpp`, `bf\audio.cpp`.
- Engine id table (14 = iter), factory `FUN_01a1b7d4`, iter instance getter `FUN_019b9bf0` (objects in L1 scratchpad), vtable `0x019103CC`.
- iter render is `0x019B99D4` (438 B, plaintext SDRAM): mono int16 output, 128 samples per call, params int16[4], pitch increments from `0xFF801950`.
- The voice loop that calls render is in encrypted L1. The vtable slot it uses (+0x14) can't be read there; it's inferred.
- The iter enable row lives in `content/op1_factory.db` (`synth_types`).
- Full write-up: engine-interface.md.
- Next: Phase 3 proposal (in-place render overwrite). Awaiting user go-ahead.

## 2026-10-06 — Phase 3, test image 1: ITER.OP1 (enable iter, DB only)
- `tools/enable_iter_db.py` adds `synth_types` row `(11,'iter','[1516, 16704, 0, 15168, 0, 0, 0, 0]')` to `content/op1_factory.db`. Integrity check ok; page size and journal mode unchanged.
- `op1pack.py build … --replace content/op1_factory.db=…` → `firmware/images/ITER.OP1`.
  - sha256 9a170568340ca8a4915f296b9a581142bfb8ad284b11d15db580fb0b60e5a025, 13,052,330 B.
- Changed vs stock: only `content/op1_factory.db`. OP1_vdk.ldr and te-boot.ldr are identical (verified by the tool).
- Result (user, 2026-10-09): **all works**: ens listed and selectable as synth, drum and master FX; gain, the master Haas shift, preset save/load and the stock FX all fine.
- Prepared (not yet handed over, waiting on image 1):
  - `SILENCE.OP1`: sha256 27aba29c…1e4a. ITER DB + 16-byte render at 0x019B99D4.
  - `SINE.OP1`: ITER DB + 56-byte render at 0x019B99D4.
  - Both are hand-written assembly (`src/poc/*.S`), LC0 only. GCC's own output uses LC1, which isn't known to be safe at this call site.
  - Simulator (`src/poc/test/run.sh`, bfin-elf-run):
    - silence → 128 zeros per call, guard intact;
    - sine → 440.03 Hz at fs = 44.1 kHz, −12.0 dBFS peak, H3 −29.7 dB.
  - `tools/ldr_patch.py` inserts the block before FINAL and bumps the FIRST block's next-DXE argument (+32 B, +72 B). It refuses ranges outside stock plaintext blocks or overlapping te-boot/ROM live RAM.
- **Result, image 1 (user):** the update screen showed "Done!" and the expected lines, but **iter did not appear** in the Shift+T1 engine browser.
  - Navigation was correct (TE guide and op1repacker README agree on Shift+T1 plus the blue encoder).
- Diagnosis (static):
  - te-boot's tar extractor opens files with flags 0x241 (write | create | truncate), so the DB file was overwritten. **Confirmed**.
  - The DB header is compatible with stock (format 4, UTF-8, 1 KB pages; stock was written by SQLite 3.17). **Confirmed**.
  - The app loads `synth_types` at runtime for the synth browser (dispatcher branch, screen 0x53 → `FUN_01a02440`, which also walks the preset records and adds "sampler"). **Confirmed**.
  - Every engine shown in the browser has a `synth_presets` folder, including `sampler`, which has no `synth_types` row. iter has none.
  - **Hypothesis (medium):** the browser lists only engines that have a preset folder.

## 2026-10-06 — Test image 1b: ITER2.OP1 (iter row + 11 iter presets)
- `tools/enable_iter_db.py … --presets third_party/op1repacker/op1repacker/assets/presets/iter`, equivalent to op1repacker `iter` + `presets-iter`. Presets are stored as compact JSON like the factory ones.
- `firmware/images/ITER2.OP1`: sha256 a9fee6677b5d4f7c595f2f538bc67849684e58a9f6aeff2829f587b516d53c99, 13,052,487 B.
- Only `content/op1_factory.db` differs from stock (107,520 → 111,616 B).
- Result (user): **iter appears in the engine browser.** Confirms that on 246 the browser lists only engines with a `synth_presets` folder. Baseline for the code tests.

## 2026-10-06 — Test image 2: SILENCE.OP1 (rebuilt on the ITER2 DB)
- Same DB as ITER2 (row + presets). OP1_vdk.ldr has one added block: 16 B at 0x019B99D4–0x019B99E3 (`src/poc/silence.S`).
- User observations on ITER2 (stock iter code):
  - **Pitch follows the keys.** Consistent with `0xFF801950` being per-sample phase increments.
  - **The encoders seem to do nothing.** Open question: render reads params[0..3], so either the UI doesn't send knob values to iter, or the effect is subtle.
- Only the app differs from ITER2.
- **Result (user): pure silence on iter.** Proof-of-life step 1 passed. **Confirmed on hardware:**
  - The boot ROM accepts an extra plaintext block inserted before FINAL, with the FIRST argument adjusted.
  - iter render is vtable slot +0x14 at `0x019B99D4`.
  - Our code meets the calling contract: no crash; other engines unaffected.
- Side observation: the "iter" preset loads TE's hidden `filter` FX (fx_type "filter", not in `fx_types`). Presets can reference unlisted FX.

## 2026-10-06 — Test image 3: SINE.OP1
- ITER2 DB + 56 B block at 0x019B99D4–0x019B9A0B (`src/poc/sine.S`). Simulator: 440.03 Hz at fs 44.1 kHz, −12 dBFS.
- sha256 c6d86c228efd18bed0301c2927d8c0d147cd5de6f4fabc91d59f8c032aac59ed.
- **Result (user): steady tone at exactly 440 Hz on every key.** Proof-of-life step 2 passed.
  - **Confirmed: fs = 44.1 kHz.** The 128-sample render block = 2.90 ms.
- **Phase 3 go/no-go: GO.** Custom code runs as iter's render inside stock firmware 246. See notes/NEXT.md.

## 2026-10-06 — Merged phase-1 into main; test image 4: PSINE.OP1 (key-tracked tone)
- Work now happens directly on `main` (user request).
- `src/poc/psine.S`: like sine.S, but the per-sample increment is `[_pitch_buf++]`.
  - `_pitch_buf` = 0xFF801950, defined via `--defsym` in build.sh. The simulator harness supplies an array instead, because bfin-elf-run doesn't map that L1 address.
  - 60 B at 0x019B99D4–0x019B9A0F.
- Simulator: C4 increments → 261.61 Hz, C5 → 523.34 Hz, −12 dBFS. The sine.S regression still gives 440 Hz.
- `firmware/images/PSINE.OP1`: ITER2 DB + this block.
- **Result (user): tracks the keys; intervals, octaves and chords are correct.** **Confirmed:** `0xFF801950` is a per-voice buffer of per-sample phase increments (2³² = 1 cycle), refilled for each voice before its render call.

## 2026-10-06 — Test image 5: CPULOAD.OP1 (CPU budget measurement)
- `src/poc/cpuload.S`: key-tracked tone (as psine) plus a burn of step × 20,000 cycles per render call (1-cycle NOP hardware loop).
  - Every 1,378 calls (all voices together) the step increments, and that call outputs a silent block (an audible tick).
  - The step wraps after 70, so an overload recovers by itself.
- Shared state is in iter's private buffer `_iter_buf` = 0x01DD4E18. Only iter code references 0x01DD4E18–0x01DD4F17 (render, the clear-on-select `FUN_019b9c64`, the static ctor), and there are 0 data pointers into it. It's zeroed when iter is selected.
- Reading the result:
  - tick spacing → voices rendered per block (4.0 s = 1, 0.67 s = 6);
  - ticks until breakup × 20,000 = cycles available per render call.
- Simulator (fast constants): ticks exactly every TICK_CALLS calls; tone 261.72 Hz on a gap-free segment; psine/sine/silence regressions pass.
- 148 B at 0x019B99D4–0x019B9A67.
- **Result (user):**
  - Ticks every ~2 s for ~25 ticks, then every ~4 s (27 ticks), then ~6 s (~20 ticks).
  - First crackle/sputter at ~25 ticks (~50 s); the sound then degraded to a square/"bit-crushed" tone.
  - No visible UI lag; knobs still responsive.
  - Clean again after the wrap (~70).
- **Interpretation:**
  - **2 s/tick = 689 calls/s = 2 render calls per block** while one key was held. PSINE showed 1 call = 128 samples per voice per block.
    - Hypothesis: voices stay rendered once used, because iter's slot +0x24 ("still sounding?") always returns 0.
  - Overload starts between step 24 and 25 → 2 × 24 × 20,000 = **0.96 M cycles/block OK, 1.0 M too much**.
    - At 400 MHz a block is 1.16 M cycles, so TE's own load in this light scenario is ~0.16–0.2 M/block.
  - Stretching intervals = audio blocks missed under overload. Graceful: no crash or watchdog; recovers.
- **Budget (Confirmed for this scenario):**
  - ~0.96 M cycles per 128-sample block for all render calls together.
  - For 6 voices: ~160 k cycles per voice per block (~1,250 cycles/sample/voice).
  - Design target: ≤ 50% (~600 cycles/sample/voice) to leave headroom for FX, tape and sequencers.
- Open: which voices are rendered while idle. Test with chords and repeated single notes (more voices → shorter tick interval).
- Background static analysis of the knob → params path: notes/params-path.md.
  - Encoders reach the slot's knob array `0xFF8013C4 + slot*16` the same way for all engines. **Confirmed**.
  - Render's R2 is set in encrypted bytes. Hypothesis: a per-voice, LFO-modulated copy of those knobs (caveat: possibly the envelope knobs).

## 2026-10-06 — Prepared test image 6: PARAMS.OP1 (held until CPULOAD is reported)
- `src/poc/params.S`: psine with level = params[0] and octave = params[1] >> 13. 72 B at 0x019B99D4.
- Simulator with harness params [1516, 16704]: 1046.7 Hz (C4 × 4), peak 379. As predicted.
- **Result, PARAMS.OP1 (user): the encoders change level and octave as expected.** **Confirmed:** render's params (R2) are the engine-page knobs; params[0] = blue, params[1] = green. The envelope-knob caveat in params-path.md is ruled out.
- User observation: holding chords eventually went buzzy, but a single key held afterwards gave a clean sine. So **releasing keys frees voices**; render calls per block follow the sounding voices. The 2 calls per block with one key remain unexplained (Hypothesis: a release tail, or one extra voice).

## 2026-10-06 — Test image 7: CENGINE.OP1 (first C engine via trampoline)
- `src/engine/`: portable C engine contract (`engine.h`) plus an asm trampoline (`tramp.S`) at iter's render slot `0x019B99D4`.
  - Trampoline saves/restores RETS, LC0/LT0/LB0, LC1/LT1/LB1 and L0–L3, zeroes L0–L3, and calls `engine_render(this+4, out, params, 0xFF801950)` (4th arg on the stack, 12 B arg area reserved).
  - The C code gets 32 B of per-voice state at `this+4`; it never sees the vptr.
  - `engine.ld` fails the link on any .data/.bss or on > 438 B. `-nostdlib` makes any libgcc/libc call a link error.
- **Confirmed (objdump):** GCC 4.3 `-O2` uses `LSETUP … LC1` and `I0` for the loop. The trampoline is needed.
- The engine is `params.S` rewritten in C (same behaviour), so this image tests only the toolchain + trampoline.
- Simulator (`src/engine/test/run.sh`):
  - Samples are **bit-identical** to `src/poc/test/run.sh params` (5120 samples, `cmp` clean).
  - ABI check (`abi_check.S`): sentinels in R4–R7, P3–P5, FP, SP, both loop register sets and L0–L3 survive the call (mask 0); the vptr is unchanged.
  - Mutation check: dropping the LC1 restore gives mask 0x200 (bit 9 = LC1), so the check works.
- 180 B at 0x019B99D4–0x019B9A87. `ldr_patch.py` changed only that range. te-boot.ldr identical to stock.
- `firmware/images/CENGINE.OP1`: ITER2 DB + this block. sha256 d3d9aecbe9ff7ab2fcfc8b776de9bf07cc3d929cd4d0e019b045b5e25e161787.
- Expected: same as PARAMS.OP1 (key-tracked tone; blue = level, green = octave).
- **Result (user): loads and works.** **Confirmed on hardware:** GCC-compiled C runs as iter's render through the trampoline; loop-1/L-register handling is safe at this call site.

## 2026-10-06 — Test image 8: CANARY.OP1 (free-SDRAM probe)
- `src/poc/canary.S`: four 1 MB regions (`_canary0..3` = 0x00800000, 0x01E10000, 0x02C00000, 0x03800000) are filled with 0x5AA5C33C by LDR FILL blocks.
  - Every render call checks 512 words per region at a rotating cursor (`_iter_buf[0]`); a full sweep takes 512 calls (~0.75 s with 1 key).
  - A mismatch sets the region's bit in `_iter_buf[4]`.
  - Clean: psine tone. Damaged: square wave, octave up = lowest damaged region index (0–3).
- Assembler: `Rn.H = CONST` takes the constant's high half (like symbols), so `.H`/`.L` both get the full 32-bit constant.
- Simulator (`--defsym REGION_BYTES=4096`, harness damages region 2 at call 25): tone until call 24, then a square at 4× pitch; mask 0x4; guard intact. psine/cpuload/params regressions still run.
- 276 B at 0x019B99D4–0x019B9AE7. te-boot.ldr identical to stock.
- `firmware/images/CANARY.OP1` sha256 973947c3eb9441f29bee416206e607574801f799712bb47821dbd9636af7ef6c.

## 2026-10-06 — Test image 9: LOGTEST.OP1 (FSK logger, fixed payload)
- Goal: read results off the device as data, by recording iter to tape and decoding the audio on the computer, instead of judging tones by ear.
- `src/poc/fsk.inc` (macro `FSK n`): one bit per render call (128 samples).
  - Bit 0 = 8 square cycles per block (2756 Hz); bit 1 = 16 cycles (5513 Hz); −12 dBFS. Whole cycles, so each block is one symbol.
  - Frame = sync 0x1ACFFC1D, n payload words, then a Fletcher check word (a = 1, b = 0; per word a += w, b += a; check = b). All 32-bit, MSB first.
  - For n = 10 a frame is 384 bits ≈ 1.11 s.
  - The bit position is per voice at voice +0x14, which iter's note-on clears (engine-interface.md), so each note starts a fresh frame.
- `src/poc/logtest.S`: the encoder plus a constant 10-word payload in canarylog's layout. 168 B at 0x019B99D4–0x019B9A7B.
- `tools/fsk_decode.py`: reads AIFF/AIFC (own chunk parser; Python 3.13+ has no aifc) or the simulator's text output.
  - Block alignment: correlation with the exact square templates, scored over loud blocks. An energy-based score was tried first: it is nearly flat under small shifts, and in noise it landed up to 4 samples off.
  - Bits: energy at 8 vs 16 cycles per block. The sync search tolerates ≤ 3 wrong bits; the check word decides.
- Simulator:
  - `run.sh logtest "" -DCALLS=1000` → 2/2 frames decode to the exact payload, offset 0.
  - `src/poc/test/fsk_roundtrip.py` writes the samples as a stereo AIFF at 0.4× level, with noise σ = 300 and 4410–4537 samples of leading silence. The decoder finds the exact offset and every frame.
- psine output is bit-identical to before the harness/run.sh changes; the canary sim behaves as before.
- `firmware/images/LOGTEST.OP1`: ITER2 DB + this block. sha256 9c6140b880871e6da34370e4550e04c640a2ec4af436e4e6d85c0868d1045a46. te-boot.ldr identical to stock.
- How to record:
  - Select iter, FX off, LFO off, attack minimal, sustain max, level about −12 dB.
  - Hold exactly one key for ≥ 4 s while recording to tape at normal speed.
  - Then in disk mode copy the tape track (or album side_a.aif) and run `tools/fsk_decode.py <file> -v`.
- Expected: ≥ 2 frames decode to `00000005 00812340 00000000 00000000 00000000 02c00004 deadbeef 00000000 00000000 000004d2`.
- Failure modes:
  - no frames: check the "symbol match" figure; < ~0.8 means the audio path distorts the symbols (filter FX, envelope or LFO, tape speed);
  - frames fail the check: two sounding voices are mixed, or the level clips.
- **Result (user capture `firmware/captures/logtest_side_a.aif`, album side A, 11.64 s, 44.1 kHz stereo): PASS after a decoder fix.**
  - The first decode found 0 frames. The 128-sample alignment had a symbol match of 0.784, and 89% of blocks read as bit 1.
  - The capture is **2× time-compressed**. Block 640 is a pure fs/4 tone (`a, b, −a, −b`, period 4), where the encoder's symbols have periods 8 and 16. The strongest bins are 16 and 32, not 8 and 16. **Confirmed** (data).
  - Decoding with 64-sample blocks gives **16 frames, all exactly the test payload**, symbol match 0.952, 1 failed sync. **Confirmed:** render → tape/album → AIFF → decoder works end to end.
  - Cause of the 2×: **Hypothesis.** Tape or album audio may be stored at 22.05 kHz but labelled 44.1 kHz, or the tape was played back at double speed when bounced to the album. Check: loud audio is 9.3 s in the file, so the key would have been held ~18.6 s if the cause is the storage rate.
  - Decoder fix: `find_timing` tries 128-, 64- and 256-sample blocks and keeps the best symbol match. `fsk_roundtrip.py` now also tests a 2×-decimated copy.

## 2026-10-06 — Prepared test image 10: CANARYLOG.OP1 (held until LOGTEST is reported)
- `src/poc/canarylog.S`: canary.S's checks (same regions, fill, sweep and FILL blocks) with fsk.inc output instead of tones.
  - On a region's first damage, a second pass scans the checked window downwards and records the lowest mismatch address and the value found there.
  - Log = `_iter_buf[4..43]`: mask, (address, value) × 4 regions, completed sweeps.
- Simulator (`REGION_BYTES=4096 CHECK_WORDS=4`, `-DCALLS=4000 -DSILENT_VOICE`, so a second voice renders each block):
  - 9 frames decode with mask 0x4 and region 2 first damaged at 0xb89c (the harness's `&canary2[700]`), value 0.
  - The one rejected frame is the first: the mask changed mid-frame. Sweeps advance 3 per frame (both voices step the canary).
- Known limitation: the sweep count changes about every 256 blocks on device. Frames during which it changes fail the check, so expect ~25% rejected. The decoder groups frames ignoring sweeps.
- 388 B at 0x019B99D4–0x019B9B57, plus the same four 1 MB FILL blocks as CANARY. te-boot.ldr identical to stock.
- `firmware/images/CANARYLOG.OP1` sha256 280fca22fb97b331890caed3ac26b40892de56e5815e29b55010a0dab90c31c2.
- **Result (user capture `firmware/captures/canarylog_side_a.aif`, sha256 d67e6f88…32f6, album side A, 21.8 s):**
  - Procedure: 10–15 min exercising every mode (all synth engines, FX/LFO, drum kits and sequencers, tape record/overdub/reverse/loop, synth and drum sampling, album bounce, preset saves), then select iter and log to tape.
  - Decode: 2× time-compressed again (64-sample blocks), symbol match 0.967. 26 frames pass, 9 fail; all 26 agree.
  - mask 0x8 with 25 sweeps after selecting iter: **regions 0 (`0x00800000`), 1 (`0x01E10000`) and 2 (`0x02C00000`) clean**, 1 MB each, every word checked 25 times after the exercise.
  - **Region 3 (`0x03800000`) damaged**: already in the first frame; lowest mismatch in the first window is the region's first word, value 0. Something zeroes or uses memory from `0x03800000`. Ruled out.
  - Caveat: the check sees only damage that persists. Memory a feature allocates but doesn't write, or that a feature we didn't exercise uses, would not show.

## 2026-10-06 — L1 code decrypted
- The XTEA key is OTP page 0xD0 from the public op1dumps OTP dump. It decrypts all 46 encrypted blocks of stock 246.
  - **Confirmed** offline: zero-byte rate of the decrypted bytes matches the plaintext; illegal instructions in L1 instr go 3,083 → 2. Details in firmware-format.md.
  - Same key on a different unit and firmware → per model, not per device. No device access needed.
- `tools/ldr.py`: `otp_key`, `decrypt`, `encrypt_payload` (verified against tabascoeye's C tool; encrypt round-trips).
- `tools/ldr2elf.py` now decrypts by default when the dump is present (`--raw` to skip).
- New Ghidra program `OP1_vdk_246_dec.elf`, seeded from 3,819 call targets (134 new, 66 in L1 instr). Log: `firmware/ghidra/analyze_dec.log`.
- First look: L1 instr has many `[Px + 0x14]` loads followed by indirect `CALL (Px)` — candidates for the voice loop's render call.
- Next: find the voice loop and check the inferred facts (vtable +0x14, LC0/LC1 use, 2 render calls per block).

## 2026-10-06 — Test image 11: RELOC.OP1 (CENGINE relocated to free SDRAM via the vtable)
- Only one reference to iter's render exists anywhere in the decrypted image: vtable slot `0x019103E0` (iter vtable `0x019103CC` + 0x14). No direct CALLs, no `.H/.L` pairs building it.
- `src/engine/build.sh 0x01E10000 firmware/build/reloc 0x100000`: size limit is now a build argument (`engine.ld` `SLOT_SIZE`, default 438). The 180 B output is byte-identical to CENGINE's (all PC-relative), so this image tests only the relocation.
- `tools/ldr_patch.py`:
  - `FREE` allowlist = canary regions 0–2 (1 MB each). Blocks must lie inside stock plaintext or one FREE region.
  - `--pointer ADDR=OLD:NEW` rewrites one stock plaintext word; refuses unless it holds OLD and NEW points into a block of this patch.
  - Negative checks: region 3, wrong OLD value, and NEW outside our block are all refused.
- Writes vs stock: `0x01E10000–0x01E100B3` (180 B, FREE region 1) and `0x019103E0–0x019103E3` (pointer `0x019B99D4` → `0x01E10000`). iter's original render is left intact. te-boot.ldr identical to stock.
- `firmware/images/RELOC.OP1`: ITER2 DB + these blocks. sha256 8a88d6c50fa4d17091c1231326ecd4a78644d78285c7470cfcd6a08b09f0b31b.
- Expected: same as CENGINE (key-tracked tone; blue = level, green = octave). Stock iter sound instead means the vtable slot is not the call path.
- **Result (user): works, sounds the same as CENGINE.** **Confirmed on hardware:** the voice loop calls render through vtable slot `0x019103E0`, and code placed in FREE region 1 by an LDR block executes. The 438 B limit is gone.

## 2026-10-06 — Test image 12: VA1.OP1 (first real engine: 2-osc virtual analog)
- `src/engine/va.c`: 2 × PolyBLEP (saw → square → 10% pulse) → trapezoidal ZDF SVF low-pass (Simper) → cubic soft clip. Audio Q24, coefficients Q31 via `__builtin_bfin_mult_fr1x32x32` (a 64-bit multiply calls libgcc `__muldi3`), block-rate restoring divisions. `va_tables.h` (cutoff → g, Q28) from `gen_va_tables.py`.
  - Knobs: [0] blue wave, [1] green osc-2 detune 0 → +1 semitone (squared), [2] white cutoff 20 Hz → 14 kHz, [3] orange resonance Q 0.5 → 10 with input gain 1 → 0.25.
  - State: phases at bytes 0/8 and filter at 16/20, so TE's note-on resets both.
- The old CENGINE engine is now `src/engine/params.c`; `ENGINE=<name>` picks the source in `build.sh` and `test/run.sh`. `ENGINE=params` regression still bit-identical to `params.S`.
- `src/engine/test/engine_main.c` (generic driver: knobs, pitch/glide, CYCLES per call, guard) + `va_check.py` (106 cases in one container):
  - saw 1/n harmonics; square H2/H4 < −109 dB; 10% pulse H10 −37 dB; detune partial at 466.2 Hz; LP slope −14.7 dB/oct above 1 kHz; resonance +13.8 dB net at cutoff; aliasing −40.8 dB (C7), −36.7 dB (C8).
  - Stable at every cutoff × resonance extreme × wave × pitch and a 30 Hz → 12 kHz glide at max resonance; peak 26,575, nothing at the rails.
  - First run found two bugs: detune 256× too small; no resonance gain compensation (+28 dB peak, hard clipping). Fixed.
  - CYCLES (sim): max 33,373 per call = 261 per sample, ~21% of the per-voice budget.
  - ABI check with va linked: mask 0.
- 1,580 B at `0x01E10000` (FREE region 1) + vtable pointer, as RELOC. te-boot.ldr identical to stock.
- `firmware/images/VA1.OP1` sha256 21467489c99d6f12963f7df745b89bde295ecd2b8a0994e3c87a9a24edc06b17.
- **Result (user, 2026-10-07): working correctly.** **Confirmed on device:** all four knobs as designed, so params[2] = white and params[3] = orange.

## 2026-10-07 — Test image 13: SLOT1.OP1 (16th synth engine id `va`)
- Design and audit: notes/engine-slot.md. Ids 8–10 are drum-side, so the new engine is id 15.
- `tools/build_slot.py` + `src/slot/slot.S`: 1,380 B at `0x01E20000` (FREE region 1) with the factory case-15 stub (iter's case + vptr swap to our vtable copy) and relocated 16-entry name, factory, ScreenId and knob-range tables; 24 checked halfword patches (`ldr_patch.py --half`, new) repoint TE's code. TE-derived table contents are generated into `firmware/build/slot/` (not committed).
- VA engine (unchanged, 1,580 B) at `0x01E10000`. iter's vtable is **not** patched: iter is stock again.
- `tools/add_engine_db.py`: `synth_types` row 12 `va` (defaults `[0, 6000, 20000, 9000, …]`) + 4 presets (envelope/FX/LFO copied from iter presets at build time). Integrity ok.
- `tools/xrefs.py` (new): code references to an address range from `.H/.L` pairs.
- Audit (subagent, spot-checked): no further patches needed; byte-flag tables already have entry 15 = 0.
- `firmware/images/SLOT1.OP1` sha256 902edee9b11911468f39cae3db995a0ef64d95fa65ca19d8bdec33379bd23c0e. te-boot.ldr identical to stock.
- **Result (user): does not boot.** Recovered by reflashing VA1.OP1 (COM updater unaffected, as designed).
- Likely cause (**Hypothesis**, strong): SLOT1 carried 24 **2-byte** LDR blocks, some only 2-byte aligned. All 801 stock blocks are DMACODE 6 (32-bit DMA), start 4-byte aligned, and all but one are a multiple of 4 long; every earlier test image's blocks were too. A 2-byte block under 32-bit DMA gives a word count of 0, which Blackfin DMA treats as 65,536 → ~256 KB of TE code overwritten at boot.
- Fix: `ldr_patch.py` merges `--half` patches into aligned 32-bit words (other bytes from stock), pads FREE-region blocks to 4, and refuses any block not 4-byte aligned and sized. Rebuilt: 15 blocks, all aligned; a load simulation of stock vs patched memory shows exactly the 24 intended halfwords changed in TE code plus the two FREE-region spans.

## 2026-10-07 — Test image 14: SLOT2.OP1 (SLOT1 with aligned patch blocks)

- Same code, tables, DB and presets as SLOT1; only the LDR block layout differs.
- `firmware/images/SLOT2.OP1` sha256 6514c2e90be26d472e4f9047693f7af1fcd92a812cee853b5978faa5b39e0e1d. te-boot.ldr identical to stock.
- If it still fails to boot, bisect: (a) only the two FREE-region blocks, no TE patches; (b) + name/factory/ScreenId; (c) + range table.
- **Result (user, 2026-10-07): works.** **Confirmed on device:** a 16th synth engine id is possible. `va` (id 15) boots, is listed and plays our VA engine alongside all 15 TE engines; iter is stock again.
- This also supports the SLOT1 diagnosis: the only difference is the LDR block layout (sub-word blocks break the boot).

## 2026-10-07 — Test image 15: SLOT3.OP1 (engines va = 15 and wt = 16)
- `tools/build_slot.py` now takes `src/slot/engines.json` (N engines: source, address, presets) and builds the engines, slot tables, LDR and DB in one step. Bounds become 15 + N.
- Ids ≥ 16 overflow the six 16-byte per-engine flag tables, so they are relocated too (copies with zeros for our ids; 11 `.L/.H` pairs repointed). `0x01a27e8c` reads entry 8 directly and stays on the stock table. Pair finder decodes the `.L/.H` instructions and checks both immediates against stock.
- With only va, the build is byte-identical in TE memory to SLOT2 except the 22 flag-pointer halfwords (memory diff).
- `src/engine/dsp.h`: shared qmul/udiv/recip/SVF/soft clip. va.c refactored onto it; output bit-identical in all 106 va_check cases.
- `src/engine/wt.c` + `gen_wt_data.py` (generated at build time, linked as `_engine_data` by build.sh): 8 banks (analog, pwm, vowel, organ, sync, fold, bell, digital) × 12 frames × 9 band-limited levels (harmonics 256 >> L, 8× oversampled), 792 KB. Level chosen per block one octave below the alias limit, crossfaded; 4-point Hermite lookups; frame morph; unison (+30 cents); SVF LP at Q 0.7.
  - First design (4× tables, linear interpolation) measured −24 dB aliasing on the organ bank: linear-interpolation images of strong top harmonics (−19 dB at 4×, numerically). 8× + Hermite: −44 dB.
- `test/simrun.py` (shared runner, 8 sims in parallel) + `wt_check.py` (341 cases): aliasing −40 to −60 dB per bank at C6/C8; 60 Hz → 6 kHz sine glide envelope steps ≤ 0.06 dB; nothing at the rails or silent; CYCLES max 82,633 per call = 646 per sample with unison (~52% of the per-voice budget; device SDRAM table reads may cost more than the sim shows). ABI check with wt linked: mask 0.
- Writes: va 1,576 B at `0x01E10000`, slot 1,676 B at `0x01E20000` (region 1), wt 795,252 B at `0x02C00000` (region 2); 46 TE halfwords (memory diff confirms exactly 46). App LDR 2,970,744 B (23 NAND blocks of 128). Image 13,656,181 B (updater limit 15,728,640).
- `firmware/images/SLOT3.OP1` sha256 bc009dbb93fc682c1ecf4d3472076518488c01d3fd2ce9e1f9eb635be7ae8169. te-boot.ldr identical to stock.
- **Result (user): boot stops on an assert screen**: `..\src\db2\patch.cpp:616 synthSynthes_preset_count<=kMaxSynthSynthes_presets` plus ~8 addresses.
  - `FUN_01a0567c` loads `synth_presets` into a fixed array of 0x38-byte entries at `0x01DDDB68`; the count is checked against **176** (`R1 = 0xb0` at `0x01a0582a`). patch.cpp:632 limits user patches to 100.
  - Counted without sampler presets (**Hypothesis** from counts): SLOT2 had 184 total / 175 non-sampler and booted; SLOT3 has 192 / 183.
  - The engine code never ran; the failure is in the DB only. The limit guards a fixed array, so it is not raised.
- Fix: `build_slot.py --db-trim iter=1` keeps one iter preset (enough for the browser to list iter) and refuses any DB over 176 non-sampler presets. Budget now: stock 160 + iter 1 + va 4 + wt 8 = 173; **3 presets left** for future engines unless TE folders are trimmed.

## 2026-10-07 — Test image 16: SLOT4.OP1 (SLOT3 with iter trimmed to 1 preset)
- LDR byte-identical to SLOT3; DB: iter presets 11 → 1 (173 counted presets). Integrity ok.
- `firmware/images/SLOT4.OP1` sha256 0fb861d12959316a883b1bd1e95e3cb2d3abd0b70faed295a5bcdeb9e10f4026. te-boot.ldr identical to stock.
- **Result (user): works.** **Confirmed on device:** two extra engines (ids 15 and 16), the flag-table relocation, an 800 KB engine in FREE region 2, and the 176-preset limit diagnosis.

## 2026-10-07 — Test image 17: SLOT5.OP1 (wt CPU fix)
- User report on SLOT4: after changing screens and playing at once, or with ~5-note chords, the sound distorts; a following single note is still distorted, and it clears after about a second.
- Diagnosis (**Hypothesis**, strong): CPU overload. CPULOAD's ~1,250 cycles/sample/voice at 6 voices is the **overload point** (0.96 M cycles/block), not a budget, but `simrun.py` used it as the limit. wt with unison measured 644 in the sim, more than the ~600 design target, and SDRAM table reads cost more on device. Screen transitions add UI load. Voices keep rendering through their release tails, so the load (and distortion) persists ~1 s.
- `simrun.py`: CPU_BUDGET now 600 × 128 per call.
- wt profile (sim): cost is independent of pitch; unison adds ~280/sample. Pre-blending frames before the Hermite interpolation did not help (646 → 644).
- Fix: linear interpolation on the 8× levels (2 reads per frame instead of 4, no Q31 multiplies) and a 16-bit fractional multiply for the deltas (max adjacent table delta 18,516). **344–350 cycles/sample with unison** (~190 without), about half. Aliasing worst −36.9 dB (sync at C8), other banks −40 to −48 dB. All wt checks pass; ABI mask 0.
- The sim does not model multiply latency (16-bit vs 32-bit multiplies count the same), so device gains may differ.
- LDR: wt 794,324 B; DB identical to SLOT4.
- `firmware/images/SLOT5.OP1` sha256 9b57d02ce68c3adbfad434cffa36a81e9e334fef6ed6eab9f1005c727e427a82. te-boot.ldr identical to stock.
- **Result (user): works.** Screen changes, 5–6 note chords and unison no longer distort. **Confirmed on device:** the distortion was CPU overload; ~350 cycles/sample/voice (sim) is safe.

## 2026-10-07 — Screen research; test image 18: SCREEN1.OP1 (calibration pattern on our engines' screen)
- Research summary and API: notes/screen.md (two background agents; key call shapes spot-checked).
- `tools/ldr.py`: trailing partial chunks (≥ 8 B) of encrypted blocks are encrypted too (30 of 46 blocks); op1-decryptor and our port skipped them. Fixed; e.g. `0xFF8017B4` now reads 320, 240, 320, 156. Illegal instructions in decrypted L1 code: 2 → 0. ELF, disassembly and Ghidra program regenerated.
- `src/slot/ui.c` + `_ui_update` (slot.S) built into the slot block by `build_slot.py` (TE functions via `--defsym`; linker refuses `.bss`). Pointer patch `0x01910418`: `0x019B9EA4` → `_ui_update` (`0x01E20000`).
  - Self-review fix before building: the first draft compared the engine id in R0 and so passed a clobbered page pointer to iter's update.
- Pattern for engines 15/16: white 1-px border 4 px inside 320 × 156; a palette-red line and a palette-blue line (width 2) labelled RED/BLUE in 8×8 text (0xF800 / 0x001F); four 6-px bars following knobs 0–3 in blue/green/white/orange; text "VA"/"WT" + the four knob values.
- DB identical to SLOT5; engines unchanged.
- `firmware/images/SCREEN1.OP1` sha256 35df3e3c35e12abc046402f1a6e2341981deccbfd947833a76f7423bd1ee9540. te-boot.ldr identical to stock.
- **Result (user, photo):** boots; fm and iter screens normal; sound unchanged.
  - Lines: positions as designed, palette red/blue correct (**Confirmed:** palette byte order works as passed). Top, left and right border visible; **bottom border (y = 152) not visible** (photo: visible height ≈ 148 SVG units).
  - Knob bars follow the knobs.
  - TE's 8×8 bitmap text: correct colours (rgb565 0xF800 red, 0x001F blue) but at a different scale (≈ 4.6 vs 5.35 photo px per unit, ratio ≈ 1.17) and shifted ~25 px left, cutting into the border.
  - **Hypothesis:** this unit has the newer 376-row panel (376/320 = 1.175); TE's matrix scales SVG space to it, while the bitmap text writes raw panel pixels. Decision: draw text with our own stroke font through te_line.

## 2026-10-07 — Test image 19: SCREEN2.OP1 (stroke font; panel diagnostics)
- `src/slot/gen_font.py` → `font.h`: stroke font on a 4 × 6 grid (0–9, A–Z, - + : . = / %), drawn with te_line.
- Read-out on our engines' screen: geometry globals `0xFF8017B4..C0`, raster object fields, variant flags `0xFF906A54..56` and `0x018BE698`, global matrix `0x01DE5E68` (hex), knobs; red ticks every 4 units from y = 136 to 160 (labelled) and edge ticks at x = 0..8 and 312..320.
- Slot block 3,956 B. Engines and DB unchanged.
- `firmware/images/SCREEN2.OP1` sha256 e301ab0d115783106597829ca24bba709dfbba6e49129649e2943d37b9f23d41. te-boot.ldr identical to stock.
- **Result (user, photo):** stroke font crisp and correctly placed; no glitches reported.
  - G = 400 240 376 188; raster width 188, stride 512, rows 376; flags `0xFF906A54..56` = 01 01 00, `0x018BE698` = 0. **Confirmed:** this unit has the newer 376 × 188 panel.
  - Matrix (Q22.10, column-major) m1 = −1.1875, m3 = +1.1748, m7 = 375.94, rest 0 except m8 = 1: panel px within row = 1.1748·y, panel row = 375.94 − 1.1875·x. So SVG 320 × 160 maps onto the panel ×1.175, rotated.
  - Visible area: side ticks at x = 0..8 and 312..320 visible; bottom ticks show only up to y ≈ 142 (label 136 visible, 140 not). **Safe area: x 4..316, y 2..140.**

## 2026-10-07 — Test image 20: WT2.OP1 (wt warp instead of cutoff, filter FX, wavetable screen)
- User asks: TE-inspired look; a Vital/Ableton-style wave view; no filter knob in wt (filtering belongs to FX); enable TE's hidden filter FX.
- **wt white knob = WARP** (filter removed): smooth phase bend `phase' = phase + k sin(2π phase)/2π`, k ≤ 0.9 (`src/engine/warp.h`, shared with the screen). wt raises its band-limit level by k octaves (≥ log2(1 + k)).
  - First try, a two-segment bend: its corner aliased at about −15 dB at C8 and shifted DC. Replaced.
  - Warp shifts the wave's mean, so a ~10 Hz DC blocker follows the oscillators (state bytes 16/20, reset on note-on).
  - Without the filter, warp-0 aliasing worst is −34.6 dB (sync, C8; the 14 kHz filter used to mask interpolation images); max warp worst −32.6 dB; others −34 to −47 dB.
  - `wt_check.py`: warp cases; aliasing ignores < 20 Hz; glide level checked with RMS over 4-period windows below 3 kHz (the 128-sample envelope measure was wrong for non-sinusoids). All pass; 386 cycles/sample max (sim).
  - `wt_layout.h` shared with the screen.
- **Filter FX**: `build_slot.py --db-enable-filter` adds `fx_types (2, 'filter', '[7548, 0, 8272, 19572, 8000, 8000, 8000, 8000]')`, as op1repacker's `filter` mod.
- wt presets: knob 3 values reset from cutoffs to warp amounts (0–12000).
- **Screen** (`src/slot/ui.c`):
  - wt: bank name, four TE-style dials (grey 300° ring, coloured needle, POS/BANK/WARP/UNI) and a Vital-like waterfall of the 12 frames (32 points each, purple-grey), with the current morphed and warped frame in blue (64 points) at its depth.
  - va: oscillator shape and dials WAVE/DET/CUT/RES.
  - About 600 line segments per frame.
  - Reads the wt tables through `--defsym _wt_data` (address taken from the wt engine's build).
- `src/slot/test/preview.py` + `preview_host.c`: builds ui.c natively with a recording te_line and renders PNGs at the device scale, to check layouts before flashing.
- `firmware/images/WT2.OP1` sha256 ddbf34fdf034a2104dcaf676a826ffdd2f47fa3a92ecb89a0bcc5d2db57e8832. te-boot.ldr identical to stock.
- **Result (user): works.** Screen correct and follows the knobs; no audio glitches; filter FX listed; warp sounds good. **Open:** LFO modulation is not shown (the screen reads the stored knobs `0xFF8013C4`, not the modulated values).

## 2026-10-07 — Test image 21: WT3.OP1 (screen shows LFO modulation)
- Cause of the open item in WT2: the voice loop (L1 `FUN_ffa009a0`, at `0xffa00cae`) posts each block's modulated knobs for the playing slot into a one-deep mailbox at `0xFF8010B8` (7 words: int16 knobs[8], slot, engine, flag; full flag `0xFF8010D4`), **only when it is empty**. The active screen's ReadInterface (`FUN_01a28210`; six variants at `0x01a28xxx–0x01a29xxx`) pops it with `FUN_0196749c` into the record `0x01AB321C` and uses the live values when a new record matches slot and engine and flag = 0. Our hook replaces iter's update, so nobody popped: the mailbox stayed full and stale. **Confirmed (code).**
- Fix (`ui.c`): pop into TE's own record each frame and use its knobs under TE's rule (new and matching), else the stored knobs.
- DB same as WT2. Host preview still builds (mailbox stub returns 0).
- `firmware/images/WT3.OP1` sha256 5f90f9d3d0909b0f66ec3dd0be7d6846673844a63091aad45f3a012b2b767fe7. te-boot.ldr identical to stock.
- **Result (user): LFO modulation still not shown** (knobs still update the screen). Also reported: slight distortion while holding a note and changing many parameters or switching screens, mostly or only with unison on (noticed before WT3 too).

## 2026-10-07 — Test image 22: PROF1.OP1 (on-device profiler + live-record diagnostics)
- `tramp.S`: CYCLES around each render call; worst call, sum and count go to `_prof_stats` (uint32[3]) at `0x00800000` (FREE region 0; test harnesses define their own). ABI check mask 0; params regression bit-identical.
- `ui.c` diagnostics on our screens (orange, under the title):
  - `DSP <% of frame> MAX <worst call, K cycles> N <render calls per frame>`;
  - `UI <% of frame for our drawing> LV <fresh mailbox records, mod 1024> E S F` (engine, slot and flag of the live record);
  - `P <live k0> <live k2> FRAME <K cycles per display frame>`.
  - % = cycles / CYCLES-measured frame period. Stats are reset each frame; writable UI state in `.data`.
- Purpose: real DSP and UI load while holding chords with unison, switching screens and sweeping knobs; and whether live records arrive and match (LFO display).
- DB same as WT3. `firmware/images/PROF1.OP1` sha256 233491789db3192c354f70b59854c5e2ce7156416a5e4706083c74da31a5a0fd. te-boot.ldr identical to stock.
- **Result (user): works; all 4 checks pass.** Requests: drop on-screen knob dials, give every knob a visual (wavetable unison had none), sampler gain for grain.

## 2026-10-07 — Test image 23: MODAL1.OP1 (modal synthesis engine, id 17; includes PROF1 diagnostics)
- Port of the user's krystal Modal engine (`krystal/firmware/engines/modal/`): same 8 materials (ratio formulas, Gong jitter from krystal's Lcg seed 7), 5 exciters, T60 = 0.05·400^ring (infinite > 95%), per-mode damping (1 + 0.035 k), strike-position weight (position 0.30), brightness tilt, energy-normalised bursts (targets 0.28 / pluck 0.5), output tanh(0.7 sum) → soft clip.
  - OP-1 changes: fixed point; 20 modes (krystal 64, density 0.75 → 50); mono; 4 knobs (blue material, green exciter, white ring, orange brightness); no lattice; velocity 0.8; no note-off (bow/breath/rain excite while the voice renders).
  - Fixed point: a1 = 2r cos w (Q30), cos w = 1 − 2 sin²(w/2) from a 1025-entry Q30 sine table; mode frequency = f0 · 2^log2ratio from the phase increment; r = 1 − x + x²/2; block-rate exp2/div/sqrt by table and restoring loops. `gen_modal_tables.py` → `modal_tables.h`.
  - Per-voice state (`modal_state.h`, ~220 B) in `voice_ram` = FREE region 0 at `0x00810000`, 4 KB per TE voice, found from the voice object address (`0xFFB00000 + i·0x24`). Note-on detected from TE's 0x80000000 marker.
- CPU (sim), in steps:
  - C, sample-outer: 735 mean, 1,206 max per sample.
  - Mode-outer: 540.
  - Hand-written resonator loops (`modal_asm.S`, dual-MAC Q31 products sharing accumulators, hardware loop): ~365.
  - Bow made chunked (friction every 32 samples): 1,025 → ~480.
  - Bursts generated as they play, with a cached dry-run normalisation: strike spike 847 → ~550 (first strike after an exciter or brightness change: up to 783).
  - Final: worst sustained mean 580/sample, peak 783.
- Bugs found by the sim:
  - asm v1 fed 2·y into the multiply and wrapped above 64.0 (Nyquist oscillation); fixed with half-scale coefficients and a saturating doubling.
  - An expected-RMS burst normalisation was up to 7 dB off; replaced by the dry run.
  - The first pre-rendered version was 2× too hot (the new pipeline hits krystal's 0.28/0.5 targets exactly, checked in Python).
  - Bow coupling 3 (krystal, 50 modes) did not sustain with 20 modes; 4–6 clipped low bright notes; 8 used.
- `modal_check.py` (329 cases) all pass: String modes 219.7/439.4/660.4/880.1 Hz, Bar 1 : 2.78 : 5.44; T60 0.32/0.98 s for 0.3/1.0 targets; mallet/pluck decay, bow/breath/rain sustain; no instability. 19 hot low sustained cases saturate the soft clipper (krystal's tanh does too).
  - Faithful but notable: mode gain ∝ (1 − r), so a mallet at very long ring is very quiet.
- Build: `build.sh`/`run.sh`/`simrun.py` link optional `<engine>_asm.S`; `tramp.S` profiling and `voice_ram` symbols in all harnesses; sim driver note-on marker fixed (was at state+4).
- Screen: krystal's crystal (`gen_crystal.py` → `crystal.h`): habits morph with material, spin follows the voices' output peak (krystal meter mapping), hidden edges dim. Edges enumerated with their two faces instead of krystal's all-pairs search. Dials MAT/EXC/RING/BRT, material and exciter names.
- Presets 3 (bell mallet, bowed string, rain glass) → 176/176 (the limit). Image 13,667,295 B.
- `firmware/images/MODAL1.OP1` sha256 590151b0cf93e379c486492b780353f9f62e1bd9e6c5145a10784bac633e96cf. te-boot.ldr identical to stock.
- **Result (user, 2026-10-07 evening):** LFO modulation still not shown on the screen (PROF1/MODAL1 diagnostics photos to follow). Modal "isn't how I expected it" (details to follow). Not yet known whether MODAL1 itself was flashed and how it measured.

## 2026-10-07/08 (overnight) — GRAIN1.OP1 (granular engine, rename, live LFO display) and HOLD1.OP1 (+ hold)
- **Live LFO display, new approach**: the render trampoline copies each call's params (after LFO modulation) to
  `_prof_stats[3..4]` and counts calls in `[5]`; the screen uses them while the count moves, else the stored knobs.
  Halfword copies: the first version used 32-bit loads on the int16 params and faulted in the sim (misaligned).
  ABI checks pass for all engines; params regression bit-identical. Not yet confirmed on device.
- **Rename**: engine `wt` → `wavetable` (DB type, presets folder, name table). `build_slot.py` supports `aliases`
  (extra name-table entries for the same id; name loops bound = engines + aliases, id checks = engines), and
  `wt` is kept as an alias so patches saved as `wt` still load (TE asserts on unknown names). Data symbols are
  keyed by source file (`_wt_data`).
- **Presets**: iter 1 → 0 (iter hidden again, as stock), va 4 → 2; granular 3. Count 176/176.
- **Granular engine** `grain` (id 18, `src/engine/grain.c`, 849 KB at `0x00820000`, FREE region 0 above voice_ram):
  - Source bank: 6 synthesised sounds × 1.6 s, rooted at C4, 20 Hz high-passed, -3 dBFS peak, plus a 256-point
    overview (`gen_grain_data.py`).
  - Up to 8 Hann grains per voice; POSITION scans the bank; SIZE 10–500 ms; DENSITY 2–80/s; SPRAY ±0.3 s,
    ±50 cents, octave shimmer above 60 %. 1/sqrt(overlap) gain.
  - State in voice_ram (`grain_state.h`); `isqrt32` added to dsp.h.
  - `grain_check.py` (84 cases) all pass: C4 −8.7 / G4 +3.5 cents (non-overlapping grains; with spray 0, overlapping
    copies comb-filter by ±45 cents, inherent to granular); all sources sound; density loudness within 1.6 dB;
    shimmer +13 dB; no saturation or clicks; CPU 553 sustained / 573 peak per sample.
  - Screen: source name, POS/SIZE/DENS/SPRAY dials, bank overview with source seams, position marker and spray
    brackets, live grains from voice_ram (rendering voices only).
- **Hold** (notes/hold.md): SHIFT+T2 toggles; key-up CALL `0x019AC3A6` and the synth controller's shift-key slot
  `0x019119B0` hooked through loop-saving wrappers. Host test 8/8. HOLD1 differs from GRAIN1 only by those two
  patches (memory diff). "HOLD" shown on our screens.
- Regressions: va_check and wt_check all pass (wt unchanged: 386 cycles/sample, aliasing as before); ABI checks pass for va, wt, modal and grain.
- Images: 14.4 MB (limit 15.7 MB).
  - `firmware/images/GRAIN1.OP1` sha256 2d57177e68b4630acf6956947c5512f0558b52f662df4e3413a411ac4f449821.
  - `firmware/images/HOLD1.OP1` sha256 c576614cc82ac81a42da0d58449639fd53da5e41a0396c515490ad220496d1c8.
- **Result (user, 2026-10-08): "everything seems to be working really well"**, plus one crash (photo), in the synth engine browser with GRAIN selected.
  - Exception screen: SEQSTAT 0x24 (EXCAUSE: misaligned data access), RETX `0x019674b4`, ADDR `0xff8010d4`; call chain `0xffa00424`, `0xffa035a8`, `0x1e20050` (our `_ui_update` → `ui_draw`), `0x19b2674` (frame render), `0x19ab8b6`, `0x1a9c290`, `0xffa09d3e`.
  - Cause (**Confirmed**, code): `RETX` is in TE's mailbox pop `FUN_0196749c`, which takes the destination record in **R1** (`P1 = R1`; TE's caller at `0x01a2829c` sets `R1 = P5`). Ghidra showed it as the first parameter, and ui.c (since WT3) passed the record in R0. So whenever a fresh record was waiting, 28 bytes went to whatever R1 held: silent memory corruption, and here a misaligned write.
  - Every TE function we call was then re-checked against a TE call site (not the decompiler): line, raster, note-off (R0), note builder (R0–R2), SHIFT-key handler (R0 = this + adj, R1 = key; `0x019b6cb4`), iter_get/init. All match.

## 2026-10-08 — Test image 26: HOLD2.OP1 (HOLD1 without the mailbox call)
- `ui.c`: mailbox pop removed (live knobs come from the trampoline). Diagnostics line 2 = `UI % LIVE <frames with live knobs>`, line 3 = shown knobs and frame length.
- Lesson: take argument registers from TE's own call sites; Ghidra's parameter list can be wrong for TE's ABI.
- Memory diff vs HOLD1: only the slot block (`0x01E2xxxx`) changes; DB identical. Engines unchanged.
- `firmware/images/HOLD2.OP1` sha256 73b16d2acbf00c4bac7d375b3dc3a70b5d4f940e065edeed583c967e92f019f5. te-boot.ldr identical to stock.
- **Result (user): the crash no longer reproduces.**

## 2026-10-08 — HOLD2 device results (user, photos)
- LFO display works: `LIVE` counts and K follows the LFO (wavetable, `DSP 17% MAX 70K N 64 UI 73% F 19090`).
- Hold works. With many held notes on grain (choir cloud): `DSP 99% MAX 86K N 132 UI 68% F 29887`; the DSP % creeps up and sometimes shows > 100 % when mashing keys.
  - The DSP % is inconsistent with its own fields: 132 calls ≤ 88k cycles each cannot make 99 % of a 30.6 M-cycle frame. Hypothesis: render calls overlap in time (two rendering contexts), so wall-clock intervals double-count. The measurement needs fixing.
  - Real costs on device: grain ~86k cycles worst per call (~670/sample; sim 553); wavetable 70k (~550; sim 386). Device ≈ 1.2–1.4 × sim.
  - UI 68–73 % of the frame time; frames 48–75 ms (13–21 fps): TE's AA line is expensive, and we draw ~600+ segments plus the diagnostics text.
- Modal: very loud white noise when switching to modal after other engines, until knobs are turned. Cause (**Confirmed**, code): modal and grain share voice_ram slots with different layouts; modal resumes grain's data as resonator states. User: "I don't think modal works very well".
- Wavetable CPU fine; rename, grain and filter FX all good.

## 2026-10-08 — Test images 27/28: HOLD3.OP1 (release) and HOLD3D.OP1 (debug lines)
- **Modal noise fix:** voice_ram slots carry an owner tag as the first word (`OWNER_MODAL`/`OWNER_GRAIN`, `modal_state.h`); an engine that finds another owner clears the slot first, and the screen only reads slots of the engine it draws. The sim driver now fills voice_ram with garbage before every run, as after boot or another engine. modal and grain checks pass with it.
- **Grain CPU:** samples left in the window computed once per grain (no per-sample end test); a guard sample at the end of the bank (no wrap test in the interpolation); 1025-point Hann table without interpolation; 16-bit multiplies (the delta is halved to fit). Sim 553 → 451/sample; device gain should be larger, since the sim counts 32-bit multiplies as one cycle. All grain checks pass.
- **Screen cost:**
  - The diagnostic lines are only in `--debug-ui` builds; the debug DSP line now shows average and max cycles per render call (not a % of the frame).
  - `vline_fast`/`line_fast` write 1-px lines straight into the back buffer, mapped through TE's matrix. Used for grain's 256 overview bars and seams, wavetable's background frames, dial rings, modal's hidden edges and va's baseline. Anti-aliased te_line stays for highlights, needles, visible edges and text.
  - Guard: the fast paths write only if the raster buffer lies inside one of the two framebuffers (`0x02000000`/`0x03000000`) and the geometry is sane; otherwise they fall back to te_line.
  - Dial rings 20 → 12 segments; wavetable frames 16 points, highlight 32.
- Modal still included (pending the user's decision whether to drop it).
- ABI checks pass for all engines.
- `firmware/images/HOLD3.OP1` sha256 b38ff15a1a0f47bbb23f329e00053c81e80a6ef70f47236c7f8214870e268ab2.
- `firmware/images/HOLD3D.OP1` sha256 5e5de554bdf94f76ad8f4d03215706ab01bacf81c828c707440ea1db3b3c9685.
- **Result (user): "looks good".** Requested: remove the debug text and modal; make grain better, granulating the OP-1's own audio if possible; cleaner UI via a UI reviewer.

## 2026-10-08 — Test image 29: GRAIN2.OP1 (sampler-source grain, UI redesign, modal removed)
- Modal removed from engines.json (code kept); `modal` kept as an alias of grain so saved modal patches cannot assert. Screens are chosen by a generated kind table (`_ui_kind`), not by engine id. Diagnostics only with `--debug-ui`.
- Grain:
  - granulates the synth sampler's sample of another slot (or the bank): notes/grain.md;
  - position overflow fixed;
  - 6 presets (+ frozen pad, stutter, shimmer cloud) → 176/176;
  - grain_check: sampler source plays a 440 Hz test sine at 440.7 Hz on C4 and 879.5 Hz on C5; all bank checks pass (the level and shimmer tests were corrected); 396 cycles/sample.
- UI redesign implemented (notes/screen.md); host previews checked. Two va scaling bugs found in the previews and fixed (oscillator width, filter dB scale).
- ABI checks pass (va, wt, grain). `firmware/images/GRAIN2.OP1` sha256 eebabd93fbf52753c4745a77af4fcf43a7a4f2cce6d65e26e55c5f5da36a49f5. te-boot.ldr identical to stock.
- **Result (user): works; all four checks pass** (grain plays the sampler's sample and follows its trim; bank position matches the title; screens clean and readable; better CPU). Requests: no knob dials on screen, but every knob must have a visual representation (e.g. wavetable unison had none); grain needs gain when granulating a sample (the sampler's own, or grain's).

## 2026-10-08 — Test image 30: GRAIN3.OP1 (knob visuals instead of dials; sampler gain in grain)
- Screens: dials removed; every knob drives part of the picture in its colour.
  - wavetable: green = title + 8 bank pips; white = warp glyph (one sine cycle through warp.h's bend); orange = unison ghost of the current frame (off-red under 25 %, then orange; AA, spread 2–12 units); blue = current frame.
  - grain: blue = playhead; green = grain-size Hann window above it (true width on the overview scale, at least 6 units); white = density ticks (1–16); orange = spray bracket.
  - va: unchanged (wave/detune ghost/cutoff curve/resonance mark already cover all four).
- Grain with the sampler as source applies the sampler's gain like TE: `(knob 8 / 8192)^2`. TE's render squares p7 in Q15 (`R7.L = R0.L * R0.L`) and shifts left by 4 after multiplying (`0x01a51c3c`), so 8192 = unity and the maximum = x16; factory sampler presets use 7864–17377. Sim: gain 16384 vs 8192 = +11.4 dB (+12 expected; soft clip). The bank stays at unity.
- `firmware/images/GRAIN3.OP1` sha256 ae6e30281c12456d4c6b5d21ba2b0a4ca68092324e169982d70318e775e29ffa. DB same as GRAIN2. te-boot.ldr identical to stock.
- Result (user, 2026-10-09): **all works**: ens listed and selectable as synth, drum and master FX; gain, the master Haas shift, preset save/load and the stock FX all fine.

## 2026-10-08 — Test image 31: FX1.OP1 (our own FX type, id 8 "ens": path test)
- Research (notes/fx-slot.md): synth/drum FX are mono (64 samples in place); only the master FX is stereo (separate L/R). Audio becomes stereo in the mixer. Side finding to verify: voices render 128 samples at 88.2 kHz, decimated 2:1 (`FUN_ffa005d4`); absolute-Hz/ms maths in our engines may be 2x off (pitch is not: it comes from TE's increment).
- Correction before building: FX range row 8 would hit another table (`0x01DE5DD0`), so the range table is relocated (15 readers), not extended in place.
- `build_slot.py --fx ens` + `src/slot/fx.c` + `FX_CASE`/`_fx_stereo_entry` in slot.S. FX1 behaviour: blue = gain (16384 = unity, up to +6 dB); as master FX, green delays R by up to 11.6 ms (Haas shift to the left).
- Built like GRAIN3 plus `--fx ens`. `firmware/images/FX1.OP1` sha256 b3c247124f115b098181d4405265a31e98898b3dea9bab5130631bbd9e76189c. te-boot.ldr identical to stock.
- Result (user, 2026-10-09): **all works**: ens listed and selectable as synth, drum and master FX; gain, the master Haas shift, preset save/load and the stock FX all fine.

## 2026-10-09 — Test image 32: FX2.OP1 (ens = chorus ensemble)
- `src/slot/fx.c`: three delay taps on one triangle LFO at 0/120/240 degrees; R's taps at +180 (mirrored around the centre delay). Knobs: blue rate 0.05–12.8 Hz (exp), green depth ±0–3 ms around 2 ms + depth, white ensemble (taps 2/3 fade in + 6.5 Hz vibrato, Solina-style), orange mix (16384 = 50/50). Defaults `[13600, 16000, 0, 16384]` (~0.5 Hz, Juno I-ish).
- LFO evaluated every 8 samples, delays interpolated in between; depth slew-limited (1/16 sample per sample), ensemble and mix glide per block: no clicks on knob jumps or LFO modulation.
- Sim (`src/slot/test/fx_check.py`, new driver `fx_main.c`): 13 checks pass. Dry bit exact at mix 0; 89-sample base delay; rate 1.47 Hz at blue 20000 (expect 1.48); depth ±3.09 ms; R anti-phase (corr −0.999); summed 3-tap vibrato at 19.5 Hz (the 6.5 Hz fundamental cancels, as in a string ensemble); level −2.6…−3.3 dB at defaults; knob jump max step 569 (dry sine 591); no saturation; **stereo 21 k cycles per 64-frame call (328/frame, ~2 % of the block), mono 11.7 k**. A first version cost 729/frame and clicked on depth jumps (fixed: sub-block LFO, slew limit, mix glide).
- `firmware/images/FX2.OP1` sha256 85954b2476e677b4cfa116ea50dd0f3c70b7b301d834529ce1c83f5d108e7492. Same build as FX1 plus `--fx-default`. Screen still filter's.
- Result (user, 2026-10-09): **all works.**

## 2026-10-09 — Test image 33: FX3.OP1 (rename ens → ensemble; FX list plumbing)
- `src/slot/fx.json` + `build_slot.py --fx FX.json` (several FX, aliases); fx.c → `ensemble.c` (`ensemble_mono/_stereo`); sim runner `src/slot/test/fxsim.py`, checks `ensemble_check.py` (all pass, unchanged audio).
- Name `ensemble`, alias `ens` so patches saved with FX1/FX2 still load. Name loops bound 10, factory/ScreenId bound 9 (checked in the patched image).
- `firmware/images/FX3.OP1` sha256 1ac683b919e0bdd235ea7981dd7e698cbddbe0c36097b8ab525fe076df574a86.
- Result (user): **all works** (confirmed together with the later images).

## 2026-10-09 — Test image 34: CHORD1.OP1 (chord mode; pingpong FX)
- CHORD (notes/seq-chord.md "CHORD1 as built"): SHIFT + up to 4 notes learns a shape; then every key plays it; SHIFT + one note turns it off. Note-on hook at `0x019AC290`; reference-counted chord notes; works with HOLD (held keys now retrigger when pressed again). Badge + shape ruler on our screens. Host test 23/23.
- `pingpong` FX (id 9; `src/slot/pingpong.c`, notes/fx-pingpong.md, by a subagent): free-time ping-pong delay, lines in FREE region 1 `0x01E80000`–`0x01EE0000`. Sim 17/17 (re-run here); 16.1 k cycles per stereo call in the sim (SDRAM wait states not modelled).
- FX ids: 8 ensemble (alias ens), 9 pingpong. Name loops 11, factories/ScreenId 10 (checked in the patched image). Slot block ends `0x01E25754`.
- `firmware/images/CHORD1.OP1` sha256 3eb45745405da4d3b32070edfcf66d2206884a3fe4bb2e802ebaf69629555383. Built like FX3 plus `--chord` and pingpong in fx.json.
- Result (user): **all works** (confirmed together with the later images).

## 2026-10-09 — Test image 35: DRIVE1.OP1 (CHORD1 + drive FX)
- `drive` FX (id 10; `src/slot/distortion.c`, notes/fx-distortion.md, by a subagent): tube → hard clip → wavefold, 2x oversampled + ADAA, auto gain, tilt tone, mix. Sim all pass (re-run here). Typical 20.8–24.8 k cycles per stereo call; worst (max drive, 5 kHz, fold blend) 40.8 k. Fold aliasing at 5 kHz max drive −18 dB (ADAA corrections capped for CPU). Mono and stereo share scratch (safe: both run sequentially in the one audio callback).
- FX ids 8 ensemble, 9 pingpong, 10 drive: name loops 12, factories/ScreenId 11 (checked). Supersedes CHORD1 (same plus drive).
- `firmware/images/DRIVE1.OP1` sha256 145c3d16f63d2f54a237cd1e377fca0baf66d4740aeee909751adadf47026dce.
- Result (user): **all works** (confirmed together with the later images).

## 2026-10-09 — Test image 36: FXUI1.OP1 (FX screens; distortion reworked and renamed)
- FX screens: filter screen page update (slot `0x01910CF0`, stock `0x019E8308`) → `_fx_ui_update` (slot.S): edit mode `[0x01DF4E3C]` + slot pick the FX id (synth `0xFF801734`, drum `0xFF801754`, master `0xFF801090`; FUN_01a28da0, Verified); TE's FX jump to the stock update, ours call `fx_ui_draw` (ui.c). Both stock updates render the SVG at their end, so ours get a clean canvas.
  - ensemble: live sweep (LFO phase from `ensemble_state`), extra taps in white, R anti-phase on the master, depth guides, mix bar. pingpong: echo train (L up, R down), decay envelope, tone glyph, mix bar; mono title DELAY. distortion: transfer curve from `distortion_curve()` (64 segments), mode name title, drive bar, filter glyph, mix bar.
- `drive` → `distortion` (alias `drive`). Rework by a subagent (notes/fx-distortion.md): drive −6…+36 dB; green = resonant SVF before the shaper (low-pass with resonance ← flat → band-pass focus); white = TUBE, FUZZ (gated octave-up), CLIP, FOLD (8 folds), CRUSH (9→3 bits, 62 k→1.9 kHz); stereo L/R variation on the master. Sim 32/32 (re-run here, with ensemble and pingpong checks); typical 24–29 k cycles per stereo call, worst 42 k. Fold aliasing on hot bright input is high (CPU cap).
- Default distortion `[20000, 20480, 2048, 32767]`. Slot block ends `0x01E294D4`.
- `firmware/images/FXUI1.OP1` sha256 0664d6d0cd80f98cdfdfe93d7e64e28eea5df057bcaf714271fd6b239537c6b8.
- Result (user): **all works** (confirmed together with the later images).

## 2026-10-10 — Test image 37: WT4.OP1 (wavetable ANALOG bank reversed)
- ANALOG bank now sine → triangle → square → saw (position 0 = sine), as the user asked. `gen_wt_data.py` builds it with x → 1 − x; `wt_check.py` cases moved to match (all pass). Preset "analog sweep" moved to position 32767 so it still starts on the saw. Same build as FXUI1 otherwise.
- SVG route research (notes/svg-route.md, subagent): feasible; our own SVG files can ship in the update tar and be loaded lazily from our screen hooks; subset and limits listed. Not built.
- `firmware/images/WT4.OP1` sha256 b0da3ef72acd96490660bda92565ffe28d6e41733a7a183413295f2f5e786353.
- Result (user): **all works** (confirmed together with the later images).

## 2026-10-10 — Code review fixes; test image 38: FIX1.OP1
- Code review (subagent, whole repo): no critical findings. Fixed now:
  - CHORD: chord keys whose key-up can't come any more (play-mode switch clears s_note_ons; TE's key-up returns early at `0x019AC37C`) leaked references, so later chords sharing notes dropped them. `purge_stale()` at each chord note-on releases keys with `key_n && !KEY_DOWN && !held`. hold_test.c: new case, fails without the fix, 25/25 with it.
  - Memory overlap: `ldr_patch.py --reserve LO-HI=WHAT` (runtime RAM not loaded); all new blocks and reserves must be pairwise disjoint and reserves inside one FREE region. build_slot.py reserves prof_stats, voice_ram and each FX "ram" (needs "ram_size"; pingpong 0x60000). Negative test: pingpong ram over the slot block is refused.
  - FX ABI: wrapper macros moved to `src/slot/entry.inc` (shared by slot.S and the test). `src/slot/test/fx_abi_check.py` (+ fx_abi.S, fx_abi_main.c): every FX through its firmware entry wrappers preserves R4–R7/P3–P5/FP/SP/loops/L regs (1200 calls each, knobs changing), the stereo 4th argument arrives (mix 0 = dry, bit exact), mono/stereo interleaved = each alone. 9/9; mutation (wrong arg offset) is caught.
- Open from the review: engines assume 44.1 kHz but render at 88.2 kHz (grain sizes/density and va cutoff 2x off; sim shares the assumption) — confirm on device, then fix constants and sim. Low: ui_kind bound, grain src_key hash, --half vs --pointer overlap, op1pack name rules/NAND block limit, rel_ref vs skipped out-of-range notes, distortion SLEW<PLAT guard, dead modal code.
- `firmware/images/FIX1.OP1` = FXUI1/WT4 + the CHORD fix (screens, FX and wavetable as WT4).
- Result (user): **all works** (confirmed together with the later images).

## 2026-10-10 — Test image 39: DIST2.OP1 (distortion drive taper)
- User: blue "does little on the bottom 50 % and just starts getting good at 80–100 %". OP-1 voices reach the FX well below full scale, so the linear-in-dB drive stayed nearly clean up to half way. `drive_taper()` (distortion.c): t = 1 − (1 − k)²(1 − 0.3 k), applied in the DSP and in distortion_curve(): knob 25 % → +14 dB, 50 % → +27 dB (the old 80 %), 75 % → +34 dB, max +36 dB unchanged; the old top fifth now spans the upper half.
- distortion_check.py: all pass (the static-curve comparison moved to blue 5941 / 11178 = the old 12000 / 20000 effective drive; heavier drive is dominated by the treble lift and oversampling the static curve leaves out). fx_abi_check 9/9. Cycles unchanged (typical 24–29 k, worst 42 k per stereo call).
- FX screen designs: ensemble Choir HQ approved (no text on FX screens); pingpong Table Tennis HQ and distortion compact-pedal HQ mocked (session scratchpad fxmock/), not built.
- `firmware/images/DIST2.OP1` = FIX1 + drive taper. sha256 db5a5f18480253322cb9908d7abdb6aa46d3dd9ff08b476b1331b7a85898307f.
- Result (user): **all works** (confirmed together with the later images).

## 2026-10-10 — Test image 40: MUFF1.OP1 (MUFF distortion character); FX screen designs approved
- MUFF (subagent; notes/fx-distortion.md): stage 1 tube knee (ADAA, 2x OS) → 110 Hz HP + 4 kHz LP → stage 2 soft clip x6 with a small bias → fixed mid scoop (−8 dB at 1 kHz). White now 6 modes: TUBE, MUFF, FUZZ, CLIP, FOLD, CRUSH (plateau centres 1536, 7475, 13414, 19353, 25292, 31231). Default `[16000, 16384, 7475, 32767]` (MUFF, thick sustain). Sim: all pass; MUFF with green on 34.2 k cycles per stereo call (accepted, own budget 36 k: green must work in every mode); worst case 44.7 k. fx_abi_check 9/9. Patches saved with the old 5-mode white positions land on different characters.
- FX screen designs approved by the user: ensemble choir, pingpong table tennis, distortion pedal + wah (real pedalboard order). Assets in `design/fx/` (our own SVGs, validated with check_svg.py). Next: SVG1 device experiment (notes/svg-route.md).
- `firmware/images/MUFF1.OP1` = DIST2 + MUFF. sha256 7ecb8948ce7cc7e46577beed7dadc7b29b72b558d44a12c25bf7a66656e43cf5.
- Result (user): **all works** (confirmed together with the later images).

## 2026-10-10 — Test image 41: SVG1.OP1 (first SVG screen: the choir on the ensemble FX)
- `op1pack.py build --add NAME=PATH`: new tar member cloned from a sibling's header (content/display/...); refuses names with FRC/RBT/KEY or components starting with '.'/'_' (te-boot command names), non-ASCII or ≥ 100 B. Image adds `content/display/choir.svg` (design/fx, 12.9 KB).
- ui.c SVG runtime (notes/svg-route.md; conventions re-checked at TE call sites: loader R0 = name; yaffs_open(path, 0, 0x100) → −1 if missing; find(node, id, R2 = 0 → 0 if missing); matrix(node R0, m R1), node matrix at +0, visible byte +0x50, vptr +0x54; render(&{doc})). Lazy load on first show of the ensemble screen after an existence check; nodes cached; missing file → the line version.
- Choir driver (SVG1 scope): s1–s3 sway (rotation about the feet by depth × tri(LFO + i/3)), heads 0.6× further, backing singers/cones shown by white, sparkles flicker with the 6.5 Hz shimmer phase, mouths scale with mix (closed smile under 5 %), notes rise by round(6 mix), master reflections in anti-phase. Not yet: the outline step for white < 50 %, green motion marks overlay. `--svg-cost` draws the render cost in kilocycles bottom-right.
- `firmware/images/SVG1.OP1` = MUFF1 + the above. sha256 e5685079074611e465cfe9354842e74798d37d4b6573a24acd1df16182286a11.
- If it asserts when opening the ensemble screen: reflash MUFF1 (the extra file stays on NAND; stock never opens it).
- Result (user): **works on device**: choir loads, renders and animates. Issues: backing singers appear instead of fading in; the green stars read as a blob.

## 2026-10-10 — Test image 42: SVG2.OP1 (choir: white fade-in, sharper stars)
- SVG route confirmed on device by SVG1 (MUFF1 etc. also confirmed: "all works").
- White fades the backing singers in from black (white 1024 → 16384): 14 fills + 8 strokes recoloured through TE's style getters `FUN_01a3b96c` / `FUN_01a3b854` (screen*, id → rgba*; they find with assert, so each id is checked with find(…, 0) first; pointers cached at load). Their colours in choir.svg are now unique (±1 of the palette) in case TE shares a style entry between equal colours; check_svg.py accepts ±1.
- Stars: 2 eight-ray stars per backing singer with long thin rays (was 3 small four-point stars that blurred into a blob); they scale with depth (0.6 → 1.5) and flick ±20° at the 6.5 Hz shimmer instead of blinking.
- `firmware/images/SVG2.OP1` sha256 f6b9046d36780d6b6770bb9fed5add2e8daf6eda012b0eb8165a4466a6e80a1c (with --svg-cost).
- Result (user): **works**: backing singers fade in, stars read as stars, smooth. Render cost ~2000 kcycles (≈2 M) at rest, ~11000 (≈11 M) with all knobs up: near a 30 fps frame if the core is 400 MHz (unconfirmed); audio unaffected. Motion at fast rates too quick to follow.

## 2026-10-10 — Test image 43: SVG3.OP1 (choir motion speed cap)
- Sway and star flick follow the audio LFO phases through a per-frame delta capped at 0.05 turn (~1.5 Hz at 30 fps): exact at slow rates, readable at fast ones, never aliases backwards.
- Budget for the next SVG screens (pingpong, pedals): ~6000 kcycles busy; animate small parts, not large fills.
- `firmware/images/SVG3.OP1` = SVG2 + wavetable output knee (5db57df) + the cap. sha256 bc99fbc36b8a4d538846ed08368fe91ddd78d51ae0f3873dd68740b390796cb2 (with --svg-cost).
- Result (user): motion too slow at full blue: wants it to keep speeding up over the whole knob.

## 2026-10-10 — Test image 44: SVG4.OP1 (choir motion follows the full blue range)
- Cap replaced: screen phases clocked by the 6.5 Hz shimmer phase delta; sway = blue rate exactly up to 1 Hz, then 1 + 0.35 x the excess (12.8 Hz -> ~5 Hz on screen); star flick a steady 3 Hz.
- `firmware/images/SVG4.OP1` sha256 de94bdeb3382c7e79ceed47ec7d38ad33b0b2b5b878138a624e7854ce7dcadbf (with --svg-cost).
- Result (user): **crash** opening the synth FX (distortion screen): `.\src\gui\Screen.cpp:84 linestyle_id`, from FUN_01a3b854 (stroke getter) called by our recol_init: it asked for the stroke of fill-only elements (shadow, wshadow). TE's style getters assert on a missing style, not only on a missing id.

## 2026-10-10 — Test image 46: SVG6.OP1 (fix: request only existing styles)
- pedals fade list now says per id whether it has a fill and/or a stroke; only those are requested. Host preview getters now assert like TE's (exit 5), so preview.py catches this class.
- `firmware/images/SVG6.OP1` sha256 cb521b5b59a29b0727c0330122221d9759858967ef26948d2d8f211fee8eff6a (with --svg-cost).
- Result: not flashed; superseded by SVG5/SVG6 (same choir code).

## 2026-10-10 — Test image 45: SVG5.OP1 (ping-pong and pedal SVG screens)
- pingpong: table tennis driver (draw_tennis): blue = table length/players, flights clocked by the delay line's write index (pingpong_state, now shared via pingpong.h), one flight per delay or a whole multiple >= 0.25 s; hitter swing + yell, receiver cocks back; ball along the newest arch, shadow; white = ball look/glow; orange = 11-pip scoreboard; green = echo arches overlay (<= 8, AA for the newest 6); synth/drum = squash against the wall.
- distortion: pedals driver (draw_pedals): blue = box shake/plate rattle/sparks/LED glow, all four knob pointers, green = wah treadle rock (3 face matrices interpolated from src/slot/wah_table.h, generated by design/fx/gen_wah_table.py from the 3D model), white = body/plaque colour + the mode word (stroke font through the shake matrix), orange = both pedals fade from black.
- Recoloured fills/strokes in pingpong.svg and pedals.svg made unique (design/fx/uniquify.py, +-1 per channel).
- Host preview renders the SVG screens (preview_host node table; preview.py applies matrices/visibility/colours and rasterises with rsvg-convert; FRAMES=n animates).
- No struct copies/array initialisers in ui.c: GCC emits memcpy (no libc).
- `firmware/images/SVG5.OP1` = SVG4 + the above, adds choir.svg, pingpong.svg, pedals.svg. sha256 e03eeec517df0efd3911f05b2b903263c61aff17716492f782523269300214b8 (with --svg-cost).
- Result (user): **crash** opening the synth FX (distortion screen): `.\src\gui\Screen.cpp:84 linestyle_id`, from FUN_01a3b854 (stroke getter) called by our recol_init: it asked for the stroke of fill-only elements (shadow, wshadow). TE's style getters assert on a missing style, not only on a missing id.

## 2026-10-10 — Test image 46: SVG6.OP1 (fix: request only existing styles)
- pedals fade list now says per id whether it has a fill and/or a stroke; only those are requested. Host preview getters now assert like TE's (exit 5), so preview.py catches this class.
- `firmware/images/SVG6.OP1` sha256 cb521b5b59a29b0727c0330122221d9759858967ef26948d2d8f211fee8eff6a (with --svg-cost).
- Result (user): **works**; everything sounds good. Distortion screen: the wah's grip lines look off (14 thick round-capped strokes under the treadle's skewed matrix merge and fray on the panel). Render cost reading on the pedals screen: 17599 kcycles.

## 2026-10-11 — Test image 47: SVG7.OP1 (wah grip as filled bars)
- pedals.svg ribs: 9 filled bars in the treadle plane (gen_rot.P3, X 6 + 12 i, 2.6 long, Y 2.5..19.5, Z 33) instead of 14 stroked lines; recoloured by fill.
- `firmware/images/SVG7.OP1` sha256 097a9336b64304e73a0adbcbcf86710d32793566c787926e5394b97dfe78b5b8 (with --svg-cost).
- Result (user): wah grip **fixed**; the distortion pedal's grooves show the same fraying.

## 2026-10-11 — Test image 48: SVG8.OP1 (distortion grooves as filled bars)
- pedals.svg grooves: 9 filled 66 x 1.6 bars instead of round-capped 1.8 strokes; recoloured by fill (ui.c fade list FS -> FF). gen_pedals.py grooves updated to match (its ribs still the old strokes; pedals.svg is the source of truth).
- `firmware/images/SVG8.OP1` sha256 003a35278b03b29ce7e73b76a90b67e8abf648b2cbc987efb027c4da6105f575 (with --svg-cost).
- Result: _pending user flash_.

## 2026-10-12 — Release tooling; test image 49: MODS.OP1 (release candidate)
- `build.py`: one command from stock 246 (downloaded and sha-checked if missing) to `firmware/images/<NAME>.OP1` and `dist/op1-mods-<name>.zip`. `--engines` / `--fx` pick a subset (names from engines.json / fx.json), `--no-hold`, `--no-chord`; `--svg-cost` / `--debug-ui` for development. `build.py --name TEST --svg-cost` reproduces SVG8.OP1 byte for byte.
- Patch bundle (`tools/mkpatch.py`, applied by `tools/apply.py`): LDR blocks inserted before FINAL (`blocks.bin`), op1_factory.db row changes (+24 rows: iter, filter, our 3 FX, our 3 engines and their 16 presets), our 3 SVGs. Checked: sha256 of the stock image, of the patched OP1_vdk.ldr and of the patched DB's rows; te-boot identical. mkpatch round-trips every build through apply.py. The image build.py writes is apply.py's output, so the tested image is the shared one.
- TE content in the bundle: 400 B of patched pointers and instruction words over stock memory, plus 269 B in 15 runs (≤ 23 B, common instruction sequences) of our code that also occur in stock. No TE code, tables or assets; the key is not needed to build or apply.
- Subset builds link: `wt_data` / `grain_data` are weak in ui.c. Subsets not tried on device.
- iter presets (templates for ours) now come from `src/slot/iter_presets.json` (op1repacker, MIT) instead of third_party/.
- `firmware/images/MODS.OP1` sha256 dbd7c8efff937378a8de4f47760ed679e0b2c72f71d6013737bbfea1c3a85181 (SVG8 without --svg-cost; DB pages laid out by apply.py).
- Result: _pending user flash_.

## 2026-10-13 — Test image 50: OVERDUB.OP1 (engine screens without titles)
- va, wavetable and grain screens: the title (wave name, bank name, grain source / SAMPLER n) is removed; the pictures already show what the knobs select. FX screens unchanged.
- README screenshots in `docs/screens/` from the host preview (`CLEAN=1`: no HOLD/CHORD badges, cropped to y < 142).
- `firmware/images/OVERDUB.OP1` sha256 8d5185b11ed6e4928b2634cdccd7b15b6f30d65762ade53fa8bbcd877d2dd36b (`build.py --version 1.0`). Supersedes MODS.OP1 as the release candidate.
- Result: _pending user flash_.

## 2026-10-13 — Test image 51: OVERDUB.OP1 (grain screen A)
- grain screen: green (grain size) is a shaded dark-green band around the playhead, one grain wide, with the bars inside drawn green; the Hann window and the orange spray bracket are gone. Live grains are white while they read at the playhead and orange when the spray threw them elsewhere (outside the band, up to 3 band-halves ahead for the drift). With no notes sounding, three orange pop-outs show where the spray would throw grains, moving about 4 times a second. True scale (whole bank across the screen), as the user chose.
- GitHub page (`docs/index.html`, served from /docs), background #161618.
- Custom wavetable research and simulator prototype: notes/wavetable-custom.md, src/engine/wt_import.h, src/engine/test/wt_import_*.
- `firmware/images/OVERDUB.OP1` sha256 77b752ec8123068239ba2e7d79b0fa68592f260a7effee5dce4ce2a755a74384 (`build.py --version 1.0`). Supersedes image 50 as the release candidate.
- Result: works on device (user: grain screen looks good).

## 2026-10-13 — Test image 52: WTS1.OP1 (wavetable SAMPLE bank)
- Design: notes/wavetable-custom.md. Green fully clockwise (>= 31744) selects bank 8, SAMPLE: a bank built from the first other sound slot that holds the sampler (grain_source.h), trimmed window, 12 frames.
- `src/engine/wt_user.h`: shared header + two banks at `_wt_ram` = 0x02CC4000 (FREE region 2; engines.json "ram", reserved; first 256 B zero-filled by a FILL block so `front` = 0 at boot). wavetable `max` lowered to 0xC4000 (engine ends 0x02CC1FEF).
- wt.c: base pointer per block (`wt_bank_base`); SAMPLE plays the published bank, else bank 0. Existing banks unchanged (wt_check: all passed, max 47,757 cycles per call).
- ui.c `wt_user_tick` (UI thread, wavetable screen only): signature of slot, window, peak table and 64 samples; rebuild 10 frames after it settles, one `wti_step` per frame (<= ~3.2 M sim cycles), publish by one store into `front`; a change mid-build abandons it; the last bank stays if the sampler goes. 9 bank pips; the 9th blinks while building, off-red when SAMPLE is selected and nothing is built.
- build_slot.py: engines may declare "ram"/"ram_size" (reserve, 256 B zero FILL, `_<source>_ram` defsym in both links); build.sh takes `ENGINE_DEFSYMS`.
- Not covered by a test before the device: the tick/publish logic itself (host preview is 64-bit and can't fake the sampler's 32-bit pointers); the builder (wt_import.h) passed its 52 simulator checks.
- Test: (1) a sampler sound with a sustained pitched note in another slot (e.g. record a held synth note); (2) wavetable, green fully clockwise: pip blinks briefly, then the sample's timbre plays at keyboard pitch; blue morphs through it; (3) move the sampler's trims: rebuild after a moment; (4) other banks unchanged; (5) no sampler slot: off-red pip, bank 0.
- `firmware/images/WTS1.OP1` sha256 8aeba9a282e088c5c91950ac58fe6b1cf775451b9dc5a53625870bab850c8cb6.
- Result: works on device (user, tested via WTS2: "it all works well").

## 2026-10-13 — Test image 53: WTS2.OP1 (wavetable bank names on change)
- WTS1 + the wavetable screen shows the bank's name (ANALOG … DIGITAL, SAMPLE, or NO SAMPLE when SAMPLE has nothing built) in green, top left, for 45 screen frames (~1.5 s) whenever green changes the bank, and on first show; hidden otherwise. User: hard to tell the banks apart without names.
- `firmware/images/WTS2.OP1` sha256 cefcbd915bae3eff09d249d06ecec91e542c7161ffab9cd2bbe17f62f32566ff.
- Result: works on device; names make the banks easy to tell apart.

## 2026-10-13 — Test image 54: VA2.OP1 (va without the filter; sine, sub, noise)
- User: take out the filter (the filter FX does that), add sine, sub and noise.
- va.c: blue wave sine → saw (first third, in phase with the saw's fundamental so the morph doesn't dip) → square → 10% pulse; green detune unchanged; white noise (LCG in state bytes 4..7, never reset; level squared); orange sub, a PolyBLEP square locked an octave below osc 1 (osc 1's phase halved, top bit toggled on each wrap; cleared on note-on). Output scaled by 1 / (1 + (sub + noise) / 2), then the soft knee (a sine stays pure). Filter, va_tables.h and the UI's filter tables removed.
- va_check: all passed (sine harmonics < -119 dB, sub at 220 Hz for A440 with square H3 -9.9 dB, noise broadband, levels 3000..26000 except the pulse's 1.8 peak in the knee, max 40,677 cycles per call = 318 per sample).
- Screen: oscillator shape sampled (sine curves), detune ghost; right panel: the sub's square in orange over four faint oscillator periods, animated white noise below; heights follow the levels.
- Presets: saw lead [10923, 6000, 0, 12000], detune pad [10923, 22000, 2500, 0]. Sounds saved with the old va keep their knob values, which now mean noise and sub.
- `firmware/images/VA2.OP1` sha256 10083c432e1d5f1cf266aade33479570abde48a675544c3f38590c9633b597b7 (includes WTS2).
- Result: _pending user flash_.

## 2026-10-13 — Test image 55: AMPPROBE.OP1 (can an engine hear the input?)
- First step towards a guitar-amp engine. Research: notes/input.md. TE's mono int16 input `0xFF800CB4` (64 samples, 44.1 kHz) is written at the start of every audio callback, before the voice loop, from the completed SPORT0 RX half (int32 L/R at `0xFF800370`/`0xFF800570`); it is filled even with the input off (only the meter and the mixing are gated).
- `inprobe` test engine (src/engine/inprobe.c, inprobe.h; engines.json `"test": true`, left out of default builds): blue gain 0..+24 dB; green SILENCE / TONE / TE IN / RAW / RAW OTHER / ALT (`0x01AB2E2C`); white L+R / L / R; orange shift 0..8 bits for the int32 sources. 2x linear upsampling, soft knee; every sounding voice plays the input. Preset "input" (TE IN): LFO and FX off, full sustain (add_engine_db.py `--patch`, engines.json `"patch"`).
- Screen: source name and address, the block after the gain, peak meter (dBFS, red held peak), frame counter `F n`, LIVE / STILL (block changed since the last frame) with `n/32`, `W0` = first raw word in hex, `R n` = render calls since the last frame.
- inprobe_check.py: all passed (bit exact for every source, channel and shift; guard; max 8,367 cycles per call; ABI). va_check unchanged.
- `firmware/images/AMPPROBE.OP1` = `build.py --engines grain,inprobe --name AMPPROBE` (grain id 15, inprobe id 16, all FX, HOLD/CHORD). sha256 fa9d2fa3d99aa1c871446a63addb77f4a3a4f5b848bff493bccd83d4ff105c9e.
- Test:
  1. Plug a guitar into the line input (through a pedal or DI; a passive guitar straight in will be quiet), or use the internal mic. Headphones are safest (speaker + mic can feed back).
  2. Select the synth engine `inprobe` (preset "input"; green on TE IN). Hold a key and latch it with SHIFT+T2 (HOLD).
  3. Turn green through every source: SILENCE, TONE, TE IN, RAW, RAW OTHER, ALT. On RAW try white L / R / L+R and orange 0 vs fully up. Raise blue if quiet.
  4. Release HOLD (no note sounding) and watch the screen again.
  5. Repeat in tape mode (record a few seconds on a track) and with the sampler (record input into a sampler slot), then come back to inprobe.
  6. Press the mic key (input on/off) and plug/unplug the line cable and headphones; note any change.
- Report, per source: (a) does the waveform move and does LIVE stay green (`n/32` near 32)? (b) peak meter reading and `W0` (a few values); (c) do you hear it, and is it doubled (TE's own monitoring plus ours), delayed or distorted? (d) TONE: audible? (e) with no key held: does the meter/waveform still move, and does `R` drop to 0? (f) any difference between mic and line, input on/off, headphones in/out, tape mode, after sampling; (g) any crash or glitch.
- Result: _pending user flash_.

## 2026-10-13 — Test image 56: DRUMPROBE.OP1 (can we add a drum engine?)
- Research and route: notes/drum-engine.md. New drum engine id 18 (`dprobe`, after va 15, wavetable 16, grain 17) on TE's dbox voice objects with our vtable; patched: name table, drum factory (assert path -> our stub), drum ScreenId (9..18 -> dbox screen), drum encoder (ids > 8 take dbox's 8-knob path), patch validator (18 validates as dbox), dbox screen update (our id -> our screen). TE's drum sampler and dbox code paths are unchanged.
- `src/slot/drumprobe.c` (test engine): per key a sine with a pitch drop (E1 + key semitones), noise burst on odd keys, velocity scales the level; knobs edit the last-pressed key: blue tune, green decay, white pitch drop, orange noise level, SHIFT+blue noise decay. TE doesn't load/save per-key values for a new type, so the factory re-initialises them (marker in key 24's SHIFT+orange value). Screen: what TE handed the engine (see the test). Sim `drumprobe_check.py`: all pass, max 12,700 cycles per render call.
- DB: the dbox preset `whathump` is replaced by `drum probe` (keeps 24 factory drum presets: factory + saved drum sounds must stay <= 58 or the OP-1 asserts at boot).
- `firmware/images/DRUMPROBE.OP1` = `build.py --engines va,wavetable,grain,dprobe --name DRUMPROBE` (all FX, HOLD/CHORD). sha256 b02ee9beab5ecd08e6dd2c650af4fe29717b029ebc2c3de6caadb1e0c20e6299.
- **Before flashing any other image or stock afterwards**: load a stock kit into every drum slot that holds `drum probe` and delete drum sounds you saved with it. Every other image asserts at boot on them (patch.cpp:221); recovery is COM at power-on, then factory reset.
- Test:
  1. Boot; play a synth engine, a drum sampler kit and a dbox kit (unchanged?).
  2. Drum mode: open the drum sound browser and load `drum probe` (dbox folder, where `whathump` was). Note where it is listed.
  3. Play all 24 keys left to right: low thumps rising a semitone per key; every other key adds a noise burst. Screen: `KEY` (0 = leftmost), `VEL`, `V` (voice 0..11), `ON`/`OFF` counts, bars (voice levels), `R` (render calls per frame).
  4. Press a key, then turn blue / green / white / orange, then the same with SHIFT (K4..K7). Each changes only the last-pressed key (`EDIT`). Note each K row's first number (what the knob wrote), second (what the engine rendered), and `min..max S` (dbox's range row), and `ROW`.
  5. With a MIDI keyboard (notes 53..76): play soft and hard.
  6. Save the sound, load another kit, load the saved one; power off and on with it in a drum slot.
- Report: which keys sound and the pitch order; noise on alternate keys; whether each knob changes the sound and the K values; the K ranges; velocity (MIDI); save/load/reboot; any crash or assert text (photo).
- Result (2026-10-14, device): boots; synth, drum sampler and dbox unchanged. `drum probe` is listed alphabetically in the dbox folder (row 4). All 24 keys play, pitch rising per key, noise on alternate keys. Knobs edit only the pressed key and are audible. Screen photo: ID 18, IN 3, ROW 1 (row 9 copied); dbox ranges K0 -24320..24832 (S 6208), K1-K6 0..32767 (S 10485, K4 4915), K7 S 8192; worst render 29,820 cycles per call (233 per sample, budget 76,800). Rendered values lag the written ones until the key is hit again (note-on reads the params). Velocity over MIDI not tested.
- **Saving fails**: saving the sound doesn't create a snapshot. Per-key edits survive switching drum slots (they stay in `0x01AAADB4`). Hypothesis: TE's saver writes no `dbox_data` for type `dprobe` (it only writes it for "dbox"), so the saved sound fails dbox validation and is dropped. Next: make the per-key loader/saver treat `dprobe` as `dbox`.
- Reboot with the probe in a drum slot: boots, the probe is still loaded in the slot.

## 2026-10-14 — Test image 55 result, and 55b: AMPPROBE2.OP1
- AMPPROBE (55) on device: stops at boot with `..\src\db2\patch.cpp:390 Invalid synth patch inprobe:input - error 21` (photo). Cause (Hypothesis, strong): the probe preset's `--patch` set ADSR attack to 0; all 169 stock synth presets have attack >= 64 (stock ranges: adsr min [64, 64, 0, 64, 2048, 64, 4000, 4000], max [13888, 16320, 32767, 16320, 14336, 12864, 31743, 32767]). TE validates factory presets' fields and asserts.
- Fix: attack 64. `add_engine_db.py` now refuses any `--patch` list value outside the stock presets' range for that field.
- `firmware/images/AMPPROBE2.OP1` sha256 86ee1c9e40b502ac24734371a9f41aa205cb554e0f77223faadbb4d6485eca00 (grain + inprobe; otherwise as 55). Test steps as 55.
- Result: _pending user flash_.
- AMPPROBE2 on device: boots past the inprobe preset, then stops with `Invalid drum patch 20760319_2255: error 1` (unknown engine name). That is the drum sound the user saved during DRUMPROBE (56): the save did write it, it just wasn't listed. TE validates saved drum sounds at boot, so every image without `dprobe` stops on it.
- 55c: `firmware/images/AMPPROBE3.OP1` = grain + inprobe + dprobe, built from commit 5f0dde5 in a clean export (the working tree held the save probe's unfinished edits). sha256 142eedeb8b5896aed72f05c3ac9764915b726683de92be679dfb315dfae46d1d. dprobe is drum id 17 here (18 in 56); sounds store the name, not the id.
- Lesson: before leaving an image with a drum engine, the saved drum sounds naming it must be deleted, and DRUMPROBE didn't list them. Until the save fix lands, every test image should include `dprobe`.
- Result: _pending user flash_.
- AMPPROBE3 on device: stops with `patch.cpp:390 Invalid synth patch wavetable: - error 1`: a synth slot holds a wavetable sound and AMPPROBE3 has no wavetable. **Rule: every test image must register every engine name the user's slots and saved sounds use** (now va, wavetable, grain, inprobe, dprobe).
- 55d: `firmware/images/AMPPROBE4.OP1` = va, wavetable, grain, inprobe (synth 15-18) + dprobe (drum 19), from commit 72feb26 in a clean export, with the wavetable preset `digital` left out for this image only (177 > 176 synth presets otherwise; 176 exactly now). sha256 d5bcc178adae5eb10466049255a3a1627fd9152704858cf4e1ffaf235e762faa. First image with a 4th synth id (18).
- Result: _pending user flash_.

## 2026-10-14 — Test image 57: SAVEPROBE.OP1 (drum save/load; synth knob hook and 8-knob storage)
- Research: notes/drum-engine.md "Save and load", notes/knob-hook.md.
- A (drum save/load). DRUMPROBE's save did write the sound (device: `20760319_2255`); the static read agrees: the snapshot path has no type check, and a failed validation would have asserted right after the INSERT. What was lost: the per-key edits (TE's saver writes `dbox_data` only for type "dbox", so the snapshot kept the preset's whathump data), and on load TE's loader skipped `dbox_data` and the probe's marker check rewrote the defaults. Fix: the "dbox" compares in TE's per-key loader (`0x01A00630`) and saver (`0x019FFFBA`) go through `_drum_type_eq_load/_save` (entry.inc): TE's result first, unchanged for "drum"/"dbox"; then a second compare with "dprobe". The marker logic is removed; the `drum probe` preset now carries the defaults in its `dbox_data` (engines.json `dbox_default`, checked against stock dbox ranges). A dprobe sound without `dbox_data` loads as zeros, no assert (jsoncpp null handling, Confirmed by reading). Screen adds `LD` / `SV` (loader/saver took the dbox path for our type) and `SUM` (checksum of the slot's 24 x 8 values).
- B (knob hook). New test synth engine `sprobe` (src/engine/sprobe.c): type-3 range row (knobs 0..5 0..32767, 6..7 -32768..32767), so SHIFT gives K4..K7 independently. The encoder CALL at `0x01A5063E` goes to `_knob_hook`: other engines reach TE's `FUN_01a1dfec` untouched; for sprobe, knobs 0..5 still go through TE's function, and the hook writes K6 = edit count + last knob (always negative) and K7 = f(K6) (raw >= 0x8000), which TE's `FUN_01a00d14` stores. Sound: FM sine; blue pitch, green level, white FM index, orange ratio, SHIFT+blue = octave-up sine level. Screen: `HOOK` calls since boot, last knob/delta/slot; `WROTE` K6 K7 and `NOW SAME/DIFF`; `EDITS` and `PAIR OK/BAD` from the stored values (survive a reboot); per knob the stored value (hex, decimal) and `L` = the value the last render got (after TE's LFO; white when it differs). Preset `knob probe` (LFO off).
- Sim: sprobe_check, knobhook_check (both hooks, register preservation), drumprobe_check, va/wt/grain/inprobe checks, fx_abi_check: all pass. Host preview checked (engine 20, and 300).
- `firmware/images/SAVEPROBE.OP1` = `build.py --engines va,wavetable,grain,inprobe,dprobe,sprobe --db-trim wavetable=6 --name SAVEPROBE`: synth ids va 15, wavetable 16, grain 17, inprobe 18, sprobe 19; dprobe drum id 20. Wavetable presets `bell keys` and `digital` left out for this image only (176 of 176 synth presets). te-boot identical to stock. sha256 499f6a557a09ca103bce69934eedb6f621acd00822f4005206298cf69ef36e65.
- Release builds (va, wavetable, grain): same patch sites (94) and DB; the image bytes change because ui.c grew (the sprobe screen code moves later slot symbols).
- **Don't save synth sounds with `sprobe` or `inprobe` unless the next image includes them.**
- Test A (drum):
  1. Boot. Note whether `20760319_2255` (DRUMPROBE's save) is listed in the drum browser, and in which folder.
  2. Drum mode: load `drum probe` (dbox folder) into a drum slot. Screen: note `LD`, `SV`, `SUM`. Play keys; they should sound as in DRUMPROBE.
  3. Press key 3, turn blue (tune) and green (decay) clearly; press key 10, turn white. Note `SV` (should count up per turn) and the new `SUM`.
  4. Save: hold the slot's key until the save completes. Note the name it gets and where it is listed in the browser.
  5. Load another kit into that slot, then load the saved sound. Note `LD` (should count up), `SUM` (should equal step 3's), and whether keys 3 and 10 sound edited.
  6. Power off and on with the saved sound in the slot. Note `SUM`, `LD`, and the sound of keys 3 and 10.
  7. Load `20760319_2255` (if listed): note `SUM`, and that it boots/plays.
  8. Load a stock dbox kit and a stock drum sampler kit: they must sound and edit as before; save one dbox snapshot, reload it, check its edits survive (unchanged TE behaviour).
- Test B (knobs):
  1. Synth: select `sprobe` (preset `knob probe`). A sine sounds. Note `HOOK` (0), `PAIR` (-).
  2. Turn each plain knob: blue pitch, green level, white FM, orange ratio. `HOOK` counts, `EDITS` counts, K6/K7 change, `NOW SAME`, `PAIR OK`.
  3. Hold SHIFT and turn each knob: K4 (blue) changes alone and adds an octave; K5 (green) changes alone; SHIFT+white/orange change only the EDITS/K6/K7 (`KNOB 6/7`). Check K0..K3 don't move with SHIFT.
  4. Save the sound (hold the slot key). Note K0..K7 (hex) and EDITS.
  5. Load another sound into the slot, then reload the saved one: K0..K7 equal to step 4? `PAIR OK`? `NOW` (SAME if values came back as written).
  6. Power cycle with the sound in a slot: K0..K7 and `PAIR` (`NOW -`, `HOOK 0` after a reboot is expected).
  7. LFO: T4, choose the element LFO, target each of knobs 4..7 in turn (note how you picked the target), play a note: does `L` for that knob move (white), and do you hear the octave pulse for knob 4? Does the stored K value move too?
- Report: A: LD/SV/SUM at each step, the saved names and the folders they are listed in, whether edits are audible after reload and reboot, whether `20760319_2255` is listed; B: HOOK/EDITS/K0..K7/PAIR/NOW at steps 2-6, SHIFT independence, the LFO result per knob 4..7; any crash or assert text (photo).
- **Cleanup before leaving this image** (stock and other images stop at boot on unknown names): replace `drum probe` in every drum slot with a stock kit; delete every saved drum sound of type dprobe (including `20760319_2255` and today's saves); load another engine into every synth slot holding `sprobe`, and delete synth sounds saved with `sprobe`. If a saved probe sound can't be found in the browser, report it before flashing anything else (recovery otherwise: COM at power-on, factory reset).
- Result (2026-10-14, device): **all passed.**
  - A (drum): `20760319_2255` is in the snapshots. `drum probe` loads with LD 4, SV 0, SUM A771. Edits count SV. Save goes to the snapshots; reload restores SUM and the edits sound; survives a power cycle. Stock dbox and sampler kits unchanged.
  - B (synth): the hook runs (HOOK/EDITS count, NOW SAME, PAIR OK). SHIFT knobs are independent (K4-K7 move alone). Packed raw values survive save → reload (snapshot) and a power cycle.
  - TE's element LFO can only target knobs 1-4 (that is how TE's LFO works), never 5-8. So LFO-able parameters must live in knobs 0..3; packed data in 4..7 is safe from the LFO.
- **AMPPROBE4 result (2026-10-14, device): boots with all the user's sounds.** Guitar into the line jack:
  - TE IN (`0xFF800CB4`): the waveform moves whether or not a note is held (the buffer is live without a voice), but the input is heard only while a note sounds. TE doesn't monitor the line input itself here, so there's no doubling. Sound clean; feels instant; recorded to tape through the engine.
  - Peak about -15 dBFS when strummed hard at minimum gain.
  - RAW (L+R, shift 0): moves, about the same level as TE IN.
  - Toggling the mic/input key changes nothing.
  - Conclusion: an amp engine can read TE IN directly. The open design point is gating: engine output passes the note envelope, so the amp needs a latched note (HOLD) or a way round the envelope.

## 2026-10-14 — Test image 58: AMP1.OP1 (guitar amp engine)
- Design: notes/amp.md. `amp` test engine (engines.json `"test": true`; synth id 20 here) at `0x01EF8000`, shared RAM `amp_ram` at `0x01F00000` (FREE region 1). TE IN → gate → 2x half-band upsampler → tight high-pass → pre-emphasis → drive (biased tube stage, 7 kHz interstage, second soft stage) → tilt tone → cab (resonant high-pass, mid scoop, presence, two low-passes) → loudness → limiter (−3 dBFS) → soft knee.
- Knobs: blue drive (0 → +44 dB, clean → crunch → lead, loudness compensated), green tone (dark … flat … bright tilt), white gate threshold (off, then −84 → −30 dBFS; 6 dB hysteresis, 50 ms hold), orange cab (direct → 1x12 → 2x12 → 4x12).
- Presets (full sustain ADSR as AMPPROBE2's, LFO off): `clean` [1500, 18000, 0, 11264] with TE's spring (stock `princess` FX values), `crunch` [15000, 17000, 8000, 22528], `lead` [27000, 15500, 14000, 32767], `fuzz` [32767, 21000, 16000, 22528], `ambient clean` [3000, 21500, 0, 11264] with TE's delay (stock `daylight` FX values). New per-preset DB patches (`"preset_patch"`; add_engine_db.py `--preset-patch`: range-guarded, and fx_params must equal a stock preset's for that FX type).
- One voice plays the input: owner = newest note-on, else the held voice when the owner is released (byte 28; Hypothesis), else whoever renders when the owner skipped a block; gains crossfade over 8 blocks (11.6 ms) towards the owner at the block's start, so they always sum to one. Blocks are detected from TE IN / `te_rx_sel` changes, so TE's unexplained second render call per block can't double the input.
- Screen (user's direction): a blackface combo front. Blue: cabinet rumble and sparks (drive x input level, only while a note sounds); green: grille sparkle and speaker glow, dark/warm → bright/airy; white: gate mark on the panel level meter and gate lamp; orange: speakers behind the cloth (DI, 1, 2, 4); red pilot jewel pulses with the input; tuner top left only while no note sounds (UI thread polls 13 blocks of TE IN every 4th frame when there is signal; YIN).
- Sim: amp_check.py all passed (presets −10.8 … −18.9 dBFS rms for a −15 dBFS note, no rail hits; 0 dBFS in held by the limiter; THD −46 → −7 dB along blue; cab/tone within 0.7 dB of the design; gate hysteresis and 55 ms close; one voice per block for chords, handovers within 1 LSB of a single voice, double renders and a constant `te_rx_sel` bit-identical; output guard; **73,442 cycles per DSP call** (budget 76,800; one voice per block pays it); ABI). amp_tune_check.py (host): drop D … E6 within 5 cents, median ≤ 3; silence/noise unpitched. inprobe_check and va_check unchanged (pass). distortion.c untouched.
- `firmware/images/AMP1.OP1` = `build.py --engines va,wavetable,grain,inprobe,dprobe,sprobe,amp --db-trim wavetable=1 --name AMP1` from commit a409d0b in a clean export: synth ids va 15, wavetable 16, grain 17, inprobe 18, sprobe 19, amp 20; dprobe drum id 21. Wavetable keeps only `analog sweep` in this image (176 of 176 synth presets). te-boot identical to stock. Includes SAVEPROBE's drum load/save wrappers (verified on device, 8e65394). sha256 15f244d1031d65280cd5862bb2d7180aaecacf3831f3b3bfa766b64f37a177c9.
- Test (guitar through a pedal or DI into the line jack, headphones):
  1. Boot; your saved sounds load (all engine names registered). Synth: select `amp`, preset `clean`. With no key held, pluck a string: the tuner (top left) shows note and cents; tune a string. Silence: "--".
  2. Hold a key and latch it with SHIFT+T2 (HOLD). The tuner hides; play: clean with spring reverb. The pilot jewel pulses.
  3. Try each preset (`crunch`, `lead`, `fuzz`, `ambient clean`), and each knob: blue (clean → crunch → lead; cabinet rumbles and sparks at high drive when you play hard), green (darker/brighter; the grille follows), white (gate: raise it until the hiss between notes goes; check pick decay isn't chopped), orange (cab: DI, 1x12, 2x12, 4x12 speakers).
  4. Release HOLD, then hold one key, add a second and a third (a chord): the guitar must not get louder or phasey; release keys in different orders; no clicks or level jumps, the guitar stays as long as one key sounds (or its release).
  5. Record a few seconds to tape through the amp; play back.
- Report: the sound per preset and knob (too quiet/loud? gate behaviour, noise, harshness), the tuner's readings vs a known-good tuner (note, cents, stability, low E), whether chords duplicate or click at handovers (which order of key presses/releases), the screen (readable? rumble/sparks feel right? smooth?), any audio dropouts or crackle at high drive (CPU), any crash or assert text (photo).
- **Don't save sounds with `amp`, `sprobe` or `inprobe` unless the next image includes them.**
- Result: _pending user flash_.

## 2026-10-14 — Test image 59: SYNTH1.OP1 (harm and fm4)
- Integration and decisions: notes/harm-fm4.md "Integration as built"; hook: notes/knob-hook.md "Several engines". Commit 27ffec6.
- `harmonic` (source harm, drawn harmonics) and `fm4` (Operator-style FM, packed operators with per-operator pages), both test engines with type-3 range rows and the encoder hook (now for several engines: `KNOB_HOOK 19, 21, 22`, knobhook.c dispatches by `_hook_kind`).
- **LFO**: TE's LFO only modulates the per-voice params copy (`0xFF801724`; all six of its knob calls read), never the stored knobs, so packed values can't be corrupted; the hook and both screens use the stored knobs only; harm ignores params (an LFO does nothing on harm); fm4's knobs 0..3 are plain and LFO-able, 4..7 (packed) are out of the LFO's reach (knobs 1-4 only, image 57).
- **Encoder delta**: still unknown; every hook call with a nonzero delta is at least one step (cursor, PAINT, STAMP, page: exactly one), multiples of the smallest |delta| seen since boot give 2, 4 (capped per control). Grey `D<last raw delta> M<smallest>` under harm's readout and bottom right on fm4, once the hook has run.
- **harm table rebuild** (~4.3 ms on the UI thread): at most every 4th screen frame while editing; edits coalesce, the last table keeps playing (crossfade on publish).
- **fm4 screen**: the user's gear design (operators as gears sized and sped by ratio, tooth bite = level x INDEX with the driven gear jittering, carriers' brightness = level, spin-up and wind-down with each operator's envelope read from the rendering voices, still with no note, feedback as an orange pawl on D).
- Sim/host: harm_check, fm4_check, sprobe_check, knobhook_check (harm/fm4 dispatch with deltas 300..4096, TE's delta 0 vs passed through, an unhooked id untouched, ABI), synth_edit_test (1023 checks, incl. delta-to-steps for 1..32767 per detent), screens_preview (rate limit: 15 builds in 61 turning frames, last edit published 3 frames after), va/wt/grain/inprobe/drumprobe/amp checks, fx_abi: all pass. Both screens rendered through ui.c's host preview (engines 22, 23).
- `firmware/images/SYNTH1.OP1` = `build.py --engines va,wavetable,grain,inprobe,sprobe,amp,harmonic,fm4,dprobe --db-trim va=1 --db-trim wavetable=1 --db-trim grain=1 --db-trim inprobe=0 --db-trim sprobe=0 --db-trim amp=1 --db-trim harmonic=6 --db-trim fm4=6 --name SYNTH1` from commit 27ffec6 in a clean export: synth ids va 15, wavetable 16, grain 17, inprobe 18, sprobe 19, amp 20, harmonic 21, fm4 22; dprobe drum id 23. 176 of 176 synth presets: this image keeps only va `saw lead`, wavetable `analog sweep`, grain `choir cloud`, amp `clean`, harm `saw square organ clarinet bell choir`, fm4 `e piano bell bass pluck metallic pad`; inprobe and sprobe have no presets here (their saved sounds still load). te-boot identical to stock. sha256 97337fff80f9dcf5c2aa2ec43a22f8c3ede35c7f601207cba7373d29c20628ca.
- Release builds (va, wavetable, grain): identical DB, same 94 halfword patches; OP1_vdk.ldr bytes change (ui.c includes the two screens).
- Test harm (synth, engine `harmonic`, preset `saw`):
  1. Blue: the blue bracket moves one bar per detent, slow and fast (H1..H24 top left). Note `D`/`M` (grey, under the readout) after a slow single detent and after a fast spin.
  2. Green: the cursor bar's level steps 3 dB per detent (`-9 DB`, `OFF`); fast turns may step 2 or 4. Listen: the harmonic appears/disappears.
  3. White (PAINT): moves the cursor and copies the starting bar's level into each bar it passes (white trail).
  4. Orange: UNISON (orange ghost on the top-right cycle, wider sound). SHIFT+blue TILT (blue caps, `TILT -3 DB/OCT`), SHIFT+green EXTEND (green bars past H24), SHIFT+white STAMP (SAW, SQUARE, TRIANGLE, EVEN, ORGAN, CLEAR; turning back to the start restores your drawing), SHIFT+orange WARP.
  5. While holding a note, turn green and white continuously: the sound should follow within a fraction of a second without clicks or zipper; the screen and other knobs should stay smooth (rebuild cost). Note how fast the sound follows.
  6. Save the sound (hold the slot key), load another, reload it: the drawing, tilt, extend, unison, warp are back. Power-cycle with it in a slot: same. (After a reboot the drawing plays as a sine until the harm screen has been shown once: known.)
  7. LFO (T4) on knobs 1-4: nothing should change on harm (expected), and the drawing must stay intact.
- Test fm4 (engine `fm4`, preset `e piano`):
  1. Main page: blue INDEX (gears' teeth mesh deeper, modulator gears brighter blue, brighter sound), green ALGORITHM: step through all 11 (`ALG n` green; the gear train rearranges: stacks, side-by-side carriers, one gear driving several), white TIME (`TIME xN`; envelopes shorter/longer), orange FEEDBACK (orange pawl on D, grittier sound).
  2. Play notes: gears spin up, plucky operators flick and stop, flat ones keep turning while held, everything still with no note.
  3. SHIFT+blue: pages A, B, C, D (the selected gear white, `C 4.41` / `LVL 11 SWELL` top right), one page per detent; on a page blue COARSE, green FINE, white LEVEL, orange ENVELOPE of that operator; back to MAIN with SHIFT+blue down. Try each preset.
  4. Save, load another, reload; power-cycle: operator settings back.
  5. LFO (T4) on knobs 1-4: should modulate INDEX / ALGORITHM / TIME / FEEDBACK; operator settings unchanged afterwards.
- Report: the `D`/`M` values (slow single detent, fast spin, on both engines) and whether cursor/level/page steps feel right; harm: drawing/sound match, stamp/undo, rebuild responsiveness, clicks; fm4: all 11 algorithms sound and draw distinctly, gear motion (readable? too busy?), operator pages; save/reload/reboot for both; LFO results; any crash, freeze or assert text (photo).
- **Don't save sounds with `harmonic`, `fm4`, `amp`, `sprobe` or `inprobe` unless the next image includes them.**
- Result: _pending user flash_.

## 2026-10-14 — Test image 60: SYNTH2.OP1 (SYNTH1 + the redesigned amp screen)
- Same engines, ids and preset trims as SYNTH1 (59), built from commit 1ec16dd (amp screen: small oblique combo, src/slot/ui_amp.h) in a clean export. te-boot identical to stock.
- `firmware/images/SYNTH2.OP1` sha256 4c6311548bfc6eda0272bb4b4d6eaf578f5ff2b4f2428dba4f19a21eb88d5440. Supersedes AMP1 (58) and SYNTH1 (59) for testing: amp steps as 58, harm/fm4 steps as 59.
- Result: _pending user flash_.

## 2026-10-14 — Test image 61: SYNTH3.OP1 (SYNTH2 + the SVG gear screen for fm4)
- Same engines, ids and preset trims as SYNTH2 (60), built from commit 82f439e in a clean export; adds content/display/fm4.svg (7731 B). te-boot identical to stock.
- `firmware/images/SYNTH3.OP1` sha256 f047fda195e6e2059f0ce2a032f9dd3b9434408ae995afcdd5fb1567298d72d8. Test steps as 58 (amp) and 59 (harm, fm4); also how the gear screen looks and runs on the device.
- Known from the preview: in some algorithms gears overlap rather than mesh (1, 6), labels collide, and sizes differ between algorithms.
- Partial result (2026-10-14, device): boots. fm4 "sounding really good". Encoder delta: **D1024 M1024**, i.e. 1024 per detent on fm4. User wants carriers ("base gear / osc") easier to tell apart, and the screen more TE-inspired, fun and polished. harm, amp: pending.
- Result: _pending user flash_.

## 2026-10-14 — Test image 62: SYNTH4.OP1 (SYNTH3 + fm4 gear screen v4)
- Same engines, ids and preset trims as SYNTH3 (61), built from commit bf3ccfb in a clean export; content/display/fm4.svg is now 22,602 B (78 elements). te-boot identical to stock.
- fm4 screen (notes/harm-fm4.md): SYNTH3's look kept (carriers grey .. white with level, white = the gear you hear; modulators blue), plus equal-pitch gears that truly mesh (3 sizes: 9, 14, 21 teeth by ratio; driven gears phase-locked to their driver), placement and labels that avoid collisions, one scale across algorithms clear of HOLD/CHORD, a small hop when a note starts, TE-style gear art (rounded chunky teeth, inner ring, rounded spokes, ringed hub). Encoder: one step per detent at 1024 per detent (checked).
- `firmware/images/SYNTH4.OP1` sha256 7888a351d8d068b0e0aac500b5f595815e9b51075ca9c0064c3180ba7e5d4fa8.
- Test: as 61; on fm4 check that meshing gears interlock and turn together, white carriers vs blue modulators read at a glance, labels and badges don't collide, sizes feel consistent between algorithms, the screen stays smooth (render cost), and steps are one per detent (page, coarse, fine, level, envelope).
- Result: _pending user flash_.

## 2026-10-14 — Test image 63: DRUMFM1.OP1 (fmdrum: an FM drum kit; SYNTH4 otherwise)
- New drum engine `fmdrum` (notes/drum-engine.md "fmdrum"): per key a 2-op FM drum with its own modulator decay, a pitch sweep and a noise layer; knobs PITCH, RATIO, FM, DECAY, SHIFT: SWEEP, SNAP, NOISE (OFF/AIR/SNARE/HAT/CLAP), MOD. Four factory kits in the dbox folder: `fm 808`, `fm 909` (punch FX), `fm metal` (delay FX), `fm zaps`. Screen: the selected key as a drum (FM rosette, sweep, noise), an envelope graph, its values, the 24 keys. Test engine.
- Several drum engines per image (slot.S DRUM_HOOKS id range, per-engine vtables, type compare per name): dprobe drum id 23 (as SYNTH4), fmdrum 24. Synth ids as SYNTH4: va 15, wavetable 16, grain 17, inprobe 18, sprobe 19, amp 20, harmonic 21, fm4 22; same preset trims (176 of 176 synth presets).
- Drum presets: 28 (stock 24 with whathump -> `drum probe`, + 4 kits): **room for 30 saved drum sounds** (was 34; factory + saved over 58 stops the OP-1 at boot, patch.cpp:656).
- Sim: fmdrum_check, drumhooks_check, knobhook_check, drumprobe_check, fx_abi_check: all pass. fmdrum worst 22,000 cycles per render call (172 per sample) in the sim; DRUMPROBE's device/sim ratio was 2.3x.
- `firmware/images/DRUMFM1.OP1` = `build.py --engines va,wavetable,grain,inprobe,sprobe,amp,harmonic,fm4,dprobe,fmdrum` with SYNTH4's `--db-trim` set, from commit 4c42dc6 in a clean export. te-boot identical to stock. sha256 4682b5c35baee69b5b283c827da8e94863a526e24967baa635649c34e6c0190c.
- **Before flashing**: you need 30 or fewer saved drum sounds (snapshots), or it stops at boot.
- Test:
  1. Boot. Synth slots (va, wavetable, grain, amp, harmonic, fm4 sounds) and drum slots (dbox, sampler, `drum probe`, your dprobe saves) load and play as on SYNTH4.
  2. Drum mode: open the browser; the dbox folder lists `fm 808`, `fm 909`, `fm metal`, `fm zaps`. Load `fm 808`. Play all 24 keys: kicks, toms, snares, clap, rim/clave, hats, cowbell, congas, maracas, cymbal, ping (key list in src/slot/gen_fmdrum.py). Note any key that is silent, too quiet/loud, clicks or sounds broken.
  3. Screen: the title names the last key pressed (KICK, SNARE, ...); the drum hops and ripples, the keys strip lights the keys you play; turn each knob on one key: blue PITCH (drum size, Hz), green RATIO (petals), white FM (petal depth, brighter/clangier sound), orange DECAY (shell depth, orange curve); SHIFT: SWEEP (blue curve), SNAP, NOISE (OFF, AIR, SNARE, HAT, CLAP as you turn), MOD (white curve: the click/brightness decay). Each knob changes only that key. Note whether one detent is a usable step on each.
  4. Hold a long note (metal kit, a bell) and turn each knob while it rings: no clicks or zipper.
  5. Play fast rolls on one key and many keys at once (all 12 voices): no crackle, no stuck notes; the UI stays smooth.
  6. Load `fm 909`, `fm metal`, `fm zaps`; play each.
  7. Edit two keys, save (hold the slot key), load another kit, reload the save: edits back. Power-cycle with it in a slot: edits back.
  8. dbox and sampler kits still sound and edit as before; `drum probe` still loads (its probe screen).
- Report: per kit, keys that sound wrong (and how); knob steps per detent; clicks; CPU (crackle with many voices); screen readability; save/reload/reboot; any assert text (photo).
- **Cleanup before an image without fmdrum**: replace fmdrum kits in every drum slot with a stock kit and delete drum sounds saved from them (stock and older images stop at boot on type `fmdrum`). Same for dprobe sounds as before.
- Result: _pending user flash_.

## 2026-10-14 — Test image 64: SOUND1.OP1 (DSP review pass 1: amp, fm4, grain, harm, va)
- Fixes (commits 36b4044..414abcd), sim before → after:
  - amp: both shapers keep the linear term at full precision (cubic in Q14): clean THD+N at a -40 dBFS sine -29 → -61 dB, aliasing on the clean presets -53 → -83 dB. Cabs: steeper, lower low-passes (1x12 5.0 kHz Q1.1 + 5.5 kHz; 2x12 4.4 kHz Q1.15 + 5.0 kHz; 4x12 3.8 kHz Q1.2 + 4.5 kHz): 8 kHz -8.7/-13.0/-18.9 → -13.7/-17.8/-22.6 dB re 1 kHz. Drive gain and make-up ramp across the block (no level step per detent). Make-up no longer wraps (its Q29 overflowed at blue < ~600: -1.4 dB and inverted polarity there; blue 0 now +1.4 dB). Cycles 73,442 → 73,902 per DSP call (budget 76,800).
  - fm4: gain ramps carry 8 extra fractional bits: block-rate sidebands of a decaying operator -57..-65 → -105..-129 dBc. Tone limiter (1.5 I + 4 sidebands, 4.5 I + 1 below 1 rad): worst alias at C6..C8 with maximum index -45 → -59 dB. Operator increments no longer overflow at ratios near 68 (up to 86 Hz off before). A note-on on a voice that is still sounding (rendered within ~40 ms) restarts only the envelopes and ramps the gains: retrigger step 5,100..9,900 → 280..860 (normal sample steps 150..590). Cycles 28,774 → 29,462 worst (+5 per sample).
  - grain: sizes and density were counted at 44.1 kHz (grains half as long, twice as dense as the knobs say); now 10..500 ms and 2..80 per second at 88.2 kHz. Level uses the overlap that can sound (at most 8 grains): at full size, loudness across density varies 2.4 dB (was 12.5). Soft knee instead of the soft clip (as va/wt). Cycles 53,178 → 51,155 peak.
  - harm: unison normalised by 1/sqrt(1 + g2²): full unison -16.5 → -13.5 dBFS (unison off -13.5).
  - va: oscillator level follows the wave knob: rms saw 79.4, square 84.3 → 79.5, 30 % pulse 83.5 → 79.4, 10 % pulse 79.6 → 77.4 dB, its peak 0.88 → 0.70 FS (out of the soft knee). Cycles +8 per sample (40,408 worst).
- Checks: amp, fm4, grain, harm, va, wt, inprobe, sprobe, modal, wt_import, fmdrum; knobhook, amp_tune: all pass. Test changes: amp_check counts a voice as sounding on any nonzero sample of the block (the first sample alone hit a zero crossing); fm4_check adds a loud-retrigger and a ramp-sideband check and its note-on check now means "no step"; grain_check runs at 88.2 kHz (simrun.FS stays 44.1 kHz for the others: at 88.2 kHz modal's T60s come out at half, sprobe/wt/va expectations move. Hypothesis: modal's decays are designed for 44.1 kHz, so they are half as long on the device; not changed here).
- `firmware/images/SOUND1.OP1` = DRUMFM1's command (`--engines va,wavetable,grain,inprobe,sprobe,amp,harmonic,fm4,dprobe,fmdrum`, same `--db-trim`) from commit 414abcd in a clean export: synth ids va 15 … fm4 22, dprobe drum 23, fmdrum 24; 176 of 176 synth presets, 28 drum presets (room for 30 saved drum sounds). te-boot identical to stock. sha256 e737b5e83f1cf4df784916ee8bda186197ba7f199efacad11c5f75bc7674abde.
- Listening checklist (headphones):
  1. amp `clean`, guitar, soft playing: smoother, no grainy fizz on quiet notes and decays. Turn blue slowly from 0 up: no zipper or level steps per detent, no dip near blue 0. Orange through 1x12/2x12/4x12: less fizz above 5 kHz, still clear. `crunch`/`lead`/`fuzz` should sound much as before.
  2. fm4 `e piano`, `bell`, `pad`: listen to long decays for a faint buzz/whine (block-rate) — should be gone. Play the same key repeatedly fast and hold notes while re-striking: no click on retrigger. High notes with INDEX up: less metallic aliasing.
  3. grain `choir cloud`: grains now longer and sparser at the same knob settings (size and density as labelled); turn white (density) up at large green (size): loudness should stay about even (it dropped before).
  4. harmonic `saw`: orange (UNISON) 0 → max: the level should stay about the same (it dropped ~3 dB).
  5. va `saw lead`: sweep blue through saw → square → pulse: about even loudness (square was clearly louder), thin pulse at the end without extra grit.
- Result: _pending user flash_.

## 2026-10-14 — Test image 65: DRUMFM2.OP1 (fmdrum after the UX review; SOUND1's DSP fixes)
- fmdrum UX review fixes (notes/drum-engine.md "fmdrum"), commit 2e3a9e8: knobs are an amount on each plain knob and its time/partner on SHIFT: blue PITCH / RATIO, green SWEEP / SWDEC (sweep time), white FM / FMDEC (index decay), orange DECAY / NOISE. RATIO steps through the 15 harmonic ratios (TONE) rising, then the 17 inharmonic ones (METAL). Keys 10..13 are a hi-hat choke group (HAT-zone voices cut in ~5 ms). All four kits share key roles: 0-2 kicks, 3/5/7 toms, 4/6 snares, 8 rim, 9 clap, 10-13 hats, 14-15 cymbals, 16-23 percussion/FX (808's fifth kick is now a ride). Screen: white titles; turning a knob shows its name and value in the title for ~1.5 s (e.g. `DECAY 450MS`, `NOISE HAT`, `RATIO 1.41 METAL`); grey labels over the values and a dimmer SHIFT row; times as nnnMS / n.nSEC; noise drawn in white/grey; class colours only in the key strip. The stroke font's S is chamfered now (all screens), so it no longer reads as 5.
- Includes the other agent's DSP fixes (amp, fm4, harm, va, grain; image 64 SOUND1, commit 77a5e32) — they were committed before this build.
- Sim: fmdrum_check (now also the choke, ratio order, stock NOISE maximum, hat keys), drumhooks_check: all pass. Worst fmdrum render 21,000 cycles per call (164 per sample).
- `firmware/images/DRUMFM2.OP1` = DRUMFM1's command (all engines, SYNTH4's trims) `--name DRUMFM2`, from HEAD 2e3a9e8 in a clean export. te-boot identical to stock. sha256 71c4b476262af94d548858c1f048ba394cfccc684e63ddae44ed03f3985eaece.
- **Before flashing**: 30 or fewer saved drum sounds (28 factory drum presets). Kits saved from DRUMFM1 load with the old knob order (their values now mean other knobs): reload the factory kits rather than DRUMFM1 saves.
- Test:
  1. Boot; synth and drum slots load as on SOUND1/DRUMFM1.
  2. Load `fm 808`; play keys 0-23 in order: kicks, toms/snares alternating on 3-7, rim, clap, four hats, cymbal, ride, then percussion. Same roles in `fm 909`, `fm metal`, `fm zaps`.
  3. Hi-hat choke: let the open hat (key 13, F#) ring, hit a closed hat (10-12): the open hat stops at once, with no click. Hitting key 14 (cymbal) must not cut it.
  4. Turn each knob on one key: the title shows the knob's name and value in its colour for ~1.5 s, then the class name returns. Switching keys alone shows no readout.
  5. **PITCH step**: on a `fm metal` bell (keys 16-20), turn blue one detent: how many semitones does it move (1 or 2)? (dbox's encoder scale on K0; 512 units per semitone).
  6. RATIO (SHIFT+blue): one detent per ratio step? Readout says TONE for the first 15, METAL after.
  7. **Snares**: do the snares (keys 4, 6) sound flat or papery (noise and body share one DECAY envelope)?
  8. Save an edited kit, reload, power-cycle: edits back.
- Report: PITCH semitones per detent, RATIO steps per detent, choke behaviour, snare verdict, readout behaviour, any crash or assert text (photo).
- **Cleanup before an image without fmdrum**: as DRUMFM1 (fmdrum and dprobe sounds in slots and saves).
- Result: _pending user flash_.

## 2026-10-15 — Test image 66: AMP2.OP1 (DSP review pass 2: amp aliasing and blackface tone)
- amp only (notes/amp.md "Chain"); commits 8601fc2, 65fa664, 2603d73. Simulator, before → after:
  1. The seven SVFs in hand-written Blackfin assembly (each sum of two 32 × 32 products in one accumulator chain,
     coefficients and samples loaded in parallel slots): 34 → 17..25 instructions per sample; 73,902 → 63,889 cycles
     per DSP call. Not bit-exact (one truncation per sum instead of two): closer to a double reference than the C was
     (rms error, Q24 LSB: cab high-pass 534 → 385, bells 61 → 43 / 6.6 → 4.8, low-passes 22 → 15).
  2. First-order ADAA on both shapers (distortion.c's tube kernel and FIX, at most 32 straddling pairs per stage and
     block; stage 2 is the tube at 1.5× its input), the loop split into passes, upsampler / drive / interstage in
     assembly. Aliasing at maximum drive (DI, tone centre, the review's metric): 997 Hz −36.4 → −50.8 dB, 2489 Hz
     −24.9 → −43.7 dB; fuzz preset −37.4 / −32.0 → −52.6 / −51.8; lead −47.0 / −38.6 → −63.2 / −56.9. Clean THD+N at
     a −40 dBFS sine −61.3 → −62.4 dB (blue 0), −58.6 → −59.5 (clean preset). 63.9k → 53.9k cycles. The gate gain now
     multiplies the drive gain (after the linear tight / pre-emphasis filters).
  3. Blackface tone: the two shelves plus a fixed −8 dB bell at 450 Hz (Q 0.7) between the interstage low-pass and
     stage 2, scaled to a peak gain of 0 dB like a passive stack (bright drives stage 2's highs 10 dB less than a
     +10 dB shelf would) and made up after stage 2 (dark fully; bright fully at clean, less with drive), so tone 0 /
     32767 stay about +4 / −4 dB from the centre at every drive, as in pass 1. The interstage got a second pole at
     7 kHz (with the scoop, stage 1's aliases weigh more: float model 997 Hz −42.9 → −53.1 dB). Stack response (dB at
     60 / 200 / 450 / 1k / 3k / 8k Hz): dark +4.6 −0.2 −7.9 −4.3 −7.4 −9.7, centre −0.3 −3.1 −8.0 −3.2 −0.4 −0.1,
     bright −5.3 −6.1 −8.1 −2.1 +6.6 +9.6. Cab bells unchanged.
- Final: aliasing at maximum drive, tone dark / centre / bright: 997 Hz −64.3 / −55.8 / −51.3 dB, 2489 Hz −49.3 /
  −55.5 / −42.7 dB. Presets (−15 dBFS note, rms) clean −18.5 → −18.2, crunch −14.0 → −13.4, lead −10.8 → −10.6, fuzz
  −14.4 → −13.6, ambient clean −18.9 → −18.8 dBFS (COMP_DB re-measured). Clean THD+N at −40 dBFS −60.5 / −57.3 dB:
  the 2 dB over step 2 is the scoop cutting the 1 kHz test tone against stage 1's 2nd harmonic; the off-harmonic floor
  stays at −83 dB. THD vs blue −50.5 … −5.1 dB (was −45.8 … −7.4).
- Cycles per DSP call: 57.7k for guitar (amp_check max 58,066), **61,854 worst** (noise or a 0 dBFS 9 kHz sine at full
  drive: FIXMAX hit in both stages); budget 76,800: 14.9k (19 %) margin at the worst case (pass 1: 73,902, 3.8 %).
  Every pass now reads and writes a 128-word buffer in amp_ram (SDRAM).
- Checks: amp (new: aliasing at maximum drive across the tone knob, the stack's mid scoop −6.4 dB at 450 Hz re
  100 Hz / 3 kHz vs design −6.4, the ADAA pair average, the stack and the two-pole interstage in the expected
  responses), amp_tune, fm4, fmdrum, grain, harm, inprobe, modal, sprobe, va, wt, wt_import, distortion, drumhooks,
  drumprobe, ensemble, fx_abi, knobhook, pingpong: all pass. Knob mappings, presets, the amp screen and the tuner
  unchanged.
- `firmware/images/AMP2.OP1` = `build.py --engines va,wavetable,grain,inprobe,sprobe,amp,harmonic,fm4,dprobe,fmdrum
  --db-trim va=1 --db-trim wavetable=1 --db-trim grain=1 --db-trim inprobe=0 --db-trim sprobe=0 --db-trim amp=1
  --db-trim harmonic=6 --db-trim fm4=6 --name AMP2` from commit 2603d73 in a clean `git archive`: synth ids va 15 …
  fm4 22, dprobe drum 23, fmdrum 24; 176 of 176 synth presets, 28 drum presets (room for 30 saved drum sounds).
  te-boot identical to stock. Includes DRUMFM2's fmdrum. sha256
  a9a71eee026a285089b17449838729f85e7a95755260d9665535d8d6d3a25085.
- **Before flashing**: 30 or fewer saved drum sounds, as DRUMFM2.
- Listening checklist (guitar into the line jack, headphones; synth `amp`; hold or latch a note):
  1. `clean`: soft and hard picking, chords. Glassier than AMP1/SOUND1 (the mid scoop), no grain on decays. Turn
     green fully dark and fully bright: dark warm, bright sparkly, both about as loud as the centre ± 4 dB.
  2. `crunch`: chords and single notes; green from dark to bright should change the grind itself (bright: more
     bite in the clipping, dark: rounder), not only the treble on top.
  3. `lead`: high notes on the B and E strings above the 12th fret and wide bends: listen for a whistle or ringing
     tone that does not follow the bend (aliasing); should be gone or far below AMP1's.
  4. `fuzz` and blue at full: repeat the bends and high notes with green fully dark, centre and fully bright.
     Fully bright is the worst case (−42.7 dB at 2489 Hz in the sim): note any inharmonic fizz.
  5. Chords at full drive: intermodulation is expected; listen for harsh non-musical hash compared with AMP1.
  6. CPU: full drive, strum hard and continuously, with another FX on and the screen animating: any crackle, dropout
     or UI stall (the DSP now peaks at 62k cycles in the sim, was 74k).
  7. Turn blue and green slowly while playing: no zipper or level steps.
- Report: clean / crunch / lead / fuzz at the tone extremes (too dark, bright, quiet, loud?), bends and high notes
  (any aliasing whistle?), chords, crackle or dropouts, any crash or assert text (photo).
- Result (2026-10-15): booted; "everything sounds really good". fmdrum: a closed hat cuts off an open one (choke
  works); snares sound good (shared noise decay is fine, no separate noise envelope needed). PITCH detent size not
  reported.

## 2026-10-15 — Test image 67: DRUMFM3.OP1 (fmdrum kits in their own drum browser column)
- Static research (notes/drum-engine.md "Browser columns"): TE's drum sound browser makes one column per distinct
  `folder` of `drum_presets` + `drum_user`, sorts the columns ("thru" first, then byte order) and labels each with the
  folder name (at most 13 characters). No fixed list, no table, no hook needed. Snapshots are always saved with folder
  `user`. Commit f14445f: build_slot.py inserts a drum engine's kits with folder = the engine name (`fmdrum`, was
  `dbox`); dprobe still replaces `whathump` in dbox. New check `src/slot/test/drum_browser_check.py` (the column logic
  on the built DB): all pass, columns `dbox` (10), `drum` (14), `fmdrum` (4), `user`.
- `firmware/images/DRUMFM3.OP1` = `build.py --engines va,wavetable,grain,inprobe,sprobe,amp,harmonic,fm4,dprobe,fmdrum
  --db-trim va=1 --db-trim wavetable=1 --db-trim grain=1 --db-trim inprobe=0 --db-trim sprobe=0 --db-trim amp=1
  --db-trim harmonic=6 --db-trim fm4=6 --name DRUMFM3` from commit f14445f in a clean `git archive`: same ids and
  presets as AMP2 (synth va 15 … fm4 22, dprobe drum 23, fmdrum 24; 176 of 176 synth presets, 28 drum presets, room
  for 30 saved drum sounds). Only the DB's folder for the four kits differs from AMP2. te-boot identical to stock.
  sha256 4292a6cc606b3b18e1d81bb186a4083dbb3ca80f0504db67dff76b7598fce7cd.
- **Before flashing**: 30 or fewer saved drum sounds, as AMP2.
- Test (drum mode):
  1. Boot; slots load as on AMP2 (a slot holding an fm kit still plays it).
  2. Open the drum sound browser: columns `dbox`, `drum`, `fmdrum`, `user` in that order; `fmdrum` lists `fm 808`,
     `fm 909`, `fm metal`, `fm zaps`; dbox no longer lists them (still lists `drum probe`). Note the exact label text.
  3. With an fm kit in the slot, open the browser: the cursor should start on the `fmdrum` column, on that kit.
  4. Load each fm kit from the new column; play keys 0-23: they sound as on AMP2.
  5. Edit a key, save a snapshot (hold the slot key): it appears in `user`; reload it, edits back; power-cycle, still
     there.
  6. Load fmdrum and dbox sounds saved on earlier images (in `user`) and a stock dbox and drum kit: all load and play.
- Report: column order and labels, where the cursor starts, the snapshot's column, any crash or assert text (photo).
- **Cleanup before an image without fmdrum**: as DRUMFM1.
- Result: _pending user flash_.

## 2026-10-15 — Test image 68: WTPOS.OP1 (wavetable blue smoothed per sample; DRUMFM3's fmdrum column)
- Cause (Confirmed, sim): blue is params[0] = position, morphing through the bank's 12 frames (green = bank, `k >> 12`;
  blue never crosses a bank). wt.c read it once per block and used it for the whole block, so each encoder detent
  (1024 raw = 0.34 frame) stepped the frame blend at a block edge. Bell, one detent at position 12288: the output steps
  by up to 1,827 LSB at 55 Hz (the frame difference D peaks at 1,949; D's own sample-to-sample slope is 78). TE's LFO on
  blue modulates the same per-block knob copy (notes/knob-hook.md), so it stepped the same way.
- Fix (src/engine/wt.c): the knob is a target; two cascaded one-poles per block (each 1/4 of the way: 50 % of a detent
  after 5-6 blocks = 8 ms at 88.2 kHz, 90 % after ~15 blocks = 22 ms), the second ramped linearly per sample in Q22
  frames (one add per sample). A ramp that crosses frames splits the block where it crosses (bit-by-bit search, no
  division), so the frame pair stays fixed inside each segment; mip levels are still chosen once per block from
  pitch[0], as before. Steady blocks run the old loop. State: pos at byte 4, pos_mid at 24, a tag at 12 (voice RAM
  starts dirty); a note-on (phase1 = 0x80000000 and the DC blocker cleared) starts at the knob. SAMPLE bank: same base
  pointer path, unchanged. Presets and knob ranges unchanged.
- Why not one block: a one-block ramp removes the step but bell still splatters (above 3 kHz, re D: hard step -27 dB,
  one-block ramp -52 dB, two poles -67 dB at 220 Hz); slow sweep (a detent every 32 blocks) off-harmonic energy
  -41.6 dB before, -42.2 one-block, -52.4 two poles (static floor -69). An LFO on blue: unchanged by the measure
  (its sidebands are the modulation itself), but it now moves per sample through the same path.
- Bank (green): still switches tables at a bank boundary (8 boundaries over the knob); not changed here.
- Cycles (sim, worst per render call): 46,929 → 45,919 steady (warp + unison, 4186 Hz), 47,635 while ramping
  (372 per sample, +5 over the old worst); wt_check worst 47,757 → 46,747.
- Checks: wt_check adds bell detent checks (output step <= D's slope + D/32, splatter above 3 kHz < -58 dB, half way
  within 10 blocks, settled after 80), a fast turn (a detent every block with unison: lands on the last frame) and the
  SAMPLE bank while turning; the old wt.c fails the step and splatter checks. engine_main.c: `turn:k,step,every,first`
  and `lfo:k,period` knob schedules. wt, amp, fm4, fmdrum, grain, harm, inprobe, modal, sprobe, va, wt_import: all pass.
- Listen: wavetable, bell bank (green), turn blue slowly one detent at a time, low and high notes: no click per detent;
  turn fast: follows without a lag you notice. LFO on blue: no zipper.
- First firmware build failed at link: `undefined reference to memset` in engine_render. The `struct osc_run`
  initialiser zeroed `lk` with memset; the simulator links libc, so wt_check passed. Fixed in 7b91d8a (fields set one
  by one; frame_pair fills lk before each run); wt_check passes (max 46,852 cycles).
- `firmware/images/WTPOS.OP1` = DRUMFM3's command with `--name WTPOS`, from commit 7b91d8a in a clean `git archive`:
  synth va 15 … fm4 22, dprobe drum 23, fmdrum 24; drum folders dbox 10, drum 14, fmdrum 4 (room for 30 saved drum
  sounds); te-boot identical to stock. sha256 a8d8e6c8f4bdfa6aed0be16ed90cb5dee381e27681276a95550a66f213ae2312.
- Also check DRUMFM3's list (test 67): the fmdrum column, kits load and play, snapshots save to user.
- Result: _pending user flash_.
