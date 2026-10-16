#!/usr/bin/env python3
"""pingpong.svg, the "pingpong" FX screen (table tennis: two paddles rally over the table), for TE's retained
renderer (notes/svg-route.md). The generator is the source: run it, never edit the SVG by hand.

  python3 design/fx/gen_pingpong.py

Writes design/fx/pingpong.svg with every element one subpath of absolute M / L / C / Z (strictify.py as a safety
net), the recoloured fills and strokes unique (uniquify.py), and checks it (check_svg.py, svg_cells.py --check when
firmware/work/246's colors.svg is there). ui.c draw_tennis drives the ids in the header; preview it through the C
path with src/slot/test/preview.py (FX 9: 109 master, 209 synth)."""
import math
import os
import subprocess
import sys

import strictify

OUT = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(OUT, "pingpong.svg")

C = dict(green="#00ED95", lightgreen="#80FFAF", blue="#698EFF", lightblue="#BFD8FF", white="#FFFFFF",
         orange="#FF423F", lightorange="#FFB3B3", grey="#7C7990", darkblue="#383572", purplegrey="#AEB1DC",
         lpg="#E6E7FF", purple="#5F4270", offgreen="#002826", darkgreen="#00475B", offblue="#1D2040",
         offgrey="#353238", offred="#4E2832", red="#FF3A5D", pink="#FF90A7", black="#010000")   # TE's colors.svg

CX = 160          # table centre
TY = 100          # table top surface
FLOOR = 132       # floor line
HREF = 104        # table half-length the asset is drawn at (= blue's longest); the runtime scales / translates from it
HMIN = 40         # half-length at the shortest delay
BAT_Y = 70        # where the ball meets the paddles (blade centre height)
BS = 1.15         # blade scale (rim, sheen, throat flare and shadow with it; the handle stays as it is)
BLADE_W = 12.5 * BS     # blade half-width
BLADE_TOP = 16.5 * BS   # blade extent from its centre away from the handle (rounded) ...
BLADE_BASE = 12.5 * BS  # ... and toward it (flatter: a superellipse half, where the throat meets it)
TILT = 19         # the paddle leans toward the net, degrees from upright (left paddle; mirrored on the right)
HANDLE_LEN = 28.5  # from the blade's base to the handle's flared end
GRIP_END = BLADE_BASE + HANDLE_LEN  # pivot: the handle's end, this far from the blade centre along the handle
BALL_R = 4.6
CONTACT_X = CX - HREF - 2   # where the ball's centre is at a hit (ui.c xl = 158 - half), at HREF
NPIP = 11         # scoreboard: game to 11
PADDLES = {"L": dict(face="blue", rim="darkblue", sheen="lightblue"),
           "R": dict(face="pink", rim="purple", sheen="lightorange")}
RECOLOURED = ["ballbody:fill"] + [f"pip{j + 1}:{a}" for j in range(NPIP) for a in ("fill", "stroke")]


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


class N:
    def __init__(self, tag, id=None, children=None, **a):
        self.tag, self.id, self.ch, self.a = tag, id, children or [], a


def A(**kw):        # attribute helper: stroke_width -> stroke-width
    return {k.replace("_", "-"): v for k, v in kw.items()}


def ellipse_d(cx, cy, rx, ry):
    k = 0.5523
    p = [(rx, 0), (rx, k * ry), (k * rx, ry), (0, ry), (-k * rx, ry), (-rx, k * ry), (-rx, 0), (-rx, -k * ry),
         (-k * rx, -ry), (0, -ry), (k * rx, -ry), (rx, -k * ry), (rx, 0)]
    s = f"M{f(cx + p[0][0])} {f(cy + p[0][1])}"
    for i in range(1, 13, 3):
        s += " C" + " ".join(f"{f(cx + x)} {f(cy + y)}" for x, y in p[i:i + 3])
    return s + " Z"


def arc_d(cx, cy, r, a0, a1):
    """An open circular arc a0 -> a1 (degrees, y down), one cubic per <= 90 deg."""
    n = max(1, math.ceil(abs(a1 - a0) / 90))
    s = ""
    for i in range(n):
        t0, t1 = math.radians(a0 + (a1 - a0) * i / n), math.radians(a0 + (a1 - a0) * (i + 1) / n)
        k = 4 / 3 * math.tan((t1 - t0) / 4) * r
        p0 = (cx + r * math.cos(t0), cy + r * math.sin(t0))
        p3 = (cx + r * math.cos(t1), cy + r * math.sin(t1))
        p1 = (p0[0] - k * math.sin(t0), p0[1] + k * math.cos(t0))
        p2 = (p3[0] + k * math.sin(t1), p3[1] - k * math.cos(t1))
        if not i:
            s = f"M{f(p0[0])} {f(p0[1])}"
        s += " C" + " ".join(f"{f(x)} {f(y)}" for x, y in (p1, p2, p3))
    return s


