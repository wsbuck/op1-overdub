#!/usr/bin/env python3
"""Generate design/fx/space.svg: the `space` reverb FX screen as lunar ridges (design: William Buck; drawn in his
op1-svgs folder as gen_space_moon.py version B, "RIDGES", and brought here unchanged but for the output path, this
credit and version A left out; drawn by src/slot/ui_space.h). A radio telescope looks out over ridge after ridge,
Earth in the sky.
      blue   SPACE  the ridges: one near wall (ROOM) to far low ranges (CATHEDRAL); VOID flattens to an endless plain
      green  DECAY  a rover's track running off to the horizon; FREEZE plants a flag
      white  TAIL   Earth's phase: TONE dark (new Earth) to bright (full); SHIMMER = a pillar of light rising
      orange MIX    radio waves from the dish's feed; ducking shrinks them

  python3 design/fx/gen_space_moon.py      # writes design/fx/space.svg

b_state() is the header's runtime rules as the designer wrote them and states() his preview states;
src/slot/test/space_screen_check.py imports both as the reference for the C path.
"""
import math
import os
import random

BLUE, GREEN, WHITE, ORANGE = "#698EFF", "#00ED95", "#FFFFFF", "#FF423F"
LBLUE, LGREEN, LORANGE = "#BFD8FF", "#80FFAF", "#FFB3B3"
GREY, PG, LPG = "#7C7990", "#AEB1DC", "#E6E7FF"
DARKBLUE, OFFBLUE, PURPLE = "#383572", "#1D2040", "#5F4270"
BLACK = "#010000"

HERE = os.path.dirname(os.path.abspath(__file__))
READOUT = (150, 20)                 # keep x < 150 and y < 20 empty


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def pts(ps):
    return " ".join(f"{f(x)} {f(y)}" for x, y in ps)


def P(a, r, c):
    """Compass angle a (0 = up, clockwise), radius r about c."""
    t = math.radians(a)
    return c[0] + r * math.sin(t), c[1] - r * math.cos(t)


def ell_arc(cx, cy, rx, ry, t0, t1):
    """An elliptical arc as one path, parameter angles in degrees (0 = right, 90 = down), cubics of <= 45 deg."""
    n = max(1, math.ceil(abs(t1 - t0) / 45))
    step = (t1 - t0) / n
    k = 4 / 3 * math.tan(math.radians(step) / 4)
    pt = lambda t: (cx + rx * math.cos(math.radians(t)), cy + ry * math.sin(math.radians(t)))
    dv = lambda t: (-rx * math.sin(math.radians(t)), ry * math.cos(math.radians(t)))
    x, y = pt(t0)
    d = f"M{f(x)} {f(y)}"
    for i in range(n):
        s, e = t0 + i * step, t0 + (i + 1) * step
        (sx, sy), (ex, ey), (dsx, dsy), (dex, dey) = pt(s), pt(e), dv(s), dv(e)
        d += f" C{f(sx + k * dsx)} {f(sy + k * dsy)} {f(ex - k * dex)} {f(ey - k * dey)} {f(ex)} {f(ey)}"
    return d


