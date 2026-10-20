# Reverb FX (`space`, firmware 246): research, design and the MVP

**Status: MVP built (2026-10-19), FX id 13 `space`; test images SPACE1 (master tank 44.1 kHz) and SPACE1H
(22.05 kHz), LOG test image 93. As built and the user's decisions: §11; the reviews: §12. A placeholder line screen.**
- The float prototype (v2, after the design review) and a simulator CPU benchmark are in
  `design/fx/reverb_proto/`.
- The FX contract is in fx-slot.md; memory sharing in fx-pingpong.md "Memory" and fx-beatrepeat.md "Memory".
- The review and what changed are in §10.

Tags: **Source** = a cited page; **Measured** = our prototype or the simulator; **Confirmed** / **Verified** /
**Hypothesis** as in the other notes; **Unverified** = from memory or a search snippet, not checked against a
primary source.

## Answer first
- **Architecture:** an 8-line FDN at its core.
  - Feedback: normalised Hadamard, done in 32-bit.
  - Each line has a short allpass and a two-band absorbent filter, which gives the exact RT60 below and above a
    crossover. 4 of the lines are modulated by at least 4 samples.
  - In front of it:
    - a stereo predelay;
    - 8 early-reflection taps per side, the ER sum smeared by 2 allpasses;
    - a 2nd-order low cut;
    - 4 allpasses per channel.
  - Shimmer: a two-head delay-line pitch shifter with jittered windows, in the feedback path.
  - Hold modes: FREEZE, INFINITE and a servoed "frozen shimmer".
  - The FDN runs **block-staged**: every line is longer than 64 samples, so each block reads all the lines, mixes,
    then writes all the lines, each stage a short sequential loop.
- **Rates:**
  - stereo (master) at 44.1 kHz, falling back to 22.05 kHz if the device measures over budget;
  - mono (synth / drum) at 22.05 kHz, so the full cathedral fits its 128 KB.
- **Memory:**
  - stereo 248 KB of pingpong's 256 KB stereo set at `0x01E80000`;
  - mono 102 KB of pingpong's 128 KB mono line at `0x01EC0000`;
  - shared through `fx_line_owner`, as repeat does. No new FREE region is needed.
- **CPU, stereo call:**
  - Measured in the simulator: plain C 139 k cycles, block-staged C 86 k, for the v1 path.
  - Hypothesis, hand-written v2 path: about 23–24 k before SDRAM.
  - Hypothesis, on the device: **30–40 k at 44.1 kHz, 22–28 k with a 22.05 kHz tank.**
  - That is at or above the 15–30 k comfort zone. Phase 0 measures the line streams on the device first, then
    picks the stereo rate.
  - Mono at 22.05 kHz: about 9–12 k.
- **Knobs (recommended, Option A):**

  | knob | role |
  |---|---|
  | blue SPACE | ROOM → HALL → CHURCH → CATHEDRAL → VOID, one continuous morph |
  | green DECAY | ×¼ … ×4 of the space's natural RT60, then INFINITE, then FREEZE at the top |
  | white TAIL, zoned | TONE dark…bright, then SHIMMER +12, +12+19, SUB −12, GATE, SWELL; an amount inside each zone; the zone is taken from the stored knob, so TE's LFO only moves the amount |
  | orange MIX | dry held at unity while the wet rises to the middle. Above the middle: on the master the duck depth rises (0 … −24 dB); on synth / drum the dry falls (kill dry) |

- **Prototype v2 (Measured):**
  - RT60 per octave band within ~5 % of target from 125 Hz to 4 kHz;
  - every shimmer case decays or holds, none grows;
  - line writes rounded toward zero leave exact silence;
  - FREEZE holds within 6 dB over 60 s; frozen shimmer holds within ~2 dB;
  - ROOM's ringing is down: persistence 0.88 → 0.63.
- **WAVs:** `/private/tmp/claude-501/-Users-william-wsbuck-op-1-mods/4fd2339c-392f-41f7-b525-fe02a8b0b83d/scratchpad/reverb/`
  - `00_dry` … `14_frozen_shimmer` (§3.4);
  - rebuilt by `python3 design/fx/reverb_proto/proto.py OUTDIR`.

## 1. Research (with sources)

