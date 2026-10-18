#!/usr/bin/env python3
"""wheel.svg, the screen of the "repeat" FX (a beat repeat; notes/fx-beatrepeat.md): an original, flat TE-style
reading of the Tarot's Wheel of Fortune (card X), drawn by src/slot/ui_repeat.h (draw_wheel) through TE's SVG
renderer. The generator is the source: run it, never edit the SVG by hand.

  python3 design/fx/gen_wheel.py [GALLERY_DIR]

Writes design/fx/wheel.svg (every element one subpath of absolute M / L / C / Z, strictify.py as a safety net; no
recolours at run time: every state change is a visibility flag or a group matrix, see HEADER), checks it
(check_svg.py when firmware/work/246's colors.svg is there; svg_cells.py --check, and the cell estimate of every
gallery state and animation frame), and, with GALLERY_DIR, renders the gallery states (320x160 and 2x PNG,
rsvg-convert), an animation GIF (magick) and index.html.

The knob maps (grid_of, fortune_of, gate_of, pitch_of, eff_gate, vary_pick) are the reference for src/slot/repeat.h;
runtime_state() is the reference for ui_repeat.h (src/slot/test/repeat_screen_check.py compares the two); timeline()
models the DSP's trigger logic (bars of 4 beats from tape position 0, 120 BPM) for the gallery."""
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
SNAKE_R, SNAKE_A0, SNAKE_SPAN, SNAKE_TRAVEL = 61, 292, 26, 75   # on an arc left of the rim, carried down by the wheel
SINK_A0 = 300                                                    # its setting dots (from 300 deg down)
ANUB_R, ANUB_A0, ANUB_TRAVEL = 75, 125, 55                       # feet on an arc right of the rim, climbing upright
RISE_R = 84                                                      # its setting dots
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
        return f"VARY {round(100 * p)}% X{a}"
    if knob == 2:
        g = gate_of(raw)
        if g >= 16:
            return {16: "GATE 1 BAR", 32: "GATE 2 BARS", 64: "GATE 4 BARS"}[g]
        return f"GATE {g}/16"
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


def rotm(a, cx=WX, cy=WY):
    r = math.radians(a)
    c, s = math.cos(r), math.sin(r)
    return (c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy)


# ------------------------------------------------------------------ artwork: the figures (TE line art, round 4)
# Our own drawing of the RWS layout (Pamela Colman Smith's card, 1909, public domain), not a trace, in TE's line-art
# style (the stock screens: 1.5 strokes, square caps, few colours, black inside): every figure is outlined, filled
# black so it hides what lies behind it. Outlines: bodies purplegrey, heads / faces / pages lpg, feathers, clouds and
# inner lines grey; only the eyes and a few tiny parts are solid. Knob colours only where a knob acts: the books'
# green pages (VARY), the sword (the roll), the snake / Anubis orange (PITCH).
OL = 1.5                # TE's line width


def T(pts, dx=0.0, dy=0.0, sx=1.0, sy=None):
    sy = sx if sy is None else sy
    return [(dx + sx * x, dy + sy * y) for x, y in pts]


def ol(id, pts, colour, sw=OL, close=True, fill=None):
    """a TE line: closed outlines are filled black (they hide what is behind), open ones not"""
    return N("path", id, d=P(*pts, close=close), fill=fill or (C["black"] if close else "none"),
             **A(stroke=colour, stroke_width=f(sw), stroke_linecap="square", stroke_linejoin="round"))


def multi_fill(id, polys, colour):
    """several closed pieces in one fill (strictify.py bridges them into one subpath; the pieces must not overlap)"""
    return N("path", id, d=" ".join(P(*p, close=True) for p in polys), fill=colour)


def arc(cx, cy, rx, ry, a0, a1, n):
    """points on an ellipse, degrees counterclockwise from 3 o'clock (screen y down)"""
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / (n - 1))),
             cy - ry * math.sin(math.radians(a0 + (a1 - a0) * i / (n - 1)))) for i in range(n)]