class SVG:
    def __init__(self):
        self.out, self.depth = [], 0

    def emit(self, s):
        self.out.append("  " * self.depth + s)

    @staticmethod
    def attrs(stroke=None, fill="none", w=1.5, cap="square", join=None):
        a = [f'fill="{fill}"']
        if stroke:
            a += [f'stroke="{stroke}"', f'stroke-width="{f(w)}"', f'stroke-linecap="{cap}"']
            if join:
                a.append(f'stroke-linejoin="{join}"')
        return " ".join(a)

    def g(self, i):
        self.emit(f'<g id="{i}">')
        self.depth += 1

    def end(self):
        self.depth -= 1
        self.emit("</g>")

    def path(self, i, d, **kw):
        self.emit(f'<path id="{i}" d="{d}" {self.attrs(**kw)}/>')

    def poly(self, i, ps, close=True, **kw):
        tag = "polygon" if close else "polyline"
        self.emit(f'<{tag} id="{i}" points="{pts(ps)}" {self.attrs(**kw)}/>')

    def line(self, i, a, b, **kw):
        self.emit(f'<line id="{i}" x1="{f(a[0])}" y1="{f(a[1])}" x2="{f(b[0])}" y2="{f(b[1])}" {self.attrs(**kw)}/>')

    def circle(self, i, c, r, **kw):
        self.emit(f'<circle id="{i}" cx="{f(c[0])}" cy="{f(c[1])}" r="{f(r)}" {self.attrs(**kw)}/>')

    def ellipse(self, i, c, rx, ry, **kw):
        self.emit(f'<ellipse id="{i}" cx="{f(c[0])}" cy="{f(c[1])}" rx="{f(rx)}" ry="{f(ry)}" {self.attrs(**kw)}/>')

    def save(self, name, header):
        text = ['<?xml version="1.0" encoding="utf-8"?>', header,
                '<svg version="1.1" xmlns="http://www.w3.org/2000/svg" x="0" y="0" width="320" height="160" '
                'viewBox="0 0 320 160">'] + self.out + ["</svg>"]
        with open(os.path.join(HERE, name), "w") as fh:
            fh.write("\n".join(text) + "\n")


def star_field(seed, n, avoid, y_max):
    """n star positions, not in the readout corner, not in the boxes in `avoid`, above y_max."""
    rnd = random.Random(seed)
    ps = []
    while len(ps) < n:
        x, y = rnd.uniform(6, 314), rnd.uniform(5, y_max)
        if x < READOUT[0] + 4 and y < READOUT[1] + 4:
            continue
        if any(x0 <= x <= x1 and y0 <= y <= y1 for x0, y0, x1, y1 in avoid):
            continue
        if any(abs(x - px) < 9 and abs(y - py) < 7 for px, py in ps):
            continue
        ps.append((x, y))
    return ps


def sparkle(s, i, c, r, color, w=1):
    """A four-point star: two crossing strokes."""
    s.g(i)
    s.line(i + "_h", (c[0] - r, c[1]), (c[0] + r, c[1]), stroke=color, w=w)
    s.line(i + "_v", (c[0], c[1] - r), (c[0], c[1] + r), stroke=color, w=w)
    s.end()


def diamond(s, i, c, r, color):
    x, y = c
    s.poly(i, [(x, y - r), (x + r * 0.6, y), (x, y + r), (x - r * 0.6, y)], fill=color)


def rot(a, cx, cy):
    t = math.radians(a)
    co, si = math.cos(t), math.sin(t)
    return f"matrix({co:.4f} {si:.4f} {-si:.4f} {co:.4f} {cx - co * cx + si * cy:.4f} {cy - si * cx - co * cy:.4f})"


def scale(sx, sy, cx, cy):
    return f"matrix({sx:.4f} 0 0 {sy:.4f} {cx - sx * cx:.4f} {cy - sy * cy:.4f})"


SPACES = ("ROOM", "HALL", "CHURCH", "CATHEDRAL", "VOID")
ZONES = ("tone", "shim12", "shim1219", "sub", "gate", "swell")


def common(space, mix, master, duck):
    """Shared readings of the signals: the stage (0..4), the wet amount, the duck depth and the dry level."""
    stage = min(4, int(round(space)))
    wet = min(1, mix * 2)                         # dry -> an even blend at the middle
    over = max(0, mix * 2 - 1)                    # above the middle
    depth = over if master else 0                 # the ducking depth (-12 .. -24 dB)
    dry = 1 - over if not master else 1           # as a synth/drum FX the dry sound fades out
    return stage, wet, depth, dry, duck if master else 0


# =================================================================================================================
# Version B: RIDGES
# =================================================================================================================

B_HORIZON = 98
B_VP = (204, B_HORIZON)             # the vanishing point on the horizon
B_EARTH = (262, 40)
B_R_EARTH = 16
B_DISH_V = (62, 101)                # the dish's vertex; it looks up and to the right (compass 45)
B_DISH_AXIS = 45
B_ANTENNA = (76.14, 86.86)          # its feed (the focus, 20 px up the axis): the radio waves' centre
B_TRACK0 = (96, 141)                # the rover track starts here, near us
B_SHIM_X = 168


