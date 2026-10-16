#!/usr/bin/env python3
"""The crush FX screen through the real C path vs the design's reference runtime.

  python3 src/slot/test/crush_screen_check.py [OUTDIR]

Renders design/fx/gen_crush.py's STATES (knobs, gain reduction) with preview.py (ui.c's draw_pyramid on the host, with
the real crush.c settled on those knobs) and with gen_crush3d.py's runtime_state (rsvg; it uses the DSP's bit map too),
at 640 x 320, and compares them: every state without GRIT must match (mean |pixel difference| < 2.5: anti-aliasing);
GRIT states (random jitter, flips) are shown. Then, through the C path only: the value readout appears on a raw knob
change of 512 (not 400) and is gone after ~1.5 s; the GRIT shake stays still without a signal and moves with one; the
DSP's flips bring slices and debris (none without flips). Writes side-by-side PNGs (reference | C) and all.png to
OUTDIR (default: a temp folder).
"""
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "design", "fx"))
import gen_crush as g2      # noqa: E402
import gen_crush3d as g3    # noqa: E402
import svg_cells as cells   # noqa: E402

W, H = 640, 320



def main():
    out = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="crush_screen_")
    ref, cdir = os.path.join(out, "ref"), os.path.join(out, "c")
    os.makedirs(ref, exist_ok=True)
    os.makedirs(cdir, exist_ok=True)
    fails, rows = [], []
    for name, k, gr, _ in g2.STATES:
        g3.composite(ref, name, k, 0.3, gr)                         # t = 0.3 s = 9 frames at 30 fps
        kk = [str(min(32767, int(round(v * 32768)))) for v in k]
        env = dict(os.environ, NAME=name + ".png", FRAMES="9", CRUSH_GR=str(int(gr * 32767)), CRUSH_LEVEL="1000",
                   CRUSH_FLIPS="1000" if k[3] > 0.5 else "0")
        subprocess.run([sys.executable, os.path.join(HERE, "preview.py"), cdir, "111"] + kk, env=env, check=True,
                       capture_output=True)
        a = Image.open(os.path.join(ref, name + "@2x.png")).convert("RGB").resize((W, H))
        b = Image.open(os.path.join(cdir, name + ".png")).convert("RGB").resize((W, H))
        d = np.abs(np.asarray(a.crop((0, 0, W, 280)), float) - np.asarray(b.crop((0, 0, W, 280)), float)).mean()
        strict = k[3] == 0
        ok = d < 2.5 or not strict
        print(("PASS " if ok else "FAIL ") + f"{name}: mean |difference| {d:.1f}" + ("" if strict else " (shown)"))
        if not ok:
            fails.append(name)
        row = Image.new("RGB", (2 * W + 10, H + 24), (30, 30, 30))
        row.paste(a, (0, 24))
        row.paste(b, (W + 10, 24))
        ImageDraw.Draw(row).text((4, 4), f"{name}: reference (gen_crush3d) | C path (ui.c + crush.c); diff {d:.1f}",
                                 fill=(255, 255, 255))
        row.save(os.path.join(out, name + ".png"))
        rows.append(row)
    sheet = Image.new("RGB", (rows[0].width, sum(r.height for r in rows)))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y))
        y += r.height
    sheet.save(os.path.join(out, "all.png"))
    print(f"\ngallery: {out}/all.png")

    def c_path(name, k, **env):
        e = dict(os.environ, NAME=name + ".png", **{a: str(b) for a, b in env.items()})
        r = subprocess.run([sys.executable, os.path.join(HERE, "preview.py"), cdir, "111"] + [str(v) for v in k], env=e,
                           check=True, capture_output=True, text=True)
        return int(r.stdout.strip().rsplit(":", 1)[1].split()[0]), \
            np.asarray(Image.open(os.path.join(cdir, name + ".png")).convert("L"), float)

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)
    base = (10240, 12288, 10240, 0)
    n512 = c_path("ro512", base, FRAMES=10, CRUSH_TURN="5:1:12800")[0]
    n400 = c_path("ro400", base, FRAMES=10, CRUSH_TURN="5:1:12688")[0]
    nold = c_path("ro_old", base, FRAMES=60, CRUSH_TURN="5:1:12800")[0]
    check(n512 > 0 and n400 == 0 and nold == 0, f"readout: drawn after a turn of 512 raw ({n512} line segments), not after "
          f"400 ({n400}), gone 55 frames later ({nold})")
    S = 1128 / 320                                                  # preview pixels per SVG unit

    def mid(a):                                                     # the big pyramid only (no sun, no far pyramids)
        return a[int(20 * S):int(140 * S), int(80 * S):int(240 * S)]
    gk = (0, 0, 0, 9830)
    a9, a10 = (c_path(f"still{f}", gk, FRAMES=f)[1] for f in (9, 10))
    b9, b10 = (c_path(f"shake{f}", gk, FRAMES=f, CRUSH_LEVEL=1000)[1] for f in (9, 13))
    still, shake = np.abs(mid(a9) - mid(a10)).mean(), np.abs(mid(b9) - mid(b10)).mean()
    check(still < 0.05 and shake > 0.5, f"GRIT 0.3 shake: frame-to-frame change {still:.2f} without a signal (still), "
          f"{shake:.2f} with one")
    fk = (0, 0, 0, 26214)
    q0 = c_path("noflips", fk, FRAMES=9, CRUSH_LEVEL=1000)[1]       # one frame: the shake is the same (same clock)
    q1 = c_path("flips", fk, FRAMES=9, CRUSH_LEVEL=1000, CRUSH_FLIPS=1000)[1]
    q2 = c_path("flips_silent", fk, FRAMES=9, CRUSH_FLIPS=1000)[1]
    q3 = c_path("silent", fk, FRAMES=9)[1]
    check(np.abs(q1 - q0).mean() > 1 and np.abs(q2 - q3).mean() == 0, f"GRIT 0.8: the DSP's flips bring slices and debris "
          f"(image change {np.abs(q1 - q0).mean():.2f}), none on silence (change {np.abs(q2 - q3).mean():.2f})")
    # the shade (right) face never vanishes into the black ground while nb >= 2 (BITS detents 0..29; past the 2-bit
    # plateau at 30310 it is the 1-bit row: black with the green rim); sampled near the base, plain and with voxels
    pts = [g3.F("R", u, v) for u, v in ((0.15, 0.08), (0.45, 0.1), (0.7, 0.06))]
    dark = []
    for d in list(range(0, 20, 4)) + list(range(20, 30)):
        for kr in (0, 10240):
            img = c_path(f"rface_{d}_{kr}", (kr, 1024 * d, 0, 0), FRAMES=9)[1]
            lum = sorted(img[int(y * S), int(x * S)] for x, y in pts)[1]           # median of the three points
            if lum < 25:
                dark.append((d, kr, int(lum)))
    check(not dark, "BITS detents 0..29 (down to 2 bits), RATE 0 and 11 kHz: the shade face stays visible (median "
          f"luminance >= 25 near the base){'; black at ' + str(dark) if dark else ''}")
    # every readout (title size 2: 12 high, y 7 .. 19) stays above the sun (y >= 24) and ends above the apex
    ends = {t: 8 + (len(t) - 1) * 12 + 8 for t in ("RATE 11.0KHZ", "BITS 8 MU 50%", "BITS 7.5 MU 75%", "SQUASH 20:1",
                                                   "JITTER 100%", "FLIP 100%")}
    check(max(ends.values()) <= 200 and 7 + 12 < 24, "readout at title size 2: y 7 .. 19 (the sun starts at 24); the "
          "longest strings end at x = " + ", ".join(f"{t!r} {v}" for t, v in ends.items()))
    # SQUASH: the sun pumps: smaller and dimmer with the gain reduction, white while SQUASH is on
    def sun(img):
        a = img[int(18 * S):int(58 * S), int(38 * S):int(78 * S)]
        return (a > 40).sum(), a.max()
    s0 = sun(c_path("sun_gr0", (0, 0, 32767, 0), FRAMES=9, CRUSH_GR=0)[1])
    s1 = sun(c_path("sun_gr1", (0, 0, 32767, 0), FRAMES=9, CRUSH_GR=32767)[1])
    soff = sun(c_path("sun_off", (0, 0, 0, 0), FRAMES=9)[1])
    check(s1[0] < 0.5 * s0[0] and s1[1] < s0[1] - 60 and s0[1] > soff[1] + 60, f"SQUASH sun: lit area {s0[0]} -> "
          f"{s1[0]} px, peak {s0[1]:.0f} -> {s1[1]:.0f} from gain reduction 0 to max (SQUASH off: peak {soff[1]:.0f})")
    n_mu = c_path("ro_mu", (0, 16384, 1000, 0), FRAMES=10, CRUSH_TURN="5:1:17408")[0]
    check(n_mu > 0, f"readout while mu-law morphs out (BITS detent 17: 'BITS 7.5 MU 75%'): {n_mu} segments")

    # AGG's cell limit (notes/screen.md): every fill and stroke of every state within half of 15 x 256 cells
    svg = os.path.join(ROOT, "design", "fx", "crush3d.svg")
    worst = (0, "", "", None)
    for r_ in (0, 4096, 10240, 18432, 25600, 32767):
        for b_ in (0, 12288, 16384, 22528, 28672, 32767):
            for s_ in (0, 32767):
                for o_ in (0, 32767):
                    k = tuple(v / 32768 for v in (r_, b_, s_, o_))
                    w = cells.worst(svg, g3.runtime_state(k, 0.3, 1.0))
                    if w[0] > worst[0]:
                        worst = w + ((r_, b_, s_, o_),)
    check(worst[0] <= cells.LIMIT // 2, f"AGG cells: the heaviest fill or stroke over 144 states is {worst[1]} "
          f"({worst[2]}, knobs {worst[3]}) ~{worst[0]} cells, {100 * worst[0] / cells.LIMIT:.0f} % of the {cells.LIMIT} "
          "that fit (bound 50 %)")
    # ui.c's recolour table (src/slot/crush3d_ids.h, generated) names leaves with that style (TE's style getters assert
    # on a group or a missing style)
    tab = open(os.path.join(ROOT, "src", "slot", "crush3d_ids.h")).read()
    ids = re.findall(r'\{ "(\w+)", (\d), RL_', tab)
    import xml.etree.ElementTree as ET
    els = {e.get("id"): e for e in ET.parse(svg).iter() if e.get("id")}
    bad = [i for i, st in ids if i not in els or els[i].tag.endswith("}g") or els[i].get("stroke" if st == "1" else "fill",
                                                                                         "none") == "none"]
    check(not bad and els.get("cells4") is not None, f"ui.c recolours {len(ids)} styles: all leaves with that style in "
          f"crush3d.svg{'; bad: ' + str(bad) if bad else ''}; the cell-budget marker 'cells4' is there")

    # the boot test card (crushcard.svg, ui.c draw_card): up for 360 frames, case 17 at half size, case 18 green,
    # the expected outlines and numbers drawn by the C; then the pyramid
    n1, c1 = c_path("card1", base, FRAMES=1, CRUSH_CARD=1)
    n2, c2 = c_path("card360", base, FRAMES=360, CRUSH_CARD=1)
    n3, c3 = c_path("card361", base, FRAMES=361, CRUSH_CARD=1)
    p0 = c_path("nocard", base, FRAMES=1)[1]
    rgb = np.asarray(Image.open(os.path.join(cdir, "card1.png")).convert("RGB"), int)
    ys, xs = rgb.shape[0] / 160, rgb.shape[1] / 320
    def at(x, y):
        return rgb[int(y * ys), int(x * xs)]
    x17, y17 = (4 + 4 * 52 + 2, 2 + 2 * 46 + 12)                    # case 17's box origin
    x18, y18 = (4 + 5 * 52 + 2, 2 + 2 * 46 + 12)
    g18 = at(x18 + 11, y18 + 15)
    in17, out17 = at(x17 + 11, y17 + 15), at(x17 + 4, y17 + 6)
    check(n1 >= 65 and np.abs(c1 - c2).mean() < 1 and np.abs(c3 - p0).mean() < 1 and np.abs(c1 - p0).mean() > 5  # frame 361 = the pyramid's first frame
          and g18[1] > 200 and g18[0] < 60 and in17.mean() > 200 and out17.mean() < 60,
          f"test card: up on frames 1 .. 360 ({n1} line segments), the pyramid on 361; case 18 green {tuple(int(v) for v in g18)}, "
          f"case 17 half size (centre {tuple(int(v) for v in in17)}, corner {tuple(int(v) for v in out17)}); "
          f"change 1 -> 360 {np.abs(c1 - c2).mean():.2f}, card vs pyramid {np.abs(c1 - p0).mean():.1f}, "
          f"361 vs pyramid {np.abs(c3 - p0).mean():.1f}")

    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
