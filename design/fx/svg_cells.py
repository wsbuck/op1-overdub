#!/usr/bin/env python3
"""Estimate TE's AGG rasterizer cells per element (notes/screen.md "AGG cell limit"): the device asserts
(agg.cpp:339 "alloc failed, too many blocks") once one fill or stroke needs a 16th block of 256 cells, so every single
fill and every single stroke must stay below 15 x 256 = 3840 cells.

  svg_cells.py file.svg [...]           the heaviest elements of each file, as drawn (no runtime state)
  svg_cells.py --check file.svg [...]   exit 1 if any element is above SAFE of the limit (build.py runs this)

Model (Hypothesis, calibrated against the screens that render): AGG makes one cell per panel pixel an edge passes
through, so an edge costs about |dx| + |dy| + 1 cells in panel pixels (1.175 per SVG unit). A fill costs its edges (all
subpaths together: one rasterization), a stroke its outline: both sides of every segment, plus about one stroke width
per join and per cap (round caps and joins a little more). Curves are flattened. cells(svg, state) takes a
gen_crush3d.py runtime_state dict ({id: {"hide", "m"}}): hidden nodes cost nothing, matrices replace the node's
transform and compose with the parents'.
"""
import math
import re
import sys
import xml.etree.ElementTree as ET

LIMIT = 15 * 256                    # cells per rasterization before the 16th block asserts
SAFE = 0.7                          # build check: fm4.svg's gears (~66 %) render on the device
PX = 1.175                          # panel pixels per SVG unit
TOK = re.compile(r"[MLHVCSZmlhvcsz]|-?\d*\.?\d+")


def mat(a, b):                      # a after b; (a b c d e f): x' = a x + c y + e
    return (a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1], a[0] * b[2] + a[2] * b[3], a[1] * b[2] + a[3] * b[3],
            a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5])


def parse_transform(t):
    m = (1, 0, 0, 1, 0, 0)
    for kind, args in re.findall(r"(\w+)\(([^)]*)\)", t or ""):
        v = [float(x) for x in re.split(r"[ ,]+", args.strip()) if x]
        if kind == "matrix":
            m = mat(m, tuple(v))
        elif kind == "translate":
            m = mat(m, (1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0))
        elif kind == "scale":
            m = mat(m, (v[0], 0, 0, v[1] if len(v) > 1 else v[0], 0, 0))
    return m


def subpaths(el, tag):
    """Polylines (list of points, closed flag) of an element, in its own coordinates."""
    g = lambda k: float(el.get(k, "0"))
    if tag == "line":
        return [([(g("x1"), g("y1")), (g("x2"), g("y2"))], False)]
    if tag in ("polyline", "polygon"):
        v = [float(x) for x in re.split(r"[ ,]+", el.get("points", "").strip()) if x]
        return [(list(zip(v[0::2], v[1::2])), tag == "polygon")]
    if tag == "rect":
        x, y, w, h = g("x"), g("y"), g("width"), g("height")
        return [([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], True)]
    if tag in ("circle", "ellipse"):
        rx = g("r") if tag == "circle" else g("rx")
        ry = g("r") if tag == "circle" else g("ry")
        n = 32
        return [([(g("cx") + rx * math.cos(2 * math.pi * i / n), g("cy") + ry * math.sin(2 * math.pi * i / n))
                  for i in range(n)], True)]
    out, cur, x, y, cmd, toks = [], [], 0.0, 0.0, "M", TOK.findall(el.get("d", ""))
    i = 0
    while i < len(toks):
        t = toks[i]
        if t.isalpha():
            cmd = t
            i += 1
            if cmd in "Zz":
                if cur:
                    out.append((cur, True))
                    x, y = cur[0]
                cur = []
                continue
            continue
        rel = cmd.islower()
        c = cmd.upper()
        n = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4}[c]
        v = [float(z) for z in toks[i:i + n]]
        i += n
        if c == "M":
            if cur:
                out.append((cur, False))
            x, y = (x + v[0], y + v[1]) if rel else (v[0], v[1])
            cur = [(x, y)]
            cmd = "l" if rel else "L"
        elif c in "LHV":
            if c == "L":
                x, y = (x + v[0], y + v[1]) if rel else (v[0], v[1])
            elif c == "H":
                x = x + v[0] if rel else v[0]
            else:
                y = y + v[0] if rel else v[0]
            cur.append((x, y))
        else:                                                      # cubic: flattened in 8
            pts = [(x + v[j], y + v[j + 1]) if rel else (v[j], v[j + 1]) for j in range(0, n, 2)]
            p0 = (x, y)
            c1, c2, p3 = (pts if c == "C" else [p0] + pts)
            for k in range(1, 9):
                s = k / 8
                cur.append(tuple((1 - s) ** 3 * a + 3 * (1 - s) ** 2 * s * b + 3 * (1 - s) * s * s * cc + s ** 3 * d
                                 for a, b, cc, d in zip(p0, c1, c2, p3)))
            x, y = p3
    if cur:
        out.append((cur, False))
    return out