def ridge_pts(seed, y_base, height, jag, x0=-4, x1=324, step=11):
    """A jagged ridge profile above y_base, closed along the horizon."""
    rnd = random.Random(seed)
    ps = [(x0, B_HORIZON)]
    x = x0
    while x < x1:
        y = y_base - height * (0.55 + 0.45 * math.sin(x / 37 + seed)) - rnd.uniform(0, jag)
        ps.append((x, min(y, B_HORIZON - 1)))
        x += step * rnd.uniform(0.7, 1.3)
    ps += [(x1, min(ps[-1][1], B_HORIZON - 1)), (x1, B_HORIZON)]
    return ps


B_RIDGES = (            # seed, base, height, jag, step: ROOM's near wall .. CATHEDRAL's far range
    (1, B_HORIZON - 4, 46, 6, 13),
    (2, B_HORIZON - 3, 26, 5, 12),
    (3, B_HORIZON - 2, 14, 3.5, 10),
    (4, B_HORIZON - 1, 6, 2, 8),
)
B_STAGE_RIDGES = ((0,), (1,), (1, 2), (2, 3), ())   # which ridges each stage shows (back to front: draw order)


def track(t):
    """The rover track's centre line at t (0 near us .. 1 at the vanishing point), its half gauge and scale."""
    u = 1 - (1 - min(1, t)) ** 1.7
    x = B_TRACK0[0] + (B_VP[0] - B_TRACK0[0]) * u
    y = B_TRACK0[1] + (B_VP[1] + 2 - B_TRACK0[1]) * u
    sz = 1 - 0.9 * u
    return (x, y), 4.2 * sz, sz


def track_seg(s, i, t0, t1, color):
    """One stretch of the track: its two wheel lines, and a tread tick across each when it is near enough."""
    (x0, y0), g0, z0 = track(t0)
    (x1, y1), g1, _ = track(t1)
    dx, dy = B_VP[0] - B_TRACK0[0], B_VP[1] - B_TRACK0[1]
    L = math.hypot(dx, dy)
    nx, ny = -dy / L, dx / L
    w = 1.5 if z0 > 0.45 else 1
    s.g(i)
    s.line(i + "_l", (x0 + nx * g0, y0 + ny * g0), (x1 + nx * g1, y1 + ny * g1), stroke=color, w=w)
    s.line(i + "_r", (x0 - nx * g0, y0 - ny * g0), (x1 - nx * g1, y1 - ny * g1), stroke=color, w=w)
    s.end()


def dish(s):
    """The radio telescope: a dish on a yoke and mast, its feed held on two struts."""
    u = (math.sin(math.radians(B_DISH_AXIS)), -math.cos(math.radians(B_DISH_AXIS)))
    n = (-u[1], u[0])
    V = B_DISH_V
    at = lambda a, b: (V[0] + a * u[0] + b * n[0], V[1] + a * u[1] + b * n[1])
    A, B = at(10, 19), at(10, -19)
    s.line("d_yoke", at(-1, 0), (V[0] - 2, 110), stroke=PG)
    s.line("d_mast", (V[0] - 2, 110), (V[0] - 2, 124), stroke=PG)
    s.path("d_bowl", f"M{pts([A])} C{pts([at(0, 9.5), at(0, -9.5), B])} Z", stroke=LPG, fill=BLACK, join="round")
    s.path("d_inner", f"M{pts([at(7.5, 14)])} C{pts([at(2, 7), at(2, -7), at(7.5, -14)])}", stroke=GREY, w=1)
    s.line("d_strut1", at(9.5, 16.5), B_ANTENNA, stroke=PG, w=1)
    s.line("d_strut2", at(9.5, -16.5), B_ANTENNA, stroke=PG, w=1)
    s.circle("d_feed", B_ANTENNA, 1.6, stroke=LPG, fill=BLACK)
    s.circle("d_hub", at(-0.5, 0), 1.5, stroke=PG, fill=BLACK)


