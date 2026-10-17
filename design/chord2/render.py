#!/usr/bin/env python3
"""Render the CHORD v2 mockups (chord2_ui.h through chord2_preview.c) to PNGs and a gallery page. Design only.

  python3 design/chord2/render.py OUTDIR

Builds chord2_preview.c natively, runs the model checks, then draws every scene at the device's scale like
src/slot/test/screens_preview.py: SVG 320 x 160 -> panel x1.175, x3 for viewing, black, the unseen bottom (y > 142)
shaded; fast (1-px) lines one panel pixel wide. Writes OUTDIR/<scene>.png and OUTDIR/index.html (captions from
CAPTIONS below; the review log lives in notes/chord2.md).
"""
import html
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SCALE = 1.175 * 3
PALETTE = {0: (255, 144, 167), 1: (1, 0, 0), 2: (255, 58, 93), 3: (78, 40, 50), 4: (53, 50, 56), 5: (0, 71, 91),
           6: (0, 40, 38), 7: (95, 66, 112), 8: (174, 177, 220), 9: (191, 216, 255), 10: (56, 53, 114),
           11: (29, 32, 64), 12: (124, 121, 144), 13: (255, 66, 63), 14: (255, 255, 255), 15: (105, 142, 255),
           16: (0, 237, 149), 17: (230, 231, 255), 18: (128, 255, 175), 19: (255, 179, 179)}   # = preview.py

CAPTIONS = {}
try:
    from captions import CAPTIONS  # noqa: F401  (design/chord2/captions.py)
except ImportError:
    pass


def draw(stdout):
    size = (int(320 * SCALE), int(160 * SCALE))
    img = Image.new("RGB", size, (0, 0, 0))
    d = ImageDraw.Draw(img, "RGBA")
    d.rectangle([0, 142 * SCALE, 320 * SCALE, 160 * SCALE], fill=(40, 0, 0, 140))
    f = SCALE / 1024
    for s in stdout.split("\n"):
        if not s:
            continue
        x0, y0, x1, y1, w, c = (int(v) for v in s.split())
        width = round(SCALE / 1.175) if w < 0 else max(1, round(w * f))
        d.line([x0 * f, y0 * f, x1 * f, y1 * f], fill=PALETTE.get(c, (255, 0, 255)), width=width)
    return img


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "chord2"
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(tempfile.mkdtemp(), "chord2_preview")
    subprocess.run(["cc", "-std=gnu99", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", HERE,
                    os.path.join(HERE, "chord2_preview.c"), "-o", exe], check=True)
    m = subprocess.run([exe, "model"], capture_output=True, text=True)
    print(m.stderr.strip())
    names = subprocess.run([exe, "list"], check=True, capture_output=True, text=True).stdout.split()
    cards = []
    for name in names:
        res = subprocess.run([exe, name], check=True, capture_output=True, text=True)
        draw(res.stdout).save(os.path.join(out, name + ".png"))
        cost = res.stderr.strip()
        print(f"{name}: {cost}")
        title, text = CAPTIONS.get(name, (name, ""))
        cards.append(f'<figure><img src="{name}.png" alt="{html.escape(title)}"><figcaption><b>{html.escape(title)}</b>'
                     f' <span class="cost">{html.escape(cost)}</span><br>{html.escape(text)}</figcaption></figure>')
    page = f"""<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>CHORD v2 mockups</title><style>
:root {{ --bg:#111; --fg:#ddd; --mute:#888; }}
body {{ background:var(--bg); color:var(--fg); font:14px/1.45 -apple-system,system-ui,sans-serif; margin:0; padding:16px; }}
h1 {{ font-size:18px; margin:0 0 4px; }} p.sub {{ color:var(--mute); margin:0 0 16px; }}
main {{ display:grid; grid-template-columns:repeat(auto-fill,minmax(min(100%,560px),1fr)); gap:20px; }}
figure {{ margin:0; }} img {{ width:100%; height:auto; image-rendering:pixelated; border:1px solid #333; }}
figcaption {{ margin-top:6px; }} .cost {{ color:var(--mute); font-size:12px; }}
</style></head><body><h1>CHORD v2: screens</h1>
<p class="sub">Rendered through the C draw code (design/chord2/chord2_ui.h) at the device scale; shaded strip = rows the
panel does not show. Model checks: {html.escape(m.stderr.strip().splitlines()[-1] if m.stderr.strip() else '')}
({'all pass' if m.returncode == 0 else 'FAILURES'}).</p><main>{''.join(cards)}</main></body></html>"""
    with open(os.path.join(out, "index.html"), "w") as fh:
        fh.write(page)
    print(os.path.join(out, "index.html"))
    sys.exit(m.returncode)


if __name__ == "__main__":
    main()
