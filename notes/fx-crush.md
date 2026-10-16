# Crush FX (`crush`, firmware id 11, DB `fx_types` 14; source `src/slot/crush.c`, firmware 246)

**Status: sim-checked (`src/slot/test/crush_check.py`: all pass; `fx_abi_check.py` 12/12 with crush), firmware builds
and links. Not on device (test images CRUSH1, CRUSH2 with the 3D pyramid screen; notes/LOG.md).**
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
| blue RATE | sample-and-hold rate, no anti-alias filter | 5 detents per octave from 44.1 kHz (1 detent = 1024 raw): 22.05 k at detent 5, 11.025 k at 10, 5.5 k at 15, 2.76 k at 20, 1.38 k at 25; the last 7 detents stretched to 400 Hz; a +-200 raw plateau on 26.04 kHz (SP-1200) at detent 4 |
| green BITS | quantiser step 2^(16 - b) (continuous b, not integer bits) | 16 -> 12 bits at detents 0..4 (one per detent), 12 -> 8 at 4..12 (10 at 8), 8-bit codes with mu-law morphing in at 12..16 (full G.711-style mu-law at 16, the centre), 8 -> 6 bits as it morphs out at 16..20, 6 -> 1 at 20..32 (4 on detent 25). +-200 raw plateaus on every whole bit and on full mu-law. Mid-rise offset below 3 bits |
| white SQUASH | compressor before the crusher | threshold -6 - 30 s dBFS, slope 0.95 s (2 - s): ratio 1.7 / 3.5 / 9 / 20 : 1 at 1/4 / 1/2 / 3/4 / max; make-up 5 s x slope x 6 dB (10.7 dB at 1/2, 28.6 dB at max: a -6 dBFS peak keeps its level); release 150 ms x 2^(-2 s) (150 -> 37 ms); attack instant; 6 dB soft knee |
| orange GRIT | 0 .. 1/2: hold-clock jitter; 1/2 .. 1: + XOR bit flips | J = min(1, 2 k): period x (1 +- 0.75 J), at least half a sample (so it acts both ways, also at RATE 0), a random walk ramping to a new random target every 2^(14 - 14 J) samples (2.7 Hz wobble -> every sample: sizzle). F = max(0, 2 k - 1): flips on 0.25 F of the holds, one bit counted down from the code's top bit, the span F x its bits, weighted to the top (u^2) |

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
  square at +-half scale) with a dead zone of (1/2 - 3 o / 4) step: 1/8 of full scale from 3 bits down to 1 (a gated
  quiet tail never comes back). Measured steps x level compensation: 12 bits 16, 4 bits 4072, 3.5 bits 5704, 3 bits
  7722, 1 bit +-9324.
