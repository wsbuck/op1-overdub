# Screen and drawing (firmware 246)

Static research 2026-10-07 (two subagents; scratch in the session scratchpad `screenA/`, `screenB/`).
Spot-checked items are marked **Verified**; the rest is Confirmed/Hypothesis as reported. Device tests below.

## Display pipeline
- Panel: portrait RGB AMOLED (register map like Samsung S6E63D6 — Hypothesis), set up over SPI (`bf\SPI.cpp`, `FUN_019617e4`), refreshed continuously by PPI + DMA0 (2D, 16-bit) with Timer0/1 syncs (`bf\RGB.cpp`). ~42 Hz if SCLK = 80 MHz (Hypothesis).
- Framebuffers: two, `0x02000000` and `0x03000000`, flipped per frame (flag `0x01DE6106`, front pointer `0x018B6450`). RGB565, 512-byte rows (256 px), 320 rows. Note: both lie in SDRAM pages we never used (FREE regions avoid them).
- Geometry globals `0xFF8017B4..C0` = 320, 240, 320, 156 (**Verified**: L1 block, encrypted partial chunk; fixed `ldr._xtea`).
  The GUI raster is landscape 320 × 156 mapped onto portrait panel rows (x → row ≈ 320 − x). Other panel revisions (flag `0xFF906A54`) use other sizes: read the globals, don't hard-code.
- Frame flow: DMA0 frame ISR `0x01a2fd04` → `FUN_01a2fb68` toggles buffers, posts the GUI thread (`0x019ab880`) → `FUN_019b2674(buf)` renders → cache flush → `FUN_01a2fb18(buf)`. Frames are dropped when the GUI is slow.
- Debug overlay: `FUN_019416ea(const char *)` shows full-screen text drawn by the ISR (the assert screen uses it).

## Screens
- Each screen (68, registry `0x01DF4E54`, ids from ScreenId.cpp) is a static object with an SVG document (`/yaffs2/content/display/<name>.svg`, rapidxml + AGG). Retained scene graph, fully re-rasterised every frame after a clear (`FUN_01a23466`).
- Page vtable entries are `{int adj; fn}`: +0x0C dtor, +0x14 `isScreen(id)`, **+0x1C `update(this, dt)`** (every frame, active screen only), +0x24 init (once at boot). iter's page: object `0x01DD4F60`, vtable `0x019103FC` (**Verified**: +0x1C at `0x01910418` = `0x019B9EA4`; only iter's object uses it).
- Current synth slot `[0x01DF4E38]`; engine id `[0xFF8016E4 + slot*4]`. Our engines (≥ 15) map to iter's screen 0x14.
- Knobs per frame: `FUN_01a28210(slot)` (ReadInterface) gives floats normalised by the range table; raw int16 knobs are at `0xFF8013C4 + slot*16`.

