# Drum-side engine: our own drum kit next to TE's sampler and dbox (firmware 246)

Static research 2026-10-13 (decompiles and disassembly of `OP1_vdk_246_dec.elf`, scratch in the session scratchpad
`drum/`). **Confirmed** = read in the code, **Verified** = re-checked at the instruction, **Hypothesis** = inferred.
Probe images: DRUMPROBE.OP1 (LOG.md test image 56), SAVEPROBE.OP1 (57: save and load).

## Answer first
- Drum engines are producers like synth engines, with the same name -> id table, but their own factory, voice loop,
  ScreenId, encoder path and patch validation. Only ids 8 (`drum`, the drum sampler) and 9 (`dbox`) exist on the drum
  side; `ls` (10) is not a drum engine (the drum factory asserts on it). Confirmed.
- A new drum engine needs a **new id** (after our synth ids; 18 in DRUMPROBE: va 15, wavetable 16, grain 17), its name
  in the grown name table, and five small patches (factory, ScreenId, encoder, validator, screen). No encrypted L1 code
  is touched. It reuses dbox's voice objects with a copy of dbox's vtable (render, note-on, active, note-off ours), the
  way our synth engines reuse iter's.
- Per-key parameters exist: 24 keys x 8 int16 per drum slot; the drum knobs edit the last-pressed key's 8 values
  (SHIFT for 5-8). TE only loads/saves them for types `drum` and `dbox`, so a new type's knob edits are **not saved**
  (Confirmed). SAVEPROBE patches the loader's and saver's "dbox" compare to accept our type ("Save and load" below).
- Drum patches are validated on load (patch.cpp:1018). A drum patch whose id is not 8 or 9 fails with error 13, and in
  drum context that is an **assert** (patch.cpp:221 "Invalid drum patch"). So the validator must treat the new id as
  dbox, and the patch must carry a valid `dbox_data`. Confirmed.

## Name -> id
- `drum_types` (factory DB) has one row: `(1, 'dbox', default_params)`. The drum sampler (`drum`) has no row; its
  presets have `"type": "drum"`. Confirmed (DB).
- The drum patch loader `FUN_01a006c0` reads `type` and maps it with the **synth** name lookup `FUN_019b9810`
  (instr_types.cpp, the table build_slot.py grows), storing the id in `0xFF801704[drum slot]`. Confirmed (0x1a006f0).
- `drum_types` is read only when a new sound is made from a type (`FUN_0193f86c`: `dbox` -> `dbox_data` = 24 x
  default_params; other types -> `knobs` = default_params). The probe adds no `drum_types` row. Confirmed / Hypothesis
  (which UI calls it).
- Presets: `drum_presets` (14 `drum`, 10 `dbox`, folder = type). **`patch.cpp:656/664`: factory drum presets plus the
  user's `drum_user` rows must stay <= 58** (`0x3a`, counted together, assert at boot, `FUN_01a04fe8`). Saving refuses at
  58 (`FUN_019feee4`), so stock leaves room for 34 user drum sounds. Adding a factory drum preset would make a user with
  34 saved drum sounds fail to boot. The probe **replaces** one dbox preset (whathump) instead of adding one. Confirmed.

## Factory — Confirmed
- `FUN_01a1b9c0(id R0, Producer **out R1, drum slot R2)` (producer_factory.cpp): id < 8 -> assert at `0x01A1BA38`;
  8 -> 12 x `FUN_01978df4(i)` (DrumSampler, 20 B each at `0xFFB00000 + i*0x14`, vptr `0x0190FAC4`), sample buffer from
  `FUN_01978da8(slot)`; 9 -> 12 x `FUN_0197189c(i)` (dbox, 0x48 B at `0xFFB00000 + i*0x48`, vptr `0x0190F8A4`) then
  `FUN_01970da8(voice, i)` (stores i at +0x14); anything else -> the same assert. `kDrumVoiceCount` = 12.
- Callers: audio thread `FUN_ffa017ec` (mode/slot change) and `FUN_01967b28` (UI), id `0xFF801704[slot]`, slot
  `[0x01AAAC10]`; producers in `s_drum_producers[12]` at `0x01AAAD30`.
- dbox vtable at vptr `0x0190F8A4` (8-byte entries `{adj, fn}`, like iter's): +0x0C dtor `0x01970EBC` (only resets the
  vptr to the base `0x0190F894`), +0x14 render `0x01970EE8`, +0x1C note-on `0x01970DE6`, +0x24 active `0x01970E5C`,
  +0x2C note-off `0x01970E7E`. Only the vtable references render (data scan). Confirmed.

