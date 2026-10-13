# Synth engine interface (firmware 246)

Sources: Ghidra decompile and objdump of `OP1_vdk_246.elf`, plus assert strings with source paths.
**Confirmed** = read in the binary. **Hypothesis** = inferred.

## Architecture
- Engines are C++ "producer" objects (`..\src\dsp\producer_factory.cpp`, `Core.cpp`). **Confirmed**.
- Engine ids come from the name table built at `0x01DD4D98` by `FUN_019b9890`. **Confirmed**:

  `0 fm, 1 digital, 2 drwave, 3 string, 4 cluster, 5 pulse, 6 sampler, 7 phase, 8 drum, 9 dbox, 10 ls, 11 dna, 12 dsynth, 13 voltage, 14 iter`

  - The DB (`synth_types.type` name) is mapped to an id by name lookup (`FUN_019b97c4`).
- **Factory `FUN_01a1b7d4(engine_id, Producer** out, …)`** — **Confirmed**:
  - `switch(engine_id)` fills `out[0..5]`, one object per synth voice (`kSynthVoiceCount` = 6).
  - **case 14 (iter):** `out[i] = FUN_019b9bf0(i)` for i = 0..5, then `FUN_019b9c64()` clears iter's shared 128-short buffer at `0x01DD4E18`.
- `Core.cpp` keeps `s_producers[6]`. The per-voice loop that calls producers is in the **encrypted** L1 instruction block (`Core.cpp:713` → `0xFFA00DDC`). **Confirmed** (assert xref).

## iter voice object — **Confirmed** (`FUN_019b9bf0`, `Iter.cpp:82`)
- Placement-constructed in **L1 scratchpad** at `0xFFB00000 + i*0x24` (36 bytes each, 6 voices).
- Layout:

  | offset | content |
  |---|---|
  | +0x00 | vptr = `0x019103CC` |
  | +0x04 | phase A (u32) |
  | +0x08 | s16 |
  | +0x0C | phase B (u32) |
  | +0x10 | s16 |
  | +0x14 | filter state (s32) |
  | +0x18 | filter state (s32) |
  | +0x1C | gain (s16) |
  | +0x1E | gain (s16) |
  | +0x20 | u8 released flag |

## iter vtable (`0x019103CC`, plaintext SDRAM) — **Confirmed** (data)
- Entries are 8 bytes. The function pointer sits at `vptr + 0x0C + 8n`; the other word is 0.

| vptr+ | fn | role | confidence |
|---|---|---|---|
| 0x0C | `0x019B9BC4` | deleting destructor (restores base vptr `0x019103BC`) | Confirmed (asm) |
| **0x14** | **`0x019B99D4`** | **render** | **Confirmed on device** (SILENCE/SINE tests, 2026-10-06) |
| 0x1C | `0x019B9B8A` | note-on / reset: phases ← 0x80000000, clears filter, gain from params[0]² | Confirmed (asm). Role is a Hypothesis. |
| 0x24 | `0x019B9980` | returns 0 (likely "still sounding?" / tail query) | Confirmed (asm). Role is a Hypothesis. |
| 0x2C | `0x019B996C` | sets `+0x20 = 1` (likely note-off/release) | Confirmed (asm). Role is a Hypothesis. |

## render: `void render(Iter* this /*R0*/, int16_t* out /*R1*/, const int16_t* params /*R2*/)`
| field | value | confidence |
|---|---|---|
| this | voice object in scratchpad, R0 | Confirmed |
| out | R1 → I0; `W[I0++] = sample`, **mono int16**, contiguous | Confirmed |
| sample count | **128 per call**, fixed (`LSETUP … LC0 = 0x80`) | Confirmed for iter |
| params | R2: `int16[≥4]` = the engine-page (T1) knobs, 0..0x7FFF; params[0] = blue, params[1] = green (PARAMS test). Stock iter's knob sensitivity is ~0.32× (range table `0x01DE5C10`), so turns are slow. | **Confirmed on device** |
| pitch input | global `int32[128]` at **`0xFF801950`** (I1), added per sample to phase A → per-sample phase increment, 2³² = one cycle | **Confirmed on device** (PSINE test: correct pitch, intervals and chords) |
| fixed point | Q15 samples; saturating 16×16 MACs (`(S)`, `(FU)` modes) | Confirmed |
| sample rate | **44.1 kHz** | **Confirmed**: the SINE test (increment 440/44100·2³²) measured 440 Hz on device |
| return | none | Confirmed |
| ABI | args R0–R2; saves R4–R7 and P3–P5 (`[--SP] = (R7:4, P5:3)`); `LINK`/`UNLINK`; returns with `P0 = [FP+4]; JUMP (P0)` (equivalent to RTS); uses I0–I2 and LC0 without restoring them | Confirmed |
| callers | only via the vtable (exactly 1 data reference to `0x019B99D4`, 0 direct calls) | Confirmed |
| context | called from the encrypted L1 voice loop; probably audio-thread or ISR context | Hypothesis |

- Render occupies `0x019B99D4–0x019B9B89` (**438 bytes**) in a plaintext SDRAM LDR block.
- Tables it reads:
  - `0x0101E5E8`, indexed `(phase >> 19) + 0x1000` → probably an 8192-entry sine table;
  - `0x0106A8F4` and `0x0106A5EC`: parameter curves.

## Proposed hook (Phase 3)
**Overwrite render's body in place.** Add one LDR block targeting `0x019B99D4`, at most 438 bytes, inserted just before the FINAL block. The ROM loads it after the original block, so it wins.
- No vtable change.
- Needs no "free memory": the code is only reachable when iter is the active engine.
- The address range is inside the app's existing SDRAM image, far from te-boot's live RAM and from NAND blocks 0–3.
- iter must be selectable, via a `synth_types` row (as op1repacker does) or by loading an iter patch.
- For larger engines later: either patch the vtable slot `0x019103E0` to code placed elsewhere, or reuse other dead iter code.
