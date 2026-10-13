#!/usr/bin/env python3
"""The Choir, HQ hybrid: choir.svg (the shippable asset, TE retained-renderer subset) + composite mock frames
(asset with runtime matrices/visibility/recolours applied + our immediate-mode line overlay) + an animated GIF."""
import math, os, re, subprocess, json

OUT = os.path.dirname(os.path.abspath(__file__))
FONT_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "slot", "font.h")
RSVG = "/opt/homebrew/bin/rsvg-convert"
MAGICK = "/opt/homebrew/bin/magick"

C = dict(green="#00ED95", lightgreen="#80FFAF", blue="#698EFF", lightblue="#BFD8FF", white="#FFFFFF",
         orange="#FF423F", lightorange="#FFB3B3", grey="#7C7990", darkblue="#383572", purplegrey="#AEB1DC",
         lpg="#E6E7FF", purple="#5F4270", offgreen="#002826", darkgreen="#00475B", offblue="#1D2040",
         offgrey="#353238", offred="#4E2832", red="#FF3A5D", pink="#FF90A7", black="#010000")
# note: colors.svg's black is #010000; use it so every colour comes from the palette file

BASE = 124                 # floor line: the singers' feet / sway pivot
XS = {1: 160, 2: 90, 3: 230}  # 1 = lead (blue), 2/3 = ensemble singers (white)
HEAD_Y = BASE - 52         # head centre
NECK_Y = BASE - 40
MAXDEG = 9.0               # sway at full depth (degrees)


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


# ------------------------------------------------------------------ tiny node tree
class N:
    def __init__(self, tag, id=None, children=None, **a):
        self.tag, self.id, self.ch, self.a = tag, id, children or [], a


def path_ellipse(cx, cy, rx, ry, rot=0.0):
    """Ellipse as 4 cubic Beziers (no A arcs in the subset), optionally rotated."""
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
    """'M x y L x y ...' from points, or pass strings through."""
    out = []
    for i, p in enumerate(pts):
        if isinstance(p, str):
            out.append(p)
        else:
            out.append(("M" if i == 0 else "L") + f"{f(p[0])} {f(p[1])}")
    return " ".join(out) + (" Z" if close else "")


def robe_d(cx, b):
    return (f"M{f(cx - 21)} {f(b)} C{f(cx - 18)} {f(b - 16)} {f(cx - 15)} {f(b - 30)} {f(cx - 11.5)} {f(b - 35.5)} "
            f"C{f(cx - 8)} {f(b - 39.5)} {f(cx + 8)} {f(b - 39.5)} {f(cx + 11.5)} {f(b - 35.5)} "
            f"C{f(cx + 15)} {f(b - 30)} {f(cx + 18)} {f(b - 16)} {f(cx + 21)} {f(b)} "
            f"C{f(cx + 8)} {f(b + 2)} {f(cx - 8)} {f(b + 2)} {f(cx - 21)} {f(b)} Z")


def hair_d(i, cx, hy):
    cap = (f"M{f(cx - 13.6)} {f(hy + 3)} C{f(cx - 16.5)} {f(hy - 18)} {f(cx + 16.5)} {f(hy - 18)} {f(cx + 13.6)} {f(hy + 3)} "
           f"C{f(cx + 12)} {f(hy - 4)} {f(cx + 6)} {f(hy - 7.5)} {f(cx - 1)} {f(hy - 6)} "
           f"C{f(cx - 6)} {f(hy - 5)} {f(cx - 11)} {f(hy - 2)} {f(cx - 13.6)} {f(hy + 3)} Z")
    if i == 1:     # lead: bob with a swept fringe + side flicks
        return cap + (f" M{f(cx - 13.6)} {f(hy + 3)} C{f(cx - 15)} {f(hy + 8)} {f(cx - 17)} {f(hy + 10)} {f(cx - 19)} {f(hy + 10)} "
                      f"C{f(cx - 16)} {f(hy + 7)} {f(cx - 15.5)} {f(hy + 2)} {f(cx - 13.6)} {f(hy - 2)} Z"
                      f" M{f(cx + 13.6)} {f(hy + 3)} C{f(cx + 15)} {f(hy + 8)} {f(cx + 17)} {f(hy + 10)} {f(cx + 19)} {f(hy + 10)} "
                      f"C{f(cx + 16)} {f(hy + 7)} {f(cx + 15.5)} {f(hy + 2)} {f(cx + 13.6)} {f(hy - 2)} Z")
    if i == 2:     # top bun
        return cap + " " + path_ellipse(cx + 1, hy - 15.5, 5.2, 4.6)
    return cap + (f" M{f(cx - 8)} {f(hy - 10)} C{f(cx - 6)} {f(hy - 20)} {f(cx + 4)} {f(hy - 24)} {f(cx + 12)} {f(hy - 20)} "   # quiff
                  f"C{f(cx + 6)} {f(hy - 19)} {f(cx + 4)} {f(hy - 16)} {f(cx + 7)} {f(hy - 11)} Z")


