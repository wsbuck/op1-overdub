"""Runner for the space FX (space_check.py, the sound review's renders): test/space_main.c with space.c, and for the
simulator build pingpong.c, repeat.c and test/pingpong_ram.c (the lines full of garbage at boot), under bfin-elf-run,
all cases in one Docker container, 8 at a time. host=True builds space.c natively instead (fast, for development; the
C reference path, bit exact with the simulator's SPACE_C build).

A case is a Case: per 64-frame block the FX ("s" space, "S" space untimed, "p" pingpong, "r" repeat, "-" none), the
mode ("m" mono on L, "s" stereo), the four params, the stored white knob (-1: as K2), the blocks TE ran without the FX
before it, the green key; and the input frames (N x 2 int16).
"""
import os
import shutil
import subprocess
import tempfile

import numpy as np

FS = 44100
HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
STATE = ["level", "t60", "frz", "inf", "zone", "clearing", "duck", "owner0", "owner1", "cyc"]
FLAGS = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"


class Case:
    def __init__(self, inp, mode="s"):
        inp = np.asarray(inp)
        if inp.ndim == 1:
            inp = np.stack([inp, inp], axis=1)
        self.inp = np.clip(np.round(inp), -32768, 32767).astype(np.int16)
        self.mode = mode
        self.lines = []

    def block(self, k, fx="s", mode=None, stored=-1, gap=0, key=0):
        self.lines.append(f"{fx} {mode or self.mode} {int(k[0])} {int(k[1])} {int(k[2])} {int(k[3])} {int(stored)} "
                          f"{int(gap)} {int(key)}")

    def run_for(self, k, blocks, **kw):
        for _ in range(blocks):
            self.block(k, **kw)

    def fill(self, k, **kw):
        """blocks with knobs k up to the end of the input"""
        while len(self.lines) * 64 < len(self.inp):
            self.block(k, **kw)


def run(cases, sim=True, keep=None, defs="", asm=True):
    """{name: Case} -> {name: (out N x 2 int, state dict of arrays, (max, mean) cycles)}"""
    w = tempfile.mkdtemp(prefix="space_")
    cmds = []
    for name, c in cases.items():
        n = len(c.lines)
        inp = c.inp[:n * 64]
        if len(inp) < n * 64:
            inp = np.concatenate([inp, np.zeros((n * 64 - len(inp), 2), np.int16)])
        inp.astype("<i2").tofile(os.path.join(w, name + ".in"))
        open(os.path.join(w, name + ".ctl"), "w").write("\n".join(c.lines) + "\n")
        exe = "/tmp/m" if sim else os.path.join(w, "m")
        d = "/w" if sim else w
        cmds.append(f"{'bfin-elf-run ' if sim else ''}{exe} {d}/{name}.ctl {d}/{name}.in {d}/{name}.out {d}/{name}.st "
                    f"2> {d}/{name}.err")
    open(os.path.join(w, "cmds"), "w").write("\n".join(cmds) + "\n")
    if sim:
        dd = defs + ("" if asm else " -DSPACE_C")
        open(os.path.join(w, "run.sh"), "w").write(
            "set -e; cd /tmp\n"
            f"bfin-elf-gcc {FLAGS} {dd} -c -o s.o /src/space.c\n"
            f"bfin-elf-gcc {FLAGS} -c -o r.o /src/repeat.c\n"
            f"bfin-elf-gcc {FLAGS} -c -o p.o /src/pingpong.c\n"
            f"bfin-elf-gcc -std=gnu99 -msim -O1 -Wall {dd} -I /src -o m /src/test/space_main.c s.o r.o p.o "
            "/src/test/pingpong_ram.c\n"
            "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n")
        subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v",
                        f"{w}:/w", "op1-bfin", "sh", "/w/run.sh"], check=True)
    else:
        subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wno-shift-negative-value", "-DSPACE_HOST", "-DSPACE_NO_KEY",
                        *defs.split(), "-I", SLOT,
                        "-o", os.path.join(w, "m"), os.path.join(HERE, "space_main.c"), os.path.join(SLOT, "space.c")],
                       check=True)
        subprocess.run(["xargs", "-P", "8", "-I", "CMD", "sh", "-c", "CMD"], stdin=open(os.path.join(w, "cmds")),
                       check=True)
    res = {}
    for name in cases:
        out = np.fromfile(os.path.join(w, name + ".out"), dtype="<i2").astype(np.int64).reshape(-1, 2)
        rows = [l.split()[1:] for l in open(os.path.join(w, name + ".st")) if l.startswith("B")]
        st = {k: np.array([int(r[i]) for r in rows], dtype=np.int64) for i, k in enumerate(STATE)}
        e = open(os.path.join(w, name + ".err")).read().split()
        res[name] = (out, st, (int(e[2]), int(e[4])) if len(e) >= 5 else (0, 0))
        if "peak" in e:
            st["peak"] = int(e[e.index("peak") + 1])
    if keep:
        os.makedirs(keep, exist_ok=True)
        for f in os.listdir(w):
            shutil.copy(os.path.join(w, f), keep)
    shutil.rmtree(w)
    return res
