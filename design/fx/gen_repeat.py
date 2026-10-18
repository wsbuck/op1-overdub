#!/usr/bin/env python3
"""Generate design/fx/repeat.svg: the repeat FX screen (design: William Buck; drawn in his op1-svgs folder and
brought here unchanged but for the output path and this credit; drawn by src/slot/ui_repeat.h). The Wheel of Fortune
stands on the ground, the sphinx's head watches it from the right, the snake of the card coils round its left rim.

  blue   GRID     the wheel's slice ticks and the captured slice; it steps one slice per repeat
  green  FORTUNE  the stripes of the sphinx's headdress: a ladder meter (chance, or bars of the interval)
  white  GATE     the ground: one bar as a timeline, the fill ending at the bar line under the sphinx
  orange PITCH    the snake: a dial needle round the wheel, climbing or sliding one semitone per repeat

  python3 design/fx/gen_repeat.py     # writes design/fx/repeat.svg
"""
import math
import os

BLUE, GREEN, WHITE, ORANGE = "#698EFF", "#00ED95", "#FFFFFF", "#FF423F"
GREY, PG, LPG = "#7C7990", "#AEB1DC", "#E6E7FF"
DARKBLUE = "#383572"
BLACK = "#010000"

C = (100, 80)                       # the wheel
R_TYRE, R_RIM = 59, 54.5            # outer rim: two rings with rivets between
R_TICK_OUT, R_TICK_IN = 51.5, 42.5    # the slice ticks grow out of the inner rim
R_RING = 27                         # the letter band lies between R_RING and R_TICK_IN
R_LETTER = 34.75
R_HUB, R_AXLE = 9.5, 3
GRIDS = (2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64)   # slices per bar: 1/2, 1/2T, 1/4 ... 1/32T, 1/64

GROUND = 140                        # the timeline
BAR_X0, BAR_X1 = 30, 310            # one bar; the bar line is at BAR_X1, under the sphinx

R_SNAKE = 67                        # the snake rides here, its head at SNAKE_REST at pitch 0
R_DIAL = 76
SNAKE_REST = 270
DEG_PER_ST = 3.5                      # + up (clockwise, towards the top), - down

STRIPE_YS = [116 - 8.4 * k for k in range(10)]   # stripe1 (bottom) .. stripe10 (top)


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def P(a, r, c=C):
    t = math.radians(a)
    return c[0] + r * math.sin(t), c[1] - r * math.cos(t)


def pts(ps):
    return " ".join(f"{f(x)} {f(y)}" for x, y in ps)


def arc_c(r, a0, a1, c=C):
    """C segments of an arc from compass a0 to a1 (the pen is at a0), cubics of at most 45 degrees."""
    n = max(1, math.ceil(abs(a1 - a0) / 45))
    step = (a1 - a0) / n
    k = 4 / 3 * math.tan(math.radians(step) / 4)
    d = ""
    for i in range(n):
        s, e = a0 + i * step, a0 + (i + 1) * step
        (sx, sy), (ex, ey) = P(s, r, c), P(e, r, c)
        ts, te = math.radians(s), math.radians(e)
        d += (f" C{f(sx + k * r * math.cos(ts))} {f(sy + k * r * math.sin(ts))}"
              f" {f(ex - k * r * math.cos(te))} {f(ey - k * r * math.sin(te))} {f(ex)} {f(ey)}")
    return d


def sector_d(r0, r1, a0, a1, c=C):
    x, y = P(a0, r1, c)
    ix, iy = P(a1, r0, c)
    return f"M{f(x)} {f(y)}" + arc_c(r1, a0, a1, c) + f" L{f(ix)} {f(iy)}" + arc_c(r0, a1, a0, c) + " Z"


# ---- writers -----------------------------------------------------------------------------------------------------

out = []
depth = [0]


def emit(s):
    out.append("  " * depth[0] + s)


def attrs(stroke=None, fill="none", w=1.5, cap="square", join=None):
    a = [f'fill="{fill}"']
    if stroke:
        a += [f'stroke="{stroke}"', f'stroke-width="{f(w)}"', f'stroke-linecap="{cap}"']
        if join:
            a.append(f'stroke-linejoin="{join}"')
    return " ".join(a)