def cloud(id, x0, y, bumps):
    """a TE line cloud: a scalloped top (bumps = [(width, height, lift)] left to right: each a half ellipse standing
    lift above the base) over a few shallow lobes, outlined darkblue (it sits back), black inside. Drawn over the figure's feet, so the
    creatures sit in their clouds"""
    pts, x = [], x0
    for w, h, lift in bumps:
        pts += arc(x + w / 2, y - lift, w / 2, h, 180, 0, 9)
        x += w
    nb = max(2, round((x - x0) / 18))                     # the underside, right to left
    wb = (x - x0) / nb
    for i in range(nb):
        pts += arc(x - wb * (i + 0.5), y, wb / 2, 1.6, 0, -180, 7)[1:]
    return ol(id, pts, C["darkblue"])                     # recessive, like lander's second lines


# the canonical wing: root at (0, 0), the tip toward +x, the leading edge up (y < 0); five primaries on the trailing
# edge; COVERT: the line where the coverts end
WING = [(0, 0), (0.18, -0.2), (0.42, -0.36), (0.7, -0.45), (0.92, -0.47), (1.0, -0.43),
        (0.83, -0.3), (0.93, -0.25), (0.74, -0.15), (0.83, -0.07), (0.63, -0.01), (0.69, 0.08),
        (0.5, 0.08), (0.53, 0.18), (0.34, 0.15), (0.32, 0.24), (0.15, 0.14), (0.02, 0.12)]
COVERT = [(0.6, -0.42), (0.52, -0.31), (0.42, -0.22), (0.3, -0.12), (0.16, -0.04)]


def wing_pts(shape, rx, ry, L, ang, side, w=1.0):
    """the wing placed: root (rx, ry), length L, tip at ang degrees above the horizontal, side +1 right / -1 left,
    w scales its width"""
    a = math.radians(ang)
    dx, dy = math.cos(a), -math.sin(a)
    ux, uy = dy, -dx                     # 90 degrees toward the leading edge (up for ang 0)
    out = []
    for u, v in shape:
        x = L * (u * dx - v * w * ux)
        y = L * (u * dy - v * w * uy)
        out.append((rx + side * x, ry + y))
    return out


def wing(pre, rx, ry, L, ang, side, w=1.0, covert=True):
    kids = [ol(pre, wing_pts(WING, rx, ry, L, ang, side, w), C["purplegrey"],
               fill=None if covert else "none")]          # a near wing hides the far one
    if covert:
        kids.append(ol(pre + "c", wing_pts(COVERT, rx, ry, L, ang, side, w), C["grey"], close=False))
    return kids


def book(n, x, y, tilt=0.0):
    """book<n> at (x, y) (the spine's foot): closed (an lpg outline, one grey page line), open (green pages and spine)
    or a turning page (lightgreen): the creatures read their fortune (VARY)"""
    def p(pts):
        return rot(T(pts, x, y), tilt, x, y)
    closed = N("g", f"book{n}c", [
        ol(f"book{n}c_d", p([(-7, -6.4), (3.4, -6.4), (7, -3.4), (7, 0), (-3.4, 0), (-7, -3)]), C["lpg"]),
        ol(f"book{n}c_p", p([(-7, -6.4), (-3.4, -3.4), (7, -3.4)]), C["grey"], 1, close=False)])
    opened = N("g", f"book{n}o", [
        ol(f"book{n}o_d", p([(0, 0), (-8.5, -1.5), (-9, -6.5), (0, -4.8), (9, -6.5), (8.5, -1.5)]), C["green"]),
        ol(f"book{n}o_s", p([(0, -0.2), (0, -4.6)]), C["green"], 1, close=False)])
    flip = N("g", f"book{n}f", [ol(f"book{n}f_d", p([(0, -4.8), (3.5, -12), (6, -11), (0.8, -4.2)]), C["lightgreen"],
                                   1.2)])
    return [closed, opened, flip]


