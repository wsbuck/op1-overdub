# amp: a guitar amp synth engine (firmware 246)

A synth engine that plays TE's audio input (the line jack) through a guitar amp: gate, drive, tone, cab.
Input research: notes/input.md (AMPPROBE, test images 55–55d). Code: `src/engine/amp.c`, `amp.h`, `amp_tables.h`
(from `gen_amp_tables.py`); screen in `src/slot/ui.c` (`draw_amp`), tuner `src/slot/amp_tune.h`.
Status: test engine (engines.json `"test": true`), first image AMP1 (LOG.md test image 58).

## Platform facts it builds on
- `0xFF800CB4` (TE IN): int16 mono, 64 samples at 44.1 kHz, written at the start of every audio callback, before the
  voice loop; live whether or not a note is held (AMPPROBE4 on device: clean, instant, records to tape).
- The engine's output passes TE's note envelope, so the amp sounds only while a note is held or latched
  (SHIFT+T2 HOLD). Accepted by the user; the presets use full sustain (stock-range ADSR, the one AMPPROBE2 validated).
- Engines render 128 samples at 88.2 kHz per call, one call per sounding voice per callback.

## Knobs (plain type-0 rows: SHIFT gives fine adjust, as on the other engines)
| knob | control | why |
|---|---|---|
| blue | **drive**: pre-gain 0 → +44 dB (linear in dB) into a biased tube stage and a second soft stage (gain 0.6 → 4, quadratic); also tightens the lows (high-pass 30 → 160 Hz before the shaper) and pushes the upper mids (+6 dB pre-emphasis above ~1 kHz). Loudness compensated: −15 dBFS in gives about −15.5 dBFS rms from crunch to full, clean ~4.5 dB lower (it is peak-bound: −2 dBFS peaks). | The one control every amp has; clean → crunch → lead on one knob. |
| green | **tone**: tilt, dark (+5 dB below 220 Hz, −10 dB above 2.2 kHz) … flat at the centre … bright (mirror), in a blackface-style stack between the interstage low-pass and stage 2 with a fixed mid scoop (−8 dB at 450 Hz, Q 0.7), so tone changes what clips. | One knob that covers "darker / brighter"; the fixed scoop is the Fender stack's mid dip, the cab's bells add the speaker colour. |
| white | **gate threshold**: off at 0, then −84 → −30 dBFS. Opens above it (0.7 ms), closes 6 dB below (hysteresis) after a 50 ms hold, then fades over ~93 ms; the detector runs 12 samples ahead of the audio (the upsampler's delay), so picks aren't clipped. | High gain amplifies pickup hum and hiss between notes; the right threshold depends on the guitar and room, so it must be a knob. Compression is not needed as a knob: the drive stages and the limiter already compress. |
| orange | **cab**: direct (no cab) → 1×12 open back → 2×12 → 4×12 closed back (parameters interpolated in log frequency / dB between the four voicings). | The cab is most of an amp's character, and direct is useful into the OP-1's own FX or an external cab. Output level is not a knob: loudness compensation and the limiter keep it steady, and the OP-1 has volume and the mixer. |

Cab voicings (gen_amp_tables.py `CAB`): high-pass (resonant low end), mid bell (scoop), presence bell, two low-passes:

| | high-pass | mid | presence | low-passes |
|---|---|---|---|---|
| direct | 20 Hz, Q 0.7 | 0 dB | 0 dB | 19 kHz |
| 1×12 | 80 Hz, Q 1.0 | −3 dB at 700 Hz | +3 dB at 2.8 kHz | 6.2 / 7 kHz |
| 2×12 | 85 Hz, Q 1.3 | −4 dB at 600 Hz | +4 dB at 2.5 kHz | 5.2 / 6 kHz |
| 4×12 | 95 Hz, Q 1.8 (+5 dB bump) | −5 dB at 500 Hz | +4 dB at 2.2 kHz | 4.3 / 5 kHz |

## Chain (per block, amp.c)
TE IN → gate detector → 2× half-band upsampler (24-tap odd phase, Kaiser β 7: flat to 18 kHz within 0.1 dB, −58 dB
images above 34 kHz) → tight high-pass → pre-emphasis → drive × gate gain → stage 1 (distortion.c's tube curve on
u + bias, bias 0.12 → 0.30: even harmonics) → interstage low-pass (two one-poles at 7 kHz) → tone stack (low and high
shelf, mid scoop) → stage 2 (1.5 (a − a³/3), distortion.c's MUFF stage 2: the tube curve at 1.5× its input) → cab
(high-pass, two bells, two low-passes) → loudness and the stack's make-up → limiter (−3 dBFS, gain from the previous
block's peak, 93 ms release) → `soft_knee`.
- **Passes** (DSP review pass 2, test image 66): the gate in C (per input sample: the gate gain × the drive gain), then
  hand-written Blackfin loops over the block: upsampler (dual 16-bit MACs on word pairs; the history is kept twice,
  one copy shifted a sample, so every pair is aligned), drive (tight, pre-emphasis, gain, saturate to ±32, bias), stage
  1, interstage, the three tone SVFs, stage 2, the five cab SVFs; then the output loop in C. The gate gain multiplies the
  drive gain after the tight and pre-emphasis filters (they are linear, so only their states differ while gated).
- **ADAA** on both shapers: first-order antiderivative anti-aliasing, distortion.c's tube kernel (`tube_fast`: the
  pair's mean in closed form, (a + b)/2 − (a + b)(a² + b²)/27, the linear term exact, the cubic from 16-bit products)
  and its `FIX` for pairs straddling the knee ((F(b) − F(a))/(b − a) through `recip_tab`, at most FIXMAX = 32 per
  stage and block, the rest keep the closed form). Its pair average is (1 + z⁻¹)/2 where the curve is linear (−0.6 dB
  per stage at 10 kHz, in amp_check's expected responses). Stage 1's DC (the kernel's mean at the bias, exactly) is
  removed before the interstage low-pass, so digital silence stays exactly 0.
- **Tone stack** before stage 2, passive-like: stage 2 sees the stack scaled to a peak gain of 0 dB (P = 1 / its peak:
  −5 dB at full dark, −10 dB at full bright), so the extremes cut what clips rather than boosting it (and drive stage
  2's highs 10 dB less at full bright: aliasing −34 → −42 dB at 2489 Hz there). The level is made up after stage 2 by
  1 + (1/P − 1) c: c = 1 on the dark side; on the bright side c = 1 up to blue 1/4, then 0.94, 0.71, 0.44, 0.24, 0.11,
  0.03 at full drive (`MAKEUP_C`, measured so tone 0 / 32767 sit about +4 / −4 dB from the centre at every drive, as
  pass 1 had them with the stack after stage 2; at clean the response is exactly the stack's). The stage-2 gain
  (1.5 k2 × the high shelf's A² × P) and the make-up ramp across the block.
- Tone stack response (gen_amp_tables.py, dB at 60, 200, 450, 1k, 3k, 8k Hz): dark +4.6 −0.2 −7.9 −4.3 −7.4 −9.7;
  centre −0.3 −3.1 −8.0 −3.2 −0.4 −0.1; bright −5.3 −6.1 −8.1 −2.1 +6.6 +9.6. The interstage's second pole was added
  with the move: with the scoop the stage-2 input carries relatively more of stage 1's aliases (float model, tone
  centre, 997 Hz: one pole −42.9 dB, two −53.1).
- Filters: Simper's trapezoidal SVFs (dsp.h `svf_coefs`), coefficients per block from 33-point knob tables
  (interpolated, knobs slewed 1024 per block), run block-wise in assembly: each sum of two 32 × 32 products (without
  the low × low partial product) in one accumulator chain, coefficients reloaded in parallel slots, 17 (low-pass) …
  25 (shelf) instructions per sample (was 34 in C); the result is closer to a double reference than the C version was.
  The high shelf runs normalised (A² divided out of m1, m2) with its A² in the stage-2 gain.
- No runtime division in the audio path (udiv / recip_q31 loops per block only; ADAA's division is a table
  reciprocal), GCC 4.3. amp_ram 3,416 B of its 8 KB.
- The shapers' C forms stay in amp.h (`amp_tube`); distortion.c is unchanged (its kernels are copied).

## One voice plays the input
Every sounding voice calls render, so without care a chord would stack copies of the guitar. Shared state lives in
`amp_ram` (engines.json `"ram"`, FREE region 1 at `0x01F00000`; the image zeroes its first 256 bytes, the engine
initialises the rest when `magic` is wrong).
- **Block detection** (there is no per-callback hook): a call starts a new block when TE IN differs from the stored
  copy of the last block, when `te_rx_sel` toggled, or when the same voice renders again while the input is digital
  silence. So a voice rendered twice per callback (LOG.md's unexplained "2 render calls per block") or a `te_rx_sel`
  that never toggles both still give one block per callback (checked in the simulator; in digital silence the extra
  blocks only advance the silent DSP).
- **Owner**: a voice takes over when it has a fresh note-on (TE writes `0x80000000` to state bytes 0..3: the newest
  note wins), when there is no owner, when the owner skipped a whole block (it stopped rendering: release ended, voice
  freed), or when the owner is released and this voice isn't (iter's note-off sets state byte 28; Hypothesis: if it
  doesn't, the skipped-block rule still hands over, after the release ends).
- **Gains**: each voice has a gain in a table in `amp_ram`, stepping 1/8 per block towards 1 (the owner as of the
  block's start) or 0, ramped per sample. A handover is an 11.6 ms crossfade of the same output block, so the gains
  always sum to one and the level stays put; taking "the owner at the block's start" makes a takeover in the middle
  of the voice loop start everyone's ramp on the next block together. A voice that skipped a block, or has a fresh
  note-on while not the owner, fades in from 0. Non-owners at gain 0 output silence.
- **DSP once per block**, by the first voice to render in it (so state runs on through handovers).

## Screen (src/slot/ui_amp.h)
A small combo amp in an oblique view, about 56 % of the width (front face, top with a carrying handle, right side, a
floor shadow), drawn like TE's object screens (the phone and boxer FX, tombola): thin lavender (purplegrey) outlines
at one weight, grey for inner structure, the knob colours only where a knob acts, a few moving details. Redesigned
after the user's review of the first, full-width blackface front (too flat, cluttered dot grille). ~50 AA lines at
most (with sparks and the tuner); the rest are 1-px fast lines.
- **blue** (drive): the panel's blue knob; the combo and its speakers rumble (offset ∝ drive² × input level) and sparks
  fly from the speakers above half drive (by drive × level). Only while a note sounds: clean or idle stays still.
- **green** (tone): the cloth's glints and the speakers' dust caps in green shades, dim and few (dark) → bright and
  many (bright), and the weave (dark grey → dark blue); one glint twinkles at a time.
- **white** (gate): the threshold mark on the panel's level meter (−60 … 0 dBFS); the meter is light blue and the
  lamp white while the gate is open (the engine's state while playing, level vs threshold otherwise).
- **orange** (cab): the speakers behind the cloth: none ("DI"), 1, 2 or 4.
- The panel knobs are small cylinders in their colours; the red pilot jewel's rays grow with the input.
- **tuner** (top left, only while no note sounds): note, octave, cents needle on a ±50 scale, green within 3 cents;
  "--" when there is nothing to read. Every 4th frame, when TE IN has signal (above −50 dBFS), the UI thread polls TE IN
  for 13 consecutive blocks (18.9 ms: a block is new when `te_rx_sel` toggles or its samples change; refused if a block
  may have been missed), then runs YIN (amp_tune.h). Display: median of three readings, a new note needs two, cents
  smoothed, held for 6 updates.

## Verified (simulator / host)
- `src/engine/test/amp_check.py` (bfin-elf-run, 6 voices, amp_main.c): gain staging for every preset and drive corner,
  0 dBFS input against the limiter, gate thresholds / hysteresis / hold / release, THD vs blue, aliasing at maximum
  drive (997 / 2489 Hz at tone dark, centre, bright), cab and tone responses against the design (white noise,
  ≤ 0.7 dB), the tone stack's mid scoop, ownership (chord, new note, release, stop without the release
  flag, steal, many notes, double renders, no `te_rx_sel`, digital silence), output guard, cycles, ABI.
- `src/slot/test/amp_tune_check.py` (host): notes from drop D to E6, readings across the note's decay, silence/noise.
- Host previews (`preview.py`, engine 21; `AMP_PLAY`, `AMP_DB`, `AMP_HZ`, `FRAMES`).

## Unverified / risks
- Device cost: ~57.7 k cycles per DSP call in the simulator for guitar, 61.9 k worst (noise or a 0 dBFS 9 kHz sine at
  full drive: FIXMAX hit in both stages); budget 76.8 k (pass 1: 73.9 k). The device has run 1.2–1.4× the simulator.
  Only one voice per block pays it (others ~1 k).
- `amp_ram` (and the filter states, and since pass 2 the per-pass buffers: each pass reads and writes 128 words) are in
  SDRAM; if SDRAM data is not cached the device cost rises.
- The UI capture busy-waits ~19 ms on the UI thread every 4th frame, only while no note sounds and there is input.
- The released flag (byte 28) and the block detection's assumptions are Hypotheses with fallbacks (above).