### 1.1 Architectures
- **Schroeder (1962).**
  - Parallel feedback combs with incommensurate delays, plus series allpasses.
  - Target ~1000 echoes/s; five allpasses give ~810/s.
  - Metallic and sparse by today's standards.
  - [JOS, Schroeder reverberators](https://ccrma.stanford.edu/~jos/pasp/Schroeder_Reverberators.html).
- **Moorer (1979).**
  - Early reflections from a 10–20 tap FIR with irregular spacing.
  - A first-order low-pass in each comb loop, so HF decays faster.
  - Sources: [Clarke / IOA](https://www.ioa.org.uk/system/files/publications/T%20CLARKE%20ARTIFICIAL%20REVERBERATION%20-%20A%20DEVELOPED%20DIGITAL%20METHOD.pdf),
    [Moorer slides](https://freeverb3-vst.sourceforge.io/doc/Moorer_Reverb.PDF),
    [JOS](https://ccrma.stanford.edu/~jos/pasp/Achieving_Desired_Reverberation_Times.html).
- **Dattorro plate (1997, after Griesinger).**
  - Delays at 29761 Hz:
    - input allpasses 142, 107 (0.75) and 379, 277 (0.625);
    - a figure-eight tank of two halves;
    - each half: modulated allpass (672 / 908, coefficient −0.70, ~16 samples excursion) → delay (4453 / 4217) →
      damping → ×decay → allpass (1800 / 2656) → delay (3720 / 3163);
    - 7 signed output taps per side.
  - Values from the port [el-visio/dattorro-verb](https://github.com/el-visio/dattorro-verb). The paper's PDF is a
    scan we could not read, so the match to the paper is Unverified.
  - Dense and lush, recognisably "plate". It doesn't shrink into convincing rooms (Hypothesis, and why it isn't
    chosen).
- **Jot FDN (1991).**
  - N lines, x(n) = Γ Q x(n − M) + u(n), with Q orthogonal
    ([JOS, FDN](https://ccrma.stanford.edu/~jos/pasp/Feedback_Delay_Networks_FDN.html)).
  - Line i gets gain g_i = 10^(−3 M_i / (T60 fs)), so every path decays alike. A low-pass in place of g_i gives
    frequency-dependent RT60
    ([JOS](https://ccrma.stanford.edu/~jos/pasp/Achieving_Desired_Reverberation_Times.html),
    [JOS, damping filters](https://ccrma.stanford.edu/~jos/pasp/Damping_Filters_Reverberation_Delay.html)).
  - Householder I − (2/N) u uᵀ costs 2N − 1 adds and no multiplies
    ([JOS](https://ccrma.stanford.edu/~jos/pasp/Householder_Feedback_Matrix.html)). Hadamard costs N log2 N adds
    and mixes harder.
  - Mode density: ΣM_i ≥ 0.15 · T60 · fs
    ([JOS](https://ccrma.stanford.edu/~jos/pasp/Mode_Density_Requirement.html)).
- **Signalsmith, "Let's write a reverb" (2021)**
  ([page](https://signalsmith-audio.co.uk/writing/2021/lets-write-a-reverb/)):
  - at least 8 channels;
  - diffuser steps (delays, shuffle with polarity flips, Hadamard), each multiplying the echo count by N;
  - Householder feedback ("too much mixing" colours the tail), 100–200 ms lines, a high shelf in the loop, ERs from
    the diffuser, light interpolated modulation;
  - target 2000–4000 echoes/s.
- **Lexicon 224** (AES 1978, Griesinger):
  - 20 kHz sample rate, 12-bit gain-ranged converters, delays modulated against mode build-up
    ([vintagedigital](https://www.vintagedigital.com.au/lexicon-224-digital-reverberator),
    [Mix](https://www.mixonline.com/technology/1978-lexicon-224-digital-reverb-383667),
    [Tape Op](https://tapeop.com/reviews/gear/85/lexicon-224-digital-reverb-plug-in)).
  - So a classic hall had a ~10 kHz bandwidth.
- **Valhalla (Sean Costello).** Sources: the
  [Shimmer manual](https://www.valhalladsp.com/shimmer/ValhallaShimmerNotes.pdf); a blog snippet only for
  [diffusion and metallic artefacts](https://valhalladsp.com/2011/01/21/reverbs-diffusion-allpass-delays-and-metallic-artifacts/).
  - More allpass diffusion rings metallically on transients.
  - Modulation is "the quickest way of reducing metallic artifacts" ("dozens of random modulators").
  - Cascaded diffusers give a Gaussian (blooming) envelope; diffusion 0.618 gives equal attack and decay.
- **Early reflections:** a tapped delay over the first ~100 ms, taps optionally filtered
  ([JOS](https://dsprelated.com/freebooks/pasp/Artificial_Reverberation.html)). We use per-space tables (§2.3).
- **Convolution vs algorithmic** (our arithmetic; Hypothesis):
  - 2 s at 44.1 kHz is 88 k taps, ~3.9 GMAC/s direct: 10× a 400 MHz core.
  - Partitioned FFT convolution (Gardner 1995, [AES](https://aes2.org/publications/elibrary-page/?id=7957)) fits
    in MACs, but needs ~0.7 MB of IR spectra in SDRAM and an FFT every block.
  - It can't freeze, shimmer or morph. So: algorithmic.

### 1.2 What makes the spaces differ
| | RT60 (mid) | pre-delay / ITDG | early reflections | colour |
|---|---|---|---|---|
| booth / small room | 0.2–0.5 s (studios 0.3–0.5, booths 0.2–0.3) | 1–5 ms | dense, strong, within ~30 ms | quick build, boxy |
| live room | ~0.8–0.9 s | 5–10 ms | dense | |
| concert hall | 1.7–2.0 s occupied (best halls; Beranek, Unverified) | < 25 ms in the best halls, 25–35 typical, > 35 "arena" | strong **lateral** reflections; IACC_E3 ≈ 0.3 in excellent halls | warm: bass ratio 1.1–1.25 |
| church (stone) | ~4 s (Bath Abbey 4.5 empty, 3.9 occupied) | 20–40 ms | sparser, discrete; flutter between parallel walls (Unverified) | long bass |
| cathedral | 6–12 s (St Paul's 11–12 s at low frequencies) | 30–60 ms | "limited early energy", little clarity | long low end, HF dies early |
- Sources:
  - rooms: [commercial-acoustics](https://commercial-acoustics.com/sound-advice/rt60-chart/),
    [secondskin](https://secondskinaudio.com/acoustics/recording-studio-acoustics);
  - halls (ITDG, IACC, bass ratio): [proav.de](https://proav.de/acoustic/concerthall.html);
  - lateral reflections: [arXiv 2111.01900](https://arxiv.org/pdf/2111.01900);
  - Bath Abbey: [Barron](https://www.ioa.org.uk/system/files/publications/M%20Barron%20CONVERTING%20A%20HISTORIC%20CHAPEL%20TO%20A%20PERFORMANCE%20%20SPACE%20BOTH%20GRAND%20AND%20INTIMATE.pdf);
  - St Paul's: [B&K](https://www.bksv.com/doc/bo0116.pdf),
    [RG Jones](https://www.rgjones.co.uk/case-studies/rg-jones-and-st-pauls-cathedral-a-legacy-of-precision-in-heritage-sound);
  - churches: [acs.eu](https://www.acs.eu/?p=1373).
- **HF damping grows with size.** Air absorbs 5.6 / 16 / 30 / 105 dB/km at 1 / 2 / 3 / 4 kHz
  ([JOS](https://ccrma.stanford.edu/~jos/pasp/Air_Absorption.html)). So the bigger the space, the lower our
  crossover and the smaller the HF ratio.
- **Modulation** is the algorithm's cure for metallic modes, not a property of the room.
- **Stereo:** low early IACC reads as wide. We decorrelate the L/R ER patterns and take L and R from orthogonal
  output vectors.

### 1.3 Shimmer
- **Origin:** Eno and Lanois (U2, *The Unforgettable Fire*): an Eventide harmoniser into a reverb
  ([Premier Guitar](https://www.premierguitar.com/eventide-releases-the-shimmerverb-plug-in)).
- **Valhalla Shimmer** (manual above):
  - the shifter is in the feedback loop, ±12 st; the classic is +12 with feedback ≥ 0.5;
  - modes: single, dual (up and down), reversed grains ("smoother, organ-like");
  - randomised grains give "noisy sidebands" instead of metallic colour;
  - low and high cut in the loop.
- **Strymon BigSky Shimmer** ([manual](https://www.strymon.net/manuals/BigSky_Plugin_UserManual_RevA.pdf)):
  - two voices made from the reverberated signal, −1 … +2 oct, including +OCT&P5;
  - shimmer on the input, in the tank (regenerating), or both;
  - Amount = level of the shifted voices.
- **Eventide ShimmerVerb** ([Attack](https://www.attackmagazine.com/news/eventide-release-shimmerverb/)): parallel
  shifters on the tail (±2 oct), pitch delay up to 1 s, infinite feedback with a crossover, freeze.
- **Boss RV-6** ([Boss](http://in.boss.info/products/rv-6/)): "sparkling high-frequency… overtones"; its knobs move
  several parameters at once.
- **Delay-line ("Doppler") shifter** ([Puckette](https://msp.ucsd.edu/techniques/latest/book-html/node115.html)):
  - a sawtooth-swept delay, the span being the window;
  - two taps half a window apart, each faded out around its reset;
  - too long a crossfade comb-filters ([MathWorks](https://cs400-web.cs.wisc.edu/nchou/s/matlab-2020b/help/audio/ug/delay-based-pitch-shifter.html)).
- **Runaway:** nothing published on normalisation. Ours is in §3.2 and §4.4.

### 1.4 Other tail effects (products)
- **Freeze vs infinite** (BigSky): freeze sustains the tail while new notes play over it; infinite lets new notes
  add to it. Eventide Blackhole has both, plus inverse-decay "Gravity"
  ([Premier Guitar](https://www.premierguitar.com/eventide-releases-the-blackhole-reverb-pedal)).
- **Gated:** Padgham and Collins (a compressed talkback mic into an SSL gate), then AMS RMX16 "NonLin2"
  ([Reverb.com](https://reverb.com/news/get-the-in-the-air-tonight-reverb-and-193-more-from-the-townhouse-studios-rmx16)).
  BigSky Non-Linear has Gate, Reverse, Ramp, Swoosh, Gauss and Bounce shapes.
- **Swell** (BigSky): the wet rises behind the dry like a volume pedal.
- **Bloom:** BigSky's bloom generator feeding a tank; Valhalla's Midiverb II "Bloom".
- **Ducking:** Boss RV-6 "Dynamic".
- **Octave down:** BigSky, Valhalla, Eventide.
- **Tail saturation, tempo-synced decay:** no product documentation found (Unverified).

### 1.5 TE's stock `spring` (Rymd), for reference (Confirmed, quick read of `0x01A326E8`, mono process)
- Per-position SDRAM at `0x01001EA8 + pos * 0xE3A0` (58,272 B each; TE's zero-fill, not FREE). It zeroes its lines
  on the first call.
- Per sample, six recirculating delays with taps (rings of 382, 1272, 5330, 3418, 1848 and 5902 samples), then
  three more lines (596, 3490, 6898). In total ~0.66 s.
- A Schroeder-style network, not a dispersive spring model (Hypothesis on the exact topology).
- Scalar per-sample code with compare-and-wrap indices.
- It can't go big, freeze or shimmer: no overlap with ours.

## 2. Architecture choice

### 2.1 Candidates (stereo call, hand-written loops, before SDRAM; Hypothesis from §4.2)
| | per call | memory (cathedral) | rooms | halls / cathedrals |
|---|---|---|---|---|
| Dattorro plate | ~6 k + ER | ~70 KB | poor (fluttery when shrunk) | very good (plate colour) |
| **FDN-8 + ER + diffusion** | **~23–24 k** all-in | ~250 KB | good (ERs and in-line allpasses) | good, neutral, exact RT60 per band |
| FDN-4 + ER | ~15 k | ~130 KB | good | metallic: ΣM too small |
- FDN-4's problem in numbers: mode density at 7 s needs ΣM ≥ 0.15 · 7 · fs = 46 k samples, and 4 lines of 120 ms
  give 21 k.
- **Choice: FDN-8.**
  - ΣBASE_MS is 410 ms. At size 2.0 that is 0.82 s, and the in-line allpasses add 13 %: 0.93 s · fs.
  - So JOS's criterion holds to ~6 s. CATHEDRAL and VOID rely on modulation, which the persistence figures confirm
    works (§3.1).
- **Hadamard, not Householder** (review). Householder at N = 8 keeps 0.75 on the diagonal: slower density and more
  per-line comb colour.

### 2.2 Signal flow (per instance; mono has one input channel)
```
in L/R -> predelay L/R --+--> 8 ER taps / side -> 2 allpasses / side (4-13 ms x sqrt size) ------+--> wet out
                         |                                                                        |
                         +--> direct + ER send -> 2nd-order HP (low cut) -> LP (bandwidth)         |
                              -> x in_gain (0.5 RT60^-1/4) -> 4 allpasses / channel (5-23 ms)      |
                              -> (+ shimmer return) -> FDN-8 inputs                               |
FDN-8: read 8 lines (4 modulated, linear interp.) -> in-line allpass (13 % of the line, g +-0.5)  |
       -> two-band filter g_lo LP + g_hi (x - LP) -> Hadamard (32-bit) / sqrt 8                    |
       -> + inputs (L, R on orthogonal sign rows) -> write (round toward 0; nearest in FREEZE)    |
       outputs: L, R = two further orthogonal sign rows, x2 make-up ---------------------------->+
shimmer: (L + R) / 2 of the tank -> HP 300 Hz -> biquad LP 7 kHz (6 kHz with +19) -> 2-head shifter(s),
         window 3000 +-20 % per reset -> x amount x min(1, 4 sqrt(1 - g^2)) -> soft limit -> FDN input
hold:    FREEZE: loop gain 1 - 2^-13, input muted (50 ms), shimmer return off, or servoed in a SHIMMER zone
         INFINITE: same loop gain, input open, input gain / (1 + tank level / 0.15)
tail FX: envelope detector on the dry input: GATE (level-keyed), DUCK, SWELL (both on the wet)
```
- **Block staging:**
  1. Every FDN line is ≥ 457 samples (ROOM's shortest, 10.3 ms) > 64, so a block's outputs exist before its writes.
  2. Stage 1, per line: a sequential read (interpolated on the 4 modulated lines), the in-line allpass and the
     two-band filter, kept in 32-bit.
  3. Stage 2, per frame: the Hadamard, in 32-bit.
     - **Not dual 16-bit `>>1`**: Blackfin's halving truncates toward −∞, which is floor rounding inside the loop.
       The worst case in §3.3 is −58 dBFS residual.
     - The 1/√8 is applied once, at the write, with round toward zero.
  4. Stage 3, per line: a sequential write, packed as 32-bit pairs (half the posted stores).
  - Each stage is a hardware loop over contiguous addresses.
  - The shimmer return enters the next block, one block of extra loop delay.
- **Coefficients per block, no division:**
  - DECAY → c = 9.966 / (T60 · fs) from a 33-entry log table, times the space's natural RT60;
  - g = 2^(−M_i c) by ensemble's `exp2_frac` and a shift;
  - 16 exp2 per block, or only on knob changes.
- **SPACE changes glide:** lengths, ER taps and allpass lengths slew at most 1/64 sample per sample (~¼ semitone of
  Doppler; review). LFO on SPACE (synth / drum) is a gentle warble, not seasickness.

### 2.3 Space tables (prototype v2, `proto.py` SPACES / ER)
| anchor (SPACE raw) | size | natural RT60 | HF ratio / crossover | predelay | ER span, level | allpass g | mod depth / rate | low cut (2nd order) / bandwidth |
|---|---|---|---|---|---|---|---|---|
| ROOM (0) | 0.33 | 0.7 s | 0.50 / 3.5 kHz | 3 ms | 3–28 ms dense, 0.8 | 0.62 | 4 smp / 1.5 Hz | 150 Hz / 9 kHz |
| HALL (8192) | 1.0 | 2.2 s | 0.50 / 3 kHz | 20 ms | 8–70 ms lateral, 0.5 | 0.70 | 7 / 0.6 | 80 / 8 k |
| CHURCH (16384) | 1.45 | 3.8 s | 0.45 / 2.5 kHz | 30 ms | 12–97 ms sparse, gains jittered, 0.55 | 0.62 | 6 / 0.4 | 90 / 7 k |
| CATHEDRAL (24576) | 2.0 | 7.0 s | 0.38 / 2 kHz | 45 ms | 15–150 ms sparse, 0.4 | 0.72 | 8 / 0.3 | 110 / 6 k |
| VOID (32767) | 2.4 | 18 s | 0.60 / 3 kHz | 70 ms | 20–210 ms faint, 0.15 | 0.75 | 18 / 0.2 | 140 / 9 k |
- Size scales the lines (31.3–73.1 ms at size 1); √size scales the ER times (capped at size 1.6) and the allpasses.
- Everything interpolates between anchors: linear, or geometric for times, sizes and frequencies. ER tap k glides to
  tap k.
- R's ER pattern is L's with each tap moved by ±5–13 %.
- Bass ratio is not separate: lows keep the full RT60 above the low cut. Dark TONE doubles the low cut.
- Measured RT60 runs ~3–5 % above the table: the in-line allpasses add loop delay, and the gains include it.

## 3. Prototype (float, `design/fx/reverb_proto/`)
- `tank.c`: the §2.2 flow in double precision, sample by sample, with optional int16 storage: round, floor, toward
  zero, or round + TPDF dither. Mode 3 is the device rule: toward zero, nearest while frozen.
- `proto.py`: the Option A mapping, the test signal, the renders and the checks: RT60, diffusion and ringing,
  shimmer, freeze, int16, headroom.
- `bench.c` / `bench.sh`: the simulator CPU benchmark (§4.2; the v1 path).
- Test signal: an E minor plucked-string arpeggio, a saw-ish pad chord and three snare-like hits (6.5 s), then
  silence.

### 3.1 RT60, diffusion and ringing (Measured, v2)
RT60 per octave band, measured / target. Measured is T30 by Schroeder integration of Hann-STFT band energy, with the
ERs off; the target is the two-band filter's prediction averaged over the 8 lines.

| setting | 125 | 250 | 500 | 1 k | 2 k | 4 k | 8 k Hz |
|---|---|---|---|---|---|---|---|
| ROOM | 0.73/0.77 | 0.75/0.77 | 0.75/0.76 | 0.73/0.73 | 0.66/0.65 | 0.55/0.54 | 0.43/0.47 |
| HALL | 2.43/2.41 | 2.38/2.40 | 2.35/2.37 | 2.24/2.25 | 1.99/1.97 | 1.58/1.62 | 1.25/1.43 |
| CHURCH | 4.19/4.16 | 4.03/4.13 | 4.06/4.03 | 3.70/3.71 | 3.08/3.06 | 2.39/2.45 | 1.92/2.18 |
| CATHEDRAL | 7.87/7.64 | 7.59/7.54 | 7.12/7.16 | 6.13/6.14 | 4.75/4.65 | 3.58/3.68 | 2.88/3.33 |
| HALL, TONE dark | 2.43/2.40 | 2.36/2.37 | 2.24/2.26 | 1.93/1.91 | 1.40/1.29 | 0.88/0.75 | 0.59/0.51 |
| HALL, TONE bright | 2.43/2.41 | 2.38/2.41 | 2.38/2.41 | 2.43/2.41 | 2.40/2.41 | 2.25/2.41 | 1.89/2.41 |
| ROOM ×¼ | 0.24/0.17 | 0.21/0.17 | 0.24/0.17 | 0.21/0.17 | 0.22/0.16 | 0.23/0.13 | 0.21/0.11 |
| CATHEDRAL ×4 | 28.0/27.9 | 27.2/27.4 | 25.6/25.9 | 21.8/22.0 | 16.1/16.5 | 11.3/13.2 | 7.7/12.0 |
- **Within 5 % up to 4 kHz.**
  - 8 kHz runs short: linear interpolation low-passes the 4 modulated lines on every pass. Allpass (Thiran)
    interpolation fixes it for ~1 k cycles (phase 1 option; review SHOULD).
  - ROOM ×¼ bottoms out at ~0.22 s: the diffusers' own ring. Fine for a "tight room".
- **Diffusion and ringing.** The review's metrics:
  - crest factor per 25 ms after the predelay (diffuse noise ≈ 4);
  - late fine structure against a ⅓-octave smooth: its std (Gaussian 5.6 dB) and its persistence, the correlation
    between consecutive windows (1 = static modes, i.e. metallic).

| space | crest, first 8 × 25 ms (v2) | v1 crest (review) | std / persistence v2 | v1 (review) |
|---|---|---|---|---|
| ROOM | 7.8 6.1 3.1 4.8 4.2 3.5 4.3 3.6 | — | 8.4 dB / 0.63 | 10.2 / 0.88 |
| ROOM ×4 | 7.9 7.1 3.1 4.6 4.1 3.2 4.3 3.6 | — | 9.6 / 0.66 | 14.9 / 0.90 |
| HALL | 13.4 8.4 6.0 5.4 3.3 3.0 3.3 4.1 | > 10 to 75 ms | 7.2 / 0.42 | — / 0.42 |
| CHURCH | 19.1 12.3 10.1 10.0 10.0 9.5 5.0 3.8 | 26 → 16 to 125 ms | 6.9 / 0.45 | |
| CATHEDRAL | 23.1 17.0 12.6 14.2 12.6 7.4 6.1 9.7 | 32 32 29 19 11 | 7.1 / 0.36 | — / 0.46 |
- CHURCH and CATHEDRAL keep some discrete early reflections on purpose (big stone spaces). Their crest is about
  half of v1's. More smear (a third ER allpass, or a lower ER level) is the lever if snares still flam by ear.

### 3.2 Shimmer stability (Measured, v2: 0.5 s noise burst, then 60 s; wet rms per 5 s)
| case | 0–5 s | 15–20 s | 30–35 s | 55–60 s |
|---|---|---|---|---|
| +12 max, CATHEDRAL ×4 (28 s) | −30.0 | −56.1 | −85.5 | −134.4 |
| +12 max, VOID ×4 (~70 s) | −32.1 | −39.9 | −48.5 | −65.6 |
| +12+19 max, VOID ×4 | −32.5 | −43.9 | −54.1 | −72.1 |
| −12 max, VOID ×4 | −31.4 | −29.2 | −30.9 | −39.5 |
| +12 max, VOID ×4, no normalisation, no limiter | −30.1 | −29.8 | −36.0 | −51.1 |
| +12 max, VOID ×4, int16 lines | −32.1 | −39.9 | −48.3 | −59.7 |
- **Nothing runs away.** Each pass moves the shimmer an octave. Only ~4.5 octaves fit between the 300 Hz high-pass
  and the 7 kHz low-pass (plus the tank's damping), so the cascade dies out.
- −12 at the extreme holds ~25 s before it decays: it climbs down into the low cut instead, and that takes longer.
- **Normalisation:** return × min(1, 4 · sqrt(1 − ḡ²)), ḡ the mean line gain per pass.
  - At normal decays the factor is 1.
  - With a factor of 1 instead of 4 the shimmer was almost inaudible at long decays (HF/LF −10 dB vs −11 dB dry); at
    4, −0.1 dB.
  - A tanh limiter (0.5 FS) on the return is the backstop.
- **The octave build is real** (review, sim, 440 Hz burst, +12): by 9–11 s, 880 / 1760 / 3520 Hz are at −3 … −10 dB
  of the total, while 440 has fallen to −27 … −31 dB. +19 builds 1320 / 2640; −12 builds 220 / 110.
- **Frozen shimmer** (freeze in a SHIMMER zone):
  - The loop stays lossless. The shifter adds octaves, and a servo trims the return so the tank stays within ~2 dB
    of the level it was frozen at.
  - Measured: the level holds −26 … −24 dB for 30 s, while HF/LF rises from −13 to −7 dB.
  - The review's suggestion (loop × √(1 − a²), return × a) drained instead: a pure up-shift in a lossless loop
    empties itself out of the top band (−36 → −177 dB over 30 s at a = 0.4).

### 3.3 Freeze and 16-bit lines (Measured, v2)
- **FREEZE** (HALL, burst, then frozen 60 s; rms per 10 s): −29.0, −31.6, −32.9, −33.4, −34.0 dB in the device
  mode.
  - The loop gain is 1 − 2⁻¹³ (bounded drift, review).
  - The first ~5 dB is linear interpolation's HF loss; Thiran interpolation would flatten it.
- **INFINITE** (`13_infinite_hall`): steady at −22 dB over 12 s of input. The line peak was 0.40 FS with a −3 dBFS
  pad: the level servo holds.
- **Residual after a burst and 60 s of silence** (last 10 s):

| line write | ROOM rms / peak | HALL ×4 rms / peak |
|---|---|---|
| round to nearest | −84.5 / −74.4 dBFS | −73.2 / −66.9 |
| floor (truncation, e.g. `>>>`) | −68.9 / −65.8 | −57.7 / −56.4 |
| round + TPDF dither | −83.6 / −70.5 | −80.6 / −66.8 |
| **toward zero** | **silence** | **silence** |
- v2's in-line allpasses made the limit cycles worse than v1's (−95 / −90 dBFS): there is more recursion to sustain
  them.
- Rounding sustains limit cycles ("soft whistling sounds which do not disappear",
  [US4475229](https://patents.google.com/patent/US4475229)). Only magnitude truncation guarantees decay
  ([pyfda](https://pyfda.readthedocs.io/en/stable/manual/input_fixpoint_specs.html)).
- **Rule:** every recursive write (lines, allpasses, the low-pass states) rounds toward zero: add the sign mask
  before the shift, 2 ops. FREEZE and INFINITE switch the line writes to nearest.
- **Headroom:** a sustained −3 dBFS saw pad gives line peaks of 0.19 FS (CATHEDRAL ×4), 0.29 FS (VOID ×4 +12) and
  0.40 FS (HALL INFINITE).
  - This comes from the −6 dB input scaling plus the RT60^−¼ normalisation at the tank input (review).
  - v1, normalised at the output, had 1.16 / 1.52 FS.

### 3.4 Renders (16-bit stereo 44.1 kHz; knobs raw; master MIX law)
All in `/private/tmp/claude-501/-Users-william-wsbuck-op-1-mods/4fd2339c-392f-41f7-b525-fe02a8b0b83d/scratchpad/reverb/`:
| file | SPACE, DECAY, TAIL, MIX | what |
|---|---|---|
| `00_dry.wav` | — | the test signal |
| `01_room.wav` | 0, 16384, 5120, 9000 | ROOM, natural 0.77 s |
| `02_hall.wav` | 8192, 16384, 5120, 11000 | HALL 2.4 s |
| `03_church.wav` | 16384, 16384, 5120, 12000 | CHURCH 4.2 s |
| `04_cathedral.wav` | 24576, 16384, 4000, 13000 | CATHEDRAL 7.7 s, slightly dark |
| `05_extreme_void.wav` | 32767, 26000, 7000, 20000 | VOID ×2.6 = 47 s, deep modulation, 60 % wet (cut at 20 s) |
| `06_shimmer_plus12.wav` | 20480, 22000, 12800, 15000 | between CHURCH and CATHEDRAL, +12 at 70 % |
| `07_shimmer_plus19.wav` | 24576, 22000, 17000, 15000 | CATHEDRAL, +12 and +19 |
| `08_sub_minus12.wav` | 16384, 18000, 20800, 13000 | CHURCH, −12 |
| `09_freeze.wav` | HALL; frozen 2.9 … 10 s | the pad chord frozen, the snares over it |
| `10_gated_room.wav` | 3000, 24000, 26000, 14000 | GATE: hold 194 ms after the input drops, close 60 dB in 30 ms |
| `11_swell_hall.wav` | 10000, 20000, 31000, 16000 | SWELL 0.56 s on the wet |
| `12_duck_cathedral.wav` | 24576, 20000, 4000, 24576 | master MIX above the middle: duck depth 0.5 (−12 dB while playing) |
| `13_infinite_hall.wav` | 8192, 31000, 5120, 13000 | INFINITE: everything layers into one held pad |
| `14_frozen_shimmer.wav` | CATHEDRAL, +12 zone, frozen at 2.9 s | the frozen chord climbs in octaves at constant level |
- Measured on the renders:
  - GATE holds the pad open to its end (3.7 s), then closes;
  - SWELL's tail after the snares is the same level as without swell (v1: 12–27 dB lower);
  - DUCK gives −12.0 dB at mid travel and −24 dB at the top, recovering fully after the playing;
  - no file clips (peaks −1.5 … −4.0 dBFS). Every file ends with a 1.5 s fade.

## 4. Feasibility on our hardware

### 4.1 Memory
- **Stereo** (master, 44.1 kHz, int16):

| buffer | samples | bytes | note |
|---|---|---|---|
| FDN lines | 8 × 8192 | 128 KB | max 8100 (184 ms): VOID's 175 ms + modulation |
| in-line allpasses | 8 × 1024 | 16 KB | 13 % of a line ≤ 1006 |
| predelay / ER L, R | 2 × 12288 | 48 KB | 278 ms: predelay ≤ 70 ms + ER span ≤ 205 ms (VOID's ER scale capped to fit) |
| input allpasses | 8 × 2048 | 32 KB | VOID's longest 36 ms |
| ER allpasses | 4 × 1024 | 8 KB | ≤ 20 ms |
| shimmer ring | 8192 | 16 KB | window 3000 + 20 % + 64; both voices read it |
| **total** | 126,976 | **248 KB** | pingpong's stereo set, 256 KB at `0x01E80000` (8 KB spare) |
- **Mono** (synth / drum) at 22.05 kHz, one channel, full sizes:
  - FDN 8 × 4096, in-line allpasses 8 × 512, predelay / ER 6144, 4 input allpasses × 1024, 2 ER allpasses × 512,
    shimmer 4096;
  - total 52,224 samples = **102 KB** of pingpong's mono line (128 KB at `0x01EC0000`).
  - Review: at 44.1 kHz the mono budget capped CATHEDRAL as well as VOID, and its 139 ms predelay/ER line was short
    for CHURCH (147 ms needed).
  - The 22.05 kHz path is the stereo fallback anyway, so it is built once.
- **Blackfin rings needn't be powers of two** (I/B/L circular addressing).
- **FREE regions today** (memory-map.md; LOG CANARYLOG: regions 0–2 clean, region 3 damaged):

| region | user | free |
|---|---|---|
| 0 `0x00800000`–`0x00900000` | prof_stats, harmonic ram `0x00801000`–`0x00810000`, voice_ram `0x00810000`–`0x00818000`, grain `0x00820000`–`0x00900000` (max) | `0x00818000`–`0x00820000` (32 KB) |
| 1 `0x01E10000`–`0x01F10000` | va, slot block `0x01E20000`–, pingpong / repeat lines `0x01E80000`–`0x01EE0000` (384 KB), inprobe, sprobe, harmonic, fm4, amp (+ram) to `0x01F02000` | `0x01F02000`–`0x01F10000` (56 KB) |
| 2 `0x02C00000`–`0x02D00000` | wavetable code and ram | none |
- **The reverb shares pingpong's lines** through `fx_line_owner` (`FX_OWNER_REVERB`):
  - there is one master FX at a time, and one mono FX object at a time (fx-beatrepeat.md);
  - switching FX loses the other FX's tail, as already accepted for pingpong ↔ repeat.
- **Takeover:** the memory is undefined or holds the last owner's audio.
  - On taking the set, the reverb clears 8 KB per block over 31 blocks (45 ms), wet muted meanwhile.
  - Otherwise the state survives the FX being off, like pingpong's paused tape (open question 7).
- **More room if wanted:** canary-probe one more 1 MB (e.g. `0x02D00000`–`0x02E00000` or `0x00900000`–`0x00A00000`,
  both untested). That would also make a true reverse reverb possible.

### 4.2 CPU
**Measured** (`bench.sh`: plain fixed-point C, `-mcpu=bf524 -O2`, simulator, no SDRAM latency; the v1 path with one
shimmer voice):
| stage | naive C | block-staged C |
|---|---|---|
| predelay + 2 × 8 ER taps | 13.4 k | 13.4 k |
| 4 allpasses | 12.3 k | 12.3 k |
| FDN-8 | 94.7 k | 41.1 k |
| shimmer, 1 voice | 13.4 k | 13.9 k |
| mix | 5.1 k | 5.1 k |
| **total** | **139 k** | **86 k** |

**v2 estimate, hand-written loops** (Hypothesis; per stereo call):
| stage | 44.1 kHz | 22.05 kHz tank |
|---|---|---|
| ER taps 2 × 8 (one MAC loop per tap) | 2.5 k | 2.5 k (stays full rate) |
| ER allpasses 4 | 0.8 k | 0.8 k |
| input allpasses 8 | 3.0 k | 1.5 k |
| FDN-8 stages 1–3 | 9 k | 4.5 k |
| in-line allpasses 8 | 2.5 k | 1.3 k |
| shimmer, 1–2 voices | 1.4–2.8 k | 0.7–1.4 k |
| low cut, bandwidth, shimmer biquads, detector, mix | 2 k | 2 k |
| half-band decimate / interpolate | — | 2 k |
| block maths, coefficients | 1.5 k | 1.5 k |
| **before SDRAM** | **~23–24 k** | **~17–18 k** |
| SDRAM: ~60 read streams × 4 line fills + posted writes (pairs) | 6–15 k | 3–8 k |
| **on the device** | **~30–40 k** | **~22–28 k** |
- **The ratio is uncertain.** 86 k → 23 k assumes ~4× from hand-written code; ensemble got 3×, so a 30 k
  pre-SDRAM figure is possible (review).
- **SDRAM** (Hypothesis):
  - Line fills are bursts of four 64-bit reads, and the core continues on the critical word
    ([ADI training](https://ftp.kemt.fei.tuke.sk/SignaloveProcesory/_web/blackfin/slides/ADST-170-SEC6-Memory-V1.6.pdf)).
  - The fill cost on the BF524 is unknown (20–60 core cycles guessed).
  - Write-back caches allocate on read only as Linux configures them (Source above). So the line writes are posted
    stores, packed as 32-bit pairs: ~320 per block.
- **Phase 0 decides the stereo rate:** a device stub that only streams the 8 + 8 FDN lines and 16 ER taps,
  measured with TE's CYCLES records:
  - FX max `0x01BA2704` (synth FX path);
  - callback max `0x01BA2628` (master FX is inside the mixer);
  - voice max `0x01BA26AC` (cache pressure on TE's voices).
  - If 44.1 kHz extrapolates over 30 k, the stereo tank runs at 22.05 kHz like the mono one.
- **Mono at 22.05 kHz:** ~9–12 k on the device. Both at once (reverb as synth FX and master FX): ~31–52 k, 5–9 % of
  a 580 k-cycle block at 400 MHz.

### 4.3 Shimmer cost and options (fixed point)
| shifter | per voice per frame | quality | note |
|---|---|---|---|
| **2 heads, sine windows, W = 3000 (68 ms) ± 20 % per reset** | ~27 cyc | v1 (no jitter): a 29.2 Hz AM line, envelope fluctuation 0.32–0.44 vs 0.17 without shimmer (review); jitter smears it | chosen; window table 1025 × Q15 in `.data` |
| 3 Hann heads | ~33 | smoother still | if the jitter isn't enough |
| reversed grains (Valhalla SingleReverse) | same | "smoother, organ-like" | later |
| PSOLA / phase vocoder | 100s–1000s | clean | too expensive; not needed in a loop |
- **Anti-alias:** a ratio-r read aliases above fs / 2r.
  - Pre-filter with a biquad: 7 kHz for +12 (limit 11 kHz), 6 kHz when +19 shares the ring (limit 7.35 kHz).
  - v1's two one-poles were only −11 dB at 11 kHz: inharmonic energy −9 … −10 dB of the tail at +12, about −6 dB at
    +19 and +7 (review).
- **High-pass 300 Hz** before the shifter (mud stays out); 120 Hz for −12.
- **+7 dropped** (review): regenerated fifths stack A → E → B → F♯ → C♯, out of key (1480 and 2217 Hz present by
  6 s). The +19 voice is always paired with +12.

### 4.4 Stability (fixed point)
- **Linear stability:** an orthogonal Q with |g_i(f)| < 1 is stable for every setting (Jot). The in-line allpasses
  are lossless, and the two-band filter never exceeds max(g_lo, g_hi).
- **No floor anywhere in the loop:**
  - the Hadamard in 32-bit;
  - the low-pass states in 32-bit;
  - every requantisation toward zero (nearest in FREEZE / INFINITE).
- **Overflow:**
  - −6 dB at the tank input, plus the RT60^−¼ normalisation there;
  - saturating writes as a backstop;
  - the shimmer return soft-limited;
  - INFINITE's input servoed on the tank level.
- **Coefficients:** Q15 gains resolve T60 to ~2 % at 60 s. Freeze's 1 − 2⁻¹³ is ~−1.3 dB per minute.
- **Denormals:** none in fixed point. The toward-zero rule reaches exact 0. The requantisation floor (~−96 dBFS
  per write) decays with the tail.
- **Shimmer runaway:** bounded by the band-limited cascade (§3.2), the ×4 cap and the limiter. Frozen shimmer is
  bounded by its servo.

## 5. Knob design
**Constraints:**
- 4 knobs; knobs 4–7 are fine adjust of 0–3, so zones can be fine-tuned;
- no SHIFT layer;
- TE's LFO reaches the 4 knobs on synth / drum FX only, not the master;
- detents of 1024 raw.

### Option A (recommended): SPACE | DECAY | TAIL | MIX
| knob | role | zones / range |
|---|---|---|
| blue SPACE | type and size, one continuous morph (§2.3) | ROOM at 0, HALL at detent 8, CHURCH 16, CATHEDRAL 24, VOID at the top. The readout names the nearest type. Glides (§2.2) |
| green DECAY | RT60 relative to the space's natural one | detents 0–29: ×¼ … ×4 exponential, detent 15 = natural; **30 INFINITE** (input open, the tail holds and layers); **31 FREEZE** (input muted, 50 ms fades). ×4 on VOID ≈ 72 s |
| white TAIL | one tail effect, with an amount inside its zone | 0–9 **TONE** dark → natural (5) → bright: HF ratio ×¼ … 1, bandwidth ×½ … ×2, dark doubles the low cut. 10–14 **SHIMMER +12**. 15–18 **SHIMMER +12+19**. 19–22 **SUB −12**. 23–27 **GATE**: hold 50 … 350 ms after the input drops; or 1/16 … 1/4 tempo-synced, phase 3. 28–31 **SWELL**: 0.15 … 1.5 s on the wet. Shimmer amount 40 … 100 % across its zone; FREEZE inside a shimmer zone = frozen shimmer |
| orange MIX | dry / wet (and ducking on the master) | 0 = bit-exact dry; 0 … 16384: dry 1, wet 0 → 1. **Master:** 16384 … 32767 keeps dry and wet at 1 and raises the duck depth 0 → −24 dB (−12 dB at 3/4 travel) while the input is above −24 dBFS. **Synth / drum:** the dry falls to 0 (kill dry) |
- **TAIL is not an LFO target in effect.** The DSP takes the zone from the stored knob
  (`0xFF801444 / 0xFF8015C4 + slot*16`, as ui.c's FX_KNOBS). Only the in-zone amount follows TE's modulated copy,
  clamped to the zone. So an LFO on white sweeps the shimmer amount and never hops between effects.
  SPACE (warble) and DECAY (a square LFO into the top = rhythmic freeze) stay full targets.
- **Zone boundaries** are jumps that glide over ~23 ms, as pingpong's FREE / SYNC boundary does.
- **Why A:**
  - SPACE and TAIL are orthogonal: shimmer on any space, gate on a hall, swell on a void;
  - DECAY is one gesture from natural to extreme to held;
  - ducking (the ambient master move, review) sits on MIX, so it combines with shimmer;
  - orange is MIX, as in pingpong and ensemble.
  - Cost: no tone control inside the shimmer / gate / swell zones; each space has its natural tone and the shimmer
    has a fixed band.

### Option B: SPACE+MODE | DECAY | TONE | MIX
- blue: the five spaces over detents 0–19, then fixed "machines" over 20–31: SHIMMER HALL, SHIMMER CATHEDRAL,
  +12+19 CATHEDRAL, SUB CHURCH, GATED ROOM, SWELL HALL. The in-zone travel is the amount.
- green DECAY, white TONE (always), orange MIX: as A.
- Pro: TONE everywhere, like BigSky machines.
- Con: shimmer and FX on preset spaces only; the morph is cut to 20 detents.

### Recommendation
- A.
- **Fixed:**
  - per-space diffusion, ER patterns, predelay, modulation, crossover, bandwidth, low cut;
  - width (full), line count;
  - the shimmer window, jitter, band and ×4 cap;
  - the hold fades (50 ms);
  - the detector's times and its −24 dBFS threshold;
  - the limiter and the servos.

### Freeze on the green encoder push (only if proven)
- Repeat's manual key uses the green push on its screen (LOG 89).
- Whether the push reaches the controller is still the pending REPEAT2 / REPEAT4 probe (`KEYS P n R n`), and LFOFX2
  is pending too.
- If confirmed: hold the push on the reverb screen = **momentary freeze** (frozen shimmer in a shimmer zone). Same
  flag word and "screen drawn in the last 100 blocks" rule as repeat; release thaws in 50 ms.
- DECAY's top detent stays the latched freeze. Until the push is proven it is the only freeze, plus TE's LFO into
  it on synth / drum.

## 6. Tail effects ranked for the OP-1 (fun ÷ effort)
| rank | effect | why on the OP-1 | cost | where |
|---|---|---|---|---|
| 1 | **FREEZE** (+ frozen shimmer) | drones under the tape, pads from one chord; the push makes it playable | ~0; servo ~0.1 k | DECAY top; push (if proven) |
| 2 | **SHIMMER +12, +12+19, SUB −12** | the ask; the ambient signature | 1.4–2.8 k, 16 KB | TAIL |
| 3 | **DUCK** (master) / **GATE** (drum FX) | DUCK keeps a big master reverb out of the playing's way (RV-6 "Dynamic"). GATE is the 80s drum sound; synced holds make it rhythmic | ~0.3 k each | MIX (master) / TAIL |
| 4 | **INFINITE** | layering chords into one pad (BigSky / Blackhole "infinite") | ~0 | DECAY detent 30 |
| 5 | **SWELL** | a reverse-ish bloom with no latency, now on the wet | ~0.2 k | TAIL |
| 6 | TONE | always useful | in the filters | TAIL 0–9 |
| 7 | tail saturation | lo-fi tails; overlaps distortion / crush | ~0.5 k | not in v1 |
| 8 | true reverse reverb (synced chunks) | iconic, but needs a 1/4 … 1/2 bar wet buffer (up to 176 KB) beside the tank; latency | ~2 k, RAM | after a 4th FREE region |
| 9 | tempo-synced decay | subtle; the synced GATE is the audible version | ~0 | no |
| 10 | BLOOM (slow-build diffusion) | SWELL covers most of it; a real one is ~3 k, ~40 KB | | later |
- **Envelope detector** (prototype values):
  - fast follower 1 ms / 60 ms, slow 40 ms / 400 ms, on max(|L|, |R|);
  - onset (SWELL): fast > 1.5 × slow + ¼ threshold and fast > ½ threshold; re-armed below 1.1 × slow;
  - GATE: open while fast > −24 dBFS, then hold, then 60 dB in 30 ms;
  - DUCK: depth × min(1, fast / threshold), attack ~2 ms, recovery ~150 ms.

## 7. Risks
1. **Device CPU** (30–40 k at 44.1 kHz) is at or over budget. Phase 0 measures it; the fallback is a 22.05 kHz tank,
   then 6 ER taps.
2. **Cache pressure** on TE's voices: ~8 KB streamed per block. Measure the voice max.
3. **GCC 4.3.5 `-O2` loop / CC miscompile** (chord2.md): loops over lines with a hoisted condition (modulated vs
   plain) match its signature. Use separate loops or asm; run `loopcc_check.py`.
4. **Shared lines:** the takeover clear mutes the wet for 45 ms; pingpong / repeat lose their tails on a switch.
5. **Discrete early reflections** in CHURCH / CATHEDRAL may flam on snares (crest still 10–23 in the first 100 ms).
   The levers are ER level and smear.
6. **The ROOM tail** is still above Gaussian (std 8.4 dB, persistence 0.63). If it rings by ear: more modulation, or
   two in-line allpasses on the shortest lines.
7. **Shimmer:** residual grain roughness, and the ×4 cap is a taste call.
8. **The mono 22.05 kHz path:** the wet is band-limited to ~10 kHz on synth / drum (Lexicon 224 territory).
9. **Two MIX laws** (master duck, mono kill-dry) must be explained on screen.
10. **The FX browser with 14 FX** and the factory / ScreenId bounds (build_slot.py grows them).
11. **No master LFO:** a moving master reverb needs the push or a turn.

## 8. Build plan
| phase | what | checks |
|---|---|---|
| 0 | device stub: an FX that streams 8 + 8 lines (4 interpolated) and 16 ER taps at both rates, no DSP | TE's CYCLES records (FX, callback, voices) → picks the stereo rate |
| 1 MVP | `src/slot/reverb.c`, fx.json id 13 (DB fx_types 16), pingpong's sets via `fx_line_owner` (`FX_OWNER_REVERB`); stereo (rate from phase 0) and mono (22.05 kHz); FDN-8, ERs, diffusion, low cut; SPACE / DECAY (incl. INFINITE, FREEZE) / TAIL as TONE only / MIX (both laws, duck on the master); asm loops; takeover clear; `reverb_pub`; TE's filter screen | `reverb_check.py` (fxsim): RT60 per band within 10 % of the table for all spaces at ×¼, ×1 and ×4; crest and persistence no worse than §3.1; exact silence 10 s after a burst; garbage-filled RAM never heard; owner hand-over with pingpong and repeat; freeze within 6 dB over 60 s; INFINITE line peak < 0.5 FS on a 0 dBFS pad; knob sweeps without clicks; CYCLES; fx_abi_check (14 FX); fx_lfo_target_check; loopcc scan. Device: listen to all spaces, CYCLES |
| 2 | shimmer zones +12, +12+19, −12, frozen shimmer: shifter with jitter, biquads, ×4 cap, limiter, servo; TAIL's zone from the stored knob | 60 s stability at every zone × max DECAY; aliasing < −60 dB for a 6 kHz sine at +19; the LFO on white never changes zone |
| 3 | GATE (synced holds from the tempo word), SWELL; the green push as momentary freeze (once proven) | detector tests (drum loops: every hit; pads: no chatter); the gate on the hit within 1 ms; the push on and off the screen |
| 4 | the screen (a designer brief, as for repeat) | the usual screen checks |
| opt | Thiran interpolation on the modulated lines; a 4th FREE region probe; true reverse | |

## 9. Open questions for the user
1. **DECAY relative or absolute?** Proposed: relative, centre = the space's natural RT60.
2. **Option A or B?**
3. **Predelay:** fixed per space, or wanted somewhere (e.g. tempo-synced)?
4. **MIX:** the master's upper half as duck depth, with kill-dry only on synth / drum? Or kill-dry everywhere, with
   DUCK as a TAIL zone?
5. **Hold modes:** INFINITE and FREEZE as DECAY's two top detents? Frozen shimmer automatically when frozen in a
   shimmer zone?
6. **v1 tail effects:** FREEZE, SHIMMER, DUCK, GATE, INFINITE, SWELL (the ranking). Anything to cut or add?
7. **Re-enable:** resume the old tail (pingpong's paused-tape rule), or start clean?
8. **Name:** `reverb`? (TE's is `spring`; `hall`, `space` and `verb` are free.)
9. **Stereo at 22.05 kHz** if phase 0 says so: OK to trade the top octave of the wet for CPU?
10. **A 4th FREE region probe** (a CANARY image): wanted, for an unshared buffer and a true reverse later?

## 10. Review log (2026-10-19)
A fresh subagent acted as a reverb DSP designer and producer. It reviewed this design, the v1 prototype and its 13
WAVs, with its own analysis scripts.

| review item | change (v2) | result |
|---|---|---|
| MUST: the onset is undiffused (CATHEDRAL crest 32 to 200 ms; snares flam) | 4 input allpasses per channel (5–23 ms × √size); 2 allpasses on each ER sum | CATHEDRAL crest 23 → 7–10 by 125–200 ms; HALL ~3–5 from 100 ms (§3.1). Discrete ERs in big spaces kept on purpose (risk 5) |
| MUST: ROOM rings (persistence 0.88, ×4 0.90) | modulation never below 4 samples (no size scaling), ROOM 1.5 Hz; one allpass inside each line (13 %, g ±0.5) | persistence 0.63 / 0.66, std 8.4 / 9.6 dB |
| MUST: the Hadamard must not floor | 32-bit Hadamard and low-pass states; 1/√8 at the write, toward zero (§2.2, §4.4) | design rule |
| MUST: SWELL on the tank input deletes percussion | SWELL moved to the wet output | tail after the snares as without swell |
| MUST: the gate caught 2 of 8 notes and cut a pad at 0.3 s; closed too slowly; doc hold / swell values wrong | level-keyed (open while above −24 dBFS, hold, 60 dB in 30 ms); render values recomputed (§3.4) | the pad held to its end |
| MUST: mono memory doesn't fit | mono tank at 22.05 kHz (102 KB, full sizes) | §4.1 |
| MUST: CPU optimistic (uncached writes, filters, second voice, detector missing) | v2 estimate 23–24 k before SDRAM, 30–40 k on the device; phase 0 stub decides the stereo rate; writes packed in pairs | §4.2 |
| MUST: a kick becomes a sub drone (6 dB/oct cut at 40–70 Hz) | 2nd-order low cut, 80–150 Hz by space; dark TONE doubles it | §2.3 |
| SHOULD: headroom (1.16 / 1.52 FS lines on a −3 dBFS pad) | RT60^−¼ normalisation moved to the tank input; ×2 make-up at the output | 0.19 / 0.29 FS |
| SHOULD: allpass (Thiran) interpolation | listed as a phase-1 option (~1 k cycles) | not prototyped |
| SHOULD: freeze rounding random-walks at 1 − 2⁻¹⁵ | freeze loop gain 1 − 2⁻¹³ | −5 dB over 60 s, then flat |
| SHOULD: shimmer grain AM (29.2 Hz line) and weak anti-aliasing | window jitter ±20 % per head reset; biquad pre-filter, 6 kHz with +19 | §4.3 |
| SHOULD: drop +7 (out-of-key stacks) | dropped; +19 only with +12 | TAIL zones |
| SHOULD: keep Hadamard, drop the Householder A/B | adopted | open question removed |
| SHOULD: ΣM and ROOM's shortest line were wrong | ΣM 0.82 s at size 2 (+13 %); ROOM's shortest line 457 samples | §2.1, §2.2 |
| COULD: CHURCH's ER flutter | gains jittered | |
| COULD: SPACE glide 1/16 sample per sample is ~1 st | 1/64 | |
| COULD: frozen shimmer as loop √(1 − a²) + return a | tried: it drains out of the top band (−36 → −177 dB in 30 s). Replaced by a lossless loop plus a servoed return | holds within ~2 dB, octaves build (`14_frozen_shimmer`) |
| COULD: an INFINITE detent | DECAY detent 30, with an input servo on the tank level | `13_infinite_hall`, line peak 0.40 FS |
| knobs: TAIL shouldn't be an LFO target | the zone comes from the stored knob; the LFO moves only the amount | §5 |
| knobs: no duck with shimmer in A; duck depth too weak | master MIX upper half = duck depth (−12 dB at 3/4 travel, −24 at the top); DUCK zone removed | Measured −12.0 / −24 dB |
| ranking: DUCK above GATE on the master; INFINITE 6th | re-ranked (§6) | |

## 11. As built: the MVP (`space`, FX id 13, 2026-10-19)
Sources: `src/slot/space.c` (DSP, the C reference kernels), `space_asm.h` (the hot loops), `space.h` (knob maps, shared
with the screen), `space_tables.h` (generated by `src/slot/gen_space.py` from `proto.py`), `ui_space.h` (the screen).
Checks: `src/slot/test/space_check.py` (simulator), `space_kernels*.c`, renders `space_render.py`.

### The user's decisions (on §9)
| question | decision | as built |
|---|---|---|
| 1 DECAY | relative, each space at noon | noon = exactly the space's RT60 (the prototype's noon was x1.10); x1/4 .. x4 in two log segments |
| 2 knobs | Option A | SPACE / DECAY / TAIL / MIX as §5, with the changes below |
| 3 predelay | fixed per space | the table's (3 .. 70 ms) |
| 4 MIX | wet rises with the dry at unity to the middle; above: master duck -12 -> -24 dB, synth / drum dry fades | -12 dB one detent above the middle (a 1-detent ramp from 0), linear in dB to -24 at the top |
| 5 holds | INFINITE, FREEZE on DECAY's top detents; frozen shimmer automatic | as §5; held shimmer also in INFINITE (servoed) |
| 6 v1 tail FX | TONE, SHIMMER +12, +12+19, FREEZE, INFINITE, DUCK; SUB / GATE / SWELL later | **zones reserved** (19 .. 31 keep their place, natural tone, no effect, screen `SUB -12 SOON` greyed): saved sounds keep their meaning when v1.1 fills them, no rescale later |
| 7 re-enable | a clean tail | a clean start after > 64 calls without process (TE's block counter), on taking the lines over, and at the first call |
| 8 name | `space` | fx.json `space`, DB fx_types next row |
| 9 22.05 kHz master | acceptable | build option `--space-tank 22` (SPACE_MASTER_HALF): SPACE1H |
| 10 FREE region | not now | pingpong's lines, shared |

### Signal path and numbers (deviations from §2 marked)
- Tank rate: master 44.1 kHz (or 22.05 with the option); synth / drum always 22.05 kHz. The 22.05 kHz paths
  decimate the input with a 31-tap half-band (Kaiser 6: -0.2 dB at 8.8 kHz, -34 dB at 13.2 kHz) and interpolate the
  wet with the same filter; the dry stays at 44.1 kHz, bit exact.
- Memory (pingpong's FREE lines through `fx_line_owner`, `FX_OWNER_SPACE`): master at 44.1 kHz 248 KB of the 256 KB
  set (lines 8 x 8192, in-line allpasses 8 x 1024, predelay / ER 2 x 16384, input allpasses 512 .. 2048 each, ER
  allpasses, shimmer 8192); at 22.05 kHz half. Mono 103 KB of 128 KB. A clean start clears the set, 8 KB per call
  (master 44.1 kHz: 31 calls, 45 ms; the wet muted meanwhile, the dry plays).
- Predelay, 8 ER taps per side (**integer delays**: they jump a sample at a time while SPACE moves; the lines glide),
  2 ER allpasses (g 0.6), **a 20 Hz DC blocker and the bandwidth low-pass on the ERs** (sound review: TONE now
  darkens the reflections too, and no DC reaches the wet).
- Tank input: (direct 0.5 + ER 0.7) x 0.5 RT60^-1/4 (mono x sqrt 2), the low cut as **two one-poles at fc / 1.554**
  (12 dB/oct, -3 dB at fc), the bandwidth one-pole, 4 allpasses (**the output computed as (1 - g^2) z - g x**, one
  requantisation in the recursion).
- FDN-8 as §2.2. Per line: the read (linear interpolation, exact at fraction 0: lines at rest read whole samples),
  the in-line allpass (g +-1/2, stored v requantised, the output exact), the two-band filter with 1/sqrt 8 folded into
  the gains, F in Q18. The Hadamard in 32-bit, in place; **the late outputs are its rows 1 and 2** (of the filtered
  lines, not the raw reads: same decay, free). The inputs on rows 3 and 4's signs, then the write.
- **The loop's rounding (a deviation, measured):** toward zero everywhere made long tails decay 1.5 .. 2x too fast at
  normal levels (each truncation loses ~0.5 LSB a pass; VOID x4 needs 1 - g of 0.003 a pass). So the line and
  in-line allpass stores round **to nearest even while the tank level is above ~-78 dBFS** (NEAR_HI 4 / NEAR_LO 3,
  hysteresis) and toward zero below, and to nearest while held. The RT60 is then within 10 % down to the last 10 dB of
  a T30, and every tail still ends in exact silence (no limit cycle: below -90 dBFS a tail never comes back).
- FREEZE: loop gain 1 - 2^-13, input muted over 50 ms, **the modulation stops** (whole-sample reads: linear
  interpolation's loss drained 4.7 dB in 18 s at 22.05 kHz); INFINITE: the same gain, half the modulation, the input
  servo 1 / (1 + level / 0.06) (0.15 gave 0.62 FS line peaks on a 0 dBFS saw pad; now < 0.5 FS).
- Shimmer: HP 300 Hz, the anti-alias biquad (7 / 6 kHz at 44.1 kHz; **3 / 2.2 kHz run twice at 22.05 kHz**: 4th
  order, sound review), the ring, 2 heads per voice, **window 100 ms (4410 samples)**, the **start offset of each grain
  jittered 0 .. 0.1 W (the pitch stays exact**: the prototype's window jitter detuned each grain by up to +-20 % of
  the shift), the window linear over each 64-sample segment, the return x min(1, 4 sqrt(1 - g^2)), a cubic limit at
  0.5 FS; held (FREEZE or INFINITE): servoed to 1.25 x the level it was held at. The ring is cleared when the shimmer
  comes on (no old tail plays; a one-off ~110 k cycle call).
- Duck (master): a peak follower on the dry (1 ms attack), **held 150 ms after the last peak, then ~200 ms**; the
  gain ducks in ~2 ms and recovers over **~400 ms** (sound review: it pumped on 8th notes).
- Mix: ramped per sample; **a 4:1 soft knee above -3 dBFS on dry + wet** (the sum peaks at -1.6 dBFS instead of
  clipping; MIX 0 bypasses it: bit exact).
- TAIL's zone on synth / drum comes from the stored knob (`0xFF801444 / 0xFF8015C4 + slot * 16`, slot from
  `0x01AAAC0C` / `+4`, read at the callback's copy loops `0xFFA02986` / `0xFFA02560`, **Verified**): the live value's
  zone if it is one of the two stored zones, else the stored value nearest it. The shimmer amount fades in over each
  shimmer zone's first detent (UX review: no cliff at TONE -> SHIMMER).
- The green push (REPEAT2's hook, `rep_key`): held on the space screen = FREEZE while held (screen heartbeat as
  repeat's; weak, so builds without repeat link). Whether the encoders push is still the open REPEAT2 probe.
- Asm: every kernel bit exact with its C reference (300 random trials each, a third at full scale, and the whole FX on
  12 cases at both rates); circular I/B/L buffers for every ring, no ALU-computed P addresses in the hot loops but the
  shimmer window's, unbiased rounding set explicitly (ASTAT RND_MOD cleared and restored).

### CPU (simulator, CYCLES per 64-frame call, mean / max; no SDRAM latency)
SPACE1 (the MVP as first built; SPACE2's numbers in §13):

| | idle | busy | VOID x4 | SHIMMER +12+19 | frozen shimmer | INFINITE |
|---|---|---|---|---|---|---|
| master, tank 44.1 kHz (SPACE1) | 47.6k / 49.4k | 49.5k / 51.3k | 49.3k / 50.7k | 61.5k / 63.5k | 57.3k / 58.0k | 49.9k / 51.1k |
| master, tank 22.05 kHz (SPACE1H) | 33.7k / 34.9k | 34.6k / 35.9k | 34.5k / 35.8k | 42.9k / 44.9k | 40.4k / 41.1k | 35.0k / 35.7k |
| synth / drum, 22.05 kHz | 23.9k / 25.1k | 24.8k / 26.0k | 24.7k / 25.9k | 32.7k / 34.7k | 30.2k / 30.9k | 25.2k / 25.9k |
- One-offs: a clean start clears 8 KB a call (~45 ms); the shimmer coming on clears its ring (one call ~110 k).
- About 2x §4.2's hand-written estimate (23–24 k): per kernel-call overhead (~35 calls a block, ~150–350 cycles of
  setup each), the precision work in the loop (Q16 allpass outputs, rounding), the review's additions (ERs filtered,
  the 4th-order shimmer filter at 22.05 kHz, the mix knee). The 22.05 kHz master saves 30 %, not 50 %: the fixed
  costs stay. The device adds SDRAM line fills (§4.2): the screen's CPU lines read the real numbers (this context's
  max / mean / peak, the FDN's share, TE's records for the callback and the synth FX stage).
- Where it goes at 44.1 kHz (simulator, before the review's additions): FDN stage 1 13 k, the input allpasses 6.5 k,
  the writes 4.5 k, the input filters 3.3 k, ER allpasses 2.9 k, ER taps 2.7 k, the Hadamard 2.4 k, params 1.7 k.
- Levers if the device is over budget: fuse the per-line kernels (one call for 8 lines), 3 input allpasses, the
  22.05 kHz master, fewer ER taps.

### Screen (placeholder, `ui_space.h`)
Line text: the title the space's name (blue); SPACE (the morph `HALL > CHURCH 46%`, or the anchor's own RT60), DECAY
(seconds and the multiplier, `> INF` from detent 28, `INFINITE  IN OPEN`, `FREEZE  IN MUTED`), TAIL (zone and
amount, reserved zones grey `SOON`), MIX (`DRY 100 WET 50`, `DUCK -14DB` on the master); a state box (FROZEN,
FREEZE HELD for the green push, INFINITE, CLEARING); the tail level meter (light blue, -72 .. 0 dBFS) and the duck now;
three grey CPU lines. Knob colours only on knob values. Drawn from the published params (fxpub: TE's LFO moves it) and
the DSP's state while it runs (fx_lfo_check: stored S + live M draws exactly stored M).

### Checks (`space_check.py`, simulator)
Every kernel and the whole FX bit exact with the C reference; RT60 per band per space within 10 % of the target and
of the prototype (125 Hz .. 2 kHz; the 22.05 kHz tanks 15 % at 2 kHz, where they run 10–12 % short on the longest
spaces); the tail's spectrum within 3 dB of the prototype's; the first reflection on time; 60 s at the extremes
bounded; FREEZE and frozen shimmer within 2 dB over 58 s (measured 1.3 .. 2.0 dB: plain FREEZE sits at the limit); INFINITE layers two chords, line peak
< 0.5 FS on a 0 dBFS pad; exact silence after every tail, nothing above -90 dBFS on the way; no DC; MIX 0 bit exact,
the law and the duck depths (-12.0 / -24.0 dB); no zipper on detent-by-detent turns of every knob (incl. the holds);
the shared lines (boot garbage, pingpong / repeat either way); a clean start after a gap; the green key; TE's LFO
keeps white's zone; CYCLES at both rates. Plus fx_abi_check, fx_lfo_check, fx_lfo_target_check (14 FX ids),
pingpong_check, repeat_check, loopcc (both builds of space.c).

## 12. Review log (MVP, 2026-10-19)
Two fresh subagents: a reverb designer / producer on the fixed-point renders (simulator WAVs,
`scratchpad/space/0*.wav`), and a UX review of the knobs and the placeholder screen.

| review item | change | result |
|---|---|---|
| sound MUST: SUB / GATE / SWELL do nothing | kept (the user's choice: reserved zones), the screen now says `SOON`, greyed | §11 |
| sound MUST: shimmer aliases at 22.05 kHz (6 kHz at +12+19: 4 / 10 kHz at -20 dB) | anti-alias biquad run twice there, corners 3 / 2.2 kHz | 4th order |
| sound SHOULD: TONE doesn't reach the ERs; dark is honky (low cut x2) | the bandwidth low-pass (follows TONE) on the ERs; dark raises the low cut x1.3 at most | |
| sound SHOULD: the duck pumps on 8th notes | follower held 150 ms after the last peak, then ~200 ms; gain recovers over ~400 ms | MIX check: back within 2 s |
| sound SHOULD: grainy shimmer (sidebands at the grain rate) | window 3000 -> 4410 samples (10 Hz), start jitter 0.3 W -> 0.1 W, shimmer ring 8192 | |
| sound SHOULD: quiet tails cut short (toward zero below -72 dBFS) | NEAR_HI / NEAR_LO 8 / 4 -> 4 / 3 (~-78 / -81 dBFS) | silence and limit-cycle checks still pass |
| sound SHOULD: dry + wet hard-clips on a hot master | a 4:1 soft knee above -3 dBFS on the sum (MIX 0 bypasses: bit exact) | peaks <= -1.6 dBFS |
| sound COULD: VOID thin (140 Hz low cut) | 90 Hz (proto.py, regenerated) | |
| sound COULD: short DECAY floors at ~0.25 s; ROOM 2 kHz tick; ER flam; static freeze; INFINITE darkens | not in this round (Thiran interpolation, diffusion vs RT60, ROOM lengths: listed for after the device test) | |
| UX MUST: the CPU line overflows | three lines, `CPU MASTER`, `CPU SYNTH`, `CALLBACK` spelled out | |
| UX MUST: TAIL zone joins are cliffs | the shimmer fades in over its zone's first detent; reserved zones stay dry (the user's choice) | |
| UX MUST: the duck jumps 0 -> -12 dB in one detent | kept: the user's -12 -> -24 dB law (a 1-detent ramp from 0); for the device test to judge | |
| UX SHOULD: MIX's two laws not readable | `DRY 100 WET 50` always, `DUCK` on the master | |
| UX SHOULD: the title repeats the SPACE row | the SPACE row shows the morph, or the anchor's own RT60 | |
| UX SHOULD: INFINITE vs FREEZE unclear; KEY cryptic | `INFINITE  IN OPEN`, `FREEZE  IN MUTED`, `FREEZE HELD` | |
| UX SHOULD: the holds undiscoverable | `> INF` after the multiplier from detent 28 | |
| UX SHOULD: the meter invisible (dark blue) | grey track and ticks, `0DB` at the end | |
| UX COULD: the multiplier's decimals | two always (`X1.00`) | |

## 13. The CPU pass (SPACE2, 2026-10-19)
The simulator numbers were above our tested FX (distortion peaks at 44.5 k) before any SDRAM latency, so the MVP's
kernels were tightened against these targets: synth / drum peak <= 25 k with shimmer, master at 22.05 kHz <= 35 k,
master at 44.1 kHz <= 45 k.

| CYCLES per 64-frame call, mean / max (space_check 14) | idle | busy | VOID x4 | SHIMMER +12+19 | frozen shimmer | INFINITE |
|---|---|---|---|---|---|---|
| master, tank 44.1 kHz (SPACE2) | 38.0k / 39.3k | 37.0k / 38.2k | 37.1k / 38.6k | 42.6k / **44.4k** | 40.1k / 40.4k | 37.4k / 38.3k |
| master, tank 22.05 kHz (SPACE2H) | 26.4k / 27.4k | 25.9k / 26.8k | 26.0k / 27.0k | 30.1k / **31.2k** | 28.6k / 28.9k | 26.3k / 26.9k |
| synth / drum, 22.05 kHz | 18.5k / 19.5k | 18.0k / 18.9k | 18.0k / 19.1k | 22.1k / **23.3k** | 20.6k / 20.9k | 18.4k / 19.0k |

- Against SPACE1: shimmer peaks 63.5 k -> 44.4 k, 44.9 k -> 31.2 k, 34.7 k -> 23.3 k (all three targets met; the
  44.1 kHz master with the least margin). Turning knobs costs more (the costly parameters recompute): a block-by-block
  sweep of all four knobs peaks at 49.9 k / 27.6 k (master 44.1 / mono).
- The shimmer coming on no longer spikes: its ring is cleared 1 KB a call (8 calls at 44.1 kHz, 4 at 22.05) with the
  return muted meanwhile; hall -> shimmer mid-stream peaks at 43.7 k (44.1 kHz), under the steady shimmer peak.
- Bit exact (the kernels unchanged in what they compute):
  - Stage 1: each line's job kept from block to block (the LP's 40-bit state stays in it; `*lo`, saturated, is
    still written, and the next block restarts from it whenever it saturated); the jobs filled by one asm loop, four
    calls by variant; the z and v terms of the LP and of F one dual multiply-accumulate each; F's rounding preset in
    the accumulator (`(F + 4096) >> 13`; |F| < 2^31 - 4096 by the gains' bounds); the in-line allpass's half one
    vector add with shift (toward zero) or one fractional multiply (to nearest); segment splits by a 6-step binary
    search, not the 32-step division.
  - The line writes: two calls (by the input's sign); to nearest even through the accumulator's rounded read-out,
    toward zero as `x + (x >> 29 logical)` (|x| < 2^23).
  - The Hadamard: the loads and stores issued with the butterflies.
  - The half-band decimator and interpolator: two outputs per pass, a dual multiply-accumulate on neighbouring samples
    read as one word (each keeps a copy shifted by one sample for the odd pairs): ~2x.
  - The allpass chains (4 input, 2 ER) one call each; the ER DC blocker + low-pass and the two low-cut high-passes as
    two-pole kernels; the predelay write, the shimmer's input, ring write, return add and the duck's peak follower
    input in asm; the shimmer's four heads one call; its return (gain, cubic limit, up to the tank rate) in asm.
  - Params: the lines' lengths, in-line allpasses and loop delays recomputed only while gliding.
- Re-baselined (the C reference changed with the asm; RT60, stability, silence and every other space_check case
  re-run and passing): the interpolated reads (stage 1 and the shimmer heads) round difference x fraction to nearest
  even (one fractional multiply; was half up), and the in-line allpass's half to nearest no longer saturates z = 32767
  first (16384, the true nearest even). Both sub-LSB.
- Not taken: 3 input allpasses instead of 4 (~1.4 k at 44.1 kHz) needs a sound A/B; the targets were met without it.
  Left for later: the mix knee (~16 cycles a sample), the ER taps' two passes, the shimmer heads' per-segment C.

