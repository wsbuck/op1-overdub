# How encoder values reach a producer's `params` (firmware 246)

Static analysis only (Ghidra + objdump of `OP1_vdk_246.elf`). **Confirmed** = read in the binary. **Hypothesis** = inferred.

## Summary
- Encoders feed one engine-generic path: synth_controller → Ranges → a per-slot knob array in L1 data. No UI page and no per-engine code sits on this path. iter has valid range entries, so turning an encoder **should** change iter's knob values.
- What is still unproven: that render's `R2` points at those knobs. Its source, `[FP-0x24]` in the encrypted voice loop, is never visibly assigned.
- Next step: an on-device probe render (see "Patch proposal").

## Data
| what | address | confidence |
|---|---|---|
| current sound slot (0..7) | `DAT_ff8016d4` | Confirmed |
| engine id per slot (`int[8]`) | `0xFF8016E4 + slot*4` (written by patch load `FUN_01a010f8`, patch.cpp:131) | Confirmed |
| **synth knobs per slot** (`int16[8]`, the patch JSON `"knobs"`) | **`0xFF8013C4 + slot*0x10`** | Confirmed |
| envelope knobs per slot (`int16[8]`, mode 1 / T2 page) | `0x01B60EF0 + slot*0x10` | Confirmed |
| FX knobs per slot (mode 2; earlier mislabelled LFO, see fx-slot.md) | `0xFF801444 + slot*0x10` | Confirmed (encoder `FUN_01a50690`) |
| patch objects | `0x01DDD7E8 + slot*0x38` (vptr-like object at +0x34) | Confirmed |
| producer slot (slot the producers were built for) | `DAT_01aaac0c` | Confirmed that it is compared with `ff8016d4` in `FUN_01967854`. Role: Hypothesis. |
| knob range table `{min,max,scale,type}` int16, 8 rows × 15 engines | `0x01DE5890 + engine*0x40 + knob*8` (runtime-filled by `FUN_01a1e914`; zero in the ELF) | Confirmed |

## Q1 — Where `params` comes from

### Write path when an encoder turns — Confirmed
1. The synth_controller vtable is `0x01911994` (set in `FUN_01a508d4`). Slot `+0x2C` is `FUN_01a505bc(this, knob R1, int16* delta R2)`.
2. It dispatches on the page mode `DAT_01b4ee88` (T1..T4 → 0..3) through the table at `0x018FB268` = `{0x1a50610, 0x1a50658, 0x1a50690, 0x1a5071e}`.
3. **Mode 0 (engine page), `0x1a50610`:**
   - calls `FUN_01a1dfec(knob, &delta, engine_id = [0xFF8016E4+slot*4], knobs = 0xFF8013C4+slot*16)` (Ranges.cpp:810);
   - then calls `FUN_01a00d14(patch, knobs)`, which writes the JSON `"knobs"` back into the patch and calls the patch's +0x34 hook.
4. `FUN_01a1dfec` switches on `type = table[engine][knob].+6`:
   - Type 0 (most engines, iter included): knobs 4..7 become `delta >>= 4; knob -= 4`, so they are the fine adjust of knobs 0..3.
   - Then `FUN_01a1de68`: `k[i] = clamp(k[i] + delta*scale (Q15), min, max)`.
   - Types 1 (drwave), 2 (sampler) and 3 (dsynth) are special cases.
5. Nothing on this path is specific to iter, apart from the table row lookup.

### Read path into producers
- **Note-on — Confirmed.** `FUN_01967854(v)` (Core.cpp:1216/1217) calls `s_producers[v]->vfn[+0x1C](obj, 0xFF8013C4 + DAT_01aaac0c*16)`.
  - `s_producers` is `0x01AAAD18`.
  - This is the synth-knob array directly. iter's +0x1C uses `params[0]²` for gain.
- **Render — Hypothesis.** The voice loop `FUN_ffa009a0` (L1, encrypted, LINK 0x44) passes `R2 = [FP-0x24]` at `0xffa00d1a`.
  - The visible plaintext copies 8 int16 per voice from `R4 + [[FP+0xC]]*16 + 2i` into a local buffer (loop `0xffa00ba8..bbc`, LC0 = 8). `R4.H = 0xFF80`; `R4.L` is encrypted.
  - A per-voice LFO object (`0x01AAAD6C[v]`, vfn +0x14 call at `0xffa00b04`) runs before it.
  - Best reading: `params` = this per-voice copy of the slot's synth knobs, after LFO modulation.
  - **Caveat:** the same `[FP-0x24]` is loaded as `R2` at `0xffa00b6e`, just before `CALL FUN_01912128`. That function is an ADSR (`p[0..3]` → attack/decay/sustain/release via tables `0x0106B3CC` and `0x0106B5CC`).
    - Either the encrypted bytes `0xffa00b70–b77` change `R2`, or the slot is reassigned in encrypted code between the two calls.
    - If neither happens, render would receive the **envelope** knobs. The on-device probe below settles it.
- **Same contract for all engines — Confirmed.** fm's producer vtable is `0x0190FF84` (ctor `FUN_019a21d8`, from `FUN_019a3d38`).
  - Its render `0x019A23D4` starts `P1 = R2; R6 = W[P1+2]; R0 = W[P1]; R5 = W[P1+6]; R6 = W[P1+4]`, the same access pattern as iter.
  - Its slots: +0x0C `0x19a23a8`, +0x14 `0x19a23d4` render, +0x1C `0x19a2248`, +0x24 `0x19a2184`, +0x2C `0x1a1b5ec`.

## Q2 — Why iter seems not to respond

