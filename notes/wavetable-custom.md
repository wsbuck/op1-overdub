# Custom wavetables for `wavetable` (firmware 246)

Status: **WTS1 built** (2026-10-13): the SAMPLE bank as designed below (src/engine/wt_user.h, wt.c, ui.c wt_user_tick). Not yet on device.
Marks: **Confirmed** = read in our code or the binary, or measured in the simulator; **Hypothesis** = inferred, untested on device.

## Recommendation
**Build a 9th bank, SAMPLE, from the sampler slot that grain already reads.** The bank is built on the UI thread, never on the audio thread.
- Reach SAMPLE by turning the green knob fully clockwise (top 1/32 of its range).
- **"Files" use the same path, with no new firmware hooks:**
  1. Convert a Serum-style WAV (2048 samples per frame) to a mono AIFF on the computer.
  2. Copy it into `synth/user` in disk mode. TE imports it as a sampler preset.
  3. Load it into any sampler slot.
  4. The builder recognises the 2048-sample frame grid and uses those frames as-is.
- **Cost:**
  - **Engine:** about 0 extra audio CPU. wt.c picks a base pointer once per block; the per-sample loop is unchanged.
  - **RAM:** 233 KB of FREE region 2 (two 99 KB banks plus 35 KB scratch).
  - **Code:** about 4.4 KB.
  - **Build time:** 36 M simulator cycles per bank (~90 ms at 400 MHz; device ≈ 1.2–1.4 × sim). It is split into 13 steps of at most 3.2 M (period detection, then one frame per UI frame), so the bank is ready about 13 UI frames (< 1 s) after the sample or trim changes.
- **Not recommended:** reading wavetable files straight off the FAT disk.
  - The disk is a FAT image that tape also streams from.
  - Using TE's FatFs from our hook risks the user's tape and FAT (see "File route").

## What the engine needs — Confirmed (src/engine/wt.c, wt_layout.h, gen_wt_data.py)
- Bank layout: 12 frames × `FRAME_STRIDE` 4128 int16.
  - Each frame has 9 levels. Level L holds harmonics 1..256>>L in `1 << level_bits[L]` samples (2048 … 32), 8× oversampled.
  - Frames are peak-normalized to 0.9 FS over all levels.
  - One bank is 99,072 B.
- wt.c reads only through `look_at()` = `engine_data + (bank*FRAMES + frame)*FRAME_STRIDE + level_off[L]`. For SAMPLE, a per-block base-pointer swap is enough.
- wt.c's 16-bit interpolation needs adjacent samples to differ by less than 32767.
  - Bernstein's inequality bounds any band-limited frame with 8 points per top harmonic: |Δ| ≤ 2π/8 · peak = 23,160 at 0.9 FS.
  - Measured worst on test inputs: 15,356.
- The screen (`ui.c` `draw_wt`/`wt_sample`) reads `wt_data` the same way, so it needs the same pointer swap.

## 1. Sampler route (recommended)

### Source
- Reuse `grain_source.h`, unchanged. **Confirmed** (code).
  - `sampler_slot()`: the first other slot whose engine id is 6, with validated pointers.
  - The window comes from the sampler's own trim knobs 0 and 3.
  - The `+4` peak array (int16 per 256 samples) is available for change detection.

### Algorithm — `src/engine/wt_import.h` (new; portable C, no libc/libgcc, no `/`)
1. **Serum grid test** (`wti_serum`). Take up to 32 frames on the 2048 grid, counted from `audio[0]`, not from the trim start.
   - Extrapolate each frame's last two samples:
     - a table frame continues into its own first sample about as well as into the next frame;
     - a recording continues only into the next sample.
   - Noise is rejected because its interior second differences are large compared with its level.
   - Cost: 3 k cycles.
2. **Otherwise, period detection** (`wti_period`):
   - Box-decimate 4× a centre segment of up to 4096 samples.
   - Autocorrelate lags 5..512, i.e. periods 20..2048 samples (2.2 kHz down to 21.5 Hz).
   - Pick the smallest local maximum within 1/8 of the best one. This avoids octave-down errors.
   - Refine at full rate with AMDF over ±5 samples and a parabola.
   - Give up (fall back to fixed 2048 slices) if the best peak is below 0.3·r0 or the window is silent.
