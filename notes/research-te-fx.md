# Research: PO-32 Tonic and EP-133 K.O. II effects as inspiration for new OP-1 FX (2026-10-18)

Research only, no code. Goal: new FX for the original OP-1 (firmware 246), the stereo master FX first.
Tags: **Official** = TE's guide or product page; **Designer** = the engine's author; **Community** = forum,
cheat sheet or review, not checked on hardware; **Inferred** = our reading.

## Answer first
- **PO-32's 16 effects are mostly not audio FX.** They change microtonic's engine parameters (pitch, decay, level,
  distortion) or the sequencer (repeat the step, triplet remap, half tempo), on all 8 drum channels at once.
  Held = momentary; in write mode they record per step. (Designer.)
- The list in the brief (loop 16 / 12, unison, octave, stutter, scratch, retrigger pattern) is the **PO-33 K.O!**'s,
  not the PO-32's. Both share "6/8 quantise" and "reverse". The PO-33 list is in §1b.
- **EP-133 K.O. II** has one shared send FX (delay, reverb, distortion, chorus, filter, compressor; 2 knobs each),
  a master compressor with note-triggered sidechain, **12 pressure-sensitive punch-in FX** (hold FX + pads, they
  stack), a 12-way assignable fader, tempo-locked LOOP mode and note repeat. (Official / Community.)
- **We already cover:** beat repeat, stutter and tape stop (`repeat`), bit and rate reduction (`crush`), drive
  (`distortion`), delay (`pingpong`, TE's delay / grid / cwo), chorus (`ensemble`), reverb (TE's spring), the static
  filter (TE's filter and nitro), compression (crush's SQUASH).
- **Gaps:** synced amplitude chopping (tremolo, trance gate, sidechain pump), synced filter movement on the master
  (it has no LFO), time remapping (half time, 6/8, swing), pitch moves (octave down, random pitch per step),
  bouncing-ball and echo-fade repeat shapes, a K.O.-style loop, and **a bank of momentary effects on the keys**.
- **Top picks:** 1 GATE, 2 repeat's BOUNCE / ECHO shapes, 3 GROOVE (half rate, 6/8, swing), 4 SWEEP, 5 PERFORM
  (punch-in bank; the most fun, the most work, staged behind two device probes).