## Voice render path — Confirmed (L1 `FUN_ffa00684`, called from `FUN_ffa017ec` in drum mode)
- Per voice v = 0..11 with `active[v]` (`0xFF801070`, bytes):
  1. key = `0xFF801334[v]` (int32, 0..23); params = the key's 8 int16 from `0x01AAADB4 + slot*0x180 + key*16`
     copied to `0xFF801724` (then modulated by the voice's LFO object `0x01AAAD84[v]`).
  2. pitch: `0xFF801950[128]` = table `0x0106B248[(params[0] >> 9) + 0x30]` x ratio `0xFF801304[v]` (constant over
     the block); velocity: `0xFF801D50[128]` = `0xFF8012EC[v]` (int16).
  3. `render(this, out = 0xFF80113C, params = 0xFF801724)`: **128 mono int16 per call, R0-R2 like the synth render**.
  4. `active(this)` (+0x24); 0 clears `active[v]`.
  5. mix: `0xFF800BB4[i] += sat(2 * out[i] * W[0x01B4EF6E])`, then the 2:1 decimator: **88.2 kHz** like the synth
     side, then the drum FX `s_fx[1]` (notes/fx-slot.md).
- Note-on `FUN_01a84ba0(note{key + 0x35, vel, ...})` (VoiceCtl, caller's thread): MIDI notes 0x35..0x4C only; takes a
  free voice or steals the one whose `active()` returns the largest value; writes `0xFF8010F4[v]` = key,
  `0xFF8012EC[v]` = sat(vel << 8), `0xFF801304[v]` = 2^(x/12) from its second argument, `0xFF801334[v]` = key,
  `0xFF801044` = key (the **edit key** the knobs act on); then `FUN_01967ccc(v)` = vtable +0x1C
  `note_on(this, &params[slot][key][0])`. Note-off `FUN_01967d64`: +0x2C with the same pointer.
- The OP-1's own keys send velocity 0x7F (0x7F00 here); only MIDI gives other velocities. Hypothesis (keyboard path
  `FUN_01a85014(&n, key, 0x7F)`, notes/hold.md).

## Per-key parameters and knobs — Confirmed
- `0x01AAADB4`: [8 drum slots][24 keys][8 int16] (0x180 per slot). Loaded by `FUN_01a0036c` and written back by
  `FUN_019ffc50` only when the patch `type` string is `"drum"` (pitch/start/end/... arrays) or `"dbox"` (`dbox_data`);
  other types are skipped (the values stay what the slot held before). Verified: each compares `type` first with
  Value("drum"), then with Value("dbox") through `CALL FUN_019bdea4` (JSON value ==): loader `0x01A00630`, saver
  `0x019FFFBA`. The saver only writes into the patch's existing JSON, then calls the patch's +0x34 hook.
- Drum encoder `FUN_01977430`, page 0 (T1): `FUN_01a1e57c(knob 0..7, &delta, id, &params[slot][edit key])`.
  `id == 9` (`0x01A1E59A`) -> each knob k clamps params[k] with range-table **row 9** (the table build_slot.py moves;
  TE adds 0x240 to its base); other ids -> the sampler logic (knobs 1/2 and 5/6 are start/end, coupled) with row id.
  Then `FUN_019ffc50` (patch write-back). Pages 1-3: dyna_env (`0xFF801544`), FX, LFO.
- dbox's 8 ranges are filled at boot (Ranges init, like rows 0-14). From the factory presets: param 0 is signed
  (-24320..16878 seen), params 1-7 within 0..32767. Exact limits: Hypothesis; the probe screen shows row 9.
- ReadInterface `FUN_01a28010(slot, key)` normalises a key's params with row id and asserts if a row has max == min
  (ReadInterface.cpp:59), so the new id's row must be a real range.

## Screen — Confirmed
- Drum ScreenId `FUN_01a3bfda` (only caller `0x0197739a`, id `0xFF801704[0xFF8016D8]`): 8 -> screen 0x10, 9 -> 0x11
  (dbox), else assert. Separate from the synth ScreenId `FUN_01a3bf44` (which sends 8-10 to an assert).
- dbox screen object `0x01CC0BB4` (`dbox.svg`), page vtable `0x0190F8D4`, **update +0x1C at `0x0190F8F0` =
  `0x01972470`** (the only reference). Same hook shape as iter's and the filter FX's.

## Validation — Confirmed (`FUN_01a037a8(patch, is_drum)`, patch.cpp:1018)
- Unknown name -> error 1; id 8: pitch/start/end/volume/reverse/playmode x 24; id 9: `dbox_data` 24 x 8 within row 9;
  other ids: error 13 if `is_drum`, else `knobs`/`adsr`. Ids 8/9 also need `dyna_env`; then fx/lfo for all.
- Drum-context failures assert (`0x01a04c00`, patch.cpp:221). This is also why stock firmware stops at boot on a drum
  sound naming an unknown engine.

## Route chosen (DRUMPROBE) and why
New id N = first id after our synth engines, so nothing collides: synth 15.., drum N, FX 8.. (separate id space).
| what | patch |
|---|---|
| name table | + `dprobe` -> N (build_slot.py's grown table) |
| synth factory / synth ScreenId (bounds grow with the table) | case N = TE's case for 9 (synth-side assert, as for dbox) |
| range table | row N: static full range, then a copy of row 9 at the first factory call (`drumprobe_slot_init`) |
| six flag tables | entry N = entry 9 (dbox's LFO behaviour) |
| drum factory | the assert path's first instruction `0x01A1BA38` -> `JUMP.L _drum_factory` (slot.S): id N builds 12 dbox voices and swaps their vptr to our vtable; other ids go back to the assert |
| drum ScreenId | `0x01A3BFE8` `R1 = 9` -> `R1 = N + 1`, `0x01A3BFEA` `CC = R0 == R1` -> `CC = R0 < R1 (IU)`: 9..N -> dbox screen |
| drum encoder | `0x01A1E59A` `R0 = 9` -> `R0 = 8`, `0x01A1E59C` `CC = R6 == R0` -> `CC = R0 < R6 (IU)`: ids > 8 take dbox's path (8 independent knobs, row 9) |
| validator | CALL `FUN_019b9810` at `0x01A03884` -> `_drum_validate_lookup`: N reads as 9, so our patches validate as dbox |
| screen | dbox page update `0x0190F8F0` -> `_drum_ui_update`: id N draws the probe screen, else TE's update |
| DB | dbox preset `whathump` replaced by `drum probe` (a dbox preset with type `dprobe`; keeps its valid `dbox_data`) |
Only ids that used to assert change behaviour in each patched function (ids >= 10 in ScreenId and the encoder; N in
the factory and validator), so TE's drum sampler and dbox are untouched.
Rejected: swapping dbox's render (changes TE's dbox); marking our sounds as dbox with a magic value (fragile, and
the knobs would still edit dbox's meaning). Hooking the per-key loader/saver's compares was rejected for DRUMPROBE
(boot risk) and done in SAVEPROBE with the smallest change found (next section).

## Boot safety
- Drum slots load at boot, so every patched path runs at boot when a slot holds the probe. The validator wrapper only
  changes the result for N; the factory stub only for N.
- **Before going back to stock or another image, replace the probe in every drum slot and delete saved probe drum
  sounds**: stock asserts at boot on a drum sound named `dprobe` (patch.cpp:221). Recovery: COM at power-on, then
  factory reset.
- Drum preset count unchanged (24); user headroom stays 34.

## Save and load (2026-10-14, SAVEPROBE)
Static research (decompiles in the session scratchpad `save/`), then DRUMPROBE's device result.
- **Snapshot save: Confirmed.** Holding a sound key runs `FUN_019ad090` (global controller): a 400-frame countdown; at
  320 it checks free space and, in drum mode, `FUN_019feee4` (factory + user drum sounds < 58). Then, in drum mode:
  `FUN_01995bac` reads the slot's row `SELECT patch FROM drum_slots WHERE id=%i`, parses it, sets `name`
  (`%i%02i%02i_%02i%02i`, e.g. `20760319_2255`), writes `user/audio/preset_drum/snapshot/<name>.raw` (a copy of
  the slot's sample for type `drum`, `FUN_01995868`; an empty file otherwise), then:
  1. `FUN_01995b80` -> `FUN_019955ec("drum_user", ..., is_drum = 1)`: validates with `FUN_01a037a8` and only on success
     `INSERT INTO drum_user (patch,folder)`. **A failed validation drops the sound silently** (no message).
  2. `FUN_01995c88`: rewrites the slot's `drum_slots` row.
  3. `FUN_01a04c48(slot patch, slot, json)` -> `FUN_01a04bb0`: validates again and **asserts** on failure
     (patch.cpp:221).
  4. `FUN_01a04fe8`: rebuilds the drum preset list from `drum_presets` and `drum_user`, validating every row with the
     assert (and the <= 58 count assert): this also runs at boot.
  No step compares the type with "drum"/"dbox" except the raw-file copy.
- **The DRUMPROBE hypothesis was wrong. Confirmed** by the device (coordinator, 2026-10-14): the save wrote
  `20760319_2255` (type `dprobe`), and every image without `dprobe` stops at boot on it ("Invalid drum patch
  20760319_2255: error 1": unknown name). A dropped save was impossible anyway: step 3 would have asserted. The saved
  sound carries the `dbox_data` the slot's patch held, i.e. whathump's (the probe preset's), **not the edits**: the
  saver skipped our type. Loading it skipped `dbox_data` too, and DRUMPROBE's marker check then rewrote the defaults.
- **Why it wasn't listed: not found.** The drum browser (`FUN_019776dc` -> `FUN_01a028ec`) adds every factory and
  user drum sound by folder and name (`FUN_01a185f4(list, folder, name)`), with no type filter; user sounds are
  inserted with the folder `FUN_01995bac` sets. Hypothesis: it is listed in a user/snapshot folder rather than with
  `drum probe` in dbox. SAVEPROBE's test asks where it shows up.
- **A `dprobe` sound without `dbox_data` does not assert. Confirmed** (jsoncpp 0.5.0 in TE): the validator reads
  `dbox_data[k][i]` with non-const `[]`, which turns a missing member into nested arrays of nulls, and `asInt(null)` =
  0 (`FUN_019be0c0` case 0), which is inside row 9 (K0 -24320..24832, K1-K7 from 0). The loader's const `[]`
  (`FUN_019beb70`/`FUN_019bea94`) returns the shared null (`0x01DD4FE4`) for a missing member or index. So such a sound
  loads as zeros. A wrong shape (a string where an int is expected) asserts, in TE's validator as for stock.
- **Fix (SAVEPROBE): the two compares.** Both `CALL FUN_019bdea4` sites go to `_drum_type_eq_load` / `_save`
  (entry.inc `DRUM_TYPE_EQ`): TE's own compare first, returned unchanged when true ("dbox"); only when it is 0, a
  second compare with Value("dprobe"), built and destroyed exactly as TE builds its temporaries (`FUN_019bdc4c` into
  16 bytes of stack, `FUN_019bdd18(v, 2)`). "drum" never reaches these sites (its own compare comes first), so `drum`
  and `dbox` patches take the same path with the same result; the only extra work is for other types. Our type now
  loads and saves `dbox_data` like dbox. Counted in `dp_type_hits` (screen: LD / SV).
- **Marker removed.** The loader runs before the factory (`FUN_01a006c0`: id, dyna_env, `FUN_01a0036c`, then
  `FUN_01a85e40` -> factory -> `drumprobe_slot_init`), so DRUMPROBE's "defaults unless key 23's K7 holds the marker"
  would have overwritten every loaded sound without the marker (whathump's data, or a user edit of K7 on key 23). The
  factory preset now carries the defaults in its `dbox_data` (engines.json `dbox_default`, checked against the stock
  dbox presets' per-column range by build_slot.py) and nothing rewrites a slot.
- Boot: the compares run at boot for every drum slot (TE loads each slot). For `dbox` slots the only change is a few
  instructions around TE's own compare; `drum` slots don't reach them. Risk: an error in our wrapper would stop the OP-1 at boot with a dbox or
  probe kit in a slot; the wrapper is checked in the simulator against stand-ins for TE's JSON functions, with the
  register-preservation harness (`src/slot/test/knobhook_check.py`).

## DRUMPROBE as built (2026-10-13)
- `src/slot/drumprobe.c` (+ `drumprobe.h`), in the slot block; engines.json `{"drum": true, "test": true}` entry
  `dprobe`; `build.py --engines va,wavetable,grain,dprobe --name DRUMPROBE` -> va 15, wavetable 16, grain 17,
  **dprobe 18**. build_slot.py: the tables above (names 21 entries with aliases, factory/ScreenId bound 19), 6 drum
  halfwords + the validator CALL + the dbox update pointer; slot.S `DRUM_HOOKS`; vtable = dbox's 12 words with
  render/note-on/active/note-off -> C_ENTRY wrappers. Checked in the patched LDR (scratch `drum/verify.py`).
- Engine (drumprobe.c header): sine with a pitch drop per key (E1 + key semitones), noise burst on odd keys,
  velocity from `0xFF8012EC[v]`; knobs per key: blue tune, green decay, white pitch drop, orange noise level,
  SHIFT+blue noise decay. In DRUMPROBE, K7 held a slot marker and `drumprobe_slot_init` rewrote defaults without it
  (removed in SAVEPROBE); it copies range row 9 to our row once TE has filled it.
- Screen (ui.c `drum_ui_draw`): last note-on (key, velocity, voice, note-on/off counts), edit key, slot, id, slot inits,
  row copied, render calls per frame, worst render cycles, TE's pitch ratio; per knob the edit key's value, the newest
  voice's rendered value and row 9's min..max and scale; a level bar per voice.
- Sim (`src/slot/test/drumprobe_check.py`): all pass; max 12,700 cycles per render call (budget 76,800); ABI through the
  entry wrappers. Host preview: `preview.py OUT 300 0 0 0 0`.