def angel():
    """top left: the angel (a man, winged, robed and girdled) holding the book before its chest, in a cloud. Hidden
    while a readout shows at the title position"""
    x = 48
    kids = wing("angel_wingL", x - 5, 25, 26, 24, -1, 0.9) + wing("angel_wingR", x + 5, 25, 26, 24, 1, 0.9)
    robe = [(x - 6, 21), (x + 6, 21), (x + 9, 28), (x + 11, 44), (x + 15, 58), (x + 9, 59), (x + 3, 57.5), (x - 3, 59),
            (x - 9, 57.5), (x - 14, 58.5), (x - 10, 44), (x - 9, 28)]
    kids += [ol("angel_robe", robe, C["purplegrey"]),
             ol("angel_folds", [(x - 6, 56), (x - 3, 41), (x + 3.5, 41), (x + 7, 55.5)], C["grey"], close=False),
             ol("angel_head", arc(x, 14, 5, 5, 0, 360, 19)[:-1], C["lpg"]),
             ol("angel_hair", arc(x, 14, 5.6, 5.8, 205, -25, 13) + arc(x + 0.6, 12.4, 4, 2.4, -10, 190, 9), C["grey"]),
             multi_fill("angel_eyes", [blob(x - 2, 15, 0.8, 0.8, 8), blob(x + 2, 15, 0.8, 0.8, 8)], C["lpg"]),
             ol("angel_armL", [(x - 9, 27), (x - 6, 27), (x - 6, 33), (x - 10, 36), (x - 12, 34)], C["purplegrey"]),
             ol("angel_armR", [(x + 9, 27), (x + 6, 27), (x + 6, 33), (x + 10, 36), (x + 12, 34)], C["purplegrey"]),
             cloud("angel_cloud", 8, 66, [(12, 4, 1), (16, 6, 3), (14, 5, 2), (18, 7, 3), (12, 4, 1), (9, 3, 0)])]
    kids += book(1, x, 38)
    kids += [multi_fill("angel_hands", [blob(x - 10.5, 35, 1.6, 1.6, 10), blob(x + 10.5, 35, 1.6, 1.6, 10)], C["lpg"])]
    return N("g", "angel", kids)


def eagle():
    """top right: the eagle standing on its book, head turned toward the wheel, hooked beak, frown, wings raised"""
    x = 270
    kids = wing("eagle_wingL", x - 4, 25, 26, 30, -1, 0.9) + wing("eagle_wingR", x + 4, 25, 26, 30, 1, 0.9)
    body = [(x - 7, 16), (x - 1, 13.5), (x + 5, 17), (x + 8, 25), (x + 8, 34), (x + 5, 42), (x + 1, 47), (x - 4, 47),
            (x - 7, 40), (x - 9, 31), (x - 9.5, 22)]
    tail = [(x + 1, 41), (x + 6, 40), (x + 14, 49), (x + 11, 49.5), (x + 11.5, 52), (x + 7.5, 50.5), (x + 6.5, 53),
            (x + 2, 47)]
    kids += [ol("eagle_tail", tail, C["grey"], fill="none"),
             ol("eagle_legs", [(x - 4.6, 50.5), (x - 3.6, 44), (x + 1.6, 44), (x + 2.4, 50.5)], C["grey"], close=False),
             ol("eagle_body", body, C["purplegrey"]),
             ol("eagle_breast", [(x - 2, 22.5), (x, 28), (x - 1, 37), (x - 3.5, 41)], C["grey"], close=False),
             ol("eagle_head", blob(x - 4, 13, 5.2, 4.2, 18), C["lpg"]),
             ol("eagle_beak", [(x - 8.8, 10.2), (x - 13.6, 10.4), (x - 15.8, 12.4), (x - 15.6, 16), (x - 14.2, 13.6), (x - 9, 15.2)],
                C["lpg"]),
             circle("eagle_eye", x - 5.2, 12.4, 1, C["lpg"]),
             ol("eagle_brow", [(x - 8.6, 10.2), (x - 3, 9.2)], C["lpg"], close=False),
             cloud("eagle_cloud", 240, 64, [(12, 4, 0), (16, 6, 2), (13, 5, 3), (17, 7, 2), (12, 4, 1), (4, 2, 0)])]
    kids += book(2, x - 1, 56)
    return N("g", "eagle", kids)