def eyes_d(cx, hy):
    s = ""
    for ex in (-4.6, 4.6):
        x, y = cx + ex, hy + 0.5
        s += f"M{f(x - 2.3)} {f(y)} C{f(x - 1.4)} {f(y - 2.4)} {f(x + 1.4)} {f(y - 2.4)} {f(x + 2.3)} {f(y)} "
    return s.strip()


def cheeks_d(cx, hy):
    return path_ellipse(cx - 8, hy + 4.5, 2, 1.4) + " " + path_ellipse(cx + 8, hy + 4.5, 2, 1.4)


def halo_pts(cx, hy, n=16, r0=17.5, r1=20.5):
    pts = []
    for j in range(n):
        a = math.radians(-90 + j * 360 / n)
        r = r1 if j % 2 == 0 else r0
        pts.append(f"{f(cx + r * math.cos(a))},{f(hy + r * math.sin(a))}")
    return " ".join(pts)


def note_d(x, y, flag=True):
    s = path_ellipse(x, y, 3.3, 2.3, -22)
    s += f" M{f(x + 2.2)} {f(y - 0.8)} L{f(x + 3.6)} {f(y - 1.4)} L{f(x + 3.6)} {f(y - 15)} L{f(x + 2.2)} {f(y - 15)} Z"
    if flag:
        s += (f" M{f(x + 3.6)} {f(y - 15)} C{f(x + 4.5)} {f(y - 11)} {f(x + 9.5)} {f(y - 10)} {f(x + 8)} {f(y - 4)} "
              f"C{f(x + 7.5)} {f(y - 8)} {f(x + 5.5)} {f(y - 9)} {f(x + 3.6)} {f(y - 10)} Z")
    return s


NOTES = [(140, 52, True, "orange"), (188, 48, False, "lightorange"), (256, 46, False, "lightorange"),
         (206, 54, True, "orange"), (84, 50, True, "orange"), (232, 40, False, "lightorange")]


def ST(i, col):
    """Ensemble singers' recoloured shapes carry a stroke (same colour as the fill), because TE's stroke-style
    getter asserts on a node without one; the outline state then just recolours fill and stroke."""
    return {} if i == 1 else {"stroke": C[col], "stroke-width": "1.5", "stroke-linejoin": "round"}


def singer(i):
    cx = XS[i]
    col = "blue" if i == 1 else "white"
    hy = HEAD_Y
    face = [
        N("circle", f"face{i}", cx=f(cx), cy=f(hy), r="12.5", fill=C["lpg"], **ST(i, "lpg")),
        N("path", f"hair{i}", d=hair_d(i, cx, hy), fill=C[col], **ST(i, col)),
        N("path", f"eyes{i}", d=eyes_d(cx, hy), fill="none", stroke=C["black"], **{"stroke-width": "1.4", "stroke-linecap": "round"}),
        N("path", f"cheek{i}", d=cheeks_d(cx, hy), fill=C["pink"]),
        N("g", f"mouth{i}", [N("path", f"mo{i}", d=path_ellipse(cx, hy + 6.5, 2.9, 3.4), fill=C["orange"])]),
        N("path", f"shut{i}", d=f"M{f(cx - 2.8)} {f(hy + 6)} C{f(cx - 1.5)} {f(hy + 7.6)} {f(cx + 1.5)} {f(hy + 7.6)} {f(cx + 2.8)} {f(hy + 6)}",
          fill="none", stroke=C["orange"], **{"stroke-width": "1.4", "stroke-linecap": "round"}),
    ]
    if i != 1:
        sd = -1 if i == 2 else 1
        face.append(N("g", f"halo{i}", [N("path", f"spark{i}", d=sparkle(cx - sd * 17, hy - 13, 4.5) + " " +
                                          sparkle(cx + sd * 19, hy - 4, 3.2) + " " + sparkle(cx - sd * 20, hy + 6, 2.4),
                                          fill=C["green"])]))
    body = [
        N("path", f"robe{i}", d=robe_d(cx, BASE), fill=C[col], **ST(i, col)),
        N("path", f"collar{i}", d=P((cx - 6, BASE - 37.5), (cx, BASE - 30), (cx + 6, BASE - 37.5)), fill="none",
          stroke=C["black"], **{"stroke-width": "1.6", "stroke-linejoin": "round", "stroke-linecap": "round"}),
        N("path", f"book{i}", d=P((cx - 12, BASE - 24), (cx, BASE - 21), (cx + 12, BASE - 24), (cx + 12, BASE - 15),
                                  (cx, BASE - 12), (cx - 12, BASE - 15), close=True),
          fill=C["lpg"] if i == 1 else C["offgrey"], stroke=C["black"], **{"stroke-width": "1.2", "stroke-linejoin": "round"}),
        N("g", f"head{i}", face),
    ]
    return N("g", f"s{i}", body)


