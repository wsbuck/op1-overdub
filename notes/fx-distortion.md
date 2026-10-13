# Distortion FX (`distortion`, was `drive`; source `distortion`, firmware 246)

**Status: MUFF character added (2026-10-10), sim-checked (`src/slot/distortion.c`, `src/slot/test/distortion_check.py`: all pass except MUFF + green filter CPU, see below; `fx_abi_check.py` 9/9). Not on device.** History: first version (tube / hard / fold, +24 dB, tilt) too tame → five characters, resonant pre-filter, envelope dynamics (FXUI1) → drive taper (DIST2, notes/LOG.md) → MUFF, the user's favourite, proposed as the default character.
FX contract and slot mechanics: fx-slot.md. Confidence: **Measured** = simulator (bfin-elf-run), **Hypothesis** = not checked.

## Knobs

| knob | role |
|---|---|
| blue | drive, through `drive_taper()` (t = 1 − (1 − k)²(1 − 0.3k): the upper half of the knob is the heavy range, half way is already +27 dB; DIST2). Tube / muff / fuzz / clip: 0.5 × 2^(7t), 0.5 … 64 (−6 … +36 dB): a −6 dBFS sine goes from a quarter of the clip level (clean) to 32× over it. Fold and crush have their own laws (below). Loudness is compensated per character, so blue changes the dirt, not the volume. Blue also darkens the top: a shelf above ~4 kHz, to −9 dB at max (fizz control; half that in crush) |
| green | voice: a resonant state-variable filter **before** the shaper, so the distortion screams through its peak. 16384 ± 2048 = flat (the output is exactly the input there, whatever the filter state). Down: resonant low-pass, 6 kHz → 150 Hz, Q 0.9 → 5. Up: "focus", a band-pass peak (+9.5 dB at Q 2.5, everything else −12 dB) sweeping 350 Hz → 4 kHz: a cocked wah into the shaper |
| white | character, six plateaus 3072 wide with 2867-wide crossfades (6 × 3072 + 5 × 2867 = 32767): see the table below |
| orange | mix: dry … wet; 0 is bit-exact dry |

| white | plateau | centre | name switches at |
|---|---|---|---|
| TUBE | 0 – 3071 | 1536 | – |
| MUFF | 5939 – 9010 | 7475 | 4506 |
| FUZZ | 11878 – 14949 | 13414 | 10445 |
| CLIP | 17817 – 20888 | 19353 | 16384 |
| FOLD | 23756 – 26827 | 25292 | 22323 |
| CRUSH | 29695 – 32767 | 31231 | 28262 |

