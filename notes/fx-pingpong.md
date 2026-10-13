# Ping-pong delay FX (`pingpong`, firmware 246)

**Status: written and sim-checked (`src/slot/pingpong.c`, `src/slot/test/pingpong_check.py`, all pass, 2026-10-09). Not integrated (fx.json), not on device.**
FX contract and slot mechanics: fx-slot.md. Confidence: **Confirmed** = read in code, **Verified** = re-checked in disassembly at the call site, **Hypothesis** = inferred.

## Design
- Master (stereo): `(L + R) / 2` feeds the L line. L's output crosses into the R line, and R's back into L, each crossing through feedback gain `g` and the tone filter.
  - A sound at t = 0 returns on L at d, R at 2d (× g), L at 3d (× g²), and so on.
- Synth/drum (mono): one line with the same knobs (a plain feedback delay).
- Feedback, tone and mix glide linearly over each 64-sample block. The first block starts on the knob values.

| knob | role | range |
|---|---|---|
| blue | time | 20 ms × 2^(6k/32768): 20 ms … 1.28 s (882 … 56448 samples), exponential. Free time, not tempo-synced (see Q2) |
| green | feedback | g = 0 … 0.95 (Q15 31130), linear |
| white | tone of the repeats, in the loop | 0 = dark (blend to a one-pole LP at 1.2 kHz), 16384 = flat (exact), max = bright (blend to the matching HP x − LP) |
| orange | mix | crossfade dry … wet. 0 is bit-exact dry; max is full wet (32767 is mapped to 32768) |

- Proposed defaults: `[22000, 14000, 12000, 10000]` (~330 ms, g ≈ 0.4, slightly dark, ~30 % wet).
- **Time glide (tape style):** the read position approaches its target exponentially (τ ≈ 93 ms), slew-limited to 0.5 sample per sample (pitch dips/rises at most ×0.5/×1.5). Knob jumps bend pitch and never click.
- **No runaway:** both tone blends have gain ≤ 1 at every frequency, so the loop gain is ≤ g < 1.
  - A soft clip on every line write is linear up to 20480 (−4 dBFS), then follows the parabola `y = K + t − t²/32768` to a ceiling of 28672 (−1.2 dBFS).
  - The line and the wet signal can never exceed 28672.
- Fixed point: delays are Q15 samples (56448 << 15 < 2³¹); taps are linearly interpolated. There is no runtime division (only `/ BLOCK`) and no 64-bit maths.

## Memory
- State in `.data`: two `struct pp` of 32 B each (mono and stereo, kept separate because both can run at once).
- Lines are in FREE SDRAM `pingpong_ram` = `0x01E80000` (fx.json `"ram"`). Each line is 65536 int16 = 128 KB:

| line | address | samples |
|---|---|---|
| stereo L | `0x01E80000` | 65536 |
| stereo R | `0x01EA0000` | 65536 |
| mono | `0x01EC0000` | 65536 |
| (free) | `0x01EE0000`–`0x01F10000` | 192 KB unused |

- **Undefined contents at boot:** each instance counts the samples written since boot (`filled`, saturating at 65536). A tap reads anything older as 0, which costs one compare per tap. Stale SDRAM is never heard.
  - In the sim, `test/pingpong_ram.c` fills the buffer with full-scale LCG garbage from a `constructor`. Verified that it runs under `bfin-elf-run`.
  - The output is exactly 0 until the first echo, then exactly the delayed input.
- State persists across FX off/on and patch changes, because the object's init is TE's filter init, not ours. On re-enable, the delay plays on from where it stopped, like a paused tape.
  - Synth and drum share the mono line.
  - To flush on (re)construction: hook init (vtable +0x1C) in the vtable copy to reset `filled = 0`.

