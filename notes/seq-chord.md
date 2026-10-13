# CHORD: one key plays a learned chord shape (firmware 246)

**Status: Option A built (CHORD1.OP1: `build_slot.py --hold --chord`, code in `src/slot/hold_impl.h`), host-tested, not yet tested on device.** Static analysis 2026-10-09 (subagent; decompiles in the session scratchpad `seq/`).
Confidence: **Confirmed** = read in code, **Verified** = re-checked in disassembly at the call site, **Hypothesis** = inferred.

Goal: define a chord shape by playing it, then each single key plays that shape transposed to the key.
Two ways in: a new sequencer type (id 6) or a keyboard-path hook like HOLD. **Recommendation: the hook first (CHORD1);
a `chord` sequencer type only if TE-menu integration or MIDI-in chords are wanted later.**

## Sequencer ids (SeqIds.cpp) — Confirmed
| id | name | player getter (singleton) | player vtable | controller ctor (size) | ctl vtable (obj+4) | screen id | svg |
|---|---|---|---|---|---|---|---|
| 0 | pattern | `FUN_01a08840` | `0x019110C4` | `FUN_01a07514` (0x88) | `0x0191101C` | 0x2B | pattern.svg |
| 1 | endless | `FUN_01987cd8` (synth/drum instances) | `0x0190FD84` | `FUN_0198673c` (200) | `0x0190FCDC` | 0x27 | endless.svg |
| 2 | tombola | `FUN_01a65ae0` | `0x01911E04` | `FUN_01a63f44` (static) | `0x01911CE4` | 0x25 | tombola.svg |
| 3 | finger | `FUN_019ed460` | `0x01910E9C` | `FUN_019ec330` (0x48) | `0x01910DF4` | 0x2D | ok.svg (src `ok_player.cpp`) |
| 4 | sketch | `FUN_0198c754` | `0x0190FF0C` | `FUN_0198ab94` (0x48) | `0x0190FE64` | 0x2E | sketch.svg |
| 5 | arpeggio | `FUN_01a46400` (one instance, obj `0x01DF5568`) | `0x019117BC` | `FUN_01a457bc` (0xE8) | `0x01911714` | 0x2C | simple.svg |
- Name table: static `{char *name, int id}` × 6 at `0x018FA634` (strings at `0x018FA600`; the SeqIds.cpp assert strings
  follow at `0x018FA664`, so it cannot grow in place). Order in the table is pattern, tombola, endless, … — ids are the
  second word, not the position.
  - `FUN_01a422ec` id → name: base `0x018FA638` (.L/.H at `0x01A422EC/F0`), `P0 = 6` loop at `0x01A422F4`. Asserts `:33`. **Verified.**
  - `FUN_01a42344` name → id: base `0x018FA634` at `0x01A42358/5C`, `R0 = 6; CC = R6 < R0` at `0x01A42360`, result base again
    at `0x01A423AE/B2`. Unknown name asserts `:41` (fatal on device: boot stops on the assert screen, see LOG). **Verified.**
- DB: factory `seq_types (id, type)` = 1 pattern … 6 arpeggio, no params column. db2 loads the whole table as a type
  list (`FUN_019707d8(…, "seq_types")`, `FUN_01970ba0` firmware id → DB id), like synth/fx types. The browser's choice
  reaches `SeqManager::select(dbid)` (`FUN_01a429d0`, vtable `0x0191153C` +0x4C): `SELECT type FROM seq_types WHERE id=`
  → name → id → `PlayerManager::setType` (`FUN_01a0d050`). Confirmed.
- **Persistence:** the chosen name is written to the *user* DB, `UPDATE sequencer SET value='<name>' WHERE key='synth_sequencer'`
  (or `drum_sequencer`), and read back at boot by `FUN_01a42ba0`. A saved unknown name (e.g. `chord` on stock firmware)
  hits the `:41` assert at boot. Confirmed.

