"""Simulator runner and test signals for the crush FX (crush_check.py).

run(cases) builds src/slot/crush.c with the device flags and test/crush_main.c for the Blackfin simulator, runs every
case under bfin-elf-run in one Docker container, 8 at a time, and returns ({name: (out int64 [frames, channels],
gr int64 [frames])}, {name: (max cycles, mean cycles per 64-frame call, bit flips over the run, then rate_hz, bits_q12, mu at the end)}). A case is
(mode "m"/"s", (k0, k1, k2, k3), input int array [frames] or [frames, 2], [schedule strings for crush_main.c]).
"""
import os
import shutil
import subprocess
import tempfile

import numpy as np

FS = 44100
HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)


def run(cases):
    w = tempfile.mkdtemp(prefix="fx_crush_")
    cmds = []
    for name, (mode, k, x, *sched) in cases.items():
        x = np.asarray(x)
        blocks = len(x) // 64
        x = np.clip(np.round(x[:blocks * 64]), -32768, 32767).astype("<i2")
        if mode == "s" and x.ndim == 1:
            x = np.stack([x, x], 1)
        x.tofile(os.path.join(w, name + ".in"))
        args = f"{mode} {' '.join(map(str, k))} {blocks} " + " ".join(sched[0] if sched else [])
        cmds.append(f"bfin-elf-run /tmp/m {args} < /w/{name}.in > /w/{name}.txt 2> /w/{name}.err")
    open(os.path.join(w, "cmds"), "w").write("\n".join(cmds) + "\n")
    open(os.path.join(w, "run.sh"), "w").write(
        "set -e; cd /tmp\n"
        "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror "
        "-c -o fx.o /src/crush.c\n"
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -o m /src/test/crush_main.c fx.o\n"
        "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v", f"{w}:/w",
                    "op1-bfin", "sh", "/w/run.sh"], check=True)
    out, cyc = {}, {}
    for name in cases:
        e = open(os.path.join(w, name + ".err")).read().split()
        if len(e) < 13:
            raise RuntimeError(f"case {name} failed: {' '.join(e)}")
        y = np.loadtxt(os.path.join(w, name + ".txt"), dtype=np.int64)
        out[name] = (y[:, :-1], y[:, -1])
        cyc[name] = (int(e[2]), int(e[4]), int(e[6]), int(e[8]), int(e[10]), int(e[12]))
    shutil.rmtree(w)
    return out, cyc


def sine(n, f, amp=16384, quarter=False):
    t = np.arange(n)
    return amp * np.sin(2 * np.pi * f * t / FS - (np.pi / 2 if quarter else 0))


def stereo_sine(n, f, amp=16384):
    return np.stack([sine(n, f, amp), sine(n, f, amp, True)], 1)


def saw(n, f, amp=16384):
    t = np.arange(n)
    y = sum(np.sin(2 * np.pi * k * f * t / FS) / k for k in range(1, int(20000 / f) + 1))
    return amp * y / np.abs(y).max()


def rms_db(x):
    x = np.asarray(x, float)
    return 20 * np.log10(np.sqrt((x ** 2).mean()) + 1e-9)
