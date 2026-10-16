# Ping-pong delay FX (`pingpong`, firmware 246)

**Status: in fx.json (id 9), on device since SVG5 (LOG 45). 2026-10-16: tempo-synced divisions on blue and the paddle
screen, sim-checked (`src/slot/test/pingpong_check.py`, all pass); not yet on device.**
FX contract and slot mechanics: fx-slot.md. Confidence: **Confirmed** = read in code, **Verified** = re-checked in disassembly at the call site, **Hypothesis** = inferred.

## Design
- Master (stereo): `(L + R) / 2` feeds the L line. L's output crosses into the R line, and R's back into L, each crossing through feedback gain `g` and the tone filter.
  - A sound at t = 0 returns on L at d, R at 2d (× g), L at 3d (× g²), and so on.
- Synth/drum (mono): one line with the same knobs (a plain feedback delay).
- Feedback, tone and mix glide linearly over each 64-sample block. The first block starts on the knob values.

| knob | role | range |
|---|---|---|
| blue | time | two zones on one knob (below): FREE 20 ms … 1.28 s exponential (detents 0 … 20), SYNCED 12 tempo divisions (detents 21 … 32) |
| green | feedback | g = 0 … 0.95 (Q15 31130), linear |
| white | tone of the repeats, in the loop | 0 = dark (blend to a one-pole LP at 1.2 kHz), 16384 = flat (exact), max = bright (blend to the matching HP x − LP) |
| orange | mix | crossfade dry … wet. 0 is bit-exact dry; max is full wet (32767 is mapped to 32768) |

- Defaults: `[28672, 14000, 12000, 10000]` (1/8. = 375 ms at 120 BPM, g ≈ 0.4, slightly dark, ~30 % wet). Until
  2026-10-16 blue was free over its whole range (default 22000, ~330 ms): sounds saved before then come back with
  another time (below 20480 shorter, above it a division).

## Blue: free and synced zones (2026-10-16; `pingpong.h` `pp_target`, shared by the DSP, ui.c and the preview)
- **Choice: one knob split in two zones, not a SHIFT layer.** FX knobs have no second layer: knobs 4–7 are always the
  fine adjust of 0–3 (`FUN_01a50690`, `delta >>= 4`, notes/fx-slot.md "Parameters", Confirmed), so SHIFT + blue stays
  the fine adjust (useful in the free zone). A SHIFT layer would need a new encoder hook for one knob. One knob also
  keeps the setting in the sound's 4 saved values, with no mode flag.
- 1 detent = 1024 raw (32 per turn). **FREE**: raw 0 … 20991, 20 ms × 2^(6 raw / 20480) (Q15 octaves = raw × 39322 >> 12),
  3.33 detents per octave, 1.28 s from 20480 (half a detent of plateau before the synced zone). **SYNCED**: one
  division per detent, `j = (raw − 20992) >> 10`, each detent at the centre of its slot (no flicker at a detent):

| detent | 21 | 22 | 23 | 24 | 25 | 26 | 27 | 28 | 29 | 30 | 31 | 32 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| division | 1/32 | 1/16T | 1/16 | 1/8T | 1/16. | 1/8 | 1/4T | 1/8. | 1/4 | 1/2T | 1/4. | 1/2 |
| beats | 1/8 | 1/6 | 1/4 | 1/3 | 3/8 | 1/2 | 2/3 | 3/4 | 1 | 4/3 | 3/2 | 2 |
| ms at 120 BPM | 62.5 | 83.3 | 125 | 167 | 188 | 250 | 333 | 375 | 500 | 667 | 750 | 1000 |

- Samples = TE's samples-per-beat word (Q14, "Tempo" below) × a × 2^−p (/ 3 for triplets), all in uint32 (a ≤ 3, so no
  overflow at 40 BPM). No divide: triplets use an exact `floor(x / 3)` by 16-bit halves and `(n × 43691) >> 17`
  (`pp_div3`, exhaustively right for n < 98304, randomly checked for all uint32). Result Q15 samples; the check
  measures every division within 0.001 sample of 60 / bpm × beats.
- **Slow tempos:** the line holds 65536 samples; the longest delay is `PP_D_MAX` = 65280 (1.48 s, Q15 < 2³¹). A
  division longer than that plays its half (quarter), which stays on the beat grid: 1/2 at 60 BPM plays 1/4, at 40
  BPM 1/8; 1/4 fits down to 40.6 BPM. Every division is 3 places above its half, so the division played is
  `j − 3 × folds`; the screen shows it and, under it, "1/2 TOO LONG". (Chosen over a plain clamp, which would put
  the echo off the grid.) More memory would not remove this: 3 lines of 2.2 s fill the whole 576 KB region.
- **Invalid tempo word** (outside 11980 … 66300 samples per beat: TE's range is 12027 … 66150, and the word is 0 until
  the tempo is first set): the last valid word, 120 BPM before any (`pp_tempo`).
