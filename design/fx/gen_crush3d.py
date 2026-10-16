#!/usr/bin/env python3
"""crush3d.svg: the 3D variant of the "crush" bitcrusher FX screen (gen_crush.py is the flat one). A square pyramid in a
low three-quarter view (orthographic, yaw 30 deg, elevation 16 deg), lit from the front left; every face is a flat
polygon, so the runtime only shows / hides / moves / recolours layers, exactly like crush.svg.

  python3 design/fx/gen_crush3d.py [PREVIEW_DIR]     writes design/fx/crush3d.svg; previews if a dir is given

Knobs (0..1): r = blue RATE, b = green BITS, s = white SQUASH, o = orange GRIT; gr = gain reduction 0..1, t = clock (s)."""
import json, math, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_crush as g2
from gen_crush import N, C, f, path, render, count, jit, mul, uniq, RSVG, MAGICK

# ---- projection: world a = 1 (base half size), Y up; screen = orthographic, yaw PSI, elevation PHI
PSI, PHI = math.radians(30), math.radians(16)
CP, SP, CF, SF = math.cos(PSI), math.sin(PSI), math.cos(PHI), math.sin(PHI)
S = 66.0                        # px per world unit
CX, B0 = 160.0, 104.0           # screen of the world origin (base centre)
H = 1.27                        # Giza: 51.8 deg faces
ROWS = 16
MU = 15
HORIZON = 90.5
LIGHT = (-1.2, 1.0, -0.5)        # toward the sun: front-left, above


def f1(v):
    s = f"{v:.1f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def pd(Q, close=True):
    """Path data at 0.1 px (keeps the file small; 0.05 px is invisible on the panel)."""
    return "M" + " L".join(f"{f1(x)} {f1(y)}" for x, y in Q) + (" Z" if close else "")


def P(X, Y, Z):
    d = -X * SP + Z * CP
    return (CX + S * (X * CP + Z * SP), B0 - S * (Y * CF + d * SF))


def W(face, u, v):
    """World point on a face: faceL (normal -Z) u = -1 left corner .. +1 front corner; faceR (normal +X) u = -1 front
    corner .. +1 right corner; v = 0 base .. 1 apex."""
    r = 1 - v
    return (u * r, v * H, -r) if face == "L" else (r, v * H, u * r)


def F(face, u, v):
    return P(*W(face, u, v))


APEX = P(0, H, 0)
LC, FC, RC = P(-1, 0, -1), P(1, 0, -1), P(1, 0, 1)


def hull(pts):
    pts = sorted(set(pts))
    cr = lambda o, a, b: (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lo, hi = [], []
    for p in pts:
        while len(lo) >= 2 and cr(lo[-2], lo[-1], p) <= 0:
            lo.pop()
        lo.append(p)
    for p in reversed(pts):
        while len(hi) >= 2 and cr(hi[-2], hi[-1], p) <= 0:
            hi.pop()
        hi.append(p)
    return lo[:-1] + hi[:-1]


def grow(poly, d):
    cx = sum(p[0] for p in poly) / len(poly)
    cy = sum(p[1] for p in poly) / len(poly)
    out = []
    for x, y in poly:
        l = math.hypot(x - cx, y - cy) or 1
        out.append((x + (x - cx) / l * d, y + (y - cy) / l * d))
    return out


def clip(subject, clipper):
    """Sutherland-Hodgman: subject polygon clipped to a convex polygon."""
    def inside(p, a, b, sgn):
        return sgn * ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) >= 0
    area = sum(clipper[i][0] * clipper[i - 1][1] - clipper[i - 1][0] * clipper[i][1] for i in range(len(clipper)))
    sgn = -1 if area > 0 else 1
    out = subject
    for i in range(len(clipper)):
        a, b = clipper[i - 1], clipper[i]
        inp, out = out, []
        for j in range(len(inp)):
            p, q = inp[j - 1], inp[j]
            pin, qin = inside(p, a, b, sgn), inside(q, a, b, sgn)
            if qin:
                if not pin:
                    out.append(isect(p, q, a, b))
                out.append(q)
            elif pin:
                out.append(isect(p, q, a, b))
        if not out:
            return []
    return out


def isect(p, q, a, b):
    x1, y1, x2, y2, x3, y3, x4, y4 = *p, *q, *a, *b
    den = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4)
    t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / den
    return (x1 + t * (x2 - x1), y1 + t * (y2 - y1))


def box(x0, x1, y0, y1, z0, z1):
    """Visible faces of an axis-aligned box (top, -Z side, +X side) as screen polygons."""
    top = [P(x0, y1, z0), P(x1, y1, z0), P(x1, y1, z1), P(x0, y1, z1)]
    left = [P(x0, y0, z0), P(x1, y0, z0), P(x1, y1, z0), P(x0, y1, z0)]
    right = [P(x1, y0, z0), P(x1, y0, z1), P(x1, y1, z1), P(x1, y1, z0)]
    return top, left, right



# ------------------------------------------------------------------ AGG cell budget (notes/screen.md "AGG cell limit")
# TE's rasterizer asserts (agg.cpp:339) once one fill or one stroke needs more than 15 x 256 cells; long compound
# paths are split into parts of at most BUDGET estimated cells (svg_cells.py), grouped under the original id (so the
# runtime's visibility still works; a recoloured part is its own leaf: id + "a", "b", ..).
import xml.etree.ElementTree as _ET
import svg_cells as _cells
BUDGET = 1400


