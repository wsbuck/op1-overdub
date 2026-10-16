# SVG route: our own SVG documents through TE's retained renderer (firmware 246)

Status: **in use** for the FX screens (choir, pingpong, pedals) and the fm4 engine screen
(design/synth/fm4.svg, same route from the engine page update, not yet on device). Original research: Static analysis 2026-10-10 (objdump + Ghidra; scratch in the session scratchpad `svg/`).
Marks: **Verified** = read at the call site or in the disassembly; **Confirmed** = consistent decompile + TE usage; **Hypothesis**.

## Verdict
- Feasible, low risk if the document is loaded lazily from our hook with an existence check first.
- TE's own screens do exactly this every frame: mutate nodes (matrix, visible flag, text, style), then render the document, mixed with immediate `te_line` calls.
- Limits: a fixed SVG subset (no text, gradients, dashes, arcs, rotate()). No point-editing API, so live curves stay immediate-mode lines.

## Loading
- Loader `FUN_019fb458(const char *name)` → document root (or 0). **Verified**:
  - `snprintf(buf, 200, "/yaffs2/content/display/%s", name)`.
  - `yaffs_open(path, 0, 0x100)` = `FUN_01a997f4`; size via `lseek(fd, 0, 2)` = `FUN_01a9a0e4`; `malloc(size+1)` = `FUN_01aa370c`; `read` = `FUN_01a99c90`; `close` = `FUN_01a99908`.
  - Skips a UTF-8 BOM.
  - rapidxml 1.13 parses in place into a heap `memory_pool` (0x10044 B: 64 KB static block plus overflow).
  - `FUN_019fcfd8(0, xml_root)` builds the node tree. The file buffer and the pool are then freed (`FUN_01aa2b04`, `FUN_01a5e310`), so only the node tree persists.
- Every failure is a fatal assert (boot-style assert screen). **Verified**:
  - missing file: `Parser.cpp:158 YAFFS can't access %s` / `:162 YAFFS error %i`;
  - empty file: `:166/167`; short read: `:176`;
  - XML error: `Parser.cpp:34 Parse error: %s at %x`;
  - tree build failed: `:832 couldn't parse %s`;
  - number format violations (see subset).
- Screens: ctor `FUN_01a3bab0(obj, id, name)` stores `{doc=0, id, svg_loader*, vtable 0x01911474}` and registers `0x01DF4E54[id] = obj`. `svg_loader` (8 B) = `{doc, name}`, ctor `FUN_01a50350`. **Verified**.
- Boot: `FUN_01a3ba50` (called once from `0x019F07D2`) walks all 68 registry slots and asserts on an empty one (`Screen.cpp:198 screen`). For each it loads the doc (`FUN_01a503d8`), then calls the screen's init (vtable +0x24). So **all TE screen SVGs are parsed at boot**. **Verified**.
  - `FUN_01a503d8` also deletes a top-level node with id `bg` (`FUN_019eb8ac(doc,"bg",0)` + `FUN_019ebdbc`). Designers may keep a reference background in `<g id="bg">`.
- TE also calls the loader outside screens: `colors.svg` at `0x01962268`, and the font loader at `0x019A5A42`. So calling `FUN_019fb458` standalone has precedent. **Verified**.
- (a) **New file via the update tar: yes.**
  - te-boot's `tar.cpp` is a generic ustar extractor: it writes every member to `/yaffs2/%s`, creates directories, and opens with 0x241 (write | create | truncate; see LOG). **Confirmed** (strings + LOG).
  - Prior art: op1repacker's `gfx-iter-lab` / `gfx-cwo-*` replace `content/display/*.svg` (Inkscape output) and work on devices.
  - `tools/op1pack.py` only has `--replace`. A new member needs an `--add NAME=PATH` option (not built).
  - The updater never deletes files: the extra SVG stays on NAND after flashing stock. That is harmless, because stock only opens names hard-coded in the app. **Hypothesis** (no delete path seen).