def edge(p, q):
    return abs(q[0] - p[0]) * PX + abs(q[1] - p[1]) * PX + 1


def element_cells(el, tag, m):
    sp = [([(m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]) for x, y in pts], closed) for pts, closed in
          subpaths(el, tag)]
    fill = el.get("fill", "#000000" if tag != "line" else "none")
    stroke = el.get("stroke", "none")
    fc = sc = 0
    if fill != "none":
        for pts, _ in sp:                                         # fills are closed implicitly
            fc += sum(edge(pts[j], pts[(j + 1) % len(pts)]) for j in range(len(pts))) if len(pts) > 1 else 0
    if stroke != "none":
        w = float(el.get("stroke-width", "1")) * math.sqrt(abs(m[0] * m[3] - m[1] * m[2])) * PX
        roundish = 1.6 if el.get("stroke-linecap") == "round" else 1.0
        for pts, closed in sp:
            segs = list(zip(pts, pts[1:] + ([pts[0]] if closed else [])))
            sc += 2 * sum(edge(p, q) for p, q in segs)
            joins = len(segs) - (0 if closed else 1)
            sc += joins * (w + 2) * (1.6 if el.get("stroke-linejoin") == "round" else 1.0)
            if not closed:
                sc += 2 * (w + 2) * roundish
    return fc, sc


def cells(path, state=None):
    """[(id or tag, fill cells, stroke cells)] of every drawn element."""
    state = state or {}
    out = []

    def walk(el, m):
        tag = el.tag.split("}")[-1]
        i = el.get("id")
        st = state.get(i, {}) if i else {}
        if st.get("hide"):
            return
        own = st["m"] if "m" in st else parse_transform(el.get("transform"))
        m = mat(m, own)
        if tag not in ("svg", "g"):
            f, s = element_cells(el, tag, m)
            out.append((i or tag, int(f), int(s)))
        for c in el:
            walk(c, m)
    walk(ET.parse(path).getroot(), (1, 0, 0, 1, 0, 0))
    return out


def worst(path, state=None):
    c = cells(path, state)
    return max(((max(f, s), i, "fill" if f >= s else "stroke") for i, f, s in c), default=(0, "", ""))


if __name__ == "__main__":
    chk = "--check" in sys.argv
    bad = []
    for p in [a for a in sys.argv[1:] if a != "--check"]:
        w = worst(p)
        if w[0] > SAFE * LIMIT:
            bad.append(f"{p}: {w[1]} {w[2]} ~{w[0]} cells > {int(SAFE * LIMIT)} ({int(100 * SAFE)} % of AGG's {LIMIT})")
        if chk:
            print(f"{p}: heaviest {w[1]} ({w[2]}) ~{w[0]} cells, {100 * w[0] / LIMIT:.0f} % of {LIMIT}")
            continue
        c = sorted(cells(p), key=lambda e: -max(e[1], e[2]))
        print(f"{p}: heaviest " + ", ".join(f"{i} {max(f, s)} ({'fill' if f >= s else 'stroke'})" for i, f, s in c[:6])
              + f"; limit {LIMIT}")
    if chk and bad:
        sys.exit("AGG cell budget exceeded (agg.cpp:339 would assert on the device):\n  " + "\n  ".join(bad))
