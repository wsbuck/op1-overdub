# Crush FX (`crush`, firmware id 11, DB `fx_types` 14; source `src/slot/crush.c`, firmware 246)

**Status: sim-checked (`src/slot/test/crush_check.py`: all pass; `fx_abi_check.py` 12/12 with crush), firmware builds
and links. Not on device (test image CRUSH1, notes/LOG.md).** Screen: a placeholder in ui.c; the real one (an Egyptian
pyramid that gets dilapidated, `design/fx/crush.svg` + `gen_crush.py`) is being designed separately.
FX contract and slot mechanics: fx-slot.md. Confidence: **Measured** = simulator (bfin-elf-run), **Hypothesis** = not
checked.

## Fit (step 0)
- FX ids: ensemble 8, pingpong 9, distortion 10, **crush 11**. Every FX table (names, mono/stereo factories,
  ScreenId, knob ranges) is generated into the slot block by build_slot.py, so nothing grows in TE's image. Bounds
  patched: name loops 14 (8 + 4 + aliases `ens`, `drive`; the `Rx = imm7` limit is 63), factory/ScreenId ids 12.
- FREE region: the slot block (`0x01E20000`) is 130 KB with crush (+6.3 KB: text 4.0 KB, data 2.3 KB), ending
  `0x01E3FCAC`; the next reservation is pingpong's RAM at `0x01E80000` (254 KB left). No FREE SDRAM buffer needed.
- Presets: FX have no preset rows; the DB gains one `fx_types` row. Synth presets stay 176/176, drum 28 (unchanged).
- fx.json has no `test` flag on any FX (all are built by default), so crush has none either.
- Hypothesis: the FX browser lists 12 rows now (stock 8 + filter + ours); 11 showed fine on device before.

## Chain and knobs
Per channel, 44.1 kHz, 64-frame blocks (FX run at 44.1 kHz; engines at 88.2 kHz / 128): compressor -> sample-and-hold
-> quantiser (+ bit flips) -> reconstruction low-pass -> DC blocker -> level compensation. Stereo (master FX) shares
the compressor's detector (linked) and the hold clock (incl. jitter and flips), so L = R in gives L = R out.
Every knob turned fully anticlockwise is clean: the block is the input, bit exact (Measured), and costs ~600 cycles.
Every knob adds damage clockwise (RATE and BITS too: their top is the clean end).

| knob | role | range (k = knob / 32768) |
|---|---|---|
| blue RATE | sample-and-hold rate, logarithmic, no anti-alias filter | 44.1 kHz x 2^(-6.785 k): 44.1 kHz, 13.6 kHz at 1/4, 4.2 kHz at 1/2, 1.3 kHz at 3/4, 400 Hz |
| green BITS | quantiser step 2^(16 - b) (continuous b, not integer bits) | b = 16 - 18 k up to the middle (16 -> 7), 7 - 12 (k - 1/2) above (7 -> 1): 11.5 bits at 1/4, 4 at 3/4. mu-law amount m: 0 below 1/4, 1 at 1/2, 0 above 3/4 (K = 2^(8 m), 1 .. 256). Mid-rise offset below 3 bits (k > 0.83) |
| white SQUASH | compressor before the crusher | threshold -6 - 30 s dBFS, slope 0.95 s (2 - s): ratio 1.7 / 3.5 / 9 / 20 : 1 at 1/4 / 1/2 / 3/4 / max; make-up 5 s x slope x 6 dB (10.7 dB at 1/2, 28.6 dB at max: a -6 dBFS peak keeps its level); release 150 ms x 2^(-2 s) (150 -> 37 ms); attack instant; 6 dB soft knee |
| orange GRIT | 0 .. 1/2: hold-clock jitter; 1/2 .. 1: + XOR bit flips | J = min(1, 2 k): period x (1 +- 0.75 J), a random walk ramping to a new random target every 2^(14 - 14 J) samples (2.7 Hz wobble -> every sample: sizzle). F = max(0, 2 k - 1): flips on 0.25 F of the holds, one bit among the lowest 1 + (nb - 1) F of the code's nb magnitude bits |