- (b) Pointing a TE screen at another file: not useful.
  - The name is an immediate in the ctor (e.g. `0x019E8A18` for mmmf.svg).
  - TE's init then looks up its own ids with assert-on-missing (`FUN_01a3b580`), so the substitute file would need every TE id.
  - An extra screen object is impossible: all 68 ids are used and asserted.
  - We need neither: we can hold documents without a screen object.
- (c) **Several documents, chosen per FX/engine id: yes.**
  - Keep `void *doc[n]` in our data, loaded lazily on first show.
  - `FUN_01a3b4c8` reads only word 0 of its argument (the doc). **Verified**. So render with a fake screen `struct { void *doc; } s = { doc[i] };`.
  - The same fake works for the Screen helpers (find/text/style), which only read `[obj]`.

## Supported SVG subset (parser strings `0x018EFDF0..0x018F1420` + decompile; TE files as evidence)
- Elements: `svg` (x, y, width, height, viewBox), `g`, `line` (x1 y1 x2 y2), `polyline`/`polygon` (points), `circle` (cx cy r), `ellipse` (cx cy rx ry), `rect` (x y width height), `path` (d).
  - 7 shape kinds = 7 `new(0x90)` sites in `FUN_019fcfd8`; group `new(0x58)`, root `new(0x78)`. **Verified**.
- Not supported (no string in the binary, **Verified** absent): `text`, `use`/`xlink`, gradients, `stroke-dasharray`, `fill-opacity`, `stroke-opacity`, `visibility`, `display`, transform `rotate()`/`skew`.
  - `display="none"` in TE files is ignored by the parser; TE hides those nodes in code.
  - Rounded `rect` rx: **Hypothesis** no.
  - `defs`/`clipPath`: avoid (behaviour unknown).
- Presentation attributes only (no `style="..."` CSS): `fill`, `stroke`, `stroke-width`, `stroke-linecap` (butt/round/square), `stroke-linejoin` (miter/round/"belvel"), `stroke-miterlimit`, `opacity` (multiplies stroke and fill alpha), `transform`, `id`.
- Colours are true RGBA, not palette indices: `#RRGGBB` (sscanf `#%02x%02x%02x%02x`), `rgb(r,g,b)`, `none`, and named black/red/green/blue/yellow/white/lime. Unknown names → opaque black; `#RGB` short form misparses. Use colors.svg hex values to match the palette.
- `transform`: `matrix(a b c d e f)`, `translate(x y)`, `scale(x y)`, space-separated (sscanf `%f %f ...`; commas fail). **Confirmed**.
- Path commands: M L H V C S Z, absolute and relative. Q and T **assert** (`Parser.cpp:525/528`, fatal); A (arcs) is ignored, which breaks the path. **Verified** (switch at `0x019FCACC`).
- Numbers become Q22.10 (`FUN_019fb274`):
  - at most **4 decimals**, else fatal `Parser.cpp:116 unsupported decimal count`;
  - no exponent (`:103`, fatal);
  - `5.` with no digits after the point → fatal;
  - |v| < 2^21. **Verified**.
- ids: ASCII; Illustrator `_xHH_` escapes are decoded; non-ASCII → fatal `Parser.cpp:577`.
- Coordinates: 320 × 160 viewBox like TE (`0 0 320 160`). Safe area x 4..316, y 2..140 (screen.md).

