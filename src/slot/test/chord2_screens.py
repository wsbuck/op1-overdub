#!/usr/bin/env python3
"""Render the CHORD v2 screens (src/slot/ui_chord2.h, the device's draw code) to PNGs and a gallery page.

  python3 src/slot/test/chord2_screens.py OUTDIR

Builds test/chord2_screens.c natively (the engine driven through its hooks sets each scene up), runs its `check`
(the frame hook's page rules), then draws every scene at the device's scale: SVG 320 x 160 -> panel x1.175, x3 for
viewing, black, the unseen bottom (y > 142) shaded; fast (1-px) lines one panel pixel wide. Fails if anything but the
black backing leaves the safe area (x 4..316, y 2..140) or a scene draws more than 120 AA segments.
Writes OUTDIR/<scene>.png and OUTDIR/index.html.
"""
import html
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
SCALE = 1.175 * 3
PALETTE = {0: (255, 144, 167), 1: (1, 0, 0), 2: (255, 58, 93), 3: (78, 40, 50), 4: (53, 50, 56), 5: (0, 71, 91),
           6: (0, 40, 38), 7: (95, 66, 112), 8: (174, 177, 220), 9: (191, 216, 255), 10: (56, 53, 114),
           11: (29, 32, 64), 12: (124, 121, 144), 13: (255, 66, 63), 14: (255, 255, 255), 15: (105, 142, 255),
           16: (0, 237, 149), 17: (230, 231, 255), 18: (128, 255, 175), 19: (255, 179, 179)}   # = preview.py
CAPTIONS = {
    "map_play": ("MAP: playing", "DIATONIC C, G4 held: the name spelled out (blue underline = ROOT knob, green = TYPE), "
                 "its notes on the ruler (root blue, top white), TOP and VOICING under it; the key lit and outlined."),
    "map_type_scan": ("TYPE scan", "C4 held, green turned two detents left: C^7 -> C6 -> C, heard as it turns (only the "
                      "changed notes retrigger). The strip shows where the scan is in the list."),
    "map_top": ("TOP", "White, two detents: E on top (the TOP readout bright)."),
    "map_voicing": ("VOICING", "Orange: DROP 2, the top note kept."),
    "frozen_playing": ("Edit while playing", "C4 frozen for editing (padlock) while G7 plays: the sounding chord at "
                       "the right in light blue."),
    "learn": ("Learn", "C4 edited, SHIFT held, A C E G played: named live in red, LEARN tag."),
    "learned": ("Learned", "SHIFT let go: C4 = A-7 as played, the confirmation and the undo hint."),
    "learned_slash": ("Slash kept", "B D G learned on C4 = G/B; TYPE turned keeps the B in the bass."),
    "map_shift": ("SHIFT layer + diagnostics", "MODE (SHIFT+blue), BASS (SHIFT+orange); the two diagnostic lines the "
                  "morning test reads (frame hook, knobs, keys, file, voices)."),
    "cleared": ("Cleared", "SPLIT + A4 cleared it; SHIFT+LIFT undoes."),
    "map_251": ("Factory II-V-I", "ii-V-I in F, Bb, Eb, C on groups of three white keys; D D# E = the minor ii-V-i."),
    "map_neo": ("Factory NEO SOUL", "m9 / maj9 / 13 colours, flat roots on the black keys; BASS and HOLD on."),
    "map_pop": ("Factory POP", "C major triads on the white keys, borrowed chords on the black keys."),
    "unmapped": ("Unmapped key", "C#4 has no chord: it plays its note; green gives it one."),
    "shape": ("SHAPE", "CHORD1's parallel shape, now voiceable: -7 drop 2 from C4; lit keys = its notes."),
    "off": ("OFF", "The slot plays plain notes; SHIFT+blue picks a mode."),
    "badge": ("Badge, our engines", "Map number and the last chord, blue frame = MAP."),
    "chip": ("Chip, TE screens", "The sounding chord top right on a black backing (frame hook)."),
    "chip_idle": ("Chip, silent", "Nothing sounding, the slot in MAP 3: the mode, dim."),
}