def hut(s, tag, col, lamp):
    """The equipment hut at the dish's foot: its lit window and lamp are the dry sound."""
    s.g("dry" + tag)
    s.poly(f"h_box{tag}", [(44, 136), (82, 136), (82, 124), (44, 124)], stroke=col, fill=BLACK)
    s.poly(f"h_roof{tag}", [(42, 124), (84, 124), (80, 120), (46, 120)], stroke=col, fill=BLACK)
    s.poly(f"h_window{tag}", [(49, 128), (57, 128), (57, 132), (49, 132)], fill=lamp)
    s.line(f"h_door{tag}", (72, 136), (72, 128), stroke=col, w=1)
    s.line(f"h_door2{tag}", (72, 128), (77, 128), stroke=col, w=1)
    s.line(f"h_door3{tag}", (77, 128), (77, 136), stroke=col, w=1)
    s.circle(f"h_lamp{tag}", (88, 132), 1.6, fill=lamp)
    s.line(f"h_post{tag}", (88, 134), (88, 136), stroke=col, w=1)
    if not tag:
        for k, a in enumerate((-50, 0, 50), 1):
            s.line(f"h_glow{k}", P(a, 3.6, (88, 132)), P(a, 5.6, (88, 132)), stroke=lamp, w=1)
    s.end()


def crescent_d(c, r, phase):
    """The lit part of the disc, lit from the right: phase 0..1 (0 = a sliver, 1 = full)."""
    cx, cy = c
    k = 0.5523
    # the outer (right) semicircle, top to bottom
    d = f"M{f(cx)} {f(cy - r)} C{f(cx + r * k)} {f(cy - r)} {f(cx + r)} {f(cy - r * k)} {f(cx + r)} {f(cy)}"
    d += f" C{f(cx + r)} {f(cy + r * k)} {f(cx + r * k)} {f(cy + r)} {f(cx)} {f(cy + r)}"
    # the terminator back up: an ellipse half, its x radius from +r (sliver) through 0 to -r (full)
    tx = r * (1 - 2 * phase)
    d += f" C{f(cx + tx * k)} {f(cy + r)} {f(cx + tx)} {f(cy + r * k)} {f(cx + tx)} {f(cy)}"
    d += f" C{f(cx + tx)} {f(cy - r * k)} {f(cx + tx * k)} {f(cy - r)} {f(cx)} {f(cy - r)} Z"
    return d