3. **Per frame** (12 evenly spaced over the window; `wti_frame`):
   - Cut the cycle where the signal is flattest and most alike one period later (`wti_cut`), so the seam never lands on an edge. Without this, a pulse frame got H2 = −22 dB; with it, −50 dB.
   - Resample one period to 2048 points (linear). Close the loop seam with a ramp (recordings only; table frames wrap onto themselves).
   - 2048-point fixed-point FFT (radix-2, Q24 data, Q31 twiddles from `sine256`).
   - Drop DC and harmonics above min(256, 0.45·P). Undo the linear interpolation's sinc² droop for P ≤ 1536.
   - Rotate all harmonics so harmonic 1 is in sine phase. Neighbouring frames then line up and the morph does not cancel. CORDIC atan2 is used, with no division.
   - For each of the 9 levels, run an inverse FFT of harmonics 1..256>>L at that level's size. This gives exactly gen_wt_data.py's band limits.
   - Peak-normalise the frame to 29490. One `udiv`.
4. `wti_step()`: stage 0 does the detection, then one frame per call. It returns 0 when the bank is complete and −1 if the window is shorter than one period.

### Simulator results — Confirmed (`src/engine/test/wt_import_check.py`; `--host` for a quick host build)
Simulator and host outputs agree; each case's SNR against the float model is the same in both.

| case | period found | result |
|---|---|---|
| saw 220 Hz (band-limited) | 200.38 (true 200.45) | harmonics 1/n within 0.04 dB to H80; SNR vs float model 70 dB |
| sine 440 Hz | 100.15 (100.23) | pure sine; 78 dB |
| saw 30 Hz | 1470.00 (1470.00) | droop path; 73 dB |
| pulse 110 Hz, width 50 → 10 % | 400.71 (400.91) | frame 0 H2/H1 0.003, frame 11 0.95: the bank morphs as the sample does |
| Serum-style 64 × 2048, random phases | grid (2048) | every frame = its source frame, band-limited (worst harmonic error 0.8 %) |
| noise | none → fixed slices | band-limited, normalised |
| silence | none | all zeros |

- Every case:
  - each level's energy above its harmonic limit is ≤ −72 dB;
  - frame peaks are 29489–29492;
  - harmonic 1 is in sine phase;
  - adjacent deltas are ≤ 15,356.
- Cycles (bfin-elf-run, `-O2`):

  | step | cycles |
  |---|---|
  | detect | 3.2 M (Serum test 3 k) |
  | frame | 2.6–2.76 M |
  | whole bank | 32.6–36.3 M |

  - FFT 2048: 0.87 M forward, 0.84 M inverse (77 cycles per butterfly: GCC uses multi-cycle 32-bit multiplies).
  - Possible later gains:
    - a real-input FFT (×2);
    - 16×16 MAC builtins in the autocorrelation;
    - two frames per FFT.
  - None of these is needed at 13 UI frames per bank.
- Code: 3.6 KB text + 0.8 KB rodata. No undefined symbols (no libgcc).

### Where it runs — design (Hypothesis until built)
- **UI thread:** `_ui_update` (iter page update hook, slot.S → ui.c) when the wavetable screen is shown. One `wti_step` per display frame.
  - TE's own SVG loader does file I/O and heavy work on this thread (svg-route.md).
  - The audio thread never builds anything.
  - Limitation: the hook runs only while our engine screen is visible. After boot, the bank builds the first time the wavetable T1 screen shows (it is the default screen on selecting the synth). Until then SAMPLE plays bank 0.
- **Change detection** each UI frame. The signature is:
  - the sampler slot, `start`, `len`;
  - the sum of the `+4` peaks over the window (≤ 1034 int16 reads);
  - 64 sampled points.

  Rebuild once the signature has been stable for ~10 frames and differs from the built one. This debounces trim-knob turns and recording.
- **Publishing (double buffer):** build into `buf[1 - front]`, then store `front` (one aligned 32-bit store). wt.c reads `front` once per block.
  - The next build does not write a buffer until at least one UI frame after a publish (stage 0 only reads), so the engine never reads a buffer being written.
- **Safety while the sample changes:**
  - Reads go through `grain_source.h`'s validated pointers, inside the 264,608-sample buffer.
  - `wti_*` reads at most `x[P + 2]` inside the window, and the window plus P + 3 is checked by `wti_step`.
  - A recording during a build gives at worst one odd bank, replaced after the signature settles. Faults are not possible.
  - If the sampler slot disappears, the last published bank stays playable: it is our copy.
