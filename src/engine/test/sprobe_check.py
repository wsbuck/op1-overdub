#!/usr/bin/env python3
"""Simulator checks for the knob probe engine (src/engine/sprobe.c) before flashing.

  python3 src/engine/test/sprobe_check.py

Builds sprobe.c + tramp.S with engine_main.c and runs each case under bfin-elf-run. Checks: a pure sine at the key's
pitch with FM off; blue pitch -12/+12 semitones; green level; white FM adds sidebands at the orange ratio; SHIFT+blue
(knob 4) adds the octave; the hook's packed knobs 6..7 (negative, raw 0x8000..0xFFFF) and negative knobs 0..5 don't
change the sound; every call records params[0..7] and counts renders in sprobe_ram; output guard, cycles per render
call; then the trampoline ABI check (abi_main.c) with the engine linked in.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

from simrun import CPU_BUDGET, ENGINE, FS, level_at, load, run_cases, spectrum

CALLS = 60
F = 440
CASES = {
    # name: (blue, green, white, orange, f, calls, f_end, [k4:p4,p5,p6,p7])
    "sine":      (16384, 16000, 0, 0, F, CALLS, None),
    "up12":      (32767, 16000, 0, 0, F, CALLS, None),
    "down12":    (0, 16000, 0, 0, F, CALLS, None),
    "quiet":     (16384, 4000, 0, 0, F, CALLS, None),
    "fm1":       (16384, 16000, 16000, 0, F, CALLS, None),
    "fm3":       (16384, 16000, 16000, 8192,  F, CALLS, None),
    "octave":    (16384, 16000, 0, 0, F, CALLS, None, "k4:24000,0,0,0"),
    "packed":    (16384, 16000, 0, 0, F, CALLS, None, "k4:0,0,-32467,-158"),
    "packed_k5": (16384, 16000, 0, 0, F, CALLS, None, "k4:0,31000,-1,-32768"),
    "negative":  (-5, -32768, -1, -100, F, CALLS, None, "k4:-7,-32768,0,0"),
    "worst":     (32767, 32767, 32767, 32767, 4000, CALLS, None, "k4:32767,32767,-32768,-32768"),
}


def peak_freq(x):
    """Dominant frequency (Hz): FFT peak with a quadratic fit."""
    x = x[len(x) // 4:].astype(float) * np.hanning(len(x) - len(x) // 4)
    n = 1 << 17
    m = np.abs(np.fft.rfft(x, n))
    i = int(np.argmax(m[1:])) + 1
    a, b, c = m[i - 1], m[i], m[i + 1]
    return (i + 0.5 * (a - c) / (a - 2 * b + c)) * FS / n


def main():
    workdir = tempfile.mkdtemp(prefix="sprobe_check_")
    run_cases("sprobe", CASES, workdir)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    res, worst = {}, 0
    for name, c in CASES.items():
        x, cyc, guard = load(workdir, name)
        err = open(os.path.join(workdir, name + ".err")).read().split()
        sp = [int(v) for v in err[err.index("sprobe") + 1:]]
        res[name] = (x, sp)
        worst = max(worst, cyc)
        check(guard == "5a5a5a5a" and len(x) == 128 * c[5], f"{name}: output guard {guard}, {len(x)} samples")
        k = [c[0], c[1], c[2], c[3]] + ([int(v) for v in c[7][3:].split(",")] if len(c) > 7 else [0, 0, 0, 0])
        check(sp == [c[5]] + k, f"{name}: sprobe_ram counts {sp[0]} renders and holds params {sp[1:]}")

    x = res["sine"][0]
    fr, m = spectrum(x)
    ref = level_at(fr, m, F)
    h = max(level_at(fr, m, n * F) for n in range(2, 8))
    check(abs(peak_freq(x) - F) < 3 and 20 * np.log10(h / ref) < -60,
          f"sine: {peak_freq(x):.1f} Hz, harmonics {20 * np.log10(h / ref):.0f} dB")
    check(abs(peak_freq(res["up12"][0]) - 2 * F) < 4 and abs(peak_freq(res["down12"][0]) - F / 2) < 3,
          f"blue: +12 {peak_freq(res['up12'][0]):.1f} Hz, -12 {peak_freq(res['down12'][0]):.1f} Hz")
    a, b = np.abs(res["sine"][0]).max(), np.abs(res["quiet"][0]).max()
    check(abs(a - 16000) < 300 and abs(b / a - 0.25) < 0.02, f"green level: peak {a} at 16000, ratio {b / a:.3f} at 4000")

    def side(name, f):
        fr, m = spectrum(res[name][0])
        return 20 * np.log10(max(level_at(fr, m, f), 1e-9) / level_at(fr, m, F))
    check(side("fm1", 2 * F) > -30 and side("sine", 2 * F) < -60,
          f"white FM index: sideband at 2f {side('fm1', 2 * F):.0f} dB (off: {side('sine', 2 * F):.0f} dB)")
    check(side("fm3", 3 * F) > -30 and side("fm3", 2 * F) < -50,
          f"orange ratio 2: sidebands at 3f {side('fm3', 3 * F):.0f} dB, none at 2f ({side('fm3', 2 * F):.0f} dB)")
    check(side("octave", 2 * F) > -6, f"SHIFT+blue (knob 4): octave at {side('octave', 2 * F):.1f} dB")
    same = all(np.array_equal(res[n][0], res["sine"][0]) for n in ("packed",)) and \
        np.array_equal(res["packed_k5"][0], res["sine"][0])
    check(same, "knobs 5..7 (the hook's packed values, any bit pattern) don't change the sound")
    check(not np.abs(res["negative"][0]).any(), "negative knobs 0..5 read as 0 (level 0: silence, no fault)")
    check(worst <= CPU_BUDGET, f"cycles per render call: max {worst} (budget {CPU_BUDGET}) = {worst // 128} per sample")

    r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{ENGINE}:/src:ro", "op1-bfin", "sh",
                        "-c", "set -e; cd /tmp; bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin "
                        "-Wall -Wextra -Werror -c -o engine.o /src/sprobe.c; bfin-elf-as -o tramp.o /src/tramp.S; "
                        "bfin-elf-gcc -std=gnu99 -msim -O1 -o abi /src/test/abi_main.c /src/test/abi_check.S tramp.o "
                        "engine.o; bfin-elf-run abi"], capture_output=True, text=True)
    check(r.returncode == 0 and "abi mask 00000000" in r.stdout, f"ABI: {r.stdout.strip() or r.stderr.strip()[-300:]}")

    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
