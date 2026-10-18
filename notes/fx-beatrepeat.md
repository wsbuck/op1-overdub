# Beat repeat FX (`repeat`, proposed firmware id 12; screen: the Wheel of Fortune)

**Status: design only (2026-10-17). No firmware code, nothing built.** Mockups: `design/fx/wheel.svg` from
`design/fx/gen_wheel.py` (run it with a directory to get the gallery). FX contract: fx-slot.md; tempo: fx-pingpong.md
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
    at 1×, moiré at 1/64) and the clouds (they read as mud; each creature stands on a flat offblue pill).

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
  - 172 drawable elements, each one subpath of absolute M / L / C / Z; check_svg PASS;
  - the heaviest element is the rim band's stroke, ~1331 cells (35 % of 3840), worst over every gallery state and
    animation frame;
  - drawn per frame: ~60–100 elements.
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

## 5. Open questions for the user
1. **Name:** `repeat` (with alias `beatrepeat`)? TE's names are short (delay, grid, punch); ours are pingpong and
   distortion.
2. **Fills on CHANCE:** seeded by the bar number (the same "random" fills on every playback from the same spot), or
   fresh every time?
3. **Manual key:** hold SPLIT (momentary) only, or also a latch (e.g. SHIFT + SPLIT is the CHORD page, so a double
   tap)? And is a 1/16 lock right, or should it lock to the grid (up to a half note's wait)?
4. **Master manual key:** worth a probe of the mixer screen's controller (which key is free there)?
5. **ALWAYS on the hard-left stop:** OK that it breaks "clockwise = more" for the sake of a quick latch?
6. **Pingpong sharing:** OK that switching between pingpong and repeat on the same position loses pingpong's echo tail?
7. **Coupled tone:** keep the low-pass and fade with the drop, and the high-pass with the rise, or keep the repeats
   full-range?

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