## Runtime API (all Q22.10, SVG user units)
| function | use |
|---|---|
| `FUN_019fb458(name)` | load `/yaffs2/content/display/<name>` → doc (asserts on any error) |
| `FUN_019eb8ac(node, id, assert)` | depth-first find by id; `assert=0` → returns 0 if missing (safe). **Verified** |
| `FUN_01a3b580(screen*, id)` | = find with assert=1 (fatal if missing) |
| `FUN_019ead58(node, const int32 m[9])` | set the node's **local** matrix (replaces any SVG `transform`); no-op if equal, else copies and calls vtable +0x74 (invalidate). **Verified** |
| `B[node + 0x50]` | visible flag (1 shown, 0 hidden; default 1). Group render returns early when 0 (`0x019EB82C`, **Verified**). TE toggles it on groups, lines and paths (mmmf `g0..g9`, tombola `ball%i`) |
| `FUN_019eaf70(node, s)` | uniform scale about the node's bbox centre (tombola "gravity"). **Confirmed** |
| `FUN_01a3b854(screen*, id)` | → `LineStyle*` `{u8 rgba[4]; i32 width; cap; join; miterlimit}` (20 B) of that node; writable. Asserts if the node has no stroke |
| `FUN_01a3b96c(screen*, id)` | → `u8 rgba[4]*` fill colour of that node; writable. Asserts if the node has no fill |
| `FUN_01a3b5d0(screen*, template_id, align 0/1/2)` | replace the template node with a text node (`new(0xA4)`, `FUN_01a61904`). Position/height from the template's bbox, colour/width from its line style. **Call once (init)**: it allocates and consumes the template |
| `FUN_01a61aac(text, const char *)` | set the text (TE's stroke font from opfont.svg) |
| `FUN_019ebdbc(parent, node)` | remove and destroy a node (permanent) |
| `FUN_01a3b4c8(screen*)` | render the whole doc with the identity matrix onto `s_raster [0x01DF4E34]` (valid inside a screen update) |
- Matrix layout **Confirmed** (parser at `0x019FC934`; tombola bouncebar): row-major, row vector, `m = {a, b, 0, c, d, 0, e, f, 1}` × 1024. x' = a·x + c·y + e, y' = b·x + d·y + f.
  - Identity = `{1024,0,0, 0,1024,0, 0,0,1024}`.
  - Rotation by θ about (px,py): `a=d=cos, b=sin, c=−sin, e=px−cos·px+sin·py, f=py−sin·px−cos·py` (all ×1024). No `rotate()` helper exists: compute it ourselves.
- Styles are one vector entry per element (`FUN_01976b30` / `FUN_019768f8` push_back, no dedupe seen), so recolouring one node does not leak to others. **Hypothesis** (strong).
- No API to edit points or path data: live curves (filter response, waveforms) stay immediate-mode.
  - Retained alternatives: pre-drawn variants toggled by visibility (mmmf's 10-segment meters); N bar nodes, each scaled/translated (costs about one AA primitive each).

## TE examples
- **Tombola** (`0x01A65EBC`, init `0x01A66648`):
  - init: `ball%i` → `FUN_01a3b580`, `+0x50 = 0`; text `speed` via `FUN_01a3b5d0(...,0)`; nodes gravity/rotation/crank/bouncebar.
  - per frame:
    1. hide all balls;
    2. per live ball, `+0x50 = 1` and `FUN_019ead58(ball, {1,0,0, 0,1,0, x+160, 73−y, 1})`;
    3. 6 × `te_line` (walls);
    4. `FUN_01a61aac(speed, "%d")`;
    5. toggle rotation/crank `+0x50`;
    6. bouncebar translate-y via `FUN_019ead58`;
    7. `FUN_019eaf70(gravity, g·1024)`;
    8. **`FUN_01a3b4c8(this)` last**.
  - So immediate lines and the retained render mix in one update. **Verified** (call order in disassembly).
- **Filter / mmmf** (update `0x019E8308`, init `0x019E811C`):
  - init: text nodes from template lines `green`/`white`/`red` (align 1); `g0..g9`, `r0..r9`; the `bluebase` bbox centre is kept as the pivot.
  - per frame:
    1. `FUN_019ead58(blue, T(−c)·M·T(c))`;
    2. three `FUN_01a61aac(.., "%02i"/"%s")`;
    3. meters as `+0x50 = (i < level)`;
    4. `FUN_01a3b4c8` last.
- **Delay** (`0x01984F3C`): 4 text nodes, `+0x50` toggles for the tempo and mode marks, one `FUN_019ead58`.

## Path quirks (TE's `d` parser, `0x019FCA1C`; command switch `0x019FCACC`, table `0x018F13A0` by toupper(c) - 'C')
- **Rule (Verified, device, test 78 `crushcard.svg`): every M after the first does not lift the pen: it closes the
  current subpath and draws a line to the new point. The subpaths of one element are always connected.** Seen on
  the card: M L L M L L (case 2) gets each Λ's base; three M L dashes (3) become an "E" (the closes retrace, the
  lines to each next M join the left ends); open H / V pieces after Ms (5) are joined the same way; a closed triangle
  then M L L (8) is joined to the V. In the code: M calls close `0x01A0722E` when it is not the first (`[FP - 0x2C]`
  counts the Ms), Verified; the line to the new point is what the device shows.
- Fills: the connecting lines become edges. Two closed pieces (card 7, and 9 with a reversed hole) come out right
  (the one connector is cancelled by the final close); with three or more pieces the connectors s1 -> s2 -> .. -> sn
  -> s1 enclose an extra polygon (most likely CRUSH8's black triangles; its stray diagonals and the frayed wah grip
  and grooves of LOG 592 .. 48 are the stroked connectors).
- Everything else on the card is right (Verified, test 78): a single M L L polyline (1), one open H / V subpath (4),
  closed H / V fill (6), polygon, polyline, line, rect (10 .. 12), miter / round / `belvel` joins on an acute angle
  (13 .. 15; TE's spelling, `bevel` is not recognised), strokes 0.5 and 0.8 (16), a group matrix (17), a recolour
  (18). line_to `0x01A0712C` skips a segment shorter than a minimum (`[0x01DE2E48]`). Relative commands go through a
  separate offset path (H / V relative read a global, `[0x01DDD564]`): avoided.
- **Rule for our SVGs: one subpath per element** (`design/fx/check_svg.py`, run by build.py on every published SVG;
  `--card` lets the test card break it). `design/fx/strictify.py` converts, in place and keeping the look (rsvg and
  the C-path previews match the old files within anti-aliasing): fill-only pieces are joined into one subpath by
  bridges from start to start and back along the same lines (the edges cancel; same id, nothing changes in ui.c);
  butt-capped single segments without a recoloured stroke become one filled subpath of their rectangles; other
  strokes become a group of one-subpath leaves `id_1`, `id_2`, .. (ui.c recolours the leaves). The generators
  call it (gen_crush.py splits itself).
- The cell model (svg_cells.py) counts closes and connectors; it matched CRUSH4 .. 7 (~22.6 k, ~7.2 k cells: assert;
  ~3.8 k: drawn with stray lines).
- Element count limits (static): no node or pool count assert in the parser or Screen; TE's `lander.svg` has 885
  elements / 171 KB and parses at boot. crush3d.svg (CRUSH9): 298 drawable elements, 23 .. 133 drawn per frame;
  rendered right on the device (test 78), cost ~3,000 kcycles idle and ~12,000 under heavy animation (pedals 17,599,
  LOG 592).
- Test card: `crushcard.svg` (design/fx/gen_card.py), 18 numbered cases with the expected result drawn beside each
  by the C. Only with `build.py --test-card` (ui.c `CRUSH_TEST_CARD`): 360 frames (~12 s) on the first open of the
  crush screen after boot, then the pyramid.

## Cost (Hypothesis; measure before relying on it)
- **Hard limit, not cost: every fill and every stroke must stay under ~3840 AGG cells** (notes/screen.md "AGG cell
  limit"); over it the OP-1 asserts (agg.cpp:339). `design/fx/svg_cells.py --check` estimates it (build.py runs it).
- No raster cache: the whole visible tree is re-rasterised (AGG, AA) every frame after the clear. Hidden groups cost almost nothing (early return).
- An element costs about one AGG rasterisation, like one `te_line` call (~20–35 k cycles, LOG 2026-10-08). A polyline/path of k segments is one pass, so it is cheaper than k `te_line`s. Fills cost in proportion to the covered area, so avoid large filled regions.
- TE renders 24–70 elements per frame on iter/delay/mmmf/tombola (ptch 390) without trouble. A 50–100 element document is in our current budget (we drew 600 AA segments at ~70 % of the frame).
- Per-frame API calls (find is a recursive strcmp walk) are cheap if node pointers are cached at load.

## Risks
- Fatal asserts: missing file, XML error, >4 decimals, Q/T commands, non-ASCII id, and find-with-assert or style getters on unsuitable nodes. Mitigations:
  - lazy load (boot stays safe);
  - `yaffs_open`/`close` existence pre-check, falling back to immediate drawing;
  - find with `assert=0`;
  - a host-side subset checker run at build time.
- Heap: the persistent tree is ~144 B per shape + 20 B line style + 4 B fill + path vertices, so roughly 20–40 KB for 100 elements (**Hypothesis**). Parsing also needs a transient 64 KB pool plus the file size. TE keeps 1.7 MB of SVG-derived trees, so a few small docs are minor; the heap size itself is unknown (memory-map.md).
- Parse time: once, on first show (one frame drop). Boot is unchanged.
- NAND: a few KB per file.
- Legal: the SVGs are our own work. Never copy TE geometry into them. TE files stay out of the repo (as with te_line, we only call TE code at runtime).

## Minimal device experiment (SVG1)
1. `assets/display/svgtest.svg` (ours): 320 × 160; `<g id="needle">` (line + filled circle pivot at 160,80); `<g id="a">` and `<g id="b">` (a filled rect and an outlined circle); `<line id="lbl">` as a text template. ≤ 4 decimals, absolute M/L/C/Z only.
2. `op1pack.py build ... --add content/display/svgtest.svg=assets/display/svgtest.svg` (new option: ustar member under `content/display/`; te-boot and the app untouched).
3. `build_slot.py` defsyms: `_svg_load=0x019FB458 _node_find=0x019EB8AC _node_matrix=0x019EAD58 _svg_render=0x01A3B4C8 _yaffs_open=0x01A997F4 _yaffs_close=0x01A99908` (optional: `_svg_text=0x01A3B5D0 _text_set=0x01A61AAC _svg_stroke=0x01A3B854 _svg_fill=0x01A3B96C`).
4. In `fx_ui_draw` for one FX id:
   - first call: `fd = yaffs_open("/yaffs2/content/display/svgtest.svg", 0, 0x100)`. If `fd >= 0`: close it, `doc = svg_load("svgtest.svg")`, cache `needle/a/b = node_find(doc, id, 0)`, `txt = svg_text(&doc, "lbl", 0)`.
   - each frame:
     1. matrix = rotation of the blue knob about (160,80) → `node_matrix(needle, m)`;
     2. `B[a+0x50] = knob<½`, `B[b+0x50] = !that`;
     3. `text_set(txt, "SVG")`;
     4. `svg_render(&doc)`;
     5. existing immediate lines on top;
     6. in `--debug-ui`, CYCLES around `svg_render`.
   - if `doc == 0`: draw as today.
5. Pass: the needle follows the knob, groups swap, text shows, no audio impact. Report the render cycles.

## What designers can't have
- No text in the SVG: use a template `<line>` + TE text node, or our stroke font.
- No gradients, dashes, per-channel opacity (only `opacity`), arcs, quadratic curves, CSS styles, clip paths, `<use>`/symbols.
- Rotation and other motion only per node (whole group: translate/scale/rotate via a matrix we compute). No morphing or point edits: live curves are immediate lines drawn over or under the SVG.
- Recolouring works per element (stroke colour/width, fill colour). Show/hide per element or group.
- Keep large filled areas and element counts modest (≈ ≤ 100 visible elements). Every number ≤ 4 decimals.

- Style getters (FUN_01a3b854 stroke, FUN_01a3b96c fill) assert (`Screen.cpp:84 linestyle_id`) when the element has no such style, even if the id exists (SVG5 crash). Request only the styles an element has.
