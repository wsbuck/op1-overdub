# Harmonic (additive) engine: draw the level of every harmonic (design, firmware 246)

Status: **research and design only** (2026-10-14). No code yet.
Marks: **Confirmed** = read in the binary, our code or a cited source; **Hypothesis** = inferred, untested on device.

## Recommendation
- A new engine, `harmonic`, where the user sets the level of **each of 24 harmonics** like a bar graph:
  - blue moves a cursor; green sets the level under it;
  - white "paints": it moves the cursor and writes the current level to each bar it crosses.
- Pattern helpers (tilt, stamps such as saw/square/even) are secondary, on SHIFT.
- **Storage:** 24 levels × 4 bits = 96 bits, packed into synth knobs 2..7 through a new encoder hook. They **save
  with the sound** in the patch's `"knobs"` array, like any knob value.
- **Sound:** the UI thread rebuilds one band-limited single-cycle table (9 mip levels, `wt_import.h`'s inverse FFT
  and levels code) whenever a level changes. The engine plays it with wt.c's oscillator and crossfades old → new
  over a few ms.
- **Audio cost** is wavetable's (about 200–390 cycles/sample with unison, sim), whatever the harmonic count.
  A rebuild costs about 1.6 M cycles (about 4 ms) on the UI thread.

## The Vital feature
- **Confirmed** (Vital source, `wave_source_overlay.cpp`): in Vital's wavetable editor, a **Wave Source** layer
  has a frequency-domain editor under the waveform.
  - It is two `BarEditor`s: one amplitude bar per harmonic (square-scaled) and one phase bar per harmonic.
  - There are 1025 bins for the 2048-sample frame.
  - Drawing bars rewrites the frame (`loadFrequencyDomain` → time domain), so picking H2, H4, H6, H8 directly
    reshapes the wave.
  - Keyframes interpolate as "Waveform Blend" or "Spectral Blend" (with "Smooth" versions).
- Related Vital features (often confused with it):
  - **Spectral Morph** on the oscillator (`kSpectralMorphNames`): None, Vocode, Formant Scale, Harmonic Stretch,
    Inharmonic Stretch, Smear, Random Amplitudes, Low Pass, High Pass, Phase Disperse, Shepard Tone, Spectral Time
    Skew. Vocode and Formant Scale use `evenOddVocodeMorph`, which shifts the spectrum while keeping each harmonic's
    odd/even parity. There is no "Even/Odd" mode by that name.
  - The wavetable editor's **Frequency Filter** modifier: Low Pass, Band Pass, High Pass, Comb. Comb is a periodic
    harmonic mask, an "every n-th" helper.

## Similar instruments
| instrument | harmonic control |
|---|---|
| Ableton Operator, User wave | draw 16, 32 or 64 harmonic amplitudes; Repeat extends the pattern upward with a fade (Confirmed, Live manual) |
| Plaits, model 5 (harmonic oscillator) | no drawing: HARMONICS = number of spectral bumps, TIMBRE = most prominent harmonic, MORPH = bump width (Confirmed, manual) |
| Hammond drawbars | 9 bars, levels 0–8 (about 3 dB per step). Footages 16', 5⅓', 8', 4', 2⅔', 2', 1⅗', 1⅓', 1' are harmonics 1, 3, 2, 4, 6, 8, 10, 12, 16 of the 16' pitch, so **24 harmonics played an octave down cover every drawbar** |
| Kawai K5000 | 64 harmonics per source, each with its own envelope, plus a formant filter (background) |
| NI Razor / Image-Line Harmor | hundreds of partials (320 / 516) driven by filter-like and resynthesis modules, not by drawing (background) |
| Synclavier partial timbres | 24 harmonic coefficients per timbre (background) |

