#!/usr/bin/env python3
"""Two pedals, HQ hybrid: pedals.svg (shippable asset, TE retained-renderer subset) + composite mock frames
(asset with runtime matrices / visibility / recolours applied + our immediate-mode overlay) + animated GIFs.
A wah (green, three-quarter view, rocking treadle) patched into a compact distortion box (blue / white / orange)."""
import json, math, os, re, subprocess

OUT = os.path.dirname(os.path.abspath(__file__))
RSVG = "/opt/homebrew/bin/rsvg-convert"
MAGICK = "/opt/homebrew/bin/magick"

C = dict(green="#00ED95", lightgreen="#80FFAF", blue="#698EFF", lightblue="#BFD8FF", white="#FFFFFF",
         orange="#FF423F", lightorange="#FFB3B3", grey="#7C7990", darkblue="#383572", purplegrey="#AEB1DC",
         lpg="#E6E7FF", purple="#5F4270", offgreen="#002826", darkgreen="#00475B", offblue="#1D2040",
         offgrey="#353238", offred="#4E2832", red="#FF3A5D", pink="#FF90A7", black="#010000")

MODES = ["TUBE", "MUFF", "FUZZ", "CLIP", "FOLD", "CRUSH"]          # white knob order
BODY = {"TUBE": "lightorange", "MUFF": "lpg", "FUZZ": "purple", "CLIP": "orange", "FOLD": "darkgreen", "CRUSH": "purplegrey"}
TRIM = {"TUBE": "black", "MUFF": "red", "FUZZ": "black", "CLIP": "black", "FOLD": "black", "CRUSH": "black"}   # plaque rim
GLOW = {"TUBE": "red", "MUFF": "red", "FUZZ": "pink", "CLIP": "white", "FOLD": "pink", "CRUSH": "red"}

# ---- distortion box, drawn in its own coordinates (x 116..204) and placed by the static group "dist" at +DX
DX = -64
WS = 1.0                            # wah group: translate(WTX 0) * scale(WS) about its heel corner on the floor
WO = (4, 136)
WTX = 184
WJI = (19, 128)                      # heel-side jack = wah OUTPUT (wah coordinates)
KX = [127, 149, 171, 193]
KY = 34
KCOL = ["blue", "green", "white", "orange"]
LED = (160, 48.5)
JY = 60
BX0, BX1, BY0, BY1 = 116, 204, 20, 138
PIV = (160, 138)
SPARKS = [(113, 21, 4.2), (207, 23, 3.6), (213, 77, 3), (107, 76, 3.2)]

# ---- wah, screen coordinates, oblique three-quarter view: side faces true shape, depth vector D
D = (11, -11)
FLOOR = 136
WX0, WX1, WPX = 10, 116, 63          # body ends, pivot x
ROOF_END, ROOF_PEAK = 122, 115
TX0, TX1, TB, TT = 5, 121, 111, 103  # treadle ends, bottom / top y (side face)
WPIV = (WPX, TB)                     # treadle pivot (axle)
MAXDEG = 9.0


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


class N:
    def __init__(self, tag, id=None, children=None, **a):
        self.tag, self.id, self.ch = tag, id, children or []
        self.a = {k.replace("_", "-"): v for k, v in a.items()}


def pts(P):
    return " ".join(f"{f(x)},{f(y)}" for x, y in P)


def poly_d(P):
    return "M" + " L".join(f"{f(x)} {f(y)}" for x, y in P) + " Z"


def rrect(x0, y0, x1, y1, r):
    k = r * 0.4477
    return (f"M{f(x0 + r)} {f(y0)} H{f(x1 - r)} C{f(x1 - k)} {f(y0)} {f(x1)} {f(y0 + k)} {f(x1)} {f(y0 + r)} "
            f"V{f(y1 - r)} C{f(x1)} {f(y1 - k)} {f(x1 - k)} {f(y1)} {f(x1 - r)} {f(y1)} H{f(x0 + r)} "
            f"C{f(x0 + k)} {f(y1)} {f(x0)} {f(y1 - k)} {f(x0)} {f(y1 - r)} V{f(y0 + r)} "
            f"C{f(x0)} {f(y0 + k)} {f(x0 + k)} {f(y0)} {f(x0 + r)} {f(y0)} Z")


def circ(cx, cy, r, id=None, **a):
    return N("circle", id, cx=f(cx), cy=f(cy), r=f(r), **a)


