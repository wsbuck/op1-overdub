# Brief: the `repeat` FX screen SVG (Wheel of Fortune)

Paste this to a designer or an agent. The rules are in `design/SVG-GUIDE.md`; the current file is
`design/fx/wheel.svg`, made by `design/fx/gen_wheel.py`.

---

Draw a 320×160 SVG screen for an effect on the Teenage Engineering OP-1 synthesizer (original model). The effect is
**repeat**, a beat repeat in the style of Ableton's Beat Repeat: it captures a slice of audio and stutters it in time.
The screen is the Tarot card **Wheel of Fortune**, using the Rider–Waite–Smith card (1909, public domain) as the
reference:
- a great wheel with the letters T A R O on its rim;
- a sphinx seated on top holding an upright sword;
- a snake descending on the left;
- a jackal-headed Anubis rising on the right;
- in the four corners, winged creatures on clouds, each with a book: the angel top-left, the eagle top-right, the
  bull bottom-left, the lion bottom-right.

It must be an original, flat, Teenage Engineering style drawing, not a trace of the card.

## Style
- Black background. Don't draw it.
- Line art like TE's own screens: 1.5 px strokes, square caps, mostly no fill. Use a black fill only where a shape
  must hide what's behind it.
- Use colours from the OP-1 palette:

  | role | colours |
  |---|---|
  | knobs | blue #698EFF, green #00ED95, white #FFFFFF, orange #FF423F |
  | light versions | lightblue #BFD8FF, lightgreen #80FFAF, lightorange #FFB3B3 |
  | neutrals | grey #7C7990, purplegrey #AEB1DC, lightpurplegrey #E6E7FF, darkblue #383572, offblue #1D2040, offgrey #353238, purple #5F4270 |
  | black | #010000 |
- Blue, green, white and orange belong to the four knobs. Use each only on the part its knob controls (below);
  everything else stays neutral grey or purple.
- It has to read on a small OLED at 1×, so clear silhouettes matter more than fine detail.

## Layout
- The wheel is centred at (160, 82), outer radius about 75. The corners hold the four creatures.
- Keep (8, 7) to about (130, 20) clear: a value readout appears there.
- Keep about (60, 136) to (130, 150) clear: a "BAR 2/4" text appears there.

## Parts the code animates (exact ids required)
The code only shows or hides elements, or moves groups with a matrix. It never recolours anything, so anything that
changes colour needs two versions, one grey and one coloured, and the code shows one or the other.

| id | what it is | how the code uses it |
|---|---|---|
| `wheel` | group: the rotating part of the wheel (rim letters can stay outside it) | rotated about (160, 82) one step per repeat |
| `spk2`, `spk3`, `spk4`, `spk6`, `spk8`, `spk12`, `spk16`, `spk24`, `spk32` | blue spoke sets with that many spokes, inside `wheel` | one is shown: the GRID (blue knob) |
| `blur1`, `blur2` | blue blur rings, inside `wheel` | for the finest grids (1/32T: blur1; 1/64: both) |
| `leadspoke` | one light-blue spoke, inside `wheel` | always shown, so each step is visible |
| `gate1` … `gate16` | 16 white arc segments around the wheel; segment 16 ends at 12 o'clock and they run anticlockwise back to 1 | the last n are shown: the GATE length (white knob) |
| `gatex1` … `gatex16` | the same 16 segments in light grey | shown where the gate is stretched |
| `gatedim` | a dim full ring under the gate | shown when nothing will fire |
| `lap1`, `lap2` | small extra white rings or marks | gates of 2 bars (lap1) or 4 bars (both) |
| `playhead` | a small tick on the gate ring at 12 o'clock | rotated about (160, 82), one turn per bar |
| `ch1` … `ch12` | 12 small green "lots" in the hub | the first n are shown: the chance (green FORTUNE knob) |
| `hubman` | the hub lit green | shown while the manual trigger is held |
| `swordrest`, `swordup` | the sphinx's sword: grey at rest, green raised | the code shows one; `swordup` is rotated about the pivot (168, 30) from 109° to 0° when a fill fires |
| `book1o`/`book1c`, `book2o`/`book2c`, `book3o`/`book3c`, `book4o`/`book4c` (and optional `book<n>f`, a turning page) | each creature's book, open (green) and closed (grey): 1 angel, 2 eagle, 3 bull, 4 lion | open or closed for VARY (green knob) |
| `snakegrey`, `snakeorg` | the snake, grey and orange, inside group `snake` | `snake` is rotated about (160, 82) down to −75° as the pitch drops (orange PITCH knob) |
| `sinkdot1` … `sinkdot6`, `sinkend` | small orange marks along the snake's path | how far down the pitch goes |
| `anubgrey`, `anuborg` | Anubis, grey and orange, inside group `anubis` | `anubis` moves up along the right side as the pitch rises |
| `risedot1` … `risedot6`, `riseend` | small orange marks along Anubis's path | how far up the pitch goes |
| `angel`, `eagle`, `bull`, `lion` | each creature as a group (with its cloud) | static; `angel` is hidden while the readout shows |
| `wheel1` | an empty group | a marker the code checks for |

## Hard rules for the OP-1's renderer (break one and it draws wrong or halts)
1. One shape per `path`: only one `M` in each `path`. Use separate elements grouped in `<g>`.
2. Only these elements: `svg`, `g`, `line`, `polyline`, `polygon`, `rect`, `circle`, `ellipse`, `path`. No text,
   gradients, clip paths, masks, dashes, `use`, or opacity other than `opacity`.
3. Path commands: `M L H V C S Z` only. No `Q`, `T` or `A` (arcs).
4. Numbers: at most 4 decimals, no exponents. Transforms: `translate(x y)`, `scale(x y)` or `matrix(a b c d e f)`,
   space-separated; no `rotate()`.
5. Presentation attributes only (`fill="…"`, `stroke="…"`), no `style="…"`. ASCII ids, all unique.
6. Keep each element's outline modest. The renderer has a budget of about 3,800 cells per element, about the
   number of pixels its outline passes through, and a stroke counts both sides. Split long outlines.
7. Keep the total drawn at once to about 150 elements or fewer.

## Deliver
`wheel.svg`, passing:
```
python3 design/fx/check_svg.py wheel.svg
python3 design/fx/svg_cells.py wheel.svg
```
Include previews at 1× and 2× of the default state and a few animated states: firing, pitch down, pitch up.