def b_build():
    s = SVG()
    # ---- sky: neutral stars ----
    avoid = [(B_EARTH[0] - 28, 10, B_EARTH[0] + 28, 66), (B_SHIM_X - 12, 20, B_SHIM_X + 12, 96)]
    for k, p in enumerate(star_field(7, 22, avoid, 60), 1):
        if k % 4 == 0:
            sparkle(s, f"star{k}", p, 2.2, GREY, w=1)
        else:
            s.circle(f"star{k}", p, 0.7 if k % 3 else 1, fill=GREY if k % 2 else PG)
    # ---- Earth (white: TAIL tone = its phase), a halo that breathes with the tail ----
    s.g("halo")
    s.circle("halo_ring", B_EARTH, B_R_EARTH + 5, stroke=WHITE, w=1)
    s.end()
    s.circle("earth", B_EARTH, B_R_EARTH, stroke=PG, fill=BLACK)
    s.g("phase")
    for k in range(1, 6):
        s.path(f"phase{k}", crescent_d(B_EARTH, B_R_EARTH - 2, (k - 0.6) / 4.4), fill=WHITE)
    s.end()
    s.path("earth_land1", f"M{B_EARTH[0] - 9} {B_EARTH[1] - 6} C{B_EARTH[0] - 4} {B_EARTH[1] - 9} {B_EARTH[0] + 1}"
           f" {B_EARTH[1] - 4} {B_EARTH[0] - 3} {B_EARTH[1] + 1}", stroke=GREY, w=1)
    s.path("earth_land2", f"M{B_EARTH[0] + 2} {B_EARTH[1] + 6} C{B_EARTH[0] + 6} {B_EARTH[1] + 4} {B_EARTH[0] + 9}"
           f" {B_EARTH[1] + 8} {B_EARTH[0] + 6} {B_EARTH[1] + 11}", stroke=GREY, w=1)
    # ---- SHIMMER (white): a pillar of light rising from the far plain ----
    s.g("shim12")
    for k in range(9):
        diamond(s, f"sh{k + 1}", (B_SHIM_X + (1.6 if k % 2 else -1.6), B_HORIZON - 12 - 8.4 * k), 2.6 - 0.1 * k,
                WHITE)
    s.end()
    s.g("shim1219")
    for k in range(8):
        side = -1 if k % 2 else 1
        s.circle(f"sx{k + 1}", (B_SHIM_X + side * (8 + 1.4 * k), B_HORIZON - 16 - 8.4 * k), 1.8, stroke=WHITE, w=1)
    s.end()
    # ---- the ridges (blue: SPACE), black-filled so they hide the sky behind; dim copies for FX off ----
    for tag, col in (("", BLUE), ("_dim", DARKBLUE)):
        s.g("ridges" + tag)
        for k in (3, 2, 1, 0):
            seed, base, h, jag, step = B_RIDGES[k]
            s.poly(f"ridge{k}{tag}", ridge_pts(seed, base, h, jag, step=step), stroke=col, fill=BLACK, join="round")
        # the ground in front: hides the ridges' bottom edge when the code shifts them down
        s.poly(f"ground{tag}", [(0, B_HORIZON + 0.8), (320, B_HORIZON + 0.8), (320, 160), (0, 160)], fill=BLACK)
        s.g("void" + tag)
        for k, x in enumerate((-140, -40, 60, 330, 430, 540), 1):
            s.line(f"vline{k}{tag}", (B_VP[0] + (x - B_VP[0]) * 0.05, B_VP[1] + 2.5), (x, 142), stroke=col, w=1)
        s.end()
        s.end()
    s.line("horizon", (0, B_HORIZON), (320, B_HORIZON), stroke=PG)
    # ground texture: small craters, neutral
    for k, (x, y, rx) in enumerate(((150, 126, 16), (262, 114, 11), (300, 134, 9)), 1):
        s.path(f"crater{k}", ell_arc(x, y, rx, rx * 0.24, 180, 360), stroke=GREY, w=1)
        s.path(f"crater{k}b", ell_arc(x, y, rx, rx * 0.24, 15, 165), stroke=PG, w=1)
    # ---- the rover track (green: DECAY) running to the horizon; INFINITE reaches it; FREEZE plants a flag ----
    s.g("prints")
    for k in range(12):
        track_seg(s, f"print{k + 1}", k / 12 * 0.86, (k + 0.62) / 12 * 0.86, GREEN)
    s.end()
    s.g("inf")
    for k in range(6):
        (x, y), g, z = track(0.88 + 0.02 * k)
        s.circle(f"inf{k + 1}", (x, y), 0.6, fill=LGREEN)
    s.circle("inf_loop1", (B_VP[0] - 2.2, B_VP[1] - 4), 2, stroke=LGREEN, w=1)
    s.circle("inf_loop2", (B_VP[0] + 2.2, B_VP[1] - 4), 2, stroke=LGREEN, w=1)
    s.end()
    s.g("flag")
    fx, fy = 130, 124
    s.line("flag_pole", (fx, fy), (fx, fy - 34), stroke=LPG)
    s.poly("flag_cloth", [(fx, fy - 34), (fx + 15, fy - 34), (fx + 15, fy - 25), (fx, fy - 25)], fill=GREEN)
    for k, a in enumerate((0, 60, 120), 1):
        s.line(f"flag_snow{k}", P(a, 2.8, (fx + 7.5, fy - 29.5)), P(a + 180, 2.8, (fx + 7.5, fy - 29.5)), stroke=BLACK,
               w=1)
    s.end()
    # ---- the radio telescope (neutral) and its hut (the dry sound: lit, or darkblue when the dry fades) ----
    dish(s)
    hut(s, "", PG, LPG)
    hut(s, "_ghost", DARKBLUE, DARKBLUE)
    # ---- radio waves (orange: MIX) from the antenna, out over the ridges ----
    s.g("waves")
    for k in range(6):
        r = 6 + 5.2 * k
        a0, a1 = 18, 82
        p0 = P(a0, r, B_ANTENNA)
        s.path(f"wave{k + 1}", "M" + pts([p0]) + arc_c(r, a0, a1, B_ANTENNA), stroke=ORANGE, w=1.5)
    s.end()
    s.g("duckdepth")
    for k in range(1, 5):
        s.circle(f"duckdepth{k}", (38, 118 - 5 * k), 1.1, fill=ORANGE)
    s.end()
    # ---- reserved TAIL zones ----
    s.g("sub")
    s.path("sub_arc", ell_arc(B_EARTH[0], B_EARTH[1], B_R_EARTH + 9, B_R_EARTH + 9, 110, 250), stroke=WHITE)
    s.end()
    s.g("gate")
    s.line("gate_bar", (B_EARTH[0] + 25, B_EARTH[1] - 12), (B_EARTH[0] + 25, B_EARTH[1] + 12), stroke=WHITE)
    s.end()
    s.g("swell")
    s.poly("swell_wedge", [(B_EARTH[0] - 36, B_EARTH[1] + 22), (B_EARTH[0] - 22, B_EARTH[1] + 14),
                           (B_EARTH[0] - 22, B_EARTH[1] + 22)], stroke=WHITE, w=1)
    s.end()
    s.save("space.svg", B_HEADER)


