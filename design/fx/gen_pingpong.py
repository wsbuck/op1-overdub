#!/usr/bin/env python3
"""Table Tennis, HQ hybrid: pingpong.svg (shippable asset, TE retained-renderer subset) + composite mock frames
(asset with the runtime matrices / visibility / recolours applied + our immediate-mode line overlay) + a GIF."""
import math, os, subprocess, json
import strictify

OUT = os.path.dirname(os.path.abspath(__file__))
RSVG = "/opt/homebrew/bin/rsvg-convert"
MAGICK = "/opt/homebrew/bin/magick"

C = dict(green="#00ED95", lightgreen="#80FFAF", blue="#698EFF", lightblue="#BFD8FF", white="#FFFFFF",
         orange="#FF423F", lightorange="#FFB3B3", grey="#7C7990", darkblue="#383572", purplegrey="#AEB1DC",
         lpg="#E6E7FF", purple="#5F4270", offgreen="#002826", darkgreen="#00475B", offblue="#1D2040",
         offgrey="#353238", offred="#4E2832", red="#FF3A5D", pink="#FF90A7", black="#010000")

CX = 160          # table centre
TY = 100          # table top surface
FLOOR = 132       # feet
HREF = 104        # table half-length the asset is drawn at (= blue knob max); runtime scales/translates from it
HMIN = 40         # half-length at blue = 0
PGAP = 26         # player centre beyond the table end
BAT_Y = 70        # bat contact height
NPIP = 11         # scoreboard: game to 11


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


class N:
    def __init__(self, tag, id=None, children=None, **a):
        self.tag, self.id, self.ch, self.a = tag, id, children or [], a


def A(**kw):        # attribute helper: stroke_width -> stroke-width
    return {k.replace("_", "-"): v for k, v in kw.items()}


def path_ellipse(cx, cy, rx, ry, rot=0.0):
    k = 0.5523
    cr, sr = math.cos(math.radians(rot)), math.sin(math.radians(rot))
    T = lambda x, y: (cx + x * cr - y * sr, cy + x * sr + y * cr)
    p = [T(rx, 0), T(rx, k * ry), T(k * rx, ry), T(0, ry), T(-k * rx, ry), T(-rx, k * ry), T(-rx, 0),
         T(-rx, -k * ry), T(-k * rx, -ry), T(0, -ry), T(k * rx, -ry), T(rx, -k * ry), T(rx, 0)]
    s = f"M{f(p[0][0])} {f(p[0][1])}"
    for i in range(1, 13, 3):
        s += " C" + " ".join(f"{f(p[j][0])} {f(p[j][1])}" for j in (i, i + 1, i + 2))
    return s + " Z"


def P(*pts, close=False):
    return " ".join(("M" if i == 0 else "L") + f"{f(p[0])} {f(p[1])}" for i, p in enumerate(pts)) + (" Z" if close else "")


# ------------------------------------------------------------------ artwork
def scene():
    L = []
    # arena: low barrier boards behind the table (two flat fills), floor line + lip
    L.append(N("rect", "barrier", x="4", y="104", width="312", height="20", fill=C["offblue"]))
    L.append(N("path", "barrier_seams", d=" ".join(f"M{x} 104 L{x} 124" for x in (56, 108, 160, 212, 264)), fill="none",
               **A(stroke=C["darkblue"], stroke_width="1")))
    L.append(N("line", "floor", x1="4", y1=f(FLOOR), x2="316", y2=f(FLOOR), **A(stroke=C["purplegrey"], stroke_width="1.5")))
    L.append(N("line", "lip", x1="4", y1=f(FLOOR + 3), x2="316", y2=f(FLOOR + 3), **A(stroke=C["darkblue"], stroke_width="1")))
    return N("g", "arena", L)


