# FX screen designs (approved 2026-10-10)

Our own SVG assets for the FX screens, for the SVG route (notes/svg-route.md): filled scene in the SVG, moved per
frame by group matrices / show-hide / recolour; live parts (echo arcs, mode word, motion marks) drawn on top with our
line renderer. No text in the SVGs (TE's renderer has none); the user's rule: no text on FX screens, except the
distortion mode word.

| FX | asset | concept |
|---|---|---|
| ensemble | choir.svg | singing choir on a stage: blue lead sways (rate), green = sway depth (motion marks, stars), white = backing singers, orange = mouths/notes (mix); master: anti-phase floor reflections |
| pingpong | pingpong.svg | table tennis: blue = table length/delay, green = echo arches (feedback), white = ball look (tone), orange = 11-lamp scoreboard (mix); mono: squash against a wall |
| distortion | pedals.svg | compact distortion pedal + wah (real pedalboard order: input right → wah → distortion → output left): blue = shake/sparks (drive), green = wah treadle heel/toe (filter), white = body colour + mode word TUBE/MUFF/FUZZ/CLIP/FOLD/CRUSH, orange = fade in/out (mix) |

Each SVG's header comment lists every id and how the code drives it. `check_svg.py FILE.svg` validates the
renderer subset (run before shipping). The `gen_*.py` mock generators were written in the session scratchpad and
are kept for reference (paths inside them point there); the GIFs show the intended animation.

`uniquify.py FILE.svg id:fill id:stroke ...` gives the runtime-recoloured colours unique values (±1 per channel).
`gen_wah_table.py` writes `src/slot/wah_table.h` (the wah treadle face matrices) from the 3D model in gen_rot.py.
`src/slot/test/preview.py` renders the screens on the host with the driver's node state applied (FRAMES=n animates).
