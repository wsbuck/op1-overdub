#!/usr/bin/env python3
"""crush.svg: the "crush" bitcrusher FX screen (a pyramid that falls apart as the crusher gets heavier), plus preview
frames: the asset with the runtime's matrices / visibility / recolours applied, rasterised by rsvg-convert.

  python3 design/fx/gen_crush.py [PREVIEW_DIR]      writes design/fx/crush.svg; previews (PNG, GIF, index.json) if a dir is given

Knobs (0..1): r = blue RATE, b = green BITS, s = white SQUASH, o = orange GRIT. Live inputs: gr = the compressor's gain
reduction 0..1 (pumping), t = frame clock (s). runtime_state() is the reference for the C side."""
import json, math, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
RSVG = "/opt/homebrew/bin/rsvg-convert"
MAGICK = "/opt/homebrew/bin/magick"

C = dict(green="#00ED95", blue="#698EFF", lightblue="#BFD8FF", white="#FFFFFF", orange="#FF423F", lightorange="#FFB3B3",
         grey="#7C7990", darkblue="#383572", purplegrey="#AEB1DC", lpg="#E6E7FF", offblue="#1D2040", offgrey="#353238",
         black="#010000", darkgreen="#00475B", offgreen="#002826")

# ---- geometry (SVG units; safe area x 4..316, y 2..140)
CX, BASE, APEX = 160, 126, 22
H = BASE - APEX                      # 104
HB = 82                              # half base: slope 104/82 = 51.7 deg (Giza)
RIDGE = 175                          # the ridge runs from the apex to the front corner (RIDGE, BASE): lit face left
ROWS = 16                            # course grid: 16 rows of 6.5 (16 bits)
RY = H / ROWS
RATE_P = [6, 11, 18, 28]             # RATE pixel (column) widths, levels 1..4
SLICES = [(44, 53), (71, 79), (98, 108)]
DEBRIS = [(125, 66, 1), (215, 92, -1), (139, 48, 1), (183, 52, -1), (199, 72, -1), (218, 96, -1)]   # chip spots; side 1 = left edge (falls left)
MU = 15


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def xl(y):
    return CX - HB + (BASE - y) * HB / H


def xr(y):
    return CX + HB - (BASE - y) * HB / H


def top(x):
    """Triangle top y at x."""
    return APEX + abs(x - CX) * H / HB


def ridge_x(y):
    return CX + (RIDGE - CX) * (y - APEX) / H


def pd(P, close=True):
    return "M" + " L".join(f"{f(x)} {f(y)}" for x, y in P) + (" Z" if close else "")


# ---- unique colour values for every recoloured element (TE may share a style entry between equal colours)
_used = {}
_R = (-2, -1, 0, 1, 2)
_OFFS = sorted(((a, b, c) for a in _R for b in _R for c in _R if (a, b, c) != (0, 0, 0)),
               key=lambda o: (max(map(abs, o)), sum(map(abs, o)), [-v for v in o]))   # 124 neighbours, +-2 per channel


def uniq(name):
    base = C[name]
    n = _used.get(name, 0)
    _used[name] = n + 1
    if n == 0:
        return base
    rgb = [int(base[j:j + 2], 16) for j in (1, 3, 5)]
    offs = _OFFS[(n - 1) % len(_OFFS)]
    return "#" + "".join(f"{min(255, max(0, c + o)):02X}" for c, o in zip(rgb, offs))


class N:
    def __init__(self, tag, id=None, children=None, **a):
        self.tag, self.id, self.ch = tag, id, children or []
        self.a = {k.replace("_", "-"): v for k, v in a.items()}


def te_safe_d(d, stroked):
    """Path data TE's parser draws as written (notes/svg-route.md "Path quirks"): absolute M, L and Z only (H / V become
    L), and, in a stroked path with several subpaths, every open subpath a single segment: TE closes each open subpath
    with a line back to its start when the next M comes, which retraces a single segment but chords a polyline."""
    toks = re.findall(r"[MLHVZmlhvz]|-?\d*\.?\d+", d)
    subs, cur, x, y, cmd, i = [], None, 0.0, 0.0, "M", 0
    while i < len(toks):
        t = toks[i]
        if t in "MLHVZ":
            cmd, i = t, i + 1
            if t == "Z":
                if cur:
                    subs.append((cur, True))
                    x, y = cur[0]
                cur = None
            continue
        if t.isalpha():
            raise ValueError(f"relative command {t} in {d[:40]}")
        if cmd == "M":
            if cur:
                subs.append((cur, False))
            x, y = float(toks[i]), float(toks[i + 1])
            cur, i, cmd = [(x, y)], i + 2, "L"
        elif cmd == "L":
            x, y = float(toks[i]), float(toks[i + 1])
            cur.append((x, y))
            i += 2
        elif cmd == "H":
            x = float(t)
            cur.append((x, y))
            i += 1
        else:
            y = float(t)
            cur.append((x, y))
            i += 1
    if cur:
        subs.append((cur, False))
    if stroked and len(subs) > 1:
        split = []
        for pts, closed in subs:
            if closed or len(pts) <= 2:
                split.append((pts, closed))
            else:
                split += [([p, q], False) for p, q in zip(pts, pts[1:])]
        subs = split
    return " ".join("M" + " L".join(f"{f(px)} {f(py)}" for px, py in pts) + (" Z" if closed else "") for pts, closed in subs)


