#!/usr/bin/env python3
"""The space FX screen (design/fx/space.svg, William Buck's design) through the real C path vs the design's states.

  python3 src/slot/test/space_screen_check.py [OUTDIR] [--user PNG_DIR]

1. The design's preview states (smallroom, cathedral, void_long, shimmer, frozen, ducking, synth_wet, off; the
   designer's gen_space_moon.py states(); and sub, gate, swell, the v1.1 zones, from his b_state()): preview.py renders ui.c's draw_space on the host with space_ui forced to the
   state (SPACE_FORCE, no DSP) and the knobs set to reach it; the reference is space.svg with the header's rules applied
   (the designer's own b_state(), imported from design/fx/gen_space_moon.py) rendered by rsvg at the same size. Must
   match (mean |pixel difference| < 2.5 on what the panel shows). With --user, also each beside the designer's own PNG
   (space_<state>_3x.png), the difference reported.
2. The C path only: the readout after a stored-knob turn (and not from TE's LFO alone), the screen following the LFO's
   knobs, a clean start, the CPU page with SHIFT in diagnostic builds (and not in release builds), the line screen
   without space.svg, the DSP running, SVG cells.
Writes the C renders, side-by-side PNGs, all.png and index.html to OUTDIR.
"""
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "design", "fx"))
import gen_space_moon as gen    # noqa: E402  (the designer's rules: b_state, states)
import svg_cells                # noqa: E402

SVG = os.path.join(ROOT, "design", "fx", "space.svg")
RSVG = "/opt/homebrew/bin/rsvg-convert"
SCALE = 1.175 * 3                       # preview.py's
W, H = int(320 * SCALE), int(160 * SCALE)


def q15_db(db):
    return max(1, round(32768 * 10 ** (db / 20)))


def level(lv):
    """the tail level 0..1 as the screen reads it (-72 .. 0 dBFS) -> space_ui.level, Q15"""
    return q15_db(-72 * (1 - lv))


# The designer's states as the device reaches them: engine (113 master, 213 synth), raw knobs (SPACE, DECAY, TAIL,
# MIX) and space_ui forced. HOW: how each is reached where it isn't just the knobs.
STATES = {
    "smallroom": (213, (0, 9830, 3584, 9830), f"zone=0,level={level(0.5)}"),
    "cathedral": (213, (24576, 19660, 8192, 14745), f"zone=0,level={level(0.8)}"),
    "void_long": (213, (32767, 31500, 10239, 16384), f"zone=0,level={level(0.9)},inf=32768"),
    "shimmer": (213, (19661, 22937, 19455, 13107), f"zone=2,level={level(0.7)}"),
    "frozen": (213, (24576, 26214, 12800, 14745), f"zone=1,level={level(0.6)},frz=32768,key=1"),
    "ducking": (113, (12288, 16384, 6144, 29491), f"zone=0,level={level(0.4)},duck={q15_db(-24 * 0.8)}"),
    "synth_wet": (213, (16384, 16384, 5000, 32767), f"zone=0,level={level(0.6)}"),
    "off": (213, (16384, 16384, 6144, 13107), None),
    # the v1.1 zones (not among the designer's previews: b_state() with zone sub / gate / swell)
    "sub": (213, (16384, 16384, 21000, 13107), f"zone=3,level={level(0.6)}"),
    "gate": (113, (16384, 16384, 26112, 13107), f"zone=4,level={level(0.6)}"),
    "swell": (213, (16384, 16384, 30000, 13107), f"zone=5,level={level(0.6)}"),
}
V11 = {z: dict(space=2, decay=0.5, zone=z, amount=0.6, mix=0.4, master=z == "gate", level=0.6)
       for z in ("sub", "gate", "swell")}
HOW = {
    "void_long": "DECAY at INFINITE (detent 30, 31500 raw: the track's 12 stretches), not the knob's end (FREEZE)",
    "frozen": "the green push held at DECAY 0.8 (the knob's FREEZE detent would draw all 12 stretches)",
    "shimmer": "SPACE 2.4: CHURCH's ridges 2.4 px down; TAIL at SHIMMER +12+19's top (100 %)",
    "synth_wet": "TAIL 0.49 (5000 raw): the design's 0.5 rounds half to even (phase 2); 0.5 on the knob is 2.5 -> 3",
    "ducking": "the duck now -19.2 dB (0.8 of -24 dB); MIX 0.9: 4 duck dots",
    "off": "the FX switched off (not running): SPACE / TAIL / MIX as stored",
}


