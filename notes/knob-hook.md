# Knob hook: engine data in the 8 synth knob values (firmware 246)

Status: **built (SAVEPROBE.OP1, LOG.md test image 57), not yet tested on device** (2026-10-14).
The storage plan of notes/harmonic-synth.md and notes/fm-operator.md: type-3 range rows for 8 independent knobs, and
an encoder hook that writes packed values into the knob array, which TE then stores in the patch's `"knobs"`.
**Confirmed** = read in the binary (decompiles in the session scratchpad `save/`) or checked in the simulator;
**Hypothesis** = untested on the device.

## Path (Confirmed)
- TE's engine-page encoder handler (synth controller mode 0, `0x01A50610`):
  `CALL FUN_01a1dfec(knob R0, int16 *delta R1, engine id R2, [SP + 0xC] knobs = 0xFF8013C4 + slot*16)` at
  **`0x01A5063E`**, then `CALL FUN_01a00d14(patch, knobs)` at `0x01A50646`.
- `FUN_01a00d14` writes all 8 knobs into the patch JSON as `(int)(int16)` (sign-extended), then calls the patch's
  +0x34 hook. The synth patch loader `FUN_01a010f8` reads `knobs[i]` with `asInt` and stores the low 16 bits into
  `0xFF8013C4 + slot*16`: **no clamp on load**.
- **But TE validates every patch it loads** (`FUN_01a037a8`, patch.cpp:1018): for synth engines, each `knobs[k]` must
  lie within the engine's range row `min..max` (`FUN_01a1deb0`), else error 15; a factory preset or a slot that fails
  stops the OP-1 at boot (as `adsr` did for AMPPROBE2: error 21). So the row must contain every value we store, and
  packed raw values need rows of -32768..32767.
- `FUN_01a1dfec` reads the type per knob from the row: type 0 remaps knobs 4..7 to fine adjust of 0..3; **type 3**
  (dsynth) clamps knob k against its own row with no remap (`0x01A1E512` -> `FUN_01a1de68`).
- ReadInterface (`FUN_01a28210`) normalises knobs as floats `(v - min) / (max - min)`, asserting only when
  `max == min`; out-of-range results become 0.5. A -32768..32767 row is safe there.
- TE's LFO "element" module (`FUN_019c9d44`): target knob = `lfo_params[0] >> 12` (0..7) when the engine's flag
  (`0x018E142C[id]`, 1 for sampler/drum/dbox/dsynth) is 0, else `>> 11`; it modulates through **`FUN_01a1dfec`**
  itself, not through the encoder handler's call site, so the hook never sees LFO changes. Hypothesis: for a type-3
  engine an LFO on 4..7 moves those knobs independently.

## Hook as built
- build_slot.py points the CALL at `0x01A5063E` to `_knob_hook` (entry.inc `KNOB_HOOK id`): `R2 == id` goes to
  `knobhook_turn` (src/slot/knobhook.c, through `FX_STEREO_ENTRY`, which hands on the 4th argument); every other id
  `JUMP.L FUN_01a1dfec` with R0-R2 and `[SP + 0xC]` untouched. Only one engine (engines.json `"knob_hook": true`).
- `knobhook_turn`: knobs 0..5 still go through TE's `FUN_01a1dfec` (type 3); knobs 6..7 don't. Then it writes the
  packed values (sprobe.h) into knobs 6..7, and TE's `FUN_01a00d14` stores all 8.
- Runs in TE's encoder (UI) thread. Simulator: `src/slot/test/knobhook_check.py` (other engines get TE's call with
  all four arguments; register preservation; packing).
- engines.json `"ranges"`: 8 x `[min, max, scale, type]` per engine; build_slot.py checks defaults and presets
  against it.

## The probe (sprobe, test engine)
- Row: type 3 on all 8 knobs; 0..32767 for 0..5, -32768..32767 for 6..7.
- Packing (sprobe.h): `K6 = 0x8000 | edits << 3 | last knob` (always negative), `K7 = 0x8000 | (K6 * 977 + 677) &
  0x7FFF` (raw >= 0x8000). The edit count continues from the K6 the sound holds. `K7 = f(K6)` lets the screen check a
  loaded pair without RAM state (PAIR OK / BAD).
- Sound: 2-op FM sine; blue pitch, green level, white index, orange ratio, **SHIFT+blue (knob 4) an octave-up sine**
  (to hear an LFO on knob 4). Knobs 5..7 are not used for sound.
- Screen: hook calls since boot, last knob and delta, slot; what it last wrote (WROTE) and whether knobs 6, 7 still
  hold it (NOW SAME / DIFF); EDITS and PAIR from the stored values; per knob the stored value (hex, decimal) and the
  last render call's param `L` (TE's per-voice copy after the LFO; white when it differs).

## Questions SAVEPROBE answers (Hypothesis until the device result)
| question | how |
|---|---|
| does the hook run? | HOOK counts on encoder turns; K6/K7 change |
| are SHIFT knobs independent? | SHIFT+blue/green move K4/K5 alone, K0/K1 unchanged |
| do packed raw values survive save, load another, reload, power cycle? | PAIR OK and EDITS after each step; NOW SAME before a reboot |
| does TE clamp on load? | after reload: K6/K7 unchanged (PAIR OK) vs changed (PAIR BAD) |
| can TE's LFO target knobs 4..7? | LFO element on knob 4: `L` for K4 moves, the octave is heard |

## Before leaving SAVEPROBE
Synth sounds saved with `sprobe` (or in a slot) stop any image without `sprobe` at boot (patch.cpp:390, error 1):
load another engine into every slot that holds sprobe and delete saved sprobe sounds.