def reuniq(v):
    """The next unique value of the palette colour v is (or v itself if it is not a palette colour)."""
    if not (isinstance(v, str) and v.startswith("#")):
        return v
    rgb = [int(v[j:j + 2], 16) for j in (1, 3, 5)]
    for name, base in C.items():
        q = [int(base[j:j + 2], 16) for j in (1, 3, 5)]
        if all(abs(a - b) <= 1 for a, b in zip(rgb, q)):
            return uniq(name)
    return v


def path(id, d, **a):
    """One element per subpath (TE's parser closes or joins subpaths, notes/svg-route.md "Path quirks"): a d with
    several subpaths becomes a group id of paths id_1, id_2, .. (each with its own unique colour value)."""
    stroked = a.get("stroke", "none") != "none"
    subs = [x.strip() for x in re.split(r"(?=M)", te_safe_d(d, False)) if x.strip()]
    if len(subs) <= 1:
        return N("path", id, d=subs[0] if subs else "", **a)
    kids = []
    for j, sp in enumerate(subs):
        aa = dict(a) if j == 0 else {k: (reuniq(v) if k in ("fill", "stroke") else v) for k, v in a.items()}
        kids.append(N("path", f"{id}_{j + 1}" if id else None, d=sp, **aa))
    return N("g", id, kids)


# ------------------------------------------------------------------ geometry builders
def stair(p):
    """Columns of width p centred on the apex, each held at the profile height at its OUTER edge (zero-order hold):
    the stair lies inside the triangle. Returns column tops [(x0, x1, y)] left to right."""
    K = int(math.ceil((HB / p) - 0.5)) + 1
    cols = []
    for k in range(-K, K + 1):
        x0, x1 = CX + (k - 0.5) * p, CX + (k + 0.5) * p
        outer = x0 if k < 0 else x1
        y = top(outer) if k != 0 else top(x1)
        if y >= BASE - 0.5:
            continue
        cols.append((max(x0, CX - HB), min(x1, CX + HB), y))
    return cols


def stair_line(cols):
    P = [(cols[0][0], BASE)]
    for x0, x1, y in cols:
        P += [(x0, y), (x1, y)]
    P.append((cols[-1][1], BASE))
    return P


def rate_level(i, p):
    cols = stair(p)
    sl = stair_line(cols)
    mask = [(CX - HB - 1.5, BASE), (CX, APEX - 1.8), (CX + HB + 1.5, BASE)] + sl[::-1]   # 1.5 past the edges: no AA seam
    joints = " ".join(f"M{f(cols[j][1])} {f(max(cols[j][2], cols[j + 1][2]))} V{f(BASE)}" for j in range(len(cols) - 1))
    return N("g", f"rate{i}", [
        path(f"mask{i}", pd(mask), fill=C["black"]),
        path(f"joint{i}", joints, fill="none", stroke=uniq("darkblue"), stroke_width="0.8"),
        path(f"step{i}", pd(sl, False), fill="none", stroke=C["blue"], stroke_width="1.2", stroke_linejoin="miter",
             stroke_linecap="square")])


def course_d(rows):
    d = []
    for i in rows:
        y = BASE - i * RY
        d.append(f"M{f(xl(y) + 0.6)} {f(y)} H{f(xr(y) - 0.6)}")
    return " ".join(d)


def mu_d():
    d = []
    for k in range(1, 8):
        y = BASE - H * ((1 + MU) ** (k / 8) - 1) / MU
        d.append(f"M{f(xl(y) + 0.6)} {f(y)} H{f(xr(y) - 0.6)}")
    return " ".join(d)


def stone(row, x0, w):
    """A missing stone: one course row high, w wide, clipped to the faces."""
    y1 = BASE - row * RY
    y0 = y1 - RY
    a, b = max(x0, xl(y0) + 1.5), min(x0 + w, xr(y0) - 1.5)
    return pd([(a, y0), (b, y0), (b, y1), (a, y1)])


