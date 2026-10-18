#!/usr/bin/env python3
"""wheel.svg, the screen of the proposed "repeat" FX (a beat repeat; notes/fx-beatrepeat.md): an original, flat
TE-style reading of the Tarot's Wheel of Fortune (card X). DESIGN MOCKUP: no firmware code drives it yet. The
generator is the source: run it, never edit the SVG by hand.

  python3 design/fx/gen_wheel.py [GALLERY_DIR]

Writes design/fx/wheel.svg (every element one subpath of absolute M / L / C / Z, strictify.py as a safety net; no
recolours at run time: every state change is a visibility flag or a group matrix, see HEADER), checks it
(check_svg.py when firmware/work/246's colors.svg is there; svg_cells.py --check, and the cell estimate of every
gallery state and animation frame), and, with GALLERY_DIR, renders the gallery states (320x160 and 2x PNG,
rsvg-convert), an animation GIF (magick) and index.html.

The knob maps (grid_of, fortune_of, gate_of, pitch_of, eff_gate, vary_pick) are the design's reference for the C;
timeline() models the DSP's trigger logic (bars of 4 beats from tape position 0, 120 BPM) for the mockups."""
import math
import os
import random
import subprocess
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import strictify  # noqa: E402
from gen_pingpong import A, C, N, P, f, render  # noqa: E402  (shared helpers, TE's palette)

OUT = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(OUT, "wheel.svg")

WX, WY = 160, 82        # wheel centre
R_OUT = 52              # outer edge of the rim band (top y 30, bottom y 134)
R_BAND = 48             # rim band centre (stroke 8: 44 .. 52)
R_GATE = 40.5           # the gate ring (static: the bar as a clock face, 16 sixteenths, the bar line at 12 o'clock)
R_LAP1, R_LAP2 = 36.8, 34.6   # gate laps: 2 BARS shows lap1, 4 BARS both
R_SPK0, R_SPK1 = 17, 33     # spokes: hub edge .. inside the laps
R_HUB = 17
R_CH = 12.5             # the lots (chance) inside the hub
SPOKES = [2, 3, 4, 6, 8, 12, 16, 24, 32]
NGATE, NCH, NDOT = 16, 12, 6
SNAKE_R, SNAKE_A0, SNAKE_SPAN, SNAKE_TRAVEL = 61, 300, 26, 75   # on an arc left of the rim, carried down by the wheel
ANUB_R, ANUB_A0, ANUB_TRAVEL = 72, 125, 55                       # feet on an arc right of the rim, climbing upright
SWORD_PV = (168, 30)    # the sword's pivot on the sphinx's right paw (viewer's right)
SWORD_REST = 109        # degrees it turns down to rest on the rim's crown


# ------------------------------------------------------------------ knob maps (33 detent positions, 1024 raw each)
def pos(raw):
    return max(0, min(32, (int(raw) + 512) >> 10))


GRID = [  # (positions, slices per bar, name): straight divisions 4 positions wide, triplets 2 (No Triplets by landing)
    (range(0, 3), 64, "1/64"), (range(3, 5), 48, "1/32T"), (range(5, 9), 32, "1/32"), (range(9, 11), 24, "1/16T"),
    (range(11, 15), 16, "1/16"), (range(15, 17), 12, "1/8T"), (range(17, 21), 8, "1/8"), (range(21, 23), 6, "1/4T"),
    (range(23, 27), 4, "1/4"), (range(27, 29), 3, "1/2T"), (range(29, 33), 2, "1/2")]
NAME = {n: name for _, n, name in GRID}
STRAIGHT, TRIPLET = [64, 32, 16, 8, 4, 2], [48, 24, 12, 6, 3]


def grid_of(raw):
    p = pos(raw)
    for r, n, name in GRID:
        if p in r:
            return n, name


def fortune_of(raw):
    """green -> (mode, every / vary amount, chance). The latch sits on the hard-left stop, one detent from OFF."""
    p = pos(raw)
    if p == 0:
        return ("always", 0, 1.0)
    if p <= 2:
        return ("off", 0, 0.0)
    if p <= 8:
        return ("every", {3: 8, 4: 8, 5: 4, 6: 4, 7: 2, 8: 2}[p], 1.0)
    if p <= 17:
        return ("chance", 0, round((p - 8) * 0.1, 2))
    if p <= 20:
        return ("every", 1, 1.0)
    a = 1 + (p - 21) // 3
    return ("vary", a, [0.5, 0.67, 0.83, 1.0][a - 1])


GATES = [1, 2, 3, 4, 6, 8, 12, 16, 32, 64]     # sixteenths