def spiky(cx, cy, r0, r1, n, ph=0.0):
    return [(cx + (r1 if i % 2 else r0) * math.cos(ph + math.pi * i / n),
             cy + (r1 if i % 2 else r0) * math.sin(ph + math.pi * i / n)) for i in range(2 * n)]


def star(cx, cy, r):
    return spiky(cx, cy, r, r * 0.28, 4, -math.pi / 2)[::-1]


def up(p, s=1.0):
    return (p[0] + D[0] * s, p[1] + D[1] * s)


# ------------------------------------------------------------------ the asset
def knob(i):
    cx, cy = KX[i], KY
    ticks = " ".join(f"M{f(cx + 7.4 * math.cos(a))} {f(cy + 7.4 * math.sin(a))} L{f(cx + 9 * math.cos(a))} {f(cy + 9 * math.sin(a))}"
                     for a in [2 * math.pi * j / 16 for j in range(16)])
    return N("g", f"k{i + 1}", [
        circ(cx, cy + 1.2, 9.6, fill=C["black"], opacity="0.45"),
        circ(cx, cy, 9.4, fill=C["black"], stroke=C["offgrey"], stroke_width="1"),
        N("path", None, d=ticks, fill="none", stroke=C["offgrey"], stroke_width="1"),
        circ(cx, cy, 5.3, fill=C[KCOL[i]], stroke=C["black"], stroke_width="0.8"),
        N("g", f"p{i + 1}", [N("line", None, x1=f(cx), y1=f(cy - 2.2), x2=f(cx), y2=f(cy - 9.2), stroke=C["white"],
                                stroke_width="1.8", stroke_linecap="round"),
                              N("line", None, x1=f(cx), y1=f(cy - 2.2), x2=f(cx), y2=f(cy - 5), stroke=C["black"],
                                stroke_width="1.8", stroke_linecap="round")])])


def wah():
    side = [(WX0, FLOOR), (WX1, FLOOR), (WX1, ROOF_END), (WPX, ROOF_PEAK), (WX0, ROOF_END)]
    roofL = [(WX0, ROOF_END), (WPX, ROOF_PEAK), up((WPX, ROOF_PEAK)), up((WX0, ROOF_END))]
    roofR = [(WPX, ROOF_PEAK), (WX1, ROOF_END), up((WX1, ROOF_END)), up((WPX, ROOF_PEAK))]
    endR = [(WX1, FLOOR), up((WX1, FLOOR)), up((WX1, ROOF_END)), (WX1, ROOF_END)]
    # treadle, drawn level; ttop (rubber) and tside get their own runtime matrices
    top = [(TX0, TT), (TX1, TT), up((TX1, TT)), up((TX0, TT))]
    ribs = " ".join(f"M{f(x + 1.5)} {f(TT - 1.5)} L{f(x + D[0] - 1.5)} {f(TT + D[1] + 1.5)}" for x in range(TX0 + 7, TX1 - 3, 8))
    tside = [(TX0 + 2, TB), (TX1 - 3, TB), (TX1, TB - 3), (TX1, TT), (TX0, TT), (TX0, TB - 2)]
    sw = dict(stroke_width="1", stroke_linejoin="round")
    wo = up((WX1, 127.5), 0.45)
    return N("g", "wah", [
        N("path", "cableI", d=f"M{f(wo[0] + 9)} {f(wo[1])} C{f(wo[0] + 16)} {f(wo[1])} {f(wo[0] + 14)} {f(wo[1] - 26)} 160 {f(wo[1] - 30)}",
          fill="none", stroke=C["offgrey"], stroke_width="3", stroke_linecap="round"),
        N("path", "plugW", d=rrect(wo[0], wo[1] - 3.2, wo[0] + 6, wo[1] + 3.2, 1.2) + " " + rrect(wo[0] + 5.5, wo[1] - 2.3, wo[0] + 10, wo[1] + 2.3, 1),
          fill=C["grey"], stroke=C["purplegrey"], stroke_width="0.8"),
        N("ellipse", "wshadow", cx="66", cy=str(FLOOR + 1), rx="60", ry="2.6", fill=C["offblue"]),
        N("path", "wroofL", d=poly_d(roofL), fill=C["grey"], stroke=C["grey"], **sw),
        N("path", "wroofR", d=poly_d(roofR), fill=C["grey"], stroke=C["grey"], **sw),
        N("path", "wend", d=poly_d(endR), fill=C["offgrey"], stroke=C["grey"], **sw),
        N("path", "wside", d=poly_d(side), fill=C["offgrey"], stroke=C["grey"], **sw),
        N("line", "wfoot", x1=str(WX0 + 4), y1=str(FLOOR - 1), x2=str(WX1 - 4), y2=str(FLOOR - 1), stroke=C["black"],
          stroke_width="2", opacity="0.6"),
        # axle bracket on the side
        N("path", "wbrk", d=poly_d([(WPX - 9, ROOF_PEAK + 2), (WPX + 9, ROOF_PEAK + 2), (WPX + 4, TB - 1), (WPX - 4, TB - 1)]),
          fill=C["purplegrey"], stroke=C["grey"], stroke_width="0.8"),
        N("g", "ttop", [N("path", "rubber", d=poly_d(top), fill=C["black"], stroke=C["purplegrey"], stroke_width="1",
                          stroke_linejoin="round"),
                        N("path", "ribs", d=ribs, fill="none", stroke=C["green"], stroke_width="1.6", stroke_linecap="round")]),
        N("g", "tside", [N("path", "chrome", d=poly_d(tside), fill=C["purplegrey"], stroke=C["lpg"], stroke_width="1",
                           stroke_linejoin="round"),
                         N("line", "shine", x1=str(TX0 + 6), y1=str(TT + 2.4), x2=str(TX1 - 8), y2=str(TT + 2.4),
                           stroke=C["white"], stroke_width="1.2", stroke_linecap="round")]),
        circ(WPX, TB - 3.5, 3.4, "axle", fill=C["green"], stroke=C["black"], stroke_width="1"),
        # jacks: OUTPUT on the heel side face (the patch leaves toward the box on the left), INPUT on the toe end face
        circ(WJI[0], WJI[1], 3.4, "wjackO", fill=C["grey"], stroke=C["purplegrey"], stroke_width="1"),
        N("path", "wjackI", d=poly_d([up((WX1, 124), 0.25), up((WX1, 124), 0.65), up((WX1, 131), 0.65), up((WX1, 131), 0.25)]),
          fill=C["black"], stroke=C["purplegrey"], stroke_width="0.8"),
    ], transform=f"matrix({WS} 0 0 {WS} {f(WTX + WO[0] - WS * WO[0])} {f(WO[1] - WS * WO[1])})")