def draw(stdout):
    size = (int(320 * SCALE), int(160 * SCALE))
    img = Image.new("RGB", size, (0, 0, 0))
    d = ImageDraw.Draw(img, "RGBA")
    f = SCALE / 1024
    segs = []
    for s in stdout.split("\n"):
        if s:
            segs.append([int(v) for v in s.split()])
    for x0, y0, x1, y1, w, c in segs:
        width = round(SCALE / 1.175) if w < 0 else max(1, round(w * f))
        d.line([x0 * f, y0 * f, x1 * f, y1 * f], fill=PALETTE.get(c, (255, 0, 255)), width=width)
    d.rectangle([0, 142 * SCALE, 320 * SCALE, 160 * SCALE], fill=(60, 0, 0, 110))
    return img, segs


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "chord2_screens"
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(tempfile.mkdtemp(), "chord2_screens")
    subprocess.run(["cc", "-std=gnu99", "-O1", "-Wall", "-Wextra", "-Werror", "-I", SLOT,
                    os.path.join(HERE, "chord2_screens.c"), "-o", exe], check=True)
    chk = subprocess.run([exe, "check"], capture_output=True, text=True)
    print(chk.stdout.strip())
    names = subprocess.run([exe, "list"], check=True, capture_output=True, text=True).stdout.split()
    cards, bad = [], []
    for name in names:
        res = subprocess.run([exe, name], check=True, capture_output=True, text=True)
        img, segs = draw(res.stdout)
        img.save(os.path.join(out, name + ".png"))
        cost = res.stderr.strip()
        aa = int(cost.split()[0])
        for x0, y0, x1, y1, w, c in segs:
            if c == 1:
                continue                    # the page's black backing covers the whole screen on purpose
            for x, y in ((x0, y0), (x1, y1)):
                if not (4 * 1024 <= x <= 316 * 1024 and 2 * 1024 <= y <= 140 * 1024):
                    bad.append(f"{name}: ({x / 1024:.1f}, {y / 1024:.1f}) outside the safe area")
                    break
        if aa > 120:
            bad.append(f"{name}: {aa} AA segments")
        print(f"{name}: {cost}")
        title, text = CAPTIONS.get(name, (name, ""))
        cards.append(f'<figure><img src="{name}.png" alt="{html.escape(title)}"><figcaption><b>{html.escape(title)}</b>'
                     f' <span class="cost">{html.escape(cost)}</span><br>{html.escape(text)}</figcaption></figure>')
    for b in bad[:20]:
        print("BAD", b)
    page = f"""<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>CHORD v2 screens</title><style>
:root {{ --bg:#111; --fg:#ddd; --mute:#888; }}
body {{ background:var(--bg); color:var(--fg); font:14px/1.45 -apple-system,system-ui,sans-serif; margin:0; padding:16px; }}
h1 {{ font-size:18px; margin:0 0 4px; }} p.sub {{ color:var(--mute); margin:0 0 16px; }}
main {{ display:grid; grid-template-columns:repeat(auto-fill,minmax(min(100%,560px),1fr)); gap:20px; }}
figure {{ margin:0; }} img {{ width:100%; height:auto; image-rendering:pixelated; border:1px solid #333; }}
figcaption {{ margin-top:6px; }} .cost {{ color:var(--mute); font-size:12px; }}
</style></head><body><h1>CHORD v2 (MVP): the device's screens</h1>
<p class="sub">Rendered through src/slot/ui_chord2.h (the code ui.c runs) at the device scale, each scene set up by
the real engine through its hooks; shaded strip = rows the panel does not show.</p><main>{''.join(cards)}</main></body></html>"""
    with open(os.path.join(out, "index.html"), "w") as fh:
        fh.write(page)
    print(os.path.join(out, "index.html"))
    ok = chk.returncode == 0 and not bad
    print("all passed" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