- **Changes glide:** a new target (knob, tempo) goes through the existing tape glide (exponential, τ ≈ 93 ms, at most
  0.5 sample per sample): a 120 → 125 BPM nudge settles in ~0.7 s with ~4 % pitch dip, 120 → 60 BPM in ~1.8 s; no
  clicks (checks below).
- **Time glide (tape style):** the read position approaches its target exponentially (τ ≈ 93 ms), slew-limited to 0.5 sample per sample (pitch dips/rises at most ×0.5/×1.5). Knob jumps bend pitch and never click.
- **No runaway:** both tone blends have gain ≤ 1 at every frequency, so the loop gain is ≤ g < 1.
  - A soft clip on every line write is linear up to 20480 (−4 dBFS), then follows the parabola `y = K + t − t²/32768` to a ceiling of 28672 (−1.2 dBFS).
  - The line and the wet signal can never exceed 28672.
- Fixed point: delays are Q15 samples (56448 << 15 < 2³¹); taps are linearly interpolated. There is no runtime division (only `/ BLOCK`) and no 64-bit maths.

## Screen (2026-10-16: paddles; `design/fx/pingpong.svg` from `design/fx/gen_pingpong.py`, ui.c `draw_tennis`)
- The players are gone: two flat table-tennis bats, L blue and R pink, one at each table end, leaning 19 deg toward
  the net. Blade 25 x 29 (taller than wide: an ellipse on top, a flatter superellipse base), its rim showing as the
  thickness, a sheen arc; a light throat flaring into the blade; a long purplegrey handle (two grip bands) with a
  flared end; a floor shadow. Mono (synth/drum FX) keeps squash: the L bat against the brick wall. Table, net,
  barrier, scoreboard, ball, arches unchanged. (First version 2026-10-16: round blades; the user found them too
  round, so bigger, real bat shapes.)
- gen_pingpong.py is the generator again (the SVG is reproducible): it writes the file, bridges multi-piece fills
  (strictify.py), makes the recoloured fills unique (uniquify.py) and runs check_svg.py and svg_cells.py --check.
  69 drawable elements, every one one subpath of absolute M / L / C / Z; heaviest fill the barrier ~784 cells (20 %).
- Motion: the ball flies blade face to blade face once per delay (or a whole multiple >= 0.25 s), clocked by the
  line's write index (the generator places each blade so the ball at x 54 / 266 touches it); the hitter (group
  `batL` / `batR`) follows through 24 deg about its handle end (24, 109) / (296, 109) and settles, the receiver cocks back over the last 40 % of the flight; a lightblue swoosh for 0.12 s after a hit.
- Blue: the table length now follows the delay played (log2 of the DSP's glided delay: 20 ms … 1.48 s = half length
  40 … 104), so it moves with the tape glide and with tempo changes; padL / padR, legs and the wall follow the ends.
