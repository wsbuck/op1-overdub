#!/usr/bin/env python3
"""The crush FX screen through the real C path vs the design's reference runtime.

  python3 src/slot/test/crush_screen_check.py [OUTDIR]

Renders design/fx/gen_crush.py's STATES (knobs, gain reduction) with preview.py (ui.c's draw_pyramid on the host, with
the real crush.c settled on those knobs, so the bit depth and mu-law flag are the DSP's) and with gen_crush3d.py's
runtime_state (rsvg), at 640 x 320, and compares them. The design maps BITS as nb = 16 - 15 b; the DSP's map
(16 -> 7 -> 1) differs, so only states whose BITS sit where both agree (0, the mu-law centre, 1) and without GRIT
(random jitter, debris timing) must match: mean |pixel difference| < 2.5 (anti-aliasing only). The others are shown,
not checked. Writes side-by-side PNGs (reference | C) and all.png to OUTDIR (default: a temp folder).
"""
import os
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

W, H = 640, 320
STRICT = {"pristine", "rate_mid", "rate_full", "bits_mid", "bits_full", "squash_mid", "squash_full", "combo_rate_bits"}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="crush_screen_")
    ref, cdir = os.path.join(out, "ref"), os.path.join(out, "c")
    os.makedirs(ref, exist_ok=True)
    os.makedirs(cdir, exist_ok=True)
    fails, rows = [], []
    for name, k, gr, _ in g2.STATES:
        g3.composite(ref, name, k, 0.3, gr)                         # t = 0.3 s = 9 frames at 30 fps
        kk = [str(min(32767, int(round(v * 32768)))) for v in k]
        env = dict(os.environ, NAME=name + ".png", FRAMES="9", CRUSH_GR=str(int(gr * 32767)))
        subprocess.run([sys.executable, os.path.join(HERE, "preview.py"), cdir, "111"] + kk, env=env, check=True,
                       capture_output=True)
        a = Image.open(os.path.join(ref, name + "@2x.png")).convert("RGB").resize((W, H))
        b = Image.open(os.path.join(cdir, name + ".png")).convert("RGB").resize((W, H))
        d = np.abs(np.asarray(a.crop((0, 0, W, 280)), float) - np.asarray(b.crop((0, 0, W, 280)), float)).mean()
        ok = d < 2.5 or name not in STRICT
        print(("PASS " if ok else "FAIL ") + f"{name}: mean |difference| {d:.1f}" + ("" if name in STRICT else " (shown)"))
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
    print(f"{len(fails)} failed" if fails else "all passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
