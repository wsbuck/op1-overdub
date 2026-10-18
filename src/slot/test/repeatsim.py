"""Simulator runner for the repeat FX (repeat_check.py, the sound review's renders): test/repeat_main.c with repeat.c,
pingpong.c (the shared lines and owner word) and test/pingpong_ram.c (the lines full of garbage at boot) under
bfin-elf-run, all cases in one Docker container, 8 at a time.

A case is a Case: per 64-frame block the FX ("r" repeat, "p" pingpong), the mode ("m" mono on L, "s" stereo), the
four params and TE's words (tempo, transport, playhead, metronome countdown, slot), and the input frames (N x 2).
Tape() models TE's transport and metronome for the words: the playhead (samples x 64) advances 64 samples a block
while it rolls; the metronome counts down 64 samples a block and reloads with the samples per beat, keeping the
residue (its click), and clicks at once on the first block of play (notes/fx-beatrepeat.md "Sync").
"""
import os
import shutil
import subprocess
import tempfile

import numpy as np

FS = 44100
HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)


def spb_of(bpm):
    """TE's samples-per-beat word (Q14): (int)(2646000.0f / bpm * 16384.0f)"""
    return int(np.float32(np.float32(2646000.0) / np.float32(bpm)) * np.float32(16384.0))


def raw(p):
    """a knob detent position 0 .. 32 as a raw value"""
    return 32767 if p >= 32 else p * 1024


class Tape:
    def __init__(self, bpm=120.0, pos=0, rolling=True, state=None):
        self.spb = spb_of(bpm)
        self.pos = int(pos) * 64                 # Q6
        self.rolling = rolling
        self.state = state if state is not None else (2 if rolling else 0)
        self.c = 0 if rolling else self.spb // 3  # metronome countdown, Q14
        self.slot = 0

    def set_bpm(self, bpm):
        new = spb_of(bpm)
        self.c = int(self.c * new / self.spb)    # TE rescales the countdown on a tempo change
        self.spb = new

    def play(self, pos=None):
        if pos is not None:
            self.pos = int(pos) * 64
        self.rolling, self.state, self.c = True, 2, 0

    def stop(self):
        self.rolling, self.state = False, 0

    def words(self):
        """the words at this block's start, then one block of time"""
        w = (self.spb, self.state, self.pos & 0xFFFFFFFF, self.c, self.slot)
        if self.rolling:
            self.pos += 64 * 64
        self.c -= 64 << 14
        while self.c <= 0:
            self.c += self.spb
        return w


class Case:
    def __init__(self, inp, mode="m"):
        self.inp = np.asarray(inp, dtype=np.int16).reshape(-1, 2) if np.ndim(inp) == 2 else \
            np.stack([np.asarray(inp, dtype=np.int16)] * 2, axis=1)
        self.mode = mode
        self.lines = []

    def block(self, k, words, fx="r", mode=None):
        spb, st, pos, c, slot = words
        self.lines.append(f"{fx} {mode or self.mode} {k[0]} {k[1]} {k[2]} {k[3]} {spb} {st} {pos} {c} {slot}")

    def run_tape(self, k, tape, blocks, fx="r", mode=None):
        for _ in range(blocks):
            self.block(k, tape.words(), fx, mode)


STATE = ["active", "hold", "kind", "rep", "grid", "blue", "fold", "depth", "dir", "fires", "lost", "turn", "bar", "ph8",
         "bar8", "lock", "w", "filled", "owner0", "owner1", "pub"]


def run(cases, keep=None):
    """{name: Case} -> {name: (out N x 2 int, state dict of arrays, (max, mean) cycles)}"""
    w = tempfile.mkdtemp(prefix="repeat_")
    cmds = []
    for name, c in cases.items():
        n = len(c.lines)
        inp = c.inp[:n * 64]
        if len(inp) < n * 64:
            inp = np.concatenate([inp, np.zeros((n * 64 - len(inp), 2), np.int16)])
        inp.astype("<i2").tofile(os.path.join(w, name + ".in"))
        open(os.path.join(w, name + ".ctl"), "w").write("\n".join(c.lines) + "\n")
        cmds.append(f"bfin-elf-run /tmp/m /w/{name}.ctl /w/{name}.in /w/{name}.out /w/{name}.st 2> /w/{name}.err")
    open(os.path.join(w, "cmds"), "w").write("\n".join(cmds) + "\n")
    flags = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
    open(os.path.join(w, "run.sh"), "w").write(
        "set -e; cd /tmp\n"
        f"bfin-elf-gcc {flags} -c -o r.o /src/repeat.c\n"
        f"bfin-elf-gcc {flags} -c -o p.o /src/pingpong.c\n"
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -I /src -o m /src/test/repeat_main.c r.o p.o /src/test/pingpong_ram.c\n"
        "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v", f"{w}:/w",
                    "op1-bfin", "sh", "/w/run.sh"], check=True)
    res = {}
    for name in cases:
        out = np.fromfile(os.path.join(w, name + ".out"), dtype="<i2").astype(np.int64).reshape(-1, 2)
        rows = [l.split()[1:] for l in open(os.path.join(w, name + ".st")) if l.startswith("B")]
        st = {k: np.array([int(r[i]) for r in rows], dtype=np.int64) for i, k in enumerate(STATE)}
        e = open(os.path.join(w, name + ".err")).read().split()
        res[name] = (out, st, (int(e[2]), int(e[4])) if len(e) >= 5 else (0, 0))
    if keep:
        os.makedirs(keep, exist_ok=True)
        for f in os.listdir(w):
            shutil.copy(os.path.join(w, f), keep)
    shutil.rmtree(w)
    return res