def wah_matrix():
    return (WS, 0, 0, WS, WTX + WO[0] - WS * WO[0], WO[1] - WS * WO[1])


def patch_path(p):
    end = (DX + 236, JY)
    return N("path", "patch", d=f"M{f(p[0])} {f(p[1])} C{f(p[0] - 26)} {f(p[1] + 2)} {f(end[0] + 14)} {f(end[1] + 4)} {f(end[0])} {f(end[1])}",
             fill="none", stroke=C["offgrey"], stroke_width="3.4", stroke_linecap="round")


def asset():
    body = BODY["CLIP"]
    dist = N("g", "dist", [
        N("ellipse", "shadow", cx="160", cy="139", rx="54", ry="2.6", fill=C["offblue"]),
        N("g", "pedal", [
            N("path", "cableO", d=f"M88 {JY} C80 {JY} 76 {JY + 6} 75 {JY + 34} S74 138 72 146", fill="none",
              stroke=C["offgrey"], stroke_width="3.4", stroke_linecap="round"),
            N("path", "plugL", d=rrect(88, JY - 2.8, 96, JY + 2.8, 1.2) + " " + rrect(95, JY - 4.2, 110, JY + 4.2, 1.5),
              fill=C["grey"], stroke=C["purplegrey"], stroke_width="0.8"),
            N("path", "plugR", d=rrect(224, JY - 2.8, 236, JY + 2.8, 1.2) + " " + rrect(210, JY - 4.2, 225, JY + 4.2, 1.5),
              fill=C["grey"], stroke=C["purplegrey"], stroke_width="0.8"),
            N("rect", "collar", x="99.4", y=str(JY - 4.2), width="3.6", height="8.4", fill=C["orange"], stroke=C["orange"], stroke_width="0.6"),
            N("rect", "jackL", x="109", y=str(JY - 6), width="8", height="12", fill=C["purplegrey"], stroke=C["grey"], stroke_width="0.8"),
            N("rect", "jackR", x="203", y=str(JY - 6), width="8", height="12", fill=C["purplegrey"], stroke=C["grey"], stroke_width="0.8"),
            N("path", "body", d=rrect(BX0, BY0, BX1, BY1, 6), fill=C[body], stroke=C[body], stroke_width="1"),
            N("rect", "shade", x=str(BX1 - 6), y=str(BY0 + 3), width="4.6", height=str(BY1 - BY0 - 6), fill=C["black"], opacity="0.22"),
            N("line", "hilite", x1=str(BX0 + 2.2), y1=str(BY0 + 6), x2=str(BX0 + 2.2), y2=str(BY1 - 6), stroke=C["white"],
              stroke_width="1.2", opacity="0.35"),
            N("line", "lip", x1=str(BX0 + 6), y1=str(BY0 + 2), x2=str(BX1 - 6), y2=str(BY0 + 2), stroke=C["white"],
              stroke_width="1", opacity="0.3"),
            *[knob(i) for i in range(4)],
            N("g", "glow", [N("path", "rays", d=" ".join(
                f"M{f(LED[0] + 5 * math.cos(a))} {f(LED[1] + 5 * math.sin(a))} L{f(LED[0] + 8.2 * math.cos(a))} {f(LED[1] + 8.2 * math.sin(a))}"
                for a in [math.radians(v) for v in (-180, -135, -90, -45, 0)]),
                fill="none", stroke=C["white"], stroke_width="1.5", stroke_linecap="round")]),
            circ(LED[0], LED[1], 3.6, "bezel", fill=C["black"]),
            circ(LED[0], LED[1], 2.2, "led", fill=C["red"], stroke=C["red"], stroke_width="0.6"),
            N("path", "plaque", d=rrect(132, 55, 188, 74, 2.5), fill=C["black"], stroke=C["black"], stroke_width="1.4"),
            N("line", "hinge", x1=str(BX0 + 1), y1="77.5", x2=str(BX1 - 1), y2="77.5", stroke=C["black"], stroke_width="2.4",
              opacity="0.55"),
            N("g", "plate", [
                N("path", "plateb", d=rrect(BX0 + 1.5, 79, BX1 - 1.5, BY1 - 1, 4), fill=C[body], stroke=C["black"], stroke_width="1.2"),
                N("path", "pad", d=rrect(123, 84, 197, 123, 3), fill=C["black"], stroke=C["black"], stroke_width="1"),
                N("path", "grooves", d=" ".join(f"M127 {f(y - 0.8)} H193 V{f(y + 0.8)} H127 Z" for y in [88 + 4.2 * j for j in range(9)]),
                  fill=C["offgrey"]),
                circ(160, 130.5, 3.6, "screw", fill=C["purplegrey"], stroke=C["grey"], stroke_width="0.8"),
                N("line", "slot", x1="157.6", y1="130.5", x2="162.4", y2="130.5", stroke=C["grey"], stroke_width="1"),
            ]),
        ]),
        *[N("g", f"s{i + 1}", [N("polygon", None, points=pts(star(x, y, r)), fill=C["lightblue"], stroke=C["blue"], stroke_width="0.6")])
          for i, (x, y, r) in enumerate(SPARKS)],
    ], transform=f"translate({DX} 0)")
    # patch: the wah's output (heel side) -> the box's input plug on its right side
    kids = [patch_path(apply(wah_matrix(), WJI)), wah(), dist]
    return N("svg", None, kids, version="1.1", xmlns="http://www.w3.org/2000/svg", x="0", y="0", width="320", height="160",
             viewBox="0 0 320 160")