- **Proposed fx.json default: `[16000, 16384, 7475, 32767, 0, 0, 0, 0]`**: MUFF, blue 16000 (taper → +21 dB of drive: a classic thick sustain; the bottom of the knob is already thick, the top is wall-of-fuzz), green flat (16384: MUFF's voice comes from its own fixed scoop; also the cheapest setting), full wet. Keep `"aliases": ["drive"]`. No FREE SDRAM buffer needed.
- Saved patches with white in the old 5-plateau layout now land on other characters (e.g. old FUZZ 9216 → MUFF); the knob is not migrated.
- Why mix (not output level): auto gain already levels every setting; mix gives parallel distortion (keeps transients and low end). The wet path has a few samples of non-linear-phase delay, so 50 % mixes comb gently in the top octave.

## Characters (shaper units: 1.0 = clip level)
- **MUFF** (our own take on the Big Muff topology): stage 1 = the tube's soft knee on u + bias (the tube bias, 0.3 above blue 8192) with ADAA (the same kernel; in a TUBE↔MUFF crossfade it runs once for both); coupling high-pass (one-pole, 110 Hz: lows thick but not flubby; also takes stage 1's DC); gentle low-pass (one-pole, 4 kHz: creamy, and band-limits what stage 2 clips); stage 2 = a − a³/3 on a = 4·lp + 0.2 saturated at ±1 (a gain of 6 after stage 1, smooth knee, +0.2 bias for some even harmonics; pointwise at 88.2 kHz); then the tone stack's fixed mid scoop in pass 4: low (one-pole 250 Hz) / mid (to 4 kHz) / high bands with the mid × 0.3 (−8.3 dB at 1 kHz, −2 dB at 200 Hz, −3.4 dB at 5 kHz; model), faded with MUFF's weight. Blue = sustain (drive into stage 1; stage 2 always clips). Loudness compensated +5 dB over plain level matching (half the scoop at 1 kHz), so lows come out a little louder than dry and 1 kHz ~4 dB under. Stage-2 DC (s2(0.2)) subtracted. No envelope, no gate: the contrast with FUZZ.
- **TUBE**: s(v) = v − 4v³/27 on |v| ≤ 1.5, ±1 beyond, on v = u + bias. Bias 0.3 fades in over blue 0…8192 (blue 0 stays clean), slewed ≤ 1/64 of its range per block; its DC s(bias) is subtracted. 2nd harmonic −24 dB at blue 8192. At +36 dB it is nearly a square.
- **FUZZ** (gated octave-up rectifier, Octavia-like): y = clamp(u/8 + |u| − off, 0, 1). off = the slow envelope of mean |u| (5 ms attack, 190 ms release; capped at 1.5× the fast envelope so decays splutter instead of muting) + a gate threshold min(0.004 g, 0.2) (≈ −48 dBFS at the input). In steady state off = mean |u|, so both half-waves poke through by different amounts: a pulse train at 2f with alternating widths. 2nd harmonic +4 (blue 8k) … +15 dB (max) over the fundamental. As a note decays the slow offset lags: pulses narrow, then the gate closes (output exactly 0, no DC). Implemented as hard clip (ADAA) of 2w − 1; its steady-state mean (table `fuzz_dc` over log2 of the fast envelope) is subtracted, so notes starting and stopping make no DC step.
- **CLIP**: clamp(u, −1, 1); odd harmonics only.
- **FOLD**: triangle through (0,0), (1,1), (3,−1), (5,1), …, odd, flat beyond ±17 (8 folds per side). Depth 2^(4k/32768) = 1 … 16 (a −6 dBFS sine: up to 4 folds per side; the drive saturation at ±32 limits it to ±8 at max blue). The slow envelope adds a bias (0.25 × env × depth, ≤ 0.5: even harmonics on loud parts, fading with the note); its DC is subtracted.
- **CRUSH** (aliasing on purpose): clamp(1.2 × 2^(k/32768) × x, ±1), sample-and-hold at 62 kHz → 1.9 kHz (2^(−0.5 − 5b) of 88.2 kHz) and rounded to a step of 2^−8 (9 bits) → 2^−2 (3 bits) of the clip level, in quarter-octave steps. No ADAA, no anti-imaging: the images are the sound.
- Loudness compensation `comp_tab[shape][blue]` (17 points, Q12): rms(−6 dBFS 1 kHz sine) / rms(shape, DC removed), float model below. Mix weights include it: a_s = w_s c_s / Σ w c, post gain Σ w c, so a crossfade between shapes of very different raw level stays level.
- The filter's peak gain (table `svf_lgl` / `svf_lgr`, log2, from the float model of the SVF and its output mix) counts as extra drive for the compensation (3/4 of it, per shape gain law: 7 octaves for tube / fuzz / clip, 4 fold, 1 crush), so a resonant peak at low drive cannot push the output to full scale.

## Dynamics and stereo
- Envelopes per instance (shared by L/R): mean |filtered input| per block; slow (attack 5 ms, release 190 ms) for the fuzz offset and the fold bias, fast (2.9 / 7 ms) for the fuzz's DC estimate and the offset cap.
- Master (stereo) only: L gets −1 dB and R +1 dB of drive (scaled by blue: none at blue 0, so clean settings stay centred); the crusher's R rate is 6 % higher. Fold depth, fuzz gating and the crusher's images differ between L and R: width.