HOLES = [[(2, 104, 11), (6, 176, 10), (9, 140, 9)],
         [(1, 186, 12), (4, 118, 10), (7, 160, 8), (11, 150, 7), (3, 208, 9)],
         [(5, 128, 13), (8, 182, 9), (2, 140, 12), (10, 166, 7), (12, 156, 8), (0, 96, 14)]]


def cap(rows):
    """Steps collapsing: the top `rows` course rows gone, with a broken stepped edge. Returns (fill d, break-line d)."""
    y = BASE - (ROWS - rows) * RY
    w = xr(y) - xl(y)
    steps = [0, 0.55, 0.2, 1.0, 0.35, 0.8, 0.1]
    brk = []
    for j, s in enumerate(steps):
        brk += [(xl(y) + w * j / len(steps), y + s * RY), (xl(y) + w * (j + 1) / len(steps), y + s * RY)]
    brk[0] = (xl(y + steps[0] * RY), y + steps[0] * RY)
    brk[-1] = (xr(y + steps[-1] * RY), y + steps[-1] * RY)
    fill = [(xr(y) + 1.5, y)] + [(CX, APEX - 1.8), (xl(y) - 1.5, y)] + brk
    return pd(fill), pd(brk, False)


def pile():
    """Collapsed stones resting at the base, left and right."""
    d = []
    for x, y, w, h in [(64, 120, 9, 6), (73, 122, 7, 4), (56, 123, 7, 3), (247, 121, 8, 5), (255, 123, 9, 3), (240, 123, 6, 3)]:
        d.append(pd([(x, y), (x + w, y), (x + w, y + h), (x, y + h)]))
    return " ".join(d)


def fp(u, y):
    """A point on the faces: u = 0 at the left edge .. 1 at the right edge, at height y."""
    return (xl(y) + u * (xr(y) - xl(y)), y)


CRACKS = [   # (u, y) lists; first point on an edge, the apex or the base
    [(0.5, 22.6), (0.38, 30), (0.6, 37), (0.42, 44), (0.52, 50)],
    [(0.0, 60), (0.12, 62), (0.2, 59.5), (0.3, 65), (0.4, 64)],
    [(1.0, 82), (0.9, 85), (0.8, 83.5), (0.72, 89), (0.64, 88), (0.6, 93)],
    [(0.3, BASE - 0.5), (0.33, 119), (0.27, 113), (0.34, 107), (0.3, 101), (0.37, 96)],
    [(0.0, 106), (0.07, 103), (0.15, 106.5), (0.21, 102.5), (0.29, 105.5), (0.36, 101.5)],
    [(0.74, BASE - 0.5), (0.71, 119), (0.77, 114), (0.72, 109), (0.79, 104)],
]


def chip(y, side, d=4.5):
    """A V notch out of the left (side -1) or right (+1) edge at height y."""
    x = xl(y) if side < 0 else xr(y)
    ya, yb = y - d, y + d
    xa = xl(ya) if side < 0 else xr(ya)
    xb = xl(yb) if side < 0 else xr(yb)
    return pd([(xa - side * 0.4, ya), (x - side * d * 1.1, y), (xb - side * 0.4, yb), (xb + side * 2, yb), (xa + side * 2, ya)])


def strip(y0, y1, x_lo, x_hi):
    return [(x_lo(y0), y0), (x_hi(y0), y0), (x_hi(y1), y1), (x_lo(y1), y1)]


def slice_group(i, y0, y1):
    full = strip(y0, y1, xl, xr)
    left = strip(y0, y1, xl, ridge_x)
    right = strip(y0, y1, ridge_x, xr)
    return N("g", f"g{i}", [
        path(f"gx{i}", pd(full), fill=C["black"]),
        N("g", f"gs{i}", [
            path(f"gl{i}", pd(left), fill=uniq("grey")),
            path(f"gr{i}", pd(right), fill=uniq("purplegrey")),
            N("line", f"go{i}", x1=f(xl(y0)), y1=f(y0), x2=f(xr(y0)), y2=f(y0), stroke=C["orange"], stroke_width="1")])])


def debris(i, x, y, side):
    s = 2.2 + (i % 3) * 0.6
    return path(f"d{i}", pd([(x, y - s), (x + s, y), (x, y + s), (x - s, y)]), fill=C["orange"])