HEADER = """
pedals.svg - "distortion" FX screen (Two Pedals). Our own artwork: a wah (green) patched into a compact distortion box.
Pedalboard convention: signal enters on the RIGHT and leaves on the LEFT. Input cable from the screen's right edge into the wah's
toe-end jack (right); patch from the wah's heel-side jack (left) to the box's right jack (input); output from the box's left jack
(orange collar) toward the screen's left edge.
Drawn by TE's retained renderer, then our immediate-mode overlay on top. Ids are found once at load (find, assert=0) and cached.
d = blue drive 0..1, g = green, c = white character, m = orange mix 0..1, t = frame clock (s).
jit(t, seed, rate) = deterministic noise -1..1 that changes `rate` times per second. mode = MODES[round(c * 5)]:
TUBE MUFF FUZZ CLIP FOLD CRUSH.
WAH (green; static group "wah" = translate(184 0) * scale(1.0) about (4, 136)), oblique view: side faces true shape, depth vector D = (11, -11). a = -1 (green 0, low-pass: heel down)
  .. 0 (16384 +- 2048, flat: level) .. +1 (green max, focus: toe down); theta = 9 deg * a (positive = toe down, clockwise).
  tside          matrix = rotate(theta) about the axle (63, 111)
  ttop           matrix = the same rotation for the treadle's long edge, depth kept: linear part [[c, c-1], [s, s+1]]
                 (columns), translation so that (63, 103) -> rotate(theta)(63, 103); c, s = cos, sin(theta)
  ribs           green rubber ribs (ride with ttop); axle = green pivot pin (static)
DISTORTION (static group "dist" = translate(-64 0); its children in the box's own coordinates):
  pedal          matrix = rotate(1.8 deg * d^1.5 * jit) about (160, 138) * translate(3.2 d^1.15 * jit, 2.0 d^1.15 * jit),
                 jitter rate 8 + 16 d Hz: the whole box shakes, still at d = 0                                     [blue]
  plate          matrix = translate(2.6 d^1.1 * jit, 1.8 d^1.1 * jit): the foot-plate rattles                       [blue]
  s1..s4         sparks, shown for d > 0.72 / 0.8 / 0.88 / 0.95, blinking; small rotate jitter                    [blue]
  glow           LED rays, shown when m > 0 and d > 0.03; matrix = scale(0.55 + 0.75 d, flicker) about the LED   [blue]
  p1..p4         matrix = rotate(-135 + 270 * knob deg) about (127|149|171|193, 34)          [all four; the green knob stays as a small indicator]
  body, plateb   fill (+ body stroke) = mode colour: TUBE lightorange, MUFF lpg, FUZZ purple, CLIP orange, FOLD darkgreen,
                 CRUSH purplegrey; plaque stroke = red for MUFF (silver box, red rim), black otherwise          [white]
  rays           stroke per mode for contrast (TUBE/MUFF/CRUSH red, FUZZ/FOLD pink, CLIP white)
  word           overlay: the mode word in our stroke font (font.h), centred in the plaque, follows the pedal matrix
MIX FADE (orange, both pedals; mix 0 is fully dry): v = 0.15 + 0.85 m; colour = lerp(black, colour, v), RGBA written per frame:
  distortion: shadow cableO plugL plugR collar jackL jackR body hilite lip plateb grooves screw slot (14)
  wah + cables: wshadow wroofL wroofR wend wside wbrk rubber chrome shine wjackI wjackO cableI patch plugW (14)
  ribs           lerp(black, green, 0.4 + 0.6 v); led red (lerped) when m > 0, offred at 0
  NOT faded: knob skirts/caps/pointers and the green axle (the controls stay readable at mix 0); black/shading parts.
Overlay (immediate mode): the mode word only, step 1.8, width 1.5: 9-25 AA segments; colour by v: white / purplegrey / grey.
"""