def g_open(i):
    emit(f'<g id="{i}">')
    depth[0] += 1


def g_close():
    depth[0] -= 1
    emit("</g>")


def path(i, d, **kw):
    emit(f'<path id="{i}" d="{d}" {attrs(**kw)}/>')


def poly(i, ps, close=True, **kw):
    tag = "polygon" if close else "polyline"
    emit(f'<{tag} id="{i}" points="{pts(ps)}" {attrs(**kw)}/>')


def line(i, a, b, **kw):
    emit(f'<line id="{i}" x1="{f(a[0])}" y1="{f(a[1])}" x2="{f(b[0])}" y2="{f(b[1])}" {attrs(**kw)}/>')


def circle(i, c, r, **kw):
    emit(f'<circle id="{i}" cx="{f(c[0])}" cy="{f(c[1])}" r="{f(r)}" {attrs(**kw)}/>')


# ---- the wheel (blue: GRID) --------------------------------------------------------------------------------------

def comb(i, n):
    """n slice ticks as one filled comb polygon; the inner rim's black disc hides its middle."""
    half = 0.75
    ps = []
    for k in range(n):
        a = k * 360 / n
        di, do = math.degrees(half / R_TICK_IN), math.degrees(half / R_TICK_OUT)
        ps += [P(a - di, R_TICK_IN), P(a - do, R_TICK_OUT), P(a + do, R_TICK_OUT), P(a + di, R_TICK_IN)]
    poly(i, ps, fill=BLUE)


LETTERS = {   # local coords: x right, y down, centred, about 6 x 8
    "T": [[(-3, -4), (3, -4)], [(0, -4), (0, 4)]],
    "A": [[(-3.2, 4), (0, -4), (3.2, 4)], [(-1.8, 1), (1.8, 1)]],
    "R": "R",
    "O": "O",
}


def letter(ch, a, r):
    """A letter on the wheel at compass a, its top pointing out from the centre."""
    rad = (math.sin(math.radians(a)), -math.cos(math.radians(a)))
    tan = (math.cos(math.radians(a)), math.sin(math.radians(a)))

    def m(p):
        x, y = p
        return C[0] + (r - y) * rad[0] + x * tan[0], C[1] + (r - y) * rad[1] + x * tan[1]

    i = f"letter_{ch}"
    s = dict(stroke=PG)
    if ch == "O":
        rx, ry, k = 3, 4, 0.5523
        q = [(0, -ry), (rx * k, -ry), (rx, -ry * k), (rx, 0), (rx, ry * k), (rx * k, ry), (0, ry), (-rx * k, ry),
             (-rx, ry * k), (-rx, 0), (-rx, -ry * k), (-rx * k, -ry), (0, -ry)]
        q = [m(p) for p in q]
        path(i, f"M{pts(q[:1])}" + "".join(f" C{pts(q[j:j + 3])}" for j in range(1, 13, 3)) + " Z", **s)
    elif ch == "R":
        q = [m(p) for p in [(-2.6, 4), (-2.6, -4), (0.6, -4), (3.4, -4), (3.4, 0), (0.6, 0), (-2.6, 0), (2.8, 4)]]
        path(i, f"M{pts(q[0:1])} L{pts(q[1:2])} L{pts(q[2:3])} C{pts(q[3:6])} L{pts(q[6:7])} L{pts(q[7:8])}", **s)
    else:
        g_open(i)
        for k, st in enumerate(LETTERS[ch], 1):
            if len(st) == 2:
                line(f"{i}{k}", m(st[0]), m(st[1]), **s)
            else:
                poly(f"{i}{k}", [m(p) for p in st], close=False, **s)
        g_close()