# ------------------------------------------------------------------ the asset
def asset():
    _used.clear()
    sky = N("g", "sky", [
        N("circle", "sun", cx="64", cy="42", r="15", fill=uniq("darkblue")),
        path("far", pd([(254, BASE), (275, 99), (296, BASE)]) + " " + pd([(286, BASE), (299, 109), (312, BASE)]),
             fill=uniq("offblue")),
        N("line", "horizon", x1="6", y1=f(BASE + 0.5), x2="314", y2=f(BASE + 0.5), stroke=uniq("grey"), stroke_width="1"),
        path("sand", "M18 133 H58 M92 137 H128 M226 136 H258 M282 132 H306", fill="none", stroke=uniq("offgrey"),
             stroke_width="1", stroke_linecap="round"),
    ])
    faceL = [(CX - HB, BASE), (CX, APEX), (RIDGE, BASE)]
    faceR = [(RIDGE, BASE), (CX, APEX), (CX + HB, BASE)]
    shadow = [(RIDGE + 4, BASE + 1), (CX + HB, BASE + 1), (CX + HB + 40, BASE + 9), (RIDGE + 22, BASE + 9)]
    pyr = N("g", "pyr", [
        path("shadow", pd(shadow), fill=uniq("offblue")),
        path("faceL", pd(faceL), fill=uniq("purplegrey"), stroke=uniq("purplegrey"), stroke_width="0.6", stroke_linejoin="round"),
        path("faceR", pd(faceR), fill=uniq("grey"), stroke=uniq("grey"), stroke_width="0.6", stroke_linejoin="round"),
        path("c1", course_d([8]), fill="none", stroke=uniq("darkblue"), stroke_width="0.8"),
        path("c2", course_d([4, 12]), fill="none", stroke=uniq("darkblue"), stroke_width="0.8"),
        path("c3", course_d([2, 6, 10, 14]), fill="none", stroke=uniq("darkblue"), stroke_width="0.8"),
        path("c4", course_d([1, 3, 5, 7, 9, 11, 13, 15]), fill="none", stroke=uniq("darkblue"), stroke_width="0.8"),
        path("cmu", mu_d(), fill="none", stroke=C["green"], stroke_width="0.8"),
    ] + [path(f"h{i + 1}", " ".join(stone(*s) for s in hs), fill=C["black"], stroke=C["green"], stroke_width="0.8",
              stroke_linejoin="miter") for i, hs in enumerate(HOLES)] + [
        path(f"k{i + 1}", pd(P, False), fill="none", stroke=C["orange"], stroke_width="1", stroke_linejoin="round",
             stroke_linecap="round") for i, P in enumerate([[fp(*q) for q in c] for c in CRACKS])] + [
        path("ch1", chip(66, -1) + " " + chip(92, 1), fill=C["black"]),
        path("ch2", chip(48, -1, 3.5) + " " + chip(52, 1, 3.5) + " " + chip(72, 1) + " " + chip(96, 1), fill=C["black"]),
    ] + [rate_level(i + 1, p) for i, p in enumerate(RATE_P)] + [
    ] + [N("g", f"cap{i}", [path(f"capf{i}", cap(n)[0], fill=C["black"]),
                             path(f"capb{i}", cap(n)[1], fill="none", stroke=C["green"], stroke_width="0.8")])
         for i, n in ((1, 2), (2, 4))] + [
        path("pile", pile(), fill=C["offgreen"], stroke=C["green"], stroke_width="0.8"),
    ] + [slice_group(i + 1, *s) for i, s in enumerate(SLICES)] + [debris(i + 1, *d) for i, d in enumerate(DEBRIS)])
    press = N("g", "press", [
        N("line", "stem", x1="160", y1="-60", x2="160", y2=f(APEX - 4), stroke=C["white"], stroke_width="2"),
        N("rect", "plate", x="128", y=f(APEX - 4), width="64", height="2.6", fill=C["white"]),
    ])
    puff = path("puff", "M70 124 L62 119 M68 127 L58 126 M250 124 L258 119 M252 127 L262 126", fill="none",
                stroke=C["white"], stroke_width="1", stroke_linecap="round")
    return N("svg", None, [sky, pyr, press, puff], version="1.1", xmlns="http://www.w3.org/2000/svg", x="0", y="0",
             width="320", height="160", viewBox="0 0 320 160")


def colour(v):
    """A palette name, or (name, brightness 0..1)."""
    if isinstance(v, tuple):
        rgb = [int(C[v[0]][j:j + 2], 16) for j in (1, 3, 5)]
        return "#" + "".join(f"{int(c * v[1]):02X}" for c in rgb)
    return C[v]