- Readout (immediate, after the SVG): top left in blue the division played ("1/8.", "1/8T", "1/4") or the free time
  ("330MS"); synced, the tempo top right in grey, 1-px lines ("120BPM", "133.3BPM", from the DSP's valid word); under a
  fold "1/2 TOO LONG" in grey. All from `pingpong_state` (dt, div, fold, spb), so it shows what plays.
- An older pingpong.svg (with players, no `padL`) left on the disk falls back to the line screen, not to missing ids.
- Previews through the C path: `src/slot/test/preview.py OUT 109 K0 K1 K2 K3` (master) / `209 ...` (mono), `PP_BPM`
  sets the preview's tempo (preview_host.c).

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

## Sim results (`python3 src/slot/test/pingpong_check.py`; driver `test/pingpong_main.c` sets `te_tempo_spb`)
- All checks pass (2026-10-16): dry bit-exact; click echoes at k·d on L, R, L, R, L with level g^(k−1) and nothing
  else; silence then exact delay from a garbage buffer; max-feedback decay monotone for every tone and mono; time
  jumps (free, into the synced zone) without clicks; 32 corners peak 28672.
- Synced: 7 divisions (1/16, 1/8T, 1/8, 1/8., 1/4, 1/4., 1/2) at 60, 90, 120, 133.3 and 200 BPM, the echo of a click
  within 0.001 sample of 60 / bpm × beats (TE's float word); stereo first echo on L at d, then R at 2d. Fold: 1/2,
  1/4., 1/2T, 1/4 at 40 BPM, 1/2 at 60 and 80, play their halves / quarters, 1/4 at 41 BPM fits. Invalid words (0, 5,
  negative, 2³¹ − 1) give 120 BPM; after a valid word, 0 or garbage keep it.
- Tempo changes (200 Hz, wet): 120 → 125 BPM, 120 → 60 BPM: the exact old delay before, no step above 0.56 × the dry
  sine's, glides 0.67 s / 1.76 s, then within 0.13 sample of the new division; 120 → 220 BPM with feedback: no
  corner (curvature 1.75 × before, a 1.5 × pitch rise allows 2.25 ×).
- Zipper: blue turned one detent per 4 blocks (fast) through the free zone, the synced zone, across the boundary and
  back down: largest step ≤ 0.76 × the dry sine's, energy above 2.5 kHz −56 … −80 dB.
- **CYCLES: stereo mean 16187 per call (253/frame), mono 7767, peak 16901** (before the sync: 16124 / 7534 / 16838;
  the per-block map costs ~60–230). The sim has no SDRAM latency (4 line reads and 2 writes per frame on the device:
  Hypothesis, a few thousand more, under the 25 k budget).

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

## Tempo (static analysis 2026-10-16; supersedes "Q2")
**Read `0x01AADAD8`: samples per beat at 44.1 kHz, int32 Q14** (120 BPM = 22050 << 14 = `0x15888000`). It is what the
metronome (and so the tape's beat) counts; pingpong.c reads it once per block (`te_tempo_spb`, build_slot.py TE_UI).

| address | contents | writer | readers | confidence |
|---|---|---|---|---|
| `0x018C1878` | bpm, float32 (tenths: the setting is divided by 10.0 at load); ELF value 120.0 | setter `FUN_01a5e9a4` (tempo.cpp:129 asserts 40 … 220) `1a5e9e8: [P5 + 0x4] = R7` (P5 = `0x018C1874`) | getter `0x1a5e7a0`: TapeController / TapeScreen, TempoScreen, midi_interface | Verified |
| **`0x01AADAD8`** | samples per beat, Q14: `(int)(2646000.0f / bpm × 16384.0f)` | `FUN_019de8f0` (metronome.cpp): `R0 = 0x4a217fc0` (2646000.0 = 60 × 44100), fdiv `0x1aa54c8`, × 16384.0 (`0x4680`), f→int `0x1aa4dbc`, then one aligned store `19de92c: [P5 + 0x8] = R6` (P5 = `0x01AADAD0`) | the metronome in the L1 audio callback (`ffa01020: CALL 0x019de548`): `19de646: R2 = [P1 + 0x1c]` reloads the click countdown; TapeController (`1a56c00`, `1a58e1e`) | Verified |
| `0x01AADADC` | metronome countdown to the next click, Q14 samples | the callback, −64 samples (`-1 << 20`) per 64-frame block; rescaled on a tempo change | metronome; beat phase `0x19de8b0` (TempoScreen) | Verified |
| `0x01AADAD4` | beat-phase increment `(int)(bpm × 1662149.125f)` (2⁴²/2646000) | tempo.cpp `1a5e840..850` | the LFOs' sync (`0x19de4f8`) | Verified store; units Hypothesis |
| `0x01B60524` | samples per 96-PPQN tick, Q18 | tempo.cpp `1a5e838` (+4 copies) | PlayerManager (sequencer clock, MIDI ext clock) | Verified |

- **Units: 44.1 kHz callback samples, not the engines' 88.2 kHz:** the metronome takes exactly 64 samples per callback
  off a countdown it reloads from this word. Verified.
- **Atomic for the audio thread:** a single 32-bit store to an aligned word (Blackfin aligned stores; Confirmed, not
  tested on hardware). The countdown next to it is rescaled after the store; harmless for us.
- **0 at boot:** the word is in zeroed data and re-zeroed by the static init (`0x19de9bc`); it is set when the tempo
  singleton (`0x1a5ed44`) loads the settings and calls the setter. Whether that precedes the first audio callback:
  Hypothesis. Hence the fallback.
- **Range:** 197054832 (220 BPM, 12027 samples) … 1083801600 (40 BPM, 66150 samples). After a failed assert the
  setter still stores (`1a5ea64: JUMP.S 0x1a5e9e0`, if the assert handler returns: Hypothesis), so we range-check.
- **Writers:** only tempo.cpp's recompute paths (setter, `1a5e7e4`, sync poll `1a5e860`, mode setter `1a5ea68`), all from
  the checked float; no data pointers to these words in the image (Verified, scan). Tap tempo and external clock go
  through the clamped setter (Hypothesis for ext clock). **Tape speed does not change it**: in tape-follow mode the
  metronome advances its countdown by the tape's advance instead (`0x01AADAD0` flag, `[0xFF802970] >> 21`; Hypothesis
  for that word), so a varispeed tape drifts from our synced echoes. Hypothesis.
- The stock delay (Dssd) is not tempo-synced: no tempo loads in `0x0197D8C4`. Confirmed.
- **On device, to confirm:** the screen prints the tempo it reads ("120BPM" top right when synced); change the BPM
  and check it follows, including tenths.