def reflection(i):
    """Glossy-floor reflection as scanline strips (no opacity/gradients needed): robe + head, squashed."""
    cx = XS[i]
    sq = 0.3
    kids = []
    def half(h):                       # half-width of the silhouette at height h above the feet
        if h <= 36:
            return 21 - 9.5 * h / 36
        if h <= 40:
            return 6
        dy = h - 52
        return math.sqrt(max(12.5 ** 2 - dy * dy, 0))
    for k in range(5):
        y0 = BASE + 2.6 + k * 2.8
        y1 = y0 + 1.4
        h0, h1 = (y0 - BASE) / sq, (y1 - BASE) / sq
        w0, w1 = half(h0), half(h1)
        if w0 < 1:
            continue
        kids.append(N("polygon", f"rf{i}{k}", points=f"{f(cx - w0)},{f(y0)} {f(cx + w0)},{f(y0)} {f(cx + w1)},{f(y1)} {f(cx - w1)},{f(y1)}",
                      fill=C["darkblue" if i == 1 else "offgrey"]))
    return N("g", f"r{i}", kids)


def sparkle(x, y, r):
    q = r * 0.28
    return (f"M{f(x)} {f(y - r)} L{f(x + q)} {f(y - q)} L{f(x + r)} {f(y)} L{f(x + q)} {f(y + q)} "
            f"L{f(x)} {f(y + r)} L{f(x - q)} {f(y + q)} L{f(x - r)} {f(y)} L{f(x - q)} {f(y - q)} Z")


def stage():
    L = []
    # back wall: three spotlight cones (dark, flat fills), one per singer spot
    for i in (2, 3, 1):
        cx = XS[i]
        L.append(N("polygon", f"cone{i}", points=f"{f(cx - 6)},2 {f(cx + 6)},2 {f(cx + 32)},{f(BASE)} {f(cx - 32)},{f(BASE)}",
                   fill=C["offblue"]))
    # floor
    L.append(N("line", "floor", x1="4", y1=f(BASE), x2="316", y2=f(BASE), stroke=C["purplegrey"], **{"stroke-width": "1.5"}))
    L.append(N("line", "lip", x1="4", y1=f(BASE + 3), x2="316", y2=f(BASE + 3), stroke=C["darkblue"], **{"stroke-width": "1"}))
    # curtains: tied-back drapes left and right (filled), pleats as dark strokes, a gold... (orange-ish) tie
    left = ("M0 0 L38 0 C30 22 22 44 20 62 C19 70 22 76 26 80 C18 82 14 96 14 124 L0 124 Z")
    right = ("M320 0 L282 0 C290 22 298 44 300 62 C301 70 298 76 294 80 C302 82 306 96 306 124 L320 124 Z")
    L.append(N("path", "curtainL", d=left, fill=C["offred"]))
    L.append(N("path", "curtainR", d=right, fill=C["offred"]))
    L.append(N("path", "pleats", d="M10 0 C12 30 10 55 13 70 M10 92 C9 104 8 114 8 124 M22 0 C20 28 16 50 17 66 "
                                   "M310 0 C308 30 310 55 307 70 M310 92 C311 104 312 114 312 124 M298 0 C300 28 304 50 303 66",
               fill="none", stroke=C["purple"], **{"stroke-width": "1.2", "stroke-linecap": "round"}))
    L.append(N("path", "ties", d="M14 72 C20 76 26 78 30 77 M306 72 C300 76 294 78 290 77", fill="none",
               stroke=C["pink"], **{"stroke-width": "2", "stroke-linecap": "round"}))
    return N("g", "stage", L)


