# harm and fm4: implementation

Status 2026-10-14: phase 1 (DSP, screens, edit functions) built and verified in the simulator and on the host;
phase 2 (integration into a test image, "Integration as built" below) done, **pending user flash**.
Designs: notes/harmonic-synth.md, notes/fm-operator.md.

## Files
| file | what |
|---|---|
| `src/engine/harm.h` | harm's packing (24 levels x 4 bits in knobs 2..7, unison/warp in 0, tilt/extend in 1) and `struct harm_ram` (engine <-> UI) |
| `src/engine/harm_build.h` | UI-thread table builder: levels, tilt, extend -> one band-limited frame (9 levels) |
| `src/engine/wt_levels.h` | the level-building half of `wti_frame` (wt_import.h), split out: `wti_levels`, `wti_store` |
| `src/engine/wt_osc.h` | wt.c's oscillator core (level pick, interpolation, level crossfade) as a header |
| `src/engine/harm.c` | harm engine: published frame + wt core, unison, warp, 1024-sample crossfade on republish |
| `src/engine/fm4.h` | fm4's knobs: v0 plain form and the packed per-operator form; tables; decode |
| `src/engine/fm4.c` | fm4 engine: 4 operators, 11 algorithms, feedback, envelopes, TIME, Tone limiter |
| `src/slot/harm_edit.h`, `src/slot/fm4_edit.h` | pure edit functions for the encoder hook |
| `src/slot/ui_harm.h`, `src/slot/ui_fm4.h` | screens (and harm's table publisher), for inclusion into ui.c |
| `src/engine/test/simdrive.py` | simulator runner at **88.2 kHz** for engines with their own driver |
| `src/engine/test/harm_main.c`, `harm_check.py` | harm sim driver (it also builds and publishes, as the UI does) and checks |
| `src/engine/test/fm4_main.c`, `fm4_check.py` | fm4 sim driver and checks |
| `src/slot/test/synth_edit_test.c` | host tests: both edit functions, harm's builder, fm4 decode/pack |
| `src/slot/test/screens_main.c`, `screens_preview.py` | host preview of both screens (no ui.c changes needed) |

Run: `python3 src/engine/test/harm_check.py`, `python3 src/engine/test/fm4_check.py`,
`cc -std=gnu99 -O1 -Wall -Wextra -Werror -I src/slot -I src/engine src/slot/test/synth_edit_test.c -lm -o /tmp/se && /tmp/se`,
`python3 src/slot/test/screens_preview.py OUTDIR`.

## Results
- **harm** (harm_check: all passed)
  - pitch exact; drawn levels within 0.04 dB of the request (saw, square, sparse -42..0 dB, tilt +-, extend to H60);
  - every built level band-limited (energy above its harmonic limit < -91 dB re 0.9 FS), peak <= 0.9 FS,
    interpolation deltas <= 14,932, float model SNR 64 dB;
  - loudness: one sine peaks at 0.6 FS; a saw has the same RMS;
  - aliasing below 20 kHz: worst -48.8 dB (warp 0, C7/C8), -41.2 dB with maximum warp;
  - crossfade saw -> square: the largest sample step equals the steady waveforms' own (a hard switch: 1.7 x);
  - glide 60 Hz -> 6 kHz: level steps <= 0.29 dB; extremes: no rails, DC <= 15;
  - CYCLES: 17,853 per call (139 per sample) plain, 35,750 (279) worst with unison + warp. Budget 76,800.
  - table build: 1.70 M cycles (4.3 ms at 400 MHz), once per UI frame at most while editing.
- **fm4** (fm4_check: all passed)
  - pitch exact; a 5:1 pair at I = 2 gives J_n(I) sidebands within 0.01 dB (n = -4..4, folding included);
  - all 11 algorithms (with feedback), 1:1 and 1:2 PM, feedback alone: harmonics within 0.05 dB of a float model;
  - feedback at max on D alone: saw-like (H2 -7, H4 -14, H8 -21.5 dB);
  - envelope 'short' tau 198 ms at TIME x1 (0.2 s), 797 ms at x4; swell; note-on restarts phases and envelopes;
  - v0 mapping: SHIFT ratios and detune as specified;
  - Tone limiter: aliasing below 20 kHz with maximum index at C6..C8, worst -45.1 dB; 16 kHz notes fade out;
  - extremes (all 11 algorithms x max/min x 27.5 Hz..16 kHz): no rails, DC <= 32;
  - CYCLES: 23,800..25,100 per call (186..196 per sample) typical, 28,771 (225) worst.
- **Host tests** (synth_edit_test.c): 979 checks pass.
- **Code size** (bfin, -O2): harm.o 2.9 KB, fm4.o 12.3 KB (11 specialised loops), ui_harm.h + ui_fm4.h + edit
  functions 11.6 KB in the slot block. No libgcc calls (`nm -u`: only TE's primitives and the RAM symbols).

## Screens (screens_preview.py)
- harm: 62..77 AA lines + 100..280 fast segments; fm4: 48..111 AA + 60..90 fast (wt/va: 50..80 AA).
- harm: 24 bars, cursor column bracketed in blue with a blue underline and its bar green, readout `H6 -15 DB`
  (blue/green) top left, PAINT trail white -> lavender -> grey, one cycle of the published frame top right in
  blue with the orange unison ghost, TILT as blue caps at the effective levels, EXTEND as a green ramp past H24, STAMP
  name in white, SHIFT values (`TILT -6 DB/OCT`, `EXTEND 11`, `UNISON 40`, `WARP 20`) for ~1.5 s after a turn.
- fm4: boxes by algorithm (letter, ratio, level bar, envelope glyph), modulation lines thicker/brighter with INDEX,
  orange feedback loop on D, carriers on a grey rail to the output cycle (blue, right). `ALG n` in green ~1.5 s after a
  change; v0 SHIFT ratios coloured by knob (B blue, C green, D white), `DETUNE n C` in orange; operator pages: the box
  in white with blue/green/white/orange underlines and `C 4.41` / `LVL 11 SWELL` top right.

## Integration as built (phase 2, 2026-10-14; test image 59)
- **engines.json** (both `"test": true`, so release builds are unchanged in behaviour):
  - `harmonic` (source harm) at `0x01EF1000` (max 0x2000; 3,062 B), ram `0x00801000` size 0xF000 (FREE region 0,
    between tramp.S's prof stats and voice_ram: amp took 0x01EF8000..0x01F02000, and region 1's tail is only 0xE000;
    struct harm_ram + struct wti_work = 0xEA48 B on the Blackfin). Rows 8 x `[-32768, 32767, 32767, 3]`.
  - `fm4` at `0x01EF3000` (max 0x4000; 12,504 B), no ram (per-voice state in voice_ram). Rows knobs 0..3
    `[0, 32767, 32767, 3]`, 4..7 `[-32768, 32767, 32767, 3]`. Packed presets (the hook builds the operator pages).
  - Both `"knob_hook": true`. Presets in engines.json order: harm saw, square, organ, clarinet, bell, choir, hollow,
    glass; fm4 e piano, bell, bass, pluck, metallic, pad, brass, organ (a `--db-trim NAME=6` test image keeps the first 6).
- **Knob mapping as built**
  - harm (all 8 stored, packed; harm.h): blue CURSOR (UI only), green LEVEL, white PAINT, orange UNISON; SHIFT blue
    TILT, SHIFT green EXTEND, SHIFT white STAMP (SAW SQUARE TRIANGLE EVEN ORGAN CLEAR, back to 0 = undo), SHIFT orange
    WARP. Storage: knob 0 unison/warp/version, 1 tilt/extend, 2..7 the 24 levels.
  - fm4: MAIN page blue INDEX, green ALGORITHM, white TIME, orange FEEDBACK (plain values, TE applies them, the LFO can
    target them); SHIFT blue = page MAIN, A, B, C, D; on an operator page blue COARSE, green FINE, white LEVEL, orange
    ENVELOPE of that operator (knob 4 + op, packed). SHIFT green/white/orange do nothing on a packed sound.
- **LFO decision: keep the packing.** TE's LFO only ever modulates the per-voice params copy `0xFF801724`, never the
  stored knobs (notes/knob-hook.md, Confirmed by reading all six LFO call sites). The hook edits and both screens read
  only the stored knobs (`KNOBS(slot)`, all 8), and harm's engine ignores params (it plays the table and unison/warp the
  screen publishes), so an LFO on knobs 1-4 does nothing on harm and can't corrupt its drawing. fm4 reads params: knobs
  0..3 are plain macros (an LFO on them modulates INDEX, ALGORITHM, TIME, FEEDBACK as for any engine); 4..7 (packed
  operators) are out of the LFO's reach (it targets knobs 1-4 only, device result of test image 57).
- **Hook**: `KNOB_HOOK id, id, ...` and `_hook_kind` (build_slot.py `HOOK_KINDS`); knobhook.c holds `harm_ui` / `fm4_ui`
  (.data) shared with the screens (weak in ui.c). harm: `harm_edit` then TE's apply with delta 0; fm4: `fm4_edit`,
  delta 0 to TE when the hook handled it, TE's own delta for plain MAIN knobs.
- **Delta per detent unknown**: `hook_steps.h` (any nonzero delta = at least one step; a multiple of the smallest
  |delta| seen since boot gives 2, 4, ... up to a cap: cursor, PAINT, STAMP, EXTEND, page, FINE, ENVELOPE 1; TILT,
  COARSE, op LEVEL 2; LEVEL, UNISON, WARP 4). `*_DELTA_SHIFT` stay 0 (the edit functions take steps). Diagnostic: grey
  `D<last raw delta> M<smallest>` under harm's readout and bottom right on fm4, once the hook has run.
- **harm table rebuild** (~4.3 ms, UI thread): at most one per `HARM_BUILD_GAP` = 4 screen frames (~0.13 s); edits in
  between coalesce into the next build, the last published table keeps playing (3 frames, crossfade). Host check
  (screens_preview.py `ratelimit`): a knob turned every frame for 61 frames builds 15 tables, the last edit is published
  3 frames after the turning stops.
- **fm4 screen: gears through the SVG route** (user design, 2026-10-14; second version after the user's feedback on
  image 59's line drawing: no output panel or shaft, bigger gears, TE-like SVG art). `design/synth/fm4.svg` (our art,
  `gen_fm4.py`; 30 elements, check_svg.py passes) holds one gear group per operator (12-tooth body filled black so
  overlaps stay clean, rim, three spokes, hub, axle, a selection ring), two belts and the feedback pawl (arm, pivot,
  spring, anchor). build.py adds it as `content/display/fm4.svg` whenever fm4 is in the image. ui_fm4.h:
  - The engine screen runs in the same screen-update context as the FX screens (`_ui_update` replaces iter's page
    update as `_fx_ui_update` replaces filter's), so the FX screens' route applies unchanged: lazy `svg_open` with an
    existence check, nodes found once with must = 0, per frame matrices / visibility / recolours, one `svg_render`, our
    text on top. Without the file it draws the same gears as 1-px polylines (also what screens_preview.py shows).
  - Layout: carriers packed left to right by size (a column as wide as its largest gear; more room where a gear stands
    between two carriers), modulators placed from the gear they drive along the algorithm's layout direction at a
    centre distance of both radii minus the bite; the algorithm 1 stack zigzags. The whole train (and the pawl) is
    scaled (at most 2.5x) and centred into x 36..284, y 41..137: under the readouts (now top left) and the HOLD/CHORD
    badges, labels beside it.
  - Per gear: matrix = translate(centre) x rotate(spin + jitter) x scale(radius / 20); body and hub strokes recoloured
    (carriers grey .. white by level, modulators dark blue .. blue by level x INDEX, the selected page white, its ring
    shown). Belts: a 100-unit line mapped onto the centre-to-centre segment with a unit normal. Pawl: D's translate and
    scale, then 0.7 .. 1.2x about the tooth contact with feedback, then up to 6 degrees of chatter about the pivot on
    alternate frames while a note sounds; offred below 1/8 feedback, else orange.
  - Unchanged: envelope-driven spin from the rendering voices, stillness in silence, bite and jitter, `ALG n` /
    `TIME xN` / operator-page values / v0 `DETUNE` (top left now), the D/M readout bottom right.
  - Host preview (ui.c + rsvg; preview.py now takes documents from design/fx and design/synth): about 30 SVG elements
    plus ~150 overlay segments. Unverified on device: the render cost, and whether TE's renderer scales stroke widths
    with the node matrix (rsvg does; gears span 0.5 .. 2.5x).
- Release builds (va, wavetable, grain): same patch sites and DB rows; the image bytes change because ui.c includes the
  two screens (as with sprobe in image 57).

## Proposed presets (all checked in the simulator: level 3000..26000 / 30000, cycles; values within the rows)
No other patch fields are needed (the template preset's ADSR, FX, LFO; never ADSR attack 0, LOG.md error 21).

harm (8 packed values; default `saw`):
| name | knobs |
|---|---|
| organ (H1 H2 H4) | 4096, 0, -3841, 0, 0, 0, 0, 0 |
| clarinet | 4104, 0, 3343, 2571, 1800, 1029, 515, 257 |
| hollow | 4096, 0, -16145, -24400, -28528, -32640, 28784, 24672 |
| bell | 4480, 128, -24561, 3072, 2304, 0, 7, 20480 |
| saw | 4128, 0, -17185, -26198, -30583, 30584, 26231, 26214 |
| square | 4108, 0, 3087, 2314, 2057, 1800, 1543, 1542 |
| choir | 4144, 0, -292, 27086, 292, 22099, 19, 0 |
| glass | 4116, 322, -8180, -16384, -24576, -28672, 28672, 24576 |

fm4 v0 (8 plain values 0..32767; default `e piano`):
| name | knobs |
|---|---|
| e piano | 11585, 13405, 6000, 0, 1536, 1536, 14848, 6000 |
| bell | 17947, 16384, 26000, 0, 3584, 7680, 11776, 20000 |
| bass | 13708, 1489, 3000, 8000, 1536, 2560, 1536, 0 |
| brass | 16384, 4468, 22000, 6000, 1536, 1536, 3584, 3000 |
| organ | 0, 31278, 16384, 0, 2560, 4608, 8704, 0 |
| pluck | 19386, 10426, 0, 10000, 1536, 3584, 1536, 0 |
| metallic | 20724, 22341, 16384, 0, 1536, 13824, 9728, 32767 |
| pad | 10362, 28299, 32767, 4000, 2560, 1536, 1536, 16000 |

fm4 packed (hook builds; per-operator levels and envelopes):
| name | knobs |
|---|---|
| e piano | 12691, 13405, 16384, 0, -31624, -31645, -31636, -18359 |
| bell | 19386, 16384, 16384, 0, -31620, -28965, -24638, -21455 |
| bass | 14654, 1489, 16384, 9000, -31624, -31638, -30639, -31647 |
| brass | 16384, 4468, 16384, 6000, -31624, -31633, -31521, -29627 |
| organ | 0, 31278, 16384, 3000, -31624, -30616, -28584, -24504 |
| pluck | 20724, 10426, 16384, 10000, -31621, -31647, -29599, -31662 |
| metallic | 21981, 22341, 16384, 0, -31621, -31381, -18718, -22573 |
| pad | 11585, 28299, 16384, 4000, -31505, -30361, -31642, -31265 |

## Decisions made here (beyond the design notes)
- harm EXTEND repeats bars 13..24 every 12 harmonics (parity-preserving), each repeat (16 - e) x 1.5 dB down; TILT is
  0.75 dB/octave per step (+-12 dB/oct).
- harm PAINT's brush is the level of the bar it starts on (stateless; equals "the last green level" right after a
  green turn). STAMP undo: turning SHIFT+white back to 0 restores the drawing; a LEVEL/PAINT edit keeps the stamp.
- harm's engine reads unison/warp from harm_ram (published by the screen), not params: the LFO does nothing on harm.
- fm4 carriers are scaled by 1 / (sum of carrier levels) once that passes 1: one carrier alone plays at full level.
- fm4 v0 detune: B x (1 + e), C x (1 - e), D x (1 + e/2), e <= 1 %. Packed mode = knobs 4..7 all negative.
- fm4 Tone limiter uses (I + 3) x f_m per modulator (Carson's I + 1 left -19 dB aliasing at C6 with a 24:1 pair).
- 11 algorithms (fm4.h); Operator's exact numbering was not verifiable, so these are TX81Z's 8 plus three.

## Open questions
- TE's encoder delta per detent and acceleration at the hook: the screens' `D`/`M` readout (image 59) answers it.
- The harm rebuild costs ~4 ms of one UI frame while editing, now at most every 4th frame; to be felt on device.
- harm's table is built only while its screen shows: a sound loaded from elsewhere (or another slot) plays the last
  built table, or a sine after boot, until the screen is opened (the SAMPLE bank's limitation).
- simrun.py / engine.h still say 44.1 kHz; the new checks use simdrive.py at 88.2 kHz. Fixing simrun.py's FS would
  shift va/wt/grain's absolute-frequency expectations, so it was left alone (this phase adds files only).
- wt.c and wt_import.h keep their own copies of wt_osc.h / wt_levels.h; switch them over when next touched.