def table():
    x0, x1 = CX - HREF, CX + HREF
    top = [N("rect", "slab", x=f(x0), y=f(TY), width=f(2 * HREF), height="6", fill=C["darkgreen"]),
           N("line", "edge", x1=f(x0), y1=f(TY), x2=f(x1), y2=f(TY), **A(stroke=C["lpg"], stroke_width="1.5")),
           N("line", "underside", x1=f(x0 + 4), y1=f(TY + 7.5), x2=f(x1 - 4), y2=f(TY + 7.5),
             **A(stroke=C["offgreen"], stroke_width="1.5"))]
    def leg(side):
        x = CX + side * (HREF - 14)
        return N("g", "legL" if side < 0 else "legR", [
            N("path", f"leg{'L' if side < 0 else 'R'}d", d=P((x - side * 6, TY + 7), (x, FLOOR), (x + side * 2, FLOOR), (x - side * 3, TY + 7), close=True),
              fill=C["grey"])])
    net = [N("line", "netpost", x1=f(CX), y1=f(TY - 12), x2=f(CX), y2=f(TY), **A(stroke=C["lpg"], stroke_width="1.5")),
           N("path", "netmesh", d=" ".join(f"M{f(CX - 2.5)} {f(TY - 10 + 2.5 * i)} L{f(CX + 2.5)} {f(TY - 10 + 2.5 * i)}" for i in range(4)),
             fill="none", **A(stroke=C["grey"], stroke_width="1")),
           N("line", "nettape", x1=f(CX - 3), y1=f(TY - 11.5), x2=f(CX + 3), y2=f(TY - 11.5), **A(stroke=C["white"], stroke_width="2", stroke_linecap="round"))]
    return N("g", "table", [N("g", "tabletop", top), leg(-1), leg(1), N("g", "net", net)])


def wall():
    """squash front wall (mono): replaces the right player at the table end; bricks as strokes"""
    x = CX + HREF
    bricks = []
    for r in range(9):
        y = 40 + r * 10
        bricks.append(f"M{f(x)} {f(y)} L{f(x + 22)} {f(y)}")
        bx = x + (8 if r % 2 else 15)
        bricks.append(f"M{f(bx)} {f(y)} L{f(bx)} {f(y + 10)}")
    return N("g", "wall", [
        N("rect", "wallface", x=f(x), y="30", width="22", height=f(FLOOR - 30), fill=C["offgrey"]),
        N("path", "bricks", d=" ".join(bricks), fill="none", **A(stroke=C["purple"], stroke_width="1")),
        N("line", "wallfront", x1=f(x), y1="30", x2=f(x), y2=f(FLOOR), **A(stroke=C["purplegrey"], stroke_width="2")),
        N("line", "tin", x1=f(x - 1), y1=f(FLOOR - 16), x2=f(x + 22), y2=f(FLOOR - 16), **A(stroke=C["lightblue"], stroke_width="2")),
        N("line", "courtline", x1=f(CX - HREF - 40), y1=f(FLOOR - 1.5), x2=f(x), y2=f(FLOOR - 1.5), **A(stroke=C["darkblue"], stroke_width="1.5")),
    ])


