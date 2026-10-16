# FX slot: adding our own FX type (firmware 246)

**Status: FX1 built (`tools/build_slot.py --fx NAME`, `src/slot/fx.c`), not yet tested on device.** Static analysis 2026-10-08; key addresses re-checked in the disassembly before building.

Goal: a 9th FX (firmware id 8) with its own name, DB row and screen, keeping all 8 TE FX. First target: a chorus/ensemble.
Confidence: **Confirmed** = read in code, **Verified** = re-checked in disassembly at the call site, **Hypothesis** = inferred.

## Answer first
- **Synth and drum FX are mono.** `process(this, int16 *buf, const int16 *params)`, in place, 64 samples, Q15, 44.1 kHz. **Verified.**
- The mono FX output goes to tape (tape tracks are mono) and to the mixer, which pans it into stereo L/R buses. **The FX slot is not where audio becomes stereo; the mixer is.** Confirmed.
- **Only the master FX (mixer) is stereo:** `process(this, int16 *L, int16 *R, [SP+0xC] const int16 *params)`, 64 + 64 samples. **Verified.** Every FX class has a mono and a stereo variant, so one new id gets both.
- So a stereo ensemble only works as the **master FX**. As a synth FX it can only be mono (chorus without spread).

## FX ids (name blob `0x018DD7D4`, table `0x018DD810`) — Confirmed
| id | name | class (assert file) | svg | screen id | stock DB id |
|---|---|---|---|---|---|
| 0 | delay | Dssd | delay.svg | 0x08 | 5 |
| 1 | grid | Grid | grid.svg | 0x09 | 3 |
| 2 | punch | Mllp | mllp.svg | 0x0A | 4 |
| 3 | phone | Ptch | ptch.svg | 0x0E | 7 |
| 4 | spring | Rymd | rymd.svg | 0x0F | 8 |
| 5 | filter | Mmmf | mmmf.svg | 0x0B | (hidden; we add it) |
| 6 | nitro | Ftwo | ftwo.svg | 0x0C | 10 |
| 7 | cwo | BodeAudioEngine | bode.svg | 0x0D | 9 |
- No `mother`/`terminal` strings exist in 246 (those are later or other products).
- The name table is **static** data: 8 × `{char *name, int id}`. Strings `fx_types.cpp:34…` follow directly at `0x018DD850`, so it cannot grow in place.
- Three loops, each bounded at 8 (fx_types.cpp):
  - `FUN_019aad38`: id → name. Asserts `:34 unknown fx`.
  - `FUN_019aadf8`: is-valid-name. Used by the patch.cpp:1018 validation.
  - `FUN_019aae44`: name → id. Asserts `:42 unknown fx name`.
- DB path, as for engines (Confirmed):
  - `FUN_019ff3b0` runs `SELECT type FROM fx_types WHERE id=%i` and `SELECT default_params …`. They become the patch JSON `fx_type` (a name) and `fx_params`.
  - Patch load (`FUN_01a00e30` synth, `FUN_01a00174` drum, MasterController `FUN_019d79bc`) turns the name into a firmware id with `FUN_019aae44`.
  - It stores the id in `0xFF801734[slot]` (synth), `0xFF801754[slot]` (drum) or `0xFF801090` (master).
  - DB ids are only browse ids (filter = DB 2, firmware 5).

## Factory and objects — Confirmed
| | mono (synth/drum) | stereo (master) |
|---|---|---|
| factory | `FUN_01a1b6d0(id R0, pos R1)`; asserts pos ∈ {0,1} (`producer_factory.cpp:279`) | `FUN_01a1b5f0(id R0)` |
| bound / table | `R1 = 8` @`0x01A1B6DE`; table `0x018F59AC` (8 words, a string follows) | `R1 = 8` @`0x01A1B5F0`; table `0x018F5910` (a string follows) |
| placement | **L1 scratchpad `0xFFB00600`** (fixed; ctor gets `R0 = R7`) | **`0xFFB00790`** |
| room (Hypothesis: next user is the LFO objects at `0xFFB00A00`) | 0x190 B | 0x270 B |
| global pointer | `s_fx[0]` synth `0x01AAAD60`, `s_fx[1]` drum `0x01AAAD64` | `0x01AAAD68` |
| enable byte | synth `0x01AAAC20[slot]`, drum `0x01AAAC30[slot]` (patch `fx_active`) | `0x01AAAC40` |
- Synth and drum FX are placement-constructed at the **same** address. Only the current play mode's object is live.
  - The audio thread rebuilds it on a slot or mode change (L1 `FUN_ffa017ec`), as does `FUN_01967b28`.
  - The UI rebuilds it on a patch change (`FUN_01a85ecc` → `FUN_019679cc(pos, id)`; master `FUN_01967a24`). So constructors and init run in **both** threads.