def P(*pts, close=False):
    return " ".join(("M" if i == 0 else "L") + f"{f(p[0])} {f(p[1])}" for i, p in enumerate(pts)) + (" Z" if close else "")


# ------------------------------------------------------------------ artwork
def arena():
    d = " ".join(P((x - 0.5, 104), (x + 0.5, 104), (x + 0.5, 124), (x - 0.5, 124), close=True)
                 for x in (56, 108, 160, 212, 264))
    return N("g", "arena", [
        N("rect", "barrier", x="4", y="104", width="312", height="20", fill=C["offblue"]),
        N("path", "barrier_seams", d=d, fill=C["darkblue"]),          # five bars: strictify bridges them into one fill
        N("line", "floor", x1="4", y1=f(FLOOR), x2="316", y2=f(FLOOR), **A(stroke=C["purplegrey"], stroke_width="1.5")),
        N("line", "lip", x1="4", y1=f(FLOOR + 3), x2="316", y2=f(FLOOR + 3), **A(stroke=C["darkblue"], stroke_width="1")),
    ])


def table():
    x0, x1 = CX - HREF, CX + HREF
    top = [N("rect", "slab", x=f(x0), y=f(TY), width=f(2 * HREF), height="6", fill=C["darkgreen"]),
           N("line", "edge", x1=f(x0), y1=f(TY), x2=f(x1), y2=f(TY), **A(stroke=C["lpg"], stroke_width="1.5")),
           N("line", "underside", x1=f(x0 + 4), y1=f(TY + 7.5), x2=f(x1 - 4), y2=f(TY + 7.5),
             **A(stroke=C["offgreen"], stroke_width="1.5"))]

    def leg(side):
        x = CX + side * (HREF - 14)
        s = "L" if side < 0 else "R"
        return N("g", f"leg{s}", [N("path", f"leg{s}d", d=P((x - side * 6, TY + 7), (x, FLOOR), (x + side * 2, FLOOR),
                                                            (x - side * 3, TY + 7), close=True), fill=C["grey"])])
    mesh = " ".join(P((CX - 2.5, TY - 10.5 + 2.5 * i), (CX + 2.5, TY - 10.5 + 2.5 * i), (CX + 2.5, TY - 9.5 + 2.5 * i),
                      (CX - 2.5, TY - 9.5 + 2.5 * i), close=True) for i in range(4))
    net = [N("line", "netpost", x1=f(CX), y1=f(TY - 12), x2=f(CX), y2=f(TY), **A(stroke=C["lpg"], stroke_width="1.5")),
           N("path", "netmesh", d=mesh, fill=C["grey"]),
           N("line", "nettape", x1=f(CX - 3), y1=f(TY - 11.5), x2=f(CX + 3), y2=f(TY - 11.5),
             **A(stroke=C["white"], stroke_width="2", stroke_linecap="round"))]
    return N("g", "table", [N("g", "tabletop", top), leg(-1), leg(1), N("g", "net", net)])


def wall():
    """squash front wall (mono): replaces the right paddle at the table end; bricks as single strokes"""
    x = CX + HREF
    bricks = []
    for r in range(9):
        y = 40 + r * 10
        bricks.append(N("line", f"brick{2 * r + 1}", x1=f(x), y1=f(y), x2=f(x + 22), y2=f(y),
                        **A(stroke=C["purple"], stroke_width="1")))
        bx = x + (8 if r % 2 else 15)
        bricks.append(N("line", f"brick{2 * r + 2}", x1=f(bx), y1=f(y), x2=f(bx), y2=f(y + 10),
                        **A(stroke=C["purple"], stroke_width="1")))
    return N("g", "wall", [
        N("rect", "wallface", x=f(x), y="30", width="22", height=f(FLOOR - 30), fill=C["offgrey"]),
        N("g", "bricks", bricks),
        N("line", "wallfront", x1=f(x), y1="30", x2=f(x), y2=f(FLOOR), **A(stroke=C["purplegrey"], stroke_width="2")),
        N("line", "tin", x1=f(x - 1), y1=f(FLOOR - 16), x2=f(x + 22), y2=f(FLOOR - 16), **A(stroke=C["lightblue"], stroke_width="2")),
        N("line", "courtline", x1=f(CX - HREF - 40), y1=f(FLOOR - 1.5), x2=f(x), y2=f(FLOOR - 1.5),
          **A(stroke=C["darkblue"], stroke_width="1.5")),
    ])