def wheel():
    g_open("wheel")
    circle("tyre", C, R_TYRE, stroke=PG, fill=BLACK)
    circle("rim", C, R_RIM, stroke=PG)
    g_open("rivets")
    for k in range(16):
        circle(f"rivet{k + 1}", P(k * 22.5 + 11.25, (R_TYRE + R_RIM) / 2), 0.9, fill=GREY)
    g_close()
    g_open("ticks")
    for n in GRIDS:
        comb(f"ticks{n}", n)
    g_close()
    g_open("slice")
    for n in GRIDS:
        path(f"slice{n}", sector_d(R_TICK_IN, R_TICK_OUT, 0, 360 / n), fill=BLUE)
    g_close()
    circle("rim_in", C, R_TICK_IN, stroke=PG, fill=BLACK)
    circle("ring", C, R_RING, stroke=PG)
    # 8 spokes through the letter band, 4 inside the ring (they meet the letters' gaps)
    for k in range(8):
        a = k * 45 + 22.5
        line(f"spoke{k + 1}", P(a, R_RING + 1.5), P(a, R_TICK_IN - 1.5), stroke=PG)
    for k in range(4):
        a = k * 90 + 45
        line(f"spokein{k + 1}", P(a, R_HUB + 1.5), P(a, R_RING - 1.5), stroke=PG)
    for ch, a in (("T", 0), ("A", 90), ("R", 180), ("O", 270)):
        letter(ch, a, R_LETTER)
    # the card's four elements between the inner spokes: small marks on the ring's inside
    for k, a in enumerate((0, 90, 180, 270), 1):
        circle(f"seal{k}", P(a, 18.25), 2, stroke=GREY, w=1)
    circle("hub", C, R_HUB, stroke=PG, fill=BLACK)
    circle("axle", C, R_AXLE, fill=PG)
    g_close()


# ---- the sphinx's head (green stripes: FORTUNE) ------------------------------------------------------------------

def back_x(y):
    """x of the headdress' back edge at height y: a straight flare from (289, 40) to (308, 128)."""
    return 289 + (y - 40) * 19 / 88


def sphinx():
    g_open("sphinx")
    # the nemes: crown, then the cloth flaring behind the head down to the shoulders
    path("nemes", "M214 34 C215 23 228 17 248 17 C267 17 282 25 289 40 L308 128 L248 128 L244 78 L232 42 Z",
         stroke=LPG, fill=BLACK, join="round")
    # shoulders and chest, under the lappet
    path("shoulder", "M248 128 C252 120 262 116 276 116", stroke=PG, join="round")
    path("chest", "M226 92 L226 106 C224 116 219 124 214 132", stroke=PG, join="round")
    # the face in profile, looking left at the wheel
    path("face", "M214 34 L230 34 L240 66 L236 86 C232 90 226 92 214 91 L207 88 L203 84 L201 78 L203 76.5 L200.5 74"
         " L203 71.5 L204 67.5 L199 66 L198 63.5 L203.5 56 L208.5 47 L211.5 44.5 Z", stroke=LPG, fill=BLACK,
         join="round")
    line("brow", (209.5, 45.5), (219, 45), stroke=PG)
    path("ear", "M236 50 C230 50 229 64 235 64", stroke=PG, join="round")
    line("cheek", (207, 79), (213, 76), stroke=GREY, w=1)
    # the brow band of the nemes and the uraeus cobra rearing from it
    line("band", (214, 34), (231, 34), stroke=LPG)
    path("uraeus", "M216.5 34 C214 31 214.5 27 217.5 26.5 C220 26 220.5 29 218.5 30.5", stroke=LPG, join="round")
    # the lappet hangs in front of the shoulder, with grey pleats
    poly("lappet", [(232, 42), (244, 78), (250, 136), (232, 136), (234, 96), (238, 86)], stroke=LPG, fill=BLACK)
    for k, y in enumerate((100, 108, 116, 124, 132), 1):
        line(f"pleat{k}", (234.5, y), (248, y), stroke=GREY, w=1)
    # the false beard, plaited
    poly("beard", [(208, 89), (216, 91), (218, 112), (213, 116), (210, 113)], stroke=LPG, fill=BLACK)
    for k, y in enumerate((96, 102, 108), 1):
        line(f"plait{k}", (210, y), (216.5, y + 1), stroke=GREY, w=1)
    line("bust", (250, 136), (310, 136), stroke=PG)
    # the eye: shut while waiting, open (with its kohl line) while a fill fires
    path("eye_shut", "M210 51.5 C212 53 215 53 217 51.5", stroke=LPG)
    g_open("eye_open")
    path("eye_lid", "M209.5 51.5 C211.5 48.5 215.5 48.5 218 51 L223 50.5", stroke=LPG)
    circle("eye_ball", (213.2, 51.6), 1.5, fill=LPG)
    g_close()
    g_close()