- **mu-law** (the middle of BITS): G.711's segment form with a variable mu: z = plog2(1 + K u) / plog2(1 + K) where
  plog2 is the piecewise-linear log2 (exponent + linear mantissa, i.e. G.711's 8 segments); z is quantised with the
  same steps (mid-tread) and expanded through the exact inverse. K = 1 is the linear quantiser, K = 256 at 8 bits is
  G.711 mu-law (16 steps per segment). Below tmu = (2^e - 1) / K, e = ceil(log2(K / Z)) (where mu-law's steps would
  be finer than the linear step) the linear quantiser stays, so the morph only coarsens: at the centre (detent 16,
  8-bit codes, K = 256) the quietest step is the linear 8-bit 255 LSB and the top segment's 1060 (Measured). One
  staircase whose spacing morphs, not a crossfade of two.
- **SQUASH**: peak detector per 8-sample sub-block over all channels (instant attack, one-pole release on the peak in
  Q26), gain computed in log2 (Q12) and ramped per sample. Quiet parts get up to the full make-up, so tails sit in
  the low bits and crackle instead of gating. Measured: -6 dBFS burst at max: 28.5 dB of gain reduction, output
  +0.01 dB; after a 48 dB drop the reduction halves in 67 ms and is gone (< 0.5 dB) in 138 ms; the -54 dBFS tail
  comes out +25.8 dB (max) / +8.9 dB (half).
- **GRIT**: jitter is applied to each hold's next period, clamped to at least half a sample: holds can come up to
  twice as fast (two in one sample: the later wins) or slower, so at RATE 0 the hold instants wander both ways
  (sizzle -23 dB re the signal at half). Flips XOR one bit of a nonzero code (zero stays zero: silence stays silent;
  sign-magnitude: no DC), counted down from the code's top bit by u^2 F of its bits (u uniform): at 3/4 mostly the
  top two bits, at max any bit, top-weighted. Measured: at 16 bits and RATE 0, GRIT max glitches 3.2 % of samples by
  more than 6000 LSB (jitter alone: 0.01 %); 0 flip events up to half, ~12 % of holds at 3/4, ~24 % at max.
- **Damage is monotonic along BITS** (crush_check, one point per detent): the quietest step never gets finer (16 bits
  ... 1 bit: 15 21 31 44 63 89 127 180 255 x5 361 511 723 1023 1350 1907 2324, then gated); a quiet (-36 dBFS) sine's
  SNR never rises. In the mu-law stretch quiet signals keep the linear 8-bit step (below tmu, where mu-law's steps
  would be finer) and the loud part gets G.711's coarse segments (top step 1060 at the centre). The loud (-6 dBFS)
  SNR rises once, by 1.7 dB, at full mu-law (G.711's top segment at K = 256 fits a sine slightly better than K ~ 75
  one detent before); below 3 bits SNR says little (a 1-bit square fits a sine better than 1.5 bits).
- **Level**: output gain = comp_bits(BITS) x comp_rate(RATE) (17-point tables over the raw knob, Q12), the inverse of the
  simulator's mean level change on sines (1 kHz -6 / -12 dBFS, 440 Hz) and saws (220 / 110 Hz) on the detent maps,
  smoothed; RATE capped at +2 dB. Ramped per sample (Q20, lands on the target exactly). Measured over BITS x RATE (0, 1/4, 1/2, 3/4,
  max each; 440 Hz sine and 220 Hz saw at -6 dBFS): -2.7 .. +1.8 dB.
- **DC blocker** 3.4 Hz (one-pole 2^-11; not in the clean path): 1 bit and the asymmetric pulse at 1 bit leave a few
  LSB. Its state tilts staircases by at most amplitude / 2048 LSB per sample (inaudible; the checks allow for it).
- **Knob smoothing**: two cascaded one-poles per block (1/4 each; a 1024 detent is half way after ~6 blocks, 8 ms;
  exact arrival); every parameter derives from the smoothed values; gains ramp per sample. Measured: turning each
  knob 16 detents (one every 2 blocks) or jumping all at once makes no step larger than the steady states passed
  through (no zipper).

## Screen
- **The 3D pyramid** (design/fx/crush3d.svg, generator design/fx/gen_crush3d.py, which imports gen_crush.py's helpers;
  the SVG header documents every id, threshold, matrix and recolour; `runtime_state()` is the reference). Published as
  `content/display/crush3d.svg` (build.py `FX_SVGS`). ui.c `pyramid_ready()` / `draw_pyramid()`: loaded lazily, every
  id found once with must = 0 (13 single nodes, 37 numbered ones, 38 recoloured styles); per frame visibility,
  matrices, recolours (only when the posterise row changes), one `svg_render`. Falls back to `draw_crush()` (the line
  placeholder) when the file is missing.
- Inputs from the DSP, so the picture follows the sound (runtime_state uses the same maps):
  - BITS: `bits_q12` is nb (the DSP's detent map), so courses, holes (h1 below 8 bits: the default's 8-bit is intact),
    the collapsed top and the colour rows come at the real bit depth; mu-law courses while `mu > 0.25`;
  - RATE: voxel levels from the RATE detents 4 / 10 / 18 / 25 (SP-1200, 11 kHz, ~3.6 kHz, 1.4 kHz), tiers 8 / 6 / 4 / 3,
    the first level without fine vertical edges (they moire on the OLED);
  - RATE, SQUASH, GRIT: the DSP's smoothed knobs `k[]`, so the picture moves with the sound's glide;
  - `gr` drives the squash pump (q = s (0.45 + 0.55 gr), at least 60 % of the height) and the dust puff (gr > 0.55);
  - GRIT: cracks (o >= .1 .25 .5 .6 .75 .9) and notches (.5, .75) show the amount, statically; the shake (clock
    jitter) needs a signal (`level` > 33, ~ -60 dBFS: still when nothing plays); `flips` since the last frame drive
    the XOR slices (1 / 2 / 3 for < 3 / < 7 / more flip events per frame; at most 2 at 3 bits or less with GRIT > 0.6)
    and start a debris fall (one per frame with flips, in up to ceil((o - .5) / .5 x 4) slots). Flip events also need
    a signal (the DSP counts them on its clock; they only change nonzero codes).
  - Colours: BITS posterises inside the neutral family (lit face purplegrey -> lpg, shade face grey -> offgrey -> black,
    a grey sun once crushed); green is BITS's accent (the 1-bit rim, rubble, mu-law courses); pure white belongs only
    to SQUASH's press, stem and puff.
  - Readout: a raw knob change of 512 or more shows its value for 45 frames (~1.5 s) at the title position (8, 7),
    size 1.5 (clear of the sun and the press), in the knob's colour: `RATE 11.0KHZ`, `BITS 8`, `BITS 7.5`,
    `BITS 8 MU-LAW` (the font has no mu), `SQUASH 4.0:1` / `20:1` / `OFF`, `GRIT JITTER` / `GRIT FLIP` / `OFF`. Not
    on entering the screen or switching sound / mode.
  - Clock: the frame counter at 30 fps (t = frames / 30); jitter is ui.c's `jit()` hash (the header allows any).
- `crush_state[mode == 3]` (`struct crush`, `src/slot/crush.h`; `[0]` synth/drum, `[1]` master), updated once per block:
  `gr` (0 .. 32767 = 0 .. 30 dB, smoothed), `k[4]` (smoothed knobs), `rate_hz`, `bits_q12`, `mu` (Q15), `flips`
  (flip events, wraps), `level` (input peak, smoothed ~12 ms).
- Check: `src/slot/test/crush_screen_check.py [OUTDIR]` renders the design's 14 states through the C path (preview.py
  links the real crush.c and settles it on the knobs; `CRUSH_GR`, `CRUSH_LEVEL`, `CRUSH_FLIPS`, `CRUSH_TURN` set the
  rest) and through `runtime_state` (rsvg), side by side: the 9 states without GRIT match within anti-aliasing (mean
  |diff| 0.3 .. 0.7 of 255); GRIT states differ by randomness. Then the readout trigger (512 yes, 400 no, gone after
  55 frames), the shake gate (still on silence) and flips (slices and debris with flips, none on silence).