## Signal path (per 64-frame block)
0. Per channel: filter. Chamberlin SVF at 44.1 kHz: hp = x − lp − d·bp; out = cl·lp + cb·(d·bp) + ch·hp from the states before the update (so out = x exactly at cl = cb = ch = 1, whatever f and d); bp += f·hp; lp += f·bp. Coefficients from 17-point tables per side (all pole radii < 0.999); f and the mix weights glide per sample, d per block. Skipped at exactly 16384 (pass 1 then reads the int16 input itself; the envelope takes every 4th sample). Mean |out| for the envelopes. Inline assembly, d waits in B0 (L0 = 0).
1. Drive (glided, ±1 dB stereo trim), saturated at ±32 (a saturating shift: keeps the ADAA's F differences in 32 bits), 3-tap treble lift `e = v1 + (2v1 − v0 − v)/8`, 2× up by linear interpolation (88.2 kHz); the tube bias is added here while TUBE or MUFF has weight.
2. Each active shape (at most two: plateaus are wider than the per-block character slew) into its own buffer: fuzz input (assembly), fold input (assembly), the kernels with first-order **ADAA** as before (closed-form mean inside a piece, recomputed (F(b) − F(a))/(b − a) for pairs straddling a breakpoint, at most **FIXMAX = 32** per shape and block, **none** while two crossfade: the closed-form mean only, a CPU bound), MUFF's post-stage (assembly), or the crusher (C). Crossfade (assembly) with the glided normalised weight.
3. 2× down: polyphase IIR half-band (HIIR, 4 coefs, transition 0.1; flat to 20 kHz, 69 dB stop band from 26.5 kHz). Assembly; d0/d3 (the previous inputs) are read back from the buffer through I0 (+4 −8 +4 −8 +20), three of the four loads issued in parallel with MACs.
4. The shapes' DC (tube s(bias), fold bias, fuzz mean, muff s2(0.2), each × its weight), bands split at 4 kHz (one-pole, step 0.4375) with loudness and fizz cut; while MUFF has weight a third band (one-pole 250 Hz of the 4 kHz low-pass, step 2^−5 + 2^−8) gives the mid band its own gain (loudness × (1 − 0.7 × MUFF weight), glided in a data register); low + mid + high = x at equal gains, DC blocker (6.9 Hz), 16-bit saturating mix. Assembly.
- Glides: everything linear over the block; drive and character also slew 2048 per block (23 ms full range), green 1024 (47 ms). The DAG updates (`I += M`) of the glides are issued in parallel with MACs (checked bit-identical to the sequential form).
- Fixed point: filter Q26, shaper input Q20, output Q24; 32×16 multiplies with Q15/Q14/Q13 coefficients. No runtime division (the per-block weight normalisation and the fuzz DC use a bitwise 15-step divide / shifts), no 64-bit, no floats.

## Screen functions (pure C, no state, no asm; build alone with `-DDISTORTION_CURVE_ONLY`, e.g. on the host: bit-identical to the Blackfin build)
- `int32_t distortion_curve(const int16_t *p, int32_t x);` static transfer curve at knobs p[0..3]: output Q15 (±32767) for input x (Q15), with drive, the character crossfade and loudness compensation; without the filter, shelf, DC blocker, envelope dynamics (fuzz offset/DC and fold bias as for a steady −6 dBFS sine) and mix. Fuzz in its bipolar form (gate = −1 + its mean); crush: the quantiser's staircase (no rate reduction). Includes the drive saturation at ±32 (visible as the fold's flat ends at max blue).
- MUFF's curve: s2(4 (s(u + b) − s(b)) + 0.2) − s2(0.2) (both stages, no filters; s(b) ≈ b below blue 8192).
- `const char *distortion_mode_name(const int16_t *p);` nearest character of p[2]: "TUBE", "MUFF", "FUZZ", "CLIP", "FOLD", "CRUSH" (boundaries at the crossfade midpoints 4506, 10445, 16384, 22323, 28262).
- Cost (simulator): 210–305 cycles per call on a plateau (MUFF the heaviest), up to ~450 inside a crossfade (two shapes evaluated; TUBE→MUFF shares stage 1: ~300). No tables in the per-call path except comp (one packed load) and fuzz_dc; exp2 is a cubic, not a table (loads are slow in the simulator).

## Memory
- `.data`: `mono_st`, `st_st` (separate: synth FX and master FX can run in the same callback) 248 B each; scratch `sc` 3.4 KB **shared** by mono and stereo (TE calls them one after the other; `fx_abi_check.py` checks interleaving = each alone). `.rodata` 0.8 KB. Object: text 9.6 KB, data 3.8 KB, no `.bss`.

## Measurements (simulator, `python3 src/slot/test/distortion_check.py`, −6 dBFS clean sine; blue values are knob positions, through the taper)

