# Drawing an OP-1 screen SVG

How to draw an SVG that our screens can load. The full reverse-engineering notes are in `notes/svg-route.md`; this
page is the short version for drawing.

## Canvas and style
- `viewBox="0 0 320 160"`, which is the OLED's size. Don't draw a background: the screen is black.
- To match TE, use line art: mostly `fill="none"`, `stroke-width="1.5"` (sometimes 2), `stroke-linecap="square"`,
  and few colours. TE's own screens use 1.5 px strokes about 90 % of the time. Use fills sparingly; large filled
  areas cost more to draw.
- Colours: use the palette in `colors.svg` (from your stock firmware: `firmware/work/246/content/display/colors.svg`)
  as `#RRGGBB`. Blue, green, white and orange mean the four knobs, so use them only for things a knob controls.
- You can draw in Illustrator, Figma or Inkscape. TE drew theirs in Illustrator 14. Export as SVG 1.1 with
  presentation attributes (`fill="…"`), not CSS (`style="…"`).

## Hard rules (break one and the OP-1 draws it wrong or halts)
1. **One shape per path.** A `path` must have only one `M`. TE's parser never lifts the pen: a second `M` draws a
   line from the previous piece to the new one, which shows as stray diagonals and black triangles. Make each piece
   its own element and group the pieces with `<g>`.
2. **Elements:** only `svg`, `g`, `line`, `polyline`, `polygon`, `rect`, `circle`, `ellipse` and `path`.
   - These are not supported: `text`, gradients, `use`, clip paths, masks, `stroke-dasharray`, `fill-opacity`, and
     `rotate()`/`skew()` transforms.
3. **Path commands:** `M L H V C S Z` only. `Q`/`T` halt the OP-1, and `A` (arcs) breaks the path, so convert arcs
   to curves (Illustrator does this on export).
4. **Numbers:** at most 4 decimals, no exponents (`1e-3`), and no trailing dot (`5.`). Transforms are
   space-separated, e.g. `translate(10 20)`; commas fail.
5. **ids:** ASCII only and unique. The code finds and animates parts by id, so name the parts you want to move,
   show/hide or recolour (`snake`, `sword`, `spoke3`…), and put things that appear together in one `<g>`.
6. **Size per element:** the renderer has a fixed budget per element (about 3,800 cells, roughly the pixels its
   outline passes through; a stroke counts both of its sides). Keep long, wiggly outlines split into a few elements.
   A single element over budget halts the OP-1 when that screen opens.
7. **Recoloured parts:** each element the code recolours needs its own colour value. Shift a channel by 1 or 2 from
   the palette colour (e.g. `#638BFF` and `#638BFE`); otherwise TE merges their styles.

## Checking your file
```
python3 design/fx/check_svg.py your.svg      # the subset and quirk rules (one subpath per element, ids, numbers, colours)
python3 design/fx/svg_cells.py your.svg      # the per-element cell budget
```
Both must pass; the build runs them too. Then send me the file and say which parts should move, and how, and I'll
wire it to the screen code.

## Looking at TE's own
Your stock SVGs (rendered in `~/Downloads/op1-stock-svgs/index.html`) are the best reference for style. They come
from your own firmware, so keep them to yourself and don't publish them.
