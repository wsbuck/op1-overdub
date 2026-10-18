# Beat repeat FX (`repeat`, firmware id 12, DB fx_types 15; screen: the Wheel of Fortune)

**Status (2026-10-18): phase 0 (the probe), 1 (the MVP) and 1b (the screen) built and sim-checked, with VARY's
per-repeat grids, the real TAPE STOP and the coupled tone from phase 2b; on the device as test image REPEAT1 (LOG.md
88), not yet tested there. Not built: the manual SPLIT key (phase 2), the sequencer-start hook, drum / master keys
(phase 3). "As built" (§7) supersedes the design where they differ.** Source: `src/slot/repeat.c`, `repeat.h`,
`ui_repeat.h`; the screen `design/fx/wheel.svg` from `design/fx/gen_wheel.py` (run it with a directory to get the
mockup gallery). FX contract: fx-slot.md; tempo: fx-pingpong.md
"Tempo". Tags: **Verified** = read at the call site in the disassembly, **Confirmed** = read in code or decompile,
**Hypothesis** = inferred, **Memory** = from memory of Live, not checked against a source.

## Answer first
- An Ableton-style beat repeat fits the FX slot: tempo-synced slices, chance, gate, pitch per repeat. The OP-1
  version has four knobs (after review round 1):
  - **GRID** (blue);
  - **FORTUNE** (green): the latch at the hard-left stop, then OFF, EVERY 8 / 4 / 2 bars, CHANCE, EVERY BAR, VARY;
  - **GATE** (white): 1/16 .. 4 bars, ending on the bar line;
  - **PITCH** (orange, bipolar): SPIN UP, +12 .. +1, FLAT, -1 .. -12, TAPE STOP. It is the total reached at the
    gate's last repeat.
- **Bar phase exists:** the tape playhead `0xFF802974` (44.1 kHz samples × 64, written by the audio thread before the
  FX run) on TE's 4/4 bar grid from tape position 0. When the tape is stopped, the metronome's countdown gives the
  beat and we count bars ourselves. Details in "Sync".
- **Manual REPEAT:** hold SPLIT (scissors) in synth mode. It uses the CHORD v2 press / release slots, already
  proven on the device (NIGHT1 / NIGHT2), and SPLIT is unused by TE on the synth screens. The repeat locks to the
  next 1/16 and repeats the slice just heard.
  - Drum and master need their own controller hooks (not traced).
  - On the master, green's hard-left ALWAYS is the latch, one detent from OFF.
- **Memory:** pingpong's lines at `0x01E80000` (2 × 128 KB stereo, 128 KB mono), shared through an owner word. The
  line records while dry, so the slice is already in it at a trigger.
- **CPU:** about 4–8 k cycles per stereo call (Hypothesis), under pingpong's measured 16 k.
- **The master FX is the killer use:** fills and stutters on the whole mix, tape included. There is no LFO there,
  and in the MVP no manual key.

