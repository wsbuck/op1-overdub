#!/usr/bin/env python3
"""Check an SVG against TE's retained-renderer subset (notes/svg-route.md). Usage: check_svg.py file.svg [colors.svg]"""
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
    """A colors.svg colour, or one within +-1 per channel (unique values keep TE from sharing a style entry)."""
    if not (len(v) == 7 and v.startswith("#")):
        return False
    try:
        rgb = [int(v[j:j + 2], 16) for j in (1, 3, 5)]
    except ValueError:
        return False
    for p in pal:
        q = [int(p[j:j + 2], 16) for j in (1, 3, 5)]
        if all(abs(a - b) <= 1 for a, b in zip(rgb, q)):
            return True
    return False


def main():
    path = sys.argv[1]
    cpath = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_COLORS
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
                errs.append(f"{where}: colour {v} not in colors.svg (within +-1 per channel)")
            if k == "d":
                bad = set(re.findall(r"[A-Za-z]", v)) - set("MLHVCSZmlhvcsz")
                if bad:
                    errs.append(f"{where}: path commands {sorted(bad)} not supported")
                # TE's parser quirks (notes/svg-route.md "Path quirks"): H / V and relative commands are drawn wrongly
                # (stray joins on the device, CRUSH7); every M after the first closes the open subpath before it with
                # a line back to its start, so in a stroked path an open subpath of more than one segment gets a chord
                rel = set(re.findall(r"[hvmlcsz]", v))
                if rel:
                    errs.append(f"{where}: relative path commands {sorted(rel)}: use absolute ones")
                subs = [x for x in re.split(r"(?=M)", v.strip()) if x.strip()]
                if any(re.search(r"[HV]", sp) and "Z" not in sp for sp in subs[:-1]):
                    errs.append(f"{where}: an open subpath with H / V followed by M: TE joins it to the next subpath "
                                "(CRUSH7); use L, or close it (pedals' closed H / V shapes are fine)")
                if el.get("stroke", "none") != "none":
                    if len(subs) > 1:
                        for sp in subs:
                            if "Z" not in sp and len(re.findall(r"[LC]", sp)) + len(re.findall(r"[HV]", sp)) > 1:
                                errs.append(f"{where}: stroked open subpath of several segments followed by M (TE "
                                            f"closes it with a chord): {sp[:40].strip()}..")
                                break
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