def _est(d, a):
    el = _ET.Element("path", {k.replace("_", "-"): str(v) for k, v in dict(a, d=d).items()})
    return max(_cells.element_cells(el, "path", (1, 0, 0, 1, 0, 0)))


def chunked(id, pieces, **a):
    """One path (id) of the pieces, or a group id of paths id+a, id+b, .. each within BUDGET. Colour names in fill /
    stroke get a unique value per part (C[name] via uniq)."""
    probe = {k: ("#000000" if k in ("fill", "stroke") and v in C else v) for k, v in a.items()}
    parts, cur = [], []
    for p in pieces:
        if cur and _est(" ".join(cur + [p]), probe) > BUDGET:
            parts.append(cur)
            cur = []
        cur.append(p)
    parts.append(cur)

    def attrs():
        return {k: (uniq(v) if k in ("fill", "stroke") and v in C else v) for k, v in a.items()}
    if len(parts) == 1:
        return path(id, " ".join(parts[0]), **attrs())
    return N("g", id, [path(f"{id}{chr(97 + j)}", " ".join(pp), **attrs()) for j, pp in enumerate(parts)])

# ------------------------------------------------------------------ layers
RATE_N = [8, 6, 4, 3]            # voxel tiers per RATE level (block = 1/N of the half base); level 1 without the
                                 # fine vertical edges (they moire on the OLED)


def voxel(i, n):
    tops, lefts, rights, lines = [], [], [], []
    for k in range(n):
        y0, y1 = k * H / n, (k + 1) * H / n
        r = (n - k - 0.5) / n
        t, l, rr = box(-r, r, y0, y1, -r, r)
        tops.append(pd(t))
        lefts.append(pd(l))
        rights.append(pd(rr))
        # tier outline: top rhombus front edges, three vertical edges, base edges
        lines.append(pd([P(-r, y1, -r), P(r, y1, -r), P(r, y1, r)], False))
        vert = lambda X, Z: "M{} {} V{}".format(f1(P(X, y0, Z)[0]), f1(P(X, y0, Z)[1]), f1(P(X, y1, Z)[1]))   # world verticals stay vertical
        lines.append(" ".join([vert(-r, -r), vert(r, -r), vert(r, r)]))
        nb = 2 * n - 2 * k - 1
        for j in range(1, nb if i > 1 else 1):
            c = -r + j * 2 * r / nb
            lines.append(vert(c, -r) + " " + vert(r, c))
    return N("g", f"vox{i}", [
        chunked(f"vt{i}", tops, fill="lpg"),
        chunked(f"vl{i}", lefts, fill="purplegrey"),
        chunked(f"vr{i}", rights, fill="grey"),
        chunked(f"ve{i}", lines, fill="none", stroke=C["blue"], stroke_width="0.6", stroke_linecap="round")])


def course_d(rows):
    return [pd([F("L", -1, i / ROWS), F("L", 1, i / ROWS), F("R", 1, i / ROWS)], False) for i in rows]


def mu_d():
    vs = [((1 + MU) ** (k / 8) - 1) / MU for k in range(1, 8)]
    return [pd([F("L", -1, v), F("L", 1, v), F("R", 1, v)], False) for v in vs]


DEPTH = 0.13
HOLES = [[("L", 2, -0.55, 0.22), ("R", 5, -0.2, 0.3), ("L", 8, 0.1, 0.35)],
         [("L", 1, 0.3, 0.16), ("R", 2, 0.35, 0.2), ("L", 5, -0.3, 0.25), ("R", 9, -0.5, 0.45), ("L", 11, -0.6, 0.5)],
         [("L", 4, 0.45, 0.25), ("R", 0, -0.75, 0.16), ("L", 0, -0.9, 0.14), ("R", 7, 0.2, 0.4), ("L", 7, 0.55, 0.4), ("R", 12, -0.4, 0.6)]]


def hole(face, row, u0, du):
    v0, v1 = row / ROWS, (row + 1) / ROWS
    u1 = min(u0 + du, 0.95)
    opening = [F(face, u0, v0), F(face, u1, v0), F(face, u1, v1), F(face, u0, v1)]
    inward = (0, 0, DEPTH) if face == "L" else (-DEPTH, 0, 0)
    def back(face, u, v):
        x, y, z = W(face, u, v)
        return P(x + inward[0], y, z + inward[2])
    floor = [F(face, u0, v0), F(face, u1, v0), back(face, u1, v0), back(face, u0, v0)]
    dx = back(face, 0, v0)[0] - F(face, 0, v0)[0]
    uw = u0 if dx > 0 else u1
    wall = [F(face, uw, v0), back(face, uw, v0), back(face, uw, v1), F(face, uw, v1)]
    return pd(opening), pd(clip(floor, opening)), pd(clip(wall, opening))


def hole_set(i, hs):
    parts = [hole(*h) for h in hs]
    return [path(f"h{i}", " ".join(p[0] for p in parts), fill=C["black"], stroke=C["green"], stroke_width="0.7",
                 stroke_linejoin="miter"),
            path(f"hw{i}", " ".join(p[2] for p in parts), fill=uniq("offgrey")),
            path(f"hf{i}", " ".join(p[1] for p in parts), fill=uniq("lpg"))]