| | |
|---|---|
| mix 0 | bit-exact dry, mono and stereo |
| blue 0, tube / clip | level +0.00 dB; THD −52.6 / −100 dB |
| THD vs blue 0/8k/16k/24k/max (1 kHz) | tube −52.6/−12.0/−8.9/−8.9/−9.0; muff −13.8/−4.9/−6.5/−7.7/−8.0; fuzz +5.1/+5.2/+14.3/+15.3/+15.5; clip −100/−11.6/−8.8/−8.9/−9.0; fold −102/+1.0/+40.6/+9.1/+8.9; crush −63/−52/−49/−41/−33 dB |
| MUFF harmonics 2–7 re the fundamental, blue 8k / 16k / max | −19/−9/−18/−13/−17/−17; −30/−10/−29/−13/−29/−16; −36/−11/−36/−15/−35/−18 dB: odd-dominant, smooth odd series, even 20–36 dB down |
| MUFF sustain: 200 Hz note decaying 36 dB (0.25 → 0.85 s), blue 16000 | output 7.3 dB lower, still at −13.9 dBFS (no gate). FUZZ, same: 34.4 dB lower, at −51.7 dBFS |
| MUFF scoop, noise (−12 dBFS) re CLIP at the same drive | 80–250 Hz +6.0, 0.7–1.4 kHz +0.1, 5–8 kHz −0.4 dB: mids −5.9 dB re the lows, lows kept |
| fuzz: 2nd harmonic re fundamental, blue 8k/16k/24k/max | +4.7 / +13.8 / +14.8 / +15.0 dB |
| fold at max | harmonics 5–19 +3.3 dB re 2–4; 12 harmonics within 20 dB of the strongest |
| crush: inharmonic (aliasing/images) re harmonics, blue 0/8k/16k/24k/max | −43 / −19 / −8 / −1 / +2 dB |
| loudness vs dry (1 kHz), blue 0/8k/16k/24k/max | tube 0/−0.1/−0.4/−0.5/−0.5; muff −3.9/−3.6/−3.9/−4.0/−4.1 (the scoop at 1 kHz); fuzz −1.4/−0.4/−0.9/−1.1/−1.2; clip 0/−0.1/−0.4/−0.5/−0.5; fold 0/−0.4/−2.0/−3.2/−3.6; crush 0/−0.1/−0.2/0/−0.3 dB |
| aliasing at max blue, inharmonic to 20 kHz: 5 kHz / 2 kHz | tube −38.9 / −47.5; muff −36.7 / −41.3; clip −38.8 / −46.6; fold −6.6 / −25.3 dB. MUFF: with ADAA on stage 2 instead of stage 1 it was −26.5 / −29.3 (stage 1 clips hardest at high drive; the 4 kHz low-pass band-limits what stage 2 sees) |
| DC at max blue, 100 Hz | tube +1, muff +1, fuzz +1, clip +1, fold +16, crush −16 LSB |
| after the input stops (0.6 s) | peak 0–2 LSB, every character |
| knob jumps (drive ×3 incl. muff, green ×4 incl. muff, character ×2 incl. tube → fuzz through muff, mix, all) | largest step ≤ 1.25 × the largest steady-state step over every setting the slew passes through: no click |
| range, 32 corner cases | none saturating (peak 28843) |
| curve vs DSP | tube/clip (100 Hz, blue 5941/11178): mean error 370–920 LSB; muff (300 Hz, rms): −4.1 dB = the scoop's −3.5 dB at 300 Hz within 0.6 |

CPU (CYCLES per call, mean; stereo unless noted):

| case | cycles |
|---|---|
| default (16000, 16384, MUFF), 200 Hz / 1 kHz | 28751 / 29575 |
| MUFF max blue, 1 kHz | 29575 |
| **MUFF with green on (16000, 20480)** | **34221: over the 30 k typical budget** (filter 5.1 k + MUFF post-stage 5.1 k + 3-band 1.1 k over the tube) |
| tube max / fuzz 20000 + focus / fuzz max / clip / fold / fold max / crush, 1 kHz | 23144 / 28959 / 24126 / 25500 / 26914 / 27874 / 23838 |
| worst: max blue, green max, 5 kHz, MUFF/FUZZ crossfade | 44502 (peak 44681) |
| mono | about half: 16705 at most |