- **Selection:** SAMPLE = green knob ≥ 31744 (bank = `k >= 31744 ? 8 : min(k >> 12, 7)`).
  - Every existing preset value (b·4096 + 2048) keeps its bank, so DB presets and saved patches do not change.
  - The screen gets a 9th pip, the title `SAMPLE` (plus slot number), and build progress (frames appear as they finish).
  - With no sampler slot and nothing built yet: SAMPLE falls back to bank 0 and the screen says `NO SAMPLE`.

### Memory — Confirmed (canary, LOG.md 2026-10-06; build outputs 2026-10-11) / proposal
- FREE region 2 `0x02C00000–0x02D00000`: all 1 MB was clean at runtime in CANARYLOG. The wavetable engine block is 794,552 B, ending at `0x02CC1FB8`.
- Proposal:
  - `0x02CC4000` header (magic, front, generation, P, source signature; 64 B);
  - `0x02CC4040` + 2 × 99,072 B banks;
  - `0x02CF4700` `wti_work` (34,952 B);
  - the region ends at `0x02CFCF88`, leaving 12 KB spare.
- Build changes:
  - lower engines.json `wavetable.max` to `0xC4000`, so a larger engine cannot grow into the banks;
  - `--reserve 0x02CC4000-0x02D00000=wt_user`;
  - one 64 B zero FILL block for the header, so a cold boot never sees a stale magic (FREE RAM is undefined at boot).