def cube_faces(cx, cz, size, yaw, tilt, lift=0.0):
    """A tumbled cube resting near the ground: visible faces as (polygon, brightness)."""
    h = size / 2
    cyw, syw, ct, st = math.cos(yaw), math.sin(yaw), math.cos(tilt), math.sin(tilt)
    def rot(x, y, z):
        y, z = y * ct - z * st, y * st + z * ct          # tilt about X
        x, z = x * cyw + z * syw, -x * syw + z * cyw     # yaw about Y
        return x, y, z
    V = [(sx * h, sy * h, sz * h) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    Vr = [rot(*v) for v in V]
    ymin = min(v[1] for v in Vr)
    Vw = [(cx + x, y - ymin + lift, cz + z) for x, y, z in Vr]
    faces = [(0, 1, 3, 2), (4, 5, 7, 6), (0, 1, 5, 4), (2, 3, 7, 6), (0, 2, 6, 4), (1, 3, 7, 5)]
    out = []
    L = LIGHT
    ll = math.sqrt(sum(c * c for c in L))
    for fc in faces:
        p = [Vw[j] for j in fc]
        a = [p[1][k] - p[0][k] for k in range(3)]
        b = [p[3][k] - p[0][k] for k in range(3)]
        n = [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
        c = [sum(q[k] for q in p) / 4 for k in range(3)]
        cc = [cx, sum(v[1] for v in Vw) / 8, cz]
        if sum(n[k] * (c[k] - cc[k]) for k in range(3)) < 0:
            n = [-x for x in n]
        nl = math.sqrt(sum(x * x for x in n))
        ev = eye()
        if sum(n[k] * ev[k] for k in range(3)) <= 0:
            continue
        out.append(([P(*q) for q in p], sum(n[k] * L[k] for k in range(3)) / nl / ll))
    return out


def eye():
    """World vector pointing from the scene toward the viewer (orthographic)."""
    # screen depth d = -X SP + Z CP grows away from the viewer; y-screen up = Y CF + d SF; the eye is along -(depth axis)
    # tilted down by PHI: eye = cos(PHI) * (-depth axis) + sin(PHI) * up
    dax = (-SP, 0, CP)
    return (-dax[0] * CF, SF, -dax[2] * CF)


PILE = [(-1.25, -0.95, 0.16, 0.5, 0.35), (-1.12, -1.25, 0.11, 1.1, 0.0), (-1.45, -0.6, 0.1, 0.2, 0.6),
        (1.2, -1.25, 0.14, 0.9, 0.4), (1.35, -0.9, 0.1, 0.3, 0.0), (0.95, -1.42, 0.09, 1.4, 0.7)]


def pile():
    cls = {"pt": [], "pm": [], "pk": []}
    for cxz in PILE:
        for poly, br in cube_faces(*cxz):
            cls["pt" if br > 0.55 else "pm" if br > 0.0 else "pk"].append(pd(poly))
    col = {"pt": "lpg", "pm": "purplegrey", "pk": "grey"}
    return N("g", "pile", [path(k, " ".join(v), fill=C[col[k]], stroke=C["green"], stroke_width="0.6",
                                stroke_linejoin="round") for k, v in cls.items() if v])


CRACKS = [   # (face, u, v) polylines; several cross the front ridge (u = 1 on L is u = -1 on R)
    [("L", 1, 0.99), ("L", 0.7, 0.9), ("R", -0.6, 0.84), ("L", 0.6, 0.76), ("L", 0.85, 0.7)],
    [("L", -1, 0.62), ("L", -0.8, 0.6), ("L", -0.62, 0.64), ("L", -0.45, 0.58), ("L", -0.3, 0.6)],
    [("L", 0.55, 0.47), ("L", 0.8, 0.5), ("L", 1, 0.46), ("R", -0.75, 0.5), ("R", -0.5, 0.44), ("R", -0.3, 0.47)],
    [("L", -0.35, 0.0), ("L", -0.3, 0.07), ("L", -0.42, 0.13), ("L", -0.28, 0.2), ("L", -0.36, 0.26)],
    [("R", 1, 0.3), ("R", 0.8, 0.27), ("R", 0.66, 0.32), ("R", 0.45, 0.27), ("R", 0.3, 0.31)],
    [("L", 0.75, 0.0), ("L", 0.8, 0.08), ("L", 0.98, 0.12), ("R", -0.82, 0.18), ("R", -0.9, 0.25), ("R", -0.7, 0.3)],
]


def notch(face, v, dv=0.035, du=0.12):
    """A V notch out of a silhouette edge (faceL left edge or faceR right edge)."""
    e = -1 if face == "L" else 1
    a, m, b = F(face, e, v - dv), F(face, e - e * du, v), F(face, e, v + dv)
    out = (-1.6, 0) if face == "L" else (1.6, 0.4)
    return pd([(a[0] + out[0], a[1] + out[1]), a, m, b, (b[0] + out[0], b[1] + out[1])])


def ridge_chip(v, dv=0.04, du=0.1):
    return pd([F("L", 1 - du, v), F("L", 1, v + dv), F("R", -1 + du, v), F("L", 1, v - dv)])


def cap(i, rows):
    vc = 1 - rows / ROWS
    rc = rows / ROWS
    yc = vc * H
    cover = grow(hull([APEX, F("L", -1, vc), F("L", 1, vc), F("R", 1, vc), P(-rc, yc, rc)]), 1.6)
    dips = [0, 0.4, 0.15, 0.75, 0.3, 0.0, 0.55, 0.2, 0.6, 0.1, 0]
    us = [-1 + 2 * j / 10 for j in range(11)]
    rim = [F("L", u, vc - d / ROWS) for u, d in zip(us, dips)] + [F("R", u, vc - d / ROWS) for u, d in zip(us, dips[::-1])][1:]
    top = [P(-rc, yc, rc)] + rim
    return N("g", f"cap{i}", [
        path(f"capf{i}", pd(cover), fill=C["black"]),
        path(f"capt{i}", pd(top), fill=uniq("offgrey")),
        path(f"capb{i}", pd(rim, False), fill="none", stroke=C["green"], stroke_width="0.8", stroke_linejoin="round")])


SLICES = [(0.6, 0.67), (0.36, 0.43), (0.12, 0.2)]


def slice_group(i, v0, v1):
    erase = grow([F("L", -1, v0), F("L", 1, v0), F("R", 1, v0), F("R", 1, v1), F("L", 1, v1), F("L", -1, v1)], 0.8)
    bl = [F("L", -1, v0), F("L", 1, v0), F("L", 1, v1), F("L", -1, v1)]
    br = [F("R", -1, v0), F("R", 1, v0), F("R", 1, v1), F("R", -1, v1)]
    return N("g", f"g{i}", [
        path(f"gx{i}", pd(erase), fill=C["black"]),
        N("g", f"gs{i}", [
            path(f"gl{i}", pd(bl), fill=uniq("grey")),
            path(f"gr{i}", pd(br), fill=uniq("purplegrey")),
            path(f"go{i}", pd([F("L", -1, v1), F("L", 1, v1), F("R", 1, v1)], False), fill="none", stroke=C["orange"],
                 stroke_width="1")])])


# debris: (face, u, v) spawn on a silhouette edge, falling outward to the ground
DEBRIS = [("L", -1, 0.55), ("R", 1, 0.4), ("L", -1, 0.3), ("R", 1, 0.7)]
DSIZE = 0.075


def debris_geo(face, u, v):
    x, y, z = W(face, u, v)
    out = (-0.45, 0, -0.15) if face == "L" else (0.3, 0, -0.35)
    s0 = P(x, y, z)
    land = P(x + out[0], 0, z + out[2])
    g0 = P(x, 0, z)
    return s0, land, g0


def debris(i, face, u, v):
    s0, land, g0 = debris_geo(face, u, v)
    x, y, z = W(face, u, v)
    t, l, r = box(x - DSIZE / 2, x + DSIZE / 2, y, y + DSIZE, z - DSIZE / 2, z + DSIZE / 2)
    return [N("ellipse", f"ds{i}", cx=f(g0[0]), cy=f(g0[1]), rx="3.6", ry="1.2", fill=C["darkblue"]),
            N("g", f"d{i}", [path(f"dt{i}", pd(t), fill=C["lightorange"]),
                             path(f"dl{i}", pd(l) + " " + pd(r), fill=C["orange"])])]


def asset():
    g2._used.clear()
    far = [(250, 0.13), (284, 0.09)]
    farL, farR = [], []
    for x0, sc in far:
        def q(X, Y, Z):
            p = P(X, Y, Z)
            return (x0 + (p[0] - CX) * sc, HORIZON + 1 + (p[1] - P(1, 0, -1)[1]) * sc)
        farL.append(pd([q(-1, 0, -1), q(1, 0, -1), q(0, H, 0)]))
        farR.append(pd([q(1, 0, -1), q(1, 0, 1), q(0, H, 0)]))
    sky = N("g", "sky", [
        N("circle", "sun", cx="58", cy="38", r="14", fill=uniq("darkblue")),
        N("g", "farg", [path("farL", " ".join(farL), fill=uniq("darkblue")), path("farR", " ".join(farR), fill=uniq("offblue"))]),
        N("line", "horizon", x1="6", y1=f(HORIZON), x2="314", y2=f(HORIZON), stroke=uniq("grey"), stroke_width="1"),
        path("sand", "M14 104 H44 M22 122 H70 M84 136 H118 M250 128 H286 M268 112 H306 M200 138 H228", fill="none",
             stroke=uniq("offgrey"), stroke_width="1", stroke_linecap="round"),
    ])
    L = LIGHT
    shp = (-L[0] / L[1] * H, 0, -L[2] / L[1] * H)
    shadow = hull([LC, FC, RC, P(-1, 0, 1), P(*shp)])
    faceL = [LC, FC, APEX]
    faceR = [FC, RC, APEX]
    pyr = N("g", "pyr", [
        path("shadow", pd(shadow), fill=uniq("offblue")),
        N("g", "smooth", [
            path("faceL", pd(faceL), fill=uniq("purplegrey"), stroke=uniq("purplegrey"), stroke_width="0.6", stroke_linejoin="round"),
            path("faceR", pd(faceR), fill=uniq("grey"), stroke=uniq("grey"), stroke_width="0.6", stroke_linejoin="round"),
            chunked("c1", course_d([8]), fill="none", stroke="darkblue", stroke_width="0.7", stroke_linejoin="round"),
            chunked("c2", course_d([4, 12]), fill="none", stroke="darkblue", stroke_width="0.7", stroke_linejoin="round"),
            chunked("c3", course_d([2, 6, 10, 14]), fill="none", stroke="darkblue", stroke_width="0.7", stroke_linejoin="round"),
            chunked("c4", course_d([1, 3, 5, 7, 9, 11, 13, 15]), fill="none", stroke="darkblue", stroke_width="0.7", stroke_linejoin="round"),
            chunked("cmu", mu_d(), fill="none", stroke=C["green"], stroke_width="0.7", stroke_linejoin="round"),
            N("line", "ridge", x1=f(FC[0]), y1=f(FC[1]), x2=f(APEX[0]), y2=f(APEX[1]), stroke=C["lpg"], stroke_width="0.8"),
        ]),
    ] + [voxel(i + 1, n) for i, n in enumerate(RATE_N)]
      + [e for i, hs in enumerate(HOLES) for e in hole_set(i + 1, hs)]
      + [path(f"k{i + 1}", pd([F(*q) for q in c], False), fill="none", stroke=C["orange"], stroke_width="1",
              stroke_linejoin="round", stroke_linecap="round") for i, c in enumerate(CRACKS)]
      + [path("ch1", notch("L", 0.5) + " " + notch("R", 0.36), fill=C["black"]),
         path("ch2", notch("L", 0.72) + " " + notch("R", 0.62) + " " + notch("L", 0.24), fill=C["black"]),
         path("chr", ridge_chip(0.56) + " " + ridge_chip(0.3), fill=C["offgrey"])]
      + [cap(1, 2), cap(2, 4), pile()]
      + [slice_group(i + 1, *s) for i, s in enumerate(SLICES)]
      + [e for i, d in enumerate(DEBRIS) for e in debris(i + 1, *d)])
    pr = 0.42
    top, left, right = box(-pr, pr, H + 0.04, H + 0.12, -pr, pr)
    sx, sy = P(0, H + 0.12, 0)
    press = N("g", "press", [
        N("line", "stem", x1=f(sx), y1="-60", x2=f(sx), y2=f(sy), stroke=C["white"], stroke_width="2"),
        path("plt", pd(top), fill=C["white"]),
        path("pll", pd(left), fill=C["lpg"]),
        path("plr", pd(right), fill=C["purplegrey"]),
    ])
    pf = lambda p, dx, dy: f"M{f(p[0] + dx * 0.4)} {f(p[1] + dy * 0.4)} L{f(p[0] + dx)} {f(p[1] + dy)}"
    puff = path("puff", " ".join([pf(LC, -9, -5), pf(LC, -10, 1), pf(FC, -5, 5), pf(FC, 5, 5), pf(RC, 9, -5), pf(RC, 10, 1)]),
                fill="none", stroke=C["white"], stroke_width="1", stroke_linecap="round")
    marker = N("g", "cells2")            # the runtime draws this file only if it finds this id: cell-budgeted (v2)
    return N("svg", None, [sky, pyr, press, puff, marker], version="1.1", xmlns="http://www.w3.org/2000/svg", x="0", y="0",
             width="320", height="160", viewBox="0 0 320 160")


# ------------------------------------------------------------------ runtime (reference for the C side)
# The DSP's knob maps (src/slot/crush.c: bits_raw / bits_k / bits_mu, rate_raw / rate_oct), raw knob 0..32767.
BITS_ANCH = [(0, 16, 0), (1024, 15, 0), (2048, 14, 0), (3072, 13, 0), (4096, 12, 0), (6144, 11, 0), (8192, 10, 0),
             (10240, 9, 0), (12288, 8, 0), (16384, 8, 1), (18432, 7, 0.5), (20480, 6, 0), (22938, 5, 0), (25600, 4, 0),
             (27853, 3, 0), (30310, 2, 0), (32767, 1, 0)]
PLAT = 200


def _anchors(points):
    out = []
    for p, *v in points:
        for q in (p - PLAT, p + PLAT):
            q = min(32767, max(0, q))
            if not out or out[-1][0] != q:
                out.append((q, *v))
    return out


def bits_dsp(b):
    """(bit depth, mu-law amount) the DSP uses at BITS knob b (0..1)."""
    a, k = _anchors(BITS_ANCH), min(32767, b * 32768)
    for (x0, b0, m0), (x1, b1, m1) in zip(a, a[1:]):
        if k <= x1:
            f = 0 if x1 == x0 else (k - x0) / (x1 - x0)
            return b0 + (b1 - b0) * f, m0 + (m1 - m0) * f
    return 1.0, 0.0


RATE_DETENTS = (4, 10, 18, 25)    # voxel levels 1..4 start on these RATE detents (26 kHz, 11 kHz, ~3.6 kHz, 1.4 kHz)
CRACK_AT = (0.1, 0.25, 0.5, 0.6, 0.75, 0.9)   # GRIT: jitter is audible from ~0.1, flips start at 0.5


def runtime_state(k, t=0.0, gr=0.0, parallax=True, level=1.0, flips=None):
    """k = raw knobs 0..1 (r b s o); gr = gain reduction 0..1; level = input peak 0..1 (the shake needs a signal);
    flips = bit flips since the last frame (None: the design preview's stand-in, a steady stream while GRIT > 1/2)."""
    r, b, s, o = k
    St = {}
    def st(i):
        return St.setdefault(i, {})
    def hide(i, c=True):
        st(i)["hide"] = bool(c)
    # RATE: one voxel level replaces the smooth pyramid
    d = r * 32
    lvl = sum(d >= x for x in RATE_DETENTS)
    hide("smooth", lvl != 0)
    for i in range(1, 5):
        hide(f"vox{i}", i != lvl)
    # BITS: the DSP's bit depth and mu-law amount
    nb, m = bits_dsp(b)
    mu = m > 0.25
    tiers = 4 if nb >= 12 else 3 if nb >= 8 else 2 if nb >= 4 else 1 if nb >= 2 else 0
    for i in range(1, 5):
        hide(f"c{i}", mu or i > tiers)
    hide("cmu", not mu)
    for i, gone in ((1, nb < 8), (2, nb <= 5), (3, nb <= 3)):
        for p in ("h", "hw", "hf"):
            hide(f"{p}{i}", not gone)
    hide("cap1", nb > 3)
    hide("cap2", nb > 1.5)
    hide("pile", nb > 3)
    # posterise inside the neutral family: lit face lpg / purplegrey, shade face offgrey / black, a grey sun; green is
    # BITS's accent (rims, rubble, mu-law courses); pure white is only SQUASH's (press, stem, puff)
    row = 0 if nb >= 9 else 1 if nb >= 5 else 2 if nb >= 2 else 3
    pal = [dict(L="purplegrey", R="grey", RS="grey", T="lpg", core="offgrey", course="darkblue", sun="darkblue",
                farL="darkblue", farR="offblue", shadow="offblue", horizon="grey", sand="offgrey"),
           dict(L="lpg", R="offgrey", RS="offgrey", T="lpg", core="black", course="black", sun="grey",
                farL="darkblue", farR="darkblue", shadow="black", horizon="grey", sand="grey"),
           dict(L="lpg", R="offgrey", RS="offgrey", T="lpg", core="offgrey", course="black", sun="grey",
                farL="offgrey", farR="offgrey", shadow="black", horizon="lpg", sand="black"),
           dict(L="lpg", R="black", RS="green", T="lpg", core="offgrey", course="black", sun="grey",
                farL="offgrey", farR="offgrey", shadow="black", horizon="lpg", sand="black")][row]
    st("faceL").update(fill=pal["L"], stroke=pal["L"])
    st("faceR").update(fill=pal["R"], stroke=pal["RS"])
    for i in range(1, 5):
        st(f"vt{i}")["fill"] = pal["T"]
        st(f"vl{i}")["fill"] = pal["L"]
        st(f"vr{i}")["fill"] = pal["R"]
        st(f"c{i}")["stroke"] = pal["course"]
    for i in range(1, 4):
        st(f"hf{i}")["fill"] = pal["T"]
        st(f"hw{i}")["fill"] = pal["core"]
        st(f"gl{i}")["fill"] = pal["R"]       # XOR: the slice's faces swap shades
        st(f"gr{i}")["fill"] = pal["L"]
    for i in ("sun", "farL", "farR", "shadow"):
        st(i)["fill"] = pal[i]
    for i in ("horizon", "sand"):
        st(i)["stroke"] = pal[i]
    # SQUASH (capped: at least 60 % of the height)
    q = s * (0.45 + 0.55 * gr)
    sy, sx = 1 - 0.4 * q, 1 + 0.18 * q
    m_ = (sx, 0, 0, sy, CX * (1 - sx), B0 * (1 - sy))
    # GRIT: the clock jitter shakes the pyramid, only while something plays
    live = level > 0.001
    dx = (0.5 + 1.5 * o) * jit(t, 1, 10 + 30 * o) if o > 0.05 and live else 0
    dy = 0.6 * o * jit(t, 2, 14 + 30 * o) if o > 0.3 and live else 0
    st("pyr")["m"] = mul((1, 0, 0, 1, dx, dy), m_)
    hide("press", s < 0.02)
    rows_gone = 4 if nb <= 1.5 else 2 if nb <= 3 else 0          # the press rests on what is left (cap1 / cap2)
    top_y = P(0, H * (1 - rows_gone / ROWS), 0)[1]
    apex_y = B0 - (B0 - top_y) * sy
    st("press")["m"] = (1, 0, 0, 1, dx, apex_y - APEX[1] + dy)
    hide("puff", not (s > 0.02 and gr > 0.55))
    st("puff")["m"] = m_
    for i, th in enumerate(CRACK_AT):                 # static: how much GRIT
        hide(f"k{i + 1}", o < th)
    hide("ch1", o < 0.5)
    hide("ch2", o < 0.75)
    hide("chr", o < 0.75)
    # flips (the DSP's count since the last frame) shed debris and shift XOR slices; the design preview stands in a
    # steady stream (the C side starts a debris fall per frame with flips and keeps slices up while they come)
    df = ((1000 if o > 0.5 else 0) if flips is None else flips) if live else 0     # flips act on sound only
    nd = 0 if o <= 0.5 else math.ceil((o - 0.5) / 0.5 * len(DEBRIS))
    for i, dd in enumerate(DEBRIS):
        s0, land, g0 = debris_geo(*dd)
        T = 1.0 + 0.17 * i
        u = ((t + i * 0.41) % T) / T * 1.25          # 0..1 falling, then 0.25 of the period resting hidden
        on = i < nd and df > 0 and u <= 1
        u = min(u, 1)
        hide(f"d{i + 1}", not on)
        hide(f"ds{i + 1}", not on)
        st(f"d{i + 1}")["m"] = (1, 0, 0, 1, (land[0] - s0[0]) * u, (land[1] - s0[1]) * u * u)
        cx, cy = g0[0] + (land[0] - g0[0]) * u, g0[1] + (land[1] - g0[1]) * u
        sc = 0.35 + 0.65 * u
        st(f"ds{i + 1}")["m"] = (sc, 0, 0, sc, cx - sc * g0[0], cy - sc * g0[1])
    ns = 0 if df <= 0 else 1 if df < 3 else 2 if df < 7 else 3        # more flips, more slices
    if nb <= 3 and o > 0.6:
        ns = min(ns, 2)                                                # the busiest state: at most 2
    start = min(2, int((jit(t, 10, 7) + 1) * 1.5))                # which slices: rotate at 7 Hz
    pick = [(start + j) % 3 for j in range(ns)]
    for i in range(len(SLICES)):
        hide(f"g{i + 1}", i not in pick)
        n = jit(t, 20 + i, 7 + 9 * i)
        st(f"gs{i + 1}")["m"] = (1, 0, 0, 1, (4 + 14 * o) * (1 if n >= 0 else -1) * (0.4 + 0.6 * abs(n)), 0)
    # idle parallax: the far layer drifts against the pyramid (a slow camera orbit), 2 matrices
    if parallax:
        w = math.sin(2 * math.pi * t / 16)
        st("farg")["m"] = (1, 0, 0, 1, -3 * w, 0)
        st("sun")["m"] = (1, 0, 0, 1, -1.5 * w, 0)
    return St


HEADER = """
crush3d.svg - "crush" bitcrusher FX screen (3D). Our own artwork: a square pyramid in a low three-quarter view
(orthographic, yaw 30 deg, elevation 16 deg), lit from the front left, falling apart as the crusher gets heavier;
clean settings = pristine. Each knob has its own damage in its own colour. Generator + reference runtime:
design/fx/gen_crush3d.py (runtime_state is the per-frame truth; this header restates all of it).

ROUTE  TE's retained renderer only (notes/svg-route.md): no immediate-mode overlay. Load lazily (svg_open). Cache every
       id below once with find(doc, id, 0). Per frame: set visible flags (B[node + 0x50]), matrices (node_matrix, Q22.10,
       {a, b, 0, c, d, 0, e, f, 1} x 1024; x' = a x + c y + e, y' = b x + d y + f; the matrix REPLACES the node's own
       transform; none of the nodes below has one), recolours (write rgba into te_svg_fill / te_svg_stroke), then ONE
       svg_render. Every recoloured element has a unique colour value (+-1 per channel) so TE keeps separate style entries.
       Style getters assert on a node without that style: request only the ones listed (fill: / stroke:).

INPUTS r b s o = blue RATE, green BITS, white SQUASH, orange GRIT knobs, 0..1 (the DSP's smoothed knobs).
       nb, m = the DSP's bit depth and mu-law amount (crush_state bits_q12, mu; bits_dsp() here); mu = m > 0.25.
       gr = the SQUASH compressor's gain reduction, 0 (none) .. 1 (max), from the DSP, smoothed (the screen pumps with it).
       level = the input's peak 0..1 (the shake, slices and debris need a signal: level > 0.001); flips = the DSP's
       bit-flip events since the last frame.
       t = frame clock (s). jit(t, seed, rate) = noise -1..1 held for 1/rate s:
         k = floor(t rate); jit = frac(sin(k 12.9898 + seed 78.233) 43758.5453) 2 - 1   (any cheap hash is fine)

VISIBILITY (hide = clear the flag; everything not listed is static and always shown)
  smooth                 shown iff lvl == 0;  lvl = number of RATE detents passed of 4, 10, 18, 25 (r 32 >= d)
  vox1..vox4             vox{i} shown iff i == lvl (8 / 6 / 4 / 3 block tiers; vox1 without inner edges)  [blue RATE]
  c1 c2 c3 c4            c{i} shown iff !mu and i <= tiers; tiers = 4 (nb >= 12) 3 (>= 8) 2 (>= 4) 1 (>= 2) 0  [green]
  cmu                    shown iff mu (green mu-law courses)                                              [green]
  h{i} hw{i} hf{i}       set i (missing blocks: opening, inner wall, floor) shown iff nb < 8 / <= 5 / <= 3, i = 1 / 2 / 3
  cap1, pile             shown iff nb <= 3 (top 2 rows gone; tumbled blocks at the base)                 [green]
  cap2                   shown iff nb <= 1.5 (top 4 rows gone)                                           [green]
  k1..k6                 k{i} shown iff o >= .1 .25 .5 .6 .75 .9 (static: how much GRIT)                 [orange GRIT]
  ch1 / ch2, chr         shown iff o >= 0.5 / 0.75 (edge notches; chr = broken front-edge facets, with ch2)
  press                  shown iff s >= 0.02                                                             [white SQUASH]
  puff                   shown iff s >= 0.02 and gr > 0.55 (dust at the base corners)
  g1..g3, d1..d4, ds1..ds4   see GRIT below

MATRICES
  pyr    (whole pyramid + damage): translate(dx, dy) * scale(sx, sy) about (160, 104):
           q = s (0.45 + 0.55 gr); sy = 1 - 0.4 q (never below 0.6 of the height); sx = 1 + 0.18 q
           M = {sx, 0, 0, sy, 160 (1 - sx) + dx, 104 (1 - sy) + dy}
           dx = (0.5 + 1.5 o) jit(t, 1, 10 + 30 o) if o > 0.05 and level > 0.001 else 0;
           dy = 0.6 o jit(t, 2, 14 + 30 o) if o > 0.3 and level > 0.001 else 0
  press  translate(dx, top' - 23.43 + dy): top' = 104 - (104 - top) sy, top = 23.43 (apex) / 33.5 with cap1 / 43.57 with
         cap2 shown (the press rests on what is left)
  puff   {sx, 0, 0, sy, 160 (1 - sx), 104 (1 - sy)} (pyr's scale, no jitter)
  gs1..gs3   translate(A, 0), A = (4 + 14 o) sign(n) (0.4 + 0.6 |n|), n = jit(t, 20 + i, 7 + 9 i)   (i = 0, 1, 2 for gs1..gs3)
  d1..d4, ds1..ds4   (i = 0..3 for d1..d4) T = 1.0 + 0.17 i; u = frac((t + 0.41 i) / T) * 1.25
         d{i}  translate(DX u, DY u^2);  ds{i} = {k, 0, 0, k, X - k GX, Y - k GY}, k = 0.35 + 0.65 u,
               X = GX + (LX - GX) u, Y = GY + (LY - GY) u          (spawn = the cube's drawn position)
           i  DX      DY      GX      GY      LX      LY
           0  -30.67  42.59   119.43  107     88.76   105.27
           1  5.6     40.47   214.09  100     219.69  108.25
           2  -30.67  22.44   96.89   108.66  66.22   106.93
           3  5.6     64.64   187.05  102     192.64  110.25
  farg, sun  (optional idle parallax) translate(-3 w, 0) and translate(-1.5 w, 0), w = sin(2 pi t / 16); else identity

GRIT VISIBILITY (driven by the DSP's flips; the design preview stands in a steady stream while o > 0.5)
  g{i}   ns = 0 (no flips this frame) 1 (< 3) 2 (< 7) 3 slices, at most 2 when nb <= 3 and o > 0.6; shown: (start + j) mod 3
         for j < ns, start = floor((jit(t, 10, 7) + 1) 1.5) (0..2)
         (gx = the hole left behind, static; gs = the moving slice: gl gr shades swapped, go orange edge)
  d{i}, ds{i}   nd = ceil((o - 0.5) / 0.5 * 4) if o > 0.5 else 0 slots. Preview: shown iff i < nd, flips and
         frac((t + 0.41 i) / T) * 1.25 <= 1. C: an idle slot i < nd starts a fall (u from 0) on a frame with flips;
         u = frames since the start / (30 T); hidden again after u > 1.25

RECOLOURS (BITS posterise; colours are colors.svg / palette 0x01BAAF50 names). Pick the row by nb:
           A: nb >= 9 (pristine)  B: 5 <= nb < 9   C: 2 <= nb < 5   D: nb < 2 (1 bit)
  Neutral family only; green is BITS's accent (rim, rubble, mu-law courses); white belongs to SQUASH (press, puff).
  The shade face is black only at 1 bit (D), where the green rim outlines it; above, it never meets the black ground.
  L  faceL fill+stroke, vl1..vl4 fill, gr1..gr3 fill   purplegrey   lpg        lpg        lpg
  R  faceR fill, vr1..vr4 fill, gl1..gl3 fill          grey         offgrey    offgrey    black
     faceR stroke                                      = R          = R        = R        green (rim)
  T  vt1..vt4 fill, hf1..hf3 fill                      lpg          lpg        lpg        lpg
     hw1..hw3 fill                                     offgrey      black      offgrey    offgrey
     c1..c4 stroke                                     darkblue     black      black      black
     sun fill                                          darkblue     grey       grey       grey
     farL fill                                         darkblue     darkblue   offgrey    offgrey
     farR fill                                         offblue      darkblue   offgrey    offgrey
     shadow fill                                       offblue      black      black      black
     horizon stroke                                    grey         grey       lpg        lpg
     sand stroke                                       offgrey      grey       black      black
  (only the shown vox level needs its vt/vl/vr written; recolouring all four is also fine)

IDS (style each recoloured node owns)
  sky: sun (fill), farg { farL (fill), farR (fill) }, horizon (stroke), sand (stroke)
  pyr { shadow (fill); smooth { faceL (fill, stroke), faceR (fill, stroke), c1..c4 (stroke), cmu, ridge };
        vox1..4 { vt (fill), vl (fill), vr (fill), ve (blue edges) }; h1..3, hw1..3 (fill), hf1..3 (fill); k1..k6;
        ch1 ch2 chr; cap1/cap2 { capf, capt, capb }; pile { pt, pm, pk }; g1..g3 { gx, gs { gl (fill), gr (fill), go } };
        ds1..ds4; d1..d4 { dt, dl } }
  press { stem, plt, pll, plr }; puff
"""


def composite(out, name, k, t=0.3, gr=0.0, w=640, level=1.0, flips=None):
    St = runtime_state(k, t, gr, level=level, flips=flips)
    body = render(asset(), St).split(">", 1)[1].rsplit("</svg>", 1)[0]
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160">\n'
           '<rect width="320" height="160" fill="#000000"/>\n' + body +
           '<rect x="0" y="141" width="320" height="19" fill="#000000" opacity="0.55"/>\n</svg>\n')
    p = os.path.join(out, name + ".svg")
    open(p, "w").write(svg)
    subprocess.run([RSVG, "-w", "320", p, "-o", os.path.join(out, name + ".png")], check=True)
    subprocess.run([RSVG, "-w", str(w), p, "-o", os.path.join(out, name + "@2x.png")], check=True)
    os.remove(p)
    return count(asset(), St)


if __name__ == "__main__":
    a = asset()
    svg_path = os.path.join(HERE, "crush3d.svg")
    open(svg_path, "w").write('<?xml version="1.0" encoding="utf-8"?>\n<!--' + HEADER.replace("--", "-") + '-->\n' + render(a))
    print(svg_path, os.path.getsize(svg_path), "bytes,", count(a), "elements")
    if len(sys.argv) > 1:
        out = sys.argv[1]
        os.makedirs(out, exist_ok=True)
        for name, k, gr, cap_ in g2.STATES:
            composite(out, name, k, 0.3, gr)
