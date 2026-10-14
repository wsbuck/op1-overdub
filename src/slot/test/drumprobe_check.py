#!/usr/bin/env python3
"""Simulator checks for the drum probe (src/slot/drumprobe.c), a test drum engine, before flashing.

  python3 src/slot/test/drumprobe_check.py

Builds drumprobe.c with the device flags and drumprobe_main.c (TE's drum voice loop in miniature: slot init, note-on,
render, active, note-off, each through its C_ENTRY wrapper with fx_abi.S's register sentinels) and runs it under
bfin-elf-run. Checks: every key sounds, pitch rises with the key (E1 + key semitones, 88.2 kHz), noise only on odd
keys, velocity scales the level, the knobs (tune, decay, pitch drop, noise level) do what drumprobe.c says, the voice
ends (active() 0, then silence), the output guard, cycles per render call under the engines' CPU budget, the slot
init (defaults when the marker is missing, edits kept, range row 9 copied only when sane), and the ABI.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
ENGINE = os.path.join(os.path.dirname(SLOT), "engine")
sys.path.insert(0, os.path.join(ENGINE, "test"))
from simrun import CPU_BUDGET  # noqa: E402

FS = 88200
BLOCKS = 120                               # 174 ms
D = [0, 16384, 12288, 20000, 8192, 0, 0]   # drumprobe.c defaults (without the marker)
CASES = {f"key{k}": (k, 127, BLOCKS, D) for k in range(24)}
CASES.update({
    "vel64": (0, 64, BLOCKS, D),
    "tune_up": (0, 127, 400, [32767, 32767] + D[2:]),
    "tune_down": (0, 127, 400, [-32768, 32767] + D[2:]),
    "decay_short": (0, 127, 400, [0, 0] + D[2:]),
    "decay_long": (0, 127, 1100, [0, 32767] + D[2:]),
    "drop0": (0, 127, BLOCKS, [0, 16384, 0] + D[3:]),
    "drop_max": (0, 127, BLOCKS, [0, 16384, 32767] + D[3:]),
    "noise0": (1, 127, BLOCKS, [0, 16384, 12288, 0] + D[4:]),
    "worst": (23, 127, 8, [32767, 32767, 32767, 32767, 32767]),
})


def freq(x):
    """Dominant frequency of x (Hz), from the FFT peak with a quadratic fit."""
    x = x.astype(float) * np.hanning(len(x))
    n = 1 << 16
    m = np.abs(np.fft.rfft(x, n))
    i = int(np.argmax(m[1:])) + 1
    a, b, c = m[i - 1], m[i], m[i + 1]
    return (i + 0.5 * (a - c) / (a - 2 * b + c)) * FS / n


def hf(x):
    """Energy above 5 kHz relative to all, dB."""
    m = np.abs(np.fft.rfft(x.astype(float) * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / FS)
    return 10 * np.log10(max(m[f > 5000].sum(), 1e-9) / max(m.sum(), 1e-9))


def main():
    w = tempfile.mkdtemp(prefix="drumprobe_")
    cflags = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
    cmds = [f"bfin-elf-run /tmp/m {k} {v} {b} {' '.join(map(str, p))} > /w/{n}.txt 2> /w/{n}.err; echo $? > /w/{n}.rc"
            for n, (k, v, b, p) in CASES.items()]
    cmds.append("bfin-elf-run /tmp/m init > /w/init.txt 2> /w/init.err; echo $? > /w/init.rc")
    open(os.path.join(w, "cmds"), "w").write("\n".join(cmds) + "\n")
    open(os.path.join(w, "run.sh"), "w").write(
        "set -e; cd /tmp\n"
        f"bfin-elf-gcc {cflags} -I /engine -I /src -c -o dp.o /src/drumprobe.c\n"
        "bfin-elf-as -I /src --defsym NO_FX=1 -o abi.o /src/test/fx_abi.S\n"
        "bfin-elf-as -I /src -o entries.o /src/test/drumprobe_abi.S\n"
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -I /src -o m /src/test/drumprobe_main.c dp.o abi.o entries.o\n"
        "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v",
                    f"{ENGINE}:/engine:ro", "-v", f"{w}:/w", "op1-bfin", "sh", "/w/run.sh"], check=True)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    res = {}
    for n in CASES:
        rc = open(os.path.join(w, n + ".rc")).read().strip()
        err = open(os.path.join(w, n + ".err")).read()
        x = np.loadtxt(os.path.join(w, n + ".txt"), dtype=np.int64)
        f = err.split()
        res[n] = (x, int(f[2]), f[4], [int(a) for a in f[f.index("active") + 1:] if a.lstrip("-").isdigit()], rc, err)

    abi = all(r[4] == "0" for r in res.values())
    check(abi, f"ABI: {sum(4 + CASES[n][2] * 2 for n in CASES)} entry calls preserve R4-R7, P3-P5, FP, SP, loops, L"
          + ("" if abi else " (" + "; ".join(r[5].strip() for r in res.values() if r[4] != "0")[:300] + ")"))
    check(all(r[2] == "5a5a5a5a" for r in res.values()), "output guard intact (128 samples per call)")
    worst = max(r[1] for r in res.values())
    check(worst <= CPU_BUDGET, f"cycles per render call: max {worst} (budget {CPU_BUDGET})")
    check(all("vptr 0190f8a4" in r[5] for r in res.values()), "vptr never written")

    # keys: sound, pitch per key (late window: the pitch drop is over), noise on odd keys only
    fs_, peaks, hfs = [], [], []
    for k in range(24):
        x = res[f"key{k}"][0]
        peaks.append(int(np.abs(x).max()))
        fs_.append(freq(x[len(x) * 2 // 3:]))
        hfs.append(hf(x[: 128 * 10]))
    check(min(peaks) > 16000, f"every key sounds (peak min {min(peaks)})")
    want = [41.2034 * 2 ** (k / 12) for k in range(24)]
    err_c = max(abs(1200 * np.log2(f / t)) for f, t in zip(fs_, want))
    check(err_c < 25, f"pitch per key E1..D#3 at 88.2 kHz: worst {err_c:.1f} cents (key 0 {fs_[0]:.1f} Hz, "
          f"key 23 {fs_[23]:.1f} Hz)")
    odd, even = [hfs[k] for k in range(1, 24, 2)], [hfs[k] for k in range(0, 24, 2)]
    check(min(odd) > -25 and max(even) < -45, f"noise on odd keys only (HF odd min {min(odd):.0f} dB, even max "
          f"{max(even):.0f} dB)")

    # velocity
    p127, p64 = np.abs(res["key0"][0]).max(), np.abs(res["vel64"][0]).max()
    check(0.4 < p64 / p127 < 0.6, f"velocity 64 vs 127: level ratio {p64 / p127:.2f}")

    # knobs
    up, dn = freq(res["tune_up"][0][128 * 100:]), freq(res["tune_down"][0][128 * 100:])
    check(abs(1200 * np.log2(up / dn) - 2300) < 30, f"blue tune: +11 / -12 semitones, ratio {up / dn:.3f} (want 3.776)")

    def length(x):          # samples until the last one above -40 dBFS
        idx = np.nonzero(np.abs(x) > 328)[0]
        return int(idx[-1]) if len(idx) else 0
    ls_, ll = length(res["decay_short"][0]) / FS, length(res["decay_long"][0]) / FS
    check(0.01 < ls_ < 0.04 and 1.1 < ll < 1.5, f"green decay: -40 dB after {ls_ * 1000:.0f} ms (0) / {ll * 1000:.0f} ms (max)")
    def crossings(x):       # sign changes in the first 20 ms
        y = x[:1764]
        return int(np.count_nonzero(np.diff(np.signbit(y[y != 0]).astype(int))))
    c0, c8 = crossings(res["drop0"][0]), crossings(res["drop_max"][0])
    check(c8 > 2 * c0, f"white pitch drop: {c8} zero crossings in the first 20 ms with max drop vs {c0} without")
    check(hf(res["noise0"][0][:1280]) < -45 and hfs[1] > -25, "orange noise level: none at 0")

    # the voice ends: active() counts blocks while sounding, then 0 and silence
    a = res["decay_short"][3]
    end = a.index(0) if 0 in a else -1
    x = res["decay_short"][0]
    check(end > 0 and a[:end] == list(range(2, end + 2)) and all(v == 0 for v in a[end:])
          and not np.abs(x[128 * (end + 1):]).any(), f"voice ends: active() = age while sounding, 0 after block {end}, then silence")

    r = open(os.path.join(w, "init.txt")).read().split()
    check(open(os.path.join(w, "init.rc")).read().strip() == "0" and r == ["defaults", "1", "row", "1", "kept", "1", "norow", "1"],
          f"slot init: defaults when the marker is missing, edits kept, row 9 copied only when sane ({' '.join(r)})")

    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
