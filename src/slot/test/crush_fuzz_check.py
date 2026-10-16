#!/usr/bin/env python3
"""Fuzz the crush screen's C draw path (ui.c draw_pyramid, crush_readout, draw_crush) on the host.

  python3 src/slot/test/crush_fuzz_check.py [FRAMES]

Hypothesis under test (CRUSH4/5 asserted agg.cpp:339 on the device): degenerate geometry, a matrix or coordinate
computed from unwritten, garbage or extreme state, makes one huge edge whose cells overflow AGG's 16 blocks. Builds
ui.c + preview_host.c + crush.c as preview.py does and runs CRUSH_FUZZ frames: frame 0 with crush_state as the image
loads it (before any DSP block), then crush_state's screen fields (gr, k[], rate_hz, bits_q12, mu, level, flips) and
the raw knobs at 0, -1, INT_MAX, INT_MIN, int16 extremes and random, mono and master alternating, the readout
triggered. Checks every frame: every visible SVG leaf, with the node matrices the C wrote composed down the tree,
stays within the screen plus a margin, every matrix is finite and sane (|a|, |b|, |c|, |d| <= 4, |e|, |f| <= 400 units);
every te_line segment (readout, labels) too; the same without the SVG (the line placeholder).
"""
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
ENGINE = os.path.join(os.path.dirname(SLOT), "engine")
ROOT = os.path.dirname(os.path.dirname(SLOT))
sys.path.insert(0, os.path.join(ROOT, "design", "fx"))
import svg_cells as sc      # noqa: E402

SVG = os.path.join(ROOT, "design", "fx", "crush3d.svg")
BOX = (-60, -60, 380, 220)          # screen 320 x 160 plus a margin, SVG units


def build(tmp, staged=False):
    data, gdata, exe = (os.path.join(tmp, n) for n in ("wt.bin", "grain.bin", "fuzz_staged" if staged else "fuzz"))
    subprocess.run([sys.executable, os.path.join(ENGINE, "gen_wt_data.py"), data], check=True, capture_output=True)
    subprocess.run([sys.executable, os.path.join(ENGINE, "gen_grain_data.py"), gdata], check=True, capture_output=True)
    subprocess.run(["cc", "-std=gnu99", "-O1", "-Wall", "-I", ENGINE, "-I", SLOT, "-lm",
                    "-DPALETTE=((const uint8_t *)host_palette)", "-DKNOBS(s)=((const int16_t *)host_knobs)",
                    "-DPROF=((volatile uint32_t *)host_prof)", "-DFX_KNOBS(m,s)=((const int16_t *)host_knobs)",
                    "-DTE_MATRIX=((const int32_t *)host_matrix)", "-include", os.path.join(HERE, "preview_decl.h")]
                   + (["-DCRUSH_STAGED"] if staged else []) + [
                    os.path.join(SLOT, "ui.c"), os.path.join(HERE, "preview_host.c"), os.path.join(SLOT, "crush.c"),
                    "-DDISTORTION_CURVE_ONLY", os.path.join(SLOT, "distortion.c"), "-o", exe], check=True)
    return exe, data, gdata


def run(exe, data, gdata, frames, svg, reachable=False):
    env = dict(os.environ, CRUSH_FUZZ=str(frames), GRAIN_DATA=gdata)
    if reachable:
        env["CRUSH_FUZZ_REACHABLE"] = "1"
    if svg:
        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "display"))
        open(os.path.join(d, "display", "crush3d.svg"), "w").write(open(SVG).read())
        ids = os.path.join(d, "ids.txt")
        lines = []
        for el in ET.parse(SVG).iter():
            if el.get("id"):
                col = [el.get(a, "-") for a in ("fill", "stroke")]
                lines.append(f"crush3d.svg {el.get('id')} " + " ".join(c[1:] if c.startswith("#") else "-" for c in col))
        open(ids, "w").write("\n".join(lines) + "\n")
        env.update(SVG_DIR=os.path.join(d, "display"), SVG_IDS=ids)
    out = subprocess.run([exe, data, "111", "0", "0", "0", "0"], env=env, capture_output=True, text=True, check=True)
    frames_out, cur = [], None
    for ln in out.stdout.split("\n"):
        f = ln.split()
        if not f:
            continue
        if f[0] == "F":
            cur = {"nodes": {}, "lines": []}
            frames_out.append(cur)
        elif f[0] == "N" and cur is not None:
            cur["nodes"][f[2]] = (int(f[3]), [int(v) for v in f[4:10]])
        elif cur is not None and len(f) == 6:
            cur["lines"].append([int(v) for v in f])
    return frames_out


