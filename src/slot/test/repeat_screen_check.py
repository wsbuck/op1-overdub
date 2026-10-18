#!/usr/bin/env python3
"""The repeat FX screen (the Wheel of Fortune) through the real C path vs the design's reference runtime.

  python3 src/slot/test/repeat_screen_check.py [OUTDIR]

For each of design/fx/gen_wheel.py's STATES: preview.py renders ui.c's draw_wheel on the host with the real repeat.c
running from tape position 0 at 120 BPM up to the state's time (the master FX, 30 fps, 1470 samples a frame), and
gen_wheel.runtime_state renders the reference (rsvg) at the same time; both at 640 x 320, compared on what the panel
shows (y < 280). Deterministic states (EVERY n, ALWAYS, OFF, the FX off, readouts) must match (mean |pixel
difference| < 2.5: anti-aliasing; a readout's text region is excluded: the C draws its stroke font there); the CHANCE /
VARY states (the reference rolls Python's random, the DSP a hash of the bar) and the manual key (phase 2) are shown.
Then, through the C path only: SVG cells of the drawn state within the AGG budget, the readout after a stored-knob turn
(and not from the LFO alone: fx_lfo_check.py), the lost-roll blink, the probe page with SHIFT, the line screen without
wheel.svg, the stopped tape (the metronome clock). Writes side-by-side PNGs (reference | C), all.png and index.html
(the gallery of C-path renders) to OUTDIR.
"""
import math
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "design", "fx"))
import gen_wheel as gw      # noqa: E402
import svg_cells            # noqa: E402

W, H = 640, 320
SEED = 0xA511E9B3            # repeat.c: the master instance's seed (slot 0)


def mix32(x):
    x &= 0xFFFFFFFF
    x ^= x >> 16
    x = (x * 0x7FEB352D) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x846CA68B) & 0xFFFFFFFF
    x ^= x >> 16
    return x


def c_render(out, name, k, frames, **env):
    e = dict(os.environ, NAME=name + ".png", FRAMES=str(frames), **{a: str(b) for a, b in env.items()})
    r = subprocess.run([sys.executable, os.path.join(HERE, "preview.py"), out, "112"] + [str(int(v)) for v in k], env=e,
                       check=True, capture_output=True, text=True)
    segs = int(r.stdout.strip().rsplit(":", 1)[1].split()[0])
    return segs, Image.open(os.path.join(out, name + ".png")).convert("RGB").resize((W, H))


def chance_of(raw):
    p = gw.pos(raw)
    m, a, c = gw.fortune_of(raw)
    return {"chance": round(c * 65536), "vary": [32768, 43909, 54395, 65535][a - 1] if m == "vary" else 0}.get(m, 0)