def gate_of(raw):
    """white: the gate in sixteenths, ending on the bar line of the interval's last bar"""
    return GATES[min(9, pos(raw) // 3)]


def eff_gate(n, g):
    """the grid wins: the gate stretches to hold at least two slices (the live capture + one repeat), up to 4 bars"""
    return min(64, max(g, 2 * 16 // n if 16 % n == 0 else math.ceil(32 / n)))


PITCH = {3: 12, 4: 12, 5: 7, 6: 7, 7: 5, 8: 5, 9: 3, 10: 3, 11: 2, 12: 2, 13: 1, 14: 1,
         18: -1, 19: -1, 20: -2, 21: -2, 22: -3, 23: -3, 24: -5, 25: -5, 26: -7, 27: -7, 28: -12, 29: -12}


def pitch_of(raw):
    """orange, bipolar: ('spin', +12) / ('step', S) / ('flat', 0) / ('stop', -inf). S = semitones reached at the
    gate's last repeat (a staircase S * k / (N - 1)); the manual key and ALWAYS reach S after one bar"""
    p = pos(raw)
    if p <= 2:
        return ("spin", 12)
    if p >= 30:
        return ("stop", 0)
    if 15 <= p <= 17:
        return ("flat", 0)
    return ("step", PITCH[p])


def vary_pick(n, a, g_eff, rng):
    """one repeat's grid under VARY a: within +-a places of blue's division in its own family (straight / triplet),
    never longer than half the gate"""
    fam = STRAIGHT if n in STRAIGHT else TRIPLET
    i = fam.index(n) + rng.randint(-a, a)
    i = max(0, min(len(fam) - 1, i))
    while fam[i] * g_eff < 32 and i > 0:        # too long for the gate: shorter
        i -= 1
    return fam[i]


def readout(knob, raw, where="synth"):
    if knob == 0:
        return "GRID " + grid_of(raw)[1]
    if knob == 1:
        m, a, p = fortune_of(raw)
        if m == "always":
            return "ALWAYS"
        if m == "off":
            return "OFF" if where == "synth" else "OFF NO KEY"
        if m == "every":
            return "EVERY BAR" if a == 1 else f"EVERY {a} BARS"
        if m == "chance":
            return f"CHANCE {round(100 * p)}%"
        return f"VARY {a} {round(100 * p)}%"
    if knob == 2:
        g = gate_of(raw)
        if g >= 16:
            return {16: "GATE 1 BAR", 32: "GATE 2 BARS", 64: "GATE 4 BARS"}[g]
        return f"GATE {g // math.gcd(g, 16)}/{16 // math.gcd(g, 16)}"
    m, s = pitch_of(raw)
    return {"flat": "PITCH FLAT", "stop": "TAPE STOP", "spin": "SPIN UP"}.get(m) or f"PITCH {s:+d} ST"


# ------------------------------------------------------------------ geometry helpers
def polar(r, deg, cx=WX, cy=WY):
    """deg clockwise from 12 o'clock"""
    a = math.radians(deg - 90)
    return (cx + r * math.cos(a), cy + r * math.sin(a))


def arc_pts(r, a0, a1, n=None, cx=WX, cy=WY):
    n = n or max(2, int(abs(a1 - a0) / 4) + 1)
    return [polar(r, a0 + (a1 - a0) * i / (n - 1), cx, cy) for i in range(n)]


def rot(pts, deg, cx, cy):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in pts]


def circle(id, cx, cy, r, fill="none", stroke=None, sw=None):
    a = dict(cx=f(cx), cy=f(cy), r=f(r), fill=fill)
    if stroke:
        a.update(A(stroke=stroke, stroke_width=f(sw)))
    return N("circle", id, **a)


def stroke_path(id, pts, colour, sw, close=False, cap="round", join="round"):
    return N("path", id, d=P(*pts, close=close), fill="none",
             **A(stroke=colour, stroke_width=f(sw), stroke_linecap=cap, stroke_linejoin=join))


def fill_path(id, pts, colour):
    return N("path", id, d=P(*pts, close=True), fill=colour)


def blob(cx, cy, rx, ry, n=24):
    return [(cx + rx * math.cos(2 * math.pi * i / n), cy + ry * math.sin(2 * math.pi * i / n)) for i in range(n)]


def capsule(cx, cy, w, h, n=8):
    r = h / 2
    pts = [(cx + w / 2 - r + r * math.cos(math.pi * (-0.5 + i / n)), cy + r * math.sin(math.pi * (-0.5 + i / n)))
           for i in range(n + 1)]
    pts += [(cx - w / 2 + r + r * math.cos(math.pi * (0.5 + i / n)), cy + r * math.sin(math.pi * (0.5 + i / n)))
            for i in range(n + 1)]
    return pts


def rotm(a, cx=WX, cy=WY):
    r = math.radians(a)
    c, s = math.cos(r), math.sin(r)
    return (c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy)


# ------------------------------------------------------------------ artwork
S_CREATURE = 1.3        # creatures are drawn about (0, 0) and placed by a static group matrix


def book(n):
    """book<n>: closed (grey) or open (green pages): the creatures read their fortune (VARY)"""
    cy = 7
    closed = N("g", f"book{n}c", [fill_path(f"book{n}c_d", [(-6, cy - 1.5), (6, cy - 1.5), (6, cy + 2.5), (-6, cy + 2.5)],
                                            C["grey"]),
                                  stroke_path(f"book{n}c_e", [(-6, cy + 0.5), (6, cy + 0.5)], C["offgrey"], 0.8, cap="butt")])
    l = [(0, cy + 2.5), (-8, cy + 1), (-8, cy - 4), (0, cy - 2.5)]
    r = [(0, cy + 2.5), (8, cy + 1), (8, cy - 4), (0, cy - 2.5)]
    opened = N("g", f"book{n}o", [fill_path(f"book{n}o_l", l, C["green"]), fill_path(f"book{n}o_r", r, C["green"]),
                                  stroke_path(f"book{n}o_s", [(0, cy - 2.5), (0, cy + 2.5)], C["offgreen"], 0.8, cap="butt")])
    flip = N("g", f"book{n}f", [fill_path(f"book{n}f_d", [(0, cy - 2.5), (3, cy - 9), (5, cy - 8), (0.6, cy - 2)],
                                          C["lightgreen"])])
    return [closed, opened, flip]


def wings(side, span, lift, x0=0, y0=0):
    """one wing: a fan of three feathers as one outline"""
    pts = [(x0, y0)]
    for a, l in [(lift + 25, 1.0), (lift + 5, 0.92), (lift - 15, 0.75)]:
        t = math.radians(a)
        pts += [(x0 + side * span * l * math.cos(t), y0 - span * l * math.sin(t)),
                (x0 + side * span * l * 0.78 * math.cos(t - 0.16), y0 - span * l * 0.78 * math.sin(t - 0.16))]
    pts.append((x0 + side * 3, y0 + 4))
    return pts


def creature(name, cx, cy, n, parts):
    """the four evangelists in the corners, one palette (wings grey, body purplegrey, face lpg), on a flat pill"""
    kids = [fill_path(f"{name}_pill", capsule(0, 14, 40, 5), C["offblue"])] + parts + book(n)
    return N("g", name, kids, transform=f"matrix({S_CREATURE} 0 0 {S_CREATURE} {cx} {cy})")


def angel(cx, cy):
    return creature("angel", cx, cy, 1, [
        fill_path("angel_wingL", wings(-1, 15, 20, -3, 1), C["grey"]),
        fill_path("angel_wingR", wings(1, 15, 20, 3, 1), C["grey"]),
        fill_path("angel_robe", [(-5, -1), (5, -1), (8, 12), (-8, 12)], C["purplegrey"]),
        circle("angel_head", 0, -5.5, 4.2, C["lpg"])])


def eagle(cx, cy):
    return creature("eagle", cx, cy, 2, [
        fill_path("eagle_wingL", wings(-1, 17, 28, -2, 2), C["grey"]),
        fill_path("eagle_wingR", wings(1, 17, 28, 2, 2), C["grey"]),
        fill_path("eagle_body", blob(0, 3, 5.5, 8.5), C["purplegrey"]),
        circle("eagle_head", 0, -6, 3.8, C["lpg"]),
        fill_path("eagle_beak", [(-2.5, -6.5), (-7.5, -5), (-2.5, -4)], C["purplegrey"])])


def bull(cx, cy):
    return creature("bull", cx, cy, 3, [
        fill_path("bull_wingL", wings(-1, 14, 30, -7, 0), C["grey"]),
        fill_path("bull_wingR", wings(1, 14, 30, 7, 0), C["grey"]),
        fill_path("bull_head", [(-7, -6), (7, -6), (6, 2), (3, 8), (-3, 8), (-6, 2)], C["purplegrey"]),
        stroke_path("bull_horns", [(-6, -5), (-11, -7), (-12, -12), (-11, -7), (-6, -5), (6, -5), (11, -7), (12, -12)],
                    C["lpg"], 1.6),
        fill_path("bull_muzzle", blob(0, 5.5, 4, 2.6, 16), C["lpg"])])


def lion(cx, cy):
    mane = [((10 if i % 2 == 0 else 7.6) * math.cos(2 * math.pi * i / 16), -1 + (10 if i % 2 == 0 else 7.6)
             * math.sin(2 * math.pi * i / 16)) for i in range(16)]
    return creature("lion", cx, cy, 4, [
        fill_path("lion_wingL", wings(-1, 13, 30, -8, 0), C["grey"]),
        fill_path("lion_wingR", wings(1, 13, 30, 8, 0), C["grey"]),
        fill_path("lion_mane", mane, C["grey"]),
        circle("lion_face", 0, -1, 5.6, C["lpg"]),
        fill_path("lion_nose", [(-1.6, 0), (1.6, 0), (0, 1.8)], C["grey"])])


def letter(ch, deg):
    """T A R O, radial (tops outward), stroked lpg, each one subpath"""
    cx, cy = WX, WY - R_BAND
    h, w = 2.8, 2.4
    if ch == "T":
        pts = [(cx - w, cy - h), (cx + w, cy - h), (cx, cy - h), (cx, cy + h)]
    elif ch == "A":
        pts = [(cx - w, cy + h), (cx, cy - h), (cx + w, cy + h), (cx + w * 0.5, cy + 0.6), (cx - w * 0.5, cy + 0.6)]
    elif ch == "R":
        pts = [(cx - w * 0.8, cy + h), (cx - w * 0.8, cy - h), (cx + w * 0.5, cy - h), (cx + w * 0.9, cy - h * 0.55),
               (cx + w * 0.5, cy - 0.1), (cx - w * 0.8, cy - 0.1), (cx + w * 0.1, cy - 0.1), (cx + w, cy + h)]
    else:
        o = blob(cx, cy, w * 0.9, h, 16)
        pts = o + [o[0]]
    return stroke_path(f"lt_{ch}", rot(pts, deg, WX, WY), C["lpg"], 1.3)


def spokes(n):
    """spk<n>: n thin spokes (blue), as filled wedges chained along the hub circle (the hub disc covers the chain);
    chunks of 8 spokes keep every fill far from the AGG cell limit"""
    half = 1.15 if n <= 16 else 0.8
    kids = []
    for c0 in range(0, n, 8):
        pts = []
        for i in range(c0, min(n, c0 + 8)):
            a = 360 * i / n
            da0, da1 = math.degrees(half * 1.4 / R_SPK0), math.degrees(half / R_SPK1)
            pts += [polar(R_SPK0 - 2, a - da0), polar(R_SPK1, a - da1), polar(R_SPK1, a + da1), polar(R_SPK0 - 2, a + da0)]
        kids.append(fill_path(f"spk{n}_{c0 // 8 + 1}", pts, C["blue"]))
    return N("g", f"spk{n}", kids)


def wheel():
    rim = [circle("rimband", WX, WY, R_BAND, stroke=C["darkblue"], sw=8),
           circle("rimout", WX, WY, R_OUT, stroke=C["purplegrey"], sw=1.2),
           circle("rimin", WX, WY, R_BAND - 4, stroke=C["grey"], sw=0.8)]
    marks = [fill_path(f"mark{i + 1}", [polar(R_BAND - 2, a), polar(R_BAND, a + 2.6), polar(R_BAND + 2, a), polar(R_BAND, a - 2.6)],
                       C["grey"]) for i, a in enumerate((45, 135, 225, 315))]
    letters = [letter(ch, a) for ch, a in zip("TARO", (0, 90, 180, 270))]
    blur = [circle("blur1", WX, WY, 23, stroke=C["blue"], sw=2.4), circle("blur2", WX, WY, 29, stroke=C["blue"], sw=2.4)]
    lead = fill_path("leadspoke", [polar(R_SPK0 - 2, -6), polar(R_SPK1 + 1, -2.6), polar(R_SPK1 + 1, 2.6), polar(R_SPK0 - 2, 6)],
                     C["lightblue"])
    return N("g", "wheel", rim + marks + letters + [spokes(n) for n in SPOKES] + blur + [lead])


def gate_ring():
    """static: the bar as a clock face. Dim ring, beat ticks, 16 white segments (the gate: the bar's last gate/16,
    ending at 12 o'clock), 16 lpg ones (the stretch when the grid wins), two laps (2 / 4 BARS), the playhead"""
    kids = [circle("gatedim", WX, WY, R_GATE, stroke=C["offgrey"], sw=2.6)]
    for i in range(NGATE):
        a0, a1 = 360 * i / NGATE + 1.6, 360 * (i + 1) / NGATE - 1.6
        kids.append(stroke_path(f"gate{i + 1}", arc_pts(R_GATE, a0, a1, 5), C["white"], 2.6, cap="butt"))
        kids.append(stroke_path(f"gatex{i + 1}", arc_pts(R_GATE, a0, a1, 5), C["lpg"], 1.4, cap="butt"))
    kids += [circle("lap1", WX, WY, R_LAP1, stroke=C["white"], sw=1.1), circle("lap2", WX, WY, R_LAP2, stroke=C["white"], sw=1.1)]
    for b in range(4):
        kids.append(stroke_path(f"beat{b + 1}", [polar(R_GATE - 3, 90 * b), polar(R_GATE + 3, 90 * b)],
                                C["lpg"] if b == 0 else C["grey"], 1.2, cap="butt"))
    kids.append(N("g", "playhead", [stroke_path("playhalo", [polar(35, 0), polar(46, 0)], C["offblue"], 3.6, cap="butt"),
                                    stroke_path("playcore", [polar(35.6, 0), polar(45.4, 0)], C["lpg"], 1.6, cap="butt")]))
    return N("g", "gatering", kids)


def hub():
    kids = [circle("hubdisc", WX, WY, R_HUB, C["offblue"], C["grey"], 1),
            circle("chdim", WX, WY, R_CH, stroke=C["offgreen"], sw=3)]
    for i in range(NCH):
        kids.append(stroke_path(f"ch{i + 1}", arc_pts(R_CH, 360 * i / NCH + 3, 360 * (i + 1) / NCH - 3, 4), C["green"], 3, cap="butt"))
    kids += [circle("hubdot", WX, WY, 4.2, C["purplegrey"]),
             N("g", "hubman", [circle("hubman_d", WX, WY, 9, C["green"])])]      # manual REPEAT held: the hub lights
    return N("g", "hub", kids)


def sword_pts(colour_blade, colour_grip, pre, deg):
    px, py = SWORD_PV
    blade = rot([(px, py - 3), (px, py - 20)], deg, px, py)
    guard = rot([(px - 3.5, py - 3), (px + 3.5, py - 3)], deg, px, py)
    grip = rot([(px, py - 3), (px, py + 1)], deg, px, py)
    return [stroke_path(f"{pre}_blade", blade, colour_blade, 1.8), stroke_path(f"{pre}_guard", guard, colour_blade, 1.6),
            stroke_path(f"{pre}_grip", grip, colour_grip, 1.6)]


def sphinx():
    """seated, frontal, on the wheel's crown; the sword of fortune in its right paw (viewer's right)"""
    cx, by = WX, WY - R_OUT + 1.5
    body = [(cx - 12, by), (cx - 9, by - 9), (cx - 5, by - 12), (cx + 5, by - 12), (cx + 9, by - 9), (cx + 12, by)]
    nemes = [(cx - 7.5, by - 11), (cx - 5.2, by - 22), (cx, by - 24), (cx + 5.2, by - 22), (cx + 7.5, by - 11)]
    kids = [fill_path("sx_body", body, C["darkblue"]),
            stroke_path("sx_bodyedge", body, C["purplegrey"], 1),
            fill_path("sx_nemes", nemes, C["purplegrey"]),
            stroke_path("sx_stripes", [(cx - 6.4, by - 14), (cx - 5, by - 19.5), (cx - 3.6, by - 13.5)], C["darkblue"], 0.8),
            stroke_path("sx_stripes2", [(cx + 6.4, by - 14), (cx + 5, by - 19.5), (cx + 3.6, by - 13.5)], C["darkblue"], 0.8),
            fill_path("sx_face", blob(cx, by - 17.5, 3.4, 4.3, 16), C["lpg"]),
            fill_path("sx_pawL", blob(cx - 7.5, by - 1.2, 4, 1.8, 14), C["purplegrey"]),
            fill_path("sx_pawR", blob(cx + 7.5, by - 1.2, 4, 1.8, 14), C["purplegrey"])]
    sword = N("g", "sword", [N("g", "swordrest", sword_pts(C["grey"], C["grey"], "swr", SWORD_REST)),
                             N("g", "swordup", sword_pts(C["green"], C["lightgreen"], "swu", 0))])
    return N("g", "sphinx", kids + [sword])


def snake():
    """Typhon, descending on the left: a tapered body along the arc r SNAKE_R, head down; the group rotates about the
    wheel's centre, so the wheel carries it down as the repeats sink in pitch"""
    n = 22
    left, right = [], []
    for i in range(n + 1):
        u = i / n                                            # 0 head end .. 1 tail
        a = SNAKE_A0 + 4 + SNAKE_SPAN * u
        r = SNAKE_R + 2.6 * math.sin(u * 3.2 * math.pi) * (1 - 0.4 * u)
        w = 5 - 4 * u
        left.append(polar(r + w / 2, a))
        right.append(polar(r - w / 2, a))
    body = left + right[::-1]
    head = [polar(SNAKE_R + 3.8, SNAKE_A0 + 4.6), polar(SNAKE_R + 2.8, SNAKE_A0 + 0.6), polar(SNAKE_R, SNAKE_A0 - 3.4),
            polar(SNAKE_R - 2.8, SNAKE_A0 + 0.6), polar(SNAKE_R - 3.8, SNAKE_A0 + 4.6)]
    track = [circle(f"sinkdot{i + 1}", *polar(SNAKE_R + 7.5, SNAKE_A0 - SNAKE_TRAVEL * (i + 1) / NDOT), 1.3, C["orange"])
             for i in range(NDOT)]
    eye = polar(SNAKE_R + 0.9, SNAKE_A0 + 1.6)
    return [N("g", "sinktrack", track), N("g", "snake", [
        N("g", "snakegrey", [fill_path("snakegrey_b", body, C["grey"]), fill_path("snakegrey_h", head, C["grey"])]),
        N("g", "snakeorg", [fill_path("snakeorg_b", body, C["orange"]), fill_path("snakeorg_h", head, C["orange"])]),
        fill_path("snake_eye", blob(*eye, 0.8, 0.8, 8), C["black"])])]


def anubis():
    """Hermanubis, rising on the right: jackal head, tall ears, a long muzzle toward the wheel, a tapered torso, an
    arm on the rim. Feet on the arc r ANUB_R; the group translates upright along the arc"""
    x, b = polar(ANUB_R, ANUB_A0)
    torso = [(x - 3.5, b), (x - 6, b - 24), (x + 6, b - 24), (x + 3.5, b)]
    head = [(x - 3.5, b - 24), (x - 4.5, b - 27.5), (x - 12, b - 28.5), (x - 12, b - 30.5), (x - 4.5, b - 33), (x - 4, b - 37),
            (x - 2.8, b - 46), (x - 1, b - 38), (x + 1, b - 38), (x + 2.8, b - 46), (x + 4, b - 37), (x + 5, b - 31),
            (x + 3.5, b - 24)]
    track = [circle(f"risedot{i + 1}", *polar(ANUB_R + 12, ANUB_A0 - ANUB_TRAVEL * (i + 1) / NDOT), 1.3, C["orange"])
             for i in range(NDOT)]
    return [N("g", "risetrack", track), N("g", "anubis", [
        N("g", "anubgrey", [fill_path("anubgrey_t", torso, C["grey"]), fill_path("anubgrey_h", head, C["grey"])]),
        N("g", "anuborg", [fill_path("anuborg_t", torso, C["orange"]), fill_path("anuborg_h", head, C["orange"]),
                           stroke_path("anuborg_c", [(x - 5.6, b - 22), (x + 5.6, b - 22)], C["lightorange"], 1.4, cap="butt")]),
        stroke_path("anub_arm", [(x - 5, b - 19), (x - 11, b - 13), (x - 14, b - 15)], C["purplegrey"], 1.6),
        fill_path("anub_eye", blob(x - 2, b - 32, 0.8, 0.8, 8), C["black"])])]


def asset():
    kids = [angel(34, 20), eagle(286, 20), bull(34, 112), lion(286, 112),
            wheel(), gate_ring(), hub(), *snake(), *anubis(), sphinx()]
    return N("svg", None, kids, version="1.1", xmlns="http://www.w3.org/2000/svg", x="0", y="0", width="320", height="160",
             viewBox="0 0 320 160")


HEADER = f"""
wheel.svg - MOCKUP for the proposed "repeat" FX screen (a beat repeat; notes/fx-beatrepeat.md): the Tarot's Wheel of
Fortune, our own flat drawing (not a copy of any deck), generated by design/fx/gen_wheel.py (do not edit by hand).
No recolours: every change is a visibility flag (B[node + 0x50]) or a group matrix. Wheel centre ({WX}, {WY}).
gen_wheel.py runtime_state() is the reference for every id below.
  wheel             matrix = rotate(a) about the centre: a steps by 360 / slices-per-bar of the grid played on every
                    repeat (TAPE STOP / SPIN UP: turns continuously with the read position)          [the repeats]
  spk2 .. spk32     one shown: the grid played (slices per bar); 1/32T and 1/64 show none          [blue: GRID]
  blur1, blur2      1/32T: blur1, 1/64: both (the buzz)
  leadspoke         always shown (lightblue): the step per repeat is visible on symmetric spokes
  gate1 .. gate16   white, shown when i > 16 - gate (the gate within the bar, ending at 12)      [white: GATE]
  gatex1 .. gatex16 lpg, shown for the stretch when the grid wins (16 - gate_eff < i <= 16 - gate)
  lap1, lap2        GATE 2 BARS: lap1; 4 BARS: both. ALWAYS and the manual key: no gate (gatedim only)
  playhead          matrix = rotate(360 * bar phase) about the centre (one turn per bar, tape bars)
  ch1 .. ch12       ch<i> shown when i <= lots: chance x 12; EVERY n: 12 / n; ALWAYS 12; OFF 0; FX off: none
                    (a lost roll hides them for ~4 frames)                                      [green: FORTUNE]
  book<n>c/o/f      closed / open / a turning page; VARY a opens a books in the order bull 3, lion 4, eagle 2, angel 1
  sword             swordrest (grey) / swordup (green, a won roll to the gate's end; ALWAYS; the key held);
                    the raise turns swordup from rotate({SWORD_REST}) to 0 about {SWORD_PV} over 3 frames
  hubman            the hub lit green while the manual REPEAT key (SPLIT) is held
  snakegrey/snakeorg  orange while PITCH is down (a negative step, TAPE STOP)                     [orange: PITCH]
  snake             matrix = rotate(-{SNAKE_TRAVEL} * depth) about the centre: depth = the current repeat's |pitch| / 12
                    (TAPE STOP: the gate's progress); sinkdot1 .. {NDOT}: the setting (1 2 3 5 7 12 st -> 1 .. 6 dots)
  anubgrey/anuborg, anubis, risedot1 .. {NDOT}: the same for up (translate along r {ANUB_R} from {ANUB_A0} to
                    {ANUB_A0 - ANUB_TRAVEL} deg, upright)
  angel, eagle, bull, lion  static (the angel hidden while a readout shows at the title position)
Overlay (immediate, the C's stroke font): a stored-knob turn shows its value ~1.5 s at (8, 7) in the knob's colour.
"""


# ------------------------------------------------------------------ runtime model (the DSP's trigger, for the mockups)
def windows(k, upto, bar_s, seed):
    """gate windows [(start, end, won, slices)] that start before `upto` (s), and lost-roll times"""
    n, _ = grid_of(k[0])
    mode, a, p = fortune_of(k[1])
    g = eff_gate(n, gate_of(k[2]))
    gs = g / 16 * bar_s
    out, lost = [], []
    nb = int(upto / bar_s) + 2 + int(g / 16)
    for b in range(nb):
        E = (b + 1) * bar_s
        if E - gs > upto:
            break
        if mode == "every":
            win = (b + 1) % a == 0
        elif mode in ("chance", "vary"):
            win = random.Random(seed * 7919 + b).random() < p
        else:
            win = False
        if win:
            if out and out[-1][1] >= E - gs:          # a gate longer than the interval merges into one
                out[-1][1] = E
            else:
                out.append([E - gs, E])
        elif mode in ("chance", "vary"):
            lost.append(E - gs)
    return out, lost


def slices_of(k, start, end, bar_s, seed):
    """the slices of one window: [(t0, t1, n)], VARY picking each repeat's grid"""
    n, _ = grid_of(k[0])
    mode, a, _ = fortune_of(k[1])
    g = eff_gate(n, gate_of(k[2]))
    rng = random.Random(int(start * 1000) + seed)
    out, t = [], start
    first = True
    while t < end - 1e-9:
        m = n if first or mode != "vary" else vary_pick(n, a, g, rng)
        d = min(bar_s / m, end - t)
        out.append((t, t + d, m))
        t += d
        first = False
    return out


def timeline(k, t, bpm=120, seed=3, manual=None, bypass=False):
    """the FX at time t (s); bars of 4 beats from t = 0 (tape position 0). manual = (t_on, t_off) of SPLIT held"""
    bar_s = 240 / bpm
    n, _ = grid_of(k[0])
    mode, a, p = fortune_of(k[1])
    pm, S = pitch_of(k[3])
    st = dict(phase=(t % bar_s) / bar_s, active=False, rep=-1, depth=0.0, up=False, raise_u=1.0, flip=False,
              manual=False, n=n, turned=0.0, lostblink=False, bypass=bypass, latch=False)
    if bypass:
        return st
    six = bar_s / 16
    held = None
    if manual and manual[0] <= t < manual[1]:
        q = math.floor(manual[0] / six) * six
        held = q if manual[0] - q <= 0.030 else q + six      # within 30 ms after a 1/16 line: now, else the next
    if mode == "always":
        held = 0.0
        st["latch"] = True
    if held is not None:
        st["manual"] = not st["latch"]
        st["up"] = True
        if t >= held:
            sl = (t - held) * n / bar_s
            rep = int(sl)
            st.update(active=rep >= 1, rep=rep)
            frac = min(1.0, (t - held) / bar_s)
            if pm == "step":
                st["depth"] = min(1.0, abs(S * frac) / 12)
            elif pm in ("stop", "spin"):
                st["depth"] = frac
            st["turned"] = (sl - (sl % 1)) * 360 / n if pm not in ("stop", "spin") else \
                360 / n * (t - held) * n / bar_s * (1 - frac / 2 if pm == "stop" else 1 + frac / 2)
        return st
    wins, lost = windows(k, t, bar_s, seed)
    turned = 0.0
    for w0, w1 in wins:
        sl = slices_of(k, w0, w1, bar_s, seed)
        nsl = len(sl)
        if t >= w1:                                   # a finished window: its whole turn
            if pm in ("stop", "spin"):
                turned += 360 * (w1 - w0) / bar_s * (0.5 if pm == "stop" else 1.5)
            else:
                turned += sum(360 / m for _, _, m in sl[1:])
            continue
        if t < w0:
            break
        st["up"] = True
        st["raise_u"] = min(1.0, (t - w0) * 15 / 3)
        i = max(j for j, (t0, _, _) in enumerate(sl) if t0 <= t)
        st.update(active=i >= 1, rep=i, n=sl[i][2])
        frac = (t - w0) / (w1 - w0)
        if pm == "step":
            st["depth"] = min(1.0, abs(S * i / max(1, nsl - 1)) / 12)
        elif pm in ("stop", "spin"):
            st["depth"] = frac
        if pm == "stop":
            turned += 360 * (t - w0) / bar_s * (1 - frac / 2)
        elif pm == "spin":
            turned += 360 * (t - w0) / bar_s * (1 + frac / 2)
        else:
            turned += sum(360 / m for _, _, m in sl[1:i + 1])
        if mode == "vary" and i >= 1:
            st["flip"] = (t - sl[i][0]) < 0.27 and sl[i][2] != n
        break
    st["turned"] = turned
    st["lostblink"] = any(0 <= t - x < 0.27 for x in lost)
    return st


def runtime_state(k, rt):
    """{id: {"hide": bool, "m": matrix}} for knobs k (raw) and the timeline state rt; the reference for the C"""
    S = {}

    def st(i):
        return S.setdefault(i, {})
    n = rt["n"]
    for s in SPOKES:
        st(f"spk{s}")["hide"] = s != n
    st("blur1")["hide"] = n < 48
    st("blur2")["hide"] = n < 64
    st("wheel")["m"] = rotm(rt["turned"])
    g = gate_of(k[2])
    ge = eff_gate(grid_of(k[0])[0], g)
    nogate = rt["manual"] or rt["latch"]
    for i in range(1, NGATE + 1):
        st(f"gate{i}")["hide"] = nogate or i <= 16 - g
        st(f"gatex{i}")["hide"] = nogate or not (16 - ge < i <= 16 - g)
    st("lap1")["hide"] = nogate or g < 32
    st("lap2")["hide"] = nogate or g < 64
    st("playhead")["m"] = rotm(360 * rt["phase"])
    mode, a, p = fortune_of(k[1])
    lots = {"always": NCH, "off": 0, "every": round(NCH / max(1, a)), "chance": round(NCH * p),
            "vary": round(NCH * p)}[mode]
    if rt["bypass"] or rt["lostblink"]:
        lots = 0
    for i in range(1, NCH + 1):
        st(f"ch{i}")["hide"] = i > lots
    order = [3, 4, 2, 1]
    nbk = 0 if rt["bypass"] else 4 if mode == "always" else a if mode == "vary" else 0
    for j, b in enumerate(order):
        st(f"book{b}o")["hide"] = j >= nbk
        st(f"book{b}c")["hide"] = j < nbk
        st(f"book{b}f")["hide"] = not (rt["flip"] and j < nbk and rt["rep"] % nbk == j)
    up = rt["up"] and not rt["bypass"]
    st("swordup")["hide"] = not up
    st("swordrest")["hide"] = up
    if up and rt["raise_u"] < 1:
        st("swordup")["m"] = rotm(SWORD_REST * (1 - rt["raise_u"]), *SWORD_PV)
    st("hubman")["hide"] = not rt["manual"]
    pm, s = pitch_of(k[3])
    down = pm == "stop" or pm == "step" and s < 0
    upw = pm == "spin" or pm == "step" and s > 0
    st("snakeorg")["hide"] = not down
    st("snakegrey")["hide"] = down
    st("anuborg")["hide"] = not upw
    st("anubgrey")["hide"] = upw
    steps = [1, 2, 3, 5, 7, 12]
    dots = NDOT if pm in ("stop", "spin") else steps.index(abs(s)) + 1 if pm == "step" else 0
    for i in range(1, NDOT + 1):
        st(f"sinkdot{i}")["hide"] = not (down and i <= dots)
        st(f"risedot{i}")["hide"] = not (upw and i <= dots)
    d = rt["depth"] if rt["rep"] >= 0 else 0
    st("snake")["m"] = rotm(-SNAKE_TRAVEL * d) if down else (1, 0, 0, 1, 0, 0)
    p0, p1 = polar(ANUB_R, ANUB_A0), polar(ANUB_R, ANUB_A0 - ANUB_TRAVEL * d)
    st("anubis")["m"] = (1, 0, 0, 1, p1[0] - p0[0], p1[1] - p0[1]) if upw else (1, 0, 0, 1, 0, 0)
    return S


# ------------------------------------------------------------------ previews
KCOL = ["#698EFF", "#00ED95", "#FFFFFF", "#FF423F"]


def apply_state(svg_text, S, text=None, kcol=None):
    ET.register_namespace("", "http://www.w3.org/2000/svg")
    root = ET.fromstring(svg_text)
    ns = "{http://www.w3.org/2000/svg}"
    parent = {c: p for p in root.iter() for c in p}
    for el in list(root.iter()):
        i = el.get("id")
        if i in S:
            if S[i].get("hide"):
                parent[el].remove(el)
                continue
            if "m" in S[i]:
                el.set("transform", "matrix(" + " ".join(f"{v:.4f}" for v in S[i]["m"]) + ")")
    root.insert(0, ET.Element(ns + "rect", {"width": "320", "height": "160", "fill": "#000000"}))
    if text:
        for el in list(root.iter()):
            if el.get("id") == "angel":
                parent[el].remove(el)
        t = ET.SubElement(root, ns + "text", {"x": "8", "y": "17", "fill": kcol, "font-family": "Menlo, monospace",
                                              "font-size": "11", "font-weight": "bold"})
        t.text = text
    return ET.tostring(root, encoding="unicode")


def raw(p):
    return 32767 if p >= 32 else p * 1024


DEF = [raw(12), raw(5), raw(9), raw(16)]        # 1/16, EVERY 4 BARS, 4/16, FLAT
STATES = [  # name, knobs, time (s), manual, readout knob, extra, caption
    ("default", DEF, 0.9, None, None, {},
     "Default 1/16, EVERY 4 BARS, GATE 4/16, FLAT. Bar 1, beat 2: waiting (sword resting, the playhead tick orbits "
     "the bar clock; 3 of 12 lots = one bar in four)."),
    ("firing", DEF, 7.82, None, None, {},
     "Bar 4's last beat: the sword is up and the wheel steps one spoke per repeat (the lightblue lead spoke counts them)."),
    ("grid_wins", [raw(30), raw(19), raw(9), raw(16)], 1.3, None, None, {},
     "GRID 1/2 with GATE 4/16: the grid wins, the gate stretches to a bar (the stretch in lpg, the set gate white)."),
    ("buzz", [raw(1), raw(19), raw(15), raw(16)], 1.5, None, None, {},
     "GRID 1/64, EVERY BAR: no spokes, two blur rings and the lead spoke (the buzz, 32 Hz at 120 BPM)."),
    ("vary", [raw(6), raw(27), raw(15), raw(16)], None, None, None, {"find": 0.55},
     "GRID 1/32, VARY 3 at 83 %: three books open (bull, lion, eagle); this repeat drew another grid, so a page turns."),
    ("sink", [raw(12), raw(19), raw(15), raw(26)], 1.95, None, None, {},
     "PITCH -7 (5 dots): the staircase reaches -7 st at the gate's last repeat; the wheel carries the snake down."),
    ("tapestop", [raw(8), raw(7), raw(21), raw(31)], 3.25, None, None, {},
     "TAPE STOP, EVERY 2 BARS, GATE 1 BAR: the wheel slows like a dying turntable; the snake slides to the floor."),
    ("spinup", [raw(20), raw(19), raw(18), raw(1)], 1.9, None, None, {},
     "SPIN UP, GATE 3/4: the repeats rise to +12 st at the gate's end; Anubis climbs upright along the rim."),
    ("rise", [raw(12), raw(13), raw(12), raw(7)], None, None, None, {"find": 0.75},
     "PITCH +5, CHANCE 50 %, GATE 3/8 (a won bar)."),
    ("lost", [raw(12), raw(11), raw(9), raw(16)], None, None, None, {"lost": True},
     "CHANCE 30 %, a lost roll: the lots blink out for ~4 frames (a visible no)."),
    ("gate2bars", [raw(12), raw(5), raw(24), raw(16)], 3.0, None, None, {},
     "GATE 2 BARS in EVERY 4 BARS: the whole ring white plus one lap; the snare-roll buildup into the drop."),
    ("manual", [raw(12), raw(1), raw(9), raw(16)], 1.8, (1.03, 2.6), None, {},
     "OFF (no lots): only SPLIT fires. Held now: the hub lit green, sword up, no gate (gatedim only)."),
    ("always", [raw(16), 0, raw(9), raw(24)], 1.4, None, None, {},
     "ALWAYS (green's hard-left stop, one detent from OFF): the master FX's latch; every lot and book lit."),
    ("bypass", DEF, 0.9, None, None, {"bypass": True}, "FX off: no lots, the sword rests: fortune is off."),
    ("readout_grid", [raw(18), raw(5), raw(9), raw(16)], 0.5, None, 0, {}, "Readout after a blue turn: GRID 1/8 (the angel steps aside)."),
    ("readout_fortune", [raw(12), raw(24), raw(9), raw(16)], 0.5, None, 1, {}, "Readout: VARY 2 67% (one book open, the bull's)."),
    ("readout_gate", [raw(12), raw(5), raw(6), raw(16)], 0.5, None, 2, {}, "Readout: GATE 3/16 (white)."),
    ("readout_pitch", [raw(12), raw(5), raw(9), raw(32)], 0.5, None, 3, {}, "Readout: TAPE STOP (orange)."),
    ("readout_nokey", [raw(12), raw(1), raw(9), raw(16)], 0.5, None, 1, {"where": "master"},
     "Master / drum FX at OFF: OFF NO KEY (no manual key there yet)."),
]


def find_t(k, frac=None, lost=False, seed=3):
    bar_s = 2.0
    wins, lo = windows(k, 40, bar_s, seed)
    if lost:
        return lo[0] + 0.07
    w0, w1 = wins[0]
    return w0 + frac * (w1 - w0)


def rsvg(svg_text, out, scale=1):
    subprocess.run(["rsvg-convert", "-w", str(320 * scale), "-h", str(160 * scale), "-o", out], input=svg_text.encode(),
                   check=True)


def gallery(outdir):
    os.makedirs(outdir, exist_ok=True)
    src = open(SVG).read()
    import svg_cells
    worst = (0, "")
    cards = []
    for name, k, t, man, ro, ex, cap in STATES:
        if t is None:
            t = find_t(k, ex.get("find"), ex.get("lost", False))
        rt = timeline(k, t, manual=man, bypass=ex.get("bypass", False))
        S = runtime_state(k, rt)
        w = svg_cells.worst(SVG, S)
        worst = max(worst, (w[0], f"{name}: {w[1]} {w[2]}"))
        txt = apply_state(src, S, readout(ro, k[ro], ex.get("where", "synth")) if ro is not None else None,
                          KCOL[ro] if ro is not None else None)
        rsvg(txt, os.path.join(outdir, f"{name}.png"))
        rsvg(txt, os.path.join(outdir, f"{name}@2x.png"), 2)
        cards.append((name, k, cap, w))
    fr = os.path.join(outdir, "frames")
    os.makedirs(fr, exist_ok=True)
    for x in os.listdir(fr):
        os.remove(os.path.join(fr, x))
    seq = [(DEF, 5.6, 8.4), ([raw(12), raw(19), raw(15), raw(26)], 0.8, 2.2), ([raw(8), raw(7), raw(21), raw(31)], 1.8, 4.2),
           ([raw(10), raw(27), raw(12), raw(16)], None, None)]
    i = 0
    for k, a, b in seq:
        if a is None:
            a = find_t(k, 0.0) - 0.4
            b = a + 1.6
        for j in range(int((b - a) * 15)):
            rt = timeline(k, a + j / 15)
            S = runtime_state(k, rt)
            w = svg_cells.worst(SVG, S)
            worst = max(worst, (w[0], f"anim {i}: {w[1]} {w[2]}"))
            rsvg(apply_state(src, S), os.path.join(fr, f"f{i:03d}.png"), 2)
            i += 1
    subprocess.run(["magick", "-delay", "7", "-loop", "0", os.path.join(fr, "f*.png"), "-layers", "Optimize",
                    os.path.join(outdir, "wheel.gif")], check=True)
    html = ["<!doctype html><meta charset=utf-8><title>Wheel of Fortune</title><style>body{background:#111;color:#ddd;"
            "font:14px -apple-system,sans-serif;margin:24px}img{image-rendering:pixelated;border:1px solid #333}"
            ".c{display:inline-block;width:660px;margin:0 18px 26px 0;vertical-align:top}code{color:#9cf}</style>",
            "<h1>repeat (beat repeat): the Wheel of Fortune, after review round 1</h1>",
            "<p>Knob colours: <b style=color:#698EFF>blue GRID</b> = spokes (lightblue lead spoke counts the repeats), "
            "<b style=color:#00ED95>green FORTUNE</b> = lots in the hub, books, the sword, <b>white GATE</b> = the lit arc "
            "of the bar clock, <b style=color:#FF423F>orange PITCH</b> = the snake carried down / Anubis climbing. "
            "Rendered with rsvg from <code>design/fx/wheel.svg</code> and gen_wheel.py's runtime state. Worst AGG "
            f"estimate over every state and frame: ~{worst[0]} cells ({worst[1]}), {100 * worst[0] / 3840:.0f} % of 3840.</p>",
            "<div class=c><img src=wheel.gif width=640><p>Animation (2x, 15 fps): the default's fill on bar 4; PITCH -7 "
            "every bar, GATE 1/2; TAPE STOP, GATE 1 BAR; VARY 3 at 1/32 (pages turn when a repeat's grid varies).</p></div>"]
    for name, k, cap, w in cards:
        html.append(f"<div class=c><img src={name}@2x.png width=640><br><img src={name}.png> <code>{name}</code> "
                    f"knobs {k} &middot; worst ~{w[0]} cells<p>{cap}</p></div>")
    open(os.path.join(outdir, "index.html"), "w").write("\n".join(html))
    print(f"gallery: {outdir}/index.html; {i} frames; worst ~{worst[0]} cells ({worst[1]})")
    if worst[0] > 0.7 * 3840:
        sys.exit("AGG cell budget exceeded")


def main():
    xml = render(asset())
    open(SVG, "w").write('<?xml version="1.0" encoding="utf-8"?>\n<!--' + HEADER.replace("--", "-") + "-->\n" + xml)
    log = strictify.strictify_file(SVG)
    print("\n".join(log))
    py = sys.executable
    colors = os.path.join(OUT, "..", "..", "firmware", "work", "246", "content", "display", "colors.svg")
    if os.path.exists(colors):
        subprocess.run([py, os.path.join(OUT, "check_svg.py"), SVG, colors], check=True)
    subprocess.run([py, os.path.join(OUT, "svg_cells.py"), "--check", SVG], check=True)
    if len(sys.argv) > 1:
        gallery(sys.argv[1])


if __name__ == "__main__":
    main()