# ------------------------------------------------------------------ runtime model
def mul(m1, m2):
    a1, b1, c1, d1, e1, f1 = m1
    a2, b2, c2, d2, e2, f2 = m2
    return (a1 * a2 + c1 * b2, b1 * a2 + d1 * b2, a1 * c2 + c1 * d2, b1 * c2 + d1 * d2,
            a1 * e2 + c1 * f2 + e1, b1 * e2 + d1 * f2 + f1)


def rot(deg, px, py):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return (c, s, -s, c, px - c * px + s * py, py - s * px - c * py)


def scale_about(sx, sy, px, py):
    return (sx, 0, 0, sy, px - sx * px, py - sy * py)


def tr(x, y):
    return (1, 0, 0, 1, x, y)


def apply(M, p):
    return (M[0] * p[0] + M[2] * p[1] + M[4], M[1] * p[0] + M[3] * p[1] + M[5])


def jit(t, seed, rate=12.0):
    n = int(t * rate) + seed * 97
    return math.sin(n * 12.9898 + seed * 78.233) * 0.5 + math.sin(n * 4.1414 + seed) * 0.5


def lerp_col(name, v):
    h = C[name]
    return "#%02X%02X%02X" % tuple(int(round(int(h[i:i + 2], 16) * v)) for i in (1, 3, 5))


def mode(k):
    return MODES[max(0, min(5, int(round(k[2] / 32767 * 5))))]


def wah_amount(g):
    if g < 14336:
        return -(14336 - g) / 14336
    if g > 18432:
        return (g - 18432) / 14335
    return 0.0