- **RATE**: the clock is a time counter in Q16 samples (t -= 1 per sample; a hold when t <= 0, then t += period), so
  the rate sweeps without steps. Each hold samples the input at its exact instant (linear interpolation between the
  two samples) and the step lands between output samples: that sample is the box-filtered mix of the old and new
  held value. At RATE 0 (period exactly 1, no jitter) every sample is its own hold (cheaper path, back on the grid).
  Tones above half the rate fold back (Measured: hold at 8 kHz, 5 kHz in -> 3 kHz alias +7.1 dB over the 5 kHz
  line; 6 kHz in -> 2 kHz: the alias moves against the pitch).
- **Reconstruction**: one-pole low-pass at half the hold rate (a = 1 - exp(-pi / period)), faded to straight through
  between 22 and 44.1 kHz (exactly through at RATE 0).
- **BITS**: sign-magnitude mid-tread codes n = floor(|x| / step + 1/2) (no dither: quiet tails gate to zero through
  stair steps), level n x step. Below 3 bits the levels move to mid-rise ((n + o) step, o -> 1/2 at 1 bit: 1 bit is a
  square at +-half scale) with a dead zone of o x step / 4 (1/8 of full scale at 1 bit), so silence stays silent.
  Measured steps x level compensation: 12 bits 16, 4 bits 3976 (17 levels), 3.5 bits 5623, 3 bits 8294, 1 bit +-9320.
- **mu-law** (the middle of BITS): G.711's segment form with a variable mu: z = plog2(1 + K u) / plog2(1 + K) where
  plog2 is the piecewise-linear log2 (exponent + linear mantissa, i.e. G.711's 8 segments); z is quantised with the
  same steps (mid-tread) and expanded through the exact inverse. K = 1 is the linear quantiser, K = 256 at 8 bits is
  G.711 mu-law (16 steps per segment). At the centre (7-bit codes, K = 256): quietest step 15-16 LSB (~12-bit
  resolution for quiet signals), largest 2062 at full scale (~5 bits) (Measured): the SP-1200 / early-sampler grain on
  loud parts while quiet ones stay smooth. One staircase whose spacing morphs, not a crossfade of two.
- **SQUASH**: peak detector per 8-sample sub-block over all channels (instant attack, one-pole release on the peak in
  Q26), gain computed in log2 (Q12) and ramped per sample. Quiet parts get up to the full make-up, so tails sit in
  the low bits and crackle instead of gating. Measured: -6 dBFS burst at max: 28.5 dB of gain reduction, output
  +0.01 dB; after a 48 dB drop the reduction halves in 67 ms and is gone (< 0.5 dB) in 138 ms; the -54 dBFS tail
  comes out +25.8 dB (max) / +8.9 dB (half).
- **GRIT**: jitter is applied to each hold's next period, clamped to at least one sample (so at RATE 0 only the
  slowing half acts). Flips XOR a random bit of a nonzero code (zero stays zero: silence stays silent; sign-magnitude:
  no DC). Measured over 300 blocks at full rate: 0 flips up to half, ~10 % of holds at 3/4, ~21 % at max.
- **Level**: output gain = comp_bits(BITS) x comp_rate(RATE) (17-point tables, Q12), the inverse of the simulator's
  mean level change on sines (1 kHz -6 / -12 dBFS, 440 Hz) and saws (220 / 110 Hz), smoothed; RATE's last point
  capped at +3 dB. Ramped per sample (Q20, lands on the target exactly). Measured over BITS x RATE (0, 1/4, 1/2, 3/4,
  max each; 440 Hz sine and 220 Hz saw at -6 dBFS): -3.1 .. +2.7 dB.
- **DC blocker** 3.4 Hz (one-pole 2^-11; not in the clean path): 1 bit and the asymmetric pulse at 1 bit leave a few
  LSB. Its state tilts staircases by at most amplitude / 2048 LSB per sample (inaudible; the checks allow for it).