## Sim results (`python3 src/slot/test/pingpong_check.py`)
- All checks pass: dry bit-exact; click echoes at k·d on L, R, L, R, L with level g^(k−1) and nothing else; silence then exact delay from a garbage buffer (stereo and mono); max-feedback decay is monotone for flat, dark and bright tone and for mono; time jumps up, down and with feedback have no click; 32 corners do not saturate (peak 28672).
- **CYCLES: stereo mean 16124 per call (252/frame), mono 7534, peak 16838.**
  - The sim has no SDRAM latency. On the device each frame makes 4 line reads and 2 writes to SDRAM. Hypothesis: a few thousand extra cycles per call, which is still under the 25 k budget.
- `fx_main.c` gained `FREQ_MHZ 0` = one click (12000) at frame 0 on every channel. Sine behaviour is unchanged, and ensemble_check.py still passes.

## Q1: master-only listing (research, not implemented)
- **FX browser list** (Confirmed; key instructions Verified):
  - `FUN_01970a84` is a lazy, once-built getter. It takes no arguments and returns the vector `0x01BAB748` (guard `[0x01BAB734]`).
  - It calls `FUN_019707d8(R0=[0x01BAB724] db, R1=&vec, R2="fx_types" @0x018D3B30)`.
  - That function snprintfs `"SELECT id, type FROM %s"` (`0x018D42A8`, Verified) and runs it. There is no WHERE clause and no per-mode filtering.
- **One call site per mode** (Verified):
  - synth `0x01A509E2` (`f9 e3 51 00`);
  - drum `0x019778B8` (`ff e3 e6 c8`);
  - master `0x019D985A` (MasterController).
  - Each result goes to `FUN_01a174dc` (PresetBrowser).
- The cached list is shared by all three modes, so editing the `"fx_types"` string alone would hide the FX from master too.
- **Minimal patch plan:**
  1. In the slot `.data`, add a zeroed 16-byte vector and the string `"fx_types WHERE id NOT IN (<db ids>)"`. build_slot generates the id list from an fx.json `"master_only": true` flag.
  2. Add a getter that, if `end == begin`, calls `0x019707d8(R0=[0x01BAB724], R1=&vec, R2=&str)`, then returns `&vec`. Skip the stock atexit registration.
  3. Repoint the two 4-byte pcrel CALLs at `0x01A509E2` (synth) and `0x019778B8` (drum) to it. Master keeps the full list.
  - Ids stay `fx_types` ids, so name↔id lookups are unaffected.
- **A master-only FX in a synth/drum patch** (Confirmed): loading only uses the name table, so it resolves and builds with the mono vtable. No fallback is needed while the mono process exists.
  - Opening the browser then puts the cursor on index 0 (id not found in the list), which is harmless.
  - The mono version stays useful anyway.

## Q2: tempo (research)
- The setter `FUN_01a5e9a4(float bpm)` asserts 40–220 (`tempo.cpp:129`). It stores:

| address | contents | confidence |
|---|---|---|
| `0x018C1878` | bpm, float | Confirmed |
| **`0x01AADAD8`** | samples per beat, int32 Q14 = (int)(2646000/bpm × 16384); 120 BPM → 22050 << 14 | Verified (`FUN_019de8f0`, P5 = `0x01AADAD0`, fdiv/fmul/f→int at `0x019DE910..91E`) |
| `0x01AADAD4` | beat-phase increment (2³² per beat at 1024 samples), read by the LFO sync | Verified store; meaning Hypothesis |
| `0x01B60524` | samples per 96-PPQN tick, Q18 | Verified |

- The LFO (L1 audio callback) already reads `0x01AADAD4` through `FUN_019de4f8`, so these values are readable from audio context.
- The stock delay (Dssd) is not tempo-synced. Confirmed: no tempo loads in `0x0197D8C4`.
- **Not used in code yet.** A tempo-synced blue would read `[0x01AADAD8] >> 14` and pick divisions (1/16 … 1/1, dotted, triplet), clamped to 56448 samples.
  - At 40 BPM a beat is 1.5 s, beyond the line. Dividing by 2 or 4 fits.
  - It needs on-device confirmation first (print the word on our screen while changing BPM).