def player(side):
    """side -1 = left player (faces right), +1 = right player. Drawn at the reference table length."""
    s = "L" if side < 0 else "R"
    px = CX + side * (HREF + PGAP)
    shirt = "blue" if side < 0 else "lightblue"
    hair = "darkblue" if side < 0 else "blue"
    d = -side                                   # direction toward the table
    hy = 62                                     # head centre
    legs = N("path", f"legs{s}", d=f"M{f(px - 5)} 116 L{f(px - 7)} {f(FLOOR - 2)} M{f(px + 5)} 116 L{f(px + 7)} {f(FLOOR - 2)}",
             fill="none", **A(stroke=C["lpg"], stroke_width="3.5", stroke_linecap="round"))
    shoes = N("path", f"shoes{s}", d=path_ellipse(px - 7 + d * 1.5, FLOOR - 1.5, 4.5, 2) + " " + path_ellipse(px + 7 + d * 1.5, FLOOR - 1.5, 4.5, 2),
              fill=C["white"])
    shorts = N("path", f"shorts{s}", d=P((px - 12, 106), (px + 12, 106), (px + 13, 118), (px + 1.5, 118), (px, 113), (px - 1.5, 118), (px - 13, 118), close=True),
               fill=C["darkblue"])
    torso = N("path", f"shirt{s}",
              d=f"M{f(px - 9)} 80 C{f(px - 14)} 82 {f(px - 14)} 96 {f(px - 12.5)} 108 L{f(px + 12.5)} 108 C{f(px + 14)} 96 {f(px + 14)} 82 {f(px + 9)} 80 "
                f"C{f(px + 4)} 78 {f(px - 4)} 78 {f(px - 9)} 80 Z", fill=C[shirt], **A(stroke=C[shirt], stroke_width="1.5", stroke_linejoin="round"))
    stripe = N("path", f"stripe{s}", d=f"M{f(px - 13)} 96 L{f(px + 13)} 96", fill="none", **A(stroke=C["white"], stroke_width="2"))
    back_arm = N("path", f"arm2{s}", d=f"M{f(px - d * 9)} 84 C{f(px - d * 15)} 90 {f(px - d * 14)} 98 {f(px - d * 10)} 102", fill="none",
                 **A(stroke=C["lpg"], stroke_width="3.5", stroke_linecap="round"))
    # head
    face = N("circle", f"face{s}", cx=f(px), cy=f(hy), r="12.5", fill=C["lpg"])
    capd = (f"M{f(px - 13.4)} {f(hy + 1)} C{f(px - 15)} {f(hy - 18)} {f(px + 15)} {f(hy - 18)} {f(px + 13.4)} {f(hy + 1)} "
            f"C{f(px + 9)} {f(hy - 6)} {f(px - 9)} {f(hy - 6)} {f(px - 13.4)} {f(hy + 1)} Z")
    # sweatband across the forehead + a ponytail (left) / spikes (right) for character
    if side < 0:
        capd += (f" M{f(px - d * 12)} {f(hy - 4)} C{f(px - d * 20)} {f(hy - 6)} {f(px - d * 23)} {f(hy + 2)} {f(px - d * 21)} {f(hy + 9)} "
                 f"C{f(px - d * 19)} {f(hy + 3)} {f(px - d * 16)} {f(hy)} {f(px - d * 11)} {f(hy + 1)} Z")
    else:
        capd += (f" M{f(px - 6)} {f(hy - 11)} L{f(px - 3)} {f(hy - 18)} L{f(px)} {f(hy - 12)} L{f(px + 4)} {f(hy - 19)} L{f(px + 6)} {f(hy - 11)} Z")
    hairn = N("path", f"hair{s}", d=capd, fill=C[hair], **A(stroke=C[hair], stroke_width="1", stroke_linejoin="round"))
    band = N("path", f"band{s}", d=f"M{f(px - 12.8)} {f(hy - 3)} C{f(px - 6)} {f(hy - 7)} {f(px + 6)} {f(hy - 7)} {f(px + 12.8)} {f(hy - 3)}",
             fill="none", **A(stroke=C["white"], stroke_width="2.5"))
    ex = px + d * 3                              # eyes look toward the table
    eyes = N("path", f"eyes{s}", d=path_ellipse(ex - 4.2, hy + 1.5, 1.5, 2) + " " + path_ellipse(ex + 4.2, hy + 1.5, 1.5, 2), fill=C["black"])
    cheeks = N("path", f"cheek{s}", d=path_ellipse(px - 8, hy + 5.5, 2.1, 1.4) + " " + path_ellipse(px + 8, hy + 5.5, 2.1, 1.4), fill=C["pink"])
    smile = N("path", f"smile{s}", d=f"M{f(ex - 2.5)} {f(hy + 6.5)} C{f(ex - 1.2)} {f(hy + 8.5)} {f(ex + 1.2)} {f(hy + 8.5)} {f(ex + 2.5)} {f(hy + 6.5)}",
              fill="none", **A(stroke=C["black"], stroke_width="1.3", stroke_linecap="round"))
    yell = N("path", f"yell{s}", d=path_ellipse(ex, hy + 7.3, 2.2, 2.6), fill=C["black"], **A(stroke=C["black"], stroke_width="0.8"))
    head = N("g", f"head{s}", [face, hairn, band, eyes, cheeks, N("g", f"smileg{s}", [smile]), N("g", f"yellg{s}", [yell])])
    # bat arm: pivot at the shoulder (px + d*9, 84); bat contact face at (px + d*24, BAT_Y)
    sx, sy = px + d * 9, 84
    hx, hy2 = px + d * 19, 80
    bat = N("g", f"bat{s}", [
        N("path", f"arm{s}", d=f"M{f(sx)} {f(sy)} C{f(sx + d * 4)} {f(sy + 3)} {f(hx - d * 3)} {f(hy2 + 2)} {f(hx)} {f(hy2)}", fill="none",
          **A(stroke=C["lpg"], stroke_width="3.5", stroke_linecap="round")),
        N("line", f"handle{s}", x1=f(hx - d * 1), y1=f(hy2 + 1), x2=f(hx + d * 3), y2=f(BAT_Y + 6), **A(stroke=C["purplegrey"], stroke_width="3.2", stroke_linecap="round")),
        N("path", f"blade{s}", d=path_ellipse(px + d * 24, BAT_Y - 1, 6.2, 8, d * 12), fill=C["blue"],
          **A(stroke=C["lightblue"], stroke_width="1.5")),
    ])
    return N("g", f"p{s}", [legs, shoes, shorts, back_arm, torso, stripe, head, bat])