## How many harmonics, and how to move between them
| count | bits per level | storage | fits | verdict |
|---|---|---|---|---|
| 16 | 4 (16 steps) | 64 bits = knobs 4..7 | leaves knobs 0..3 as live macros | too few for bright sounds; one Hammond octave short |
| **24** | **4** | **96 bits = knobs 2..7** | 32 bits left for globals (knobs 0..1); 24 keys; 13 px per bar on screen | **recommended** |
| 32 | 4 | 128 bits = all 8 knobs | nothing left for unison, tilt or extend | no |
| 32 | 3 (8 steps, 6 dB) | 96 bits | coarse levels | possible, worse to draw with |
- Levels: 0 = off, 1..15 = -42 … 0 dB in 3 dB steps (Hammond-like).
- Above 24, **EXTEND** (Operator's Repeat) continues the drawn pattern to harmonic 256 with a fade.

| way to move | for | against |
|---|---|---|
| **blue cursor + green level** | precise; stays on screen; no new key hooks | 23 detents end to end |
| **white paint** (moves the cursor and writes the last green level to every bar it passes) | fast ramps, "fill the evens" in two passes, clear a range | needs the level set first |
| **keyboard** (SHIFT + key n selects harmonic n) | instant jumps; key = harmonic number, easy to learn | on this engine it takes over CHORD's SHIFT+note learn; the note still sounds (handy for auditioning). Code exists: `chord_keydown` already sees shifted notes |
- Recommendation: blue/green/white in v1; SHIFT+key as v1.1, after the user decides whether CHORD learn may be off
  on this engine.

## Controls (type 3 range rows + encoder hook)
| | blue | green | white | orange |
|---|---|---|---|---|
| plain | CURSOR 1–24 (RAM, not saved) | LEVEL of the cursor harmonic | PAINT: sweep the cursor, writing the level | UNISON (wt.c's second oscillator) |
| SHIFT | TILT: non-destructive brightness, ±dB/octave at build | EXTEND: repeat above 24, 0 = none | STAMP (destructive helper): saw, square (odd), triangle, even only, organ 16'+8'+4', clear; one undo in RAM | WARP (wt.c's warp.h) |
- **Why the hook is needed. Confirmed** (disassembly 2026-10-14):
  - Encoder turns reach `FUN_01a1dfec` from the CALL at **`0x01A5063E`** (R0 knob, R1 &delta, R2 engine id,
    [SP+0xC] knobs `0xFF8013C4 + slot*16`).
  - Then `FUN_01a00d14(patch, knobs)` at `0x01A50646` writes the array into the patch JSON.
  - Our hook (HOLD-style CALL replacement) runs only for our engine ids. It turns each delta into a cursor move or a
    packed level edit in the knob array, then passes delta 0 to TE, so TE's own call saves the result.
  - Running our code there is a Hypothesis.
- **8 independent knobs. Confirmed** (disassembly): the range row's type is read per knob. Type 3 (dsynth) clamps
  knob k against its own row with no SHIFT fine remap (`0x01A1E512`). Our engine rows are static in our block.
  The rows' min/max are set to -32768..32767, so a clamp can't damage packed values.
- Packed layout (int16 each):
  - knobs 2..7: 4 harmonics × 4 bits;
  - knob 0: unison 6 bits + warp 6 bits;
  - knob 1: tilt 5 bits + extend 4 bits;
  - spare bits: a format version.
- **TE's LFO** modulates the per-voice params copy (fx-slot.md). The engine therefore never reads harmonic data from
  `params`. The UI thread unpacks from the unmodulated array `0xFF8013C4` and publishes it. So the LFO has no
  effect on this engine in v1 (Hypothesis that it can't target knobs 4..7 anyway).
- Presets: factory sounds are 8 packed values. `add_engine_db.py` needs an 8-value form, and a small encoder in
  gen_*.py turns `levels=[…24]` into knob values.

## Knob hook (built for SAVEPROBE, 2026-10-14)
- The hook and the type-3 rows are built and described in notes/knob-hook.md, with the probe engine `sprobe` and
  the device questions (hook runs, SHIFT independence, packed values across save/load/reboot, clamping, LFO on 4..7).
- **Confirmed constraint:** TE's patch validator rejects any `knobs[k]` outside the engine's row min..max (error 15)
  and a rejected factory preset or slot stops the OP-1 at boot. So every knob that holds packed bits needs a
  -32768..32767 row, and factory presets must be generated inside the rows (build_slot.py checks this).
- **Confirmed:** TE stores the knobs sign-extended and loads them back with no clamp (`FUN_01a00d14`, `FUN_01a010f8`).
  Whether anything else rewrites them is what SAVEPROBE tests.

## Where per-harmonic data can be stored
| place | saves with the sound | survives reboot | status |
|---|---|---|---|
| **patch `"knobs"`, 8 × int16** | **yes** | **yes** | **Confirmed** format (factory DB; negatives exist: `digital` -1000, `dna` -16000). The only per-sound space confirmed. 128 bits. |
| other patch arrays (`adsr`, `fx_params`, `lfo_params`, 8 each) | yes | yes | **no**: TE's envelope (T2 SHIFT uses adsr 4..7), FX and LFO read them |
| an extra JSON key in the patch (e.g. `"harm":[…]`) | maybe | maybe | Hypothesis. Sampler patches carry their own `base_freq`, but that is TE code, not proof that unknown keys survive save. Needs `patch.cpp`'s serializer read, plus a probe. Would give room for 64+ harmonics and phases. |
| **TE's user-DB `knobs` table** (`id TEXT, data TEXT` JSON: tempo, mic gain, radio …) | no (global) | **yes** | Confirmed table and SQL (`REPLACE INTO knobs VALUES ('%s','%s')` `0x018D3260`, used at `0x0196940E`; `SELECT data FROM knobs WHERE id='%s'` `0x018D32B0`, `0x0196947C`). Calling it from our code is a Hypothesis. **Fallback for "user slots"** (e.g. `harm_u1..8`). Stock firmware never asks for those ids, so reverting is safe (Hypothesis). |
| our FREE-region RAM | no | no (lost at power-off; FILL-zeroed at boot) | scratch and undo only |
| the SAMPLE trick (a sampler slot holds the source) | no | only as the sampler's sample | as wavetable's SAMPLE bank: not saved with the sound (wavetable-custom.md) |

## DSP: playing the result
- **Recommended: table rebuild on the UI thread**, as `wt_user_tick`.
  - **Input:** 24 levels (+ tilt, + extend) → amplitudes for harmonics 1..256, sine phase.
  - **Levels:** the second half of `wti_frame` already goes from a spectrum (`cr/ci[k]`) to 9 band-limited levels
    by inverse FFT, with exact gen_wt_data band limits. Split it into a `wti_levels(spectrum, dst)` function and
    reuse it. **Reuse works**; the forward FFT, period detection and resampling are not needed.
  - **Cost** (from WTS sim figures): inverse FFTs 2048 + 1024 + … ≈ 1.6 M cycles, about 4 ms at 400 MHz, in one UI
    frame. Rebuild at most once per frame while editing, i.e. immediately; no 10-frame debounce is needed.
  - **Loudness:** don't peak-normalise each edit (lowering one bar would raise the rest). Use a fixed gain from
    √Σa² (RMS), with the peak capped at 0.9 FS. That cap keeps wt.c's interpolation-delta bound (Bernstein,
    wavetable-custom.md). Sine phases keep the textbook shapes on screen; the soft knee catches the rest.
  - **Publishing:** 3 single-frame buffers × 4128 int16 = 24.8 KB in FREE RAM (the wt_user header pattern). The
    engine crossfades from the buffer it plays to the newly published one over about 8 blocks (12 ms), using
    wt.c's frame-blend path (`ff`). That makes edits click-free. With 3 buffers, the UI never writes one a voice
    may still fade from.
  - **Before the first build** (boot, before our screen has shown): play a built-in sine frame (flash data). Same
    limitation as SAMPLE: the hook runs only while our screen is visible.
- **Alternative: real-time oscillator bank.** 24 interpolated sines in C ≈ 10 cycles each = about 240–300
  cycles/sample/voice, plus band-limiting per block (skip n·f > 20 kHz).
  - Pros: no UI dependency, instant, per-voice LFO possible, per-harmonic envelopes possible (K5000-style).
  - Cons: 32 or more harmonics, or EXTEND, exceed the budget with unison.
  - Keep it in mind for a v2 "evolving" mode.
- **Engine:** wt.c's oscillator core (`lookup`/`pick_level`/`osc`, unison, warp, DC blocker), moved to a shared
  header and pointed at the published frame (1 frame instead of 12; the frame blend becomes the crossfade).
  Cycles ≈ wavetable's.

## Screen (iter page hook)
- **Bottom:** 24 bars at x 8..312 (13 px pitch), height ∝ level in 3 dB steps.
  - Bars are grey; off bars are a dark-blue stub.
  - Odd/even ticks under the bars in dark blue, and a mark every 4th, to help pick H2/H4/H6.
- **blue:** the cursor bar, drawn thick, with its number above in the stroke font (`H6`).
- **green:** the cursor bar's fill and a `-9 dB` readout.
- **white:** the paint trail. Bars changed in the last ~0.5 s are white and fade to grey.
- **SHIFT helpers:**
  - TILT is a blue curve over the bars, showing the effective levels;
  - EXTEND is faded bars beyond 24, drawn as a short "…" ramp;
  - STAMP shows its pattern name for 1.5 s, as wt's bank name.
- **Top:** one cycle of the published table (level 3, as `wt_sample`) in blue, with the orange unison ghost as
  wavetable's. The waveform visibly reshapes as bars change.
- About 40 AA lines plus 64 fast segments.

## Mode or bank of `wavetable`?
- **Technically yes:** a 10th "HARM" bank could take its frame from the harmonic builder (wt.c only needs a base
  pointer, as SAMPLE).
- **Recommended no.** Per-harmonic editing needs all 8 knobs and the hook, and wavetable's knobs are taken
  (position, bank, warp, unison; SHIFT = fine). A bank also wouldn't save its harmonics with the sound unless
  wavetable's knobs 2..7 changed meaning.
- **Share the code instead:**
  - the oscillator core and warp: wt.c → header;
  - the levels builder: wt_import.h → `wti_levels`;
  - the double-buffer and publish pattern: wt_user.h.
- Comparison with wavetable:

  | | wavetable | harmonic |
  |---|---|---|
  | source | 12 fixed frames per bank, morphed by blue | one frame the user draws |
  | per-sound content | which bank | the drawing (24 × 4 bits) |
  | live morph | position, per voice | none in v1 (TILT and level edits rebuild the shared table) |
  | cost | same oscillator | same oscillator + UI rebuilds |

## v1 / v2
- **v1:**
  - encoder hook;
  - type 3 rows;
  - 24-bar editor (cursor, level, paint) with SHIFT helpers;
  - packed storage in knobs 0..7;
  - UI-thread rebuild with crossfade;
  - wt.c core with unison and warp;
  - 8 presets (organ 16'+8'+4', clarinet-ish odd, hollow even, bell-ish sparse, saw, square, choir-ish formant
    bump, glass);
  - sim checks: harmonic levels within 0.1 dB, edit crossfade without clicks, aliasing at C8, cycles.
- **v1.1:** SHIFT+key selects a harmonic (decide on CHORD).
- **v2:**
  - user slots in the `knobs` table (SHIFT+orange: recall/store);
  - two drawings morphed by a live knob, per voice and LFO-able, built as a 12-frame bank between drawing A and
    drawing B. This needs the extra storage below;
  - per-harmonic phase;
  - an "evolving" real-time-bank mode.

## Risks and unknowns (device probes)
1. **Encoder hook:** do values written into knobs 0..7 survive save, preset reload and reboot, and is TE's encoder
   acceleration usable for a 24-step cursor? (Shared with fm-operator.md.)
2. **TE's LFO** with element targets on this engine: does it touch params 4..7? (We don't read params, so the risk is
   only that the LFO does nothing.)
