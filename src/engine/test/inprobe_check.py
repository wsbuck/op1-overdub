#!/usr/bin/env python3
"""Simulator checks for the input probe engine (src/engine/inprobe.c) before flashing.

  python3 src/engine/test/inprobe_check.py

Builds inprobe.c + tramp.S with engine_main.c (which fills the input buffers with integer triangles before each
call) and runs every case under bfin-elf-run. Checks, bit exact against a model of inprobe.h: each source, channel
and shift, the 2x linear upsampling across blocks, the gain and knee; that the tone sounds, the output guard and
CYCLES per render call; then the trampoline ABI check (abi_main.c) with the engine linked in.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

from simrun import CPU_BUDGET, ENGINE, load, run_cases

CALLS = 20
GAIN_Q10 = [round(1024 * 10 ** (d / 20)) for d in range(25)]

# name: (blue gain, green source, white channel, orange shift, f_start, calls, f_end)
SIL, TONE, TE, RAW, OTHER, ALT = 0, 6000, 12000, 17000, 22000, 30000
LR, L, R = 0, 16384, 30000
CASES = {
    "silence":   (0, SIL, 0, 0, 440, CALLS, None),
    "tone":      (0, TONE, 0, 0, 440, CALLS, None),
    "te":        (0, TE, 0, 0, 440, CALLS, None),
    "te_24db":   (32767, TE, 0, 0, 440, CALLS, None),
    "te_12db":   (16384, TE, 0, 0, 440, CALLS, None),
    "raw_lr":    (0, RAW, LR, 0, 440, CALLS, None),
    "raw_l":     (0, RAW, L, 0, 440, CALLS, None),
    "raw_r":     (0, RAW, R, 0, 440, CALLS, None),
    "raw_l_sh8": (0, RAW, L, 32767, 440, CALLS, None),
    "other_l":   (0, OTHER, L, 0, 440, CALLS, None),
    "other_sh8": (0, OTHER, L, 32767, 440, CALLS, None),
    "alt_lr":    (0, ALT, LR, 0, 440, CALLS, None),
    "alt_l":     (0, ALT, L, 0, 440, CALLS, None),
    "worst":     (32767, RAW, LR, 32767, 440, CALLS, None),
}


def pos(k, n):
    return min((max(k, 0) * n) >> 15, n - 1)


def tri(n, p, a):
    t = n % p
    return -a + 4 * a * t // p if 2 * t < p else 3 * a - 4 * a * t // p


def sat16(v):
    return max(-32768, min(32767, v))


def knee(x):
    t = 24576
    ax = abs(x)
    if ax > t:
        d = ax - t
        ax = 32767 if d >= 2 * (32767 - t) else t + d - ((d * d) >> 15)
    return -ax if x < 0 else ax


def block_input(src, ch, sh, c):
    """inp_fetch() on the buffers engine_main.c fills for call c."""
    x = []
    for i in range(64):
        n = c * 64 + i
        if src == 2:
            x.append(tri(n, 44, 8000))
            continue
        if src in (3, 4, 5):
            if src == 3:
                l, r = tri(n, 88, 12000) << 16, tri(n, 30, 6000) << 16
            elif src == 4:
                l = r = 0x00123400
            else:
                l = tri(n, 60, 10000) << 16
                r = -l
            v = l >> (16 - sh) if ch == 1 else r >> (16 - sh) if ch == 2 else ((l >> 1) + (r >> 1)) >> (15 - sh)
            x.append(sat16(v))
            continue
        x.append(0)
    return x


def model(k0, k1, k2, k3, calls):
    g = GAIN_Q10[pos(k0, 25)]
    src, ch, sh = pos(k1, 6), pos(k2, 3), pos(k3, 9)
    out, prev = [], 0
    for c in range(calls):
        for v in block_input(src, ch, sh, c):
            out += [knee((((prev + v) >> 1) * g) >> 10), knee((v * g) >> 10)]
            prev = v
    return np.array(out)


def main():
    workdir = tempfile.mkdtemp(prefix="inprobe_check_")
    run_cases("inprobe", CASES, workdir)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    worst = 0
    for name, (k0, k1, k2, k3, _, calls, _) in CASES.items():
        x, cyc, guard = load(workdir, name)
        worst = max(worst, cyc)
        check(guard == "5a5a5a5a", f"{name}: output guard {guard}")
        check(len(x) == 128 * calls, f"{name}: {len(x)} samples")
        if name == "tone":
            peak = int(np.abs(x).max())
            check(15000 < peak <= 16384, f"tone: sounds, peak {peak} (half scale)")
            continue
        want = model(k0, k1, k2, k3, calls)
        bad = np.nonzero(x != want)[0]
        check(not len(bad), f"{name}: bit exact, peak {int(np.abs(x).max())}"
              + (f"; first mismatch at {bad[0]}: {x[bad[0]]} != {want[bad[0]]}" if len(bad) else ""))
    check(not np.abs(load(workdir, "silence")[0]).any(), "silence: all zero")
    check(np.abs(load(workdir, "te_24db")[0]).max() == 32767 and np.abs(load(workdir, "te")[0]).max() == 8000,
          "te: unity at 0 dB, clipped by the knee at +24 dB")
    check(worst <= CPU_BUDGET, f"cycles per render call: max {worst} (budget {CPU_BUDGET})")

    # ABI: the trampoline with this engine preserves every register it promises to (abi_check.S).
    r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{ENGINE}:/src:ro", "op1-bfin", "sh",
                        "-c", "set -e; cd /tmp; bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin "
                        "-Wall -Wextra -Werror -c -o engine.o /src/inprobe.c; bfin-elf-as -o tramp.o /src/tramp.S; "
                        "bfin-elf-gcc -std=gnu99 -msim -O1 -o abi /src/test/abi_main.c /src/test/abi_check.S tramp.o "
                        "engine.o; bfin-elf-run abi"], capture_output=True, text=True)
    check(r.returncode == 0 and "abi mask 00000000" in r.stdout, f"ABI: {r.stdout.strip() or r.stderr.strip()[-300:]}")

    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