def blade_pts(w, top, base, n=40):
    """The blade outline in its own frame (v across, h along: + toward the top), shortest first: an ellipse on the
    top half, a flatter superellipse (exponent 3) on the base half."""
    pts = []
    for i in range(n):
        t = 2 * math.pi * i / n
        c, sn = math.cos(t), math.sin(t)
        if sn >= 0:
            pts.append((w * c, top * sn))
        else:
            e = 2 / 3
            pts.append((w * math.copysign(abs(c) ** e, c), -base * abs(sn) ** e))
    return pts


def paddle_geom(side):
    """Blade centre, the unit vector from it down the handle, and a point mapper, for one paddle at HREF. The blade
    centre is placed so the ball (r BALL_R) at CONTACT_X, BAT_Y just touches the blade's face."""
    a = math.radians(TILT) * -side
    ux, uy = -math.sin(a), math.cos(a)          # down the handle (left paddle: down and to the left)
    hx, hy = -ux, -uy                           # toward the blade top
    vx, vy = -hy, hx

    def mapper(bx, by):
        return lambda v, h: (bx + v * vx + h * hx, by + v * vy + h * hy)
    m0 = mapper(0, BAT_Y)
    ext = max(side * -1 * x for x, y in (m0(v, h) for v, h in blade_pts(BLADE_W, BLADE_TOP, BLADE_BASE))
              if abs(y - BAT_Y) < 3)            # the face's reach toward the net at the contact height
    cx = 320 - CONTACT_X if side > 0 else CONTACT_X
    bx = cx + side * (BALL_R + ext - 0.4)       # 0.4 overlap: touching
    pv = (bx + GRIP_END * ux, BAT_Y + GRIP_END * uy)
    return (bx, BAT_Y), (ux, uy), mapper(bx, BAT_Y), (round(pv[0]), round(pv[1]))


def paddle(side):
    """A flat, face-on table-tennis bat: an elongated rounded blade (taller than wide, flatter at the base) with its
    rim showing as the thickness and a sheen, a throat flaring into the blade, a long handle with a flared end, a
    floor shadow. Group pad<S> moves with the table end; inside it bat<S> turns about the handle's end (the pivot)."""
    s = "L" if side < 0 else "R"
    c = PADDLES[s]
    (bx, by), (ux, uy), at, pv = paddle_geom(side)
    # at(v, h): v across, h toward the blade top; the handle runs to h = -GRIP_END
    hand = [(-2.2, -BLADE_BASE + 1), (-2.3, -BLADE_BASE - 6), (-2.9, -GRIP_END + 9), (-3.4, -GRIP_END + 3),
            (-4.6, -GRIP_END + 0.6)]                                # one side, blade to the flared end
    right = [at(v, h) for v, h in hand]
    left = [at(-v, h) for v, h in reversed(hand)]
    e0, e1 = at(-4.6, -GRIP_END - 1.6), at(4.6, -GRIP_END - 1.6)  # the end, slightly domed
    handle = P(*right) + f" C{f(e0[0])} {f(e0[1])} {f(e1[0])} {f(e1[1])} {f(left[0][0])} {f(left[0][1])} " + \
        " ".join(f"L{f(x)} {f(y)}" for x, y in left[1:]) + " Z"
    # the throat: a wedge flaring from the handle into the blade's base (lighter, over the rubber)
    th = [at(-2.3, -BLADE_BASE - 4), at(-6.5 * BS, -BLADE_BASE + 2.2 * BS), at(-3.5 * BS, -BLADE_BASE + 4.5 * BS),
          at(0, -BLADE_BASE + 5.2 * BS), at(3.5 * BS, -BLADE_BASE + 4.5 * BS), at(6.5 * BS, -BLADE_BASE + 2.2 * BS),
          at(2.3, -BLADE_BASE - 4)]
    grips = [(at(-3.1, h), at(3.1, h)) for h in (-GRIP_END + 13, -GRIP_END + 7)]
    outline = blade_pts(BLADE_W, BLADE_TOP, BLADE_BASE)
    off = (-side * 1.4 * BS, 1.2 * BS)                                       # the rim: the blade's edge, seen as its thickness
    rim = [at(v, h) for v, h in outline]
    # sheen: an arc along the inner outline, upper outer quarter (away from the net)
    inner = blade_pts(BLADE_W - 3.4 * BS, BLADE_TOP - 3.4 * BS, BLADE_BASE - 3.4 * BS, 48)
    idx = range(14, 21) if side < 0 else range(4, 11)              # 105 .. 150 deg (left) / 30 .. 75 deg (right)
    sh = [at(*inner[i]) for i in idx]
    bat = N("g", f"bat{s}", [
        N("path", f"handle{s}", d=handle, fill=C["purplegrey"]),
        *[N("line", f"grip{s}{j + 1}", x1=f(p0[0]), y1=f(p0[1]), x2=f(p1[0]), y2=f(p1[1]),
            **A(stroke=C["grey"], stroke_width="1.1")) for j, (p0, p1) in enumerate(grips)],
        N("path", f"rim{s}", d=P(*[(x + off[0], y + off[1]) for x, y in rim], close=True), fill=C[c["rim"]]),
        N("path", f"blade{s}", d=P(*rim, close=True), fill=C[c["face"]]),
        N("path", f"throat{s}", d=P(*th, close=True), fill=C["lpg"]),
        N("path", f"sheen{s}", d=P(*sh), fill="none",
          **A(stroke=C[c["sheen"]], stroke_width="2", stroke_linecap="round", stroke_linejoin="round")),
    ])
    shadow = N("path", f"padshadow{s}", d=ellipse_d(bx - side * 6 * BS, FLOOR - 2.5, 11 * BS, 1.8), fill=C["darkblue"])
    return N("g", f"pad{s}", [shadow, bat])