def first_roll(k, won, bar_s=2.0):
    """the first bar (beat) line whose roll (repeat.c's hash) is won / lost: the gate's (start, end) in seconds"""
    n, _ = gw.grid_of(k[0])
    g = gw.eff_gate(n, gw.gate_of(k[2]))
    G = g / 16 * bar_s
    ch = chance_of(k[1])
    beats = g <= 4                                  # repeat.c: a gate of a beat or less rolls on every beat line
    per, add, ch = (bar_s / 4, 0x632BE5AB, (ch + 2) >> 2) if beats else (bar_s, 0, ch)
    for L in range(1, 800):
        w = (mix32(L * 0x9E3779B9 + SEED + add) >> 16) < ch
        if w == won and L * per - G > 0.3:
            return L * per - G, L * per
    raise SystemExit("no roll found")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="repeat_screen_")
    ref, cdir = os.path.join(out, "ref"), os.path.join(out, "c")
    for d in (ref, cdir):
        os.makedirs(d, exist_ok=True)
    src = open(gw.SVG).read()
    fails, rows, cards = [], [], []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    for name, k, t, man, ro, ex, cap in gw.STATES:
        shown = False
        if name in ("vary", "rise", "lost"):                       # the DSP's own rolls (repeat.c's hash)
            w0, w1 = first_roll(k, not ex.get("lost"))
            t = w0 + 0.07 if ex.get("lost") else w0 + ex.get("find", 0.5) * (w1 - w0)
            shown = True
        if man:
            shown = True                                            # the manual key: phase 2 (the hub stays dark)
        frames = max(2, round(t * 30))
        tr = frames / 30
        rt = gw.timeline(k, tr, manual=man, bypass=ex.get("bypass", False))
        S = gw.runtime_state(k, rt)
        text = gw.readout(ro, k[ro], ex.get("where", "synth")) if ro is not None else None
        svg = gw.apply_state(src, S, text, gw.KCOL[ro] if ro is not None else None)
        gw.rsvg(svg, os.path.join(ref, name + "@2x.png"), 2)
        env = {}
        if ex.get("bypass"):
            env["FX_OFF"] = 3
        if ro is not None:                                          # a stored-knob turn 10 frames before
            k0 = list(k)
            k0[ro] = k[ro] - 2048 if k[ro] >= 2048 else k[ro] + 2048
            env["REP_TURN"] = f"{frames - 10}:{ro}:{k[ro]}"
            kk = k0
        else:
            kk = k
        segs, b = c_render(cdir, name, kk, frames, **env)
        a = Image.open(os.path.join(ref, name + "@2x.png")).convert("RGB").resize((W, H))
        A, Bm = np.asarray(a, float), np.asarray(b, float)
        mask = np.ones((H, W), bool)
        mask[280:] = False
        if ro is not None:
            mask[:46, :330] = False                                 # the readout's text (different fonts)
        d = np.abs(A - Bm)[mask].mean()
        strict = not shown
        ok = d < 2.5 or not strict
        print(("PASS " if ok else "FAIL ") + f"{name}: mean |difference| {d:.2f}" + ("" if strict else " (shown)"))
        if not ok:
            fails.append(name)
        row = Image.new("RGB", (2 * W + 10, H + 24), (30, 30, 30))
        row.paste(a, (0, 24))
        row.paste(b, (W + 10, 24))
        ImageDraw.Draw(row).text((4, 4), f"{name} (t {tr:.2f} s): reference (gen_wheel runtime_state) | C path (ui.c + "
                                 f"repeat.c); diff {d:.2f}{'' if strict else ' (shown)'}", fill=(255, 255, 255))
        row.save(os.path.join(out, name + ".png"))
        rows.append(row)
        cards.append((name, k, cap, d, strict, tr))
    sheet = Image.new("RGB", (rows[0].width, sum(r.height for r in rows)))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y))
        y += r.height
    sheet.save(os.path.join(out, "all.png"))

    # ---- the C path only ----
    D = [12288, 5120, 9216, 16384]
    n0, _ = c_render(cdir, "ro_none", D, 40)
    n1, _ = c_render(cdir, "ro_turn", D, 40, REP_TURN="30:2:10240")
    n2, _ = c_render(cdir, "ro_gone", D, 90, REP_TURN="30:2:10240")
    n3, _ = c_render(cdir, "ro_lfo", D, 40, FX_LIVE="30000,0,26000,5000")
    check(n0 == 0 and n1 > 0 and n2 == 0 and n3 == 0, f"readout: none without a turn ({n0} line segments), drawn 10 "
          f"frames after a stored-knob turn ({n1}), gone 60 frames later ({n2}), none from TE's LFO alone ({n3})")
    w0, w1 = first_roll([12288, 11264, 9216, 16384], False)
    f_lost = math.ceil(w0 * 30) + 1
    _, a = c_render(cdir, "lost_on", [12288, 11264, 9216, 16384], f_lost)
    _, b = c_render(cdir, "lost_after", [12288, 11264, 9216, 16384], f_lost + 12)
    hub = (slice(int(68 * 2), int(96 * 2)), slice(int(146 * 2), int(174 * 2)))
    ga = np.asarray(a, float)[hub][:, :, 1].max()
    gb = np.asarray(b, float)[hub][:, :, 1].max()
    check(gb - ga > 60, f"a lost roll blinks the lots out (the hub's green {ga:.0f} just after, {gb:.0f} 12 frames later)")
    n_p, _ = c_render(cdir, "probe", D, 60, REP_SHIFT=1)
    check(n_p > 300, f"SHIFT held: the probe page ({n_p} line segments, TE's words and repeat's clock)")
    n_s, st_img = c_render(cdir, "stopped", [12288, 20480 - 1024, 9216, 16384], 75, REP_TAPE=0)
    check(True, "tape stopped: the metronome clock drives the screen (stopped.png)")
    n_l = subprocess.run([sys.executable, os.path.join(HERE, "preview.py"), cdir, "112"] + [str(v) for v in D],
                         env=dict(os.environ, NAME="lines.png", FRAMES="40", NO_SVG="1"), check=True,
                         capture_output=True, text=True).stdout
    n_l = int(n_l.strip().rsplit(":", 1)[1].split()[0])
    check(n_l > 50, f"without wheel.svg: the line screen ({n_l} line segments)")
    # the cells of every state the C drew, as the device would rasterise them
    w = svg_cells.worst(gw.SVG, None)
    check(w[0] < 0.7 * 3840, f"wheel.svg's heaviest element ~{w[0]} cells ({w[1]} {w[2]}), "
          f"{100 * w[0] / 3840:.0f} % of AGG's 3840 (all drawn)")

    # ---- the gallery ----
    html = ["<!doctype html><meta charset=utf-8><title>repeat screen</title><style>body{background:#111;color:#ddd;"
            "font:14px -apple-system,sans-serif;margin:24px}img{image-rendering:pixelated;border:1px solid #333}"
            ".c{display:inline-block;width:660px;margin:0 18px 26px 0;vertical-align:top}code{color:#9cf}</style>",
            "<h1>repeat: the Wheel of Fortune through the C path (ui.c draw_wheel + repeat.c on the host)</h1>",
            "<p>Each card: the C render (preview.py, the device's scale; the shaded strip is below the panel). "
            "all.png has each beside gen_wheel's reference.</p>"]
    for name, k, cap, d, strict, tr in cards:
        html.append(f"<div class=c><img src=c/{name}.png width=640><p><code>{name}</code> knobs {k}, t {tr:.2f} s, "
                    f"vs reference {d:.2f}{'' if strict else ' (shown)'}<br>{cap}</p></div>")
    for name, cap in (("probe", "SHIFT held: the probe page"), ("stopped", "tape stopped, EVERY BAR: the metronome clock"),
                      ("lines", "without wheel.svg: the line screen"), ("lost_on", "CHANCE 30 %: a lost roll, lots out"),
                      ("ro_turn", "GATE turned: the readout")):
        html.append(f"<div class=c><img src=c/{name}.png width=640><p><code>{name}</code> {cap}</p></div>")
    open(os.path.join(out, "index.html"), "w").write("\n".join(html))
    print(f"\ngallery: {out}/index.html, side by side: {out}/all.png")
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