- Cost (Hypothesis until read on the device with `--svg-cost`; CRUSH2 / CRUSH3 show it): 10 visible shape elements
  (voxel levels, mu-law) to ~59 (everything at max) of 86; pedals has 66 shapes and read 17.6 M cycles per frame on the device, choir 2 .. 11 M.
  The large fills are the two faces, the shadow and the sun (smaller than pedals' bodies). The per-frame C work is a
  few thousand cycles (no loops over pixels; matrices and ~90 flag writes).

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
| fx.json default (10240, 12288, 10240, 0) | 13.7 k |
| typical: warm / lofi / SP-1200-like (26 kHz, 12 bits) / full-rate mu-law / full-rate linear | 11.0 k / 14.9 k / 17.9 k / 15.0 k / 9.5 k |
| worst: RATE 0 with jitter (holds both ways: about one interpolated hold per sample) + mu-law + SQUASH max | 31.0 k mean, 36.3 k peak |
- Distortion for comparison: ~29 k typical, 44.5 k worst; ensemble 21 k. The cost scales with holds per block, so
  RATE near the top with mu-law is the expensive corner.

## Open
- On device: the sound (especially SQUASH's pump and how far GRIT goes), the default, cycles (simulator vs BF524),
  the FX browser with 12 rows.
- The RATE level table is fitted to a few test signals; at 400 Hz a sine near a multiple of the rate can come out
  quieter or louder (aliasing onto DC / Nyquist).
