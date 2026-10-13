#!/usr/bin/env python3
"""Find code that builds an absolute address in a range, from an objdump disassembly.

  xrefs.py DIS LO HI [--window N]

Blackfin code loads 32-bit constants as two halves (`P0.L = 0x5a04; P0.H = 0x18f;`, either order,
not always adjacent). This pairs .L/.H loads of the same register within N instructions (default 8)
and prints each pair whose combined value is in [LO, HI), plus 32-bit `Rx = 0x...` immediates.
Data words (pointers stored in tables) are not covered: search the image for those.
"""
import re
import sys

LINE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*(.*)$")
HALF = re.compile(r"\b([RP][0-7]|[IMLB][0-3]|SP|FP)\.([HL]) = (-?0x[0-9a-f]+|-?\d+);")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    window = int(sys.argv[sys.argv.index("--window") + 1]) if "--window" in sys.argv else 8
    if "--window" in sys.argv:
        args.remove(str(window))
    if len(args) != 3:
        sys.exit(__doc__)
    path, lo, hi = args[0], int(args[1], 0), int(args[2], 0)
    recent = []                                   # (index, addr, reg, half, value)
    hits = set()
    for i, line in enumerate(open(path, errors="replace")):
        m = LINE.match(line)
        if not m:
            recent.clear()
            continue
        addr, text = int(m.group(1), 16), m.group(2)
        for reg, half, val in HALF.findall(text):
            v = int(val, 0) & 0xFFFF
            for j, a2, r2, h2, v2 in reversed(recent):
                if i - j > window:
                    break
                if r2 == reg and h2 != half:
                    full = (v << 16 | v2) if half == "H" else (v2 << 16 | v)
                    if lo <= full < hi:
                        hits.add((min(a2, addr), reg, full))
                    break
            recent.append((i, addr, reg, half, v))
        recent[:] = [r for r in recent if i - r[0] <= window]
    for a, reg, full in sorted(hits):
        print(f"0x{a:08x}  {reg} = 0x{full:08x}")


if __name__ == "__main__":
    main()
