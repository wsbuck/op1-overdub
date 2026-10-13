#!/usr/bin/env python3
"""Simulator checks for the granular engine (src/engine/grain.c) before flashing.

  python3 src/engine/test/grain_check.py [--keep DIR]

Checks pitch tracking (sources are rooted at C4), the sampler source (a fake sampler slot in the driver),
that every source region sounds, that loudness stays
within a few dB across density, that high spray adds octave-up grains, stability and range, the output
guard and CYCLES per render call.
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np

from simrun import CPU_BUDGET, FS, level_at, load, run_cases, spectrum

NAMES = ["choir", "strings", "bells", "glass", "air", "plucks"]
SRC = [round((i + 0.5) / 6 * 32767) for i in range(6)]       # position knob at the middle of each source
BIG, MID, LOW = 32767, 16384, 0


def rms_db(x):
    x = x[len(x) // 4:].astype(float)
    return 20 * np.log10(np.sqrt((x ** 2).mean()) + 1e-9)


# name: (position, size, density, spray, f_start, calls, f_end)
CASES = {
    # Pitch on non-overlapping grains (500 ms, 2/s): overlapping copies of the same spot (spray 0) comb-filter
    # the spectrum and move its peak by ~45 cents, which is the granular sound, not a tuning error.
    "pitch_c4": (SRC[1], BIG, LOW, 0, 261.63, 690, None),
    "pitch_g4": (SRC[1], BIG, LOW, 0, 392.0, 690, None),
    "dense": (SRC[1], 26000, BIG, 0, 261.63, 690, None),
    "medium": (SRC[1], 26000, 20000, 0, 261.63, 690, None),
    "shimmer": (SRC[3], 26000, 26000, BIG, 261.63, 1380, None),
    "no_shimmer": (SRC[3], 26000, 26000, 16000, 261.63, 1380, None),
}
# Sampler source: a fake sampler slot holding a 440 Hz sine; C4 key plays it unshifted (rate 1).
CASES["sampler_c4"] = (MID, BIG, LOW, 0, 261.63, 690, None, "sampler")
CASES["sampler_c5"] = (MID, BIG, LOW, 0, 523.25, 690, None, "sampler")
CASES["sampler_gain2"] = (MID, BIG, LOW, 0, 261.63, 690, None, "sampler:16384")   # (16384/8192)^2 = x4
for i, n in enumerate(NAMES):                          # three positions across each source (sounds swell)
    for j, frac in enumerate((0.25, 0.5, 0.75)):
        CASES[f"src_{n}_{j}"] = (round((i + frac) / 6 * 32767), 26000, 24000, 8000, 261.63, 345, None)
for p in (0, MID, BIG):
    for sz in (LOW, BIG):
        for d in (LOW, BIG):
            for sp in (LOW, BIG):
                for f in (55, 880, 4186):
                    CASES[f"range_p{p}_s{sz}_d{d}_sp{sp}_f{f}"] = (p, sz, d, sp, f, 120, None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    workdir = tempfile.mkdtemp(prefix="grain_check_")
    run_cases("grain", CASES, workdir)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    data, worst, means = {}, 0, []
    for name in CASES:
        x, cyc, guard = load(workdir, name)
        data[name] = x
        worst = max(worst, cyc)
        means.append(float(open(os.path.join(workdir, name + ".err")).read().split()[4]))
        if guard != "5a5a5a5a":
            check(False, f"{name}: output guard overwritten ({guard})")

    for name, f0 in (("pitch_c4", 261.63), ("pitch_g4", 392.0)):
        fr, m = spectrum(data[name])
        sel = (fr > f0 * 0.8) & (fr < f0 * 1.2)
        peak = fr[sel][np.argmax(m[sel])]
        cents = 1200 * np.log2(peak / f0)
        check(abs(cents) < 15, f"{name}: fundamental {peak:.1f} Hz ({cents:+.1f} cents)")

    for name, f in (("sampler_c4", 440.0), ("sampler_c5", 880.0)):
        fr, m = spectrum(data[name])
        sel = (fr > f * 0.8) & (fr < f * 1.2)
        peak = fr[sel][np.argmax(m[sel])]
        check(abs(1200 * np.log2(peak / f)) < 15 and rms_db(data[name]) > 55,
              f"{name}: sampler source plays its 440 Hz sine at {peak:.1f} Hz (expect {f:.0f}), {rms_db(data[name]):.1f} dB")
    d = rms_db(data["sampler_gain2"]) - rms_db(data["sampler_c4"])
    check(abs(d - 12.04) < 1.0, f"sampler gain knob carries over: 16384 vs 8192 = {d:+.1f} dB (expect +12.0)")
    for n in NAMES:
        r = max(rms_db(data[f"src_{n}_{j}"]) for j in range(3))
        check(r > 55, f"source {n}: level {r:.1f} dB (loudest of 3 positions)")

    d = rms_db(data["dense"]) - rms_db(data["medium"])
    check(abs(d) < 4, f"loudness dense vs medium density: {d:+.1f} dB (1/sqrt(overlap) normalisation)")

    fr, m = spectrum(data["shimmer"])
    fr0, m0 = spectrum(data["no_shimmer"])
    # glass has partials 1, 2.76, 5.4, ... and no 2nd harmonic: octave-up grains put energy at 2 f0
    oct_gain = 20 * np.log10(level_at(fr, m, 2 * 261.63, 10) / max(level_at(fr0, m0, 2 * 261.63, 10), 1e-9))
    check(oct_gain > 3, f"spray max adds octave-up grains (2 f0 on glass: {oct_gain:+.1f} dB vs spray 50 %)")

    bad = [n for n, x in data.items() if np.mean(np.abs(x[-1280:]) >= 32700) > 0.05]
    check(not bad, f"range: {len(data)} cases, none saturating" + (f": {bad[:4]}" if bad else ""))
    jumps = []
    for n, x in data.items():
        if n.endswith("_f4186"):
            continue                                    # grains pitched ~16x up: steep but real content
        dx = np.abs(np.diff(x.astype(int)))
        if dx.max() > 20000:
            jumps.append((n, int(dx.max())))
    check(not jumps, "no sample-to-sample jumps > 20000 up to 880 Hz (clicks)" + (f": {jumps[:4]}" if jumps else ""))
    peaks = {n: int(np.abs(x).max()) for n, x in data.items()}
    loud = max(peaks, key=peaks.get)
    print(f"INFO peak {peaks[loud]} in {loud}")
    mean_worst = max(means)
    check(mean_worst <= CPU_BUDGET, f"cycles per call, worst mean {mean_worst:.0f} ({mean_worst / 128:.0f}/sample, target {CPU_BUDGET // 128})")
    check(worst <= 1000 * 128, f"cycles per call, peak {worst} ({worst / 128:.0f}/sample, ceiling 1000)")

    if a.keep:
        os.makedirs(a.keep, exist_ok=True)
        for f in os.listdir(workdir):
            if f.endswith((".txt", ".err")):
                shutil.copy(os.path.join(workdir, f), a.keep)
    shutil.rmtree(workdir)
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
