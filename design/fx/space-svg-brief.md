# Brief: the `space` FX screen (reverb)

This is an open brief: the concept, what the controls do and what the screen can know. The design is up to you.

---

**Design an animated screen for an effect on the Teenage Engineering OP-1 (original model).**

## The effect: `space`
`space` is a reverb that places the sound in a room, from a small room, through a hall and a church, to a cathedral,
and on to an impossible endless VOID. It can be pushed to extremes:
- tails that last over a minute;
- tails that hold forever;
- a frozen chord you can play over;
- a "shimmer" tail that climbs in octaves like a choir of light.
On the master it can duck its tail under the music, so the mix stays clear and the reverb blooms in the gaps.

## The four knobs: what you hear
- **Blue, SPACE:** the size and kind of room, morphing continuously: ROOM, HALL, CHURCH, CATHEDRAL, VOID.
  Bigger means a later, softer onset, denser and longer tails, and more distance.
- **Green, DECAY:** how long the tail lasts, from a quarter of the space's natural length to four times it (up to
  about 70 s in VOID). The last two steps are **INFINITE**, where the tail never fades and new notes keep layering in,
  and **FREEZE**, where the tail holds exactly as it is and new notes stay out of it.
- **White, TAIL:** the colour or effect of the tail, in zones:
  - TONE, from dark and muffled to bright and airy;
  - SHIMMER +12, an octave climbing up through the tail;
  - SHIMMER +12+19, an octave and a twelfth: a bright, choir-like stack.
  - Three more zones come later, so reserve room for them: SUB −12 (an octave down), GATE (the tail cut off when
    you stop playing) and SWELL (the tail fades in slowly).
  - Within each zone, the knob sets the amount.
- **Orange, MIX:** how much reverb, from dry to an even blend in the middle. Above the middle, on the master, the
  tail ducks harder under the music (−12 to −24 dB). As a synth or drum effect, the dry sound fades out instead,
  leaving only the reverb.
- **Green encoder push (if it works on the device):** hold it to freeze the tail momentarily; let go to release it.

## What the screen can know, every frame
- The four knob values, including TE's LFO movement.
- The space type and position in the morph.
- The decay in seconds.
- The TAIL zone and amount.
- The tail's level now, 0–1; animate it with the sound.
- The duck amount now, 0–1, i.e. how much the music is pushing the tail down.
- FREEZE and INFINITE, each fading in 0–1.
- Whether the push is held.
- Whether the tail is starting clean, a short moment after switching on.

## Canvas and style
- 320 × 160 pixels on a black OLED.
- Teenage Engineering's look: flat, geometric, playful but restrained, mostly line art with 1.5 px strokes and
  square caps.
- OP-1 palette:

  | role | colours |
  |---|---|
  | knobs | blue #698EFF, green #00ED95, white #FFFFFF, orange #FF423F |
  | light versions | lightblue #BFD8FF, lightgreen #80FFAF, lightorange #FFB3B3 |
  | neutrals | grey #7C7990, purplegrey #AEB1DC, lightpurplegrey #E6E7FF, darkblue #383572, offblue #1D2040, offgrey #353238, purple #5F4270 |
  | black | #010000 |

- Use blue, green, white and orange only for what their knob controls.
- Keep the top-left corner, about 150 × 20 px, free for a value readout (e.g. "CATHEDRAL", "DECAY 12.4S").

## Technical limits (the OP-1's SVG renderer)
- Each `path` may contain only one shape (a single `M`). Make each piece its own element and group them.
- No text, gradients, masks, clip paths, arcs (`A`) or `rotate()`. Use lines, polylines, polygons, rects, circles,
  ellipses and paths with M, L, H, V, C, S and Z.
- At most 4 decimals in numbers.
- Keep each element's outline modest (about 3,800 renderer cells, roughly the pixels its outline crosses; a stroke
  counts both sides).
- Draw about 150 elements or fewer at once.
- The code can only show or hide parts, or move, scale or rotate groups with a matrix. Nothing changes colour, so a
  part that changes colour needs a copy in each colour.
- Check it with `python3 design/fx/check_svg.py space.svg` and `python3 design/fx/svg_cells.py space.svg`.

## Deliver
- An SVG with each moving or switching part as a named group.
- A header comment listing each group and what drives it: which knob or signal, and how it moves.
  `design/fx/repeat.svg`'s header is a good example of the format.
- Preview images of a few states: small room, cathedral, VOID with a long decay, shimmer, frozen, the master ducking,
  and FX off.
