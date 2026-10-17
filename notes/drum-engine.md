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
- **Why it wasn't listed: it was, in the user column.** The drum browser adds every factory and user drum sound by
  folder (no type filter), and `FUN_01995bac` saves every drum snapshot with folder `"user"` ("Browser columns"
  below). Device (SAVEPROBE): `20760319_2255` was in the snapshots.
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

## Browser columns (2026-10-15, static; decompiles in the session scratchpad `br/`)
How the drum sound browser (the preset list a drum slot loads from) builds its columns. **Confirmed** unless marked.
- **Columns = the distinct `folder` values of the drum sounds; label = the folder name; order = sorted.** No fixed
  list and no table (`drum_types` is not read here).
  1. `FUN_01a04fe8` (boot and after every save) fills `s_drumPresets` (`0x01DE20F8`, 0x38 bytes per sound, count
     `0x018B6118`) from `drum_presets`, then `drum_user`, both through `FUN_0196fe8c` = `SELECT id,patch,folder FROM
     %s` (`0x018D4200`, no WHERE, no ORDER BY: rowid order). Entry: +4 the parsed patch, +0x14 the folder string.
  2. `FUN_019776dc` (DrumController, opens the browser) -> `FUN_01a028ec`: clears its list (`0x01DDD698`) and, for
     every sound, `FUN_01a185f4(list, folder, patch["name"])`.
  3. `FUN_01a185f4`: linear search of the list's columns (0x30 bytes each, name std::string at +0) with
     `FUN_01a17918` (std::string compare); no match -> `FUN_01a17d44(col, folder, -1)` + push_back (`FUN_01a182ec`),
     then **`FUN_01a18a58(begin, end, depth)` = std::sort of the columns** (insertion sort under 33 columns), comparator:
     a column named `"thru"` (`0x018F54FC`) first (`FUN_01a17cde` = equals C string), otherwise `FUN_01a17918` <, i.e.
     memcmp byte order (case-sensitive). The sound is appended to its column (`FUN_01a18050`). Verified: the CALL at
     `0x01A1874C` takes R0/R1 = the list's begin/end, only when a column was created.
  4. `FUN_01a174dc` (PresetBrowserController) -> `FUN_01a172d4` selects the column holding the slot's current sound;
     the screen (`FUN_01a15f8c`, presetbrowser.svg) shows 6 column labels at a time (scroll arrows above 6), each
     `snprintf(buf, 14, "%s", column name)`: **the label is the folder name verbatim, at most 13 characters.**
     Verified (`0x01A1601A`-`0x01A1603C`: R3 = the column's string data, R1 = 0xE, format `0x018F4C24` "%s").
  Stock: `dbox` (10), `drum` (14), `user` (snapshots). Items within a column are listed alphabetically (device,
  DRUMPROBE: `drum probe` at row 4 of dbox; the code path `FUN_01a18050` not read).
- **Snapshots go to folder `"user"`.** `FUN_01995bac` sets the record's folder (+0x1c) to `"user"` (`0x018D9A18`) for
  every drum snapshot, whatever the type; `FUN_019955ec` inserts `INSERT INTO %s (patch,folder) VALUES('%s','%s')`
  with that string (`0x01995688`: R7 = record + 0x20). So a saved fmdrum sound lands in `user` with every other
  snapshot, in any image.
- **Other readers of the folder** (every code reference to `0x01DE20F8`/`0x01DE20FC` and the count, decompiled):
  - `FUN_01a025e8` (DrumController `0x01977906`, the drum *type* browser): `drum_types` rows plus only the sounds whose
    folder == `"drum"` (`0x018F14D0`). Our kits were never in it and still aren't.
  - `FUN_0193f578` / `FUN_0193f86c` (load a sound into a slot / new sound from a sampler kit): use the folder only in
    the sample path `content/audio/factory_drum/<folder>/<name>` or `user/audio/preset_drum/<folder>/<name>`, and only
    when `type == "drum"` (`FUN_019bdea4` against `"drum"` `0x018C78B8`). dbox-type sounds (ours) never take it.
  - `FUN_01a01dc8` (the slot's current sound -> index), `FUN_01a04dc0` (slot vs preset compare): name only.
  - `FUN_01995430` (fat_interface.cpp, disk mode) reads `SELECT folder FROM %s WHERE id=%i` for user records.
  No code compares a folder with `"dbox"`: the DRUM_TYPE_EQ wrappers, the validator, the factory, the dbox_data
  loader/saver and the 58 count all work on `type` or the count, not the folder. Hypothesis (static coverage): the
  folder string literals are built from base + offset, which tools/xrefs.py can't pair, so the claim rests on the
  readers of the preset array above, not a string scan.
- **The synth browser works the same way** (Hypothesis, from the call sites only): `FUN_01a185f4` is also called from
  the synth list builders (`0x01A0275A`, `0x01A0283A`), and synth presets have folder = type (LOG: an engine is listed
  only once it has a `synth_presets` folder).
- **fmdrum's column (DRUMFM3):** build_slot.py inserts a drum engine's kits with folder = the engine name, so the
  browser shows `dbox`, `drum`, `fmdrum`, `user`. dprobe keeps replacing `whathump` in dbox (a test probe; moving
  it would only need the same folder change). Limits unchanged: the 58 count is over all folders (factory 28, 29 from test image 82;
  user 30); no per-folder limit was found. Edge case (Hypothesis): a sampler kit imported over USB into a disk folder
  named `fmdrum` would share our column (the browser only groups by name).
- Rejected: a `drum_types` row (feeds the type browser, not this one, and makes new sounds from `knobs`, not
  `dbox_data`, in `FUN_0193f86c`); a code hook (not needed: the column is data).
- Check: `src/slot/test/drum_browser_check.py FACTORY.db --stock STOCK.db` re-implements steps 1-4 on a built DB.

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

## fmdrum: an FM drum kit (2026-10-14, test image DRUMFM1)
A real drum engine on the DRUMPROBE/SAVEPROBE route: `src/slot/fmdrum.c` (+ `fmdrum.h`, tables and kits from
`src/slot/gen_fmdrum.py`), screen `src/slot/ui_fmdrum.h`, sim `src/engine/test/fmdrum_check.py`. engines.json
`{"drum": true, "kits": "src/slot/fmdrum_kits.json", "test": true}`.

### Several drum engines (build_slot.py, slot.S DRUM_HOOKS)
- Drum engines take the ids after the synth engines, in engines.json order (DRUMFM1: dprobe 23, fmdrum 24). Each gets its
  own vtable (dbox's 12 words, render/note-on/active/note-off -> its C_ENTRYs) and `<source>_id`.
- `DRUM_HOOKS first, count, names...`: the factory takes `id - first < count (IU)`, loads the engine's vtable into R4
  (saved in TE's frame) and calls its slot init through `_drum_inits[i]`, then builds dbox's 12 voices as before; the
  validator lookup and the dbox screen update test the same range; `DRUM_TYPE_EQ` compares the type with each name in
  turn after TE's "dbox" compare (each Value built and freed as before). `_dp_type_hits` moved into DRUM_HOOKS.
- Drum ScreenId: `R1 = last + 1` (25). The screen update reads the drum slot / ids through symbols (`_te_drum_slot`
  0xFF8016D8, `_te_drum_ids` 0xFF801704; same instructions on the device) so the simulator can map them.
- ui.c `drum_ui_draw(slot, id)` dispatches by `drum_kind[id - drum_first]` (build_slot.py DRUM_KINDS: drumprobe 1,
  fmdrum 2).
- DB: `"kits"` adds one preset per kit in the engine's own folder (`fmdrum` since DRUMFM3, `dbox` before; "Browser
  columns"), patch fields from a stock dbox template, type fmdrum; every
  per-key value is checked against the stock dbox presets' column range; factory drum presets are capped at
  MAX_FACTORY_DRUM_PRESETS = 28 (stock 24 + 4 kits), so users keep room for 30 saved drum sounds (was 34); 29 with
  the fifth kit (test image 82): room for 29.
- Sim: `src/slot/test/drumhooks_check.py` (factory per id with TE's frame and assert path, validator, screen dispatch,
  register preservation); knobhook_check covers two type names; drumprobe_check unchanged.

### Sound (fmdrum.c)
Per key: a 2-operator phase-modulation drum, carrier `sin(phc + I(t) sin(phm))`, the modulator at RATIO x the carrier,
both swept together (pitch envelope in semitones, exponential), plus LCG noise through a Chamberlin SVF whose lp/bp/hp
outputs are blended. Three envelopes per voice (amplitude for tone and noise, the modulator's index, the sweep), one
step per 128-sample block; all gains, the index and both increments ramp linearly across the block. A note starts with
a 32-sample attack to its start values (index and sweep at full); a retriggered or stolen voice keeps its phases and
ramps from its current gains (no click). Aliasing: carrier held below 0.45 fs, modulator faded out between 0.4375 and
0.5 fs, index limited so fc + (1.5 I + 4) fm <= 0.77 fs (beyond that the sidebands are below -50 dB; only components
above 0.77 fs fold back under 20 kHz). DC: 8 Hz one-pole high-pass (a 1:1 ratio puts a sideband at 0 Hz); soft knee. A note is over at -70 dB of its
amplitude envelope; the voice then plays out the DC blocker's own decay (rounded toward 0) until under 4 LSB and only
then ends (since 2026-10-17: ending at once cut that decay, a step of up to ~-30 dBFS after short low notes). Such a
voice reports itself to TE as the first to steal; a note on it starts from silence (phases and filters cleared, the
DC blocker's value continues).
State: `fmdrum_voice[12]` in the slot block, indexed by TE's voice index; note-on (TE's caller thread) only writes key,
velocity and a trigger count that render picks up.

### Knobs (per key, dbox's row 9: K0 -24320..24832, K1-K7 0..32767; UX review 2026-10-14: an amount on each plain knob, its time or partner on SHIFT)
| knob | name | range and curve |
|---|---|---|
| K0 blue | PITCH | 512 per semitone (dbox's own convention), 0 = A3 220 Hz: 14 Hz .. 3.6 kHz |
| K1 green | SWEEP | pitch starts 0 .. +48 semitones above, quadratic (half = +12) |
| K2 white | FM | peak index 0 .. 16 rad over detents 0..20, quadratic (detent 10 = 4 rad); velocity scales it 0.5 .. 1; then FEEDBACK 1..7 (detents 21..27) and SQUARE 1..5 (28..32): "Harsh zones" below |
| K3 orange | DECAY | amplitude (tone and noise) T60 15 ms .. 4 s, exponential |
| K4 SHIFT+blue | RATIO | modulator : carrier, 32 steps (1024 each): the 15 harmonic ratios (n/2: 0.5 .. 15.5, TONE) rising, then the 17 inharmonic ones (0.71 .. 9.42: sqrt 2, 2.76, pi, 5.19, ...; METAL) rising |
| K5 SHIFT+green | SWDEC | sweep time constant 1 .. 300 ms, exponential |
| K6 SHIFT+white | FMDEC | the FM index's own decay, T60 3 ms .. 3 s, exponential |
| K7 SHIFT+orange | NOISE | by detent: OFF 0; AIR 1-4 (dark lp->bp 300 Hz..1.5 kHz, level to 0.55, under the tone); SNARE 5-9 (band 1.5..5 kHz, level to 1, tone kept); HAT 10-14 (bright bp->hp 5..9 kHz, the tone fades out); CLAP 15-19 (noise only, the band falls back to 1.2 kHz and narrows, three bursts 8.7 ms apart fade in); METAL 20-25; GRIND 26-32 ("Harsh zones"). The sound runs one detent ahead of the knob (each zone's first detent is already in it, its last is the zone at full). Stock dbox values stop at 32255: GRIND. Until FX4 the zones were OFF < 1024, then 8192 each from 1024 |
TE's LFO reaches K0-K3 only (knob-hook.md): PITCH, SWEEP, FM, DECAY. Noise shares DECAY (one amplitude envelope):
snares may sound flat where a real snare's rattle outlasts its body (device question in LOG 65).

### Hi-hat choke
Keys 10..13 are a choke group: a note-on there on a key whose NOISE is in the HAT or METAL zone sets `choke` on every
other sounding voice of the group in the HAT or METAL zone (the same key too: one hi-hat); render then decays it with a 5 ms T60
(ramped per block, no click). Keys outside 10..13 and group keys without HAT noise are never choked. note-on reads the
other keys' values through its `key_params` pointer (TE's [slot][key][8] row, so the same slot).

### Kits (folder fmdrum since DRUMFM3, 24 keys each; gen_fmdrum.py)
Shared key roles in all four: 0-2 kicks, 3/5/7 toms, 4/6 snares, 8 rim, 9 clap, 10-13 hats (the choke group, all in
the HAT zone), 14-15 cymbals, 16-23 free (percussion, effects). `fm 808` (template dem drums): its fifth kick is now a
ride; clave, cowbell, congas, maracas, floor tom, sub boom, ping on 16-23. `fm 909` (lo-fi, punch FX on). `fm metal`
(cuckoos nest, delay on): metal kick, gong, spring hit; metal and anvil snares; tin rim; clap; metal hats; shimmer and
crash; bells on a scale, chime, glock, triangle on 16-23. `fm zaps` (dem drums): zap kicks, zap snares, tick rim, clap
zap, swept hats, zap crash, metal zap; lasers and blips on 16-23.

### Screen (ui_fmdrum.h)
Title in white: the selected key's class (KICK, TOM, SNARE, CLAP, HAT, METAL, BELL, ZAP, PERC, from its values) and
number; after a knob turn, for ~1.5 s (45 frames), the knob's name and value in its colour instead (as harm: the 8
values compared every frame, a change of key is not a turn), e.g. DECAY 450MS, NOISE HAT, RATIO 1.41 METAL. An oblique
drum in grey and white (size = pitch, shell depth = DECAY, an FM rosette of 2 x RATIO petals with depth = FM on the head,
SWEEP as a green curve falling onto it, noise as white/grey speckle told apart by height and density, clap marks for
CLAP); it hops, ripples and follows the live index while the key plays. An envelope graph on log time, 40 units tall,
ticks without labels (DECAY orange, FM index white, sweep green, playhead). Grey labels over the values (PITCH SWEEP FM
DECAY), a dimmer SHIFT row (RATIO with TONE/METAL in the readout, SWDEC, FMDEC, NOISE); times as nnnMS below 1 s and
n.nSEC above. Knob colours only on knob values; class colours only as marks in the 24-key strip (CLAP lavender, ZAP
red). The stroke font's S is now chamfered (gen_font.py), so it no longer reads as 5 (1.2SEC, 35MS) on every screen.

### Checks (sim, before DRUMFM1)
fmdrum_check: pitch within 0.2 cents; sweep follows f0 2^(S e^(-t/tau)/12); FM sidebands = Bessel J_k(I) within
0.5 dB; MOD decay; aliasing at the extremes <= -48 dB (energy off the true sidebands below 20 kHz); DC <= 1.1 %;
decay T60s; noise zones; clap bursts; velocity; no clicks on knob changes or retrigger; all 96 kit keys -11..-0.1 dBFS,
one sample >= 32000 in 6.1 M; the hi-hat choke (6 ms after the closing hit; not across key 14, a SNARE-zone hit or a
group key without HAT noise); RATIO's order; NOISE's stock maximum still CLAP; no libgcc calls; worst render 21,000
cycles per call (164 per sample; tone-only keys ~85); ABI.

## fmdrum harsh zones (2026-10-16/17, test image 82)
Goal: much further into harsh, industrial metal, with the same 8 knobs and meanings. Design approved by the user;
details below are as built after two sound and two UX review rounds.

### FM (K2, white): musical, then FEEDBACK, then SQUARE
| detents | zone | readout (title / value row) | sound |
|---|---|---|---|
| 0..20 | musical | `FM 4.0` / `4.0` | index 16 rad x (k / 20480)^2 (detent 10 = 4 rad, 20 = 16 rad): the old range, squeezed into 20 detents |
| 21..27 | FEEDBACK 1..7 | `FM FEEDBACK 3` / `FB3` | the modulator reads the mean of its last two outputs x beta (DX7): beta 1.0, 1.9, 2.7, 3.3, 3.9, 4.6, 6.0 (chaos from ~4.3: grit, saw, then noise); the index x (1 + beta / 4); the feedback follows the square root of the FM envelope (FMDEC) |
| 28..32 | SQUARE 1..5 | `FM SQUARE 2` / `SQ2` | the carrier clipped (gain 1 .. 32 x, level-matched to the sine), beta falls 6 -> 4.3, FEEDBACK 7's index |
A value between detents shows the next level. Sound is continuous in k (tables `fd_fmz`, 128 raw apart; feedback,
clip gain, index ramp per sample), so TE's LFO sweeps it smoothly. The index limiter counts the fed-back modulator's
harmonics (fm x (1 + beta / 4), beta <= 3). Only the harsh loop (`run_hv`) has feedback and the clip: clean keys run
the old loop bit for bit.

### NOISE (K7): METAL and GRIND after CLAP
- Zones on detents (`fd_noise_edge`): OFF 0, AIR 1-4, SNARE 5-9, HAT 10-14, CLAP 15-19, METAL 20-25, GRIND 26-32;
  the path is a 129-point table (`fd_noise`, 256 raw apart, 14 columns) read at K7 + 1023 (`FD_NOISE_SHIFT`).
- METAL: the 808 recipe. Six squares at the 808's ratios (205.3 .. 800 Hz / 205.3) from PITCH / 2 (with the sweep),
  within 70 Hz .. 1.6 kHz (100 Hz until 2026-10-17); crossfaded (equal power) from CLAP's noise over the zone's first half; band-passed
  1.2 -> 11 kHz (damping 0.6 -> 0.5); over the top 40 % the squares are first high-passed (one pole, to 3 kHz: an
  808 hat's band-pass then high-pass, without passing the edges); ring-modulated with the FM voice (0.3, x2 scale);
  the FM body comes in to 0.45 by mid-zone and falls to 0 at the top (hats). Level: `fd_metal_gain` [cluster
  octave][position] makes the band-passed squares 1.15 x as loud as white noise in the same band (they thin out
  above their fundamentals: up to ~20 dB quieter), applied after the filter in proportion to the squares' share.
- GRIND: back to white noise; the band falls to a 150 Hz high-pass (no sub); then a square comes in (RMS = white
  noise's) whose pitch rises from max(80 Hz, PITCH / 2) to PITCH x sqrt 2, unsteady (+-10 %, a new value every 8
  blocks, glided); everything ring-modulated with the voice (0.3; body tone 0.35). The noise is lifted mid-zone so GRIND's level is
  flat.
- Choke: METAL keys on 10..13 choke like HAT keys (and each other). GRIND keys never choke.
- Screen: FEEDBACK tears the head and the FM rosette and throws sparks off the rim (2 per level); SQUARE flattens
  the petals and calms the tear as the feedback falls; METAL turns the shell's lugs into six hex nuts (rattling while
  it plays); GRIND sprays angle-grinder sparks off the right rim (8 .. ~27 streaks). Classes: GRIND, CRASH (METAL with
  a long decay); METAL reads CLAP while the bursts last, TOM / SNARE under ~250 Hz, HAT when short and bright; noise
  off with FEEDBACK 5+ or SQUARE above a kick reads ZAP.

### Old kits and saved sounds
- The four old kits are written in the old scales in gen_fmdrum.py (`key()`) and remapped: FM to the K2 with the same
  Q8 index (exact), NOISE zone by zone (minus the one-detent shift). Against renders of FX4's engine (commit
  a9935e4): residual <= -51 dB, third-octave spectra within 0.08 dB (fmdrum_check `--ref`).
- **User-saved fmdrum sounds are not remapped** (nothing marks the old scale): an old FM value now gives more index
  (old half knob 4 rad -> 16 rad; old FM above ~6.25 rad lands in FEEDBACK / SQUARE), and old NOISE values land in
  other zones (old HAT -> CLAP / METAL, old CLAP -> METAL / GRIND). Reload factory kits or re-tune saves.

### Cost
The harsh path (run_hv) is Blackfin assembly since 2026-10-17: five passes over each run, each with its state in
registers (`fmdrum.c`): the FM core (feedback and carrier, two table sines; the clipped carrier into `out[]`), METAL's six
squares (four phases as two vector pairs of sign bits, two in I/M registers) into `fd_src[]`, the source (LCG noise x wx,
plus the squares and GRIND's square) into `fd_x[]`, the filter (SVF and blend with exact 32 x 16 dual-MAC products) into
`fd_src[]`, and the mix (ring mod, the DC blocker's qmul as gcc expands it, the knee off the loop's main path) into
`out[]`. The sine is one table word per step (`fd_sin2`: sine256[i] and the step to i + 1). The C loops under
`#ifndef __bfin__` are the reference: fmdrum_check builds fmdrum.c both ways and compares every case bit for bit
(including 48 random keys with knob turns and retriggers). Against the C engine before (commit 808432e): bit-identical
except 1-LSB saturation at -32768 instead of -32767 in four clamps (SQUARE's clip, METAL, GRIND, the filter: residual
<= -84 dB) and the knee and note-end fixes below; FEEDBACK's chaotic loop is exact.
- Sim, worst render call (a note's first block: two runs and the per-note setup): harsh keys 121 cycles / sample
  without METAL / GRIND (was 227), 160 with them (was 292); clean keys unchanged (the gcc loop: ~172 worst).
- 12 voices at once (all starting in the same block, the fresh blocks run two passes each): all harsh worst block
  **240 k cycles (156 per sample per voice; was 440 k)**; fm industrial's 12 heaviest keys 223 k (was 409 k); the old
  kits 220 .. 247 k (clean path, unchanged). Overhead outside the loops ~2 k per call.
- Device: total ~0.96 M cycles per block for all render calls (LOG 5); DRUMPROBE ran 2.3 x the sim on the device. At 2.3 x
  the worst block is ~0.55 M (58 %). The asm loops avoid what likely made gcc's code slow there: three 32-bit
  multiplies per sample (3 cycles each; the rest are 16-bit MACs), no stack spills, independent work between each register-to-pointer move and its use as an
  address (FSIN_A's FILL), parallel loads; their scratch (`fd_x`, `fd_src`) is in the slot block (SDRAM), 4-5 accesses
  per sample. Unmeasured on the device; next levers if needed: the clean path in the same passes (old kits ~247 -> ~180 k),
  scratch in L1.

### Review log
Round 1 (sound: industrial sound designer / DSP; UX: OP-1 player / TE designer), on the first build:
- Sound, done: kit balance (snares/hats/toms 6-9 dB under the kicks): METAL level 1.0 and its gain x1.25 (not the
  asked +6 dB: the hats' attacks then clip; measured); METAL body tone (0.45, was 0.15) tapering at the top; METAL
  top high-passed (as a pre-filter high-pass, not the SVF's high-pass output, which passed the squares' edges and
  clipped); the gain moved after the filter (the pre-filter clamp made difference tones); the 205 Hz cluster floor
  lowered to 100 Hz; FEEDBACK betas respaced (chaos from ~4.3: FB7 = 6, was 10, which also removes the crackle near
  FM 27.4), the index x (1 + beta / 8), SQUARE from beta 6; feedback follows the envelope's square root (short FMDEC
  keeps the grit); GRIND's square from max(80 Hz, PITCH / 2), jitter +-10 % per 8 blocks glided, a 150 Hz high-pass;
  METAL reaches its level by mid-zone (the NOISE 18-21 valley); kit keys as suggested (kick 0 FEEDBACK 5, rim decay,
  hats FM 1 and longer decays, ping quieter, sub kick shorter). Rejected: capping SQUARE's index at 8 rad (tried: it
  made SQUARE tamer, the opposite of the other complaint that SQUARE drops back); velocity to METAL's band (the
  OP-1's keys send 127; MIDI only, later). Test fixed: the sweeps decayed before reaching the knob's top (now
  retriggered every second).
- UX, done: each NOISE zone's first detent sounded like the zone below (AIR 1 silent, CLAP 15 without bursts, METAL 20
  a clap): the sound now runs one detent ahead; FM detent 20 shows 16.0, FEEDBACK 1-7 / SQUARE 1-5 (DX7: 0 = none);
  title "FM FEEDBACK 3" like "FM SQUARE 2"; SQUARE drawn calmer than FB7; the hex nuts (read as bubbles, collided
  with the sweep arrow) moved onto the shell as lugs; grinder spray from 8 streaks; CRASH its own colour (purple);
  value-row collisions (times "1.5S", columns moved); classes (CLAP / TOM / SNARE / ZAP rules); kit: tom ladder
  110/165/245 Hz (key 7 was a second anvil), snare 4 in SNARE, true clap, pedal chick on 11, a "slam" instead of a
  second sub on 22, fewer METAL keys. Rejected: GRIND pink vs ZAP red (no other distinct non-knob hue in TE's
  palette); "KEY n" numbering (unchanged, 1-based as before).
Round 2 (fresh reviewers, same roles), on the round-1 fixes:
- Sound, done: an engine bug at METAL -> GRIND (GRIND's first table point was a high-pass at 8.5 kHz with METAL's
  gain: detent 25 glitched to 0 dBFS): GRIND now starts at METAL's top values (band-pass, the squares' high-pass) and
  fades its own in (`fmdrum_check` now checks every zone join in the path's parameters); hats: METAL's top band 8.5 ->
  11 kHz, METAL's body tone 0 at the top, kit hats at PITCH 900 (the 441 Hz tink); GRIND's body tone 0.2 -> 0.35;
  FEEDBACK 1-3 lost bite on bright voices: the index x (1 + beta / 4) and the limiter counts beta / 4; FB1 beta 1.0;
  kit: kick 0 SQUARE 2 (FEEDBACK 5 was barely audible above 4 kHz), slam in SNARE with SWEEP 24, ping / anvil / scream
  shorter. Partly: METAL level - the hats came out 1-2 dB under fm 909's (K-weighted); raising them clips the crashes'
  attacks (METAL gain x1.15, level 1.0 as a compromise; no sample of the kit >= 32000). Rejected: CLAP make-up gain
  (the old kits' claps would change; a user decision: CLAP is ~9 dB under the snare in every kit); scaling SQUARE's
  clip down above 1 kHz (the aliasing is the deliberate part of SQUARE).
- UX, done: the top NOISE detent was dead (the one-detent shift saturated): the shift is taken back across GRIND
  (detent 31 and 32 now differ); class titles: GRIND, METAL keys go through the pitch / decay rules first (KICK under
  80 Hz, TOM / SNARE under 250 Hz), HAT up to ~1 s in METAL's upper half, CRASH only there, no CLAP carry-over at
  detent 20; sparks from FEEDBACK 1 (FB1 had no cue); SQUARE's end now brighter (beta 6 -> 4.3, was -> 3: the top
  was darker than FB7). Rejected: knob colours in the key strip's class marks (pre-existing, TE's palette has too few
  non-knob hues for 11 classes); swapping the clank off key 8 (key 8 is the shared layout's rim); CLAP make-up (as
  above). Device feel (detent size on the drum knobs, harshness on real speakers) is for the LOG 82 checklist.
Round 3 (2026-10-17, after test image 82; performance, an independent code review, a sound review by an
industrial / techno producer on renders and spectrograms):
- Performance: the harsh path in assembly ("Cost"): 12 harsh voices 440 k -> 240 k sim cycles per block, bit for bit
  the C loops; FEEDBACK's chaotic loop exact against the engine before.
- Bugs fixed (each with a check in fmdrum_check): dsp.h's soft_knee returned 32768 for |x| 40779 .. 40957 (just
  below the knee's top), which wrapped a positive sample to -32768 (a full-scale click; all engines; it happened in
  fmdrum's METAL / GRIND tests); a voice ended by cutting the DC blocker's decay (a click after short, low notes:
  -41 dBFS on a 15 ms A3 sine, up to ~-30 dBFS lower down); a note from silence kept the DC blocker's and METAL's
  high-pass state, so after a kit load mid-note (the factory's slot init silences the voices) the next note started
  with a DC step (~-25 dBFS); drumhooks_check no longer linked (CHORD v2's hooks in slot.S; stubs added).
- Code review, no change needed: overflow at the zone joins and knob extremes (TE's LFO can push any int16; every
  index is clamped, every product fits), the table indices, choke (keys and slot row), slot init, stock-range clamps of
  the kits and the 29-preset cap (drum_browser_check), the screen (no division or libc, every line clipped, buffers
  fit the longest readouts), fxpub (synth and drum FX share one live object and its mono slot: only one mode runs).
  Noted, not changed: two hats hit in the same block don't choke each other (the later can't see the pending one);
  the regular DC blocker's truncating qmul holds a negative value under ~7 LSB (inaudible). A second reviewer on the
  asm (GCC's generated code read): `"cc"` is no clobber on bfin (`"CC"` is: hv_mix writes CC; harmless as compiled,
  fixed); two M-register moves right before their DAG use (a device stall) moved earlier; render now sets age before
  marking the note seen (TE's steal scan on the other thread could read 0 for a just-started voice); gen_fmdrum.py
  asserts the ranges the 16-bit MAC operands assume.
- Sound review: fm industrial "ships": harsh, near-pink loop spectrum, no clicks, DC, clipping or join glitches;
  FEEDBACK grit -> chaos -> noise and METAL / GRIND deliver. Done (kit values): open / closed / half-open hats NOISE
  METAL 0.85 / 0.9 -> 0.95 (the FM body's 900 Hz ding was -11 dB of the open hat for its whole decay; now ~-19 dB,
  more air above 12 kHz), closed hat decay 0.08 -> 0.10 s (+1 dB); scream FMDEC 0.5 -> 2 s (it screamed ~60 ms, then
  hummed); snare 4 ratio 1.41 -> pi (its 83 Hz difference tone sat on the kick: < 160 Hz share -5 -> -25 dB); rim
  decay 0.09 -> 0.12 s; METAL's lowest cluster 100 -> 70 Hz (the review asked 50: at 50 METAL's top on a 110 Hz key
  drops ~2 dB, outside fmdrum_check's level window), so the low and mid pipe toms no longer share their partials (tom
  3 METAL 0.45 -> 0.40 for its peak). Only METAL keys under 200 Hz change; the old kits don't use METAL. Not done:
  anvil 16 / ping 23 / crash 14 levels (+3 .. 5 dB over the kicks: a device call, no level knob), FEEDBACK 2 vs 3 and
  FEEDBACK 7 vs SQUARE 1 nearly the same (respacing FB_BETA changes saved sounds' meaning; after the device test),
  SQUARE getting darker rather than brighter (by design, round 2), the square kicks' infrasonic push (judge on a sub),
  CLAP 9 dB under (rejected before), ratchet 17 without audible ratchet pulses (usable as a blat).