def sparkle_d(r):
    q = r * 0.26
    return (f"M0 {f(-r)} L{f(q)} {f(-q)} L{f(r)} 0 L{f(q)} {f(q)} L0 {f(r)} L{f(-q)} {f(q)} L{f(-r)} 0 L{f(-q)} {f(-q)} Z")


def ball():
    """drawn at the origin; runtime translates the 'ball' group to the live position"""
    return N("g", "ball", [
        N("g", "glow", [N("path", "glowstar", d=sparkle_d(11), fill=C["white"], **A(stroke=C["white"], stroke_width="0.8", stroke_linejoin="round"))]),
        N("circle", "ballbody", cx="0", cy="0", r="4.6", fill=C["white"], **A(stroke=C["white"], stroke_width="1.2")),
        N("path", "ballshade", d="M-3.6 1.6 C-2 4.6 2.4 4.8 4.2 1.6 C2.6 2.4 -1.6 2.6 -3.6 1.6 Z", fill=C["grey"], **A(stroke=C["grey"], stroke_width="0.8")),
        N("circle", "ballglint", cx="-1.5", cy="-1.6", r="1.1", fill=C["lpg"], **A(stroke=C["lpg"], stroke_width="0.5")),
    ])


def shadow():
    return N("g", "shadow", [N("path", "shadowd", d=path_ellipse(0, 0, 5, 1.3), fill=C["offgreen"], **A(stroke=C["offgreen"], stroke_width="0.5"))])


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
    kids = [scene(), table(), wall(), shadow(), player(-1), player(1), scoreboard(), ball()]
    return N("svg", None, kids, version="1.1", xmlns="http://www.w3.org/2000/svg", x="0", y="0", width="320", height="160",
             viewBox="0 0 320 160")