def render(n, S=None, ind=0, inherit=None):
    """inherit: a recolour set on a group whose parts are its leaves (split paths: id + a, b, ..)."""
    S = S or {}
    a = dict(n.a)
    rc = dict(inherit or {})
    if n.id:
        a = {"id": n.id, **a}
        st = S.get(n.id, {})
        if st.get("hide"):
            a["display"] = "none"          # preview only; the device clears the node's visible flag
        if "m" in st:
            a["transform"] = "matrix(" + " ".join(f"{v:.4f}" for v in st["m"]) + ")"
        rc.update({k: st[k] for k in ("fill", "stroke") if k in st})
    if n.tag != "g":
        for k, v in rc.items():
            if a.get(k, "none") != "none":
                a[k] = colour(v)
    attrs = " ".join(f'{k}="{v}"' for k, v in a.items())
    pad = "  " * ind
    if not n.ch:
        return f"{pad}<{n.tag} {attrs}/>\n"
    return f"{pad}<{n.tag} {attrs}>\n" + "".join(render(c, S, ind + 1, rc if n.tag == "g" else None) for c in n.ch) \
        + f"{pad}</{n.tag}>\n"


def walk(n):
    yield n
    for c in n.ch:
        yield from walk(c)


def count(n, S=None):
    """Drawable elements, all or (with S) visible."""
    S = S or {}
    if n.id and S.get(n.id, {}).get("hide"):
        return 0
    own = 0 if n.tag in ("svg", "g") else 1
    return own + sum(count(c, S) for c in n.ch)


# ------------------------------------------------------------------ runtime (reference for the C side)
def jit(t, seed, rate):
    k = math.floor(t * rate)
    return math.sin(k * 12.9898 + seed * 78.233) * 43758.5453 % 1 * 2 - 1


def mul(A, B):
    """Affine (a b c d e f) composition: A after B."""
    a1, b1, c1, d1, e1, f1 = A
    a2, b2, c2, d2, e2, f2 = B
    return (a1 * a2 + c1 * b2, b1 * a2 + d1 * b2, a1 * c2 + c1 * d2, b1 * c2 + d1 * d2, a1 * e2 + c1 * f2 + e1,
            b1 * e2 + d1 * f2 + f1)


def bits_of(b):
    return 16 - 15 * b


def mu_law(b):
    return 0.4 <= b <= 0.6


def runtime_state(k, t=0.0, gr=0.0):
    r, b, s, o = k
    S = {}
    hide = lambda i, cond=True: S.setdefault(i, {}).__setitem__("hide", bool(cond))
    # RATE: one level shown (or none)
    lvl = 0 if r < 0.12 else 1 if r < 0.4 else 2 if r < 0.65 else 3 if r < 0.85 else 4
    for i in range(1, 5):
        hide(f"rate{i}", i != lvl)
    # BITS
    nb = bits_of(b)
    mu = mu_law(b)
    tiers = 4 if nb >= 12 else 3 if nb >= 8 else 2 if nb >= 4 else 1 if nb >= 2 else 0
    for i in range(1, 5):
        hide(f"c{i}", mu or i > tiers)
    hide("cmu", not mu)
    hide("h1", nb > 8)
    hide("h2", nb > 5)
    hide("h3", nb > 3)
    hide("cap1", nb > 3)
    hide("cap2", nb > 1.5)
    hide("pile", nb > 3)
    # shade levels: posterise the scene
    if nb >= 9:
        pal = dict(faceL="purplegrey", faceR="grey", sun="darkblue", far="offblue", shadow="offblue", horizon="grey",
                   sand="offgrey", course="darkblue")
    elif nb >= 5:
        pal = dict(faceL="lpg", faceR="offgrey", sun="darkblue", far="darkblue", shadow="black", horizon="grey",
                   sand="grey", course="black")
    elif nb >= 2:
        pal = dict(faceL="white", faceR="offgrey", sun="grey", far="offgrey", shadow="black", horizon="white",
                   sand="black", course="black")
    else:
        pal = dict(faceL="white", faceR="black", sun="white", far="black", shadow="black", horizon="white",
                   sand="black", course="black")
    for i in ("faceL", "faceR"):
        S.setdefault(i, {}).update(fill=pal[i], stroke=pal[i] if not (i == "faceR" and nb < 2) else "green")
    for i in ("sun", "far", "shadow"):
        S.setdefault(i, {})["fill"] = pal[i]
    for i in ("horizon", "sand"):
        S.setdefault(i, {})["stroke"] = pal[i]
    for i in range(1, 5):
        S[f"c{i}"]["stroke"] = pal["course"]
    for i in range(1, 4):      # XOR: the slice's faces swap shades (in the current BITS palette)
        S.setdefault(f"gl{i}", {})["fill"] = pal["faceR"]
        S.setdefault(f"gr{i}", {})["fill"] = pal["faceL"]
    # SQUASH: squatter and pumping with the gain reduction
    q = s * (0.45 + 0.55 * gr)
    sy, sx = 1 - 0.4 * q, 1 + 0.18 * q   # cap: at least 60 % of the height
    m = (sx, 0, 0, sy, CX * (1 - sx), BASE * (1 - sy))
    # GRIT: clock jitter (whole pyramid trembles), then cracks, chips, debris, XOR slices
    dx = (0.5 + 1.5 * o) * jit(t, 1, 10 + 30 * o) if o > 0.05 else 0
    dy = 0.6 * o * jit(t, 2, 14 + 30 * o) if o > 0.3 else 0
    S.setdefault("pyr", {})["m"] = mul((1, 0, 0, 1, dx, dy), m)
    rows_gone = 4 if nb <= 1.5 else 2 if nb <= 3 else 0          # the press rests on what is left (cap1 / cap2)
    apex_y = BASE - (H - rows_gone * RY) * sy
    hide("press", s < 0.02)
    S.setdefault("press", {})["m"] = (1, 0, 0, 1, 0, apex_y - APEX + dy)
    hide("puff", not (s > 0.02 and gr > 0.55))
    S.setdefault("puff", {})["m"] = (sx, 0, 0, 1, CX * (1 - sx), 0)
    for i, th in enumerate([0.08, 0.2, 0.32, 0.45, 0.58, 0.72]):
        hide(f"k{i + 1}", o < th)
    hide("ch1", o < 0.3)
    hide("ch2", o < 0.62)
    nd = 0 if o < 0.45 else math.ceil((o - 0.45) / 0.55 * 6)
    for i, (x, y, side) in enumerate(DEBRIS):
        T = 0.9 + 0.13 * i
        ph = ((t + i * 0.37) % T) / T
        fall = ph * T * 1.6
        yy = y + 0.5 * 140 * fall ** 2
        S.setdefault(f"d{i + 1}", {})["m"] = (1, 0, 0, 1, -side * (4 + 14 * fall), min(yy, BASE - 2) - y)
        hide(f"d{i + 1}", i >= nd or yy > BASE + 4)
    for i, (y0, y1) in enumerate(SLICES):
        p_on = max(0, (o - 0.6) / 0.4)
        on = (jit(t, 10 + i, 7 + 9 * i) * 0.5 + 0.5) < p_on * 0.85
        hide(f"g{i + 1}", not on)
        sh = jit(t, 20 + i, 7 + 9 * i)
        S.setdefault(f"gs{i + 1}", {})["m"] = (1, 0, 0, 1, (4 + 14 * o) * (1 if sh >= 0 else -1) * (0.4 + 0.6 * abs(sh)), 0)
    return S


