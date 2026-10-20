#!/usr/bin/env python3
"""TE's LFO on our FX: the DSP publishes the knobs it used, and the screens draw from them (notes/fx-slot.md "LFO").

  python3 src/slot/test/fx_lfo_check.py [OUTDIR]

TE modulates only the per-block params copy (0xFF801724) that a synth / drum FX's process gets; the stored knobs the
screens used to read never move. Two parts:
  1. Simulator (bfin-elf-run, fx_lfo_main.c): each FX, mono and stereo, gets a new params array every block (the
     stored knobs plus a TE-style LFO stepped per block on all four knobs, then held). After every call its
     <source>_pub holds exactly that block's params and counts the call, and only its own instance publishes (the
     driver exits 1 otherwise). crush's smoothed knobs (crush_state.k, what its screen draws) follow the params and
     settle on them. The output stays in range while the knobs move.
  2. Host (preview.py: ui.c with stand-ins), per FX screen, synth (mono) and master (stereo): stored knobs S with the
     FX publishing modulated knobs M (FX_LIVE) draws exactly what stored knobs M draw (node matrices, visibility,
     colours and every line: the PNGs are identical), and differs from stored S alone; with the FX not running
     (FX_OFF) the ensemble and distortion screens fall back to the stored knobs (identical to S; pingpong's time and
     crush's rate / bits come from the DSP's own state, which stays as it last ran). repeat's and space's screens run the
     real repeat.c / space.c on the host (on the params, as the device does), so its state follows them too. crush: knobs moved only by the LFO never
     bring up the value readout (it fires on stored-knob turns: crush_screen_check.py).
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
FX = ["ensemble", "pingpong", "distortion", "crush", "repeat", "space"]
BLOCKS = 600
STORED = {8: (15360, 21504, 0, 16384), 9: (28672, 14000, 12000, 10000), 10: (16000, 16384, 7475, 32767),
          11: (10240, 12288, 10240, 0), 12: (12288, 5120, 9216, 16384), 13: (8192, 16384, 5120, 8192)}   # fx.json
LIVE = {8: (6000, 30000, 26000, 9000), 9: (12000, 30000, 30000, 26000), 10: (30000, 4000, 26000, 20000),
        11: (26000, 28000, 2000, 12000), 12: (30000, 0, 26000, 5000),
        13: (20000, 9000, 2000, 20000)}   # what an LFO might make of them (space: white kept in TONE, its zone)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="fx_lfo_")
    os.makedirs(out, exist_ok=True)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    # ---- 1. simulator ----
    w = tempfile.mkdtemp(prefix="fx_lfo_sim_")
    cflags = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
    runs = [(i, m) for i in range(len(FX)) for m in "ms"]
    script = ["set -e; cd /tmp"] + [f"bfin-elf-gcc {cflags} -c -o {f}.o /src/{f}.c" for f in FX] + [
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -I /src -o t /src/test/fx_lfo_main.c "
        + " ".join(f"{f}.o" for f in FX) + " /src/test/pingpong_ram.c"]
    script += [f"bfin-elf-run /tmp/t {i} {m} {BLOCKS} > /w/{i}{m}.txt 2> /w/{i}{m}.err; echo $? > /w/{i}{m}.rc"
               for i, m in runs]
    open(os.path.join(w, "run.sh"), "w").write("\n".join(script) + "\n")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v", f"{w}:/w",
                    "op1-bfin", "sh", "/w/run.sh"], check=True)
    for i, m in runs:
        f, tag = FX[i], "mono" if m == "m" else "stereo"
        rc = open(os.path.join(w, f"{i}{m}.rc")).read().strip()
        err = open(os.path.join(w, f"{i}{m}.err")).read().strip()
        rows = np.array([[int(v) for v in l.split()[1:]] for l in open(os.path.join(w, f"{i}{m}.txt")) if l[0] == "B"])
        p = rows[:, 0:4] if len(rows) else np.zeros((0, 4))
        moved = len(rows) == BLOCKS and all(np.ptp(p[:BLOCKS // 2, j]) > 8000 for j in range(4))
        check(rc == "0" and moved, f"{f} {tag}: {BLOCKS} blocks with an LFO on knobs 1-4 (each swept "
              f"{', '.join(str(int(np.ptp(p[:BLOCKS // 2, j]))) for j in range(4))} raw): the published knobs are "
              "each block's params, one count per call, the other instance silent" + (f" ({err})" if err else ""))
        peak = int(rows[:, 12].max()) if len(rows) else 0
        check(0 < peak <= 32767, f"{f} {tag}: output peak {peak} while the knobs move (in range, not silent)")
        if f == "crush":
            c = rows[:, 8:12]
            lag = np.abs(c[40:BLOCKS // 2] - p[40:BLOCKS // 2]).max(axis=0)       # after the start from 0
            settle = np.abs(c[-1] - p[-1]).max()
            check(settle <= 1 and lag.max() < 4000, f"crush {tag}: the smoothed knobs the screen draws follow the LFO "
                  f"(largest lag {', '.join(map(str, lag))} raw while it moves) and settle on the params "
                  f"(off by {settle} after {BLOCKS // 2} still blocks)")

    # ---- 2. host: the screens ----
    def render(name, eng, k, frames=3, **env):
        e = dict(os.environ, NAME=name + ".png", FRAMES=str(frames), **{a: str(b) for a, b in env.items()})
        for v in ("FX_LIVE", "FX_OFF"):
            if v not in env:
                e.pop(v, None)
        r = subprocess.run([sys.executable, os.path.join(HERE, "preview.py"), out, str(eng)] + [str(v) for v in k],
                           env=e, check=True, capture_output=True, text=True)
        segs = int(r.stdout.strip().rsplit(":", 1)[1].split()[0])
        return segs, np.asarray(Image.open(os.path.join(out, name + ".png")).convert("RGB"), int)

    for fid, name in zip(range(8, 14), FX):
        S, M = STORED[fid], LIVE[fid]
        live = ",".join(map(str, M))
        for base, tag in ((200, "synth"), (100, "master")):
            eng = base + fid
            _, a = render(f"{name}_{tag}_stored", eng, S)
            _, b = render(f"{name}_{tag}_lfo", eng, S, FX_LIVE=live)
            _, c = render(f"{name}_{tag}_asif", eng, M)
            d_bc, d_ab = np.abs(b - c).mean(), np.abs(a - b).mean()
            check(d_bc == 0 and d_ab > 0.3, f"{name} {tag} screen: stored {S} + FX using {M} draws exactly stored {M} "
                  f"(difference {d_bc:.2f}), not stored {S} (difference {d_ab:.2f})")
            if name in ("ensemble", "distortion"):  # pingpong's time and crush's rate / bits: the DSP's state
                _, o = render(f"{name}_{tag}_off", eng, S, FX_LIVE=live, FX_OFF=1)
                check(np.abs(o - a).mean() == 0, f"{name} {tag} screen: FX not running -> the stored knobs "
                      f"(difference {np.abs(o - a).mean():.2f})")
    n_lfo, _ = render("crush_lfo_readout", 211, STORED[11], frames=10, FX_LIVE=",".join(map(str, LIVE[11])))
    n_turn, _ = render("crush_turn_readout", 211, STORED[11], frames=10, CRUSH_TURN="5:1:16384")
    check(n_lfo == 0 and n_turn > 0, f"crush readout: the LFO alone brings none ({n_lfo} line segments), a stored-knob "
          f"turn does ({n_turn})")
    print(f"\nimages: {out}")
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