HEADER = f"""
pingpong.svg - "pingpong" FX screen (Table Tennis). Our own artwork. TE's retained renderer draws it, then our
immediate-mode overlay (the green echo arches) goes on top. All ids are found once at load (find, assert=0) and cached.
No text anywhere. Reference geometry: table centre x = {CX}, top y = {TY}, drawn at half-length HREF = {HREF};
players stand PGAP = {PGAP} beyond each end; bat contact height y = {BAT_Y}; floor y = {FLOOR}.
Knobs: u = blue/32767, half = {HMIN} + ({HREF} - {HMIN}) * u; g = 0.95 * green/32767; tone = (white - 16384)/16384; mix = orange/32767.
  tabletop          matrix = scale(half/{HREF}, 1) about ({CX}, {TY})                                      [blue: table length = delay time]
  legL / legR       matrix = translate(+-({HREF} - half), 0)
  pL / pR           matrix = translate(+-({HREF} - half), 0)   (players walk in/out with the table ends)
  batL / batR       extra matrix = rotate(a) about the shoulder (x = player x -+ 17, y = 84):
                    hitter swings through a = -35 deg -> +10 deg over 0.12 s after its hit, then rests at 0;
                    receiver cocks back to +20 deg over the last 40 % of the flight (sign mirrored for R)
  smilegL/R yellgL/R  show yell (open mouth) on the hitter for 0.12 s after the hit, smile otherwise
  ball              matrix = translate(x, y) along the newest arch (same curve as the overlay arch), once per
                    flight period T = d * ceil(0.25 s / d) (d = delay time), direction flips every flight (L->R, R->L)
  ballbody          recolour fill: tone < -0.5 grey, < -0.1 purplegrey, else white (stroke white)            [white: tone]
  ballshade         shown when tone < -0.1 (dull, heavy ball)
  ballglint         shown when tone > -0.1
  glow              shown when tone > 0.1; matrix = rotate(t * 360 deg/s) * scale(0.35 + 0.65 * tone) about the ball centre
  shadow            matrix = translate(ball x, {TY}) * scale(1 - 0.5 * h, 1) where h = ball height above the bats / 50 (squash: floor y {FLOOR - 1})
  pip1..pip{NPIP}       recolour fill+stroke: pip j <= round({NPIP} * mix) orange (wet), others offred (dry); lit from the right [orange: mix]
  table             hidden for synth/drum FX (mono = squash)
  wall              shown only for synth/drum FX; matrix = translate({HREF} - half ... i.e. -( {HREF} - half ), 0)
  pR                hidden for synth/drum FX
  everything else   static (arena barrier, floor, net, scoreboard board + wires).
Overlay (immediate mode, <= ~56 AA segments, green = feedback):
  echo k = 0.. while g^k > 0.04 (max 8): an arch bat-face to bat-face (squash: bat to wall), peak 6 + 36 * g^k above the bats;
  k = 0 lightgreen 2 px, 14 segments; k = 1..5 green 1.2 px, 8 segments; older: darkgreen 1-px framebuffer lines.
  A short lightblue 1-px swoosh (3 arcs) below/in front of the hitter's bat for 0.12 s after each hit.
"""


def render(node, state=None, ind=""):
    st = (state or {}).get(node.id, {}) if node.id else {}
    if st.get("hide"):
        return ""
    a = dict(node.a)
    if node.id:
        a = {"id": node.id, **a}
    if "fill" in st:
        a["fill"] = C[st["fill"]]
    if "stroke" in st:
        a["stroke"] = C[st["stroke"]]
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
    return (0 if node.tag in ("svg", "g") else 1) + sum(count(c, S) for c in node.ch)


# ------------------------------------------------------------------ runtime model
def rot(deg, px, py):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return (c, s, -s, c, px - c * px + s * py, py - s * px - c * py)


def mul(m1, m2):          # apply m2 then m1
    a1, b1, c1, d1, e1, f1 = m1
    a2, b2, c2, d2, e2, f2 = m2
    return (a1 * a2 + c1 * b2, b1 * a2 + d1 * b2, a1 * c2 + c1 * d2, b1 * c2 + d1 * d2,
            a1 * e2 + c1 * f2 + e1, b1 * e2 + d1 * f2 + f1)


def tr(x, y):
    return (1, 0, 0, 1, x, y)


def scale_about(sx, sy, px, py):
    return (sx, 0, 0, sy, px - sx * px, py - sy * py)


def knobs(k):
    u = k["time"] / 32767
    d = 0.020 * 2 ** (6 * u)
    return dict(u=u, d=d, half=HMIN + (HREF - HMIN) * u, g=0.95 * k["fb"] / 32767,
                tone=(k["tone"] - 16384) / 16384, mix=k["mix"] / 32767)