- **Knob smoothing**: two cascaded one-poles per block (1/4 each; a 1024 detent is half way after ~6 blocks, 8 ms;
  exact arrival); every parameter derives from the smoothed values; gains ramp per sample. Measured: turning each
  knob 16 detents (one every 2 blocks) or jumping all at once makes no step larger than the steady states passed
  through (no zipper).

## Screen
- The screen reads **`crush_state[mode == 3]`** (`struct crush`, `src/slot/crush.h`): `[0]` the synth/drum (mono)
  instance, `[1]` the master (stereo) one; updated once per 64-frame block:
  - `gr`: compressor gain reduction, smoothed (half way per block), 0 .. 32767 = 0 .. 30 dB (the pyramid's pump);
  - `k[4]`: the knobs after the DSP's smoothing (RATE, BITS, SQUASH, GRIT), 0 .. 32767;
  - `rate_hz` (44100 .. 400), `bits_q12` (16.0 .. 1.0 in Q12), `mu` (mu-law share, Q15): ready-made readouts;
  - `flips`: bit flips so far (wraps): a counter to trigger cracks or falling stones.
  The raw knobs are `FX_KNOBS(mode, slot)` in ui.c as for the other FX.
- ui.c: `extern struct crush crush_state[2] __attribute__((weak))`, kind `FX_CRUSH = 4` (= `FX_UI_KINDS["crush"]` in
  build_slot.py), drawn by `draw_crush()` (placeholder: four knob bars, a gain-reduction meter, rate / bits text).
  Host preview: `src/slot/test/preview.py OUT 111 k0 k1 k2 k3` (preview_host.c fakes a mid-pump state).
- For the SVG route (as pedals/choir): add `"crush": "crush.svg"` to `FX_SVGS` in build.py and a `crush_ready()` /
  `draw_pyramid()` pair in ui.c that falls back to `draw_crush()`.

## Implementation
- Fixed point: signals Q15 in int32, compressor envelope Q26, gains Q15 / Q13 / Q20, clock Q16. 16 x 16 MACs where
  operands fit, `mul16` (32 x 16, two MACs) elsewhere, SIGNBITS for the mu-law segments. No division at run time (the
  per-block 1 / plog2(1 + K) is a 15-step bitwise divide), no 64-bit, no tables in the per-sample path, no libgcc.
- Per block: knob smoothing and parameters; the compressor (8 sub-blocks); the clock (holds: sample index, distance
  back, flip mask; cost per hold, not per sample); then per channel simple array passes (GCC 4.3 keeps these in
  hardware loops; fused per-sample loops spilled and were 30 % slower): compressor gain, the input at each hold
  (gather), the quantiser (one loop per kind: linear, mid-rise, mu-law), held values and edges, then the low-pass /
  DC blocker / output gain.
- Memory: `crush_state[2]` 2 x 128 B, scratch `sc` 2.0 KB shared by mono and stereo (written before read in every
  call; fx_abi_check's interleave test passes). No FREE SDRAM.

## CPU (simulator, CYCLES per stereo call of 64 frames, Measured)
| case (RATE, BITS, SQUASH, GRIT) | cycles |
|---|---|
| clean | ~600 (mono and stereo) |
| fx.json default (10000, 12000, 12000, 3000) | see crush_check.py output: ~12 k |
| typical: warm / lofi / SP-1200-like (26 kHz, 12 bits) / full-rate mu-law / full-rate linear | ~10 k / 14 k / 16 k / 13 k / 9 k |
| worst: near-full rate (41 kHz: ~60 holds a block, each interpolated) + mu-law + SQUASH max + GRIT | 26.7 k mean, 27.8 k peak |
- Distortion for comparison: ~29 k typical, 44.5 k worst; ensemble 21 k. The cost scales with holds per block, so
  RATE near the top with mu-law is the expensive corner.

## Open
- On device: the sound (especially SQUASH's pump and how far GRIT goes), the default, cycles (simulator vs BF524),
  the FX browser with 12 rows.
- The RATE level table is fitted to a few test signals; at 400 Hz a sine near a multiple of the rate can come out
  quieter or louder (aliasing onto DC / Nyquist).