def asset():
    kids = [stage()]
    kids += [reflection(i) for i in (2, 3, 1)]
    kids += [singer(i) for i in (2, 3, 1)]
    kids.append(N("g", "notes", [N("g", f"n{j + 1}", [N("path", f"nd{j + 1}", d=note_d(x, y, fl), fill=C[c])])
                                 for j, (x, y, fl, c) in enumerate(NOTES)]))
    return N("svg", None, kids, version="1.1", xmlns="http://www.w3.org/2000/svg", x="0", y="0", width="320", height="160",
             viewBox="0 0 320 160")


HEADER = """
choir.svg - "ensemble" FX screen (The Choir). Our own artwork. Drawn by TE's retained renderer, then our
immediate-mode overlay on top. All ids below are found once at load (find, assert=0) and cached.
Pivots: singer i stands at x = XS[i] (s1 160 lead, s2 90, s3 230) on the floor y = 124; neck at y = 84; head centre y = 72.
phase = live LFO phase (0..1), tri(p) = triangle -1..1, R channel = phase + 0.5.
  s1, s2, s3       matrix = rotate(theta_i) about (XS[i], 124), theta_i = depth * 9 deg * tri(phase + (i-1)/3)   [blue LFO, green depth]
  head1..head3     extra matrix = rotate(0.6 * theta_i) about (XS[i], 84) (the head swings a little further)
  s2, s3           hidden while ensemble = 0                                                                     [white]
  robe2/3 hair2/3  recolour fill+stroke: ensemble < 0.5 -> fill black, stroke grey/purplegrey (outline);
  face2/3          >= 0.5 -> fill purplegrey (< 0.75) / white; face2/3 fill lpg once filled, else black with stroke
  halo2, halo3     (3 green sparkles) shown with s2/s3 when depth > 0; matrix = rotate(+-20 deg, flipped at 6.5 Hz) * scale((0.6 + 0.9 depth) * 1 or 0.75) about the head centre  [green]
  cone2, cone3     shown with s2/s3
  mouth1..3        matrix = scale(1, 0.3 + 0.7 mix) about the mouth centre (XS[i], 78.5); hidden when mix < 0.05    [orange mix]
  shut1..3         shown only when mix < 0.05 (closed smile)
  n1..n6           shown for j < round(6 mix); matrix = translate(sin wobble, -34 * ((t * 0.6 + j/6) mod 1)) (notes rise)
  r1, r2, r3       master FX only (stereo); matrix = rotate(-theta_i(R)) about (XS[i], 124): reflections lean in anti-phase
                   (r2/r3 also follow s2/s3 visibility). Hidden for synth/drum FX (mono).
  everything else  static (stage: curtains, cones, floor).
Overlay (immediate mode, <= 4 AA segments): no text (FX screens carry no text); green motion marks beside the lead head on the side it moves toward (length by depth, 1..4 by rate, shown mid-swing only, none at depth 0);
darkblue floor X marks where hidden singers would stand.
"""


def render(node, state=None, ind=""):
    """state: id -> dict(hide, m, fill, stroke). Hidden nodes are skipped (as the renderer's early return)."""
    st = (state or {}).get(node.id, {}) if node.id else {}
    if st.get("hide"):
        return ""
    a = dict(node.a)
    if node.id:
        a = {"id": node.id, **a}
    if "fill" in st:
        a["fill"] = C[st["fill"]] if st["fill"] != "none" else "none"
    if "stroke" in st:
        a["stroke"] = C[st["stroke"]]
        a.setdefault("stroke-width", st.get("sw", "1.5"))
    if "m" in st:
        a["transform"] = "matrix(" + " ".join(f"{v:.4f}" for v in st["m"]) + ")"
    attrs = " ".join(f'{k}="{v}"' for k, v in a.items())
    if not node.ch:
        return f"{ind}<{node.tag} {attrs}/>\n"
    inner = "".join(render(c, state, ind + "  ") for c in node.ch)
    return f"{ind}<{node.tag} {attrs}>\n{inner}{ind}</{node.tag}>\n"


