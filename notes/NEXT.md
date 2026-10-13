# Proof of life: result and how to continue

## Result (2026-10-06) — **viable**
Custom code runs as the `iter` engine's render inside stock OP-1 firmware 246.

| test | image | result |
|---|---|---|
| pipeline | REPACK.OP1 (stock, recompressed) | boots normally; "te_boot skipped" |
| enable iter | ITER2.OP1 (iter row + presets) | iter listed and playable. The row alone (ITER.OP1) is not enough on 246: the browser lists only engines with presets. |
| hook: silence | SILENCE.OP1 (+16 B at 0x019B99D4) | iter is silent; everything else normal |
| hook: sine | SINE.OP1 (+56 B at 0x019B99D4) | exactly 440 Hz on every key → fs = 44.1 kHz |

How it works:
- One plaintext LDR block, inserted before the FINAL block, overwrites iter's render function body.
- The FIRST block's next-DXE argument grows by the same number of bytes.
- `te-boot.ldr` is never touched.

Build everything from the stock image with:
```
tools/op1pack.py, tools/enable_iter_db.py --presets …, src/poc/build.sh, tools/ldr_patch.py
```
(Exact commands are in LOG.md.)

## What we can rely on (Confirmed)
- **Call:** `render(this R0, int16* out R1, const int16* params R2)`, 128 mono Q15 samples per call, 44.1 kHz, 6 voices.
- **Per-voice phase increments:** `int32[128]` at `0xFF801950`. They track the keys.
- **Voice object:** 36 B in L1 scratchpad (`0xFFB00000 + i*0x24`). We may reuse iter's fields: +4 … +0x20 (+0 is the vptr).
- **ABI:**
  - args R0–R2;
  - save R4–R7 and P3–P5;
  - LC0/LT0/LB0 may be clobbered (TE's render does);
  - return via `RTS` (fine, since we make no calls) or TE's `P0=[FP+4]; JUMP (P0)`.

## Next steps, in order (one change per image, as before)
1. ~~**Pitched sine.**~~ Done 2026-10-06: correct pitch, intervals and chords.
   **Pitched sine.** Use the increments at `0xFF801950` instead of the constant. This proves the pitch input; expect correct pitch per key.
2. ~~**CPU budget.**~~ Done 2026-10-06 (CPULOAD): ~0.96 M cycles per block for all render calls; ~1,250 cycles/sample/voice at 6 voices. Target ≤ 50%. Overload degrades audio gracefully, without crashing. Open: 2 render calls per block with one held key.
   **CPU budget.** Read the `CYCLES` register at render entry and exit, and keep a max in a spare object field. Expose it audibly, e.g. a tone whose pitch ∝ cycles. That gives the cycle budget per voice per 2.9 ms block.
   - Upper bound: 400 MHz × 2.9 ms ≈ 1.16 M cycles per block for the *whole* system.
3. ~~**Parameters.**~~ Done 2026-10-06 (PARAMS): R2 = the T1 engine knobs.
   **Parameters.** Find how knob values reach `params` (R2). Stock iter appears to ignore the encoders on device.
   - Trace the caller's `params` pointer in the plaintext parts of the L1 voice loop. Compare with fm's render, which does respond to the knobs.
   - Fallback: read the global knob state that `CtlKnob.cpp` / `synth_controller.cpp` maintain.
4. ~~**C instead of assembly.**~~ Done 2026-10-06 (CENGINE.OP1, `src/engine/`): confirmed on device. Write engines in `src/engine/engine.c` against `engine.h`.
   GCC 4.3 uses hardware loop 1 (LC1). TE's code only shows LC0 being clobbered across this call.
   - Add an asm trampoline at `0x019B99D4` that saves and restores LC1/LT1/LB1 (and R4–R7/P3–P5 per ABI), sets L0–L3 = 0, and calls the C function.
   - Or verify that the caller doesn't use loop 1 around the call.
5. ~~**More space.**~~ Done 2026-10-06 (CANARYLOG, RELOC.OP1): canary regions 0–2 (3 × 1 MB) are free; the engine runs from `0x01E10000` via vtable slot `0x019103E0`. Build with `src/engine/build.sh 0x01E10000 <out> 0x100000`, patch with `ldr_patch.py --block 0x01E10000=… --pointer 0x019103E0=0x019B99D4:0x01E10000`.
   **More space.** The in-place budget is 438 B. For a real engine:
   - Point the vtable slot `0x019103E0` at code placed elsewhere.
   - Free memory is not yet proven. Candidates are in memory-map.md: `0x01DFFF34…`, `0x00000000–0x00FFFFFF`, `0x027FFB04…`.
   - Proposed probe image: a FILL block writing a canary pattern over a candidate range, plus a render that checks the canary each call and emits a different tone if it's ever damaged. Play for a while, using tape, sampler and every mode.
   - Only then place code there. Extend `tools/ldr_patch.py` with an explicit, logged allowlist for that region.
6. **Prototype DSP on the Daisy Seed.** Write engines as portable fixed-point C with the same contract (128-sample block, Q15 out, per-sample phase increments, 4 int16 params) so the same file builds for the Daisy and for Blackfin. Unit-test in `bfin-elf-run` (`src/poc/test/`) before flashing.

## Known limits and risks
- Everything is tied to firmware 246. Addresses change with any TE update; 245's app differs.
- The voice loop and audio ISR are in L1, now decrypted (`ldr.decrypt`, Ghidra program `OP1_vdk_246_dec.elf`) but not yet analysed.
- Free memory is proven only for what CANARYLOG's exercise covered; region 3 (`0x03800000`) is in use.
- Never use `FRC`, `RBT` or `KEY` in an image file name. Never change `te-boot.ldr`. The tools enforce the second.