def bull():
    """bottom left: the winged bull lying in its cloud, head toward the wheel, horns up, reading the book before it"""
    kids = wing("bull_wingF", 24, 103, 18, 108, 1, 0.9, covert=False)
    kids += wing("bull_wing", 31, 102, 28, 64, -1, 0.9)
    body = [(8, 115), (11, 106), (20, 102), (36, 101), (46, 102), (53, 106), (56, 113), (56, 126), (48, 127), (12, 127),
            (7, 122)]
    kids += [ol("bull_tail", [(8.5, 112), (5, 117), (5.5, 121), (8, 121.5)], C["grey"], close=False),
             ol("bull_body", body, C["purplegrey"]),
             ol("bull_haunch", arc(17, 117, 8, 7, 90, 270, 9)[1:-1] + [(17, 125)], C["purplegrey"], close=False),
             ol("bull_fold", [(45, 104), (43, 111), (45.5, 117)], C["purplegrey"], close=False),
             ol("bull_leg", [(42, 117), (58, 119.5), (62, 121.5), (61.5, 124.5), (40, 125)], C["purplegrey"]),
             ol("bull_ears", [(47, 98), (44, 93), (49, 96), (63, 96), (68, 93), (65, 98)], C["grey"], close=False),
             ol("bull_horns", [(46.5, 85), (47, 90), (51, 94), (56, 94.5), (61, 94), (65, 90), (65.5, 85)], C["lpg"],
                close=False),
             ol("bull_head", [(50, 94), (62, 94), (63.5, 99), (62, 106), (60.5, 112), (51.5, 112), (50, 106),
                              (48.5, 99)], C["purplegrey"]),
             ol("bull_muzzle", arc(56, 110, 5.2, 3.6, 0, 360, 17)[:-1], C["lpg"]),
             multi_fill("bull_eyes", [blob(52.6, 101, 1, 1, 8), blob(59.4, 101, 1, 1, 8)], C["lpg"]),
             multi_fill("bull_nostrils", [blob(54, 110.5, 0.8, 0.8, 8), blob(58, 110.5, 0.8, 0.8, 8)], C["grey"]),
             cloud("bull_cloud", 4, 137, [(9, 3, 0), (14, 6, 4), (16, 7, 6), (14, 5, 3)])]
    kids += book(3, 69, 120, -8)
    return N("g", "bull", kids)


def lion():
    """bottom right: the winged lion lying in its cloud, maned head toward the wheel, its book before the paws,
    the tail up with a tuft"""
    kids = wing("lion_wingF", 292, 104, 18, 108, -1, 0.9, covert=False)
    kids += wing("lion_wing", 284, 103, 28, 64, 1, 0.9)
    body = [(258, 108), (266, 104), (284, 103), (298, 104), (306, 108), (309, 116), (308, 126), (300, 127), (262, 127),
            (256, 120)]
    mane = []
    for i in range(16):
        a = 2 * math.pi * i / 16
        r = 13 if i % 2 == 0 else 9.5
        mane.append((254 + r * math.cos(a), 109 + 0.95 * r * math.sin(a)))
    kids += [ol("lion_tail", [(307, 112), (312, 108), (313, 100), (311.6, 95.4), (309.5, 91), (311.5, 88), (314, 91),
                              (313, 95), (311.6, 95.4)], C["grey"], close=False),
             ol("lion_body", body, C["purplegrey"]),
             ol("lion_haunch", arc(299, 118, 8, 8, 90, -90, 9)[1:-1] + [(292, 125)], C["purplegrey"], close=False),
             ol("lion_mane", mane, C["grey"]),
             ol("lion_face", T([(-4, -6), (4, -6), (6.5, -2), (5.5, 4), (2.5, 8), (-2.5, 8), (-5.5, 4), (-6.5, -2)],
                               254, 109, 0.85), C["lpg"]),
             multi_fill("lion_eyes", [blob(251.6, 107.2, 0.9, 0.9, 8), blob(256.4, 107.2, 0.9, 0.9, 8)], C["lpg"]),
             fill_path("lion_nose", [(252.4, 110), (255.6, 110), (254, 112)], C["lpg"]),
             ol("lion_mouth", [(251.6, 114.2), (254, 112), (256.4, 114.2)], C["lpg"], 1, close=False),
             ol("lion_legs", [(256, 118), (243, 119.5), (241, 122), (243, 125), (262, 126)], C["purplegrey"]),
             cloud("lion_cloud", 236, 137, [(9, 3, 1), (16, 6, 4), (14, 6, 6), (17, 7, 5), (14, 6, 4), (10, 4, 1)])]
    kids += book(4, 238.5, 121)
    return N("g", "lion", kids)


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
    rim = [circle("rimband", WX, WY, R_BAND, stroke=C["offblue"], sw=8),       # the band dark: the rim is two lines
           circle("rimout", WX, WY, R_OUT, stroke=C["purplegrey"], sw=OL),
           circle("rimin", WX, WY, R_BAND - 4, stroke=C["purplegrey"], sw=OL)]
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
    kids.append(N("g", "playhead", [stroke_path("playhalo", [polar(35, 0), polar(42.6, 0)], C["offblue"], 3.0, cap="butt"),
                                    stroke_path("playcore", [polar(35.6, 0), polar(42, 0)], C["lpg"], 1.5, cap="butt")]))
    return N("g", "gatering", kids)


