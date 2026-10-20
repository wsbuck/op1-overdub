#!/usr/bin/env python3
"""The repeat FX screen (design/fx/repeat.svg, William Buck's design) through the real C path vs the design's states.

  python3 src/slot/test/repeat_screen_check.py [OUTDIR] [--user PNG_DIR]

1. The design's preview states (idle, firing, pitchdown, pitchup, tapestop, spinup, waiting, off; the designer's
   preview.py): preview.py renders ui.c's draw_wheel on the host with repeat_state set to the state (REP_FORCE, no DSP);
   the reference is repeat.svg with the header's rules applied (ref_state below, the designer's state() as reachable
   on the device) rendered by rsvg at the same size. Must match (mean |pixel difference| < 2.5 on what the panel shows,
   the BAR k/n text excluded). With --user, also each beside the designer's own PNG (repeat_<state>_3x.png), the diff
   reported (shown, not checked: a few of those states aren't reachable as drawn, see REACH).
2. The DSP running (repeat.c from tape position 0 at 120 BPM, the master FX, 30 fps): a fill, SPIN UP, the manual key
   (forced), shown.
3. The C path only: the readout after a stored-knob turn (and not from the LFO alone), a lost roll dims the gate, the
   probe page with SHIFT (a -DUI_DEBUG build: the page is compiled only into --debug-ui builds; in a normal build SHIFT
   draws the screen unchanged), the line screen without repeat.svg, the stopped tape (the metronome clock), SVG cells.
Writes the C renders, side-by-side PNGs, all.png and index.html to OUTDIR.
"""
import math
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "design", "fx"))
import svg_cells            # noqa: E402

SVG = os.path.join(ROOT, "design", "fx", "repeat.svg")
RSVG = "/opt/homebrew/bin/rsvg-convert"
SCALE = 1.175 * 3                       # preview.py's
W, H = int(320 * SCALE), int(160 * SCALE)
SEED = 0xA511E9B3                       # repeat.c: the master instance's seed (slot 0)
GRIDS = (2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64)
SLICES_AT = [64] * 3 + [48] * 2 + [32] * 4 + [24] * 2 + [16] * 4 + [12] * 2 + [8] * 4 + [6] * 2 + [4] * 4 + [3] * 2 + [2] * 4
GRID_BYTE = {2: 1, 3: 0x80, 4: 2, 6: 0x81, 8: 3, 12: 0x82, 16: 4, 24: 0x83, 32: 5, 48: 0x84, 64: 6}


def tables():
    """repeat.h's knob maps, parsed"""
    src = open(os.path.join(ROOT, "src", "slot", "repeat.h")).read()
    out = {}
    for name in ("rep_fa_at", "rep_chance_at", "rep_gate_at", "rep_pitch_at"):
        body = re.search(name + r"\[33\] = \{(.*?)\};", src, re.S).group(1)
        body = body.replace("REP_SPIN", "100").replace("REP_STOP", "-100")
        out[name] = [int(v) for v in re.findall(r"-?\d+", re.sub(r"/\*.*?\*/", "", body))]
    return out


T = tables()


