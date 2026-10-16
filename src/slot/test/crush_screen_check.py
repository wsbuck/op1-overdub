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
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