def hub():
    kids = [circle("hubdisc", WX, WY, R_HUB, C["offblue"], C["grey"], 1),
            circle("chdim", WX, WY, R_CH, stroke=C["offgreen"], sw=3)]
    for i in range(NCH):
        kids.append(stroke_path(f"ch{i + 1}", arc_pts(R_CH, 360 * i / NCH + 3, 360 * (i + 1) / NCH - 3, 4), C["green"], 3, cap="butt"))
    kids += [circle("hubdot", WX, WY, 4.2, C["purplegrey"]),
             N("g", "hubman", [circle("hubman_d", WX, WY, 9, C["green"])])]      # manual REPEAT held: the hub lights
    return N("g", "hub", kids)


def sword_pts(colour_blade, colour_grip, pre, deg, length=17):
    """the blade (a long tapered fill with a point), the guard, the grip and the pommel (a sword from either end),
    turned deg about the pivot"""
    px, py = SWORD_PV
    blade = rot([(px - 0.9, py - 3), (px - 0.9, py - 1 - length), (px, py - 3 - length), (px + 0.9, py - 1 - length),
                 (px + 0.9, py - 3)], deg, px, py)
    guard = rot([(px - 3, py - 3.8), (px + 3, py - 3.8), (px + 3, py - 2.4), (px - 3, py - 2.4)], deg, px, py)
    grip = rot([(px, py - 3), (px, py + 3.5)], deg, px, py)
    pommel = rot(blob(px, py + 4.5, 1.3, 1.3, 10), deg, px, py)
    return [fill_path(f"{pre}_blade", blade, colour_blade), fill_path(f"{pre}_guard", guard, colour_blade),
            stroke_path(f"{pre}_grip", grip, colour_grip, 1.4), fill_path(f"{pre}_pommel", pommel, colour_grip)]


