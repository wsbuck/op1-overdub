#!/usr/bin/env python3
"""Render src/slot/ui.c on the host to PNGs, to check layouts before flashing.

  python3 src/slot/test/preview.py OUTDIR [ENGINE K0 K1 K2 K3 ...]

ENGINE >= 100 previews our FX screens: 100 + FX id for the master (stereo) screen, 200 + FX id for synth (mono);
300 the drum probe's screen (drum_ui_draw, sample values); 301 fmdrum's (FD_PARAMS, FD_KEY, FD_PLAY, FD_LIT: see
preview_host.c; NAME = the PNG's file name).

Builds ui.c natively with stand-ins for TE (te_line records segments), draws them at the device's scale
(SVG 320 x 160 -> 376 x 188 panel, ×1.175) on black, with the unseen bottom (y > 142) shaded.
CLEAN=1: screenshots for the README (no HOLD/CHORD badges, cropped to what the panel shows).
Palette colours are TE's colors.svg values (earlier guesses were wrong: offgrey is near-black, purplegrey bright). Default: a few wt and va knob settings.
"""
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

DESIGN = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))), "design")
SVG_PATHS = {"choir.svg": "fx", "pingpong.svg": "fx", "pedals.svg": "fx", "crush3d.svg": "fx", "fm4.svg": "synth"}   # name -> design/ subfolder
SVGS = tuple(SVG_PATHS)
SVG_DIR = None                       # main(): a temp folder holding every document (preview_host looks there by name)
NS = "{http://www.w3.org/2000/svg}"
RSVG = "/opt/homebrew/bin/rsvg-convert"


def svg_ids(path):
    """'doc id fill stroke' per element with an id, for preview_host's node table."""
    lines = []
    for name in SVGS:
        for el in ET.parse(os.path.join(SVG_DIR, name)).iter():
            i = el.get("id")
            if i:
                col = [el.get(a, "-") for a in ("fill", "stroke")]
                col = [c[1:] if c.startswith("#") else "-" for c in col]
                lines.append(f"{name} {i} {col[0]} {col[1]}")
    open(path, "w").write("\n".join(lines) + "\n")


def svg_base(nodes, size, tmp):
    """The SVG with the drawn node state applied (TE's node matrix replaces the element's transform), rasterised."""
    doc = nodes[0][0]
    ET.register_namespace("", "http://www.w3.org/2000/svg")
    tree = ET.parse(os.path.join(SVG_DIR, doc))
    by_id = {el.get("id"): el for el in tree.iter() if el.get("id")}
    for _, i, vis, m, fill, stroke in nodes:
        el = by_id[i]
        if not vis:
            el.set("display", "none")
        if m[0] != -1 or any(v != -1 for v in m):
            el.set("transform", "matrix(" + " ".join(f"{v / 1024:.5f}" for v in m) + ")")
        if fill != "-" and el.get("fill", "none").startswith("#"):
            el.set("fill", "#" + fill)
        if stroke != "-" and el.get("stroke", "none").startswith("#"):
            el.set("stroke", "#" + stroke)
    root = tree.getroot()
    root.insert(0, ET.Element(NS + "rect", {"width": "320", "height": "160", "fill": "#000000"}))
    sp, pp = os.path.join(tmp, "s.svg"), os.path.join(tmp, "s.png")
    tree.write(sp)
    subprocess.run([RSVG, "-w", str(size[0]), "-h", str(size[1]), sp, "-o", pp], check=True)
    return Image.open(pp).convert("RGB")

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
ENGINE = os.path.join(os.path.dirname(SLOT), "engine")
SCALE = 1.175 * 3                     # panel px per SVG unit, x3 for viewing
PALETTE = {0: (255, 144, 167), 1: (1, 0, 0), 2: (255, 58, 93), 3: (78, 40, 50), 4: (53, 50, 56), 5: (0, 71, 91), 6: (0, 40, 38), 7: (95, 66, 112), 8: (174, 177, 220), 9: (191, 216, 255), 10: (56, 53, 114), 11: (29, 32, 64), 12: (124, 121, 144), 13: (255, 66, 63), 14: (255, 255, 255), 15: (105, 142, 255), 16: (0, 237, 149), 17: (230, 231, 255), 18: (128, 255, 175), 19: (255, 179, 179)}   # TE's colors.svg, in the palette order of notes/screen.md
DEFAULT = [(16, 0, 2048, 0, 0), (16, 16384, 10240, 16000, 20000), (16, 32767, 18432, 32767, 0),
           (16, 9000, 30720, 8000, 0), (15, 0, 6000, 20000, 9000), (15, 28000, 0, 17000, 15000)]