- Alternative for the scratch: FREE region 1 tail `0x01EE0000–0x01F10000` (192 KB above pingpong's lines). No user is known; it was clean in the canary test.

### Firmware hooks
**No new TE hooks.** The design reuses:
- the iter page update pointer (UI thread);
- `te_engine_ids` `0xFF8016E4`, `te_ssamp` `0x01DF6F78`, `te_knobs` `0xFF8013C4`, `te_synth_slot` `0x01AAAC0C` (already defsym'd for grain).

The rest is ours: a defsym for the shared block's address in both links, wt.c's pointer swap, and ui.c's 9th zone and step call.

## 2. File route

### Disk mode = a FAT image on YAFFS
- **USB mass storage** reads and writes sectors of one YAFFS file through TE's YAFFS API:
  - fd at `0x018C180C`;
  - read `0x01A7153C`: `lseek(fd, sector << 9)` `0x01A9A0E4`, then `read` `0x01A99C90`;
  - write `0x01A715DC`: `write` `0x01A99F84`;
  - asserts `usb_ms_yaffs.cpp:43–57`.

  **Confirmed** (disassembly).
- **The file:** `0x01997250` builds `"%s%s"` from a prefix (Hypothesis: `/yaffs2`) and `"/user/tape_c.raw"` (`0x018D9E98`). It then calls `yaffs_open(path, 2 /*RDWR*/, 0x180)` (`0x01997294`) and stores the fd at `0x018C180C`.
  - The open and the fd store are **Confirmed**; the prefix is a Hypothesis.
  - This is the "121 MB tape file" of grain.md: tape tracks are AIFF files inside this FAT image. `Defragmenter.cpp` keeps them contiguous; `fat_manager.cpp` handles `/tape/track_%d.aif` and `/album/side_a.aif`.
- **FAT root directories:** `/synth`, `/drum`, `/album`, `/tape` (`0x018D9DE8–0x018D9E00`), on FatFs drive `0:` (`0x018D9DE4`; `FR_OK` asserts). **Confirmed** (strings and code references).
- **After disk mode:**
  - `filesync_common.cpp` / `filesync_yaffs.cpp` / `change_tracker.cpp` ("New:/Changed:/Removed: %s", `/log/files.txt`) sync `/synth/<folder>/*.aif` into `%s/user/audio/preset_synth/%s/%s.raw` (`0x018C7BA4`), and `/drum` likewise. Only `.aif` patterns appear (`%s/%s/%s.aif` `0x018DB2D8`).
  - **Hypothesis** (strings, plus the community's import behaviour; sync code not traced): non-AIFF files (`.wav`) are not imported, and an imported AIFF is stored as raw int16 on YAFFS.

### Options
| option | verdict |
|---|---|
| Read a user file straight from the FAT (TE's FatFs `f_open`/`f_read` from our UI hook) | **No.** FatFs re-entrancy is unknown and tape streams from the same volume. A concurrent access could corrupt the user's tape or FAT. Also needs FatFs entry points (not located). |
| Read TE's imported raw `/yaffs2/user/audio/preset_synth/<folder>/<name>.raw` by a fixed name (e.g. `user/wavetable`) with `te_yaffs_open/lseek/read/close` (known addresses; UI thread, load-once) | Possible step 2. Needs a malloc'd or FREE 529 KB staging buffer (or chunked reads into the builder) and the raw format confirmed (header? pad?). Benefit: no sampler slot used. |
| **Convert on the computer, import through `synth/user`, load into a sampler slot** | **Recommended.** TE does the file handling. Our builder sees a normal sampler buffer and detects the 2048 grid. Limit: 6 s = 129 frames of 2048 (we sample 12 evenly). |
| Bake banks in at build time (`gen_wt_data.py` reading WAVs) | Zero device risk, but needs a reflash. Good for our own factory banks, not for users. |
- **File format:**
  - Serum-style is 2048 samples per frame, often 256 frames (11.9 s; a `clm ` chunk marks the frame size).
  - The OP-1 sampler takes 6 s, so the host converter keeps ≤ 129 evenly spaced frames, writes them on the 2048 grid from sample 0, and saves mono 16-bit 44.1 kHz AIFF.
  - Raw int16 files are not imported by TE (Hypothesis), so AIFF is the only practical container.
- **Unknowns (Hypothesis):**
  - Does TE's import keep sample 0 at `audio[0]`? The grid test needs frames aligned to the buffer start.
  - Does the import normalise, fade or trim silence? A fade would only change the edge frames.

## 3. Failure modes
- **No sampler slot:** last bank, else bank 0 + `NO SAMPLE`.
- **Unpitched source:** fixed 2048 slices. The result is band-limited but noisy, as Serum's "noise" import.
- **Octave-up error:** the detector picks 2P, giving two cycles per frame, so the bank sounds an octave up. Mitigated by the smallest-near-max lag rule; not eliminated.
- **Pitch drift or vibrato:** one period for all frames. The seam ramp hides small mismatches. Re-detecting per frame would cost +0.3 M cycles each.
- **Serum test errors:**
  - a table that is not aligned to the buffer start fails the test → pitch detection → usually a period of 2048 or a divisor of it (still works, possibly sliced differently);
  - a recording whose period divides 2048 exactly is treated as a table (harmless: same frames).
- **Weak fundamental:** phase alignment is skipped when |H1| < max/32 (L1). Morphs between such frames can partly cancel.
- **Build while recording:** an odd bank for < 1 s, then rebuilt.
- **Patch portability:** a saved patch with SAMPLE depends on the sampler slot's content at load time.

## 4. Test plan (one change per image)
1. **Sim** (done): `python3 src/engine/test/wt_import_check.py`, 7 cases, all checks pass. Next sim step: give wt.c a SAMPLE base pointer to a bank built by `wti_step` and run wt_check's aliasing and glide cases on it.
2. **WTS1:** SAMPLE bank (shared block, wt.c pointer swap, ui.c step call, 9th zone; debug line shows P, mode, step CYCLES). Then:
   1. load the factory sampler with a sustained sound into another slot;
   2. on wavetable, turn green fully clockwise;
   3. play.

   Pass:
   - the sample's timbre plays at keyboard pitch, and blue morphs through it;
   - DSP % unchanged vs bank 0;
   - UI frame time + ≤ 10 ms during a build;
   - bank ready < 1 s.
3. **WTS2:** the same image, file path. Convert a Serum table on the computer, import it via `synth/user`, and load it into the sampler. Pass: debug shows table mode (grid), and the 12 frames match the host preview of the same file.
4. **WTS3:** the same image, robustness. Record into the sampler while SAMPLE plays; move the trims; change the sampler slot's engine away; reboot. Pass: no fault, rebuild after each settle, last bank kept, `NO SAMPLE` / bank 0 only before the first build.

## Files
- `src/engine/wt_import.h`: builder (header-only, for the slot or engine block).
- `src/engine/test/wt_import_main.c`: simulator/host driver (stdin sample → stdout bank, stderr cycles).
- `src/engine/test/wt_import_check.py`: checks and float model.