def sphinx():
    """seated on the wheel's crown, three-quarter, in line art: the nemes (lpg) with its stripes, a face with eyes,
    the brow band and the false beard, a pectoral collar, the lion body crouched along the crown (purplegrey, the
    haunch and tail to the viewer's right), the left forepaw on the rim and the right paw closed on the sword's grip
    at the pivot (the fist drawn over the turning sword)"""
    cx = WX - 2
    # the nemes: its half-width by height (the dome, then the lappets flaring to the shoulders)
    hw = [(3.4, 0), (4.0, 2.8), (5.0, 4.6), (6.6, 5.9), (9, 6.4), (13, 6.9), (19.4, 8.6)]

    def half(y):
        for (y0, w0), (y1, w1) in zip(hw, hw[1:]):
            if y0 <= y <= y1:
                return w0 + (w1 - w0) * (y - y0) / (y1 - y0)
        return hw[-1][1]
    nemes = [(cx + half(y), y) for y, _ in hw] + [(cx - half(y), y) for y, _ in hw[::-1]]
    stripes = []                                   # one line zigzagging down: across, then along the nemes' edge
    for i, y in enumerate((9.6, 13.4, 17.2)):           # three bands (five knotted at 1x)
        s = -1 if i % 2 == 0 else 1
        stripes += [(cx + s * (half(y) - 0.6), y), (cx - s * (half(y) - 0.6), y)]
    face = [(cx - 3.6, 8.2), (cx + 3.6, 8.2), (cx + 3.8, 12.4), (cx + 2.5, 15.6), (cx, 16.6), (cx - 2.5, 15.6),
            (cx - 3.8, 12.4)]
    base = 28.4                                  # a black gap above the rim's outer line (y 30)
    body = [(cx - 10, base), (cx - 10, 25), (cx - 8.2, 19), (cx + 7, 18.6), (cx + 9.6, 21.4)] + \
        arc(cx + 13, base, 9, 5.5, 140, 0, 9)
    leg = [(cx - 8.2, base), (cx - 7.8, 22.5), (cx - 3.2, 22.5), (cx - 3.8, base)]
    arm = [(cx + 3, 21.5), (cx + 7.6, 20.6), (SWORD_PV[0] + 2, SWORD_PV[1] - 1.6), (SWORD_PV[0] - 1.6, SWORD_PV[1] + 0.6)]
    collar = arc(cx, 18, 7.6, 4.6, 200, 340, 9) + arc(cx, 18.2, 4.4, 2.2, 340, 200, 7)
    kids = [ol("sx_tail", [(cx + 21, 27.6), (cx + 24, 24), (cx + 23, 21)], C["purplegrey"], close=False),
            ol("sx_body", body, C["purplegrey"]),
            ol("sx_leg", leg, C["purplegrey"]),
            fill_path("sx_pawL", blob(cx - 6, base, 3.6, 1.5, 14), C["purplegrey"]),
            ol("sx_nemes", nemes, C["lpg"]),
            ol("sx_stripes", stripes, C["purplegrey"], 1, close=False),
            ol("sx_collar", collar, C["grey"]),
            ol("sx_face", face, C["lpg"]),
            ol("sx_band", [(cx - 4, 8.4), (cx + 4, 8.4)], C["lpg"], close=False),
            multi_fill("sx_eyes", [[(cx - 3, 11), (cx - 0.9, 10.8), (cx - 1.1, 12), (cx - 2.8, 12)],
                                   [(cx + 3, 11), (cx + 0.9, 10.8), (cx + 1.1, 12), (cx + 2.8, 12)]], C["lpg"]),
            fill_path("sx_beard", [(cx - 1, 16.2), (cx + 1, 16.2), (cx + 0.8, 20.4), (cx - 0.8, 20.4)], C["purplegrey"]),
            ol("sx_arm", arm, C["purplegrey"])]
    sword = N("g", "sword", [N("g", "swordrest", sword_pts(C["purplegrey"], C["purplegrey"], "swr", SWORD_REST, 13.6)),
                             N("g", "swordup", sword_pts(C["green"], C["lightgreen"], "swu", 0))])
    fist = ol("sx_fist", blob(SWORD_PV[0], SWORD_PV[1] - 0.4, 2.2, 1.8, 12), C["purplegrey"])
    return N("g", "sphinx", kids + [sword, fist])


def snake():
    """Typhon, descending on the left: a tapered body in three waves along the arc r SNAKE_R, outlined (grey, or
    orange while PITCH is down), the head down with an eye and a forked tongue; the group rotates about the wheel's
    centre, so the wheel carries it down as the repeats sink in pitch"""
    n, span = 30, 30
    left, right = [], []
    for i in range(n + 1):
        u = i / n                                            # 0 the neck .. 1 the tail's tip
        a = SNAKE_A0 + 5 + span * u
        r = SNAKE_R + 2.8 * math.sin(u * 5 * math.pi) * (1 - 0.3 * u)
        w = 4.6 * (1 - u) ** 0.8 + 0.2
        left.append(polar(r + w / 2, a))
        right.append(polar(r - w / 2, a))
    body = left + right[::-1]
    h = SNAKE_A0
    head = [polar(SNAKE_R + 2.8, h + 6), polar(SNAKE_R + 4.1, h + 3), polar(SNAKE_R + 3.2, h - 1),
            polar(SNAKE_R + 0.6, h - 3.6), polar(SNAKE_R - 1.8, h - 3.2), polar(SNAKE_R - 3.5, h), polar(SNAKE_R - 3.1, h + 3),
            polar(SNAKE_R - 2.4, h + 6)]
    tongue = [polar(SNAKE_R + 1, h - 7.8), polar(SNAKE_R - 0.2, h - 5.6), polar(SNAKE_R - 0.2, h - 3.8),
              polar(SNAKE_R - 0.2, h - 5.6), polar(SNAKE_R - 1.4, h - 7.6)]
    track = [circle(f"sinkdot{i + 1}", *polar(SNAKE_R + 7.5, SINK_A0 - SNAKE_TRAVEL * (i + 1) / NDOT), 1.3, C["orange"])
             for i in range(NDOT)]
    a = SINK_A0 - SNAKE_TRAVEL * 7 / NDOT + 6                     # TAPE STOP's mark: a bar past the 6th dot
    track.append(stroke_path("sinkend", [polar(SNAKE_R + 4.5, a), polar(SNAKE_R + 10.5, a)], C["orange"], 1.8, cap="butt"))
    eye = polar(SNAKE_R + 0.9, h + 1)

    def variant(pre, c):
        return N("g", pre, [ol(f"{pre}_b", body, c, fill="none"), ol(f"{pre}_h", head, c),
                            ol(f"{pre}_t", tongue, c, 0.8, close=False)])
    return [N("g", "sinktrack", track), N("g", "snake", [
        variant("snakegrey", C["grey"]), variant("snakeorg", C["orange"]),
        fill_path("snake_eye", blob(*eye, 0.9, 0.9, 8), C["lpg"])])]


