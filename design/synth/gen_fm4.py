#!/usr/bin/env python3
"""Write the fm4 engine screen's SVG (src/slot/ui_fm4.h), TE renderer subset (notes/svg-route.md; check with
design/fx/check_svg.py). Our own artwork.

  gen_fm4.py [--style speaker|musicbox|flywheel] [OUT.svg]      (default: speaker -> design/synth/fm4.svg)

Every gear has the same tooth pitch (FU_PITCH units), so any two mesh: a size is a tooth count (SIZES), and each
operator group holds one variant per size; the code shows the one its ratio picks. Carriers (the operators you hear)
and modulators look different: each variant has modulator art (open, spoked) and carrier art (per style), toggled.
Carriers stand on a floor through a leg; under the floor each carrier has a foot whose art is the style's
"sound maker" (a speaker, a music-box comb, a bearing block).
"""
import argparse
import math
import os

PITCH = 6.0                                 # tooth pitch at the pitch radius, units (= FU_PITCH in ui_fm4.h)
SIZES = (8, 11, 14, 18, 23)                # tooth counts (= fu_teeth in ui_fm4.h)
DEPTH = 3.4                                 # tooth depth (= FU_DEPTH x 2 in ui_fm4.h: addendum + dedendum)
LEG = 40.0                                  # leg height as drawn (the code scales it to reach the floor)
OPS = "abcd"

# runtime-recoloured strokes/fills: one unique value (+-1 of a colors.svg colour) per operator and role, shared by that
# operator's size variants (the code gives them all the same colour, so even a shared style entry would be harmless)
_used = set()
_per = {}


def per(key, base):
    if key not in _per:
        _per[key] = uniq(base)
    return _per[key]


def uniq(base):
    r, g, b = int(base[1:3], 16), int(base[3:5], 16), int(base[5:7], 16)
    for dr in (0, -1, 1):
        for dg in (0, -1, 1):
            for db in (0, -1, 1):
                c = f"#{min(255, max(0, r + dr)):02X}{min(255, max(0, g + dg)):02X}{min(255, max(0, b + db)):02X}"
                if c not in _used:
                    _used.add(c)
                    return c
    raise SystemExit("out of unique colours for " + base)


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def pol(r, deg):
    a = math.radians(deg)
    return r * math.cos(a), r * math.sin(a)


def pts(seq):
    return " ".join(f"{f(x)} {f(y)}" for x, y in seq)


def body_points(n):
    rp = n * PITCH / (2 * math.pi)
    ro, ri = rp + DEPTH / 2, rp - DEPTH / 2
    step = 360 / n
    out = []
    for i in range(n):
        c = i * step                        # tooth i centred at angle c (the code's phase maths relies on it)
        for r, d in ((ri, c - 0.36 * step), (ro, c - 0.17 * step), (ro, c + 0.17 * step), (ri, c + 0.36 * step)):
            out.append(pol(r, d))
    return out, rp, ro, ri