def ends(kv, stereo):
    """bat-face x positions (left, right) and the arch end y; squash: right end is the wall face"""
    half = kv["half"]
    xl = CX - half - PGAP + 24
    xr = (CX + half + PGAP - 24) if stereo else (CX + half - 5)
    return xl, xr


def arch(x0, x1, y0, y1, h, n):
    pts = []
    for i in range(n + 1):
        t = i / n
        pts.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t - 4 * h * t * (1 - t)))
    return pts


def echo_levels(g):
    lv, a = [], 1.0
    while a > 0.04 and len(lv) < 8:
        lv.append(a)
        a *= g
    return lv


def flight(kv, t):
    T = kv["d"] * math.ceil(0.25 / kv["d"])
    n = int(t // T)
    return T, n, (t % T) / T          # period, flight index, phase 0..1


def runtime_state(k):
    kv = knobs(k)
    stereo, t = k["stereo"], k["t"]
    half = kv["half"]
    dx = HREF - half
    S = {}
    S["tabletop"] = {"m": scale_about(half / HREF, 1, CX, TY)}
    S["legL"] = {"m": tr(dx, 0)}
    S["legR"] = {"m": tr(-dx, 0)}
    T, n, ph = flight(kv, t)
    hitter = "L" if (n % 2 == 0 or not stereo) else "R"
    since_hit = ph * T
    if not stereo:
        hitter = "L"
    for s, side in (("L", -1), ("R", 1)):
        px = CX + side * (HREF + PGAP) + (dx if side < 0 else -dx)
        S[f"p{s}"] = {"m": tr(dx if side < 0 else -dx, 0)}
        shx = CX + side * (HREF + PGAP) - side * 9          # shoulder in asset coords (before the player translate)
        d = -side
        if s == hitter:
            a = -35 + 45 * min(since_hit / 0.12, 1.0) if since_hit < 0.12 else 0
            a = 10 if 0.12 <= since_hit < 0.2 else a
            S[f"yellg{s}"] = {} if since_hit < 0.12 else {"hide": True}
            S[f"smileg{s}"] = {"hide": True} if since_hit < 0.12 else {}
        else:
            a = 20 * max(0.0, (ph - 0.6) / 0.4)
            S[f"yellg{s}"] = {"hide": True}
        S[f"bat{s}"] = {"m": rot(-a * d, shx, 84)}           # positive a = cocked back (away from the table)
    if not stereo:
        S["table"] = {"hide": True}
        S["pR"] = {"hide": True}
        S["wall"] = {"m": tr(-dx, 0)}
    else:
        S["wall"] = {"hide": True}
    # ball
    xl, xr = ends(kv, stereo)
    y_end = BAT_Y if stereo else BAT_Y + 4
    pts = arch(xl, xr, BAT_Y, y_end, 6 + 36, 64)
    if stereo and hitter == "R":
        pts = pts[::-1]
    if not stereo and n % 2 == 1:
        pts = pts[::-1]                                     # squash: back from the wall
    i = ph * 64
    i0 = min(int(i), 63)
    fr = i - i0
    bx = pts[i0][0] + (pts[i0 + 1][0] - pts[i0][0]) * fr
    by = pts[i0][1] + (pts[i0 + 1][1] - pts[i0][1]) * fr
    tn = kv["tone"]
    S["ball"] = {"m": tr(bx, by)}
    S["ballbody"] = {"fill": "grey" if tn < -0.5 else "purplegrey" if tn < -0.1 else "white"}
    if tn >= -0.1:
        S["ballshade"] = {"hide": True}
    else:
        S["ballglint"] = {"hide": True}
    if tn > 0.1:
        sc = 0.35 + 0.65 * tn
        S["glow"] = {"m": mul(rot(t * 360, 0, 0), scale_about(sc, sc, 0, 0))}
    else:
        S["glow"] = {"hide": True}
    h = (BAT_Y - by) / 50
    sy = TY if stereo else FLOOR - 1
    S["shadow"] = {"m": mul(tr(bx, sy), scale_about(max(0.3, 1 - 0.5 * h), 1, 0, 0))}
    lit = round(NPIP * kv["mix"])
    for j in range(NPIP):
        c = "orange" if j < lit else "offred"
        S[f"pip{j + 1}"] = {"fill": c, "stroke": c}
    return S, kv, hitter, since_hit


class Overlay:
    def __init__(self):
        self.e, self.aa = [], 0

    def aa_line(self, x0, y0, x1, y1, col, w=1.5):
        self.aa += 1
        self.e.append(f'<line x1="{x0:.2f}" y1="{y0:.2f}" x2="{x1:.2f}" y2="{y1:.2f}" stroke="{C[col]}" stroke-width="{w}" stroke-linecap="round"/>')

    def fast(self, x0, y0, x1, y1, col):
        self.e.append(f'<line x1="{x0:.2f}" y1="{y0:.2f}" x2="{x1:.2f}" y2="{y1:.2f}" stroke="{C[col]}" stroke-width="1" stroke-linecap="square"/>')

    def poly(self, pts, col, w=None):
        for a, b in zip(pts, pts[1:]):
            if w is None:
                self.fast(*a, *b, col)
            else:
                self.aa_line(*a, *b, col, w)


def overlay(k, kv, hitter, since_hit):
    o = Overlay()
    stereo = k["stereo"]
    xl, xr = ends(kv, stereo)
    y_end = BAT_Y if stereo else BAT_Y + 4
    lv = echo_levels(kv["g"])
    for j in reversed(range(len(lv))):                  # oldest first so the newest sits on top
        a = lv[j]
        h = 6 + 36 * a
        if j == 0:
            o.poly(arch(xl, xr, BAT_Y, y_end, h, 14), "lightgreen", 2)
        elif j <= 5:
            o.poly(arch(xl, xr, BAT_Y, y_end, h, 8), "green", 1.2)
        else:
            o.poly(arch(xl, xr, BAT_Y, y_end, h, 16), "darkgreen")
    if since_hit < 0.12:                                # swoosh beside the hitter's bat
        bx = xl if hitter == "L" else xr
        side = 1 if hitter == "L" else -1              # in front of the bat, toward the table, below the flight
        for r in (10, 13, 16):
            pts = [(bx + side * r * math.cos(a), BAT_Y + r * math.sin(a))
                   for a in [math.radians(v) for v in range(10, 71, 20)]]
            o.poly(pts, "lightblue")
    return o


def composite(name, k, png=True, w=960):
    S, kv, hitter, sh = runtime_state(k)
    o = overlay(k, kv, hitter, sh)
    body = render(asset(), S)
    body = body.split(">", 1)[1].rsplit("</svg>", 1)[0]
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160">\n'
           f'<rect width="320" height="160" fill="#000000"/>\n{body}\n<g id="overlay">\n' + "\n".join(o.e) +
           f'\n</g>\n<!-- hidden panel area y > 140 -->\n<rect x="0" y="141" width="320" height="19" fill="#000000" opacity="0.55"/>\n</svg>\n')
    p = os.path.join(OUT, name + ".svg")
    open(p, "w").write(svg)
    if png:
        subprocess.run([RSVG, "-w", str(w), p, "-o", os.path.join(OUT, name + ".png")], check=True)
    return o.aa, count(asset(), S)


