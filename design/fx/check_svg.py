#!/usr/bin/env python3
"""Check an SVG against TE's retained-renderer subset and its path quirks (notes/svg-route.md).
Every element must be one subpath (TE joins subpaths, Verified test 78).
Usage: check_svg.py file.svg [colors.svg] [--card: the test card, several subpaths allowed]"""
import os, re, sys
import xml.etree.ElementTree as ET

ELEMS = {"svg", "g", "line", "polyline", "polygon", "circle", "ellipse", "rect", "path"}
COMMON = {"id", "fill", "stroke", "stroke-width", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "opacity", "transform"}
GEOM = {"svg": {"x", "y", "width", "height", "viewBox", "version", "xmlns"}, "g": set(),
        "line": {"x1", "y1", "x2", "y2"}, "polyline": {"points"}, "polygon": {"points"}, "circle": {"cx", "cy", "r"},
        "ellipse": {"cx", "cy", "rx", "ry"}, "rect": {"x", "y", "width", "height"}, "path": {"d"}}
NUMERIC = {"x", "y", "width", "height", "x1", "y1", "x2", "y2", "cx", "cy", "r", "rx", "ry", "stroke-width",
           "stroke-miterlimit", "opacity", "points", "d", "transform", "viewBox"}
DEFAULT_COLORS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "firmware", "work", "246", "content", "display", "colors.svg")   # from your own stock image (op1pack.py unpack)


def near_palette(v, pal):
    """A colors.svg colour, or one within +-2 per channel (unique values keep TE from sharing a style entry)."""
    if not (len(v) == 7 and v.startswith("#")):
        return False
    try:
        rgb = [int(v[j:j + 2], 16) for j in (1, 3, 5)]
    except ValueError:
        return False
    for p in pal:
        q = [int(p[j:j + 2], 16) for j in (1, 3, 5)]
        if all(abs(a - b) <= 2 for a, b in zip(rgb, q)):
            return True
    return False


CARD = "--card" in sys.argv         # the test card: several subpaths on purpose (the subset rules still apply)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path = args[0]
    cpath = args[1] if len(args) > 1 else DEFAULT_COLORS
    pal = {c.upper() for c in re.findall(r'fill="(#[0-9A-Fa-f]{6})"', open(cpath).read())}
    errs, n_el, ids = [], 0, set()
    root = ET.parse(path).getroot()
    for el in root.iter():
        tag = el.tag.split("}")[-1]
        where = f"<{tag} id={el.get('id')}>"
        if tag not in ELEMS:
            errs.append(f"{where}: element not supported")
            continue
        if tag not in ("svg", "g"):
            n_el += 1
        for k, v in el.attrib.items():
            k = k.split("}")[-1]
            if k not in COMMON | GEOM[tag]:
                errs.append(f"{where}: attribute {k} not supported")
            if k in NUMERIC:
                for num in re.findall(r"-?\d*\.?\d+(?:[eE][-+]?\d+)?\.?", v):
                    if "e" in num.lower():
                        errs.append(f"{where}: exponent in {k}: {num}")
                    if num.endswith("."):
                        errs.append(f"{where}: bare decimal point in {k}: {num}")
                    if "." in num and len(num.split(".")[1]) > 4:
                        errs.append(f"{where}: > 4 decimals in {k}: {num}")
                    if abs(float(num)) >= 2 ** 21:
                        errs.append(f"{where}: |value| >= 2^21 in {k}")
            if k in ("fill", "stroke") and v != "none" and not near_palette(v, pal):
                errs.append(f"{where}: colour {v} not in colors.svg (within +-2 per channel)")
            if k == "d":
                bad = set(re.findall(r"[A-Za-z]", v)) - set("MLHVCSZmlhvcsz")
                if bad:
                    errs.append(f"{where}: path commands {sorted(bad)} not supported")
                # TE's parser (notes/svg-route.md "Path quirks", Verified on the device, test 78): every M after the
                # first closes the subpath before it and draws a line to the new point (the pen never lifts), so
                # every element must be one subpath (strictify.py converts). Relative commands: avoided.
                rel = set(re.findall(r"[hvmlcsz]", v))
                if rel:
                    errs.append(f"{where}: relative path commands {sorted(rel)}: use absolute ones")
                subs = [x for x in re.split(r"(?=M)", v.strip()) if x.strip()]
                if not CARD and len(subs) > 1:
                    errs.append(f"{where}: {len(subs)} subpaths: TE joins them (one subpath per element; "
                                "design/fx/strictify.py)")
            if k == "stroke-linejoin" and v not in ("miter", "round", "belvel"):
                errs.append(f"{where}: stroke-linejoin {v!r}: TE knows miter, round and 'belvel' (its spelling; "
                            "'bevel' is not recognised)")
            if k == "transform":
                for fn, args in re.findall(r"(\w+)\(([^)]*)\)", v):
                    if fn not in ("matrix", "translate", "scale"):
                        errs.append(f"{where}: transform {fn}() not supported")
                    if "," in args:
                        errs.append(f"{where}: commas in transform (use spaces)")
            if k == "id":
                if not v.isascii():
                    errs.append(f"{where}: non-ASCII id")
                if v in ids:
                    errs.append(f"{where}: duplicate id")
                ids.add(v)
        if tag == "svg" and el.get("viewBox") != "0 0 320 160":
            errs.append("svg: viewBox should be 0 0 320 160")
    for e in errs:
        print("FAIL", e)
    print(f"{path}: {n_el} drawable elements, {len(ids)} ids, {len(errs)} problems -> {'PASS' if not errs else 'FAIL'}")
    sys.exit(1 if errs else 0)


main()