## 1. Research: Live's Beat Repeat
Sources:
- [Ableton Live 12 manual, Audio Effect Reference, "Beat Repeat"](https://www.ableton.com/en/manual/live-audio-effect-reference/);
- [Sound On Sound, "Ableton Beat Repeat Plug-in"](https://www.soundonsound.com/techniques/ableton-beat-repeat-plugin);
- [MusicRadar, "How to use Ableton Beat Repeat to keep your loops interesting"](https://www.musicradar.com/how-to/ableton-beat-repeat-loops-interest);
- [frederickk/b-b-b-b-beat GUIDE](https://github.com/frederickk/b-b-b-b-beat/blob/primary/GUIDE.md) (a norns clone, reverse-engineered by ear);
- [OBEDIA guide](https://obedia.com/how-to-use-the-ableton-live-beat-repeat-audio-effect/);
- [CDM on Beast Repeat](https://cdm.link/free-beast-repeat/) (a Max for Live extension).

### Parameters
| parameter | what it does | range |
|---|---|---|
| Interval | How often new material is captured, tempo-synced | 1/32 .. 4 bars (manual) |
| Offset | Moves the capture point within the Interval, in sixteenths. Interval 1 bar + Offset 8/16 = capture on beat 3 (manual) | 0/16 .. 15/16 (clone; Memory) |
| Grid | The size of each repeated slice, so also the repeat rate. Large grids give rhythmic loops, small ones "sonic artifacts" (manual) | 1/32 .. 4 bars with triplets (Memory: down to 1/48 / 1/64 unsure) |
| No Triplets | Binary grid only. A triplet value is replaced by the next lower straight one, including values set by Variation (SOS) | toggle |
| Variation | Random grid changes. 0 = fixed; higher = larger fluctuation (manual) | 0 .. 10 (clone; Memory) |
| Variation mode | Trigger: a new variation when a repetition is triggered. 1/4, 1/8, 1/16: at those regular intervals. Auto: after each repetition (manual) | |
| Chance | Probability that a repetition happens when Interval / Offset call for it. 100 % = always (manual) | 0 .. 100 % |
| Gate | Total length of all repetitions, in sixteenths. **No effect unless Gate > Grid** (SOS) | 1/16 .. 4 bars (Memory) |
| Repeat | A button: bypasses Interval / Offset / Chance and repeats until released (manual) | momentary / latch |
| Pitch | Repetitions pitched **down by resampling**, which lengthens the segments (manual) | 0 .. −48 st (Memory) |
| Pitch Decay | Tapers the pitch curve: each slice plays lower than the one before (manual) | 0 .. 100 % (Memory) |
| Filter | A combined LP / HP (band-pass) on the repeats only, with frequency and width (manual, SOS) | on / off, freq, width |
| Mix mode | Mix: dry plus repeats. Insert: dry muted while repeating. Gate: only the repeats (for return tracks) (manual) | 3-way |
| Volume | Level of the repeats only (SOS) | |
| Decay | Each repetition quieter than the last (SOS's example: −2 dB on the first, then 33 % drops) | 0 .. 100 % (Memory) |

### How it behaves
- Capture is forward-looking. At the trigger it records the next Grid of input while that audio plays through, then
  loops it for the rest of the Gate. The first slice is always the live one (manual; SOS's description).
- Gate ends the effect, and Interval spaces the triggers. With Gate shorter than the Interval, dry audio returns
  between events.
- Classic uses (SOS, MusicRadar, common practice):
  - **Fills:** Interval 1 or 2 bars, Offset late in the bar (12/16), Gate 4/16, Grid 1/16 or 1/32, Insert. The
    stutter lands in the last beat before the downbeat.
  - **Stutters / rolls:** small grids (1/32, 1/64). At audio rate the slice becomes a buzz with a pitch of its own
    (1/64 at 120 BPM = 32 Hz).
  - **Tape-stop drops:** Pitch Decay high plus Decay. Every repeat lower and quieter.
  - **Buildups:** Grid automated smaller across 4 or 8 bars, often with pitch rising (which Live can't do: its
    Pitch only goes down; producers automate a pitch shifter after it).
  - **Variation for glitch:** Chance 30–60 % with Variation on Auto.
  - **Parallel instances:** MusicRadar's rack, four Beat Repeats with Offsets 0, 4, 8, 12/16 at Chance 50 %, for
    occasional double hits.
  - **Repeat button:** live stutters with Chance at 0.
- Fun: Grid, Chance, the Repeat button, and the pitch drop. Each changes the groove at once.
- Fiddly: Interval vs Offset vs Gate (three time values that must agree: "Gate > Grid or nothing happens" is the
  classic confusion), the Variation mode menu, and the filter's tiny graphic. Most producers set Interval / Offset
  once per use and play Grid, Chance and the button.

## 2. Mapping to the OP-1

### Knobs (33 detent positions per knob, 1024 raw each; after UX round 1)
The maps are coded in `design/fx/gen_wheel.py` (`grid_of`, `fortune_of`, `gate_of`, `eff_gate`, `pitch_of`,
`vary_pick`), the reference for the C.

| knob | role | detent positions |
|---|---|---|
| **blue GRID** | The slice. Straight divisions are 4 positions wide and triplets 2, so No Triplets is a matter of landing (as crush's plateaus) | 1/64 0–2, 1/32T 3–4, 1/32 5–8, 1/16T 9–10, **1/16 11–14**, 1/8T 15–16, 1/8 17–20, 1/4T 21–22, 1/4 23–26, 1/2T 27–28, 1/2 29–32 |
| **green FORTUNE** | When it fires. The latch is on the hard-left stop, the easiest place to hit blind and one detent from OFF. From OFF on, clockwise = more often, then more random | 0 **ALWAYS** (latch; engages on the next 1/16) · 1–2 **OFF** (the key only; drum / master: `OFF NO KEY`) · 3–4 EVERY 8 BARS · **5–6 EVERY 4 BARS** · 7–8 EVERY 2 BARS · 9–17 CHANCE 10 .. 90 % per bar · 18–20 EVERY BAR · 21–32 VARY a = 1..4 (3 positions each) at 50 / 67 / 83 / 100 % |
| **white GATE** | How long, in sixteenths, ending on the bar line of the interval's last bar (Offset = interval − gate). A gate longer than the interval merges the events into one | 1/16 0–2 · 2/16 3–5 · 3/16 6–8 · **4/16 9–11** · 6/16 12–14 · 8/16 15–17 · 12/16 18–20 · 1 BAR 21–23 · 2 BARS 24–26 · 4 BARS 27–32 |
| **orange PITCH** | Bipolar. S = semitones reached at the gate's last repeat. Repeat k of N plays at S × k / (N − 1), a staircase. With the manual key and ALWAYS, S is reached after one bar and then holds | 0–2 SPIN UP · +12 3–4 · +7 5–6 · +5 7–8 · +3 9–10 · +2 11–12 · +1 13–14 · **FLAT 15–17** · −1 18–19 · −2 20–21 · −3 22–23 · −5 24–25 · −7 26–27 · −12 28–29 · 30–32 TAPE STOP |
- **Default `[12288, 5120, 9216, 16384]`:** 1/16, EVERY 4 BARS, GATE 4/16, FLAT. That is a one-beat sixteenth
  stutter into every fourth downbeat: predictable, and musical the moment it is dropped on the master.
- **The grid wins:** the gate stretches to at least two slices (the live capture plus one repeat): `gate_eff =
  min(4 bars, max(gate, 2 × grid))`, still ending on the bar line. So every blue detent is audible. For example,
  1/2 with GATE 4/16 plays a 1-bar gate. The screen shows the stretch in lpg (dim white). VARY's picks are limited
  to half the gate.
- **VARY:** each repeat after the live slice draws its grid within ± a places of blue's division in blue's own family
  (straights only, or triplets only, like Live's No Triplets with Variation). It is Live's Auto mode, with Chance
  rising together with the amount (Live's glitch recipe: Chance 30–60 % plus Variation).
- **TAPE STOP is a real tape stop:**
  - it ignores the grid and reads on from the gate start, decelerating (rate 1 → 0 across the gate), while the line
    keeps recording;
  - the read lags the write by up to half the gate, which fits the line (1.486 s) for a 1-bar gate down to 80.8 BPM;
  - longer, or slower tempos: the looped version (the first slice at a falling rate);
  - it fades over the gate's last 15 %.
  **SPIN UP** (1 → 2× across the gate) loops the slice, since it cannot read ahead. An upward step wraps within the slice, so
  rolls speed up as they rise: a buildup.
- **Coupled tone (UX "could", adopted as cheap):** going down, a one-pole low-pass closes with the depth (to about
  1.5 kHz at −12 st or TAPE STOP's end) and the last repeat fades by up to −6 dB (Live's Pitch Decay + Decay
  tape-stop recipe). Going up, a one-pole high-pass opens (to about 600 Hz at +12). A few cycles per sample, no knob.
- **Mix: Insert, always:** the dry signal is muted while the repeats play. Volume is unity. Every slice boundary and
  gate edge crossfades over 32 samples (0.7 ms).
- Grid and pitch changes (knob or LFO) take effect at the next slice boundary, never mid-slice. Counts are kept in
  samples, not bar phase: a gate ends when its sample count runs out. A tape loop, jump or stop inside a gate cannot
  strand the freeze, and a backward jump across the trigger point does not retrigger within the same gate.
- SHIFT + knob (knobs 4–7) is TE's fine adjust, 1/16 of a detent (`delta >>= 4`). On these stepped knobs it only
  moves within a step, so it seems dead. Accepted.
- **Dropped, and why:**
  - **Interval / Offset:** EVERY 2 / 4 / 8 and the gate ending on the bar line cover the uses (the fill before the
    phrase's downbeat). A free Offset would be a third time control, the fiddly part.
  - **Mix / Gate modes:** the OP-1 has no send buses, so Gate mode has no use. Mix mode doubles the transient,
    which fits rarely.
  - **Filter and Decay as knobs:** they come back coupled to PITCH.
  - **Volume:** unity.
  - **Variation modes:** only Auto.
  - **Live's static Pitch:** PITCH's total covers it.

### Sync (static analysis 2026-10-17, subagent; Ghidra decompiles in the session scratchpad)
| address | meaning | writer | tag |
|---|---|---|---|
| `0x01AADAD8` | samples per beat, Q14 (pingpong's word) | tempo.cpp (UI) | Verified |
| `0x01AADADC` | metronome countdown to the next click, Q14 samples. The reload keeps the residue (sample-accurate) and is rescaled on a tempo change | metronome `FUN_019de548` (audio) | Verified |
| `0x01AADAD2` (B) | play latch: on the first block of tape play / record, the countdown is set to 0 (a click at once) | metronome | Verified |
| `0x01AADABC` .. | click level, volume, pitch: **every click is the same: no accent, no beat-in-bar index, no time signature** | metronome | Verified |
| **`0xFF802960`** | tape transport: 0 stop, 1 record, 2 play, 3 FF / REW | TapeController `FUN_01a581f4` | Confirmed |
| **`0xFF802974`** | **tape playhead, samples × 64 (Q6, unsigned), 0 .. 6 min** | L1 tape engine `FUN_ffa067a8`, called at `0xFFA01ACA` in the audio callback, before the FX (by address order: Hypothesis) | Verified |
| `0xFF802968` / `0xFF8029D0` | tape speed / its target (varispeed, FF / REW) | tape engine / TapeController | Hypothesis for the 1.0 value `0x08000000` |
| `0x018C1884` | sequencer 96-PPQN tick, mod 48 (half a beat); reset by `FUN_01a5e7d0` at every sequencer start, tape key and MIDI Start | PlayerManager `FUN_01a0d2c0` (sequencer thread) | Verified |
| `0x01B60524` | samples per 96-PPQN tick, Q18 | tempo.cpp | Verified |
| `0x01AADAF4` | free-running block counter (+1 per callback, no readers) | metronome | Verified |
- **Bars are TE's tape grid:** 4 beats from tape position 0, fixed 4/4.
  - Stop-with-snap rounds the position to `10584000 / (bpm × {1,2,4,8})` from 0 (`FUN_01a5715c`, Confirmed).
  - MIDI Start seeks the tape to 0 (or the loop start) and resets the ticks (Verified).
- **The metronome is not on the tape grid:** it restarts at play wherever the playhead is (Verified). So only the
  playhead gives bars.
- **Plan (per block, audio thread, one udiv):**
  1. **Tape rolling** (state 1 or 2):
     - `pos_q8 = [0xFF802974] << 2` (< 4.07e9, fits uint32), `bar_q8 = ([0x01AADAD8] >> 6) × 4`;
     - bar phase = `pos_q8 % bar_q8` (Q8 avoids the overflow of 4 × spb Q14 below ~41 BPM);
     - a trigger is the sample in this block where the phase crosses `bar × (16 − gate) / 16`.
     The repeats then follow the tape's bars, loops, snap and MIDI Start.
  2. **Tape stopped** (0 or 3):
     - the beat comes from the metronome countdown (the click you hear);
     - bars are our own count of clicks mod 4, continuing from the last tape phase.
     - Optional alignment to the sequencer: re-zero when `0x018C1884` jumps back to 0 (it misses about 1 in 16
       starts that fall on tick 0). Phase 2 can hook `FUN_01a5e7d0` (the sequencer start) to bump a start epoch:
       exact.
  3. **Invalid tempo word:** pingpong's rule (the last valid word, 120 BPM before any).
  - **Varispeed:** with a varispeed tape the bar grid follows the tape, while slice lengths follow the tempo word.
    They drift as pingpong's echoes do (Hypothesis, accepted).
- **Device probe** (one diagnostic line on our FX screen, phase 0):
  - `[0xFF802960]`, `[0xFF802974] >> 6`, `[0xFF802968]` through stop / play / rec / FF / varispeed; expect 0/2/1/3,
    +44100 per second, `0x08000000` at 1.0;
  - with snap on, stop off-grid: `pos % (10584000 / bpm) == 0`;
  - `0x018C1884` on a pattern start and on MIDI Start.

### Triggering
- **Automatic:** green 3–32 (at the gate's start, sample-accurate within the block). EVERY n counts bars from tape
  position 0. CHANCE and VARY roll an LCG once per bar at the bar line. With the tape rolling, the seed comes from the bar number, so a song plays
  the same fills each time from the same spot. Open question below.
- **Manual REPEAT = hold SPLIT** (synth mode, any synth page; the CHORD page keeps SPLIT for its pin):
  - Hooks: the synth controller's press slot +0x14 (`0x019119A8`) and release slot +0x24 (`0x019119B8`). We own
    both since CHORD v2 (Verified, device-proven NIGHT1 / NIGHT2).
  - SPLIT (key 0xC) returns 0 from TE's press function on the synth screens: unused by TE (Verified, chord2.md §2).
  - The press sets a byte `br_manual` (KeyHandler writes, the audio thread reads: one aligned byte), only when all
    hold: no SHIFT, the CHORD page closed, the current synth slot's FX is ours (`0xFF801734[slot]` == 12), and the
    FX is on (`0x01AAAC20[slot]`). The release clears it. Otherwise TE's function runs as stock.
  - **Fail-safe:** the frame hook clears it when the current controller is not the synth controller (a mode switch
    while held, so a release that never comes). Our FX screen draws it (the lit hub).
  - **Behaviour (UX round 1):** the press locks to the 1/16 grid. Within 30 ms after a 1/16 line it starts at once;
    otherwise it waits for the next line (at most 125 ms at 120 BPM). The slice is the last grid of audio ending at
    that line (the line records while dry, so it is already there). That way a staccato note is repeated, not the
    silence after it. While held, the repeats use blue's grid and orange's pitch (S reached after one bar); GATE is
    ignored. Release goes back to dry within one crossfade, at once. ALWAYS engages the same way, at the next 1/16.
  - Overrides FORTUNE while held, so OFF (green 1–2) + SPLIT = Live's "Chance 0 + Repeat button".
  - **Needs the press / release hooks installed in builds with this FX, not only with `--chord`**: build_slot.py
    change. In apply.py's mask the hook must still call TE's function for every other key.
- **Drum mode:** the drum controller is a different vtable (not traced). A key there needs a probe (phase 3).
- **Master:** MasterController / tape controller. On the tape screen SPLIT is TE's cut, so no. A mixer-screen key:
  not traced (phase 3). Until then the master has the automatic triggers and green's hard-left ALWAYS as a latch: one
  detent from OFF, so it is turned in and out with a flick.

### Synth / drum vs master
- **Master (stereo):** the strongest case. Fills on the whole mix including the tape, the DJ-style stutter
  (Live users put Beat Repeat on groups and the master for that). Limits:
  - no LFO (TE's LFO cannot target the master FX, fx-slot.md "LFO on our FX");
  - no manual key in the MVP: ALWAYS is the latch;
  - with the tape rolling the fills land on the tape's bars.
- **Synth FX (mono):** a playable stutter on one sound, with the SPLIT key and TE's LFO:
  - an LFO on blue sweeps the grid (a buildup with a rising LFO);
  - on green, fortune breathes;
  - on orange, the pitch swings.
  The LFO writes once per block; the DSP applies it at the next slice boundary.
- **Drum FX (mono):** the classic drum stutters / fills. Automatic triggers only, until a drum key is found.
- The tape records the synth / drum FX output (mono), so a stutter played live is printed to tape.

### Memory
| what | where | size |
|---|---|---|
| state (×2: mono, stereo; both can run at once) | slot block `.data` | ~64 B each |
| stereo lines L, R | pingpong's `0x01E80000`, `0x01EA0000` | 65536 int16 each (1.486 s) |
| mono line | pingpong's `0x01EC0000` | 65536 int16 |
| owner word per line set | slot block `.data` | 2 × 4 B |
- **Sharing pingpong's lines is safe:**
  - there is one master FX, so master pingpong and master repeat never run together;
  - mono: only the current play mode's FX object is live (fx-slot.md), so one mono FX runs at a time.
  - Each FX sets the owner word on its first block. When it finds another owner, it resets its `filled` count (the
    stale-read guard both already have). Cost: pingpong's echo tail is lost after the other FX used the lines
    (accepted; stock FX are rebuilt on every slot change anyway). One small pingpong change: it must honour the
    owner word.
- **Other FREE RAM** (memory-map / build_slot reservations):
  - region 0 has 32 KB left (`0x00818000`–`0x00820000`);
  - region 1 has the slot block's tail up to `0x01E80000` (its size with CHORD v2 must be measured);
  - region 2 is full (wavetable).
  None of them holds 256 KB, so a separate stereo buffer is not possible.
- **Capture rule:** the line records while dry. An automatic trigger captures forward (the first slice plays live, as
  Live does). The manual key and ALWAYS take the grid that ends at their start line (backward). Either way the writes
  stop for the repeats (a freeze), so the line only needs one slice, and the gate or the key can last any time.
  TAPE STOP is the exception: it keeps writing and reads behind the write.
- **Slice lengths:**

| grid | 120 BPM | fits down to | below that |
|---|---|---|---|
| 1/2 | 1.0 s (44100) | 80.8 BPM | plays 1/4 (on the grid; "1/2 TOO LONG", as pingpong) |
| 1/2T | 0.667 s | 53.9 BPM | 1/4T |
| 1/4 | 0.5 s | 40.4 BPM | 1/8 (40.0 .. 40.3 BPM only) |
| 1/16 | 125 ms | all | |
| 1/64 | 31 ms (1378) | all | |
- **Pitch and memory:** a slower read (a downward PITCH, TAPE STOP) plays the start of the slice over one grid period, as
  Live's resampling does. A faster read (an upward PITCH, SPIN UP; up to 2×) wraps within the slice. No extra memory either way.
- **Stale SDRAM:** a slice read before it is written (the manual key on the very first block) reads 0, through
  pingpong's `filled` guard.

### CPU (Hypothesis; measure in the simulator as for pingpong)
- Per stereo frame:
  - when not repeating: 2 line writes plus the crossfade check;
  - when repeating: 2 linearly interpolated reads (Q16 read position), the gain ramp near boundaries.
  - About 30–60 cycles per frame in C with GCC 4.3 (pingpong's heavier loop, with 2 taps, tone, clip and mix, measured
    253).
- So 2–4 k cycles per stereo call (block math, the bar phase udiv and the LCG included), or 4–8 k with SDRAM latency.
  For comparison: pingpong 16.2 k, ensemble 14.3 k, crush 11–31 k, distortion 29 k. Mono about half. The coupled one-pole tone adds ~10 cycles per
  frame while repeating.
- The read rate per slice: 2^(S·k / (N − 1) / 12), computed once per slice boundary (a table of 2^(n / 192), 1/16-semitone steps,
  385 Q15 entries over ±12 st); the ramps (TAPE STOP, SPIN UP) step the rate once per 8 samples.
  No division in the sample loop.

## 3. Screen: the Wheel of Fortune
### The card
- **Rider–Waite–Smith** (Pamela Colman Smith for A. E. Waite, Rider 1909/1910; public domain): the great wheel.
  - The rim carries T-A-R-O clockwise (also ROTA, TORA) and, between them, the Hebrew YHVH.
  - The inner wheel has the alchemical signs of mercury, sulphur, salt and water (Aquarius), with eight spokes.
  - A blue **sphinx** with a sword sits on top; a yellow **snake** (Typhon) descends on the left; a red
    **Anubis / Hermanubis** rises on the right.
  - Four winged creatures read books in the corners on clouds: angel, eagle, bull, lion (the evangelists, the fixed
    signs). The sky is blue.
  - Sources: [Wikipedia, "Wheel of Fortune (tarot card)"](https://en.wikipedia.org/wiki/Wheel_of_Fortune_(tarot_card));
    Waite's *Pictorial Key to the Tarot* (1910).
- **Tarot de Marseille:** a wooden wheel on a stand with a crank and no letters, three animal figures (one
  climbing, one falling, a crowned one with a sword and wings on top), and no corner creatures.
- **Ours:** the RWS layout as flat TE-style vectors, drawn from scratch. Simplified figures, no lettering style or
  linework copied from any deck.
  - Kept: the wheel with T-A-R-O on the rim, the sphinx with its sword on the crown, the snake descending left,
    Anubis rising right, the four winged creatures with books in the corners.
  - Dropped: the Hebrew letters (four small diamonds instead), and after UI round 1 the alchemical symbols (clutter
    at 1×, moiré at 1/64). The clouds went in round 1 (mud) and came back in round 3 as scalloped offblue cushions
    with a thin grey edge, drawn over the creatures' feet.
  - Figures (round 3, after the user's "the guys on the outside need work"): built from a few shared shapes (a
    feathered wing of five primaries with purplegrey coverts, the scalloped cloud, a three-face closed book / an open
    green one), neutral palette (wings grey, bodies purplegrey, faces lpg, details darkblue / black).
    - Sphinx: striped lpg nemes (darkblue bands), purplegrey face with eyes, the brow band and the false beard, a grey
      pectoral collar, a grey lion body edged purplegrey crouched along the crown (low haunch, short tail), the left
      forepaw on the rim, the right paw a fist over the sword's grip at the pivot; the sword has a pommel.
    - Angel (robe with folds, hair, arms holding the book), eagle (standing on its book, hooked beak, frown, tail),
      bull (lying, horns, ears, muzzle, the book before it), lion (lying, a 16-point mane, face, paws, the book in
      front, a tufted tail); all winged.
    - Snake: three waves, tapering 5.9 -> 0.4, a head wider than the neck, an eye and a forked tongue.
    - Anubis: in profile toward the wheel, jackal head with two tall ears and a long snout, lpg collar, purplegrey
      kilt, striding legs, an arm reaching for the rim; the body, head and arm turn orange.

### Mapping (after UI round 1; each knob's effect in its knob's colour; structure in darkblue / offblue / grey / purplegrey)
| state | image | colour |
|---|---|---|
| GRID | the spokes: slices per bar of the grid played (1/2 = 2 .. 1/32 = 32); 1/32T and 1/64 show no spokes, one / two blur rings (the buzz) | blue |
| the repeats | the wheel (rim, TARO, diamonds, spokes) steps 360 / slices per repeat; a **lightblue lead spoke** makes the steps countable on symmetric spokes; TAPE STOP / SPIN UP turn it continuously at the read rate (a dying / speeding turntable) | lightblue |
| GATE | the static ring inside the rim is the bar as a clock face (12 o'clock = the bar line, ticks on the beats); the gate's sixteenths lit white, ending at 12; the grid-wins stretch in lpg (thinner); 2 BARS / 4 BARS add one / two inner white laps; ALWAYS and the key: no gate (dim ring only) | white |
| bar phase | a radial tick (lpg core on an offblue halo, so it shows on white and on the dim ring) orbits once per bar | |
| FORTUNE | 12 lots in the hub: chance × 12, EVERY n = 12 / n, ALWAYS all, OFF none; FX off: none | green |
| the roll | the sphinx raises the sword in its right paw (rotates up from resting on the rim's crown over 3 frames) when the bar fires, and holds it to the gate's end; a lost roll blinks the lots out for ~4 frames | green |
| VARY | the creatures open their books, in the order bull, lion, eagle, angel (the angel is the one the readout covers); a page turns when a repeat drew another grid | green |
| ALWAYS | sword up, every lot and book lit | green |
| PITCH down | the snake (a tapered body along an arc left of the rim) turns orange and the wheel carries it down (rotation about the centre) with the current repeat's drop (the floor at −12 st or TAPE STOP's end); dots outside its arc show the setting (−1 −2 −3 −5 −7 −12 → 1 .. 6 dots) | orange |
| PITCH up | Anubis (jackal head, tall ears, long muzzle, tapered torso) turns orange and climbs upright along an arc right of the rim | orange |
| manual REPEAT | the hub fills green while SPLIT is held (the key is FORTUNE's) | green |
| FX off | lots and books out, the sword rests: fortune is off | |
| readout | a stored-knob turn shows the value ~1.5 s at (8, 7) in the knob's colour (`GRID 1/16T`, `EVERY 4 BARS`, `CHANCE 30%`, `VARY 2 67%`, `ALWAYS`, `OFF` / `OFF NO KEY`, `GATE 3/16`, `GATE 2 BARS`, `PITCH -7 ST`, `TAPE STOP`, `SPIN UP`, `PITCH FLAT`); the angel steps aside meanwhile (crush's rule: never from the LFO alone). When the grid wins, the GRID readout names the grid played | knob's |
- Mechanics: no recolours at run time. Changes are visibility flags (prebuilt lit / unlit variants) and group
  matrices: the wheel's rotation, the playhead, the sword's raise, the snake's rotation, Anubis's translation. The
  SVG header lists every id; `runtime_state()` in the generator is the reference for ui.c.
- Renderer rules:
  - 222 drawable elements (121 in the figures, round 3), each one subpath of absolute M / L / C / Z; check_svg PASS;
  - the heaviest element is the rim band's stroke, ~1331 cells (35 % of 3840; the heaviest figure element is the
    lion's cloud, ~1092, 28 %), worst over every gallery state and
    animation frame;
  - drawn per frame: ~60–100 elements before round 3, ~126 after (default state; ~21 k cells).
  - Per frame the C does matrices and flag writes only (a few thousand cycles). Render cost is like pingpong's /
    choir's (2–11 M cycles; Hypothesis until `--svg-cost`).
- Gallery: the session scratchpad `beatrepeat/index.html` (19 states at 1× and 2×, `wheel.gif`, 123 frames).
- Could (not done): cap the continuous turn's visual speed at 0.4 × the spoke spacing per frame against the
  wagon-wheel effect at 1/32 (the lead spoke keeps the direction readable meanwhile).

## 4. Review log
### Round 1, UX (a fresh subagent as a Beat Repeat producer who plays the OP-1), all must / should applied
| finding | change |
|---|---|
| "The gate wins" made blue 1/8 .. 1/2 sound the same at the default gate: half the main knob (and the LFO target) dead | **The grid wins:** `gate_eff = min(4 bars, max(gate, 2 × grid))`; the stretch shows in lpg; VARY limited to half the gate |
| Leaving ALWAYS (32 → 31) landed on VARY 15, still firing every bar; on the master (no key) stopping took 32 detents. No fixed "fill every 4 bars", the first producer use | **FORTUNE reordered:** ALWAYS at the hard-left stop, OFF next, EVERY 8 / 4 / 2, CHANCE 10..90 %, EVERY BAR, VARY with chance rising 50 → 100 % |
| PITCH per repeat scaled with GRID: 0.5 st on 1/16 inaudible, RISE 6 on 1/32 pinned at +12 after 2 repeats | **PITCH = the total at the gate's end** (a staircase), in musical steps: 1 2 3 5 7 12 st each way |
| Manual SPLIT started off the beat and could repeat the silence after a staccato note | **Locked to 1/16** (now if within 30 ms after a line, else the next), **the slice is the last grid before that line**; ALWAYS the same |
| A tape loop or jump inside a gate could strand the freeze or retrigger | **Gates count samples**, not bar phase; no retrigger within a gate |
| Default 50 % per bar wrecks half the bars at random on the master | **Default EVERY 4 BARS** |
| GATE lacked the common lengths and multi-bar buildups | **GATE** 1/16 2/16 3/16 4/16 6/16 8/16 12/16 1 / 2 / 4 BARS; longer than the interval merges |
| TAPE STOP looped a small slice at a falling rate (a falling buzz) | **A real tape stop:** reads on behind the write, decelerating; the looped version only when it doesn't fit |
| VARY could leave blue's family (a 1/4T inside a 1/16 fill) | **VARY stays in blue's family** (straight / triplet) |
| A knob silently dead (OFF on drum / master) | **`OFF NO KEY`** readout there; the gate ring dims in ALWAYS / manual; GRID's readout names the grid played |
| (could) Filter and Decay dropped | **Adopted:** a one-pole low-pass and up to −6 dB fade with the drop, a high-pass with the rise |
| (could) Bar 1 when jamming with the tape stopped | Kept as phase 2 (the sequencer-start hook `FUN_01a5e7d0`); checked: the metronome's countdown runs with the click volume at 0 (the volume only gates the click's level, `0x019DE642..52`, read at the site), so the automatic triggers always have a beat |
| (could) SHIFT fine adjust seems dead on stepped knobs | Noted (§2) |

### Round 1, UI (a fresh subagent as a TE-style designer), all must / should applied, one could
| finding | change |
|---|---|
| VARY 1–4 opened the angel's book, which the readout hides | Books open bull → lion → eagle → angel |
| Pink (lightorange, PITCH's family) on the lion's mane and eagle's beak read as "pitch on" | One neutral palette for the creatures: wings grey, body purplegrey, face lpg |
| The raised sword floated; the resting one was a grey sliver | One sword in the sphinx's paw, pivot (168, 30), resting on the rim's crown (rotate 109°), raised green over 3 frames. **Deviation:** the right paw, not the left as proposed: the resting blade on the left would cross the snake's tail |
| One-spoke steps were invisible on symmetric spokes | A lightblue lead spoke in the wheel |
| Lightblue meant three things; the playhead dot vanished on the white gate | Playhead = a radial tick (lpg core, offblue halo); the manual key fills the hub green |
| Clouds looked like mud mounds | Removed; flat offblue pills under the creatures |
| Creatures too bright and too small | Neutral palette, scaled 1.3 at (34, 20), (286, 20), (34, 112), (286, 112) |
| Snake / Anubis slid vertically (Anubis a floating post) | The wheel carries them: the snake rotates along an arc (r 61, 300° → 225°); Anubis translates upright along an arc (feet r 72, 125° → 70°); setting dots outside the arcs |
| The figures read as squiggle / bin | Tapered snake (5 → 1 px) with a bigger head; Anubis's head 1.5×, a long muzzle, tall ears, a tapered torso |
| Alchemical symbols: clutter, moiré at 1/64 | Removed |
| 1/32T and 1/64 were a blue smear | No spokes there, only the blur rings and the lead spoke |
| Idle / firing / off not distinct | FX off: no lots, sword resting. (The transport-stopped look is not needed: our clock runs on the metronome's beat when the tape is stopped, so the playhead keeps orbiting truthfully) |
| (could) A lost roll looked like waiting | Adopted: the lots blink out ~4 frames |
| (could) Rim letters thin | 1.3 stroke |
| (could) Wagon wheel in continuous turns | Not done (noted in §3) |

### Round 2, sound (a fresh subagent as a Beat Repeat producer, on WAVs of the real DSP: a drum loop and a synth line through 16 settings; scratchpad `repeat/wavs/`)
Kept as measured: every gate ends on the bar line, the live slice is bit-exact dry, the latch phase-aligned (corr.
1.00), TAPE STOP's rate linear 1 -> 0 within +-0.03 and near silence before a clean downbeat, -12's centroid 6.0 ->
1.3 kHz and -5.5 dB, no boundary clicks, peaks never above dry.

| finding | change |
|---|---|
| MUST: a rising pitch wrapped the read inside the slice: a second hit at 1 / rate of the slice (a flam 16 ms early at x1.19, a gallop, a clean roll only at x2): sloppy buildups | (A faster read of dense 16ths still meets the next 16th's hit at L / r: that is the source's own, a kick or a stab slice is no longer doubled.) **No wrap:** a faster read runs on past the slice's end (the line holds what followed), so every hit stays on the grid; the slice is halved on the grid when r x L would overtake the write by the slice's end; the boundary's tail fades at rate 1 |
| SHOULD: the staircase's steps were fractional semitones (-1.37, -2.75 ..): out of key on melodic material | **Whole semitones** (round(S k / (N - 1))); the ramps keep their fine steps |
| SHOULD: CHANCE / VARY fills always landed on beat 4 (the gate ends on the bar line) | **With GATE <= 1/4 they roll on every beat line** at a quarter of the chance (the same fills per bar, scattered: Live's glitch use); larger gates keep the bar roll |
| COULD: with the tape stopped, the first click counted as beat 2 (and the snap at boot could take our clock below 0) | **The first click is beat 1** when the clock has never had the tape or a click; the snap never goes below 0 |
| COULD: dense grids +1 .. +4 dB over dry | not done (Live does the same; noted) |
| COULD: 64 .. 128-sample equal-power crossfades on sustained material | not done (the 32-sample linear one measured clean here; device listening first) |
| COULD: VARY 3 fired 16 of 16 bars | the map is 83 % (repeat_check: 80.5 % over 200 bars): chance; the per-beat roll changed it anyway |

### Round 2, UI (a fresh subagent as a TE-style designer, on the C-path renders; scratchpad `repeat/gallery/`)
| finding | change |
|---|---|
| MUST: the FX off looked like waiting (only 3 lots fewer); FORTUNE OFF showed a gate that never fires | FX off and FORTUNE OFF: the dim gate ring only (as ALWAYS); the playhead hidden with the FX off |
| MUST: the playhead tick reached into the rim and read as a letter | shortened to r 35 .. 42.6, halo 3.0 |
| MUST: `GATE 1/4` read like a GRID value | the gate in sixteenths below a bar: `GATE 4/16`; the line screen too |
| SHOULD: `VARY 2 67%` cryptic | `VARY 67% X2` |
| SHOULD: the manual state can't show (phase 2) | marked phase 2 (§7) |
| SHOULD: TAPE STOP = -12 and SPIN UP = +12 (6 dots each) | a mode bar past the 6th dot (`sinkend`, `riseend`) |
| SHOULD: the resting snake floated by the sphinx; Anubis's staff touched the rim | snake 8 deg lower (its dots stay); Anubis r 72 -> 75 |
| COULD: the resting sword read as a broken spoke | blade 20 % shorter |
| COULD: the probe page: unlabelled hex, faint grey, the bare TAPE | `SPEED 1.00 = HEX 08000000` on its own line, lpg, `CLOCK TAPE / METRO` |
| COULD: the line screen had no roll state | FIRING / WAITING / LATCHED in green |
| COULD: wagon wheel in continuous turns; 1/32 spokes at 1x | not done (noted; the lead spoke keeps the direction) |
| (note) the per-beat CHANCE gates end on beat lines, the ring still draws the gate ending at 12 | accepted: the ring shows the gate's length |

### Round 3, UI art (a fresh subagent as a TE-style illustrator, on the C-path renders against the RWS card; scratchpad `repeat_art/`)
The figures redrawn first (§3 "Figures"); the review on that, all must / should applied, two coulds.

| finding | change |
|---|---|
| MUST: the sphinx's darkblue body sank into the darkblue rim (only the nemes read); the grey resting sword was invisible on it | body, leg and arm grey with the purplegrey edge; darkblue only in the nemes stripes; the resting sword purplegrey |
| MUST: the sphinx's tall round haunch and curly tail read as a mouse beside it | a low long haunch along the crown (9 x 5.5), a short thick tail (1.4) |
| MUST: the eagle read as a dove / owl (round lpg head, small beak, big breast) | a 7-unit hooked grey beak, the head an ellipse, a long frowning brow, the breast narrowed |
| SHOULD: the sword pointing down read as an arrow (guard by the tip) | the guard narrowed to +-3, the grip to py + 3.5 and a pommel |
| SHOULD: the lion's face, paws and book merged into one lpg jumble; the face read as a man | mane 13 / 9.5 in 16 points, the face 85 %, grey paws, darkblue nose and mouth, the book in front of the paws |
| SHOULD: the grey snake a thin worm at 1x | neck 5.5 wide, a head 1 unit wider than the neck, the belly line dropped |
| COULD: Anubis's lpg kilt the brightest thing on him | kilt purplegrey, the ears further apart |
| COULD: the bull's and lion's far wing a grey fern | shortened to 18 |
| COULD: keep the bull's cloud clear of BAR k/n | kept (x <= 60) |

## 5. Decisions (the user, 2026-10-17)
1. The name is `repeat` (no alias).
2. CHANCE and VARY fills are seeded by the bar number (and the instance / synth slot): the same fills from the same
   spot on every playback.
3. The SPLIT key (phase 2) will be momentary, locked to blue's GRID (not the 1/16 of UX round 1).
4. A probe for a free mixer-screen key comes later.
5. ALWAYS on the hard-left stop is OK.
6. Losing pingpong's echo tail when switching to repeat (the shared lines) is acceptable.
7. Keep the coupled low-pass / fade with the drop and the high-pass with the rise.

## 6. Build plan (MVP first)
| phase | what | checks | device probes / risks |
|---|---|---|---|
| 0 | **Probe image** REPEATPROBE: one diagnostic line on pingpong's screen showing `[0xFF802960]`, `[0xFF802974] >> 6`, `[0xFF802968]`, bar phase and `0x018C1884`; no new FX | — | Transport states 0 / 1 / 2 / 3; playhead +44100 / s; stop-snap on the 4/4 grid from 0; the speed word at 1.0. Risk: low (reads only) |
| 1 | **MVP `repeat` (id 12, DB fx_types 15):** `src/slot/repeat.c` mono + stereo; GRID, FORTUNE (no VARY yet), GATE, PITCH (steps, SPIN UP, looped TAPE STOP); Insert, 32-sample crossfades; bar phase from the playhead, the metronome when stopped; pingpong's lines with an owner word (+ pingpong's guard); `repeat_pub` | `repeat_check.py` (fxsim): every grid × tempo (40, 60, 80.8, 120, 220 BPM) slice length within 1 sample; EVERY n fires on bars n − 1 mod n from position 0; the gate ends on the bar line; the grid wins; no click (largest step ≤ the dry signal's); stale SDRAM never heard; the owner hand-over; CYCLES; fx_abi_check with 13 FX | Risk: the tape engine's order before the FX (Hypothesis: one block late at worst); the FX browser with 13 rows |
| 1b | **Screen:** `wheel.svg` through the SVG route (lazy load, find with assert 0, `cells` marker), ui.c `draw_wheel` from `runtime_state`'s rules, the readout; line-screen fallback | `repeat_screen_check.py`: the C path vs `runtime_state` (rsvg) on the gallery states, within anti-aliasing; svg_cells per state | `--svg-cost` on the device |
| 2 | **Manual SPLIT** in synth mode (press / release slots installed with the FX, not only `--chord`; apply.py mask), the backward capture and the 1/16 lock, the frame-hook fail-safe | host test: the press path calls TE's function for every other key; a missed release is cleared | On the device: SPLIT on every synth page, the CHORD page unaffected |
| 2b | VARY, the real TAPE STOP (read behind the write), the coupled tone, the sequencer-start hook (`FUN_01a5e7d0`) for bar 1 with the tape stopped | sim: VARY stays in its family and within half the gate; TAPE STOP's read never overtakes the write | |
| 3 | Drum-mode and master keys (trace the drum controller's and MasterController's press slots for a free key) | | Probe first: which keys reach the slot on the drum and mixer screens |
- **Risks:**
  - The press hook now also serves the FX. A bug there affects every synth key: keep TE's path the default and add
    host tests.
  - Varispeed drift (accepted).
  - SDRAM latency on the reads (measure).
  - The screen's AGG budget is fine (35 %), but the C must hide what the mockup hides (the spoke sets).

## 7. As built (2026-10-18: phases 0, 1, 1b, with VARY, the real TAPE STOP and the coupled tone of 2b)
### Files
- `src/slot/repeat.c` (DSP), `repeat.h` (the knob maps = gen_wheel.py's, and `struct rep`, the state the screen
  reads), `src/slot/ui_repeat.h` (the screen and the probe page, included by ui.c), `design/fx/wheel.svg` from
  `gen_wheel.py` (now the shipped file: marker id `wheel1`, published as `content/display/wheel.svg`, build.py
  `FX_SVGS`).
- `fx.json`: `repeat`, id 12 (DB fx_types 15), default `[12288, 5120, 9216, 16384]`. build_slot.py `FX_UI_KINDS`
  repeat 5, and refuses an FX list with repeat but without pingpong (the shared lines). The FX tables grow by
  themselves: name loops 15 entries (with the aliases ens, drive), factory / ScreenId bounds 13, the range table 13
  rows, the five `FX_FLAG_TABLES` copies 13 bytes (id 12 reads 0: the LFO targets knob `>> 12`).
- TE words (build_slot.py `TE_UI`): `_te_tape_state` 0xFF802960, `_te_tape_pos` 0xFF802974, `_te_metro_count`
  0x01AADADC, `_te_play_slot` 0xFF8016D4 (the fills' seed), `_te_ppqn_tick` 0x018C1884 (the probe only); the tempo
  `_te_tempo_spb` as pingpong. ui_repeat.h also reads the tape speed word 0xFF802968 and SHIFT (0x01DD7F78).
  Reads only: nothing writes TE state.

### DSP
- **Bar clock (per block, `clock()`):** tape rolling (state 1 / 2, playhead < 2^30): position = playhead << 2 (Q8);
  else our own count, +64 samples a block, snapped to the metronome: the countdown (Q14) says where the next click
  is, and that is moved onto our nearest beat line (so bars continue the tape's count across a stop; with no clock
  before, at boot, the next click is beat 1). Countdown out
  of range (< 0 or > 1.06 beats): free running. Stopped for minutes: whole 8-bar groups are dropped before the
  uint32 wraps. One shift-subtract division a block (bar index and phase; it loops over the quotient's bits only).
- **Trigger:** the position shifted by the gate (eff gate in sixteenths) crosses a bar line L: the gate ends on L.
  It fires at the exact sample (Q8 phase). A line passed in the last 128 samples (varispeed, a locate) still fires
  at sample 0, never twice. EVERY n: `L % n == 0`; CHANCE / VARY: `mix32(L * golden + seed) >> 16 < chance` (seed:
  instance and synth slot). With GATE <= 1/4, CHANCE / VARY roll on every beat line instead (L = the beat, a
  quarter of the chance: the same fills per bar). A lost roll counts `lost` (the screen's blink).
- **Gates count samples** (grem, Q8): the end is the bar line's sample; the last 32 samples fade to dry. A trigger
  inside a gate extends it (merge); one on the very sample where the old gate ends starts a fresh gate; one inside
  the end fade lets that fade finish over the new live slice (the live slice is the dry input itself).
- **Slices (forward):** the first slice is the live input (read back from the line at lag 0: bit exact dry); every
  boundary restarts the read at the slice start with a 32-sample crossfade from the old read (the tail continues
  past the slice end: the line keeps recording). The line records until it would reach the slice's start
  (`FREEZE` = 65408 samples after it), so gates and the latch can last any time.
- **ALWAYS (the latch):** at the next 1/16 line, or at once within 30 ms after one (then phase-aligned to that
  line); the slice is the grid that ended on the line (backward); a crossfade from dry. Pitch: the total after one
  bar, then held. Leaving ALWAYS: dry within one 32-sample crossfade.
- **Grid per slice:** blue's (with the fold: a grid longer than 65280 samples plays its half, on the grid);
  VARY draws each repeat's grid (`rep_vary_pick`: blue's family, +-a places, at most half the gate).
- **Pitch:** STEP: repeat k of N at round(S x k / (N - 1)) whole semitones (2^(n/192) by the 2^(i/16) table,
  interpolated); a rate > 1 reads on past the slice's end (no wrap: the hits stay on the grid; supersedes §2's
  "an upward step wraps within the slice"), the slice halved on the grid if the read would overtake the write. SPIN UP and
  the looped TAPE STOP: a per-sample ramp (Q28) from the gate's start, 1 -> 2 / 1 -> 0. **The real TAPE STOP**
  (when half the gate fits the line: 1 bar down to 80.8 BPM) reads on from the gate's start at the decelerating
  rate while the line records; it fades over the last 15 % (and to silence if the rate reaches 0 while latched).
- **Coupled tone:** down a one-pole low-pass (`b = 2^(-2.44 d)`, open = exactly the input, ~1.4 kHz at d = 1) and
  -6 dB at the full drop; up a high-pass from ~5 Hz to ~600 Hz; d = |pitch| / 12 (ramps: their progress). Gains and
  the coefficient glide per block (the gain linearly across it). Rounded multiplies: no bias.
- **Memory:** pingpong's lines through `fx_line_owner[2]` (pingpong.h: [0] mono line, [1] stereo pair): the FX that
  finds another owner takes the set over, resets `filled` (and repeat its gate). Reads older than `filled` are 0.
- **CPU:** idle, and blocks with nothing playing: a copy loop. Repeating: a steady loop specialised by channels,
  pitch direction and ramp (noinline functions, so its values stay in registers; the bfin GCC spilled ~30 live
  values in the general loop), the general loop only for event samples (starts, the end fade, the line not yet
  full).

### Probe (phase 0): SHIFT held on the repeat screen
- Lines: `TAPE <state> <name> POS <playhead / 64>`; `TAPE BAR n BEAT b.cc` (from the playhead and the tempo, 4/4
  from 0); `SPB <samples per beat> <BPM>`; `METRO <countdown samples> TICK <0x018C1884>`; `SPEED <word / 0x08000000>
  = HEX <word>` (0xFF802968); `OURS BAR n BEAT b CLOCK TAPE|METRO [IDLE]` (repeat's clock, IDLE = the FX not
  running); `FIRES n LOST n DRY|GATE|LATCH REP k`.
- What to look for on the device: LOG.md test image 88's checklist.

### Screen (phase 1b): ui_repeat.h `draw_wheel`
- runtime_state's rules, from the DSP state while it runs (fx_live: the published knobs, so TE's LFO moves it) and
  the stored knobs otherwise; the sword rises over 0.2 s on a won roll, the lots blink out 0.27 s on a lost one, a
  page turns 0.27 s when a VARY repeat drew another grid. The manual key's lit hub is phase 2 (never shown yet).
- Readout: a stored-knob turn (>= 512 raw) shows the value 1.5 s at the title position (the angel hides), never TE's
  LFO alone; `GRID 1/4` and under it `1/2 TOO LONG` when folded.
- Without wheel.svg (or an older one without `wheel1`): a line screen (the four values, the bar with the gate, the
  playhead, FIRING / WAITING / LATCHED).

### Checks (all pass, 2026-10-18)
- `src/slot/test/repeat_check.py` (312 simulator runs, ~11 min; `repeatsim.py`, `repeat_main.c`): grid timing (11
  grids x 40 / 60 / 80.8 / 120 / 133.3 / 220 BPM: every boundary, each repeat's read start and the return to dry
  within 1 sample); bar alignment (odd tape position, a locate, the metronome with the tape stopped, a stop mid-gate);
  CHANCE seeded by the bar; every FORTUNE mode (rates over 200 bars, VARY's picks); every GATE x 5 grids and the grid
  wins; the pitch staircase within 6.6 cents; TAPE STOP within 1.5 samples of its curve, never ahead, the fade, the
  looped one at 60 BPM, SPIN UP, a latched stop is silent; no clicks in 114 runs (knob jumps and an LFO on all
  knobs included); pingpong hand-over exact zeros both ways, boot garbage never heard; invalid tempo / transport /
  playhead / metronome words; no DC (an input and its negation give opposite outputs: |mean| <= 0.6 LSB);
  the fold; stereo channels; cycles.
- `fx_abi_check.py`, `fx_lfo_check.py` (repeat added), `repeat_screen_check.py` (the C path vs runtime_state: the
  deterministic states within 0.8 mean pixel difference; readout, blink, probe, line screen, cells).
- **CYCLES per call** (simulator, no SDRAM latency), mean / max over 2.5 bars, mono / stereo: idle 1.1 k / 1.4 k;
  default (one fill every 4 bars) 1.6 k / 2.2 k mean, 12.8 k / 16.7 k peak; 1/64 latched at -12 7.3 k / 11.0 k (peak
  15.5 k stereo); VARY 6.6 k / 10.1 k (15.2 k); TAPE STOP 9.2 k / 13.4 k (14.0 k); SPIN UP 7.9 k / 11.9 k (16.4 k).
  Pingpong: 16.2 k / 16.9 k stereo. The design's 4-8 k was optimistic.

## 8. REPEAT1 on the device, and REPEAT2 (2026-10-18)
REPEAT1 (LOG 88): repeat in the FX list; the probe shows the right clock (TAPE while rolling, METRO stopped); BPM and
bar count follow tempo changes; the wheel follows the repeats and the LFO. Two bugs:
- **With the tape stopped no fill ever fired.** Cause (Verified in TE's metronome `FUN_019de548`, `0x019DE614..2E`):
  with the transport stopped the countdown `0x01AADADC` is advanced by `[0xFF802970] >> 21` (the tape's own advance)
  instead of 64 samples when the tape-follow byte `0x01AADAD0` is set, so it holds still. Our clock snapped onto the
  countdown every block, so it stood still too: no gate start was ever crossed. The simulator's metronome always
  counted down. **Fix:** the countdown counts only while it runs (one block less than the last, or that plus a
  reload); otherwise our count runs free at the tempo, continuing the tape's bar count (`CLOCK FREE` on the probe). The
  first click is beat 1 only at boot, and the clock's jump there is not a late trigger. repeatsim.Tape models the held
  countdown (`follow=True`); `metro_held` failed on REPEAT1's code (no fills) and passes now.
- **"The fill lands one bar early".** Not reproduced: EVERY 2 / EVERY 4 from tape position 0, after a stop with a held
  metronome, end on the bar lines after bars 2, 4, 6 / 4, 8 (the interval's last bar's beat 4), and from a start at
  2.5 bars on tape bar lines 4 and 8 (relative to tape bar 1, as intended); these checks pass on REPEAT1's code too.
  TE's own playhead getter (`FUN_01a29f58`) also reads the word as samples x 64. Likely: play started from a tape
  position other than 0 (the tape stays where it stopped), so bar 1 after play was the interval's last bar. **Now
  shown:** `BAR k/n` (white, under the wheel's left, and on the probe and the line screen): the bar within the EVERY n
  interval, n/n = the bar whose last beat fills.

### The manual key: the green encoder's push (REPEAT2)
- Static: encoder pushes would be key events 0x3B..0x3E (GPIO bits 8..11, chord2.md §6; **whether the hardware has
  push switches is unproven**). TE's KeyHandler `FUN_019b6c74` sends a press without SHIFT to the current controller's
  +0x14 slot and releases to +0x24. Controllers (base ctor `FUN_01a16eec(this, id)`): synth id 0 vtable `0x01911994`,
  drum id 1 `0x0190FA64`, mixer id 3 `0x01910AAC` (MasterController, ctor `0x019D97A2`); the same slot layout. Their
  press functions leave these codes unhandled: synth returns 0 for >= 0x3A, drum's is-note test fails (notes end at
  0x35), mixer's returns 0 above 0x2F. So today a push does nothing on these screens (Verified statically).
- Hooks (build_slot.py, with the repeat FX, checked against stock): drum `0x0190FA78` / `0x0190FA88` and mixer
  `0x01910AC0` / `0x01910AD0` -> `rk_*` (repeat.c); synth: CHORD v2's c2_press / c2_release (installed by `--chord`, or
  by this FX without it) call `rep_key_event` first (weak: absent without repeat.c).
- `rep_key_event`: counts every 0x3B..0x3E press / release, the last code and controller; takes green (0x3C,
  Hypothesis: GPIO order blue, green, white, orange) only while the repeat screen of that controller's context was
  drawn within the last 100 blocks (TE's block counter `0x01AADAF4`); everything else goes to TE's function with the
  same arguments. If the code or the hook is wrong, the press just does nothing (and the probe's KEYS line says what
  arrived).
- DSP: held = the latch (ALWAYS's path), at the next line of blue's grid (or at once within 30 ms after one), the slice
  that ended there, the pitch total after a bar; released or the screen gone for 100 blocks: dry within a crossfade.
  It overrides FORTUNE. Screen: the hub lights green, the gate ring dims.
- Checks: `repeat_key_check.py` (simulator: the entries keep every register and pass 4 x 70 keys through with the same
  this, key and return value; green taken only on its own context's fresh screen; release clears and still reaches
  TE; stale screen; counters; the DSP's latch on blue's line, dry on release and on a stale screen); chord2_check (the
  synth wrappers' pass-through, rep_key_event allowed as the one weak symbol).