def anubis():
    """Hermanubis, rising on the right, in profile toward the wheel, in line art: the jackal head (tall ears, the
    long snout), a lpg collar, a purplegrey kilt, striding legs, an arm reaching for the rim (grey, or orange while
    PITCH is up). Feet on the arc r ANUB_R; the group translates upright along the arc"""
    x, b = polar(ANUB_R, ANUB_A0)
    x += 2.5                                     # drawn 2.5 out from the rim (the runtime moves it by a delta)

    def p(pts):
        return T(pts, x, b)
    body = p([(-9, 0), (-4.5, 0), (-3.4, -5), (-1.6, -11), (0.4, -5), (2.2, 0), (6.6, 0), (4.2, -6), (3.4, -12.5),
              (3.8, -18), (4.6, -23), (3.4, -26.5), (1.4, -28.4), (1.4, -31), (-2.2, -31), (-2.4, -28), (-4.6, -26.4),
              (-5.4, -22), (-3.8, -17), (-4, -13), (-6, -7.5)])
    head = p([(-2.6, -29.6), (-2.8, -33.6), (-5, -35.6), (-12.6, -34.2), (-13.4, -32.8), (-12.6, -32),
              (-6.6, -30.6), (-4.6, -29.2), (-2.6, -28.6), (1.6, -29.6), (3.2, -32.6), (3.6, -36.2), (4.4, -46.4),
              (1.8, -38.6), (0.4, -38.4), (-1, -45), (-2.4, -37.2)])
    arm = p([(-2.6, -25.8), (-0.4, -23.4), (-7.6, -18.6), (-12.6, -21.4), (-14.2, -20.4), (-13.4, -22.8),
             (-10.6, -23.8), (-7.8, -21.8)])
    kilt = p([(-4.3, -16.6), (3.6, -16.6), (4.6, -10.6), (-0.4, -11.4), (-6.6, -8.4)])
    collar = p([(-4.8, -25.8), (-2.6, -26.8), (1.4, -27.2), (3.4, -25.8)])
    track = [circle(f"risedot{i + 1}", *polar(RISE_R, ANUB_A0 - ANUB_TRAVEL * (i + 1) / NDOT), 1.3, C["orange"])
             for i in range(NDOT)]
    a = ANUB_A0 - ANUB_TRAVEL * 7 / NDOT + 3                      # SPIN UP's mark: a bar past the 6th dot
    track.append(stroke_path("riseend", [polar(RISE_R - 3, a), polar(RISE_R + 3, a)], C["orange"], 1.8, cap="butt"))

    def variant(pre, c):
        return N("g", pre, [ol(f"{pre}_t", body, c, fill="none"), ol(f"{pre}_h", head, c), ol(f"{pre}_a", arm, c)])
    return [N("g", "risetrack", track), N("g", "anubis", [
        variant("anubgrey", C["grey"]), variant("anuborg", C["orange"]),
        ol("anub_kilt", kilt, C["purplegrey"]), ol("anub_collar", collar, C["lpg"], close=False),
        fill_path("anub_eye", p([(-4.8, -34), (-2.8, -34.8), (-3.2, -33.4)]), C["lpg"])])]


MARKER = "wheel1"       # ui_repeat.h draws the file only if it finds this id (this generation: one subpath per element)