def eff_gate(n, gate16):
    """repeat.h rep_eff_gate on slices per bar n"""
    trip = n % 3 == 0
    sh = int(math.log2(n // 3 if trip else n))
    two = [11, 6, 3, 2, 1][min(sh, 4)] if trip else [32, 16, 8, 4, 2, 1, 1][sh]
    return min(max(gate16, two), 64)


def rot(a, cx=100, cy=80):
    t = math.radians(a)
    co, si = math.cos(t), math.sin(t)
    return f"matrix({co:.5f} {si:.5f} {-si:.5f} {co:.5f} {cx - co * cx + si * cy:.5f} {cy - si * cx - co * cy:.5f})"


def ref_state(grid=8, gate=4, burnt=0, armed=True, fortune=("chance", 5), bars_done=0, firing=False, repeat=0,
              phase=0.1, pitch=0, pitch_now=None, laps=None):
    """The header's rules (the designer's preview.py state(), ported): ids hidden and group transforms. pitch: the knob
    in semitones (+-14 the extremes); pitch_now: reached so far (default the knob)."""
    hide, tf = set(), {}
    hide |= {f"ticks{n}" for n in GRIDS if n != grid}
    hide |= {f"slice{n}" for n in GRIDS if n != grid or not firing}
    if firing:
        tf["wheel"] = rot(360 / grid * repeat)
    segs = min(gate, 16)
    laps = max(0, gate // 16 - 1) if laps is None else laps
    off = fortune[0] == "off"
    for i in range(1, 17):
        on = i > 16 - segs + burnt
        hide.add(f"gate{i}" if (not on or not armed or off) else f"gate_dim{i}")
        if not on:
            hide.add(f"gate_dim{i}")
    hide |= {f"lap{k}" for k in range(laps + 1, 4)}
    mode = fortune[0]
    if mode == "chance":
        lit = range(1, fortune[1] + 1)
    elif mode == "every":
        lit = range(1, bars_done * 8 // fortune[1] + 1)
    else:
        lit = {"off": [], "always": range(1, 11), "vary": (2, 4, 6, 8, 10)}[mode]
    hide |= {f"stripe{k}" for k in range(1, 11) if k not in lit}
    tf["playhead"] = f"translate({280 * phase:.4f} 0)"
    hide.add("eye_shut" if firing else "eye_open")
    if not firing:
        hide.add("pow")
    st = pitch_now if pitch_now is not None else pitch
    st = max(-12.5, min(12.5, st))
    hide |= {f"up{k}" for k in range(1, 13) if pitch <= 0 or k > pitch}
    hide |= {f"down{k}" for k in range(1, 13) if pitch >= 0 or k > -pitch}
    if pitch > -14:
        hide.add("tapestop")
    if pitch < 14:
        hide.add("spinup")
    hide |= {"snake_grey", "snake_up", "snake_down"} - {"snake_grey" if pitch == 0 else
                                                        "snake_up" if pitch > 0 else "snake_down"}
    if st:
        tf["snake"] = rot(3.5 * st)
    return hide, tf


# The designer's states (preview.py STATES) as the device reaches them, and how: knob positions (grid, fortune, gate,
# pitch: rep_pos 0..32) and the repeat_state forced (REP_FORCE). REACH: where the reference differs from the designer's
# own state, and why (the designer's PNG is compared as shown).
STATES = [
    ("idle", dict(), (18, 13, 10, 16), "active=0,blue=3,grid=3,ph=100,bar=0"),
    ("firing", dict(grid=16, gate=8, burnt=3, firing=True, repeat=3, phase=0.69, fortune=("chance", 7)),
     (12, 15, 16, 16), "active=1,kind=1,blue=4,grid=4,rep=3,ph=690,grem=310,bar=0"),
    ("pitchdown", dict(grid=8, gate=12, burnt=4, firing=True, repeat=2, phase=0.56, pitch=-12, pitch_now=-7,
                       fortune=("every", 4), bars_done=3),
     (18, 5, 19, 28), "active=1,kind=1,blue=3,grid=3,rep=2,ph=560,grem=440,bar=3,depth=19115"),
    ("pitchup", dict(grid=32, gate=16, burnt=10, firing=True, repeat=13, phase=0.66, pitch=12, pitch_now=9,
                     fortune=("always",)),
     (6, 0, 22, 3), "active=1,hold=1,kind=1,blue=5,grid=5,rep=13,ph=660,bar=0,depth=24576"),
    ("tapestop", dict(grid=4, gate=64, firing=True, repeat=1, phase=0.05, pitch=-14, fortune=("vary",)),
     (24, 21, 28, 31), "active=1,kind=1,blue=2,grid=2,rep=1,ph=50,grem=3950,bar=0,depth=32767"),
    ("spinup", dict(grid=64, gate=32, burnt=12, firing=True, repeat=40, phase=0.8, pitch=14, fortune=("chance", 3)),
     (1, 11, 25, 1), "active=1,kind=1,blue=6,grid=6,rep=40,ph=800,grem=1200,bar=0,depth=32767"),
    ("waiting", dict(grid=64, gate=2, armed=False, fortune=("every", 8), bars_done=5, phase=0.4, pitch=3, pitch_now=0),
     (1, 3, 4, 9), "active=0,blue=6,grid=6,ph=400,bar=5"),
    ("off", dict(grid=3, gate=11, fortune=("off",), phase=0.85), (27, 1, 13, 16), "active=0,blue=128,grid=128,ph=850"),
]
REACH = {
    "pitchdown": "burnt 4, not 5: the playhead at 0.56 bar is in segment 9, so 9..16 are left",
    "pitchup": "the knob at +12 (the dots), not +9: the knob's values are 1 2 3 5 7 12; the snake at +9 reached",
    "waiting": "the snake at rest: no fill, no semitones reached yet",
    "off": "the gate 11/16, not 6: under the 1/2T grid the gate holds two slices (rep_eff_gate)",
}


def mask():
    m = np.ones((H, W), bool)
    m[int(142 * SCALE):] = False                                   # not on the panel
    m[:int(14 * SCALE), int(268 * SCALE):] = False                 # BAR k/n (the C's text, not in the design)
    return m


def c_render(out, name, pos, frames, **env):
    k = [p * 1024 for p in pos]
    e = dict(os.environ, NAME=name + ".png", FRAMES=str(frames), **{a: str(b) for a, b in env.items()})
    r = subprocess.run([sys.executable, os.path.join(HERE, "preview.py"), out, "112"] + [str(v) for v in k], env=e,
                       check=True, capture_output=True, text=True)
    segs = int(r.stdout.strip().rsplit(":", 1)[1].split()[0])
    return segs, Image.open(os.path.join(out, name + ".png")).convert("RGB")


def ref_render(path, hide, tf, w=W, h=H):
    ET.register_namespace("", "http://www.w3.org/2000/svg")
    tree = ET.parse(SVG)
    for el in tree.getroot().iter():
        i = el.get("id")
        if i in hide:
            el.set("display", "none")
        if i in tf:
            el.set("transform", tf[i])
    with tempfile.NamedTemporaryFile("wb", suffix=".svg", delete=False) as fh:
        tree.write(fh)
    subprocess.run([RSVG, "-w", str(w), "-h", str(h), "-b", "#000000", fh.name, "-o", path], check=True)
    os.unlink(fh.name)
    return Image.open(path).convert("RGB")


def diff(a, b, m):
    d = np.abs(np.asarray(a, float) - np.asarray(b, float)).mean(axis=2)
    return d[m].mean(), d


def heat(d, m):
    v = np.clip(d * 2, 0, 255).astype(np.uint8)
    v[~m] = 40
    return Image.fromarray(np.stack([v, v // 3, v // 3], axis=2))


def mix32(x):
    x &= 0xFFFFFFFF
    x ^= x >> 16
    x = (x * 0x7FEB352D) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x846CA68B) & 0xFFFFFFFF
    x ^= x >> 16
    return x


def first_lost(pos, bar_s=2.0):
    """the first roll repeat.c loses (CHANCE, its hash), at least 0.3 s in: the roll's time in seconds"""
    slices = SLICES_AT[pos[0]]
    g = eff_gate(slices, T["rep_gate_at"][pos[2]])
    ch = T["rep_chance_at"][pos[1]]
    beats = g <= 4
    per, add, ch = (bar_s / 4, 0x632BE5AB, (ch + 2) >> 2) if beats else (bar_s, 0, ch)
    G = g / 16 * bar_s
    for L in range(1, 800):
        if (mix32(L * 0x9E3779B9 + SEED + add) >> 16) >= ch and L * per - G > 0.3:
            return L * per - G, per
    raise SystemExit("no lost roll found")


def main():
    args = [a for a in sys.argv[1:]]
    user = None
    if "--user" in args:
        i = args.index("--user")
        user = args[i + 1]
        del args[i:i + 2]
    out = args[0] if args else tempfile.mkdtemp(prefix="repeat_screen_")
    ref, cdir = os.path.join(out, "ref"), os.path.join(out, "c")
    for d in (ref, cdir):
        os.makedirs(d, exist_ok=True)
    fails, rows, cards = [], [], []
    M = mask()

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    # ---- 1. the design's states ----
    for name, kw, pos, force in STATES:
        hide, tf = ref_state(**kw)
        a = ref_render(os.path.join(ref, name + ".png"), hide, tf)
        _, b = c_render(cdir, name, pos, 3, REP_FORCE=force)
        d, dm = diff(a, b, M)
        check(d < 2.5, f"{name}: C path vs the header's rules, mean |difference| {d:.2f}")
        cols = [("reference (header rules)", a), ("C path (ui.c, forced state)", b), ("difference x2", heat(dm, M))]
        du = None
        if user and os.path.exists(os.path.join(user, f"repeat_{name}_3x.png")):
            u = Image.open(os.path.join(user, f"repeat_{name}_3x.png")).convert("RGB").resize((W, H), Image.LANCZOS)
            du, dum = diff(u, b, M)
            print(f"     {name}: vs the designer's PNG, mean |difference| {du:.2f}" + (f" ({REACH[name]})" if name in
                                                                                        REACH else ""))
            cols = [("designer's preview", u)] + cols[1:2] + [("difference x2 (vs designer)", heat(dum, M))]
        row = Image.new("RGB", (len(cols) * (W + 10), H + 24), (30, 30, 30))
        for j, (lab, im) in enumerate(cols):
            row.paste(im, (j * (W + 10), 24))
            ImageDraw.Draw(row).text((j * (W + 10) + 4, 4), f"{name}: {lab}", fill=(255, 255, 255))
        row.save(os.path.join(out, name + ".png"))
        rows.append(row)
        cards.append((name, pos, d, du))
    sheet = Image.new("RGB", (rows[0].width, sum(r.height for r in rows)))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y))
        y += r.height
    sheet.save(os.path.join(out, "all.png"))

    # ---- 2. the DSP running (shown) ----
    dsp = [("dsp_every", (12, 19, 16, 16), 52, {}, "EVERY BAR, 1/16, gate 8/16: a fill burning down, the wheel turning"),
           ("dsp_spinup", (18, 19, 22, 1), 50, {}, "EVERY BAR, 1/8, gate 1 bar, SPIN UP: the snake climbing"),
           ("dsp_tapestop", (24, 5, 25, 31), 160, {}, "EVERY 4 BARS, 1/4, gate 2 bars, TAPE STOP"),
           ("manual", (12, 1, 16, 9), 3, {"REP_FORCE": "active=1,hold=1,key=1,kind=1,blue=4,grid=4,rep=5,ph=300,"
                                                        "bar=0,depth=8192"},
            "OFF, the manual key held (forced): the latch, eye open, pow steady")]
    for name, pos, frames, env, cap in dsp:
        c_render(cdir, name, pos, frames, **env)
        cards.append((name, pos, None, cap))

    # ---- 3. the C path only ----
    D = (12, 5, 9, 16)
    n0, _ = c_render(cdir, "ro_none", D, 40)
    n1, _ = c_render(cdir, "ro_turn", D, 40, REP_TURN="30:2:10240")
    n2, _ = c_render(cdir, "ro_gone", D, 90, REP_TURN="30:2:10240")
    n3, _ = c_render(cdir, "ro_lfo", D, 40, FX_LIVE="30000,0,26000,5000")
    n4, _ = c_render(cdir, "ro_lfo_base", (29, 0, 25, 5), 40)
    n5, _ = c_render(cdir, "ro_none90", D, 90)
    check(n1 > n0 and n2 == n5 and n3 == n4, f"readout: drawn 10 frames after a stored-knob turn ({n1} line segments, "
          f"{n0} without: BAR k/n), gone 60 frames later ({n2}), none from TE's LFO alone ({n3} = {n4} as stored)")
    L = (12, 11, 9, 16)                                             # 1/16, CHANCE 30 %, gate 4/16: beat rolls
    t, per = first_lost(L)
    f_lost = math.ceil(t * 30) + 1
    _, a = c_render(cdir, "lost_before", L, f_lost - 3)
    _, b = c_render(cdir, "lost_on", L, f_lost)
    band = (slice(int(138 * SCALE), int(142 * SCALE)), slice(int(30 * SCALE), int(310 * SCALE)))
    wa = (np.asarray(a, int)[band].min(axis=2) > 245).sum()
    wb = (np.asarray(b, int)[band].min(axis=2) > 245).sum()
    check(wa > 200 and wb < 40, f"a lost roll dims the gate (pure white ground pixels {wa} before the roll, {wb} just after)")
    dbg = (os.environ.get("PREVIEW_DEFS", "") + " -DUI_DEBUG").strip()
    n_p, _ = c_render(cdir, "probe", D, 60, REP_SHIFT=1, PREVIEW_DEFS=dbg)
    check(n_p > 300, f"--debug-ui, SHIFT held: the probe page ({n_p} line segments, TE's words and repeat's clock)")
    n_s, a = c_render(cdir, "shift_normal", D, 60, REP_SHIFT=1)
    n_q, b = c_render(cdir, "noshift60", D, 60)
    same = n_s == n_q and not np.abs(np.asarray(a, int) - np.asarray(b, int)).any()
    check(same, f"normal build, SHIFT held: no probe page, the screen as without SHIFT ({n_s} = {n_q} line segments, "
          f"same pixels)")
    c_render(cdir, "stopped", (12, 19, 9, 16), 75, REP_TAPE=0)
    check(True, "tape stopped: the metronome clock drives the screen (stopped.png)")
    n_l = subprocess.run([sys.executable, os.path.join(HERE, "preview.py"), cdir, "112"] + [str(v * 1024) for v in D],
                         env=dict(os.environ, NAME="lines.png", FRAMES="40", NO_SVG="1"), check=True,
                         capture_output=True, text=True).stdout
    n_l = int(n_l.strip().rsplit(":", 1)[1].split()[0])
    check(n_l > 50, f"without repeat.svg: the line screen ({n_l} line segments)")
    w = svg_cells.worst(SVG, None)
    check(w[0] < 0.7 * 3840, f"repeat.svg's heaviest element ~{w[0]} cells ({w[1]} {w[2]}), "
          f"{100 * w[0] / 3840:.0f} % of AGG's 3840 (all drawn)")

    # ---- the gallery ----
    html = ["<!doctype html><meta charset=utf-8><title>repeat screen</title><style>body{background:#111;color:#ddd;"
            "font:14px -apple-system,sans-serif;margin:24px}img{image-rendering:pixelated;border:1px solid #333;"
            "max-width:100%}.c{display:inline-block;width:620px;margin:0 18px 26px 0;vertical-align:top}"
            ".r{margin:0 0 26px}code{color:#9cf}</style>",
            "<h1>repeat: William Buck's screen through the C path (ui.c draw_wheel on the host)</h1>",
            "<p>The design's states, each row: " + ("the designer's own preview PNG | " if user else
                                                      "the header's rules (rsvg) | ") +
            "the C path with repeat_state forced to the state | the difference (x2). The strip below y 142 (shaded in "
            "the C render) and the BAR k/n text (top right, the C's) are left out of the numbers.</p>"]
    for name, pos, d, du in cards[:len(STATES)]:
        note = f" Not reachable as drawn: {REACH[name]}." if name in REACH else ""
        html.append(f"<div class=r><img src={name}.png><p><code>{name}</code> knob positions {pos}: vs the header "
                    f"rules {d:.2f}" + (f", vs the designer's PNG {du:.2f}" if du is not None else "") +
                    f".{note}</p></div>")
    html.append("<h2>The DSP running, and the C path only</h2>")
    for name, pos, _, cap in cards[len(STATES):]:
        html.append(f"<div class=c><img src=c/{name}.png width=600><p><code>{name}</code> {cap}</p></div>")
    for name, cap in (("ro_turn", "GATE turned: the readout in the top-left box"),
                      ("lost_before", "CHANCE 30 %, gate 4/16: before a roll"),
                      ("lost_on", "just after a lost roll: the gate dims for the rest of the beat"),
                      ("stopped", "tape stopped, EVERY BAR: the metronome clock"),
                      ("probe", "--debug-ui, SHIFT held: the probe page"), ("lines", "without repeat.svg: the line screen")):
        html.append(f"<div class=c><img src=c/{name}.png width=600><p><code>{name}</code> {cap}</p></div>")
    open(os.path.join(out, "index.html"), "w").write("\n".join(html))
    print(f"\ngallery: {out}/index.html, side by side: {out}/all.png")
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