## Runtime — Confirmed unless marked
- `SeqManager` singleton `0x01DF500C` (getter `FUN_01a42e38`, vtable `0x0191153C`). Enabled flags: `+8` drum
  (`0x01DF5014`), `+9` synth (`0x01DF5015`); `FUN_01a423f8(mgr)` returns the one for the current play mode.
  Controllers `ctl[6]` at `0x01DF5018` (index = seq id), heap-allocated in `FUN_01a42e38`. `0x01DF5030` is another object
  (written at `0x01A43114`).
- `PlayerManager`: current players `s_active_drum_player 0x01AB3138`, `s_active_synth_player 0x01AB313C`
  (`FUN_01a0d030` returns the one for play mode `0x01DE4C40`); types `0x01AB3144` synth / `0x01AB3148` drum
  (`FUN_01a0cf94`); running flags `0x01AB3140` drum / `0x01AB3141` synth (`FUN_01a0cf2c`, which calls player +0x3C on stop).
- `setType` `FUN_01a0d050(type)`: old player +0x3C, then `CC = R5 < 6 (IU)` at `0x01A0D090`, `JUMP` via table
  `0x018F2EF8` (6 words; assert string at `0x018F2F10`). Each case is `CALL getter` followed by an identical tail that
  stores R0 to the player slot and R5 to the type (R6/R7/P4/P5 are set before the jump). **Verified.**
- Player vtable (8-byte `{adj, fn}` entries, like engines):
  | slot | role | arp |
  |---|---|---|
  | +0x0C | dtor | `0x01A46476` |
  | +0x14 | `keyDown(this, key 0..23, shifted)` → nonzero = consumed | `0x01A46310` (adds key, returns 1) |
  | +0x1C | `keyUp(this, key)` | `0x01A46370` |
  | +0x24 | reset (play-mode switch `FUN_019ac3c8`) | `0x01A462C8` |
  | +0x2C | `tick(this, step 0..47)` | `0x01A47024` |
  | +0x34 | unknown (arp: empty) | `0x01A4626E` |
  | +0x3C | stop / all notes off | `0x01A4669C` |