def sparkle_d(r):
    q = r * 0.26
    return P((0, -r), (q, -q), (r, 0), (q, q), (0, r), (-q, q), (-r, 0), (-q, -q), close=True)


def ball():
    """drawn at the origin; the runtime translates the 'ball' group to the live position"""
    return N("g", "ball", [
        N("g", "glow", [N("path", "glowstar", d=sparkle_d(11), fill=C["white"],
                          **A(stroke=C["white"], stroke_width="0.8", stroke_linejoin="round"))]),
        N("circle", "ballbody", cx="0", cy="0", r="4.6", fill=C["white"], **A(stroke=C["white"], stroke_width="1.2")),
        N("path", "ballshade", d="M-3.6 1.6 C-2 4.6 2.4 4.8 4.2 1.6 C2.6 2.4 -1.6 2.6 -3.6 1.6 Z", fill=C["grey"],
          **A(stroke=C["grey"], stroke_width="0.8")),
        N("circle", "ballglint", cx="-1.5", cy="-1.6", r="1.1", fill=C["lpg"], **A(stroke=C["lpg"], stroke_width="0.5")),
    ])


def shadow():
    return N("g", "shadow", [N("path", "shadowd", d=ellipse_d(0, 0, 5, 1.3), fill=C["offgreen"],
                               **A(stroke=C["offgreen"], stroke_width="0.5"))])


def scoreboard():
    w = NPIP * 9 + 8
    x0 = CX - w / 2
    kids = [N("line", "wire1", x1=f(x0 + 8), y1="2", x2=f(x0 + 8), y2="6", **A(stroke=C["grey"], stroke_width="1")),
            N("line", "wire2", x1=f(x0 + w - 8), y1="2", x2=f(x0 + w - 8), y2="6", **A(stroke=C["grey"], stroke_width="1")),
            N("rect", "board", x=f(x0), y="6", width=f(w), height="13", fill=C["offgrey"], **A(stroke=C["grey"], stroke_width="1"))]
    for j in range(NPIP):                      # pip 1 = rightmost: wet pips light from the right
        x = x0 + w - 8.5 - j * 9
        kids.append(N("circle", f"pip{j + 1}", cx=f(x), cy="12.5", r="3", fill=C["offred"], **A(stroke=C["offred"], stroke_width="1")))
    return N("g", "score", kids)


def asset():
    kids = [arena(), table(), wall(), shadow(), paddle(-1), paddle(1), scoreboard(), ball()]
    return N("svg", None, kids, version="1.1", xmlns="http://www.w3.org/2000/svg", x="0", y="0", width="320", height="160",
             viewBox="0 0 320 160")