3. Whether TE clamps or validates knob values when loading a patch. Our rows are full-range, so this matters only if
   TE checks against something else.
4. Whether **unknown patch JSON keys** survive. That decides whether 64 harmonics with phases and two drawings are
   possible per sound.
5. The `knobs`-table write from our code: which thread, and whether `FUN_0196f950`/`FUN_0196dd44` are the DB exec and
   DB getter, as they appear.
6. The UI frame-time hit of a 4 ms rebuild (WTS1 measures the same kind of work).
7. 88.2 kHz: wt.c's level choice uses pitch increments, so it is right at either rate. The sim still runs at 44.1 kHz.

## Sources
- Vital source (`mtytel/vital`): `src/interface/wavetable/overlays/wave_source_overlay.cpp` (amplitude/phase bar
  editors, interpolation types), `src/common/wavetable/wave_source.h`, `src/interface/look_and_feel/synth_strings.h`
  (`kSpectralMorphNames`), `src/synthesis/producers/spectral_morph.h` (`evenOddVocodeMorph`),
  `src/common/wavetable/frequency_filter_modifier.cpp` (Comb): <https://github.com/mtytel/vital>
- Vital overview: <https://synthanatomy.com/2020/11/vital-free-spectral-warping-wavetable-synthesizer-from-the-maker-of-helm-vst.html>
- Ableton Operator (User waveform, 16/32/64 harmonics, Repeat): <https://www.ableton.com/en/manual/live-instrument-reference/>
- Plaits manual (model 5): <https://pichenettes.github.io/mutable-instruments-documentation/modules/plaits/manual/>
- Hammond, K5000, Razor, Harmor, Synclavier: background knowledge, not re-checked this session.
- Ours: notes/wavetable-custom.md, `src/engine/wt_import.h` (`wti_frame` levels half), `src/slot/ui.c`
  (`wt_user_tick`), disassembly `0x01A1DFEC`/`0x01A5063E`/`0x0196940E`/`0x0196947C`, factory and user DB schemas.