- Keys → player (`FUN_019ac0b0(key, shifted)`; R1 = 0 from the normal press at `0x019AC548`, 1 from the global SHIFT
  handler's note-key default at `0x019ACA10`): if the sequencer is enabled, `player->keyDown`; if consumed, the key is
  pushed on a "sent to sequencer" vector (`0x01AAD47C/80/84`) and its release calls `player->keyUp` instead of note-off.
  Otherwise the normal note-on (`0x019AC290`). **Verified.**
- MIDI-in note-on/off (`FUN_01a859e4`/`FUN_01a858d6`) send notes 0x35..0x4C to `player->keyDown(key, 0)`/`keyUp` the same
  way (endless takes all notes); other notes play directly.
- Notes out: players build notes with `FUN_01a85014(&n, key + offset, vel)` and call `FUN_01a85750(&n, 0)` /
  `FUN_01a85840(&n)`, the same pair as the keyboard. The note's flag byte (+8, set to 1 by `FUN_01a85014`) also sends
  **MIDI out** (`FUN_019e2e64`, `midi_interface.cpp`; it keeps a per-note on flag, so duplicate on/offs are not resent).
  `FUN_01a85750` **asserts on notes outside 0..127**: arp checks `< 0x80` before calling.
- Threads: `keyDown/keyUp` run in the KeyHandler thread; `tick` runs in a sequencer thread (`FUN_01a4318c`: pend on
  semaphore `0x01DF50B8`, then `FUN_01a0d2c0`). Clock: internal accumulator `0x01AB3170` vs `FUN_01a5e77c()`, or
  MIDI clock (`PlayerManager.cpp:366`); step counter `0x018C1884` wraps at 48 (PPQN unknown; Hypothesis 48 per beat).
- Controller interface (obj+4 vtable): +0x14 key → consumed, +0x1C, +0x24, +0x2C knob (index, delta*), +0x34,
  +0x3C, **+0x44 `screenId()`** (returns the constants above), **+0x4C `setEnabled(bool)`** (+0x44/+0x4C are pure virtual
  in the base vtable `0x0191149C`; the others are no-ops there). SeqManager forwards to `ctl[type]` and asserts
  `SeqManager.cpp:216/222 ctl` on null.
- Screens: no ScreenId table for sequencers; the controller returns its id. Arp's page: object `0x01DF5D48`
  (`FUN_01a3bab0(obj, 0x2C, "simple.svg")`), page vtable `0x019117FC`, **update +0x1C at `0x01911818` = `0x01A474B8`**.
  Same shape as iter's page, so the SCREEN1 hook technique applies.
- Special cases by type: `== 4` (sketch) in tick, slot change and `FUN_01a50ffc`/`FUN_01a5102c`; `== 1` (endless) in
  MIDI-in routing. A new id takes the generic path.

## Option B: new sequencer type `chord` (id 6) — what it needs
| what | stock | patch |
|---|---|---|
| name table | `0x018FA634`, 6 entries | 7-entry copy in our block (`{"chord", 6}`); repoint the 3 bases in `FUN_01a422ec`/`FUN_01a42344`; counts 6 → 7 (`P0 = 6` `0x01A422F4`, `R0 = 6` `0x01A42360`; both 16-bit imm7) |
| player switch | bound `R5 < 6` at `0x01A0D090` (imm3, 7 fits), table `0x018F2EF8` (P0.L/H `0x01A0D096/9A`) | 7-entry table; case 6 = stub `CALL _chord_player_get; JUMP.L 0x01A0D0C4` |
| controller array | `0x01DF5018` × 6 | 7-entry array in our block; repoint the 10 base loads (xrefs at `0x01A424DA`, `…255A`, `…25DA`, `…2632`, `…267A`, `…26DE`, `…2916`, `…2A4E`, `…2C14`, `…2EA0`); entry 6 = our static ctl object (vptr at +4: copy of base vtable `0x0191149C` with +0x44 → `return 0x2C`, +0x4C → no-op) |
| DB | — | factory `seq_types` row `(7, 'chord')` |
| screen | — | ctl returns 0x2C (arp's page); pointer `0x01911818` → `_seq_ui_update`: type 6 → our draw, else `JUMP.L 0x01A474B8`. simple.svg's own nodes may still show (Hypothesis) |
- No data words point at the three tables (scan of the decrypted image); code references only.
- Player: static object, vptr → our 0x40-byte vtable: keyDown plays the chord and returns 1 in synth mode (returns 0 in
  drum mode so drums play normally), keyUp releases, +0x24/+0x3C release everything, tick/+0x34 no-ops.
- Gains over option A: TE's sequencer menu and on/off key, MIDI-in chords for notes 0x35..0x4C, knobs (+0x2C) for
  shape edits, its own screen slot.
- Risks: 4 structures and a C++-shaped object pair (more to get wrong than HOLD); **saved `chord` in the user DB asserts
  at boot on stock firmware** (switch to another sequencer before reverting, or keep the name unknown to the DB);
  chords then only work with the sequencer on, and cannot combine with arp.

## Option A: keyboard hook (HOLD-style) — recommended for CHORD1
- Hooks (all in the KeyHandler thread, synth play mode only; drum/game pass through):
  - note-on CALL `0x019AC290` (`FUN_01a85750(&n, 0)`, R0 = `&n`) → `chord_keydown(&n)`. **Verified.**
  - note-off CALL `0x019AC3A6` (already hooked by HOLD) → hold decides, then `chord_keyup(&n)`. **Verified.**
  - The key index is recovered from `n`: `key = n.note − 0x35 − 12 × octave[slot]` (inverse of `FUN_01a85014`;
    octave `0x01DFE4E0 + slot*4`, slot `0xFF8016D4`).
- Learn gesture (no new button): **hold SHIFT and play the chord.** SHIFT+note already reaches the same note-on with
  `shifted = 1` (global SHIFT handler default → `FUN_019ac0b0(key, 1)`, **Verified**; the synth controller's SHIFT slot
  only takes keys 6, 8, 9, 0x24, 0x25). SHIFT state byte `0x01DD7F78` (from hold.md).
  - First SHIFT+note of a new learn: shape cleared, root = that key. Each further SHIFT+note adds `key − root`
    (signed, so notes below the root work). Learn notes sound normally.
  - First plain note after learning ends the learn. Shape of ≥ 2 notes → chord mode on; a single SHIFT+note → shape of
    one = chord mode off. Cap at 4 notes (6 voices).
- Play: note-on for key k sends `FUN_01a85014(&m, k + iv, 0x7F)` + `FUN_01a85750(&m, 0)` for each interval; key-up the
  same with `FUN_01a85840`. Skip notes outside 0..127 (TE asserts).
- Bookkeeping: per held key, the shape used at its note-on (shape can change while keys are held); refcount per
  *relative* index `k + iv` (octave-independent, so octave changes, which transpose active voices, still match TE's own
  rebuild-at-key-up). Note-on only on 0 → 1, note-off only on 1 → 0, so overlapping chords don't cut shared notes.
- HOLD interplay: hold sits above chord (hold keeps keys, its release calls `chord_keyup`).
- Gaps: sequencers consume keys before the hook (with a sequencer on, chord mode does nothing); MIDI-in notes are not
  chorded; MIDI out does get the chord notes. Play-mode switch clears `s_note_ons` without note-offs (`FUN_019ac3c8`):
  reset our state when play mode ≠ 0. While VoiceCtl is locked (`0x01DFE730`, preset load) note-offs are dropped as
  for stock single notes.
- Display: "CHORD" plus the shape (dots on a 2-octave mini keyboard, root highlighted) on our engines' screens; TE screens
  show nothing (same open item as HOLD: frame render CALL `0x019AB8B2`, Hypothesis).

## First test image (CHORD1)
- `build_slot.py --hold --chord`: the two CALL hooks above, `chord.c` + host test (stand-ins as in `hold_test.c`:
  learn, play, overlap refcount, octave change between down/up, out-of-range notes, hold + chord).
- Device checks: SHIFT+C, SHIFT+E, SHIFT+G, then single keys play major triads; SHIFT+C then SHIFT+E' (10th) gives the
  root + 10th; SHIFT + one key turns it off; octave shift while holding a chord releases cleanly; MIDI out shows 3 notes;
  arp on → single notes reach the arp unchanged.

## CHORD1 as built (2026-10-09)
- `chord_keydown` replaces the note-on CALL `0x019AC290`; releases go through HOLD's key-up hook (`--chord` needs `--hold`). SHIFT state: byte `0x01DD7F78` (set to 1 by the global press handler at `0x019AC856`, cleared at `0x019ACF94`; Verified).
- Learn: SHIFT + notes (first = root, intervals by note number, so an octave change while learning is fine; clamped ±35; duplicates ignored; max 4). First plain note ends learning; 1 note learned = chord off.
- Per key: the shape it was played with; per relative index (key + interval): a reference count. Out-of-range notes skipped.
- A key pressed again while HOLD keeps it (or with a stale chord) is released first, so it retriggers. This also fixes HOLD's "re-pressed held keys don't retrigger" for plain notes.
- A play-mode change (seen at the next key event) resets chord and hold bookkeeping: `FUN_019ac3c8` releases sequencer keys, resets the player, clears `s_note_ons` and calls `FUN_01a862d4`/`FUN_01a8624c` (Hypothesis: all voices off).
- Screen: "CHORD" (red; orange "LEARN" while learning) under HOLD's badge, and the shape on a semitone ruler (root white).
- Host test `src/slot/test/hold_test.c`: 23 checks pass (learn, F/A overlap refcount, octave change, root + 10th, > 127 skipped, hold + chord, retrigger, off gesture, 4-note cap, play-mode reset).