def stripes():
    """Ten stripes across the back of the headdress: darkblue always, green on top for the lit ones."""
    g_open("stripes_dim")
    for k, y in enumerate(STRIPE_YS, 1):
        line(f"stripe_dim{k}", (252, y), (back_x(y) - 4, y), stroke=DARKBLUE, w=2, cap="butt")
    g_close()
    g_open("stripes")
    for k, y in enumerate(STRIPE_YS, 1):
        line(f"stripe{k}", (252, y), (back_x(y) - 4, y), stroke=GREEN, w=2, cap="butt")
    g_close()


def pow_marks():
    """Short strokes radiating over the crown while a fill fires."""
    c = (250, 34)
    g_open("pow")
    for k, a in enumerate((-36, -12, 12, 36), 1):
        line(f"pow{k}", P(a, 23, c), P(a, 29, c), stroke=LPG)
    g_close()


# ---- the ground: one bar (white: GATE) ---------------------------------------------------------------------------

def ground():
    w = (BAR_X1 - BAR_X0) / 16
    seg = lambda i: ((BAR_X0 + (i - 1) * w + 1.5, GROUND), (BAR_X0 + i * w - 1.5, GROUND))
    g_open("gate_dim")
    for i in range(1, 17):
        line(f"gate_dim{i}", *seg(i), stroke=DARKBLUE, w=2, cap="butt")
    g_close()
    g_open("gate")
    for i in range(1, 17):
        line(f"gate{i}", *seg(i), stroke=WHITE, w=2, cap="butt")
    g_close()
    line("barline", (BAR_X1, GROUND - 5), (BAR_X1, GROUND + 3), stroke=PG)
    for k in range(1, 4):          # gates over a bar: a pip per extra bar
        circle(f"lap{k}", (315.5, GROUND - (k - 1) * 5), 1.4, fill=WHITE)
    g_open("playhead")
    circle("playhead_dot", (BAR_X0, GROUND), 2.4, stroke=BLACK, fill=LPG)
    g_close()


# ---- the snake (orange: PITCH) -----------------------------------------------------------------------------------

def snake(i, color, sign):
    """The snake on R_SNAKE, its head at SNAKE_REST: sign +1 climbs (head up the rim, clockwise), -1 dives (head
    down, anticlockwise). The body coils back behind the head."""
    a = SNAKE_REST
    g_open(i)
    body = []
    for k in range(37):
        t = k / 36                                   # 0 neck .. 1 tail tip
        amp = 4.6 * (1 - 0.6 * t) * min(1, t * 5)
        body.append(P(a - sign * (3.5 + t * 38), R_SNAKE + amp * math.sin(t * 3 * 2 * math.pi)))
    poly(i + "_body", body, close=False, stroke=color, w=2.5, join="round")
    hc = P(a, R_SNAKE)
    fwd = (sign * math.cos(math.radians(a)), sign * math.sin(math.radians(a)))
    out_ = (math.sin(math.radians(a)), -math.cos(math.radians(a)))
    H = lambda u, v: (hc[0] + u * fwd[0] + v * out_[0], hc[1] + u * fwd[1] + v * out_[1])
    poly(i + "_head", [H(-4, 0), H(-1, 4), H(4, 3), H(6, 0), H(4, -3), H(-1, -4)], stroke=color,
         fill=BLACK, join="round")
    circle(i + "_eye", H(2, 1.4), 0.9, fill=color)
    poly(i + "_tongue1", [H(6.6, 0), H(9.4, 0), H(11.4, 1.8)], close=False, stroke=color, w=1)
    line(i + "_tongue2", H(9.4, 0), H(11.4, -1.8), stroke=color, w=1)
    g_close()