## Sources
- [TE PO-32 guide](https://teenage.engineering/guides/po-32/en): the 16 effect names; hold FX + 1–16; recording and
  clearing.
- [Sonic Charge forum, "PO-32 synth engine FX"](https://soniccharge.com/forum/topic/893-po-32-synth-engine-fx):
  Magnus Lidström's (microtonic's author) exact definition of each effect, 2017-05-24. Our main PO-32 source.
- [TE PO-32 product page](https://teenage.engineering/products/po-32): "tweaking the internal parameters of
  microtonic in weird ways … chopping up the beat, making it sound like bouncing balls".
- [TE PO-33 guide](https://teenage.engineering/guides/po-33/en) and [lode/PO-33](https://github.com/lode/PO-33):
  the PO-33 list.
- [TE EP-133 guide: effects](https://teenage.engineering/guides/ep-133/effects): send FX, X / Y, master compressor,
  sidechain, live input; punch-in FX 2.0 named but not listed.
- [TE EP-133 guide: functions](https://teenage.engineering/guides/ep-133/functions): note repeat, loop, chop, timing
  correct.
- [TE EP-133 guide: what's new](https://teenage.engineering/guides/ep-133/whats-new): fixes naming FX+1 (beat
  repeat), tape-stop punch-in, punch-in ENTER, pad 0; FX dry / wet change.
- [EP-133 cheat sheet (ep133.joeyh.org)](https://ep133.joeyh.org/): the punch-in pad map with each pad's pressure
  direction; the 12 fader labels.
- [Gabriel Roth's K.O. II manual](https://gabrielroth.com/ko2/): fader, loop, time stretch, sidechain tricks.
- [Sound On Sound review](https://www.soundonsound.com/reviews/teenage-engineering-ep-133-ko-ii): punch-ins act on
  the whole performance, pressure, stacking; LOOP mode "a bonus"; one FX for all groups is the main limitation.
- [Soundtech's EP-133 guide](https://www.soundtech.co.uk/musicians-blog/our-guide-ep133-koii-sampler-composer-teenage-engineering.html):
  a second (partly garbled) pad map; HPF + LPF = band-pass.
- [Elektronauts, "EP-133 Tips and Tricks" p. 4](https://www.elektronauts.com/t/ep-133-tips-and-tricks/204124?page=4):
  first-hand descriptions; tape stop also slows MIDI clock out; punch-ins can't be motion-recorded.
- [n-drobyshevski/Arc PR #40](https://github.com/n-drobyshevski/Arc/pull/40): a third pad list from the printed labels.
- [TE OP-1 modules page](https://teenage.engineering/products/op-1/original/modules) and the 2013 OS post: stock
  OP-1 FX one-liners (CWO = a frequency-shifting delay after Lidström's Echobode).
- Our notes: fx-slot.md, fx-beatrepeat.md, fx-pingpong.md, fx-crush.md, fx-distortion.md, chord2.md §6, LOG.md 88–90.
- Not read: TE's EP-133 PDF (B&H link returns 403) and video breakdowns (no transcripts fetched). The EP-133 pad
  names below come from three community sources that agree on 9 of 12.

## 1. PO-32 Tonic
How it's played (Official / Designer):
- Hold FX and press 1–16. In play mode it is momentary: nothing is stored. In write mode the effect records on the
  steps it is held over, and the recorded effect sequence keeps playing. Hold FX alone in write mode to clear.
- Every effect acts on all 8 drum channels at once. Pitch moves shift osc, noise and EQ frequency together;
  attack / decay move inversely by 1/4 of the pitch change.
- Timing effects are tempo-synced (per step, 1/8 bar, a bar). Tempo 60–240 BPM, swing per pattern.
- Whether several effects stack at once: not documented.

| # | name | what it does (Designer) | kind | why it's fun / OP-1 audio analog (Inferred) |
|---|---|---|---|---|
| 1 | half rate | sequencer tempo / 2, pitch an octave down | sequencer + pitch | instant half-time drop. Audio: read a window at half speed (causal; see GROOVE) |
| 2 | distortion | distortion to max, level −1/8 | engine | covered by `distortion` |
| 3 | squash | noise and osc decay +1/4, level −1/16 | engine (envelope) | longer, washier drums. Audio: a transient shaper's sustain boost |
| 4 | echo fade | repeat the current step every 1/8 while level steps down then back up, cycling; attack / decay up slightly to smear | sequencer + level | a breathing dub echo locked to 1/8. Audio: a repeat shape (ECHO) |
| 5 | pitch lfo | triangle pitch mod, one cycle per 1/8 bar, ±1/8 of range | engine (pitch) | synced warble. Audio: synced vibrato via a modulated delay (WARP) |
| 6 | eq sweep | EQ frequency min → max over one bar; EQ gain 0 → +40 dB over the first two beats | engine (EQ) | a one-bar riser. Audio: a synced band sweep (SWEEP, RISER) |
| 7 | mega morph | like turning both knobs min → max over one bar: pitch −1/8 → +1/8, morph left → right | engine (macro) | the whole kit dissolves into its B sound. Audio: no direct analog (RISER is the nearest) |
| 8 | pitch bend up | pitch +2 octaves over 1/8 bar | engine (pitch) | laser zips. Audio: repeat's SPIN UP, or WARP |
| 9 | punch | more noise, shorter noise attack / decay, longer osc decay | engine | snappier hits. Audio: transient boost |
| 10 | 6/8 quantise | plays 3 of every 4 sixteenths, evenly spaced over the beat (the 1st, 2nd, 3rd; the 4th if the 3rd is empty) | sequencer | a straight beat turns into a triplet shuffle. Audio: a causal slice remap (GROOVE) |
| 11 | beat-repeat | repeat the current step every 1/24 bar | sequencer | triplet-16th rolls. Covered by `repeat` |
| 12 | beat-repeat faster | every 1/32 bar | sequencer | 32nd rolls. Covered by `repeat` |
| 13 | fm | osc mod = sine at full depth (±4 oct), ~200 Hz; attack +1/16, level −1/16 | engine (FM) | clangy metal. Audio: ring mod / cwo-like frequency shift |
| 14 | granular | retrigger each note 8 times ~11 ms apart, level −1/16 | sequencer | a buzzy flam on every hit. Audio: a short onset retrigger (GRAIN) |
| 15 | reverse | noise env exponential, all attacks +1/2 | engine (envelope) | fake reverse: swelling hits. Audio: an envelope inverter / swell |
| 16 | bouncing ball | repeat the step faster then slower across the bar; MIDI velocity −1/32 per hit in the first half, back to full in the second | sequencer | a ball dropping and rising; "used sparingly adds nuance, held down mangles the beat" (Community). Audio: a repeat shape (BOUNCE) |

Signature: everything is tempo-locked and bar-shaped (1/8 bar, one bar, half a bar), so a held key always resolves
musically. The effects are recordable per step, which turns them into parameter-lock-like variation.

### 1b. PO-33 K.O! (the list the brief named; Official names, Community descriptions)
loop 16, loop 12 (retrigger 1/16 and 1/12 segments), loop short (1/32), loop shorter (1/48), unison, unison low,
octave up, octave down, stutter 4, stutter 3, scratch, scratch fast, 6/8 quantise, retrigger pattern (randomly
rearranges the sequence), reverse (sequence in order, samples reversed), no effect. Same play model as the PO-32.
Ideas from it: loop 1/12 and 1/48 (triplet grids in `repeat`), scratch (a back-and-forth read of the slice), and the
retrigger pattern (a slice shuffle).

## 2. EP-133 K.O. II
### 2a. Send / master FX (Official)
One effect, shared by all four groups and the live input. Each group's send is set with the fader in FX mode
(recordable as automation). The mix then goes through the master compressor. OS 2.5: reverb, delay and chorus keep
more dry signal as wet rises. CC 12 / 13 map to X / Y.

| FX | knob X | knob Y | note |
|---|---|---|---|
| delay | length (time between repeats) | feedback | SOS: "particularly fun"; Soundtech: their favourite |
| reverb | length (room size) | colour (darker ↔ brighter) | |
| distortion | drive | colour (filter + resonance) | |
| chorus | modulation (rate) | feedback (prominence) | |
| filter | cutoff (left cuts highs, right cuts lows: one-knob DJ filter) | resonance | |
| compressor | drive (input level) | speed | |
| master compressor (SHIFT + FX) | drive | speed | always on the output |
| sidechain (SHIFT + FX, +) | duck length | duck shape | triggered by a chosen group's **notes**, not audio: it can pump without a sound |

No ranges are published. Two knobs per FX, no mix knob (the send is the mix).

### 2b. Punch-in FX 2.0 (Community pad map; Official: 12, pressure sensitive, combinable)
Hold FX and press pads while playing. Pressure sets the depth (the arrow says which way harder goes). They stack:
LPF + HPF = band-pass. While FX is held, the group pads are momentary solos and the fader still works. They act on
the whole performance; time-based ones also bend MIDI clock out. They can't be recorded as automation.

| pad | name (joeyh; others' labels) | pressure | what it does (Community / Inferred) |
|---|---|---|---|
| 1 | beat repeat (repeat) | harder = shorter repeat | grabs the current beat and repeats it; release = back in time |
| 2 | tape stop | harder = slower | the whole machine decelerates, MIDI clock included |
| 3 | lpf tremolo (filter LFO) | harder = faster | a synced LFO on a low-pass |
| 4 | lpf | harder = lower cutoff | momentary low-pass |
| 5 | hpf | harder = higher cutoff | momentary high-pass; with 4 = band-pass |
| 6 | master fx send (send FX) | harder = more send | throws everything into the current send FX (a "dub throw") |
| 7 | mute slicer (tremolo, slice) | harder = faster chop | a synced volume chop / trance gate |
| 8 | octave down | harder = back toward original pitch | drops an octave on the press, bends back up with pressure |
| 9 | bit redux (decimator) | harder = fewer bits | bit / sample-rate crush |
| . | pitch mod (pitch random) | harder = more randomness | random pitch per hit ("each drum hit pitched differently") |
| 0 | pad random (sample swap) | — | plays other pads' sounds in place of the programmed ones (fills, melody fragments) |
| ENT | stutter (granuliser) | — | a fast retrigger / granular stutter |

### 2c. Fader (Official / Community)
One fader per group, assignable by holding FADER + a pad. Labels: LEVEL (default), PITCH, TIME (playback speed /
stretch), LPF, HPF, →FX (send), ATK, REL, PAN, TUNE, VEL, MOD. Hold RECORD + move = automation per group; SHIFT =
fine. The "filter fader" is LPF / HPF per group: one knob, performance-sized.

### 2d. Other performance and sampling tricks
- **LOOP (SHIFT + TEMPO)**, after the OB-4's magic radio: loops the output at the playhead. X slides the loop
  through time, tempo-locked; Y sets the length. Hold SHIFT + TEMPO to set the length by hand. Exit at once or at the
  next bar. SOS calls it a bonus.
- **Note repeat** (TIMING + pad): 1, 2, 4, 8, 16, 32 per bar, 8T, 16T; pressure-sensitive velocity; latchable.
- **Resample** while the pattern plays (hands-free, bars set with − / +), time stretch to BPM or bars, equal-length
  auto chop, timing correct per pad.

## 3. What our OP-1 already has
| family | TE stock (246) | ours | covers from PO-32 / K.O. II |
|---|---|---|---|
| delay | delay; grid (grid echo); cwo (frequency-shifting delay) | pingpong (synced, ping-pong) | K.O. delay; part of PO-32 fm (cwo's shifting) |
| reverb | spring | — | K.O. reverb (roughly) |
| modulation | — | ensemble (CE-2) | K.O. chorus |
| drive | punch (Inferred: a drive / compressor) | distortion (5 characters, voice filter) | PO-32 distortion; K.O. distortion |
| lo-fi | phone ("hacked phone") | crush (rate, bits, SQUASH, GRIT) | K.O. bit redux, compressor (SQUASH) |
| filter | filter (hidden, we expose it); nitro (dynamic filter) | distortion's pre-filter | K.O. filter FX, static LPF / HPF |
| time | — | repeat (grid, chance, gate, pitch incl. SPIN UP / TAPE STOP; manual key) | PO-32 beat-repeat / faster / pitch bend up; K.O. beat repeat, stutter, tape stop |
- "terminal" and "mother" are not in 246 (fx-slot.md: no strings); they came later or belong to other products.
- **Not covered:** synced tremolo / gate / pump; a synced filter LFO or sweep on the master (the master has no LFO);
  half time, 6/8, swing; octave down and random pitch per step; bounce and echo-fade repeat shapes; a K.O. LOOP;
  momentary punch-ins on the keys; a send-style "throw".

## 4. Constraints used for every idea
4 knobs, no SHIFT layer; 64-frame blocks at 44.1 kHz, Q15 fixed point; 15–30 k cycles per stereo call (pingpong
16 k, ensemble 14 k, crush 11–31 k, distortion 29 k measured); RAM beyond the object only via pingpong's 256 KB
lines at `0x01E80000` (owner word, as repeat does) unless a new FREE region is claimed; tempo (samples-per-beat word)
and the bar phase (playhead, or the metronome countdown when stopped) as repeat uses them; the green encoder push
(0x3C) as a momentary key, **not yet proven on the hardware** (REPEAT2's probe is pending); TE's LFO reaches synth /
drum FX knobs, not the master. Every idea gives both the stereo (master) and mono (synth / drum) variant.

## 5. Ideas, ranked by fun ÷ effort
Effort in focused days, host / sim checks included, screen excluded unless said.

### 1. GATE: synced trance gate, tremolo and pump (K.O. mute slicer / lpf tremolo, sidechain; PO-32 echo fade's level steps)
- Pitch: the master gets a tempo-locked chopper and a kick-free sidechain pump.
- Knobs: blue RATE (1/32 .. 1 bar, with triplets); green PATTERN (16 step patterns: all, offbeat, 3-3-2, 2-1,
  Euclidean 5/8 and 7/16, a dub "1 and 3" pump …; the last detents are PUMP 1/4, PUMP 1/8, PUMP 1/2); white SHAPE
  (smooth tremolo → hard gate; in PUMP the release curve); orange DEPTH.
- Sound: stuttering pads, house pump on the whole tape, a dub chop on a synth.
- Play: set and leave; turn PATTERN on the bar; on synth / drum TE's LFO can sweep RATE or DEPTH.
- Screen: a 16-cell ring that lights the open steps with a hand sweeping on the bar.
- Effort / risk: 1.5 d, ~1–2 k cycles, no RAM; repeat's sync code reused. Low risk. Merges K.O. pads 3 and 7 and the
  sidechain.

### 2. Repeat shapes: BOUNCE and ECHO (PO-32 bouncing ball, echo fade; PO-33 loop 12 / 48)
- Pitch: two new FORTUNE modes for `repeat` rather than a new FX.
- Knobs: unchanged. BOUNCE: inside the gate, repeat intervals shrink, then grow (ball dropping, 1/8 → 1/64 → 1/8),
  level down then up, as Lidström's definition. ECHO: 1/8-spaced repeats, level stepping down then back up, a small
  smear. Add the 1/12 and 1/48 grids if missing.
- Sound: PO-32's two most characterful sequencer FX on any audio, the tape included.
- Play: as repeat today; the manual key gives them on demand.
- Screen: the wheel's slice ticks bunch up and spread out (BOUNCE) or fade (ECHO).
- Effort / risk: 1 d; slice scheduling only; no new RAM or CPU worth naming. Risk: the FORTUNE dial grows.

### 3. GROOVE: half rate, 6/8 and swing from audio (PO-32 half rate, 6/8 quantise; PO-33 6/8)
- Pitch: re-time the music, not the sequencer: half-time drops, triplet feel and swing on any audio.
- Every mode is **causal** (reads only the past), so no latency:
  - HALF: in each window W (1/2 bar .. 1 bar), output time t reads input W0 + t/2. Exactly PO-32's half rate
    (half tempo, octave down). Lag at the window's end = W/2.
  - 6/8: in each beat, output third k reads the k-th sixteenth onward at normal speed (start k/4 ≤ k/3, always
    past); the 4th sixteenth is dropped, as the PO-32. Short crossfades at the joins.
  - SWING: delay the off sixteenth by s; segments play at speed 1, the cut is crossfaded (no pitch change).
- Knobs: blue MODE (HALF / 6/8 / SWING / SHUFFLE (a random reorder of past slices, PO-33 retrigger pattern)); green
  AMOUNT (window, swing %, or the shuffle's odds); white SMEAR (join crossfade 2–40 ms); orange MIX.
- Sound: the most "how did it do that" item; a straight tape track turns into a triplet groove live.
- Play: latched; also a natural punch-in in PERFORM.
- Screen: two rulers, the input sixteenths above and where each lands below, joined by lines.
- Effort / risk: 3 d. ~3–6 k cycles (one interpolated read per channel in HALF, two in the crossfades). RAM: HALF at
  1 bar needs W/2 of history: 1 s stereo at 120 BPM = 176 KB, fits pingpong's 256 KB down to ~85 BPM (then a 1/2-bar
  window). Risk: the joins' clicks on sustained material; the owner word shared with pingpong and repeat.

### 4. SWEEP: synced filter LFO and one-shot sweeps (K.O. lpf tremolo, LPF / HPF pads, filter fader; PO-32 eq sweep)
- Pitch: a DJ filter whose movement is locked to the bar, for the master that has no LFO.
- Knobs: blue RATE (1/16 .. 8 bars; the top detents are ONE-SHOT 1 / 2 / 4 bars: a riser that ends on the bar line
  and snaps back); green SHAPE (triangle, saw up, saw down, square, random step, smooth random); white CUTOFF centre
  (left = LP, middle = open, right = HP, as the K.O.'s filter knob); orange RESONANCE / depth.
- Sound: wah-wah on 1/8, builds over 4 bars, random-step filter "talking" on a drum loop.
- Play: set and leave, or a riser per section. ONE-SHOT could re-arm on the green push.
- Screen: the filter curve drawn live with a phase dot running along a bar ruler.
- Effort / risk: 2 d; an SVF exists in distortion (~2–4 k cycles stereo). Risk: overlaps TE's filter and nitro
  (but neither syncs to the bar).

### 5. PERFORM: a punch-in bank on the keys (K.O. II punch-in FX 2.0, PO-32 / PO-33 FX + key)
- Pitch: the K.O. II's best feature on the OP-1: hold keys for momentary, stackable effects on the whole mix.
- The bank (12 effects, one per white / black key of an octave, K.O. order where it maps):
  1 REPEAT 1/16, 2 TAPE STOP, 3 FILTER LFO, 4 LPF, 5 HPF, 6 THROW (into a built-in synced echo), 7 GATE 1/16,
  8 OCTAVE DOWN, 9 CRUSH, 10 HALF, 11 6/8, 12 BOUNCE.
- **Pressure substitute:** the OP-1's keys have no pressure. The 24 keys give each effect two strengths: the lower
  octave is light, the upper octave hard (K.O.'s arrows: shorter repeat, slower stop, lower cutoff …). Pressing both
  = the hard one.
- Knobs: blue TIME (the grid / rate all synced punch-ins use, 1/32 .. 1/4); green DEPTH (scales every punch-in,
  the "pressure" master); white TONE (the THROW echo's feedback and colour); orange MIX (dry / wet, for parallel use).
- Play: momentary; several keys stack (LPF + HPF = band-pass, REPEAT + OCTAVE DOWN = pitched roll). The green push
  latches everything held (K.O.'s note-repeat latch idea). Everything starts on the next 1/16 and ends with a 32-sample
  crossfade.
- Screen: the 24 keys as on the CHORD page, each labelled with its effect icon; held ones fill; a strip shows TIME's
  grid and the bar.
- **Feasibility with our hooks:**
  - Synth: CHORD1's note-on CALL `0x019AC290` and HOLD's key-up `0x019AC3A6` are proven on the device; CHORD v2's
    press / release slots too. Swallowing a note-on and recording key-down / key-up into a 24-bit word the DSP reads
    is the same pattern as SPLIT + key in CHORD v2 and repeat's held flag (one word, atomic). **Feasible.**
  - Drum: the drum controller's press / release slots are hooked for repeat (`0x0190FA78` / `0x0190FA88`); its press
    handler treats codes < 0x36 as notes, so note keys pass through our hook first. **Feasible, unproven on device.**
  - Master (mixer): the mixer slots are hooked (`0x01910AC0` / `0x01910AD0`). **Unknown:** whether note keys reach
    the mixer controller and what TE does with them on the mixer screen. Needs a probe: count every key code there.
  - Gating: take keys only while the PERFORM screen of that context was drawn in the last 100 blocks (repeat's rule).
    Then no modifier is needed: on the screen the keyboard is the effect bank (the K.O.'s FX mode), elsewhere keys
    play notes. A modifier (green held + keys) would also work, if the push exists.
  - The green push: unproven on hardware (REPEAT2 pending). PERFORM must not depend on it; only the latch does.
  - CPU: only engaged effects run. Cap the stack (e.g. 4 at once; REPEAT / HALF / 6/8 share one read head, so only
    one of them sounds). Budget ~20 k worst case. RAM: the shared 256 KB, owner word.
- Effort / risk: 6–8 d staged. Stage A, no new hooks: a single "punch" FX where blue picks the effect and the green
  push (or TE's LFO, on synth / drum) engages it. Stage B: the key bank on synth and drum. Stage C: the master,
  after the mixer key probe. Risk: key routing on the mixer; a stuck effect on a missed key-up (clear on screen
  loss, as repeat does); the name `punch` is TE's, so call it `perform`.

### 6. LOOP: the K.O.'s magic radio (K.O. II LOOP mode, PO-33 loop 16 / 12)
- Pitch: catch the last bar of the master and slide through it in tempo-locked steps.
- Knobs: blue LENGTH (1/16 .. 1 bar); green SLIDE (where in the history the loop sits, in 1/16 steps back); white
  FADE (each pass quieter, 0 = infinite); orange ENGAGE zone (left = off, right = on; the green push = momentary).
- Sound: a DJ loop roll on the whole tape, then you slide it to the snare.
- Play: momentary or latched; drops out on the next bar after release (the K.O.'s MAIN exit).
- Screen: a bar-long waveform strip with a bracket that slides and shrinks.
- Effort / risk: 3 d. ~2–4 k cycles. RAM is the risk: 1 bar of stereo at 120 BPM is 352 KB, more than pingpong's
  256 KB. Options: mono-sum history (176 KB), cap at 1/2 bar below ~90 BPM, or claim a 1 MB FREE region
  (memory-map.md, canary regions; needs a check). Overlaps `repeat` (which loops a grid, not a slid window).

### 7. WARP: octave down, random pitch per step, synced vibrato (K.O. octave down, pitch mod; PO-32 pitch lfo, pitch bend up)
- Pitch: pitch moves on the master, locked to the grid.
- Engine: a two-head delay-line pitch shifter (crossfaded heads, as ensemble's interpolated taps).
- Knobs: blue SHIFT (−12 .. +12 semitones, OCT DOWN at the end); green RANDOM (a new random offset each 1/16 or 1/8,
  ± up to an octave); white WOBBLE (synced vibrato 1/8 bar, PO-32's pitch lfo, depth); orange MIX.
- Sound: detuned drums, warbly tape, a drunk random-pitch drum loop.
- Screen: a pitch trace over the bar, each step's offset a bar.
- Effort / risk: 3 d. ~6–10 k cycles stereo. RAM ~8 KB, the object's room or SDRAM. Risk: TE's `phone` (class
  `Ptch`) may already be a pitch FX: check before building; delay-line shifters sound grainy on sustained parts.

### 8. RISER: a one-shot build into the bar (PO-32 mega morph / eq sweep idea; K.O. tape stop as the release)
- Pitch: one gesture builds tension for 1–8 bars and drops dry on the downbeat.
- Knobs: blue LENGTH (1, 2, 4, 8 bars); green INTENSITY; white FLAVOUR (filter only → filter + gate doubling → +
  pitch rise → + noise swell); orange END (snap dry / tape stop / mute the last beat).
- Play: green push (or a key in PERFORM) arms it on the next bar; otherwise it re-arms every LENGTH (an LFO-free
  pump on the master).
- Screen: a progress arc filling toward a bar line, the drop marked.
- Effort / risk: 2 d after GATE and SWEEP (it drives their engines on a ramp). Needs the push or PERFORM to be
  playable rather than periodic.

### 9. GRAIN: onset stutter (PO-32 granular; K.O. ENT stutter)
- Pitch: every hit flams into a short buzz of retriggers.
- Knobs: blue SIZE (5–80 ms, PO-32 = 11 ms); green COUNT (2–16); white PITCH per grain (− / +, a scratch at the
  ends); orange MIX. Triggered by a transient detector, or on the grid when the detector is off.
- Sound: buzzy, granular drums; a smeared synth attack.
- Screen: hits on a ruler, each sprouting a comb of grains.
- Effort / risk: 2 d. ~3 k cycles; 32 KB of history. Risk: the onset detector on a full mix; overlaps repeat's small
  grids, so it ranks last.

### Not proposed
- A plate / FDN reverb (the K.O.'s reverb): spring and grid cover the space; an 8-line FDN is ~30–60 k cycles and
  100+ KB. Revisit only if the master needs a clean hall.
- Pad random / sample swap and mega morph: they need the sequencer or the engine's parameters, not audio.
- A send-style FX bus: the OP-1 has one master FX slot; PERFORM's THROW is the reachable part of it.

## 6. Ranking summary
| rank | idea | fun | effort (d) | main risk |
|---|---|---|---|---|
| 1 | GATE (gate, tremolo, pump) | high | 1.5 | none |
| 2 | repeat BOUNCE / ECHO | high | 1 | the FORTUNE dial's size |
| 3 | GROOVE (half rate, 6/8, swing) | very high | 3 | joins on sustained sound; shared RAM |
| 4 | SWEEP (synced filter) | medium-high | 2 | overlaps TE filter / nitro |
| 5 | PERFORM (punch-in bank) | highest | 6–8, staged | mixer key routing; the push unproven |
| 6 | LOOP (magic radio) | high | 3 | RAM for a full bar |
| 7 | WARP (pitch) | medium | 3 | may duplicate `phone`; grainy |
| 8 | RISER | medium-high | 2 after 1 + 4 | needs a trigger |
| 9 | GRAIN | medium | 2 | onset detection; overlaps repeat |

Next steps if wanted: extend REPEAT2's KEYS probe to count every key code on the mixer (unblocks PERFORM stage C),
and read REPEAT2's result for the green push before relying on it anywhere.