def main():
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 700
    tmp = tempfile.mkdtemp()
    exe, data, gdata = build(tmp)
    root = ET.parse(SVG).getroot()
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    for reach in (False, True):
        fr = run(exe, data, gdata, frames, True, reach)
        worst_box, worst_m, worst_cells, bad = None, 0, 0, []
        for i, f in enumerate(fr):
            nodes = f["nodes"]
            for nid, (vis, m) in nodes.items():
                if m[0] != -1 or any(v != -1 for v in m):
                    big = max(abs(v) for v in m[:4]) / 1024, max(abs(v) for v in m[4:]) / 1024
                    worst_m = max(worst_m, big[1])
                    if big[0] > 4 or big[1] > 400:
                        bad.append((i, nid, m))

            def walk(el, mt):
                nonlocal worst_box, worst_cells
                i_ = el.get("id")
                vis, m = nodes.get(i_, (1, [-1] * 6)) if i_ else (1, [-1] * 6)
                if not vis:
                    return
                own = tuple(v / 1024 for v in m) if (m[0] != -1 or any(v != -1 for v in m)) else sc.parse_transform(
                    el.get("transform"))
                mt = sc.mat(mt, own)
                tag = el.tag.split("}")[-1]
                if tag not in ("svg", "g"):
                    pts = [(mt[0] * x + mt[2] * y + mt[4], mt[1] * x + mt[3] * y + mt[5]) for p, _ in sc.subpaths(el, tag)
                           for x, y in p]
                    if pts:
                        bx = (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))
                        if bx[0] < BOX[0] or bx[1] < BOX[1] or bx[2] > BOX[2] or bx[3] > BOX[3]:
                            bad.append((i, i_ or tag, bx))
                        cells = max(sc.element_cells(el, tag, mt))
                        worst_cells = max(worst_cells, cells)
                for c in el:
                    walk(c, mt)
            walk(root, (1, 0, 0, 1, 0, 0))
            for ln in f["lines"]:
                x0, y0, x1, y1 = (v / 1024 for v in ln[:4])
                if min(x0, x1) < BOX[0] or min(y0, y1) < BOX[1] or max(x0, x1) > BOX[2] or max(y0, y1) > BOX[3]:
                    bad.append((i, "te_line", (x0, y0, x1, y1)))
        check(len(fr) == frames and not bad, f"pyramid, {len(fr)} {'reachable' if reach else 'garbage'} fuzzed frames (first: the state as loaded): every visible "
              f"element inside {BOX}, matrices sane (largest translate {worst_m:.1f} units), heaviest element under these "
              f"states ~{int(worst_cells)} cells{'; bad: ' + str(bad[:5]) if bad else ''}")
    fr = run(exe, data, gdata, frames, False)
    bad = [(i, ln) for i, f in enumerate(fr) for ln in f["lines"]
           if min(ln[0], ln[2]) / 1024 < BOX[0] or max(ln[0], ln[2]) / 1024 > BOX[2]
           or min(ln[1], ln[3]) / 1024 < BOX[1] or max(ln[1], ln[3]) / 1024 > BOX[3]]
    n_lines = sum(len(f["lines"]) for f in fr)
    check(len(fr) == frames and not bad, f"line placeholder (no SVG), {len(fr)} fuzzed frames, {n_lines} segments: all "
          f"inside {BOX}{'; bad: ' + str(bad[:5]) if bad else ''}")
    # the staged bring-up (build.py --staged-screens): pyr hidden only in stage 0, the voxels only in stages 0 .. 1,
    # then back for good (CRUSH6 hid pyr at stage 0 and never showed it again)
    exe2, _, _ = build(tmp, staged=True)
    fr = run(exe2, data, gdata, 600, True, True)
    wrong = []
    for i, f in enumerate(fr):
        stage = i // 90
        pv = f["nodes"].get("pyr", (1, None))[0]
        vox = max(f["nodes"].get(f"vox{j}", (0, None))[0] for j in range(1, 5))
        if pv != (stage >= 1) or (stage < 2 and vox):
            wrong.append((i, stage, pv, vox))
    later = sum(f["nodes"].get("pyr", (0, None))[0] for f in fr[450:])
    check(not wrong and later == len(fr) - 450, f"staged bring-up: pyr hidden only in stage 0, voxels only in stages 0 .. 1, "
          f"the pyramid drawn on all {len(fr) - 450} frames after the stages{'; wrong: ' + str(wrong[:5]) if wrong else ''}")

    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
