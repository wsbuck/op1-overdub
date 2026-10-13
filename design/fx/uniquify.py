#!/usr/bin/env python3
"""Give the recoloured fills/strokes of an FX SVG colour values used nowhere else in the document (each channel
moved by at most 1, which check_svg.py accepts), in case TE's renderer shares one style entry between elements of
equal colour: the runtime then recolours only what it means to.
  python3 design/fx/uniquify.py FILE.svg id:fill id:stroke ..."""
import itertools
import re
import sys


def main():
    path, targets = sys.argv[1], sys.argv[2:]
    src = open(path).read()
    used = {m.lower() for m in re.findall(r'(?:fill|stroke)="(#[0-9A-Fa-f]{6})"', src)}
    for t in targets:
        i, attr = t.split(":")
        m = re.search(r'(<[^>]*\bid="%s"[^>]*>)' % re.escape(i), src)
        if not m:
            sys.exit(f"{i}: no such id")
        tag = m.group(1)
        c = re.search(r'\b%s="(#[0-9A-Fa-f]{6})"' % attr, tag)
        if not c:
            sys.exit(f"{i}: no {attr} colour")
        rgb = [int(c.group(1)[k:k + 2], 16) for k in (1, 3, 5)]
        same = sum(1 for v in re.findall(r'(?:fill|stroke)="(#[0-9A-Fa-f]{6})"', src) if v.lower() == c.group(1).lower())
        if same == 1:
            continue                                        # already unique
        for d in sorted(itertools.product((0, 1, -1), repeat=3), key=lambda d: sum(map(abs, d))):
            new = [min(255, max(0, v + e)) for v, e in zip(rgb, d)]
            h = "#%02X%02X%02X" % tuple(new)
            if h.lower() not in used:
                break
        else:
            sys.exit(f"{i}: no free colour near {c.group(1)}")
        used.add(h.lower())
        new_tag = tag[:c.start(1)] + h + tag[c.end(1):]
        src = src[:m.start(1)] + new_tag + src[m.end(1):]
    open(path, "w").write(src)


if __name__ == "__main__":
    main()
