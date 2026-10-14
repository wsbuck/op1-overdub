# FM engine with operators, Ableton Operator style (design, firmware 246)

Status: **research and design only** (2026-10-14). No code yet.
Marks: **Confirmed** = read in the binary, our code or a cited source; **Hypothesis** = inferred, untested on device.

## Recommendation
- A 4-operator phase-modulation engine (`op`) with 11 algorithms, per-operator ratio/fine/level/envelope, feedback, and
  a pitch-tracked index limiter (Operator's "Tone").
- **Knobs:** 4 live macros. Per-operator values are edited on operator pages (SHIFT+blue picks the operator) through
  one new hook on the encoder path. They are packed into synth knobs 4..7, so they **save with the sound**.
- **CPU:** about 150–200 cycles/sample/voice in C, i.e. 25–35 % of the 600 budget. Sine table lookups only.
- **Order:** build it after the harmonic engine (notes/harmonic-synth.md). That engine needs the same encoder hook
  and storage scheme, and gets them proven first.

## Reference designs
| synth | ops | algorithms | per op | notes |
|---|---|---|---|---|
| **Ableton Operator** | 4 (A–D) + LFO | 11, from the 4-op stack D→C→B→A to 4 parallel carriers (additive) | Coarse (0.5, 1, 2 … 48; integers = harmonic), Fine (fraction: inharmonic), Fixed mode (Hz, independent of the key; Freq × Multiplier, down to 0.1 Hz), Level, Phase, waveform (sine, 4/8-bit sine, saw/square/tri variants with set harmonic counts, noise, **User**: drawn amplitudes of 16, 32 or 64 harmonics, plus Repeat for higher partials with a fade-out), Feedback (any op not modulated by another), envelope (Initial/Peak/Sustain levels, Attack/Decay/Release times; loop modes Loop/Beat/Sync/Trigger), velocity and key scaling | Filter: LP/HP/BP/notch/Morph, 12/24 dB, several circuit types. LFO: targets op pitches and the filter; classic shapes, S&H, noise. Pitch envelope: ADSR, targets per op and the LFO. Global: Spread, Transpose, **Tone** (high-frequency content vs aliasing), **Time** (scales all envelope times). Up to 32 voices. |
| **OP-1 stock `fm`** | 4 | 9 (Attack Magazine) | none directly | blue: FM intensity (levels of the non-base operators: modulation depth, or volume for carriers); green: frequency (picks a *set* of 3 ratios for the other operators); white: topology (algorithm); orange: detune. Default knobs `[20000, 4500, 3000, 18000, 15000, 0, 100, 1500]`: TE also stores values in knobs 4..7 (factory DB, Confirmed). |
| DX7 / Volca FM | 6 | 32 | ratio or fixed, level, 4-rate/4-level EG, key/velocity scaling | one feedback operator per algorithm; Volca FM plays DX7 patches |
| Elektron Digitone | 4 in groups C, A, B (B1+B2 share macro controls) | 8 | ratios, HARM (harmonics: reshapes the operator waves), detune, feedback, MIX between output pairs | A and B have their own envelopes |
| Mutable Plaits, model "2-op FM" | 2 | 1 | HARMONICS = ratio (quantized table), TIMBRE = index, MORPH = feedback (op 2 on itself, or on op 1's phase) | runs **4× oversampled** (fm_engine.cc) |

What Operator adds over the OP-1's `fm`: free per-operator ratios (not preset sets), per-operator levels and
envelopes, and feedback. Those are what we add. The filter, LFO and pitch envelope already exist on the OP-1 (filter
FX, T4 LFO, T2 amp envelope), so the engine leaves them out.

## Platform facts this design depends on
- Render: 128 samples per call per voice, 6 voices. Pitch is `int32[128]` phase increments from TE, so it is correct at
  any sample rate. Confirmed (engine-interface.md).
- **Sample rate 88.2 kHz.** Voices are decimated 2:1 by a 32-tap half-band (`FUN_ffa005d4`). Confirmed
  (fx-slot.md). The coefficients (`0x0106A6F0`) are filled at runtime, so the stopband is unknown. Absolute times
  (envelopes) must use 88.2 kHz. The simulator still assumes 44.1 kHz (simrun.py).
- Budget: about 600 cycles/sample/voice (simrun.py `CPU_BUDGET`). Overload is about 1,250.
- State: 32 B in the voice object, plus a **4 KB voice_ram slot** per voice (modal_state.h, used by grain and
  modal). Note-on is seen as `0x80000000` in state bytes 0..3 (grain.c). Note-off: iter's +0x2C sets byte +0x20
  (Hypothesis that our vtable copy keeps that behaviour). The design does not need it: release is TE's amp envelope.
- **8 independent knobs are possible. Confirmed** (disassembly 2026-10-14). `FUN_01a1dfec` reads the type from the
  range row *of the knob being turned* (`row + knob*8 + 6`):
  - type 0 remaps knobs 4..7 to fine adjust of 0..3;
  - **type 3** (dsynth) clamps knob k against its own row with no remap (`0x01A1E512` → `FUN_01a1de68`).

  Our row 15+ is static in our block (engine-slot.md), so setting type 3 there makes SHIFT+knob = params 4..7.
  Fine adjust is lost.
- **Per-sound storage = the patch's 8 knob values.** These are the JSON `"knobs"` array, int16; factory values
  include negatives (`digital` -1000, `dna` -16000). The other patch arrays (`adsr`, `fx_params`, `lfo_params`)
  belong to TE's envelope, FX and LFO, and T2 SHIFT uses adsr 4..7. Confirmed (factory DB).
- **Encoder hook point. Confirmed** (disassembly). The mode-0 handler does
  `CALL 0x01A1DFEC` at **`0x01A5063E`** (R0 knob, R1 &delta, R2 engine id, [SP+0xC] knobs `0xFF8013C4+slot*16`). It
  then calls `FUN_01a00d14(patch, knobs)` at `0x01A50646`, which writes the knob array into the patch.

  A HOLD-style CALL replacement can, for our engines, read the delta and write packed values into the knob array.
  It then passes delta 0 to TE, so TE's own `FUN_01a00d14` stores the result in the patch. The hook mechanics are a
  Hypothesis, the same kind as hold.md.

## DSP
Per sample, per operator: `ph += inc·ratio`; `out = sin(ph + mod)·env·level`. Phase modulation, as DX7 and Operator.
- **Sine:** `sine256` interpolated (as va.c, about -90 dB error). 514 B, so it stays in cache. Cost about 10 cycles.
  Plain 4096-entry lookup without interpolation is the fallback if cycles are short (spurs around -70 dB).
- **Ratios:** coarse table {0.5, 1, 2 … 16, 18, 20, 24, 28, 32} (32 entries, 5 bits) × fine (4 bits: 0, small
  detunes in cents, and inharmonic offsets like +0.41, +0.5, √2). Per-block `inc_ratio` as Q16 × pitch[i] with a
  32×16 multiply per op per sample (about 3 cycles).
- **Algorithms:** 11, one specialised inner loop each (switch outside the loop). Proposed set (topologies as
  Operator's range; Operator's exact numbering is not verified):
  1. D→C→B→A
  2. (C+D)→B→A
  3. (D→C)+B → A
  4. D→C→A, B→A, with C+B summed (= 3 with D only on C)
  5. (B→A) + (D→C), two pairs
  6. D→(A, B, C), fan-out
  7. (D→C) + B + A
  8. B+C+D → A
  9. D→C→B + A
  10. (D→C)→(B, A), fan-out of a pair
  11. A + B + C + D, additive
- **Feedback:** on the top operator of the algorithm (D, or the unmodulated one). DX7-style: the mean of the last two
  outputs, which tames the noise. Up to about 1.2 rad gives saw-like tones.
- **Envelopes:** one per operator, computed per block, ramped linearly per sample (1 add). This avoids zipper on the
  index. 8 shapes (3 bits): flat; 4 decays to 0 (short → long); 2 decays to half; a slow swell. The **TIME** macro
  scales all of them (Operator's Time). Amp attack and release come from TE's T2 envelope.
- **Aliasing:** at 88.2 kHz, folded energy below 22.05 kHz needs partials above about 66 kHz. Content between 22 and
  44 kHz is removed by the decimator, so 88.2 kHz is effectively 2× oversampling for free. A per-block **index
  limiter** from pitch[0] (Carson: bandwidth ≈ 2(I + 1)·f_mod) reduces the index, and the feedback, as the bandwidth
  passes about 40 kHz. This is Operator's Tone, automatic.
- **Cycle estimate (C, GCC 4.3):** about 30/op × 4 + 8 feedback + 15 output (sum, scale, soft knee) ≈ 150, up to 200
  with register spills. **About 25–35 % of the budget**; 6 voices ≈ 0.12–0.15 M cycles per block. Measure in
  simrun like va/wt.
- **State:** voice_ram slot (OWNER "OPFM"): 4 phases, 4 envelope values and steps, 2 feedback samples, the block
  counter. Under 100 B.

## Controls (type 3 rows, encoder hook)
| page | blue | green | white | orange |
|---|---|---|---|---|
| **MAIN** (plain knobs; live; TE's LFO can target them) | INDEX: scales every modulator's level 0…2× | ALGORITHM 1–11 | TIME: envelope time scale | FEEDBACK |
| **SHIFT** | **OPERATOR select**: MAIN / A / B / C / D (RAM only, not saved) | — | — | — |
| **OP A–D** (plain knobs while an operator is selected) | RATIO coarse | FINE | LEVEL | ENVELOPE shape |
- Storage: knobs 0..3 are the macros, as plain values. Knobs 4..7 hold one operator each, 16 bits:
  coarse 5 + fine 4 + level 4 + envelope 3.

  These rows' min/max are ours (-32768..32767), so any TE clamp on load is a no-op (Hypothesis: TE clamps only on
  turns). The engine unpacks params 4..7 once per block.
- **Fallback without the hook (v0):** type 3 and 8 plain params.
  - Main: index, algorithm, time, feedback.
  - SHIFT: ratio of B, C and D (coarse+fine in one knob, snapping to the table), and a global detune.

  This loses per-op levels and envelopes (they come from the algorithm's defaults), but needs no new hook.
- Presets: curated, all 8 values. `add_engine_db.py` needs `--preset NAME=K0..K7` (today it takes 4 and pads zeros).

## Screen (iter page hook, like va/wt)
- Four operator boxes laid out by the algorithm. Modulators sit above their targets; carriers are on a bottom
  rail to an output tick. Routing lines are dark blue.
- **white:** the routing is redrawn when ALGORITHM changes. The algorithm number shows for about 1.5 s (as wt's
  bank name).
- **blue:** INDEX sets the thickness and brightness of the modulation lines.
- Each box shows its **ratio** (stroke font, green on op pages) and a **level bar** inside.
- A tiny envelope glyph sits under each box (white on op pages, scaled by TIME).
- **orange:** a feedback loop arc on the feedback operator, growing with FEEDBACK.
- On an op page the selected box is drawn bright, with a coloured underline per knob: blue ratio, green fine, white
  level, orange envelope.
- Right edge: one cycle of the carrier output at the current settings, computed on the UI thread (no envelopes),
  in blue (as va's shape).
- About 60–90 AA lines.

## v1 / v2
- **v1:** engine as above with sine operators; MAIN plus OP pages through the encoder hook; per-op data in knobs 4..7;
  8 curated presets (EP, bell, bass, brass, organ, pluck, metallic, pad). Sim checks: pitch, algorithms, aliasing at
  C6/C8 with maximum index, cycles.
- **v2:**
  - per-op waveform: Operator's User waves = a harmonic table per op. The UI thread builds a small 256-sample
    band-limited table, as in notes/harmonic-synth.md; it needs storage, see below;
  - fixed-frequency mode (a coarse-table entry);
  - key scaling of level;
  - velocity, if MIDI velocity reaches us;
  - more storage (see risks) for full ADSR per op.

## Risks and unknowns
- The encoder hook (shared with harmonic-synth.md): the call site is Confirmed. Untested: running our code there,
  the thread it runs on, and that `FUN_01a00d14` saves knobs 4..7 we wrote. **Probe:** a knob-4 value written by the
  hook survives save, preset reload and reboot.
- **TE's LFO** modulates the per-voice params copy (fx-slot.md). If it can target params 4..7, packed data would be
  corrupted per voice. Hypothesis: it targets only knobs 1–4. Probe: an LFO on each element while showing params
  4..7 on screen. Mitigation: the UI thread unpacks from the unmodulated array `0xFF8013C4` and publishes to the
  engine.
- 64 bits of per-op data is tight: no per-op waveform, fixed mode or full ADSR. The patch JSON may keep unknown keys
  (sampler patches carry `base_freq`). Hypothesis; it needs reading `patch.cpp`'s serializer, and would be a large gain.
- The half-band's stopband is unknown. Dump `0x0106A6F0` on device, or measure aliasing with a sweep.
- The sim runs at 44.1 kHz. Envelope times and the limiter thresholds need the 88.2 kHz fix first (LOG.md open item).

## Sources
- Ableton Live 12 manual, Instrument Reference (Operator): <https://www.ableton.com/en/manual/live-instrument-reference/>
- Operator algorithms (stack vs parallel): <https://www.musicradar.com/how-to/the-definitive-guide-on-how-to-use-operator-in-ableton-live>
- OP-1 FM engine (encoders, 4 ops, algorithms): <https://www.attackmagazine.com/technique/hardware-focus/unwrapping-an-unsung-hero-of-fm-synthesis-perhaps-exploring-the-op-1-field-op-1-og/>
- Plaits manual: <https://pichenettes.github.io/mutable-instruments-documentation/modules/plaits/manual/>; source
  <https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/engine/fm_engine.cc> (4× oversampling)
- Digitone (4 ops in C/A/B groups, 8 algorithms, HARM, MIX): <https://www.elektronauts.com/t/algorithms-and-their-effects/105498>
- DX7 / Volca FM: background (6 ops, 32 algorithms), not re-checked this session.
- Ours: disassembly `firmware/work/elf/OP1_vdk_246_dec.dis` (`0x01A1DFEC`, `0x01A5063E`), factory DB `synth_types`/`synth_presets`.