### Refuted (Confirmed)
- **"iter's UI page lacks an encoder handler."** UI pages have only 4 virtual slots (dtor, `0x1a3b3f6` id-compare, draw, init). This holds for fm (`0x0190FFDC`: `19a4cf0 1a3b3f6 19a4d34 19a4a3c`) and iter (`0x019103FC`: `19ba384 1a3b3f6 19b9ea4 19b9de4`). Encoders never go through them.
- **"Missing case 14":**
  - Range rows for iter exist (`0x01DE5C10`: 4 × `{0, 0x7FFF, 0.32, type 0}`, plus fine rows).
  - `ScreenId.cpp` `FUN_01a3bf44` maps 14 → screen 0x14.
  - The factory has case 14.

### Remaining hypotheses (ranked)
1. **The effect is coarse or subtle** (Hypothesis).
   - Stock render quantizes p0 and p1 to 16 steps (`>>11`), indexing `int32` table `0x0106A8F4`. p3 goes to 64 steps (`>>9`), table `0x0106A5EC`, crossfading `tbl[p3>>9]` with `tbl[64-p3>>9]`. p2 is used as `p2²`.
   - iter's sensitivity is 0.32. fm's is the full range. digital and voltage are 0.5.
   - So p0 and p1 change only every 2048 units. Defaults are `[1516, 16704, 0, 15168]`: p0 index 0, p2 = 0.
   - Both tables are runtime-filled (zero in the ELF), so their shape is unknown. If they are flat near the defaults, small turns do nothing.
2. **Render gets the envelope knobs, not the synth knobs** (see the caveat above). That would also explain it, but it would break every engine equally, so it is less likely.
3. The iter page draw (`FUN_019b9ea4`) shows voices, not knob values, so there is no visual feedback. This is perception only.

## Q3 — Patch proposal (plaintext SDRAM, inside iter's 438 B render)
**No routing patch is needed** if hypothesis 1 holds. Do this first:

1. **Probe image (`PARAMS.S`)**, built from `psine.S`:
   - Keep the key-tracked sine.
   - Amplitude = `params[0]` (Q15 multiply of the sample by `W[R2]`).
   - Octave shift = `params[1] >> 13` (increment `<<= n`).
2. Test on device:
   - T1-page encoders 1–2 change loudness and octave → R2 = synth knobs. Hypothesis 1 confirmed: write our own engine with continuous params.
   - Only T2-page (envelope) encoders do it → hypothesis 2.
3. **Fallback if R2 is not the synth knobs:** read the knobs directly in our render and ignore R2.
   ```
   P0.L=0xAC0C; P0.H=0x01AA; R0=[P0]; R0<<=4;   // producer slot
   P1.L=0x13C4; P1.H=0xFF80; R1=P1; R0=R0+R1; P1=R0   // P1 -> int16 knobs[8]
   ```
   - Risks:
     - We lose per-voice LFO modulation.
     - Reads race with the UI writer (16-bit stores are atomic; fine).
     - `DAT_01aaac0c` vs `DAT_ff8016d4` semantics are a Hypothesis; `0xFF8016D4` is the safe alternative.
     - No encrypted or shared code is touched; the change stays inside the iter render block at `0x019B99D4`.
4. Do **not** retarget UI vtables. They are not on the encoder path.
5. Optional: raise iter's sensitivity. This needs a patch in the code of `FUN_01a1e914` (the table is runtime-built), e.g. the `FUN_01a22eea(&0x01DE5C14, 0x3EA3D70A)` immediates → `0x3F7FFE00`. Low value; skip.

## Q4 — Code that depends on the engine id (0..14)
| site | what | bound / form |
|---|---|---|
| `FUN_01a1b7d4` (producer_factory.cpp:401) | producer factory, table `0x018F5A04` | `id < 15` jump table |
| `FUN_01a3bf44` (ScreenId.cpp:62) | engine → screen id, table `0x018F803C` (0–7 identity; 8–10 assert; 11→0x32, 12→0x12, 13→0x13, 14→0x14) | `id < 15` jump table |
| `FUN_019b9890` / `FUN_019b97c4` / `FUN_019b9810` (instr_types.cpp:49) | name table at `0x01DD4D98`, name → id loop | `R4 = 15` loop bound |
| `FUN_01a1e914` (Ranges init) + `FUN_01a1dfec` (Ranges.cpp:810) + `FUN_01a28210` (ReadInterface.cpp) + `FUN_01a1dd74`, `FUN_01a1e57c`, `FUN_01a28010` | range table `0x01DE5890 + id*0x40` | no bound check |
| `FUN_01967b28` and its L1 twin near `0xffa01bfe` | builds producers via the factory, from `ff8016e4[slot]` | generic |
| `FUN_01a50aa8`, other `== 6` checks | sampler special cases | `== 6` |
| `FUN_01a1dfec` | per-engine knob type (0/1/2/3) from the table | data-driven |
| ~6 functions at `0x19c9d44..0x19cfcc4` | read `ff8016e4[slot]` and call `FUN_01a1dfec`; probably LFO/modulation targets (Hypothesis) | data-driven |

- The range table for engine 15 would overlap `0x01DE5C50` (the next table, used by the mode-2/FX handler).
- Other `< 15` jump tables (`0x196a830`, `0x19d98de`, `0x1a57a5c`, `0x1a7e5be`) switch on offset values (char − 0x2B, key − 6, …), not engine ids.
- About 30 more `R = 15; CC = Rx < R (IU)` sites look like loop bounds. They are unclassified.
- **Cost of ids ≥ 15:**
  - extend 2 jump tables (factory, ScreenId) and the name table;
  - relocate or extend the range table (`0x40` B per engine, collides at 15);
  - check `DAT_01dd4d98` capacity.
- Reusing id 14 (iter) avoids all of this.