# ------------------------------------------------------------------ previews
HEADER = """
crush.svg - "crush" bitcrusher FX screen. Our own artwork: a pyramid on a night horizon that falls apart as the
crusher gets heavier; clean settings = pristine. Each knob has its own kind of damage in its own colour.
Drawn by TE's retained renderer (no overlay needed). Ids are found once at load (find, assert=0) and cached.
r = blue RATE, b = green BITS (nb = 16 - 15 b, mu = the DSP's mu-law range), s = white SQUASH, o = orange GRIT (0..1);
gr = compressor gain reduction 0..1 (live, smoothed), t = frame clock (s), jit(t, seed, rate) = noise -1..1 held 1/rate s.
Generator and reference runtime: gen_crush.py (runtime_state).
STATIC   sky: sun far horizon sand (recoloured by BITS, below).
pyr      group, matrix = translate(dx, dy) * scale(sx, sy) about (160, 126):                                  [white, orange]
         q = s * (0.45 + 0.55 gr); sy = 1 - 0.4 q (>= 0.6: still a pyramid); sx = 1 + 0.18 q (squat and pumping with the audio);
         dx = (0.5 + 1.5 o) * jit(t, 1, 10 + 30 o) when o > 0.05; dy = 0.6 o * jit(t, 2, 14 + 30 o) when o > 0.3 (clock jitter)
  shadow faceL faceR   the pyramid (faceL lit). Recoloured by BITS (fill; faces also stroke).
  c1..c4 course lines, nested tiers (2, 4, 8, 16 courses): show c1..cN, N = 4 (nb >= 12) 3 (>= 8) 2 (>= 4) 1 (>= 2) 0 [green]
  cmu    mu-law courses (dense at the base, sparse at the apex), green: shown instead of c1..c4 while mu        [green]
  h1 h2 h3   missing stones (black, green rim): shown for nb <= 8 / 5 / 3                                       [green]
  k1..k6 cracks (orange): shown for o >= .08 .2 .32 .45 .58 .72                                                [orange]
  ch1 ch2    chips out of the edges (black): o >= 0.3 / 0.62                                                   [orange]
  rate1..rate4   RATE levels (pixel columns 6 11 18 28): show exactly one, level = 0 (r < .12) 1 (< .4) 2 (< .65)
         3 (< .85) 4; each = mask (black: triangle minus the held stair), joint (darkblue column joints), step (blue
         stair outline)                                                                                          [blue]
  cap1 cap2  groups, steps collapsing: top 2 / 4 rows gone for nb <= 3 / 1.5 (capf black fill, capb green break line); pile (fallen stones at the
         base, green rim) with cap1                                                                               [green]
  g1..g3 XOR glitch slices: group shown while on = noise < 0.85 (o - 0.6) / 0.4 (jit(t, 10 + i, 7 + 9 i));
         gx = black hole left behind (static), gs = the slice with swapped shades (gl fill = faceR's, gr = faceL's) + orange top edge,
         matrix translate((4 + 14 o) * sign(n) * (0.4 + 0.6 |n|), 0), n = jit(t, 20 + i, 7 + 9 i)               [orange]
  d1..d6 debris (orange): first ceil((o - 0.45) / 0.55 * 6) shown; each falls from its chip with period
         0.9 + 0.13 i s: translate(-side (4 + 14 u), min(140 u^2 / 2 ...)) (see runtime_state), hidden below the base [orange]
press    group (stem + plate, white), shown for s >= 0.02; matrix translate(0, apex_y - 22 + dy),
         apex_y = 126 - (104 - 6.5 * rows_gone) sy, rows_gone = 4 / 2 / 0 with cap2 / cap1 / neither             [white]
puff     dust puffs at the base corners (white), shown for s >= 0.02 and gr > 0.55; matrix scale(sx, 1) about x 160 [white]
BITS SHADE LEVELS (recolour fill/stroke; colours unique per element, so TE keeps separate style entries):
         nb >= 9: faceL purplegrey, faceR grey, courses darkblue (pristine)
         nb 5..9: faceL lpg, faceR offgrey, shadow black, far darkblue, sand grey
         nb 2..5: faceL white, faceR offgrey, sun grey, far offgrey, horizon white, sand black
         nb < 2 : faceL white, faceR black with a green rim, sun white, far black (1 bit: on or off)
"""