# ------------------------------------------------------------------ runtime model
def tri(p):
    p %= 1.0
    return 4 * p - 1 if p < 0.5 else 3 - 4 * p


def rate_n(rate):
    return math.log(rate / 0.05) / math.log(256)


def rot(deg, px, py):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return (c, s, -s, c, px - c * px + s * py, py - s * px - c * py)


def mul(m1, m2):          # apply m2 then m1 (SVG matrix(a b c d e f))
    a1, b1, c1, d1, e1, f1 = m1
    a2, b2, c2, d2, e2, f2 = m2
    return (a1 * a2 + c1 * b2, b1 * a2 + d1 * b2, a1 * c2 + c1 * d2, b1 * c2 + d1 * d2,
            a1 * e2 + c1 * f2 + e1, b1 * e2 + d1 * f2 + f1)


def scale_about(sx, sy, px, py):
    return (sx, 0, 0, sy, px - sx * px, py - sy * py)


def runtime_state(k):
    rate, depth, ens, mix, ph, t, stereo = k["rate"], k["depth"], k["ens"], k["mix"], k["ph"], k["t"], k["stereo"]
    S = {}
    theta = {i: depth * MAXDEG * tri(ph + (i - 1) / 3) for i in (1, 2, 3)}
    for i in (1, 2, 3):
        cx = XS[i]
        S[f"s{i}"] = {"m": rot(theta[i], cx, BASE)}
        S[f"head{i}"] = {"m": rot(0.6 * theta[i], cx, NECK_Y)}
        if mix < 0.05:
            S[f"mouth{i}"] = {"hide": True}
        else:
            S[f"mouth{i}"] = {"m": scale_about(1, 0.3 + 0.7 * mix, cx, HEAD_Y + 6.5)}
            S[f"shut{i}"] = {"hide": True}
        if stereo:
            thR = depth * MAXDEG * tri(ph + 0.5 + (i - 1) / 3)
            S[f"r{i}"] = {"m": rot(-thR, cx, BASE)}
        else:
            S[f"r{i}"] = {"hide": True}
    for i in (2, 3):
        if ens <= 0:
            for idn in (f"s{i}", f"cone{i}", f"r{i}"):
                S[idn] = {"hide": True}
            continue
        if ens < 0.5:
            oc = "grey" if ens < 0.25 else "purplegrey"
            for part in ("robe", "hair", "face"):
                S[f"{part}{i}"] = {"fill": "black", "stroke": oc, "sw": "1.5"}
            S[f"eyes{i}"] = {"stroke": oc, "sw": "1.4"}
            S[f"cheek{i}"] = {"hide": True}
            S[f"book{i}"] = {"fill": "black", "stroke": oc, "sw": "1.2"}
            S[f"collar{i}"] = {"stroke": oc, "sw": "1.4"}
        else:
            fc = "purplegrey" if ens < 0.75 else "white"
            S[f"robe{i}"] = {"fill": fc, "stroke": fc}
            S[f"hair{i}"] = {"fill": fc, "stroke": fc}
        if depth > 0.02:
            flip = 1 if int(t * 6.5 * 2) % 2 == 0 else -1
            hy = HEAD_Y
            sc = (0.6 + 0.9 * depth) * (1.0 if flip > 0 else 0.75)
            S[f"halo{i}"] = {"m": mul(rot(flip * 20 * (1 if i == 2 else -1), XS[i], hy), scale_about(sc, sc, XS[i], hy))}
        else:
            S[f"halo{i}"] = {"hide": True}
    nn = round(6 * mix)
    for j in range(6):
        if j >= nn:
            S[f"n{j + 1}"] = {"hide": True}
        else:
            u = (t * 0.6 + j / 6) % 1.0
            S[f"n{j + 1}"] = {"m": (1, 0, 0, 1, 3 * math.sin(2 * math.pi * (u * 1.5 + j * 0.3)), -34 * u)}
    return S, theta


# ------------------------------------------------------------------ overlay (our immediate-mode lines)
def load_font():
    src = open(FONT_H).read()
    off = [int(v) for v in re.search(r"font_off\[\d+\] = \{(.*?)\}", src, re.S).group(1).replace("\n", "").split(",") if v.strip()]
    seg = [int(v, 16) for v in re.search(r"font_seg\[\d+\] = \{(.*?)\}", src, re.S).group(1).replace("\n", "").split(",") if v.strip()]
    return off, seg