Per pass (stereo, MUFF default): block 1.0 k, pass 1 3.2 k, pass 2 15.4 k (stage-1 kernel ~8.2 k + fix-ups, post-stage ~5 k), half-band 4.2 k, pass 4 5.1 k (4.0 k without MUFF). The green filter adds ~5 k to any character.

## Derivations
- MUFF float model (88.2 kHz, numpy), for its `comp_tab` row (+5 dB): `y1 = s(g x + bt)`, `z = y1 − LP(y1, 2^−7)`, `l = LP(z, 1/4)`, `a = clip(4 l + 0.2, ±1)`, `y = a − a³/3`; one-pole `LP(x, k): st += (x − st) k`. Scoop model: `L250 + 0.3 (L4k − L250) + (1 − L4k)` with one-pole sections at 44.1 kHz.
- Float model (numpy) of the shapes, used for `comp_tab` (17 points per shape, Q12; fuzz of its bipolar form 2y − 1, first point capped at 16000 from 18005 so the Q13 post gain fits):
  ```
  s = lambda v: np.where(abs(v) <= 1.5, v - 4*v**3/27, np.sign(v))
  fold = lambda u: (lambda p: np.where(p < 2, p - 1, 3 - p))(np.mod(np.clip(u, -17, 17) + 1, 4))
  x = 0.5*np.sin(2*np.pi*1000*np.arange(44100)/44100); rms = lambda y: np.sqrt(((y - y.mean())**2).mean())
  for i in range(17): k = min(2048*i, 32767); b = k/32768; g = 0.5*2**(7*b); gf = 2**(4*b); gc = 1.2*2**b
      tube = s(g*x + bt) - s(bt), bt = 0.3*min(1, 4*b)
      fuzz = np.clip(g*x/8 + abs(g*x) - off, 0, 1), off = mean|g x| + min(0.004*g, 0.2)   (comp / 2)
      clip = np.clip(g*x, -1, 1); fold(gf*x + bf) - bf, bf = min(0.5, 0.25*mean|x|*gf)
      crush = round(clip(gc*x)/q)*q, q = 2**(-8 + 6*b)        ->  round(rms(x)/rms(y) * 4096)
  ```
- `fuzz_dc`: mean of clamp(u/8 + |u| − e, 0, 1) for a sine with mean |u| = e, at log2 e = −3 … 5 in half octaves (Q15); 0.215 e below e = 1/8.
- SVF tables: f = 2 sin(π fc / 44100) (Q15), d = 1/Q (Q14); down side fc = 6000 (1/40)^s, Q = 0.9 × 5.56^s; up side fc = 350 × 11.43^s, Q = 2.5; s = 0 … 1 in 16 steps. Stability: eigenvalues of the state matrix [[1 − f², f(1 − fd)], [−f, 1 − fd]] all < 0.999. `svf_lgl/lgr`: log2 of the max |H| of the mixed output (Q12 octaves), dead zone ±2048 and mix ramp m = clamp((|tg| − 2048) × 5 / 16384) included.
- exp2: 2^f − 1 ≈ f (0.69568 + f (0.22693 + 0.07739 f)) (Q15 22796, 7436, 2536; 1.7e-4, exact at octaves).
- Half-band coefficients and `recip_tab` as in the first version: HIIR `compute_coefs_spec_order_tbw(coefs, 4, 0.1)` quantised to Q15; `round(2**14 / (1 + i/64))`.

## Open
- MUFF with the green filter on costs ~34 k per stereo call (budget 30 k typical). Options if it matters on device: a cheaper filter, or no filter while MUFF has weight (green would then be inert for MUFF).
- MUFF on device: the sustain/scoop balance and the default blue (16000) by ear.
- On device: cycles (simulator model vs BF524), the sound (especially the fuzz gate/offset release, the fold bias, the default), the shared-scratch assumption.
- Fold aliasing on bright, hot input is FIXMAX-bound (CPU); a 5 kHz sine at max depth is mostly aliases (−6.6 dB).
- The green sweep is slewed (47 ms full range) and the resonant low-pass rings when swept fast through a strong partial, as an analog one would.
- Screen: `distortion_curve` / `distortion_mode_name` for ui.c (being built separately).