def composite(out, name, k, t=0.3, gr=0.0, w=640):
    S = runtime_state(k, t, gr)
    body = render(asset(), S).split(">", 1)[1].rsplit("</svg>", 1)[0]
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160">\n'
           '<rect width="320" height="160" fill="#000000"/>\n' + body +
           '<rect x="0" y="141" width="320" height="19" fill="#000000" opacity="0.55"/>\n</svg>\n')
    p = os.path.join(out, name + ".svg")
    open(p, "w").write(svg)
    subprocess.run([RSVG, "-w", "320", p, "-o", os.path.join(out, name + ".png")], check=True)
    subprocess.run([RSVG, "-w", str(w), p, "-o", os.path.join(out, name + "@2x.png")], check=True)
    os.remove(p)
    return count(asset(), S)


STATES = [
    ("pristine", (0, 0, 0, 0), 0, "Pristine: all knobs clean. 16 courses, smooth casing, no colour accents."),
    ("rate_mid", (0.5, 0, 0, 0), 0, "RATE mid (~4 kHz): held into 11-px columns, blue stair outline, darkblue column joints."),
    ("rate_full", (1, 0, 0, 0), 0, "RATE full (~400 Hz): 28-px columns, a chunky step pyramid."),
    ("bits_mid", (0, 0.5, 0, 0), 0, "BITS mid (mu-law, ~8 bit): green mu-law courses (dense at the base, sparse up top), shades posterised to lpg / offgrey."),
    ("bits_low", (0, 0.85, 0, 0), 0, "BITS ~3 bit: 2 courses, posterised to white / offgrey, stones missing, apex collapsed (2 rows), rubble."),
    ("bits_full", (0, 1, 0, 0), 0, "BITS 1 bit: no courses, on/off shades, faceR black with a green rim, top 4 rows gone."),
    ("squash_mid", (0, 0, 0.5, 0), 0.4, "SQUASH mid (gr 0.4): squatter, the white press sits on the apex."),
    ("squash_full", (0, 0, 1, 0), 1.0, "SQUASH full on a hit (gr 1): squashed to 60 % height (the cap), dust puffs."),
    ("grit_mid", (0, 0, 0, 0.5), 0, "GRIT mid: clock jitter tremble, 4 cracks, chips, first debris."),
    ("grit_full", (0, 0, 0, 1), 0, "GRIT full: all cracks and chips, debris falling, XOR slices shifted and inverted."),
    ("combo_rate_bits", (0.5, 0.5, 0, 0), 0, "RATE mid + BITS mid: stepped, mu-law courses, missing stones."),
    ("combo_squash_grit", (0, 0, 0.6, 0.7), 0.6, "SQUASH 60 % + GRIT 70 %: pressed, cracked, shedding debris."),
    ("combo_lofi", (0.3, 0.7, 0.3, 0.3), 0.3, "A typical lo-fi setting: RATE 30 %, BITS ~5.5, SQUASH 30 %, GRIT 30 %."),
    ("destroyed", (1, 1, 1, 1), 0.9, "Fully destroyed: everything at max."),
]