def treadle_matrices(a):
    th = MAXDEG * a
    R = rot(th, *WPIV)
    c, s = math.cos(math.radians(th)), math.sin(math.radians(th))
    # linear part: e1 -> (c, s) (the long edge rotates), D -> D (depth kept): columns (c, s), (c - 1, s + 1) for D = (k, -k)
    L = (c, s, c - 1, s + 1, 0, 0)
    P0 = (WPX, TT)
    q = apply(R, P0)
    lp = apply(L, P0)
    return R, (c, s, c - 1, s + 1, q[0] - lp[0], q[1] - lp[1])


def pedal_matrix(d, t):
    rate = 8 + 16 * d
    th = 1.8 * d ** 1.5 * jit(t, 7, rate)
    return mul(rot(th, *PIV), tr(3.2 * d ** 1.15 * jit(t, 1, rate), 2.0 * d ** 1.15 * jit(t, 2, rate)))


FADE = {"shadow": ("offblue", None), "cableO": (None, "offgrey"), "plugL": ("grey", "purplegrey"), "plugR": ("grey", "purplegrey"),
        "collar": ("orange", "orange"), "jackL": ("purplegrey", "grey"), "jackR": ("purplegrey", "grey"), "hilite": (None, "white"),
        "lip": (None, "white"), "grooves": ("offgrey", None), "screw": ("purplegrey", "grey"), "slot": (None, "grey"),
        "wshadow": ("offblue", None), "wroofL": ("grey", "grey"), "wroofR": ("grey", "grey"), "wend": ("offgrey", "grey"),
        "wside": ("offgrey", "grey"), "wbrk": ("purplegrey", "grey"), "rubber": (None, "purplegrey"), "chrome": ("purplegrey", "lpg"),
        "shine": (None, "white"), "wjackI": (None, "purplegrey"), "wjackO": ("grey", "purplegrey"), "cableI": (None, "offgrey"),
        "patch": (None, "offgrey"), "plugW": ("grey", "purplegrey")}


def runtime_state(k, t):
    d, g, c, m = [v / 32767 for v in k]
    md = mode(k)
    v = 0.15 + 0.85 * m
    rate = 8 + 16 * d
    S = {}
    S["pedal"] = dict(m=pedal_matrix(d, t))
    S["plate"] = dict(m=tr(2.6 * d ** 1.1 * jit(t, 3, rate), 1.8 * d ** 1.1 * jit(t, 4, rate)))
    for i in range(4):
        S[f"p{i + 1}"] = dict(m=rot(-135 + 270 * k[i] / 32767, KX[i], KY))
    Rm, Am = treadle_matrices(wah_amount(k[1]))
    S["tside"] = dict(m=Rm)
    S["ttop"] = dict(m=Am)
    for id_, (fc, sc) in FADE.items():
        S[id_] = {}
        if fc:
            S[id_]["fill"] = lerp_col(fc, v)
        if sc:
            S[id_]["stroke"] = lerp_col(sc, v)
    S["ribs"] = dict(stroke=lerp_col("green", 0.4 + 0.6 * v))
    S["body"] = dict(fill=lerp_col(BODY[md], v), stroke=lerp_col(BODY[md], v))
    S["plateb"] = dict(fill=lerp_col(BODY[md], v))
    S["plaque"] = dict(stroke=lerp_col(TRIM[md], v))
    on = m > 0
    S["led"] = dict(fill=lerp_col("red", v) if on else C["offred"], stroke=lerp_col("red", v) if on else C["offred"])
    S["rays"] = dict(stroke=C[GLOW[md]])
    if on and d > 0.03:
        sc = (0.55 + 0.75 * d) * (1 + 0.06 * d * jit(t, 5, 20))
        S["glow"] = dict(m=scale_about(sc, sc, *LED))
    else:
        S["glow"] = dict(hide=True)
    for i, th in enumerate([0.72, 0.8, 0.88, 0.95]):
        x, y, r = SPARKS[i]
        if d > th and jit(t, 10 + i, 15) > -0.35:
            S[f"s{i + 1}"] = dict(m=mul(rot(25 * jit(t, 20 + i, 15), x, y), scale_about(0.8 + 0.4 * d, 0.8 + 0.4 * d, x, y)))
        else:
            S[f"s{i + 1}"] = dict(hide=True)
    return S


def render(node, state=None, ind=""):
    st = (state or {}).get(node.id, {}) if node.id else {}
    if st.get("hide"):
        return ""
    a = dict(node.a)
    if node.id:
        a = {"id": node.id, **a}
    for key in ("fill", "stroke"):
        if key in st:
            a[key] = st[key] if st[key].startswith("#") else C[st[key]]
    if "m" in st:
        a["transform"] = "matrix(" + " ".join(f"{v:.4f}" for v in st["m"]) + ")"
    attrs = " ".join(f'{k}="{v}"' for k, v in a.items())
    if not node.ch:
        return f"{ind}<{node.tag} {attrs}/>\n"
    inner = "".join(render(c, state, ind + "  ") for c in node.ch)
    return f"{ind}<{node.tag} {attrs}>\n{inner}{ind}</{node.tag}>\n"


