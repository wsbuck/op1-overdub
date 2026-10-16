#!/usr/bin/env python3
"""crushcard.svg: a test card for TE's SVG route (device ground truth), shown for ~12 s on the first open of the crush
FX screen after boot (ui.c), plus src/slot/crushcard.h: where to draw each case's number and its expected result
(1-px framebuffer lines, which do not go through TE's SVG parser or AGG).

  python3 design/fx/gen_card.py

Grid of 18 cases (6 x 3, 52 x 46 units each): the number top left, the SVG case on the left half, the expected result
(outlines, grey) on the right half. One photo shows which cases TE draws differently. Cases (also in CASES below):
   1 path, stroked polyline M L L (one subpath)           10 polygon, filled
   2 path, two open subpaths M L L M L L, stroked          11 polyline, stroked (3 points)
   3 path, single segments M L M L M L, stroked            12 line, and a stroked rect
   4 path, open H / V (one subpath), stroked               13 acute angle, stroke-width 3, miter join
   5 path, open H then M, H then M, V, stroked             14 same, round join
   6 path, closed H / V (Z), filled                        15 same, belvel join (TE's spelling)
   7 path, two closed disjoint subpaths, filled            16 strokes 0.5 and 0.8 wide
   8 path, closed triangle Z then open M L L, stroked      17 a square under a group matrix (the C scales it to half
   9 path, square with a square hole (reversed), filled       size about its centre)
                                                           18 a grey square the C recolours green
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
W, H, X0, Y0 = 52, 46, 4, 2
LPG, GREY, WHITE, BLUE = "#E6E7FF", "#7C7990", "#FFFFFF", "#698EFF"


def cell(n):
    c, r = (n - 1) % 6, (n - 1) // 6
    return X0 + c * W, Y0 + r * H


def f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def tr(d, dx, dy):
    """Shift absolute path data (M L H V Z, numbers in pairs except after H / V)."""
    out, toks, i, cmd = [], re.findall(r"[A-Za-z]|-?\d*\.?\d+", d), 0, "M"
    for t in toks:
        if t.isalpha():
            cmd = t
            out.append(t)
            i = 0
            continue
        v = float(t)
        if cmd == "H" or (cmd != "V" and i % 2 == 0):
            v += dx
        else:
            v += dy
        out.append(f(v))
        i += 1
    s = ""
    for t in out:                                   # "M2 4 L20 4": a command letter sticks to its first number
        s += t if (s and s[-1].isalpha()) else (" " + t if s else t)
    return s


# n: (svg element builder given (dx, dy), expected polylines [(points, closed)] in case coordinates 0..22 x 0..30)
def stroke(d, **a):
    a = dict({"stroke_width": "1"}, **a)
    extra = "".join(f' {k.replace("_", "-")}="{v}"' for k, v in a.items())
    return lambda dx, dy, n: f'<path id="k{n}" d="{tr(d, dx, dy)}" fill="none" stroke="{LPG}"{extra}/>'


def fill(d):
    return lambda dx, dy, n: f'<path id="k{n}" d="{tr(d, dx, dy)}" fill="{LPG}"/>'


ACUTE = "M4 28 L11 3 L18 28"
CASES = {
    1: (stroke("M2 26 L10 4 L20 26"), [([(2, 26), (10, 4), (20, 26)], False)]),
    2: (stroke("M1 28 L5 12 L9 28 M12 28 L16 12 L20 28"), [([(1, 28), (5, 12), (9, 28)], False),
                                                        ([(12, 28), (16, 12), (20, 28)], False)]),
    3: (stroke("M2 4 L20 4 M2 14 L20 14 M2 24 L20 24"), [([(2, 4), (20, 4)], False), ([(2, 14), (20, 14)], False),
                                                      ([(2, 24), (20, 24)], False)]),
    4: (stroke("M2 4 H20 V26"), [([(2, 4), (20, 4), (20, 26)], False)]),
    5: (stroke("M2 6 H20 M2 16 H20 M11 21 V30"), [([(2, 6), (20, 6)], False), ([(2, 16), (20, 16)], False),
                                                ([(11, 21), (11, 30)], False)]),
    6: (fill("M2 4 H20 V26 H2 Z"), [([(2, 4), (20, 4), (20, 26), (2, 26)], True)]),
    7: (fill("M2 2 L10 2 L10 12 L2 12 Z M12 16 L20 16 L20 28 L12 28 Z"),
        [([(2, 2), (10, 2), (10, 12), (2, 12)], True), ([(12, 16), (20, 16), (20, 28), (12, 28)], True)]),
    8: (stroke("M2 2 L20 2 L11 12 Z M2 18 L11 28 L20 18"), [([(2, 2), (20, 2), (11, 12)], True),
                                                         ([(2, 18), (11, 28), (20, 18)], False)]),
    9: (fill("M2 2 L20 2 L20 28 L2 28 Z M7 8 L7 22 L15 22 L15 8 Z"),
        [([(2, 2), (20, 2), (20, 28), (2, 28)], True), ([(7, 8), (15, 8), (15, 22), (7, 22)], True)]),
    10: (lambda dx, dy, n: f'<polygon id="k{n}" points="{f(2 + dx)},{f(26 + dy)} {f(11 + dx)},{f(4 + dy)} '
                           f'{f(20 + dx)},{f(26 + dy)}" fill="{LPG}"/>', [([(2, 26), (11, 4), (20, 26)], True)]),
    11: (lambda dx, dy, n: f'<polyline id="k{n}" points="{f(2 + dx)},{f(4 + dy)} {f(11 + dx)},{f(26 + dy)} '
                           f'{f(20 + dx)},{f(4 + dy)}" fill="none" stroke="{LPG}" stroke-width="1"/>',
         [([(2, 4), (11, 26), (20, 4)], False)]),
    12: (lambda dx, dy, n: f'<line id="k{n}" x1="{f(2 + dx)}" y1="{f(4 + dy)}" x2="{f(20 + dx)}" y2="{f(10 + dy)}" '
                           f'stroke="{LPG}" stroke-width="1"/><rect id="k{n}r" x="{f(2 + dx)}" y="{f(14 + dy)}" '
                           f'width="18" height="12" fill="none" stroke="{LPG}" stroke-width="1"/>',
         [([(2, 4), (20, 10)], False), ([(2, 14), (20, 14), (20, 26), (2, 26)], True)]),
    13: (stroke(ACUTE, stroke_width="3", stroke_linejoin="miter"), [([(4, 28), (11, 3), (18, 28)], False)]),
    14: (stroke(ACUTE, stroke_width="3", stroke_linejoin="round"), [([(4, 28), (11, 3), (18, 28)], False)]),
    15: (stroke(ACUTE, stroke_width="3", stroke_linejoin="belvel"), [([(4, 28), (11, 3), (18, 28)], False)]),
    16: (lambda dx, dy, n: f'<line id="k{n}" x1="{f(2 + dx)}" y1="{f(10 + dy)}" x2="{f(20 + dx)}" y2="{f(10 + dy)}" '
                           f'stroke="{LPG}" stroke-width="0.5"/><line id="k{n}b" x1="{f(2 + dx)}" y1="{f(20 + dy)}" '
                           f'x2="{f(20 + dx)}" y2="{f(20 + dy)}" stroke="{LPG}" stroke-width="0.8"/>',
         [([(2, 10), (20, 10)], False), ([(2, 20), (20, 20)], False)]),
    17: (lambda dx, dy, n: f'<g id="k{n}"><path id="k{n}s" d="M{f(2 + dx)} {f(4 + dy)} L{f(20 + dx)} {f(4 + dy)} '
                           f'L{f(20 + dx)} {f(26 + dy)} L{f(2 + dx)} {f(26 + dy)} Z" fill="{LPG}"/></g>',
         [([(6.5, 9.5), (15.5, 9.5), (15.5, 20.5), (6.5, 20.5)], True)]),
    18: (lambda dx, dy, n: f'<path id="k{n}" d="M{f(2 + dx)} {f(4 + dy)} L{f(20 + dx)} {f(4 + dy)} L{f(20 + dx)} '
                           f'{f(26 + dy)} L{f(2 + dx)} {f(26 + dy)} Z" fill="{GREY}"/>',
         [([(2, 4), (20, 4), (20, 26), (2, 26)], True)]),
}
CASE_DX, EXP_DX, DY = 2, 28, 12      # case and expected boxes inside a cell; numbers at (1, 2)


def main():
    els, segs, nums = [], [], []
    for n, (mk, exp) in CASES.items():
        x, y = cell(n)
        els.append(mk(x + CASE_DX, y + DY, n))
        nums.append((x + 1, y + 2))
        for pts, closed in exp:
            for p, q in zip(pts, pts[1:] + ([pts[0]] if closed else [])):
                segs.append(tuple(int(round(v * 4)) for v in (x + EXP_DX + p[0], y + DY + p[1], x + EXP_DX + q[0],
                                                              y + DY + q[1])))
    cx, cy = cell(17)
    svg = ('<?xml version="1.0" encoding="utf-8"?>\n<!-- crushcard.svg: TE SVG-route test card (design/fx/gen_card.py; '
           'cases in its docstring). Shown ~12 s on the first open of the crush FX screen after boot. -->\n'
           '<svg version="1.1" xmlns="http://www.w3.org/2000/svg" x="0" y="0" width="320" height="160" '
           'viewBox="0 0 320 160">\n  ' + "\n  ".join(els) + "\n</svg>\n")
    open(os.path.join(HERE, "crushcard.svg"), "w").write(svg)
    h = ["/* Generated by design/fx/gen_card.py: crushcard.svg's case numbers and expected results (SVG units x 4). */",
         f"#define CARD_N {len(nums)}", f"#define CARD_SEGS {len(segs)}",
         "static const int16_t card_num[CARD_N][2] = {", "    " + ", ".join(f"{{{x}, {y}}}" for x, y in nums) + " };",
         "static const int16_t card_seg[CARD_SEGS][4] = {"]
    h += [f"    {{{a}, {b}, {c}, {d}}}," for a, b, c, d in segs] + ["};"]
    h += [f"#define CARD_GM_CX {cx + CASE_DX + 11}    /* case 17: the group scales to half about its square's centre */",
          f"#define CARD_GM_CY {cy + DY + 15}"]
    open(os.path.join(HERE, "..", "..", "src", "slot", "crushcard.h"), "w").write("\n".join(h) + "\n")
    print("crushcard.svg:", len(els), "cases,", len(segs), "expected segments")


if __name__ == "__main__":
    main()