FONT = load_font()


class Overlay:
    def __init__(self):
        self.e, self.aa = [], 0

    def aa_line(self, x0, y0, x1, y1, col, w=1.5):
        self.aa += 1
        self.e.append(f'<line x1="{x0:.2f}" y1="{y0:.2f}" x2="{x1:.2f}" y2="{y1:.2f}" stroke="{C[col]}" stroke-width="{w}" stroke-linecap="round"/>')

    def fast(self, x0, y0, x1, y1, col):      # 1-px framebuffer line
        self.e.append(f'<line x1="{x0:.2f}" y1="{y0:.2f}" x2="{x1:.2f}" y2="{y1:.2f}" stroke="{C[col]}" stroke-width="1" stroke-linecap="square"/>')

    def text(self, s, x, y, step, col):
        off, seg = FONT
        for ch in s:
            c = ord(ch)
            if 0x20 <= c <= 0x5A:
                i = off[c - 0x20]
                while seg[i] != 0xFF:
                    a, b = seg[i], seg[i + 1]
                    self.e.append(f'<line x1="{x + (a >> 4) * step:.2f}" y1="{y + (a & 15) * step:.2f}" x2="{x + (b >> 4) * step:.2f}" '
                                  f'y2="{y + (b & 15) * step:.2f}" stroke="{C[col]}" stroke-width="{max(step / 4, 1) * 1.0:.2f}" stroke-linecap="square"/>')
                    i += 2
            x += 6 * step


def overlay(k, theta):
    o = Overlay()
    rate, depth, ens, ph = k["rate"], k["depth"], k["ens"], k["ph"]
    # green motion marks beside the lead head, on the side it is moving toward: length grows with depth,
    # count with rate; shown only mid-swing (|tri| < 0.75), so they blink in time with the sway; none at depth 0
    cx = XS[1]
    th = theta[1]
    hx = cx + (BASE - HEAD_Y) * math.sin(math.radians(th))
    hy = HEAD_Y
    p = ph % 1
    side = 1 if p < 0.5 else -1                  # tri rises for p < 0.5: moving right
    if depth > 0.02 and abs(tri(p)) < 0.75:
        nl = round(1 + 3 * rate_n(rate))
        ln = 3 + 11 * depth
        for j in range(nl):
            yy = hy - 5 + j * 4.5
            x0 = hx + side * (18 + (j % 2) * 2)
            o.aa_line(x0, yy, x0 + side * ln * (1 if j % 2 == 0 else 0.7), yy, "green", 1.5)
    # empty spots: floor X marks
    if ens <= 0:
        for i in (2, 3):
            x = XS[i]
            o.fast(x - 5, BASE - 3, x + 5, BASE - 1, "darkblue")
            o.fast(x - 5, BASE - 1, x + 5, BASE - 3, "darkblue")
            o.fast(x - 18, BASE - 2, x - 10, BASE - 2, "darkblue")
            o.fast(x + 10, BASE - 2, x + 18, BASE - 2, "darkblue")
    return o


def composite(name, k, png=True, w=960):
    S, theta = runtime_state(k)
    o = overlay(k, theta)
    body = render(asset(), S)
    body = body.split(">", 1)[1].rsplit("</svg>", 1)[0]
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160">\n'
           f'<rect width="320" height="160" fill="#000000"/>\n{body}\n<g id="overlay">\n' + "\n".join(o.e) +
           f'\n</g>\n<!-- hidden panel area y > 140 -->\n<rect x="0" y="141" width="320" height="19" fill="#000000" opacity="0.55"/>\n</svg>\n')
    p = os.path.join(OUT, name + ".svg")
    open(p, "w").write(svg)
    if png:
        subprocess.run([RSVG, "-w", str(w), p, "-o", os.path.join(OUT, name + ".png")], check=True)
    return o.aa


def count(node, S=None):
    n = 0 if node.tag in ("svg", "g") else 1
    if S and node.id and S.get(node.id, {}).get("hide"):
        return 0
    return n + sum(count(c, S) for c in node.ch)