def variant(op, j, n, style):
    pp, rp, ro, ri = body_points(n)
    rim = ri - 2.6
    hub = max(3.2, rp * 0.28)
    spokes = " ".join(f"M{f(pol(hub + 0.6, a)[0])} {f(pol(hub + 0.6, a)[1])} L{f(pol(rim, a)[0])} {f(pol(rim, a)[1])}"
                      for a in (-90, 30, 150))
    vid = f"g{op}{j}"
    # modulator art: open rim and three spokes (light, airy)
    mod = (f'      <g id="{vid}m">\n'
           f'        <circle cx="0" cy="0" r="{f(rim)}" fill="none" stroke="#383572" stroke-width="0.9"/>\n'
           f'        <path d="{spokes}" fill="none" stroke="#7C7990" stroke-width="1.4" stroke-linecap="round"/>\n'
           f'      </g>\n')
    # carrier art, per style
    if style == "speaker":                  # a solid face with a dust cap: the voice coil of a speaker
        car = (f'      <g id="{vid}c">\n'
               f'        <circle id="{vid}face" cx="0" cy="0" r="{f(rim)}" fill="{per(op + "face", "#383572")}"/>\n'
               f'        <circle cx="0" cy="0" r="{f(rim * 0.62)}" fill="none" stroke="#698EFF" stroke-width="1"/>\n'
               f'        <circle cx="0" cy="0" r="{f(rim * 0.62 - 1.6)}" fill="none" stroke="#383572" stroke-width="0.8"/>\n'
               f'      </g>\n')
    elif style == "musicbox":               # a pinned drum face: a ring of pins
        pins = " ".join(f"M{f(pol(rim * 0.78, a)[0] - 0.01)} {f(pol(rim * 0.78, a)[1])} "
                        f"L{f(pol(rim * 0.78, a)[0] + 0.01)} {f(pol(rim * 0.78, a)[1])}"
                        for a in range(0, 360, 360 // max(6, n // 2)))
        car = (f'      <g id="{vid}c">\n'
               f'        <circle id="{vid}face" cx="0" cy="0" r="{f(rim)}" fill="{per(op + "face", "#1D2040")}" stroke="#383572" stroke-width="0.9"/>\n'
               f'        <path d="{pins}" fill="none" stroke="#FFB3B3" stroke-width="2.2" stroke-linecap="round"/>\n'
               f'      </g>\n')
    else:                                   # flywheel: heavy disc with a counterweight
        cw = [pol(rim - 0.5, a) for a in range(20, 161, 20)] + [pol(hub + 2, a) for a in range(160, 19, -35)]
        car = (f'      <g id="{vid}c">\n'
               f'        <circle id="{vid}face" cx="0" cy="0" r="{f(rim)}" fill="{per(op + "face", "#1D2040")}" stroke="#AEB1DC" stroke-width="1.6"/>\n'
               f'        <polygon points="{pts(cw)}" fill="#383572" stroke="#7C7990" stroke-width="0.8" stroke-linejoin="round"/>\n'
               f'        <circle cx="{f(pol(rim * 0.6, -90)[0])}" cy="{f(pol(rim * 0.6, -90)[1])}" r="1.6" fill="#E6E7FF"/>\n'
               f'      </g>\n')
    return (f'    <g id="{vid}">\n'
            f'      <polygon id="{vid}body" points="{pts(pp)}" fill="#010000" stroke="{per(op + "body", "#E6E7FF")}" stroke-width="1.3" stroke-linejoin="round"/>\n'
            + mod + car +
            f'      <circle id="{vid}hub" cx="0" cy="0" r="{f(hub)}" fill="#010000" stroke="{per(op + "hub", "#E6E7FF")}" stroke-width="1.3"/>\n'
            f'      <circle cx="0" cy="0" r="1.5" fill="#7C7990"/>\n'
            f'      <circle id="{vid}sel" cx="0" cy="0" r="{f(ro + 3)}" fill="none" stroke="#FFFFFF" stroke-width="1"/>\n'
            f'    </g>\n')


def foot(op, style):
    """Under the floor, at the carrier's x: the style's sound maker. Origin = where the leg meets the floor."""
    i = f"s{op}"
    if style == "speaker":                  # a little cabinet with a cone (pulses) and sound waves
        return (f'  <g id="{i}foot">\n'
                f'    <rect x="-9" y="1.5" width="18" height="13" fill="#010000" stroke="#AEB1DC" stroke-width="1.2"/>\n'
                f'    <g id="{i}cone">\n'
                f'      <circle cx="0" cy="0" r="4.6" fill="#1D2040" stroke="#E6E7FF" stroke-width="1"/>\n'
                f'      <circle cx="0" cy="0" r="1.6" fill="#E6E7FF"/>\n'
                f'    </g>\n'
                f'    <path id="{i}wave" d="M11.5 4.5 C13 6.5 13 9.5 11.5 11.5 M14 2.5 C16.5 6 16.5 10 14 13.5" fill="none" stroke="{uniq("#698EFF")}" stroke-width="1" stroke-linecap="round"/>\n'
                f'  </g>\n')
    if style == "musicbox":                 # a comb: tines plucked by the drum (the code bends the plucked one)
        tines = " ".join(f"M{f(-7.5 + 2.5 * t)} 3 L{f(-7.5 + 2.5 * t)} {f(9 + (t % 3))}" for t in range(7))
        return (f'  <g id="{i}foot">\n'
                f'    <rect x="-10" y="1.5" width="20" height="3" fill="#383572"/>\n'
                f'    <path d="{tines}" fill="none" stroke="#AEB1DC" stroke-width="1" stroke-linecap="round"/>\n'
                f'    <line id="{i}tine" x1="0" y1="3" x2="0" y2="13" stroke="{uniq("#FFB3B3")}" stroke-width="1.4" stroke-linecap="round"/>\n'
                f'  </g>\n')
    return (f'  <g id="{i}foot">\n'                     # flywheel: a bearing block bolted to the floor
            f'    <polygon points="-8 13 8 13 5.5 2 -5.5 2" fill="#1D2040" stroke="#AEB1DC" stroke-width="1.2" stroke-linejoin="round"/>\n'
            f'    <circle cx="-4.5" cy="10" r="1" fill="#7C7990"/>\n'
            f'    <circle cx="4.5" cy="10" r="1" fill="#7C7990"/>\n'
            f'    <line id="{i}wave" x1="-11" y1="15.5" x2="11" y2="15.5" stroke="{uniq("#698EFF")}" stroke-width="1.2" stroke-linecap="round"/>\n'
            f'  </g>\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--style", default="speaker", choices=("speaker", "musicbox", "flywheel"))
    ap.add_argument("out", nargs="?", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "fm4.svg"))
    a = ap.parse_args()
    tip = (14.1, -14.1)
    legs = "".join(f'  <path id="s{op}leg" d="M-1.5 0 L-7 {f(LEG)} M1.5 0 L7 {f(LEG)}" fill="none" stroke="#7C7990" '
                   f'stroke-width="1.4" stroke-linecap="round"/>\n' for op in OPS)
    feet = "".join(foot(op, a.style) for op in OPS)
    gears = "".join(f'  <g id="g{op}">\n' + "".join(variant(op, j, n, a.style) for j, n in enumerate(SIZES)) + "  </g>\n"
                    for op in "dcba")
    svg = f'''<?xml version="1.0" encoding="utf-8"?>
<!--
fm4.svg ({a.style}) - the fm4 synth engine screen (Operator-style FM, notes/harm-fm4.md): every operator is a gear,
the algorithm is the gear train. Our own artwork, generated by design/synth/gen_fm4.py (style {a.style}). TE's
retained renderer draws it, then src/slot/ui_fm4.h draws the text on top with our stroke font. Ids are found once
(find, assert=0); the code places everything (local coordinates: gears at their axle, feet where the leg meets the floor).
  floor             a line (0,0)-(100,0) mapped onto the carriers' floor
  sXleg             carrier X's stand: matrix scales its {f(LEG)}-unit height to reach the floor; hidden for modulators
  sXfoot            carrier X's sound maker under the floor; hidden for modulators. sXcone scales with the sound,
                    sXwave fades with it, sXtine (if present) bends when a pin/tooth passes
  belt1, belt2      a modulator's further targets: (0,0)-(100,0) mapped onto the segment
  gX                operator X: matrix = translate(axle) * rotate(angle) * fit scale; one variant gXj per size
                    (tooth counts {", ".join(map(str, SIZES))}, pitch {f(PITCH)}), the others hidden
  gXjm / gXjc       modulator art / carrier art of that variant (one shown)
  gXjbody, gXjhub   stroke recoloured: carriers by level, modulators by level x INDEX [blue], the selected page white
  gXjface           carrier face fill recoloured (dark .. lit with the envelope)
  gXjsel            shown only on that operator's page
  pawl              feedback on D: shown when feedback > 0 [orange]; arm, pivot, spring recoloured
-->
<svg version="1.1" xmlns="http://www.w3.org/2000/svg" x="0" y="0" width="320" height="160" viewBox="0 0 320 160">
  <line id="floor" x1="0" y1="0" x2="100" y2="0" stroke="#7C7990" stroke-width="1.2" stroke-linecap="round"/>
  <line id="belt1" x1="0" y1="0" x2="100" y2="0" stroke="{uniq("#698EFF")}" stroke-width="2" stroke-linecap="round"/>
  <line id="belt2" x1="0" y1="0" x2="100" y2="0" stroke="{uniq("#698EFF")}" stroke-width="2" stroke-linecap="round"/>
{legs}{feet}{gears}  <g id="pawl">
    <path id="spring" d="M23.5 -27.5 L29.5 -30 L24.5 -33 L30.5 -35.5 L25.5 -38.5 L31 -41" fill="none" stroke="{uniq("#FF423F")}" stroke-width="1.2" stroke-linejoin="round" stroke-linecap="round"/>
    <line x1="25" y1="-42.5" x2="35" y2="-42.5" stroke="#7C7990" stroke-width="1.5" stroke-linecap="round"/>
    <path id="arm" d="M24 -26 L{f(tip[0] + 0.6)} {f(tip[1] - 0.6)} L{f(tip[0] + 3.2)} {f(tip[1] + 0.4)} Z" fill="#010000" stroke="{uniq("#FF423F")}" stroke-width="1.5" stroke-linejoin="round"/>
    <circle id="pivot" cx="24" cy="-26" r="2.2" fill="#010000" stroke="{uniq("#FF423F")}" stroke-width="1.2"/>
  </g>
</svg>
'''
    open(a.out, "w").write(svg)


if __name__ == "__main__":
    main()