def arc_c(r, a0, a1, c):
    """The C segments of a circular arc from compass a0 to a1 (the pen is at a0)."""
    n = max(1, math.ceil(abs(a1 - a0) / 45))
    step = (a1 - a0) / n
    k = 4 / 3 * math.tan(math.radians(step) / 4)
    d = ""
    for i in range(n):
        s_, e = a0 + i * step, a0 + (i + 1) * step
        (sx, sy), (ex, ey) = P(s_, r, c), P(e, r, c)
        ts, te = math.radians(s_), math.radians(e)
        d += (f" C{f(sx + k * r * math.cos(ts))} {f(sy + k * r * math.sin(ts))}"
              f" {f(ex - k * r * math.cos(te))} {f(ey - k * r * math.sin(te))} {f(ex)} {f(ey)}")
    return d


B_HEADER = """<!--
space.svg - the space reverb as lunar ridges (generated by design/fx/gen_space_moon.py; edit that). Design:
William Buck. Drawn by src/slot/ui_space.h (published as content/display/space.svg).
Only show/hide and group matrices change. scale(s, cx, cy) = matrix(s 0 0 s  cx - s cx  cy - s cy).

BLUE SPACE - the ridges beyond the radio telescope (black-filled: they hide the sky behind them)
  ridge0 .. ridge3    ROOM: ridge0 (a near wall); HALL: ridge1; CHURCH: ridge1 + ridge2; CATHEDRAL: ridge2 + ridge3.
                      Between stages the code may translate the shown ridges down by up to 6 px toward the next
                      stage (translate(0  6 * frac(space)))
  void                VOID: no ridges, the plain runs on forever (lines to the vanishing point (204, 98))
  ridges_dim          the same in darkblue (ridge0_dim .., void_dim): shown instead when FX is off
GREEN DECAY - a rover's track from near us to the horizon
  print1 .. print12   track stretches, nearest first: the first 1 + round(11 * decay) (the knob)
  inf                 INFINITE > 0.5: the trail reaches the horizon and loops (inf1 .. inf6, inf_loop1-2)
  flag                FREEZE > 0.5 or the push held: a flag planted in the ground
WHITE TAIL - Earth and a pillar of light
  phase1 .. phase5    zone TONE: show one, phase round(5 * amount) (none = a dark new Earth .. 5 = full)
  halo                scale(0.85 + 0.35 * tail level, 262, 40): Earth's halo breathes with the tail
  shim12              zone SHIMMER +12: sh1 .. sh9, the first round(9 * amount), rising from the far plain
  shim1219            zone SHIMMER +12+19: sh1 .. sh9 as above, plus sx1 .. sx8, the first round(8 * amount)
  sub / gate / swell  reserved for the later zones SUB -12, GATE, SWELL (hidden for now)
ORANGE MIX - radio waves from the dish's feed (76.14, 86.86)
  wave1 .. wave6      the first round(6 * wet), wet = min(1, 2 * mix)
  waves               scale(1 - 0.55 * duck, 76.14, 86.86) on the master: the music pushes the reverb back
  duckdepth1 .. 4     on the master above the middle: the first ceil(4 * (2 * mix - 1)), the duck depth
DRY (neutral)
  dry / dry_ghost     the hut at the dish's foot, its window and lamp lit: one shown. As a synth/drum FX above the
                      middle the dry sound fades: dry_ghost (darkblue, unlit) once the dry level (2 - 2 * mix) < 0.5
FX OFF: show ridges_dim, hide ridges, prints, inf, flag, phase*, halo, shim*, waves, duckdepth.
CLEAN START: hide prints, inf, shim12, shim1219 for the moment the tail is clean.
-->"""