STATES = [
    ("default", dict(time=22000, fb=14000, tone=12000, mix=10000, stereo=True, t=0.10),
     "defaults on the master FX: 326 ms (mid table), feedback 40 % (3 arches), slightly dark (purple-grey ball), 30 % wet (3 of 11 pips)"),
    ("short_highfb", dict(time=6000, fb=29000, tone=16384, mix=18000, stereo=True, t=0.31),
     "43 ms, feedback 84 %: short table, players close, a dense fan of 8 arches, flat white ball, 55 % wet"),
    ("long_bright_wet", dict(time=31000, fb=18000, tone=31000, mix=30000, stereo=True, t=1.30),
     "1.02 s, feedback 52 %, bright: full-length table, sparkling ball, 92 % wet (10 of 11 pips)"),
    ("mix0_dry", dict(time=22000, fb=20000, tone=4000, mix=0, stereo=True, t=0.42),
     "mix 0 (dry): scoreboard dark, rally still shown (the loop runs, silent); dark tone = grey heavy ball"),
    ("mono_squash", dict(time=24000, fb=22000, tone=20000, mix=16000, stereo=False, t=0.05),
     "synth/drum FX (mono): SQUASH - one player vs the front wall, 410 ms, feedback 64 %, just after a hit"),
]

if __name__ == "__main__":
    a = asset()
    xml = render(a)
    open(os.path.join(OUT, "pingpong.svg"), "w").write('<?xml version="1.0" encoding="utf-8"?>\n<!--' + HEADER.replace("--", "-") + '-->\n' + xml)
    strictify.strictify_file(os.path.join(OUT, "pingpong.svg"), ("barrier_seams", "netmesh"))   # one subpath per element
    info = {}
    for name, k, cap in STATES:
        info[name] = composite(f"hq_{name}", k)
    # GIF: two flights at long+bright (1.02 s each), 60 ms frames
    gk = dict(STATES[2][1])
    kv = knobs(gk)
    T = kv["d"] * math.ceil(0.25 / kv["d"])
    nfr = int(round(2 * T / 0.06))
    fr = os.path.join(OUT, "frames")
    os.makedirs(fr, exist_ok=True)
    for j in range(nfr):
        composite(f"frames/frame_{j:02d}", dict(gk, t=j * 0.06), w=640)
    subprocess.run([MAGICK, "-delay", "6", "-loop", "0"] + [os.path.join(fr, f"frame_{j:02d}.png") for j in range(nfr)]
                   + [os.path.join(OUT, "pingpong.gif")], check=True)
    idx = [dict(concept="hq", title="Table Tennis (HQ hybrid)",
                summary="Two cute filled-SVG players rally across a table; the table length is the delay time, the green echo arches (our overlay) shrink with feedback, the ball's look is the tone and a hanging 11-pip scoreboard lights orange with the mix. Mono (synth/drum FX) turns into squash against a brick wall.",
                route="mixed",
                knobs=dict(blue="table length = delay time: tabletop scales in x, legs and both players (blue / light-blue shirts, blue bats) translate in/out; the ball's flight period follows the delay",
                           green="echo arches (overlay): one arch per echo above -28 dB, peak height by level g^k, newest lightgreen 2 px, then green, oldest darkgreen 1-px; low feedback = 1-2 arches, high = a dense fan of 8",
                           white="the ball: dark = grey ball with a shade crescent, slightly dark = purple-grey, flat = white with a glint, bright = rotating white star glow growing with brightness",
                           orange="hanging scoreboard, game to 11: round(11 x mix) pips light orange from the right; all dark at mix 0"),
                animation="ball translates along the newest arch once per flight (T = delay, or a multiple of it >= 0.25 s so 20-60 ms delays stay readable), alternating L->R / R->L (squash: to the wall and back); hitter's bat swings through and mouth opens for 0.12 s with a lightblue swoosh; receiver cocks back as the ball arrives; shadow tracks the ball; bright glow spins",
                cost=f"SVG: {count(a)} elements in the asset ({info['default'][1]} visible stereo, {info['mono_squash'][1]} mono), ~8 matrices + 13 recolours per frame (pips only change with the knob). Overlay: {max(v[0] for v in info.values())} AA segments max (arches) + 1-px old arches/swoosh; no text",
                states=[dict(file=f"hq/hq_{n}.svg", png=f"hq/hq_{n}.png", caption=c) for n, _, c in STATES]
                + [dict(file="hq/pingpong.gif", caption=f"animated: two flights at long + bright (1.02 s each), {nfr} frames of 60 ms")])]
    json.dump(idx, open(os.path.join(OUT, "index.json"), "w"), indent=2)
    print(info, count(a), nfr)
