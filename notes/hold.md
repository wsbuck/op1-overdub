# HOLD (sustain/latch) for the synth (firmware 246)

Status: **built (HOLD1.OP1), host-tested, not yet tested on device.** Static research 2026-10-07 (subagent; decompiles in the session
scratchpad `hold/`), key claims re-checked against the disassembly (marked Verified).

## Behaviour
- SHIFT+T2 on the synth screen toggles hold. While on, releasing a key keeps its note sounding; turning hold off
  releases every held note whose key is not still down. Synth play mode only; sequencers and MIDI unaffected.
- "HOLD" is shown on our engines' screens (TE screens show nothing yet).

## Note path (Confirmed unless marked)
- Keys: `FUN_019b6c74(key, state)` in the KeyHandler thread. Press → current controller vfn+0x14 (else global `FUN_019ac4b4`);
  SHIFT+press → vfn+0x1C (else `FUN_019ac9b8`); release → vfn+0x24 (else `FUN_019acd60`). SHIFT = key 0x26, state `0x01DD7F78`.
  Note keys map to index 0..23 (Keys.cpp:208).
- Keyboard note-on `FUN_019ac0b0`: `s_note_ons[key] = 1` (`0x01AAD488[24]`), `FUN_01a85014(&n, key, 0x7F)`, CALL at
  `0x019AC290` → `FUN_01a85750` (VoiceCtl:776).
- Keyboard note-off `FUN_019ac2f8`: clears `s_note_ons[key]`, `FUN_01a85014(&n, key, 0x40)`, **CALL at `0x019AC3A6` →
  `FUN_01a85840`** (VoiceCtl:814). **Verified.**
- `FUN_01a85014(n, key, vel)`: `n = {key + 0x35 + 12 * octave[slot] (unless drum mode), vel, flag 1, 0}`; octave at
  `0x01DFE4E0 + slot * 4`, slot `0xFF8016D4`. **Verified.**
- Generic note-on/off switch on play mode `0x01DE4C40` (0 synth, 1 drum, 2 game); synth off → `FUN_01a85058` → producer
  vfn +0x2C and ADSR release. Sequencers and MIDI-in call the same generic pair, so the hook is at the keyboard call site.
- VoiceCtl: lock byte `0x01DFE730` (note-offs while locked are dropped), active list `FixedVector<128>` at `0x01DFE734`
  (count `0x01DFF134`), 20-byte entries. Preset/engine load re-triggers the active list on the new engine; octave and
  slot changes transpose it.
- Synth controller vtable `0x01911994`, SHIFT-key slot +0x1C (pointer `0x019119B0` = `0x01A50904`): handles keys 6, 8, 9,
  0x24, 0x25 only; key 7 (T2) unused. **Verified.**
- MIDI CC64: stored in `cc[128]` at `0x01DFF138`, never read (only CC1–4 are). A pedal hook would be at the CALL `0x01A85C8A`.

## Implementation (`src/slot/hold.c`, `hold_impl.h`, wrappers in `slot.S`, `build_slot.py --hold`)
- Pointer `0x019119B0` → `_hold_shiftkey_entry`; CALL at `0x019AC3A6` → `_hold_keyup_entry` (pc-relative, checked against
  the expected `CALL FUN_01a85840`). Wrappers save RETS and both hardware loops and zero L0–L3 around the C code.
- State: `hold_on`, `hold_keys` (bit = key index). Release rebuilds the note with `FUN_01a85014(&n, key, 0x40)`, so it
  matches TE's own key-up after octave/slot changes. If VoiceCtl is locked (preset loading) nothing is released and the
  keys stay recorded until hold is toggled again.
- Host test `src/slot/test/hold_test.c` (stand-ins for TE): 8 checks pass.

## Known limits / next
- Re-pressing a held key retriggers it when built with `--chord` (the note-on hook releases it first; CHORD1). Without `--chord`, it does not.
- No indicator on TE's own screens; candidate: wrap the frame render CALL at `0x019AB8B2` (Hypothesis).
- Optional MIDI CC64 sustain via the CALL at `0x01A85C8A` (+ MIDI note-off at `0x01A8599E`).
- More than 6 held notes: TE steals the oldest voice; stolen entries are released safely later.
