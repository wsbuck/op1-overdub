# Engine slot 15: a 16th synth engine (firmware 246)

**Status: works on device (SLOT2.OP1, 2026-10-07).**

Goal: add a synth engine with its own name, database row and presets, keeping all 15 TE engines.
Built by `tools/build_slot.py` (code: `src/slot/slot.S`) and `tools/add_engine_db.py`.

## Why the DB row alone is not enough — Confirmed
- The DB's `synth_types.type` is only a name. The firmware maps it to an engine id with a compiled-in
  name table and turns the id into objects, screens and knob ranges with `id < 15` tables.
- Ids are shared with the drum side: 8 `drum`, 9 `dbox`, 10 `ls` (the synth factory asserts on 8 and 9;
  ScreenId on 8–10). There is no free id below 15, so the new engine is **id 15**.

## Engine ids (name blob at `0x018DF7BC`)
| id | name | id | name | id | name |
|---|---|---|---|---|---|
| 0 | fm | 5 | pulse | 10 | ls |
| 1 | digital | 6 | sampler | 11 | dna |
| 2 | drwave | 7 | phase | 12 | dsynth |
| 3 | string | 8 | drum | 13 | voltage |
| 4 | cluster | 9 | dbox | 14 | iter |

## What id 15 needs — Confirmed (code read), patched by build_slot.py
| what | stock | patch |
|---|---|---|
| name → id table, `{char*, int}` × 15, built at boot by `FUN_019b9890` | `0x01DD4D98` (entry 15 would hit iter data at `0x01DD4E10`) | static 16-entry copy in our block; `FUN_019b97c4` (validate) and `FUN_019b9810` (lookup, instr_types.cpp:49): base `.L/.H` and loop bound 15 → 16 |
| producer factory `FUN_01a1b7d4` (producer_factory.cpp) | `id < 15`, jump table `0x018F5A04` | bound 16, 16-entry table; case 15 = our stub |
| ScreenId `FUN_01a3bf44` (ScreenId.cpp:62) | `id < 15`, jump table `0x018F803C` | bound 16; case 15 = iter's case (screen 0x14) |
| knob range table, 0x40 B per engine (Ranges.cpp) | `0x01DE5890` (entry 15 would overlap the table at `0x01DE5C50`) | 16-row copy in our block; the 6 base loads repointed. Rows 0–14 are written field by field by Ranges init at boot; row 15 is static: 8 × `{0, 0x7FFF, 0x7FFF, 0}` (full range and sensitivity, type 0) |
- No data words in the image point into any of these tables (scan of all loaded blocks, decrypted), so code references are the only users (`tools/xrefs.py`).
- Range row format: `{int16 min, max, scale (Q15), type}`; type 0 = knobs 4–7 are fine adjust of 0–3 (`FUN_01a1dfec`).

## Factory case 15
- Same as iter's case 14: `FUN_019b9bf0(i)` for i = 0..5 (constructs iter voice i in L1 scratchpad at
  `0xFFB00000 + i*0x24`, vptr = `0x019103CC`), then `FUN_019b9c64` (clears iter's 256 B buffer).
- Then each voice's vptr is set to our copy of iter's vtable (12 words) with render (+0x14) → our engine.
  iter itself is left stock, so iter and the new engine can be compared directly.
- Voice objects are shared with iter. Only one synth slot's producers exist at a time
  (`DAT_01aaac0c`), so this is safe as long as nothing keeps iter and id 15 alive together (Hypothesis).

## Other engine-id consumers — audit 2026-10-07
Every reader of the per-slot id array `0xFF8016E4` and every caller of the name lookup, factory and
ScreenId was followed (decompiled). Beyond the four patched structures:
- **Comparisons only** (`== 6` sampler, `== 8/9` drum, mailbox `==`): global_controller, synth_controller, patch.cpp:1018 validation, Core.cpp mailbox (L1 `FUN_ffa009a0`). Id 15 takes the ordinary path. Confirmed.
- **Per-engine byte flags** (1 for ids 6, 8, 9, 12; read-only, plaintext): LFO modules at `0x018E142C`, `0x018E151C`, `0x018E1710`, `0x018E182C`, `0x018E19FC`, and `FUN_01a27eb4`'s `0x018F5ECC`. Each is 16 bytes; entry 15 already exists and is 0 (next table starts at +0x10). Confirmed. Setting it to 1 gives id 15 8 LFO targets (selector detent d -> knob d, DSP `>>> 11`) instead of 4 (d / 2, `>>> 12`); the FX tables work the same way (fx-slot.md "LFO target flag", Verified 2026-10-18).
- **Passed on** to the factory, ScreenId or the range functions (all patched): Core.cpp, patch.cpp:131 (`FUN_01a010f8` → `FUN_01a85db4` → `FUN_01967960` → factory), LfoRnd, synth_controller.
- Unresolved:
  - a *drum* patch naming `va` would put 15 into the drum id array `0xFF801704`; it fails validation first (patch.cpp:1018, error 13), which asserts in drum context. Drum engines: notes/drum-engine.md.
  - how the engine browser builds its list (no static id list found; probably from the DB).
  - ~52 other `R = 15; CC = R < …` sites: those sampled are std::string capacity checks or a 15-entry interrupt table; none is near an engine-array reference. Not all checked.

## Build
```
src/engine/build.sh 0x01E10000 firmware/build/va 0x100000
tools/build_slot.py firmware/work/246/OP1_vdk.ldr firmware/build/va/engine.bin firmware/build/OP1_vdk_slot.ldr
tools/add_engine_db.py <iter-enabled db> firmware/build/op1_factory_va.db --type va --default "[...]" --preset NAME=K0,K1,K2,K3 ...
tools/op1pack.py build firmware/stock/op1_246.op1 OUT.OP1 --replace content/op1_factory.db=… --replace OP1_vdk.ldr=…
```
- Every LDR block must be 4-byte aligned and sized (32-bit boot DMA). SLOT1 broke this and did not boot.
- More engines: ids 16+ need the same tables grown again (names, factory, ScreenId, ranges, and the six 16-byte flag tables, which end at 16 entries).
