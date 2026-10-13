# Granular engine (`grain`, src/engine/grain.c)

## Source: the OP-1's own sampler, else the built-in bank — fw 246
- If another sound slot holds the synth **sampler** (engine id 6), grain granulates that slot's sample inside the sampler's
  own start/end trim (its knobs 0 and 3). Otherwise it uses the built-in bank (6 synthesised sounds, `gen_grain_data.py`).
- Static research 2026-10-08 (subagent; decompiles in the session scratchpad `sampler/`), key claims re-checked (Verified):
  - `g_ssamp[8]` at `0x01DF6F78`: per-slot `SynthSampler*` holders, created once by `FUN_01a51d74` (Core init); only
    the init code and the bounds-checked accessor `FUN_01a51de8` reference it (**Verified**, xrefs).
  - holder `+0` SampleBuffer*, `+4` base pitch ratio. SampleBuffer `+0` int16 *data (calloc), `+4` int16 peak per 256
    samples, `+0x1038` length 264608 = 8 pad + 264600 (capacity `0x40998` set in R7 at `0x01a51d7a/80`, **Verified**).
  - Loaded at boot (`/yaffs2/user/audio/synth/synth_%i.raw`, raw int16) only for slots whose patch type is "sampler";
    presets and recording write the same buffer; switching engines does not free it.
  - Sampler knobs: Q15 fractions of the 6 s buffer (`index = knob * 264600 >> 15`): p0 start, p3 end, p1/p2 loop.
  - Drum sampler buffers (`0x01CC10CC`, 12 s each, always loaded) are a possible later source. Tape is a 121 MB
    NAND file with a 24-chunk RAM cache; not usable without more work.
- `grain_source.h` (shared with the screen): validity checks on every pointer (alignment, non-null, the length word),
  window clamped so the interpolating read stays inside the buffer, 16-bit reads. Grains are cleared when the source
  changes. Pitch: the sample plays at its recorded pitch on C4.
- TE addresses are link-time symbols (`_te_engine_ids`, `_te_ssamp`, `_te_knobs`, `_te_synth_slot`) so the simulator
  can fake a sampler slot (`engine_main ... sampler`: a 440 Hz sine in slot 1).

## Fixed on the way
- Position overflow (HOLD3 and earlier): `pos * 423359` wrapped above ~31 % of the knob, so the upper two thirds of the
  bank played the wrong sounds. Now `pos * ((len - 1) >> 3) >> 12`.