def main():
    out = sys.argv[1]
    nums = [int(v) for v in sys.argv[2:]]
    settings = [tuple(nums[i:i + 5]) for i in range(0, len(nums), 5)] or DEFAULT
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp()
    global SVG_DIR
    SVG_DIR = os.path.join(tmp, "display")
    os.makedirs(SVG_DIR)
    for name, sub in SVG_PATHS.items():
        path = os.environ.get("FM4_SVG") if name == "fm4.svg" and os.environ.get("FM4_SVG") else os.path.join(DESIGN, sub, name)
        with open(path, "rb") as src, open(os.path.join(SVG_DIR, name), "wb") as dst:   # FM4_SVG: another fm4 design
            dst.write(src.read())
    data = os.path.join(tmp, "wt.bin")
    subprocess.run([sys.executable, os.path.join(ENGINE, "gen_wt_data.py"), data], check=True)
    gdata = os.path.join(tmp, "grain.bin")
    subprocess.run([sys.executable, os.path.join(ENGINE, "gen_grain_data.py"), gdata], check=True)
    os.environ["GRAIN_DATA"] = gdata
    exe = os.path.join(tmp, "preview")
    subprocess.run(["cc", "-std=gnu99", "-O1", "-Wall", "-I", ENGINE, "-I", SLOT, "-lm",
                    "-DPALETTE=((const uint8_t *)host_palette)", "-DKNOBS(s)=((const int16_t *)host_knobs)",
                    "-DPROF=((volatile uint32_t *)host_prof)", "-DFX_KNOBS(m,s)=((const int16_t *)host_knobs)", "-DTE_MATRIX=((const int32_t *)host_matrix)",
                    "-include", os.path.join(HERE, "preview_decl.h"),
                    os.path.join(SLOT, "ui.c"), os.path.join(HERE, "preview_host.c"),
                    os.path.join(SLOT, "crush.c"),
                    "-DDISTORTION_CURVE_ONLY", os.path.join(SLOT, "distortion.c"), "-o", exe,
                    ], check=True)
    ids = os.path.join(tmp, "ids.txt")
    svg_ids(ids)
    env = dict(os.environ, SVG_DIR=SVG_DIR, SVG_IDS=ids)
    if os.environ.get("NO_SVG"):
        env.pop("SVG_DIR")
    for eng, *k in settings:
        lines = subprocess.run([exe, data, str(eng)] + [str(v) for v in k], check=True, env=env,
                               capture_output=True, text=True).stdout.split("\n")
        nodes = []
        for s in lines:
            if s.startswith("N "):
                f = s.split()
                nodes.append((f[1], f[2], int(f[3]), [int(v) for v in f[4:10]], f[10], f[11]))
        segs = [s for s in lines if s and not s.startswith("N ")]
        size = (int(320 * SCALE), int(160 * SCALE))
        img = svg_base(nodes, size, tmp) if nodes else Image.new("RGB", size, (0, 0, 0))
        d = ImageDraw.Draw(img, "RGBA")
        if not os.environ.get("CLEAN"):
            d.rectangle([0, 142 * SCALE, 320 * SCALE, 160 * SCALE], fill=(40, 0, 0, 140))
        n = 0
        for s in segs:
            x0, y0, x1, y1, w, c = (int(v) for v in s.split())
            f = SCALE / 1024
            d.line([x0 * f, y0 * f, x1 * f, y1 * f], fill=PALETTE.get(c, (255, 0, 255)),
                   width=max(1, round(w * f)))
            n += 1
        name = os.path.join(out, os.environ.get("NAME") or f"e{eng}_{'_'.join(map(str, k))}.png")
        if os.environ.get("CLEAN"):   # screenshots: only what the panel shows (y < 142)
            img = img.crop((0, 0, size[0], int(142 * SCALE)))
        img.save(name)
        print(f"{name}: {n} segments")


if __name__ == "__main__":
    main()
