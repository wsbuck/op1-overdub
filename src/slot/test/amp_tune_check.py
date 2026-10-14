#!/usr/bin/env python3
"""Host checks for the amp screen's tuner (src/slot/amp_tune.h).

  python3 src/slot/test/amp_tune_check.py

Plucked guitar-like notes (harmonics 1..12, string decay, -60 dBFS noise) from drop D to the 24th fret of high E,
each a few cents off, in 18.9 ms windows taken across the first 0.9 s of the note; silence and noise. Checks the note
in every window, the cents (within 5 cents in the first 0.5 s, median error within 3), and that silence and noise read
as unpitched.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
ENGINE = os.path.join(os.path.dirname(SLOT), "engine")
FS, N = 44100, 13 * 64
RNG = np.random.default_rng(3)


def pluck(f, seconds=2.0, db=-15):
    t = np.arange(int(seconds * FS)) / FS
    v = sum(np.sin(2 * np.pi * f * h * t * np.sqrt(1 + 0.00004 * h * h) + RNG.uniform(0, 6)) / h ** 1.2
            * np.exp(-t * (0.8 + 0.6 * h)) for h in range(1, 13))
    v *= np.exp(-t / 1.2) * (1 - np.exp(-t / 0.002))
    v *= 32767 * 10 ** (db / 20) / np.abs(v).max()
    return v + RNG.normal(0, 32767 * 10 ** (-60 / 20), len(v))


def main():
    tmp = tempfile.mkdtemp()
    exe = os.path.join(tmp, "tune")
    subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror", "-I", SLOT, "-I", ENGINE,
                    os.path.join(HERE, "amp_tune_main.c"), "-o", exe], check=True)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    def run(windows):
        data = np.clip(np.round(np.concatenate(windows)), -32768, 32767).astype("<i2").tobytes()
        out = subprocess.run([exe], input=data, capture_output=True, check=True).stdout.split()
        return [int(v) for v in out]

    # (name, midi note, cents off)
    notes = [("D2", 38, 4), ("E2", 40, -7), ("A2", 45, 12), ("D3", 50, -20), ("G3", 55, 2), ("B3", 59, 31),
             ("E4", 64, -3), ("A4", 69, 0), ("E5", 76, 9), ("E6", 88, -15)]
    for name, m, c in notes:
        f = 440 * 2 ** ((m + c / 100 - 69) / 12)
        v = pluck(f)
        starts = [int(s * FS) for s in (0.05, 0.15, 0.3, 0.5, 0.7, 0.9)]
        res = run([v[s:s + N] for s in starts])
        got = [r / 65536 for r in res]
        err = [abs((g - m) * 100 - c) for g in got]
        ok = all(r >= 0 and round(g) == m for r, g in zip(res, got)) and max(err[:4]) <= 5 and np.median(err) <= 3
        check(ok, f"{name} {c:+d} cents ({f:.2f} Hz): " + " ".join(
            f"{round(g)}{(g - round(g)) * 100:+.1f}" if r >= 0 else "-" for r, g in zip(res, got)))
    res = run([np.zeros(N), RNG.normal(0, 30, N)])
    check(res == [-1, -1], f"silence and -60 dBFS noise: unpitched ({res})")
    res = run([RNG.normal(0, 3000, N) for _ in range(20)])
    check(sum(r >= 0 for r in res) <= 2, f"white noise at -21 dBFS: {sum(r >= 0 for r in res)} of 20 windows pitched")
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