def gif(out, name, frames):
    subprocess.run([MAGICK, "-delay", "5", "-loop", "0"] + frames + [os.path.join(out, name)], check=True)


def main(out):
    os.makedirs(out, exist_ok=True)
    a = asset()
    info = []
    for name, k, gr, cap in STATES:
        # pick a frame time where the glitch shows for the grit states
        t = 0.3
        if k[3] >= 0.99:
            for tt in [x * 0.02 for x in range(200)]:
                S = runtime_state(k, tt, gr)
                if sum(not S[f"g{i}"]["hide"] for i in (1, 2, 3)) >= 2 and sum(not S[f"d{i}"]["hide"] for i in range(1, 7)) >= 4:
                    t = tt
                    break
        n = composite(out, name, k, t, gr)
        info.append(dict(name=name, png=f"{name}.png", png2=f"{name}@2x.png", caption=cap, knobs=k, gr=gr, visible=n))
    fr = os.path.join(out, "frames")
    os.makedirs(fr, exist_ok=True)
    seqs = []

    def run(prefix, ks, dt=0.05):
        files = []
        for j, (k, gr) in enumerate(ks):
            composite(fr, f"{prefix}_{j:02d}", k, t=j * dt, gr=gr, w=640)
            files.append(os.path.join(fr, f"{prefix}_{j:02d}@2x.png"))
        return files

    # pumping: a 4-on-the-floor kick at 120 bpm: gain reduction jumps on the hit, releases over ~0.3 s
    def env(t):
        ph = (t % 0.5)
        return math.exp(-ph / 0.12) if ph > 0.01 else 0.5
    seq = run("pump", [((0, 0, 0.8, 0), env(j * 0.025)) for j in range(40)], dt=0.025)
    gif(out, "squash_pump.gif", seq)
    seqs.append(dict(gif="squash_pump.gif", strip=[os.path.relpath(p, out) for p in seq[:20:2]],
                     caption="SQUASH 80 % pumping on a 120 bpm kick (gain reduction jumps on each hit, 120 ms release): the press bobs, the pyramid breathes, dust puffs on the hits."))
    seq = run("grit", [((0, 0, 0, 1), 0) for j in range(48)], dt=0.04)
    gif(out, "grit_anim.gif", seq)
    seqs.append(dict(gif="grit_anim.gif", strip=[os.path.relpath(p, out) for p in seq[:20:2]],
                     caption="GRIT full, 25 fps: jitter tremble, debris falling from the chips, XOR slices flicking sideways."))
    seq = run("sweep", [((min(1, j / 40),) * 2 + (min(1, j / 40), min(1, j / 40)), 0.5 + 0.5 * math.sin(j * 0.6)) for j in range(52)], dt=0.04)
    gif(out, "sweep.gif", seq)
    seqs.append(dict(gif="sweep.gif", strip=[os.path.relpath(p, out) for p in seq[:44:4]],
                     caption="All four knobs swept together from clean to max: pristine to ruin."))
    seq = run("tour", [(tuple(min(1, max(0, (j - 30 * i) / 24)) if (j // 30) == i else 0 for i in range(4)), 0.5) for j in range(120)], dt=0.04)
    gif(out, "knob_tour.gif", seq)
    seqs.append(dict(gif="knob_tour.gif", strip=[], caption="Each knob in turn (blue, green, white, orange) from 0 to max."))
    json.dump(dict(states=info, seqs=seqs, elements=count(a)), open(os.path.join(out, "index.json"), "w"), indent=2)
    print("elements", count(a), [(i["name"], i["visible"]) for i in info])


if __name__ == "__main__":
    a = asset()
    svg_path = os.path.join(HERE, "crush.svg")
    open(svg_path, "w").write('<?xml version="1.0" encoding="utf-8"?>\n<!--' + HEADER.replace("--", "-") + '-->\n' + render(a))
    print(svg_path, os.path.getsize(svg_path), "bytes,", count(a), "elements")
    if len(sys.argv) > 1:
        main(sys.argv[1])