def b_state(space=2, decay=0.5, infinite=0, freeze=0, zone="tone", amount=0.6, mix=0.4, master=False, level=0.5,
            duck=0, push=False, clean=False, off=False):
    hide, tf = set(), {}
    stage, wet, depth, dry, duck = common(space, mix, master, duck)
    shown = B_STAGE_RIDGES[stage]
    for tag in ("", "_dim"):
        hide |= {f"ridge{k}{tag}" for k in range(4) if k not in shown}
        if stage < 4:
            hide.add("void" + tag)
    hide.add("ridges" if off else "ridges_dim")
    if stage < 4 and space - math.floor(space) > 0.01:
        for k in shown:
            tf[f"ridge{k}"] = f"translate(0 {f(6 * (space - math.floor(space)))})"
    n = 1 + round(11 * decay)
    hide |= {f"print{k}" for k in range(1, 13) if k > n}
    if infinite <= 0.5:
        hide.add("inf")
    if not (freeze > 0.5 or push):
        hide.add("flag")
    if zone != "tone":
        hide |= {f"phase{k}" for k in range(1, 6)}
    else:
        hide |= {f"phase{k}" for k in range(1, 6) if k != round(5 * amount)}
    tf["halo"] = scale(0.85 + 0.35 * level, 0.85 + 0.35 * level, *B_EARTH)
    if zone not in ("shim12", "shim1219"):
        hide.add("shim12")
    hide |= {f"sh{k}" for k in range(1, 10) if k > round(9 * amount)}
    if zone != "shim1219":
        hide.add("shim1219")
    hide |= {f"sx{k}" for k in range(1, 9) if k > round(8 * amount)}
    if zone == "shim1219":
        hide.discard("shim12")
    hide |= {z for z in ("sub", "gate", "swell") if z != zone}
    hide |= {f"wave{k}" for k in range(1, 7) if k > round(6 * wet)}
    if duck:
        tf["waves"] = scale(1 - 0.55 * duck, 1 - 0.55 * duck, *B_ANTENNA)
    hide |= {f"duckdepth{k}" for k in range(1, 5) if k > math.ceil(4 * depth)}
    hide.add("dry" if dry < 0.5 else "dry_ghost")
    if off:
        hide |= {"prints", "inf", "flag", "halo", "shim12", "shim1219", "waves", "duckdepth"}
        hide |= {f"phase{k}" for k in range(1, 6)}
    if clean:
        hide |= {"prints", "inf", "shim12", "shim1219"}
    return hide, tf


def states(fn):
    return {
        "smallroom": fn(space=0, decay=0.3, zone="tone", amount=0.35, mix=0.3, level=0.5),
        "cathedral": fn(space=3, decay=0.6, zone="tone", amount=0.8, mix=0.45, level=0.8),
        "void_long": fn(space=4, decay=1, infinite=1, zone="tone", amount=1, mix=0.5, level=0.9),
        "shimmer": fn(space=2.4, decay=0.7, zone="shim1219", amount=1, mix=0.4, level=0.7),
        "frozen": fn(space=3, decay=0.8, freeze=1, zone="shim12", amount=0.7, mix=0.45, level=0.6),
        "ducking": fn(space=1.5, decay=0.5, zone="tone", amount=0.6, mix=0.9, master=True, level=0.4, duck=0.8),
        "synth_wet": fn(space=2, decay=0.5, zone="tone", amount=0.5, mix=1, master=False, level=0.6),
        "off": fn(space=2, decay=0.5, zone="tone", amount=0.6, mix=0.4, off=True),
    }


if __name__ == "__main__":
    b_build()