def count(node, S=None):
    if S and node.id and S.get(node.id, {}).get("hide"):
        return 0
    n = 0 if node.tag in ("svg", "g") else 1
    return n + sum(count(c, S) for c in node.ch)


class Overlay:
    def __init__(self):
        self.e, self.aa = [], 0

    def poly(self, P, col, w=2.0):
        self.aa += len(P) - 1
        self.e.append(f'<polyline points="{pts(P)}" fill="none" stroke="{C[col]}" stroke-width="{w}" '
                      f'stroke-linecap="square" stroke-linejoin="round"/>')


def load_font():
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "slot", "font.h")).read()
    grab = lambda nm: [int(v, 0) for v in re.search(nm + r"\[\d+\] = \{(.*?)\}", src, re.S).group(1).replace("\n", "").split(",") if v.strip()]
    return grab("font_off"), grab("font_seg")


FONT = load_font()


def overlay(k, t):
    d, g, c, m = [v / 32767 for v in k]
    v = 0.15 + 0.85 * m
    col = "white" if v >= 0.6 else ("purplegrey" if v >= 0.35 else "grey")
    word = mode(k)
    step = 1.8
    x, y = 160 - (6 * len(word) - 2) * step / 2, 64.5 - 3 * step
    M = mul(tr(DX, 0), pedal_matrix(d, t))
    off, seg = FONT
    o = Overlay()
    for ch in word:
        i = off[ord(ch) - 0x20]
        while seg[i] != 0xFF:
            a_, b_ = seg[i], seg[i + 1]
            o.poly([apply(M, (x + (a_ >> 4) * step, y + (a_ & 15) * step)), apply(M, (x + (b_ >> 4) * step, y + (b_ & 15) * step))], col, 1.5)
            i += 2
        x += 6 * step
    return o


def composite(name, k, t=0.3, png=True, w=960):
    S = runtime_state(k, t)
    o = overlay(k, t)
    body = render(asset(), S).split(">", 1)[1].rsplit("</svg>", 1)[0]
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160">\n'
           f'<rect width="320" height="160" fill="#000000"/>\n{body}\n<g id="overlay">\n' + "\n".join(o.e) +
           f'\n</g>\n<!-- hidden panel area y > 140 -->\n<rect x="0" y="141" width="320" height="19" fill="#000000" opacity="0.55"/>\n</svg>\n')
    p = os.path.join(OUT, name + ".svg")
    open(p, "w").write(svg)
    if png:
        subprocess.run([RSVG, "-w", str(w), p, "-o", os.path.join(OUT, name + ".png")], check=True)
    return count(asset(), S), o.aa


CV = [round(32767 * i / 5) for i in range(6)]
STATES = [
    ("idle", (0, 16384, CV[0], 0), "idle: TUBE, drive 0 (still), wah level, mix 0: both pedals dark ghosts, knobs and green axle still readable"),
    ("tube", (12000, 16384, CV[0], 32767), "TUBE (light orange): drive 37 %, wah level (flat), full wet: light shake"),
    ("muff", (20000, 16384, CV[1], 32767), "MUFF (silver box, red plaque rim): drive 61 %, wah level"),
    ("fuzz", (26000, 8000, CV[2], 32767), "FUZZ (purple): drive 79 %, wah half heel-down (low-pass), strong shake, first sparks"),
    ("wah_heel", (14000, 0, CV[3], 32767), "CLIP, drive 43 %, green full LEFT: wah heel down (resonant low-pass)"),
    ("wah_level", (14000, 16384, CV[3], 32767), "CLIP, drive 43 %, green MIDDLE: wah treadle level (flat)"),
    ("wah_toe", (14000, 32767, CV[3], 32767), "CLIP, drive 43 %, green full RIGHT: wah toe down (focus 4 kHz)"),
    ("clip_max", (32767, 24000, CV[3], 32767), "CLIP at max drive, wah half toe: violent shake and rattle, all sparks"),
    ("fold", (22000, 28000, CV[4], 26000), "FOLD (dark teal): drive 67 %, wah toe 70 %, mix 79 %"),
    ("crush", (32767, 6000, CV[5], 32767), "CRUSH (purple-grey): max drive, wah heel 58 %"),
    ("mix0", (20000, 30000, CV[3], 0), "mix 0 (dry): both pedals faded to ghosts while drive still shakes the box; knobs stay readable"),
]