## Drawing API (immediate mode, after the clear)
| function | what |
|---|---|
| `FUN_01a3b490()` | the raster (current back buffer, width, stride, rows, AGG buffer) |
| `FUN_01a24a4c(raster, pt *a, pt *b, [SP+0xC] width, [SP+0x10] const uint8_t rgba[4] *)` | anti-aliased thick line; points/width in Q22.10 landscape units (**Verified** call shape at dr_wave's call site `0x019130a2`) |
| `FUN_019c5bd4(char *s, int x, int y, [SP+0xC] right_align, [SP+0x10] rgb565)` | 8×8 bitmap text straight into the framebuffer (Lander "SCORE": x = 2, colour 0xFFFF) |
| palette `0x01BAAF50` | 20 × 4-byte colours from colors.svg: 0 pink, 1 black, 2 red, 3 offred, 4 offgrey, 5 darkgreen, 6 offgreen, 7 purple, 8 purplegrey, 9 lightblue, 10 darkblue, 11 offblue, 12 grey, 13 orange, 14 white, 15 blue, 16 green, 17 lightpurplegrey, 18 lightgreen, 19 lightorange |
- Retained alternatives (phase 2): find node `FUN_01a3b580`, set transform `FUN_019ead58`, text nodes `FUN_01a3b5d0` + `FUN_01a61aac`, render `FUN_01a3b4c8`; an own `.svg` would need a new tar member in `content/display/`.
- GCC's call layout matches (args 4+ at `[SP+12]`, `[SP+16]`) — **Verified** by compiling a 5-argument call.

## Hook (SCREEN1)
- `--pointer 0x01910418 = 0x019B9EA4 → _ui_update` (slot.S): for engines < 15 `JUMP.L 0x019B9EA4` with R0/R1 untouched; for ours save loop/L registers and call `ui_draw(engine, slot)` (`src/slot/ui.c`). iter's dials are not drawn for our engines.

## Device results (SCREEN1/SCREEN2, this unit)
- Newer panel: globals 400, 240, 376, 188; flags `0xFF906A54..56` = 1, 1, 0. Raster 188 px × 376 rows, stride 512.
- Global matrix: panel px = 1.1748·y, panel row = 375.94 − 1.1875·x. SVG 320 × 160 is scaled ×1.175 and rotated.
- **Safe area: x 4..316, y 2..140** (bottom rows beyond ~142 are not visible).
- Palette colours render as named (red is red). te_line widths in Q22.10 SVG units look right (FP(1) thin, FP(6) bar).
- TE's 8×8 bitmap text (`FUN_019c5bd4`) writes raw panel pixels: wrong scale and ~25 px offset on this panel. Use the stroke font (`src/slot/font.h`) instead.
- A screen of ~800 short line segments per frame caused no reported audio or UI problems.

## Open
- Frame rate, GUI thread priority vs audio, exact cost per line.

## UI redesign (2026-10-08, from a subagent review; mockups in the session scratchpad `ui_review/`)
- The preview palette was guessed and wrong (offgrey is near-black, purplegrey bright lavender, TE's "orange" coral red);
  `preview.py` now uses colors.svg. Device colours were always right.
- TE patterns adopted: structure in dark shades (darkblue 10, grey 12), live elements in the knob colours; iter.svg's
  dial (4 grey ticks, coloured needle, pivot dot, 270 degree sweep from 225); sampler.svg's boxed red HOLD; title coloured by the knob
  that selects it; small labels as 1-px text (`text_fast`).
- Screens: wavetable waterfall (dark-blue back frames, grey neighbours, blue current frame + handle); grain overview
  (128 bars: blue in the spray window, grey in the current sound or sampler window, dark blue elsewhere), playhead with
  cap, spray bracket, ruler, grains as white 2-px runs (light blue for octave shimmer); va oscillator shape
  (sampled, one segment per unit), detune ghost, the sub's square over four faint oscillator periods, animated noise.
- About 50–80 AA lines per screen (was 126–168 plus diagnostics); dense detail uses the fast framebuffer lines.

## AGG cell limit (agg.cpp:339 "alloc failed, too many blocks") — a constraint for every SVG screen
Found 2026-10-15 after CRUSH4 stopped on opening the crush FX screen (LOG 72). Static analysis of the decrypted app
(`firmware/work/elf/OP1_vdk_246_dec.dis`). **Verified** = read in the disassembly, **Hypothesis** = inferred.
- The assert is TE's change to AGG's `rasterizer_cells_aa::add_curr_cell` at `0x0191412C` (string `0x018C2880`,
  assert call `0x019141F4`). **Verified**:
  - a cell is stored only when it has area or cover; at a block boundary (low byte of the cell count `[obj + 0x0C]`
    is 0) it checks the number of blocks allocated so far (`[obj + 0]`) and **asserts if it is >= 16** (stock AGG
    silently drops cells at its limit of 1024 blocks);
  - otherwise it reuses block `curr_block` (`[obj + 8]`) or allocates a new one (`malloc(0x800)` = 256 cells of 8
    bytes; the block pointer array `[obj + 0x10]` grows by 16 at a time).
  - So 16 blocks x 256 = 4096 cells; blocks are never freed and `num_blocks` never decreases.
- Reset `0x019140F2` clears the cell count and `curr_block` (and the bounds) but **not** `num_blocks`. **Verified**.
- Scope: the only callers of reset are TE's fill (`~0x01A25512`) and stroke (`~0x01A2734A`) element renderers, which
  fetch the shared rasterizer, reset it, feed the element's vertices (`0x01914904` line_to) and render. So the
  count is **per fill or per stroke of one element** (all subpaths of a compound path together), not per frame or
  per tree. **Verified** (call sites); that this is one global rasterizer: **Hypothesis**.
- Consequence: an element whose fill or stroke needs a 16th block (more than 15 x 256 = 3840 cells) makes the pool
  reach 16 blocks, and the next block boundary of any later element (even its first cell) asserts. Effective rule:
  **every single fill and stroke must stay below ~3840 cells.** Hidden nodes are not rendered (early return,
  svg-route.md "Cost"), so they cost nothing. No clip paths exist in TE's subset. Loading or parsing does not use
  the rasterizer (rapidxml pool, svg-route.md), so file size and node count are not limited by this.
- What a cell is (AGG): one per panel pixel an edge passes through (both sides of a stroke's outline, plus joins and
  caps). Model: `design/fx/svg_cells.py` (an edge costs |dx| + |dy| + 1 panel pixels at 1.175 per SVG unit; a stroke
  twice its segments plus about a stroke width per join and cap). **Hypothesis**, consistent with the device: the
  screens that render peak at 22 .. 66 % of the limit (pingpong 827, choir 1142, pedals 1465, fm4's gears 2530);
  crush3d.svg (CRUSH4) had 4861 for its default state (RATE level 2's voxel-edge stroke, one path of every block edge
  with round caps) and asserted on the first frame.
- Rules: split long compound paths and long strokes (many short stroked subpaths cost two sides plus two caps
  each); `build.py` runs `svg_cells.py --check` on every published SVG and fails above 70 % of the limit; crush's own
  check holds every state to 50 %. A fill's interior costs nothing here (only its edges): large fills cost render
  time (svg-route.md), not cells.