- Typical classes:
  - Stereo variants are two mono objects (filter stereo = vptr + mono@+4 + mono@+0x28; punch and phone the same).
  - Delay lines live in **SDRAM per position**, e.g. phone's init uses `0x027D1FE0 + pos*0x89D0`.
- Sizes: filter mono ≈ 0x24 B, filter stereo ≈ 0x4C B, delay mono 0x14 B, phone stereo 0x11C B.

### Vtable — Confirmed (8-byte entries `{int16 adj; fn}`, like engines)
| slot | role | filter mono `0x01910C94` | filter stereo `0x01910CB4` | delay mono `0x0190FB6C` |
|---|---|---|---|---|
| +0x0C | dtor (resets vptr to base `0x01910C84`) | `0x019E781E` | `0x019E78EC` | `0x0197D852` |
| +0x14 (adj +0x10) | **process** | `0x019E7934` | `0x019E7EE4` | `0x0197D8C4` |
| +0x1C (adj +0x18) | init(this, pos): sets `m_fxpos` | `0x019E780A` (`[+8]=0, [+0x10]=pos`) | `0x019E78D0` (sub-objects pos 2 = L, 3 = R) | `0x0197D7E8` |
- Positions (`m_fxpos`, −1 = NoTeFxPosition, asserted in process): 0 synth, 1 drum, 2 master L, 3 master R.
- Process writes peak in/out meters to `0xFF801774 + pos*8` (filter). Who reads them: Hypothesis (UI meters).
- Other vtables:
  - stereo: delay `0x0190FB8C`, grid `0x01910224`, punch `0x01910C3C`, phone `0x01911314`, spring `0x019113DC`, nitro `0x01910034`, cwo `ctor 0x0194425c`;
  - mono: grid `0x01910204`, punch `0x01910C1C`, phone `0x019112F4`, spring `0x019113BC`, nitro `0x01910014`, cwo `0x0190F574`.

## Audio graph (L1 `FUN_ffa017ec`, one call per codec block) — Confirmed unless marked
1. **Block = 64 stereo frames** at 44.1 kHz (1.45 ms). Codec in/out is int32 L/R; output loop `0xFFA01478`: 64 × two words. **Verified.**
2. Voice loop `FUN_ffa009a0`: each voice renders **128** samples into `0xFF80113C` (params `0xFF801724` = copy of the synth knobs). They are summed with the envelope into `0xFF800BB4` (128 int16). **Verified** (`0xFFA00D1A..D70`).
3. `FUN_ffa005d4`: a **2:1 half-band decimator** (32-tap, circular I2/I3), 128 → 64 into `0xFF801E50`. `FUN_019674f8` then applies a DC/one-pole stage `0xFF801E50` → `0xFF800BB4[0..63]`. **Verified.**
   → **Engines run at 2 × fs = 88.2 kHz.** This matches the LOGTEST capture's unexplained "2× time compression" of 128-sample symbols (LOG.md). The "440 Hz" SINE result was probably octave-blind. engine-interface.md's "44.1 kHz" needs correcting.
4. Synth LFO (`0x01AAAC44`), then **synth FX**: `s_fx[0]->process(this+adj, 0xFF800BB4, 0xFF801724)`, where `0xFF801724` = copy of `0xFF801444 + slot*16` (8 int16). Call `0xFFA01FDC`. **Verified** (R1 = `[SP+0x38]` = `0xFF800BB4`, R2 = `[FP+0xC]` = `0xFF801724`).
   Drum mode: drum voices → `s_fx[1]->process(…, 0xFF800BB4, copy of 0xFF8015C4 + dslot*16)`.