def dial():
    line("dial_zero", P(SNAKE_REST, R_DIAL - 2), P(SNAKE_REST, R_DIAL + 2), stroke=GREY)
    g_open("up")
    for st in range(1, 13):
        circle(f"up{st}", P(SNAKE_REST + DEG_PER_ST * st, R_DIAL), 1.2, fill=ORANGE)
    g_close()
    g_open("down")
    for st in range(1, 13):
        circle(f"down{st}", P(SNAKE_REST - DEG_PER_ST * st, R_DIAL), 1.2, fill=ORANGE)
    g_close()
    sx, sy = P(SNAKE_REST - DEG_PER_ST * 13.5, R_DIAL)
    poly("tapestop", [(sx - 2.5, sy - 2.5), (sx + 2.5, sy - 2.5), (sx + 2.5, sy + 2.5), (sx - 2.5, sy + 2.5)],
         fill=ORANGE)
    ux, uy = P(SNAKE_REST + DEG_PER_ST * 13.5, R_DIAL)
    g_open("spinup")
    poly("spinup_1", [(ux - 3, uy + 1), (ux, uy - 2), (ux + 3, uy + 1)], close=False, stroke=ORANGE)
    poly("spinup_2", [(ux - 3, uy + 4.5), (ux, uy + 1.5), (ux + 3, uy + 4.5)], close=False, stroke=ORANGE)
    g_close()


HEADER = """<!--
repeat.svg - the repeat FX screen (generated by design/fx/gen_repeat.py; edit that). Design: William Buck.
Drawn by src/slot/ui_repeat.h (published as content/display/repeat.svg).
The Wheel of Fortune, the sphinx's head, the snake. Only show/hide and group matrices change.
rot(a, cx, cy) = matrix(cos a  sin a  -sin a  cos a  cx - cx cos a + cy sin a  cy - cx sin a - cy cos a), a clockwise.

BLUE GRID - the wheel, centre (100, 80)
  ticks2 .. ticks64   show one: slices per bar (2 3 4 6 8 12 16 24 32 48 64 = 1/2 1/2T 1/4 .. 1/32T 1/64)
  slice2 .. slice64   the captured slice (0 .. 360/n deg): show the one for the grid while a fill fires
  wheel               rot(360/n * repeat, 100, 80): one slice per repeat while firing, still otherwise
GREEN FORTUNE - the headdress stripes (a ladder, stripe1 at the bottom)
  stripe1 .. 10       chance 10..90% = the first 1..9; ALWAYS = all 10; VARY = 2 4 6 8 10;
                      every N bars = bars elapsed of the interval, the first k * 8 / N (8 stripes = the interval);
                      OFF = none. stripes_dim stays under them
WHITE GATE - the ground is one bar, left to right, the bar line under the sphinx
  gate1 .. gate16     a sixteenth each; a gate of n sixteenths shows gate(17-n) .. gate16. While firing, hide each
                      one the playhead has passed (the fill burns down to the downbeat)
  gate_dim1 .. 16     the same in darkblue, shown instead when this bar will not fire (off, lost roll, interval)
  lap1 .. lap3        gates over a bar: 2 bars lap1, 3 bars lap1-2, 4 bars all (with all 16 segments)
  playhead            translate(280 * bar phase  0)
FIRING (neutral)
  eye_shut / eye_open one shown: open while a fill fires
  pow                 shown while a fill fires (flash it on each repeat for more life)
ORANGE PITCH - the snake is the needle of a dial round the wheel's left rim
  up1 .. up12 / down1 .. down12   the knob's range: the first |st| dots on its side
  tapestop / spinup   the extremes
  snake               rot(3.5 * st, 100, 80), st = semitones reached so far (+ up, - down; +-12.5 at the extremes, by the marks);
                      show one inside: snake_grey at pitch 0, snake_up (climbing) when the knob is up,
                      snake_down (diving head first) when it is down
-->"""


def build():
    emit('<?xml version="1.0" encoding="utf-8"?>')
    out.append(HEADER)
    emit('<svg version="1.1" xmlns="http://www.w3.org/2000/svg" x="0" y="0" width="320" height="160" '
         'viewBox="0 0 320 160">')
    depth[0] += 1
    dial()
    wheel()
    g_open("snake")
    snake("snake_grey", PG, 1)
    snake("snake_up", ORANGE, 1)
    snake("snake_down", ORANGE, -1)
    g_close()
    sphinx()
    stripes()
    pow_marks()
    ground()
    depth[0] -= 1
    emit("</svg>")
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "repeat.svg"), "w") as fh:
        fh.write("\n".join(out) + "\n")


if __name__ == "__main__":
    build()