def asset():
    kids = [angel(), eagle(), bull(), lion(),
            wheel(), gate_ring(), hub(), *snake(), *anubis(), sphinx(), N("g", MARKER)]
    return N("svg", None, kids, version="1.1", xmlns="http://www.w3.org/2000/svg", x="0", y="0", width="320", height="160",
             viewBox="0 0 320 160")


HEADER = f"""
wheel.svg - the "repeat" FX screen (a beat repeat; notes/fx-beatrepeat.md; drawn by src/slot/ui_repeat.h): the Tarot's
Wheel of Fortune, our own flat drawing (not a copy of any deck), generated by design/fx/gen_wheel.py (do not edit).
No recolours: every change is a visibility flag (B[node + 0x50]) or a group matrix. Wheel centre ({WX}, {WY}).
gen_wheel.py runtime_state() is the reference for every id below.
  wheel             matrix = rotate(a) about the centre: a steps by 360 / slices-per-bar of the grid played on every
                    repeat (TAPE STOP / SPIN UP: turns continuously with the read position)          [the repeats]
  spk2 .. spk32     one shown: the grid played (slices per bar); 1/32T and 1/64 show none          [blue: GRID]
  blur1, blur2      1/32T: blur1, 1/64: both (the buzz)
  leadspoke         always shown (lightblue): the step per repeat is visible on symmetric spokes
  gate1 .. gate16   white, shown when i > 16 - gate (the gate within the bar, ending at 12)      [white: GATE]
  gatex1 .. gatex16 lpg, shown for the stretch when the grid wins (16 - gate_eff < i <= 16 - gate)
  lap1, lap2        GATE 2 BARS: lap1; 4 BARS: both. ALWAYS, the manual key, FORTUNE OFF and the FX off: no gate
                    (gatedim only: nothing will fire)
  playhead          matrix = rotate(360 * bar phase) about the centre (one turn per bar, tape bars); hidden with the
                    FX off
  ch1 .. ch12       ch<i> shown when i <= lots: chance x 12; EVERY n: 12 / n; ALWAYS 12; OFF 0; FX off: none
                    (a lost roll hides them for ~4 frames)                                      [green: FORTUNE]
  book<n>c/o/f      closed / open / a turning page; VARY a opens a books in the order bull 3, lion 4, eagle 2, angel 1
  sword             swordrest (grey) / swordup (green, a won roll to the gate's end; ALWAYS; the key held);
                    the raise turns swordup from rotate({SWORD_REST}) to 0 about {SWORD_PV} over 3 frames
  hubman            the hub lit green while the manual REPEAT key (SPLIT) is held
  snakegrey/snakeorg  orange while PITCH is down (a negative step, TAPE STOP)                     [orange: PITCH]
  snake             matrix = rotate(-{SNAKE_TRAVEL} * depth) about the centre: depth = the current repeat's |pitch| / 12
                    (TAPE STOP: the gate's progress); sinkdot1 .. {NDOT}: the setting (1 2 3 5 7 12 st -> 1 .. 6 dots);
                    sinkend / riseend: a bar past the 6th dot for TAPE STOP / SPIN UP
  anubgrey/anuborg, anubis, risedot1 .. {NDOT}: the same for up (translate along r {ANUB_R} from {ANUB_A0} to
                    {ANUB_A0 - ANUB_TRAVEL} deg, upright)
  angel, eagle, bull, lion  static (the angel hidden while a readout shows at the title position)
  {MARKER}            an empty group: the runtime draws this file only if it finds it
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
    mode0 = fortune_of(k[1])[0]
    nogate = rt["manual"] or rt["latch"] or rt["bypass"] or mode0 == "off"   # nothing will fire: the dim ring only
    for i in range(1, NGATE + 1):
        st(f"gate{i}")["hide"] = nogate or i <= 16 - g
        st(f"gatex{i}")["hide"] = nogate or not (16 - ge < i <= 16 - g)
    st("lap1")["hide"] = nogate or g < 32
    st("lap2")["hide"] = nogate or g < 64
    st("playhead")["m"] = rotm(360 * rt["phase"])
    st("playhead")["hide"] = rt["bypass"]
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
    st("sinkend")["hide"] = pm != "stop"
    st("riseend")["hide"] = pm != "spin"
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