STATES = [
    ("default", dict(rate=0.5, depth=0.42, ens=0.0, mix=0.5, ph=0.15, t=0.1, stereo=True),
     "defaults on the master FX (Juno I chorus): rate 0.5 Hz, depth 42%, ensemble 0 (soloist, empty spots marked), mix 50%"),
    ("fast_deep", dict(rate=9.0, depth=1.0, ens=0.3, mix=0.8, ph=0.62, t=0.4, stereo=True),
     "rate 9 Hz, depth 100% (big lean), ensemble 30% (singers step in as outlines), mix 80%"),
    ("ensemble_max", dict(rate=0.83, depth=0.6, ens=1.0, mix=1.0, ph=0.40, t=0.3, stereo=True),
     "master, ensemble 100%: three filled singers 120 deg apart with 6.5 Hz shimmer halos, all six notes, anti-phase floor reflections"),
    ("mix0_dry", dict(rate=0.83, depth=0.6, ens=1.0, mix=0.0, ph=0.40, t=0.3, stereo=True),
     "mix 0 (dry): mouths shut, no notes; everything else as ensemble max"),
    ("synth_mono", dict(rate=2.0, depth=0.7, ens=0.6, mix=0.5, ph=0.80, t=0.7, stereo=False),
     "as a synth FX (mono): no reflections; rate 2 Hz, depth 70%, ensemble 60% (filled purple-grey), mix 50%"),
]

if __name__ == "__main__":
    a = asset()
    xml = render(a)
    open(os.path.join(OUT, "choir.svg"), "w").write('<?xml version="1.0" encoding="utf-8"?>\n<!--' + HEADER.replace("--", "-") + '-->\n' + xml)
    info = {}
    for name, k, cap in STATES:
        aa = composite(f"hq_{name}", k)
        S, _ = runtime_state(k)
        info[name] = (count(a, S), aa)
    # GIF: one LFO cycle at ensemble max (0.83 Hz -> 1.2 s), 24 frames of 50 ms
    gk = dict(STATES[2][1])
    fr = os.path.join(OUT, "frames")
    os.makedirs(fr, exist_ok=True)
    for j in range(24):
        t = j * 0.05
        k = dict(gk, ph=(t * gk["rate"]) % 1.0, t=t)
        composite(f"frames/frame_{j:02d}", k, w=640)
    subprocess.run([MAGICK, "-delay", "5", "-loop", "0"] + [os.path.join(fr, f"frame_{j:02d}.png") for j in range(24)]
                   + [os.path.join(OUT, "choir.gif")], check=True)
    idx = [dict(concept="hq", title="The Choir (HQ hybrid)",
                summary="A tiny choir on a curtained stage: filled SVG singers lean about their feet with the LFO, two more step in (hidden, outline, filled) with ensemble and shimmer at 6.5 Hz, mouths open and notes rise with mix; on the master FX the glossy floor reflects them in anti-phase.",
                route="mixed",
                knobs=dict(blue="lead singer (blue robe + hair) + sway rate: s1 rotates about the feet with the live LFO; the green motion marks number 1..4 by rate and blink mid-swing",
                           green="how far they sway: lean (0 to +-9 deg) + green motion marks beside the lead (length by depth) and green stars around the backing singers (size by depth, flicker at 6.5 Hz); none at depth 0",
                           white="backing singers s2/s3: hidden (floor X marks) -> grey/purple-grey outline (< 50%) -> filled purple-grey/white",
                           orange="mouth groups scale open with mix (closed smile at 0); 0..6 notes rise from the choir"),
                animation="s_i rotate(depth*9deg*tri(phase+i/3)) about the feet, heads swing 0.6x further about the neck; halos rotate +-11 deg at 6.5 Hz; notes translate upward on a 1.7 s loop with a sine wobble; master: r_i rotate with the R channel's anti-phase",
                cost=f"SVG: {count(a)} elements in the asset ({info['ensemble_max'][0]} visible at ensemble max, {info['default'][0]} at defaults), ~25 matrices + a few recolours per frame. Overlay: {max(v[1] for v in info.values())} AA segments max (green motion marks only) + 1-px X marks; no text",
                states=[dict(file=f"hq/hq_{n}.svg", png=f"hq/hq_{n}.png", caption=c) for n, _, c in STATES]
                + [dict(file="hq/choir.gif", caption="animated: one LFO cycle (1.2 s) at ensemble max, 24 frames")])]
    json.dump(idx, open(os.path.join(OUT, "index.json"), "w"), indent=2)
    print(info, count(a))
