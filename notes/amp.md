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
| green | **tone**: tilt, dark (+5 dB below 220 Hz, −10 dB above 2.2 kHz) … flat at the centre … bright (mirror). | One knob that covers "darker / brighter"; the cab already supplies the mid voicing a full TMB stack would. |
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
images above 34 kHz) → gate gain → tight high-pass → pre-emphasis → drive → stage 1 (distortion.c's tube curve on
u + bias, bias 0.12 → 0.30: even harmonics) → 7 kHz interstage low-pass → stage 2 (1.5 (a − a³/3), distortion.c's
MUFF stage 2) → tone (low and high shelf) → cab (high-pass, two bells, two low-passes) → loudness → limiter (−3 dBFS,
gain from the previous block's peak, 93 ms release) → `soft_knee`.
- Filters: Simper's trapezoidal SVFs (dsp.h `svf_coefs`), coefficients per block from 33-point knob tables
  (interpolated, knobs slewed 1024 per block), run block-wise with a 4-instruction 32×32 multiply (the builtin's
  low×low term dropped: ≤ 1 LSB). The high shelf's A² is folded into the loudness gain.
- No runtime division in the audio path (udiv / recip_q31 loops per block only), GCC 4.3, 204 B of stack.
- The design was not ported line by line from distortion.c (its DSP is FX-shaped inline assembly); its two curves are
  copied as plain C into amp.h. distortion.c is unchanged.

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
  0 dBFS input against the limiter, gate thresholds / hysteresis / hold / release, THD vs blue, cab and tone
  responses against the design (white noise, ≤ 0.7 dB), ownership (chord, new note, release, stop without the release
  flag, steal, many notes, double renders, no `te_rx_sel`, digital silence), output guard, cycles, ABI.
- `src/slot/test/amp_tune_check.py` (host): notes from drop D to E6, readings across the note's decay, silence/noise.
- Host previews (`preview.py`, engine 21; `AMP_PLAY`, `AMP_DB`, `AMP_HZ`, `FRAMES`).

## Unverified / risks
- Device cost: 73.4 k cycles per DSP call in the simulator (budget 76.8 k); the device has run 1.2–1.4× the
  simulator. Only one voice per block pays it (others ~1 k).
- `amp_ram` (and the filter states) are in SDRAM; if SDRAM data is not cached the device cost rises.
- The UI capture busy-waits ~19 ms on the UI thread every 4th frame, only while no note sounds and there is input.
- The released flag (byte 28) and the block detection's assumptions are Hypotheses with fallbacks (above).
