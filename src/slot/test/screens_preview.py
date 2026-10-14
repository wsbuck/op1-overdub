#!/usr/bin/env python3
"""Render the harm and fm4 screens (src/slot/ui_harm.h, ui_fm4.h) on the host to PNGs, before they are included into ui.c.

  python3 src/slot/test/screens_preview.py OUTDIR

Builds screens_main.c natively (stand-ins for ui.c's primitives; harm's table is really built by harm_build.h) and
draws each segment at the device's scale like preview.py: SVG 320 x 160 -> 376 x 188 panel (x1.175), x3 for viewing,
on black, the unseen bottom (y > 142) shaded. Fast (1-px) lines are drawn one panel pixel wide, as on the device.
Writes one PNG per named setting below and prints the AA / fast line counts (AA lines cost ~20k cycles each on device).
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
ENGINE = os.path.join(os.path.dirname(SLOT), "engine")
SCALE = 1.175 * 3
PALETTE = {0: (255, 144, 167), 1: (1, 0, 0), 2: (255, 58, 93), 3: (78, 40, 50), 4: (53, 50, 56), 5: (0, 71, 91),
           6: (0, 40, 38), 7: (95, 66, 112), 8: (174, 177, 220), 9: (191, 216, 255), 10: (56, 53, 114),
           11: (29, 32, 64), 12: (124, 121, 144), 13: (255, 66, 63), 14: (255, 255, 255), 15: (105, 142, 255),
           16: (0, 237, 149), 17: (230, 231, 255), 18: (128, 255, 175), 19: (255, 179, 179)}   # = preview.py


def s16(v):
    return v - 65536 if v >= 32768 else v


def harm(levels, uni=0, warp=0, tilt=0, extend=0):
    lv = list(levels) + [0] * (24 - len(levels))
    k = [(uni & 63) | (warp & 63) << 6 | 1 << 12, (tilt & 31) | (extend & 15) << 5]
    k += [sum((lv[4 * j + i] & 15) << (4 * i) for i in range(4)) for j in range(6)]
    return [s16(v) for v in k]


def op(coarse, fine=0, level=15, env=0):
    return s16(0x8000 | coarse << 10 | fine << 7 | level << 3 | env)


def alg(a):
    return ((a - 1) * 32768 + 16384) // 11


SAW = [15, 13, 12, 11, 10, 10, 9, 9, 9, 8, 8, 8, 8, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6]
ORGAN = [15, 15, 0, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
BELL = [15, 0, 0, 9, 0, 0, 12, 0, 0, 0, 6, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 1]
CHOIR = [12, 13, 14, 15, 14, 12, 9, 6, 4, 2, 1, 0, 3, 5, 6, 5, 3, 1, 0, 0, 0, 0, 0, 0]
SETTINGS = [
    ("harm_saw", "harm", harm(SAW), ["cursor=5"]),
    ("harm_organ_unison", "harm", harm(ORGAN, uni=40), ["cursor=3"]),
    ("harm_paint", "harm", harm(CHOIR, warp=20), ["cursor=9", "trail=4:9"]),
    ("harm_tilt", "harm", harm(BELL, tilt=-8), ["cursor=10", "last=4"]),
    ("harm_extend", "harm", harm(SAW, extend=11, uni=63), ["cursor=23", "last=5"]),
    ("harm_stamp", "harm", harm([15, 0, 12, 0, 10, 0, 9, 0, 9, 0, 8, 0, 8, 0, 7, 0, 7, 0, 6, 0, 6, 0, 6, 0]),
     ["cursor=2", "last=6", "stamp=2"]),
    ("harm_badges", "harm", harm(SAW, uni=20), ["cursor=5", "badges=1"]),
    ("fm4_v0_alg1", "fm4", [16384, alg(1), 16384, 8000, 2048, 3072, 1024, 0], ["alg_shown=1"]),
    ("fm4_v0_alg5_detune", "fm4", [12000, alg(5), 8000, 0, 1024 + 512, 4096, 2048, 20000], []),
    ("fm4_alg2_epiano", "fm4", [14000, alg(2), 20000, 3000, op(1), op(1, 0, 13, 2), op(14, 0, 9, 1), op(1, 1, 11, 6)],
     []),
    ("fm4_alg6", "fm4", [20000, alg(6), 16384, 12000, op(1), op(2, 0, 12, 5), op(3, 0, 10, 3), op(5, 4, 8, 1)],
     ["alg_shown=1"]),
    ("fm4_alg8_page_c", "fm4", [16384, alg(8), 16384, 20000, op(1), op(2), op(4, 5, 11, 7), op(7, 0, 9, 2)],
     ["page=3"]),
    ("fm4_alg11_organ", "fm4", [0, alg(11), 16384, 0, op(1), op(2, 0, 12), op(4, 0, 10), op(8, 0, 8)], []),
    ("fm4_badges", "fm4", [16384, alg(9), 16384, 32767, op(1), op(3), op(5), op(1, 0, 15, 4)], ["badges=1"]),
]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "screens"
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp()
    exe = os.path.join(tmp, "screens")
    subprocess.run(["cc", "-std=gnu99", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", SLOT,
                    "-I", ENGINE, os.path.join(HERE, "screens_main.c"), "-o", exe], check=True)
    for name, kind, k, extra in SETTINGS:
        res = subprocess.run([exe, kind] + [str(v) for v in k] + extra, check=True, capture_output=True, text=True)
        size = (int(320 * SCALE), int(160 * SCALE))
        img = Image.new("RGB", size, (0, 0, 0))
        d = ImageDraw.Draw(img, "RGBA")
        d.rectangle([0, 142 * SCALE, 320 * SCALE, 160 * SCALE], fill=(40, 0, 0, 140))
        f = SCALE / 1024
        for s in res.stdout.split("\n"):
            if not s:
                continue
            x0, y0, x1, y1, w, c = (int(v) for v in s.split())
            width = round(SCALE / 1.175) if w < 0 else max(1, round(w * f))
            d.line([x0 * f, y0 * f, x1 * f, y1 * f], fill=PALETTE.get(c, (255, 0, 255)), width=width)
        path = os.path.join(out, name + ".png")
        img.save(path)
        print(f"{path}: {res.stderr.strip()}")


if __name__ == "__main__":
    main()