def gif(name, frames):
    subprocess.run([MAGICK, "-delay", "6", "-loop", "0"] + frames + [os.path.join(OUT, name)], check=True)


if __name__ == "__main__":
    a = asset()
    open(os.path.join(OUT, "pedals.svg"), "w").write('<?xml version="1.0" encoding="utf-8"?>\n<!--' + HEADER.replace("--", "-") + '-->\n' + render(a))
    info = {name: composite(f"hq_{name}", k) for name, k, cap in STATES}
    fr = os.path.join(OUT, "frames")
    os.makedirs(fr, exist_ok=True)
    for fn in os.listdir(fr):
        os.remove(os.path.join(fr, fn))

    def run(prefix, ks):
        seq = []
        for j, k in enumerate(ks):
            composite(f"frames/{prefix}_{j:02d}", k, t=j * 0.06, w=640)
            seq.append(os.path.join(fr, f"{prefix}_{j:02d}.png"))
        return seq

    gif("pedal.gif", run("f", [(int(32767 * min(1, j / 40)), 16384, CV[3], 32767) for j in range(48)]))
    gif("pedal_modes.gif", run("m", [(20000, 16384, CV[j // 10], 32767) for j in range(60)]))
    gif("pedal_mix.gif", run("x", [(15000, 16384, CV[3], int(32767 * min(1, j / 32))) for j in range(40)]))
    gif("pedal_wah.gif", run("w", [(13000, int(32767 * (0.5 - 0.5 * math.cos(2 * math.pi * j / 48))), CV[3], 32767) for j in range(48)]))
    vis = [v[0] for v in info.values()]
    aa = [v[1] for v in info.values()]
    idx = [dict(concept="hq", title="Two Pedals (HQ hybrid)",
                summary="A wah patched into a compact distortion box, input from the right, output to the left (pedalboard convention: the wah first, then the box), no signals: green rocks the wah's treadle, the distortion box shows the mode word and colour, shakes harder with drive, and mix fades both pedals in from dark ghosts.",
                route="mixed",
                knobs=dict(blue="distortion box: whole box shakes and tilts (still at 0, ~3 px / 1.8 deg at max, 8 -> 24 Hz), foot-plate rattles on top, LED rays grow, sparks from 72 %; blue knob pointer",
                           green="rocks the wah's treadle: heel down = low-pass, level = flat, toe down = focus (up to 9 deg, three-quarter view, green rubber ribs and green axle); the green knob pointer on the box stays as a small indicator",
                           white="distortion mode: TUBE MUFF FUZZ CLIP FOLD CRUSH as a word (stroke font) in the plaque + body colour (lightorange, lpg with red rim, purple, orange, darkgreen, purplegrey); white knob pointer",
                           orange="fades both pedals: 28 elements recoloured to lerp(black, colour, 0.15 + 0.85 mix); LED off at 0; word white / purplegrey / grey; knobs and green axle stay full colour"),
                animation="frame clock: shake jitter (rate rises with drive), plate rattle, sparks blink, LED rays flicker; the treadle follows green directly",
                cost=f"SVG: {count(a)} elements in the asset ({min(vis)}-{max(vis)} visible), ~10 matrices + up to 32 recolours per frame. Overlay: {min(aa)}-{max(aa)} AA segments (the mode word only)",
                states=[dict(file=f"hq/hq_{n}.svg", png=f"hq/hq_{n}.png", caption=c) for n, _, c in STATES]
                + [dict(file="hq/pedal.gif", caption="drive sweep 0 -> max (CLIP, wah level, full wet): the shake grows from still to violent"),
                   dict(file="hq/pedal_modes.gif", caption="mode tour at drive 61 %: TUBE, MUFF, FUZZ, CLIP, FOLD, CRUSH"),
                   dict(file="hq/pedal_mix.gif", caption="mix 0 -> max (CLIP, drive 46 %): both pedals fade in from dark ghosts"),
                   dict(file="hq/pedal_wah.gif", caption="green left -> right -> left (CLIP, drive 40 %): the wah rocks heel down -> level -> toe down")])]
    json.dump(idx, open(os.path.join(OUT, "index.json"), "w"), indent=2)
    print(info, count(a))
