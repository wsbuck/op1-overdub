#!/usr/bin/env python3
"""Rewrite an SVG so every element is ONE subpath (TE's path parser draws a line from each subpath to the next M,
notes/svg-route.md "Path quirks", Verified on the device, test 78). In place; idempotent; the look is unchanged.

  python3 design/fx/strictify.py FILE.svg [..] [--as-fill ID,ID]

Per <path> whose d has several subpaths (absolute M L H V C Z only, as check_svg.py requires):
- fill only: one subpath, the pieces joined by bridges from start to start and back along the same lines (the
  two edges cancel, so the fill is the pieces' own; same id and colours, nothing for the runtime to change);
- stroked, every piece one straight segment, butt caps, no two pieces overlapping, no id (or named in --as-fill: the
  runtime does not recolour its stroke): one filled subpath of the
  segments' rectangles (exactly the butt stroke), bridged as above (no new elements);
- otherwise stroked: a <g> with the id and transform holding one <path> per piece (ids id_1, id_2, ..; each with
  the original colours and stroke attributes). The runtime must recolour those leaves, not the group.
Prints what it changed."""
import math
import re
import sys

NUM = r"-?\d*\.?\d+(?:[eE][-+]?\d+)?"


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def subpaths(d):
    return [s.strip() for s in re.split(r"(?=M)", d.strip()) if s.strip()]


def start(sp):
    x, y = re.match(r"M\s*(%s)[\s,]+(%s)" % (NUM, NUM), sp).groups()
    return float(x), float(y)


def body(sp):
    """The piece after its M point, with Z replaced by an explicit line back to its start (no Z mid-path)."""
    x, y = start(sp)
    rest = re.sub(r"^M\s*%s[\s,]+%s" % (NUM, NUM), "", sp).strip()
    rest = re.sub(r"\s*Z\s*$", "", rest)
    return (rest + " " if rest else "") + f"L{f(x)} {f(y)}"


def bridged(pieces):
    """One subpath: M s1 <piece 1> L s2 <piece 2> .. L sn <piece n>, then back L s(n-1) .. L s1 and Z: every bridge is
    drawn once each way along the same line, so the two edges cancel and the fill is the pieces' own."""
    starts = [start(sp) for sp in pieces]
    out = f"M{f(starts[0][0])} {f(starts[0][1])} " + body(pieces[0])
    for (x, y), sp in zip(starts[1:], pieces[1:]):
        out += f" L{f(x)} {f(y)} " + body(sp)
    for x, y in reversed(starts[:-1]):
        out += f" L{f(x)} {f(y)}"
    return out + " Z"


def segment(sp):
    m = re.fullmatch(r"M\s*(%s)[\s,]+(%s)\s*L\s*(%s)[\s,]+(%s)" % (NUM, NUM, NUM, NUM), sp)
    return tuple(map(float, m.groups())) if m else None


def rect(seg, w):
    x0, y0, x1, y1 = seg
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy) or 1.0
    nx, ny = -dy / n * w / 2, dx / n * w / 2
    pts = [(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)]
    return "M" + " L".join(f"{f(x)} {f(y)}" for x, y in pts) + " Z"


def overlap(rects):
    def box(r):
        p = [tuple(map(float, q)) for q in re.findall(r"(%s) (%s)" % (NUM, NUM), r)]
        return min(x for x, _ in p), min(y for _, y in p), max(x for x, _ in p), max(y for _, y in p)
    bs = [box(r) for r in rects]
    return any(a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3] for i, a in enumerate(bs) for b in bs[i + 1:])


def attrs(tag):
    return re.findall(r'([\w:-]+)="([^"]*)"', tag)


def convert(src, as_fill=()):
    log = []

    def one(m):
        indent, tag = m.group(1), m.group(2)
        a = dict(attrs(tag))
        pieces = subpaths(a.get("d", ""))
        if len(pieces) <= 1:
            return m.group(0)
        stroked = a.get("stroke", "none") != "none"
        filled = a.get("fill", "black") != "none"
        name = a.get("id")
        if not stroked:
            a["d"] = bridged(pieces)
            log.append(f"{name}: {len(pieces)} filled pieces bridged into one subpath")
            return indent + "<path " + " ".join(f'{k}="{v}"' for k, v in a.items()) + "/>"
        segs = [segment(p) for p in pieces]
        w = float(a.get("stroke-width", "1"))
        if (not filled and (not name or name in as_fill) and all(segs) and a.get("stroke-linecap", "butt") == "butt"
                and a.get("stroke-linejoin", "miter") in ("miter", "round", "belvel")):
            rects = [rect(s, w) for s in segs]
            if not overlap(rects):
                b = {k: v for k, v in a.items() if not k.startswith("stroke")}
                b["fill"] = a["stroke"]
                b["d"] = bridged(rects)
                log.append(f"{name or 'no id'} ({a['stroke']}): {len(pieces)} butt segments as one filled subpath of "
                           "rectangles")
                return indent + "<path " + " ".join(f'{k}="{v}"' for k, v in b.items()) + "/>"
        g = {k: a[k] for k in ("id", "transform", "display", "opacity") if k in a}
        leaf = {k: v for k, v in a.items() if k not in g and k != "d"}
        kids = []
        for j, p in enumerate(pieces):
            kid = ({"id": f"{name}_{j + 1}"} if name else {}) | {"d": p} | leaf
            kids.append(indent + "  <path " + " ".join(f'{k}="{v}"' for k, v in kid.items()) + "/>")
        log.append(f"{name}: {len(pieces)} stroked pieces as a group of {len(pieces)} paths")
        head = "<g" + "".join(f' {k}="{v}"' for k, v in g.items()) + ">"
        return indent + head + "\n" + "\n".join(kids) + "\n" + indent + "</g>"

    out = re.sub(r"([ \t]*)(<path\b[^>]*?/>)", one, src)
    return out, log


def strictify_file(path, as_fill=()):
    """For the generators: rewrite path in place; returns the log."""
    src = open(path).read()
    out, log = convert(src, as_fill)
    if out != src:
        open(path, "w").write(out)
    return log


def main():
    args, as_fill = sys.argv[1:], ()
    if "--as-fill" in args:
        i = args.index("--as-fill")
        as_fill = tuple(args[i + 1].split(","))
        del args[i:i + 2]
    for p in args:
        src = open(p).read()
        out, log = convert(src, as_fill)
        if out != src:
            open(p, "w").write(out)
        print(f"{p}: " + ("; ".join(log) if log else "already one subpath per element"))


if __name__ == "__main__":
    main()