def mask():
    m = np.ones((H, W), bool)
    m[int(142 * SCALE):] = False                                   # not on the panel
    return m


def c_render(out, name, eng, knobs, frames, **env):
    e = dict(os.environ, NAME=name + ".png", FRAMES=str(frames), **{a: str(b) for a, b in env.items()})
    r = subprocess.run([sys.executable, os.path.join(HERE, "preview.py"), out, str(eng)] + [str(v) for v in knobs],
                       env=e, check=True, capture_output=True, text=True)
    segs = int(r.stdout.strip().rsplit(":", 1)[1].split()[0])
    return segs, Image.open(os.path.join(out, name + ".png")).convert("RGB")


def ref_render(path, hide, tf):
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
    subprocess.run([RSVG, "-w", str(W), "-h", str(H), "-b", "#000000", fh.name, "-o", path], check=True)
    os.unlink(fh.name)
    return Image.open(path).convert("RGB")


def diff(a, b, m):
    d = np.abs(np.asarray(a, float) - np.asarray(b, float)).mean(axis=2)
    return d[m].mean(), d


def heat(d, m):
    v = np.clip(d * 2, 0, 255).astype(np.uint8)
    v[~m] = 40
    return Image.fromarray(np.stack([v, v // 3, v // 3], axis=2))


def main():
    args = list(sys.argv[1:])
    user = None
    if "--user" in args:
        i = args.index("--user")
        user = args[i + 1]
        del args[i:i + 2]
    out = args[0] if args else tempfile.mkdtemp(prefix="space_screen_")
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
    rules = gen.states(gen.b_state)
    rules.update({z: gen.b_state(**kw) for z, kw in V11.items()})
    for name, (eng, knobs, force) in STATES.items():
        hide, tf = rules[name]
        a = ref_render(os.path.join(ref, name + ".png"), hide, tf)
        env = {"SPACE_FORCE": force} if force else {"FX_OFF": 3}
        _, b = c_render(cdir, name, eng, knobs, 3, **env)
        d, dm = diff(a, b, M)
        check(d < 2.5, f"{name}: C path vs the header's rules, mean |difference| {d:.2f}")
        cols = [("reference (header rules)", a), ("C path (ui.c, forced state)", b), ("difference x2", heat(dm, M))]
        du = None
        up = os.path.join(user, f"space_{name}_3x.png") if user else ""
        if user and os.path.exists(up):
            u = Image.open(up).convert("RGB").resize((W, H), Image.LANCZOS)
            du, dum = diff(u, b, M)
            print(f"     {name}: vs the designer's PNG, mean |difference| {du:.2f}")
            cols = [("designer's preview", u), ("C path (ui.c, forced state)", b),
                    ("difference x2 (vs designer)", heat(dum, M)), ("reference (header rules)", a)]
        row = Image.new("RGB", (len(cols) * (W + 10), H + 24), (30, 30, 30))
        for j, (lab, im) in enumerate(cols):
            row.paste(im, (j * (W + 10), 24))
            ImageDraw.Draw(row).text((j * (W + 10) + 4, 4), f"{name}: {lab}", fill=(255, 255, 255))
        row.save(os.path.join(out, name + ".png"))
        rows.append(row)
        cards.append((name, eng, knobs, force, d, du))
    sheet = Image.new("RGB", (rows[0].width, sum(r.height for r in rows)))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y))
        y += r.height
    sheet.save(os.path.join(out, "all.png"))

    # ---- 2. the C path only ----
    K = (16384, 16384, 3000, 9830)
    F = f"zone=0,level={level(0.5)}"
    n0, _ = c_render(cdir, "ro_none", 213, K, 40, SPACE_FORCE=F)
    n1, _ = c_render(cdir, "ro_turn", 213, K, 40, SPACE_FORCE=F, SPACE_TURN="30:1:24000")
    n2, _ = c_render(cdir, "ro_gone", 213, K, 90, SPACE_FORCE=F, SPACE_TURN="30:1:24000")
    n3, _ = c_render(cdir, "ro_lfo", 213, K, 40, SPACE_FORCE=F, FX_LIVE="16384,24000,3000,9830")
    n5, _ = c_render(cdir, "ro_none90", 213, K, 90, SPACE_FORCE=F)
    check(n1 > n0 and n2 == n5 and n3 == n0, f"readout: drawn 10 frames after a stored-knob turn ({n1} line segments, "
          f"{n0} without), gone 60 frames later ({n2}), none from TE's LFO alone ({n3})")
    for i, (knob, txt) in enumerate(((0, "0:0:24576"), (2, "0:2:19000"), (3, "0:3:29491"))):
        c_render(cdir, f"ro_k{knob}", 113 if knob == 3 else 213, K, 10, SPACE_FORCE=F + (",zone=2" if knob == 2 else ""),
                 SPACE_TURN=txt.replace("0:", "5:", 1))
    for z, raw in ((3, 21000), (4, 26112), (5, 30720)):            # the v1.1 zones' readouts: SUB-12, GATE, SWELL
        c_render(cdir, f"ro_z{z}", 213, K, 10, SPACE_FORCE=F.replace("zone=0", f"zone={z}"), SPACE_TURN=f"5:2:{raw}")
    _, s0 = c_render(cdir, "lfo_stored", 213, (0, 16384, 3000, 9830), 3, SPACE_FORCE=F)
    _, s1 = c_render(cdir, "lfo_live", 213, (0, 16384, 3000, 9830), 3, SPACE_FORCE=F, FX_LIVE="24576,16384,3000,9830")
    _, s2 = c_render(cdir, "lfo_as", 213, (24576, 16384, 3000, 9830), 3, SPACE_FORCE=F)
    dl, _ = diff(s1, s2, M)
    ds, _ = diff(s0, s1, M)
    check(dl < 0.5 and ds > 2, f"TE's LFO moves the screen: blue live at CATHEDRAL, stored at ROOM draws CATHEDRAL "
          f"(vs CATHEDRAL stored {dl:.2f}, vs ROOM {ds:.2f})")
    _, cl = c_render(cdir, "clean", 213, (16384, 26000, 19455, 13107), 3, SPACE_FORCE=F.replace("zone=0", "zone=2") +
                     ",clearing=1")
    _, nc = c_render(cdir, "not_clean", 213, (16384, 26000, 19455, 13107), 3, SPACE_FORCE=F.replace("zone=0", "zone=2"))
    green = lambda im: int(((np.asarray(im, int)[..., 1] > 200) & (np.asarray(im, int)[..., 0] < 60)).sum())
    white = lambda im: int((np.asarray(im, int)[:int(98 * SCALE), int(150 * SCALE):int(190 * SCALE)].min(axis=2)
                            > 230).sum())
    check(green(cl) < 20 < green(nc) and white(cl) < white(nc),
          f"a clean start hides the track and the shimmer (green px {green(cl)} vs {green(nc)}, pillar white px "
          f"{white(cl)} vs {white(nc)})")
    np_, _ = c_render(cdir, "cpu_page", 113, K, 3, SPACE_FORCE=F, SPACE_SHIFT=1, PREVIEW_DEFS="-DSP_CPU_PAGE")
    nr, _ = c_render(cdir, "shift_release", 113, K, 3, SPACE_FORCE=F, SPACE_SHIFT=1)
    nn, _ = c_render(cdir, "noshift_release", 113, K, 3, SPACE_FORCE=F)
    check(np_ > 300 and nr == nn, f"SHIFT held: the CPU page in diagnostic builds ({np_} line segments), "
          f"nothing different in release builds ({nr} = {nn})")
    nl = subprocess.run([sys.executable, os.path.join(HERE, "preview.py"), cdir, "113"] + [str(v) for v in K],
                        env=dict(os.environ, NAME="lines.png", FRAMES="3", NO_SVG="1", SPACE_FORCE=F,
                                 PREVIEW_DEFS="-DSP_CPU_PAGE"), check=True, capture_output=True, text=True).stdout
    nl = int(nl.strip().rsplit(":", 1)[1].split()[0])
    nlr = subprocess.run([sys.executable, os.path.join(HERE, "preview.py"), cdir, "113"] + [str(v) for v in K],
                         env=dict(os.environ, NAME="lines_release.png", FRAMES="3", NO_SVG="1", SPACE_FORCE=F),
                         check=True, capture_output=True, text=True).stdout
    nlr = int(nlr.strip().rsplit(":", 1)[1].split()[0])
    check(nl > 300 and nlr < nl - 100, f"without space.svg: the line screen, CPU lines at its foot in diagnostic builds "
          f"({nl} segments), none in normal builds ({nlr})")
    c_render(cdir, "dsp_hall", 113, (8192, 16384, 5120, 8192), 30)
    c_render(cdir, "dsp_shimmer", 213, (19661, 24000, 17000, 14000), 30)
    check(True, "the DSP running (space.c on the host): dsp_hall.png, dsp_shimmer.png (shown)")
    w = svg_cells.worst(SVG, None)
    check(w[0] < 0.7 * 3840, f"space.svg's heaviest element ~{w[0]} cells ({w[1]} {w[2]}), "
          f"{100 * w[0] / 3840:.0f} % of AGG's 3840 (build limit 70 %)")

    # ---- the gallery ----
    html = ["<!doctype html><meta charset=utf-8><title>space screen</title><style>body{background:#111;color:#ddd;"
            "font:14px -apple-system,sans-serif;margin:24px}img{image-rendering:pixelated;border:1px solid #333;"
            "max-width:100%}.c{display:inline-block;width:620px;margin:0 18px 26px 0;vertical-align:top}"
            ".r{margin:0 0 26px}code{color:#9cf}</style>",
            "<h1>space: William Buck's screen through the C path (ui.c draw_space on the host)</h1>",
            "<p>The design's states, each row: " + ("the designer's own preview PNG | the C path with space_ui forced "
                                                      "| the difference (x2) | the header's rules (rsvg)" if user else
                                                      "the header's rules (rsvg) | the C path | the difference (x2)") +
            ". The strip below y 142 (shaded in the C render) is left out of the numbers.</p>"]
    for name, eng, knobs, force, d, du in cards:
        note = f" How: {HOW[name]}." if HOW.get(name) else ""
        html.append(f"<div class=r><img src={name}.png><p><code>{name}</code> engine {eng}, knobs {knobs}, "
                    f"<code>{force or 'FX off'}</code>: vs the header rules {d:.2f}" +
                    (f", vs the designer's PNG {du:.2f}" if du is not None else "") + f".{note}</p></div>")
    html.append("<h2>The C path only</h2>")
    for name, cap in (("ro_turn", "DECAY turned: the readout (seconds and the multiplier) in the top-left box"),
                      ("ro_k0", "SPACE turned: CATHEDRAL"), ("ro_k2", "TAIL turned into SHIMMER +12+19"),
                      ("ro_k3", "MIX turned above the middle on the master: the duck depth"),
                      ("ro_z3", "TAIL turned into SUB -12"), ("ro_z4", "TAIL turned into GATE: the hold"),
                      ("ro_z5", "TAIL turned into SWELL: the rise"),
                      ("lfo_live", "TE's LFO: blue live at CATHEDRAL, stored at ROOM"),
                      ("clean", "a clean start: no track, no shimmer"), ("not_clean", "the same, running"),
                      ("cpu_page", "SHIFT held (diagnostic builds): the CPU page"),
                      ("lines", "without space.svg: the line screen (diagnostic build: CPU lines at the foot)"),
                      ("dsp_hall", "the DSP running, master, HALL (1 s of a saw, then the tail)"),
                      ("dsp_shimmer", "the DSP running, synth FX, SHIMMER +12+19")):
        html.append(f"<div class=c><img src=c/{name}.png width=600><p><code>{name}</code> {cap}</p></div>")
    open(os.path.join(out, "index.html"), "w").write("\n".join(html))
    print(f"\ngallery: {out}/index.html, side by side: {out}/all.png")
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
