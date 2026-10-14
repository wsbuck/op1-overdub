# Audio input: can an engine hear the mic/line input? (firmware 246)

Static research 2026-10-13 (decrypted L1 disassembly and Ghidra decompiles), for a guitar-amp engine like the
OP-1 Field's "amp". **Confirmed** = read in code, **Hypothesis** = inferred. Device test: AMPPROBE.OP1 (LOG.md, test image 55).

## Answer first
- **`0xFF800CB4` is written at the very start of every audio callback, before the voice loop** (fill loop `0xFFA01A30`
  or `0xFFA030D0`; the mode dispatch that leads to `FUN_ffa009a0` comes after, at `0xFFA01C0E`). An engine's render
  reads the **current** block's input, no block of latency. **Confirmed.**
- Format: **int16, mono, 64 samples at 44.1 kHz** (one codec block); the engine renders 128 at 88.2 kHz, so it must
  upsample 2x. **Confirmed.**
- It is filled **whether or not the input is switched on**: the on/off state (`0xFF8010B0`) only gates the meter and
  the mixing. Whether the ADC still delivers samples while the input is off: **Hypothesis**, AMPPROBE tests it.

## The callback's inputs — Confirmed
- SPORT0, 32-bit words, stereo frame sync (`SPORT0_RCR2` = 0x21F). RX = DMA2 (PMAP 3), descriptor ping-pong over two
  halves **`0xFF800370` and `0xFF800570`**, 128 words each = 64 frames of {L int32, R int32}. TX = DMA1 (PMAP 4),
  `0xFF800770` / `0xFF800970`. Set up in `FUN_0191ab40`.
- ISR `0xFFA0047C` (IVG11, on TX done): `[0xFF800364]` = (DMA2 START_ADDR == `0xFF800370`), i.e. 1 when the DMA is
  filling the first half; calls `FUN_ffa04804(out = 0xFF800770 + toggle * 0x200, in = 0xFF800370 + [0xFF800364] * 0x200)`.
  So the callback gets the **completed** RX half; the other is being filled.
- `FUN_ffa04804` → `FUN_ffa017ec(out, in, 0x01AB2E2C)`. `0x01AB2E2C` is 64 frames of int32 L/R resampled by
  `FUN_019e193c` from a 512-frame ring at `0x01AADB60` (zeros when its source is idle). Hypothesis: USB or radio audio.
- Sample alignment: TE takes the top 16 bits of (L + R), so it treats the words as MSB-aligned. Hypothesis: 24-bit
  codec data, MSB-aligned.

## Conversion into `0xFF800CB4` (FUN_ffa017ec, state at `0xFF801040`) — Confirmed
- Source pointer: the RX half; **the alternate stream `0x01AB2E2C` when `[0xFF8010B4]` = 3**. Skipped entirely if the
  pointer is 0 (the ISR never passes 0).
- `[0xFF8010B4]` = 0 (mic) **and** byte `[0x01BA15DC]` != 0: per frame `sat((L << 2) * 0.795 + (R << 2) * 0.795)`, top
  16 bits: **x3.18 = +10 dB, digital**. Otherwise: `sat(L + R)`, top 16 bits (unity, channels summed).
- `[0x01BA15DC]` is the "+10" step of the mic boost set by `FUN_0191b200` (audio.cpp:918): 0 / 10 / 20 / 40 dB; 10 is
  this digital gain, 20 and 40 are codec register bits (`0x01BA15EE`, TWI address 0x1A). On units with the panel flag
  `0xFF906A54` set (this unit) another path `FUN_01961ada` is used instead. Hypothesis: a different codec revision.

## The "mode dependent" mixing — Confirmed (meaning of the flags: Hypothesis)
| state | what happens to `0xFF800CB4` |
|---|---|
| `[0xFF8010B0]` != 0 (input off) | nothing: no meter (`0xFF801086` = 0), no mixing |
| on, `[0xFF8010B4]` != 0 (line/alt), or byte `[0x018B621D]` != 0 | after the synth FX, before the mixer `FUN_ffa00e50`: `0xFF800BB4 += CB4 / 2` (monitored) |
| on, mic, `[0x018B621D]` = 0 | after the mixer, before the tape write `FUN_ffa06334`: `0xFF800BB4 += CB4` (to tape, not monitored) |
- `[0xFF8010B0]`: 0 on, 2 off, 1 when `FUN_019dead6` returns 4; written via `FUN_019672f0` from the key handler
  `FUN_019ac4b4` event 0x2b (Hypothesis: the mic key; opens the input screen on sampler/drum instead) and tape record.
- `[0xFF8010B4]`: written via `FUN_019672d8` by `FUN_019deaec(this, on)`, on event 0x38 (which also sets byte
  `0x018B621C`). Hypothesis: 0x38 = line jack in, 1 = line.
- Byte `[0x018B621D]` = 1 on event 0x39, which also turns the speaker amp off (`FUN_0195b1fc(0)`) and changes a codec
  output register (`FUN_0191ae68(1)`). Hypothesis: headphones plugged in, so the mic is only monitored on headphones
  (no feedback through the speaker).
- Input meter `FUN_01967614` (only when on): peak |CB4| → `0xFF801086`, held peak `0xFF801088` (held 689 blocks ≈ 1 s).
- If `[0xFF8010AC]` != 0, `FUN_01a3464a` copies CB4 into a recording buffer once a threshold is crossed (the sampler's
  record trigger, Hypothesis).

## Candidate buffers for an engine
| buffer | format | notes |
|---|---|---|
| `0xFF800CB4` | int16 x 64, mono | TE's converted input, gain as above; current block |
| `0xFF800370 + [0xFF800364] * 0x200` | int32 L/R x 64 | the raw block the callback converted; both channels separately |
| the other half | int32 L/R x 64 | being filled by DMA during the callback |
| `0x01AB2E2C` | int32 L/R x 64 | alternate stream (mode 3) |

## Probe engine (AMPPROBE.OP1)
- `src/engine/inprobe.c`, `inprobe.h` (shared with the screen in `src/slot/ui.c`). engines.json `"test": true`: only
  built when named (`build.py --engines grain,inprobe`). Preset "input": LFO and FX off, full sustain.
- Blue gain 0..+24 dB; green source SILENCE / TONE / TE IN / RAW / RAW OTHER / ALT; white L+R / L / R; orange shift
  0..8 bits (int32 sources). 2x linear upsampling, soft knee. Every sounding voice plays the input.
- Screen: source name and address, the block after the gain (64 samples), peak meter (dBFS, held peak), frame
  counter, LIVE / STILL (block changed since the last frame) with a count over 32 frames, the first raw word in hex,
  render calls per frame.
- Simulator: `src/engine/test/inprobe_check.py` (bit exact per source/channel/shift, guard, cycles ≤ 8.4 k per call,
  ABI).