PIV_L, PIV_R = paddle_geom(-1)[3], paddle_geom(1)[3]
HEADER = f"""
pingpong.svg - "pingpong" FX screen (table tennis: two paddles, L and R, rally over the table). Our own artwork,
generated by design/fx/gen_pingpong.py (do not edit by hand). TE's retained renderer draws it, then our immediate
overlay (echo arches, swoosh, the time readout) goes on top. All ids are found once at load (find, assert=0).
Reference geometry: table centre x = {CX}, top y = {TY}, drawn at half-length HREF = {HREF}; blades {f(2 * BLADE_W)} x
{f(BLADE_TOP + BLADE_BASE)} leaning {TILT} deg toward the net, the ball meets them at x = {CONTACT_X} / {320 - CONTACT_X}, y = {BAT_Y}; handle pivots L {PIV_L}, R {PIV_R}; floor y = {FLOOR}.
Knobs: half = {HMIN} + ({HREF} - {HMIN}) * blue's table position (ui.c); g = feedback; tone = white; mix = orange.
  tabletop          matrix = scale(half/{HREF}, 1) about ({CX}, {TY})                    [blue: table length = delay time]
  legL / legR       matrix = translate(+-({HREF} - half), 0)
  padL / padR       matrix = translate(+-({HREF} - half), 0): the paddles stay at the table ends (and their floor shadows)
  batL / batR       matrix = rotate(a) about the pivot (handle end): the hitter follows through (+ toward the net)
                    after its hit and settles; the receiver cocks back as the ball arrives (mirrored for R)
  ball              matrix = translate(x, y) along the newest arch, blade face to blade face, once per flight
                    period T = d * ceil(0.25 s / d) (d = the delay the DSP plays), alternating L->R, R->L
  ballbody          recolour fill: dark tone grey, slightly dark purplegrey, else white            [white: tone]
  ballshade         shown when tone < -0.1 (dull, heavy ball)
  ballglint         shown when tone > -0.1
  glow              shown when tone > 0.1; matrix = rotate(t * 360 deg/s) * scale(0.35 + 0.65 * tone) about the ball
  shadow            matrix = translate(ball x, {TY}) * scale(1 - h, 1), smaller as the ball flies up (squash: floor)
  pip1..pip{NPIP}       recolour fill+stroke: round({NPIP} * mix) pips orange from the right, the rest offred  [orange: mix]
  table, padR       hidden for synth/drum FX (mono = squash: one paddle against the wall)
  wall              shown only for synth/drum FX; matrix = translate(-({HREF} - half), 0)
  everything else   static (arena barrier, floor, net, scoreboard board + wires).
Overlay (immediate mode, green = feedback): echo arches blade to blade (squash: blade to wall), one per echo above
  -28 dB (max 8), peak 6 + 36 g^k above the blades; a lightblue swoosh at the hitter for 0.12 s; top left the time in
  blue ("1/8.", "1/8T", "1/4" synced; "330MS" free), top right the tempo in grey when synced ("120BPM").
"""


def render(node, ind=""):
    a = dict(node.a)
    if node.id:
        a = {"id": node.id, **a}
    attrs = " ".join(f'{k}="{v}"' for k, v in a.items())
    if not node.ch:
        return f"{ind}<{node.tag} {attrs}/>\n"
    return f"{ind}<{node.tag} {attrs}>\n" + "".join(render(c, ind + "  ") for c in node.ch) + f"{ind}</{node.tag}>\n"


def main():
    xml = render(asset())
    open(SVG, "w").write('<?xml version="1.0" encoding="utf-8"?>\n<!--' + HEADER.replace("--", "-") + "-->\n" + xml)
    log = strictify.strictify_file(SVG)                                  # one subpath per element
    print("\n".join(log))
    py = sys.executable
    subprocess.run([py, os.path.join(OUT, "uniquify.py"), SVG] + RECOLOURED, check=True)
    colors = os.path.join(OUT, "..", "..", "firmware", "work", "246", "content", "display", "colors.svg")
    if os.path.exists(colors):
        subprocess.run([py, os.path.join(OUT, "check_svg.py"), SVG, colors], check=True)
    subprocess.run([py, os.path.join(OUT, "svg_cells.py"), "--check", SVG], check=True)


if __name__ == "__main__":
    main()