5. Mic/line input `0xFF800CB4` is mixed into `0xFF800BB4` (mode dependent). Details, and when it is written (before the voice loop): notes/input.md.
6. `FUN_ffa00e50` (mixer):
   - pans `0xFF800BB4` (and the tape tracks) into the stereo buses **L `0xFF800D34`, R `0xFF800DB4`** (64 int16 each);
   - then, if `0x01AAAC40`, runs the **master FX**: `s_fx[2]->process(this+adj, 0xFF800D34, 0xFF800DB4, [SP+0xC] = 0xFF8016C4)` (call `0xFFA017E6`, **Verified**);
   - then EQ/compressor and the int32 output.
7. `FUN_ffa06334(0xFF800BB4)`: writes the (FX'd, mono) buffer to tape.
- All of it is one callback, so FX shares the CPU budget with the voices.

## Parameters — Confirmed
| what | address |
|---|---|
| synth FX knobs (T3 page), int16[8] per slot | `0xFF801444 + slot*16` (**params-path.md calls this LFO: wrong**; LFO is `0xFF8014C4`) |
| drum FX knobs | `0xFF8015C4 + dslot*16` |
| master FX knobs | `0xFF8016C4` (int16[8]) |
| FX id per slot | synth `0xFF801734`, drum `0xFF801754`, master `0xFF801090` |
| **FX range table** | `0x01DE5C50 + id*0x30 + knob*6`: `{int16 min, max, scale(Q15)}` × 8; runtime-filled by Ranges init `FUN_01a1f712`; zero in the ELF |
- Encoder (synth, mode 2) `FUN_01a50690`:
  - knobs 4–7 are always the fine adjust of 0–3 (`delta >>= 4`; there is no type field, unlike engines);
  - then `FUN_01a1de68(&delta, &table[id][k], &knobs[slot][k])`, then the patch write-back `FUN_019ff664`.
- Range-table readers (no id bound anywhere):
  - encoders: `0x01a506bc`, `0x01977584`, `0x019da88e`;
  - ReadInterface: `0x01a28e62`, `0x01a2907a`, `0x01a29280`;
  - LFO targets: `0x019ca630`, `0x019cb5cc`, `0x019cd298`, `0x019cdfe2`, `0x019cf0e4`, `0x019d0160`;
  - MasterController: `0x019da092`;
  - `0x01a1df6a`; init `0x01a1f712`.
- **Row 8 would land at `0x01DE5C50 + 8*0x30 = 0x01DE5DD0`, the start of another table** (refs at `0x01DE5DD0`–`0x01DE5DE4`, e.g. `0x01a27d3c`). Verified: the encoder computes `id*0x30`. (The research pass first wrote `0x01DE5D10`, i.e. 4 rows: wrong.) So the table is **relocated** into the slot block, like the engine range table: 15 reader pairs repointed (`FX_TABLE_REFS` in build_slot.py); rows 0–7 are filled by Ranges init at boot through the new base, row 8 is static.
- Per-FX byte table `0x018F5EDC` (8 bytes, all 0; read by `FUN_01a27e70` / `0x01a27e98`). Entries 8 and 9 read padding (0), and the next table starts at `0x018F5EE6`. **Nothing to patch for id 8.**

## Screen — Confirmed
- ScreenId fx `FUN_01a3bec8`: `R1 = 8` @`0x01A3BEC8`; table `0x018F7FE4` (a string follows). Callers: synth `0x01a50548`, drum `0x019773be`, master `0x019d9716`.
- Cases are `P0=[FP+4]; UNLINK; R0=imm; JUMP (P0)`. The filter case (screen 0x0B) is `0x01A3BF26`.
- The filter screen object is at `0x01DD8B44`:
  - ctor `0x019E89DC`: `FUN_01a3bab0(obj, 0xB, "mmmf.svg")`;
  - page vtable at obj+0xC = `0x01910CD4`, with **update (+0x1C) slot `0x01910CF0` = `0x019E8308`**.
- The screen gets its context from `FUN_01a3b3e8` = slot `[0x01DF4E38]` and `FUN_01a3b3dc` = edit mode `[0x01DF4E3C]` (0 synth, 1 drum, 3 master). It reads normalised knobs with `FUN_01a28da0(slot, mode)` (ReadInterface).
- Hook like `_ui_update`: point `0x01910CF0` at our function.
  - It picks the id by mode: `0xFF801734[slot]`, `0xFF801754[slot]` or `0xFF801090`.
  - For id 8 it draws our screen; otherwise it does `JUMP.L 0x019E8308`. The filter screen exists even when filter is not in the DB.

## CPU — Confirmed / Hypothesis
- No static cost figures. TE times each stage with CYCLES into `{start, last, max, min, sum u64; count u32}` records in SDRAM:
  - whole callback: start `0x01BA2618`, last `0x01BA2620`, **max `0x01BA2628`**;
  - voices: `0x01BA269C`, max `0x01BA26AC`;
  - **synth LFO + FX: `0x01BA26F4`, last `0x01BA26FC`, max `0x01BA2704`**, count `0x01BA271C`.
  - Our screen can print them.
- FX processes 64 samples per callback. The voices process 6 × 128. So even 500 cycles/sample of FX is ~32 k cycles per callback, small next to the voices. Hypothesis: the core clock is unknown, so the absolute budget per 1.45 ms is too.

## What id 8 needs (patch plan; mirrors engine-slot.md)
| what | stock | patch |
|---|---|---|
| name table | `0x018DD810`, 8 × `{char*, int}` | 9-entry copy + name string in our block. `FUN_019aad38`: base `P1 = 0x018DD814` (.L/.H @`0x019AAD38`/`3C`) → copy+4, `P0 = 8` @`0x019AAD40` (`40 68` → `48 68`). `FUN_019aadf8`: `P5 = 8` @`0x019AAE02` (`45 68` → `4d 68`), base `P3` @`0x019AAE06`/`0A`. `FUN_019aae44`: base `P5` @`0x019AAE4A`/`4E`, `R4 = 8` @`0x019AAE5A` (`44 60` → `4c 60`) |
| mono factory `FUN_01a1b6d0` | `R1 = 8` @`0x01A1B6DE`, table `0x018F59AC` (`P0` .L/.H @`0x01A1B6E6`/`EA`) | bound 9 (`41 60` → `49 60`), 9-entry table; case 8 = our stub |
| stereo factory `FUN_01a1b5f0` | `R1 = 8` @`0x01A1B5F0`, table `0x018F5910` (@`0x01A1B5FE`/`602`) | same |
| ScreenId fx `FUN_01a3bec8` | `R1 = 8` @`0x01A3BEC8`, table `0x018F7FE4` (@`0x01A3BED4`/`D8`) | bound 9; case 8 = `0x01A3BF26` (filter screen 0x0B) |
| range table | `0x01DE5C50`, 8 × 0x30 (row 8 = `0x01DE5DD0`, another table) | 9-row copy in our block, 15 reader pairs repointed; row 8 static `{0, 0x7FFF, 0x7FFF}` × 8 |
| flag table `0x018F5EDC` | [8] = 0 | none |
| screen | update slot `0x01910CF0` = `0x019E8308` | → our hook (optional for FX1) |
| DB | `fx_types` | add `(11, '<name>', '[…8 ints]')`. Presets naming it need no firmware change |
- All four tables are followed by strings, so all must move; none has data-word users (scan of the image).
- Case stubs run inside the factory frame (`LINK 0x10`, R7 saved at `[SP+0xC]`). Mono: `R0 = R7; CALL 0x019E77B4` (filter mono ctor; it returns this); set `[R0] = our_mono_vt`; epilogue `P0=[FP+4]; R7=[SP+0xC]; UNLINK; JUMP (P0)`. Stereo: same with `0x019E784C` and `our_stereo_vt`.
- Vtables: copy the filter's (32 B from vptr; header words 0):
  - mono `0x01910C94` with +0x14 → our mono process; keep the dtor and init (init sets pos at +0x10);
  - stereo `0x01910CB4` with +0x14 → our stereo process. The stock stereo process calls the mono one directly, so both slots must be ours.
- Our state: stay inside the stock object's footprint plus the region room (mono ≤ 0x190 B from `0xFFB00600`, stereo ≤ 0x270 B from `0xFFB00790`). Delay lines go in SDRAM in our block; use separate buffers for mono and master, since both can run at once.
- Process ABI: like engine render (args R0–R2, stereo's 4th arg at `[SP+0xC]`, save R4–R7/P3–P5). Reuse the engine trampoline (LC1, L0–L3).
- Ranges init also runs per row at boot. If any reader caches rows before our first init, row 8 reads 0 (knobs stuck). Hypothesis; FX1 checks it.

## Proposed FX1 test image
- Id 8 named `ens`, DB row 11, defaults `[16384, 16384, 0, 0, 0, 0, 0, 0]`. Tables, case stubs, filter-vtable copies and range row as above. **No screen hook** (the filter screen shows; lowest risk).
- Mono process: `out = in * knob0` (Q15). Blue turns the level, which proves the call path, buffer and params.
- Stereo process: `L = L * knob0`; `R = R` delayed by `knob1 >> 6` samples (≤ 512) from a ring buffer in our SDRAM block. On headphones the master FX gives an audible Haas shift, which proves the L/R buffers.
- Exercise: select `ens` as synth FX, drum FX and master FX. Switch sounds and modes, load and save presets. Check the stock FX and filter still work.
- FX2: the real ensemble (master: 2–3 modulated delay taps, L/R in anti-phase; synth: mono chorus) plus a screen hook showing `0x01BA2704` / `0x01BA2628` max cycles.

## Open
- Whether anything else in the browser/UI bounds the FX id (the browser builds from the DB; not traced).
- That `0x01DE5D10–0x01DE5DCF` is really unused.
- The meters at `0xFF801774`: who reads them, and whether ours must write them.
- When the dtor (+0x0C) is called, if ever.

## FX1 as built (2026-10-08)
- `build_slot.py --fx ens`: tables above copied/grown in the slot block, 6 bounds 8 → 9, 21 `.L/.H` pairs repointed (names ×3, factories, ScreenId, ranges ×15); DB `fx_types` row `(11, 'ens', [16384, 0, …])`. After the patch no code builds an address in the stock tables (xrefs on the patched image).
- Factory cases (`FX_CASE` in slot.S) construct TE's filter FX in place and swap the vptr for a copy of filter's vtable (40 B from vptr − 8) with process → `_fx_mono_entry` / `_fx_stereo_entry` (fx.c). The filter screen (0x0B) shows.
- fx.c FX1: blue = gain (16384 unity); master only: green delays R by up to 511 samples. State (ring) in the slot block's `.data`.
- FX1 confirmed on device 2026-10-09: listed, selectable as synth/drum/master FX, params and both buffers as described, preset save/load fine.
- FX2: fx.c is the chorus ensemble (LOG.md test image 32); sim checks in `src/slot/test/fx_check.py`.

## Multiple FX (2026-10-09)
- `src/slot/fx.json` lists our FX in id order (8, 9, …): `{name, source, default, aliases}`; `src/slot/<source>.c` exports `<source>_mono(this, buf, params)` and `<source>_stereo(this, L, R, params)`. `build_slot.py --fx src/slot/fx.json` generates per FX: `FX_MONO_ENTRY`/`FX_STEREO_ENTRY` wrappers, two `FX_CASE` stubs, two filter-vtable copies, name (+ aliases), factory/ScreenId entries (filter's screen) and a full-range row.
- Name loops count entries including aliases; factories and ScreenId count ids.
- Sim: `src/slot/test/fxsim.py` (runner + helpers) and `fx_main.c -DFX=<source>`; one `<source>_check.py` per FX.
- Big buffers (delays) cannot live in `.data` (it is in the image). Use a FREE SDRAM range (not loaded, contents undefined at boot: never assume zeros). Assigned: region 1 `0x01E80000`–`0x01F10000` (576 KB, after the slot block) for the ping-pong delay.
- Renamed `ens` → `ensemble` (alias `ens` keeps saved patches loading).
- 2026-10-15: four FX: ensemble 8, pingpong 9, distortion 10, crush 11 (notes/fx-crush.md). Screen kinds by source in
  build_slot.py `FX_UI_KINDS` (crush 4); name loops 14, factory/ScreenId bounds 12.

## ensemble (2026-10-16 rework: BBD chorus, CE-2 by default)
The first version was a Juno/Solina string ensemble (LOG test image 32); the user wanted classic chorus tones
("like the CE-2"). `src/slot/ensemble.c` now:
- Blue RATE: triangle LFO 0.1 .. 8 Hz, exponential. Green DEPTH: sweep 0 .. +-3 ms. Orange MIX: dry .. wet at constant
  average power, 50/50 = 0.707 each (level within 0.2 dB of bypass, averaged over 200 .. 5000 Hz).
- White CHARACTER (`ensemble.h`): 0 .. 8191 CE (one delay line, fixed 6.0 ms centre; master: L = dry + wet,
  R = dry - wet); 8192 .. 12287 crossfade (R's wet changes sign: spread over >= 16 blocks); 12288 .. 20479 DUAL
  (1.66 ms + depth, R's line swept in anti-phase, Juno 60 style); 20480 .. 32767 the old three-tap ensemble fades in
  (taps at 120 / 240 degrees, 6.5 Hz / 0.2 ms vibrato, base 1.66 -> 2 ms + depth); the top is the old sound (now
  band-limited like the rest).
- Default `[15360, 21504, 0, 16384]`: CE, 0.78 Hz, +-1.97 ms, 50/50.
- Wet chain (once per channel, before the line; all linear around the clip, so it commutes with the delay): 2-pole
  low-pass -> pre-emphasis 2x - x[-1] -> soft clip u (1.5 - u^2/2), u = 2/3 v saturated (flat at 1.5x full scale;
  HD3 ~-40 dB at -6 dBFS before the 50 % mix) -> de-emphasis (exact inverse) -> 2-pole low-pass. The two sections
  are a 4th-order Butterworth at 9 kHz (-3 dB 9 kHz, -15 dB 12 kHz, -31 dB 15 kHz). No compander noise modelled.
- Linear interpolation: the line holds band-limited audio, so the loss swings 0 .. -0.5 dB at 5 kHz (measured 0.49 dB
  ripple at full depth). Cubic would cost ~10 cycles per tap-sample more for little audible gain below 9 kHz.
- All per-sample loops in inline asm (16-bit fractional MACs, circular write); weights and mix ramp per sample.
- Reference values (from memory, not measured here; marked where unsure):
  - Boss CE-2: MN3007 (1024 stages) + MN3101 clock, mono, dry + wet ~50/50, triangle LFO. Rate knob roughly
    0.1 .. 6-8 Hz (unsure of the exact ends); delay centre around 5-7 ms with up to ~+-2-3 ms sweep (unsure: from
    common descriptions, not a measured unit). Pre/de-emphasis around the BBD and ~8-10 kHz filtering (unsure of
    order and corner). No compander (fairly sure).
  - CE-1: stereo out = dry + wet / dry - wet (wet inverted on one side): the master's CE wiring.
  - Juno 60 chorus (MN3009, two lines in anti-phase): I 0.513 Hz, II 0.863 Hz, delay ~1.66 .. 5.35 ms; I+II 9.75 Hz
    at ~3.3 .. 3.7 ms (widely quoted figures; I+II's fast vibrato is above our 8 Hz and not provided). Blue 12288 =
    0.517 Hz (I), 16384 = 0.894 Hz (II, 3.5 % fast); DUAL at depth 20206 (+-1.85 ms) spans 1.66 .. 5.36 ms.
- Checks `src/slot/test/ensemble_check.py`; cycles: CE default 14.3 k per stereo call (223 per frame), DUAL ~14 k,
  white max 21.2 k (old ensemble 21 k), mono 8 .. 12.3 k.
