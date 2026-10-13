#!/usr/bin/env python3
"""Simulator checks for the VA engine (src/engine/va.c) before flashing.

  python3 src/engine/test/va_check.py [--keep DIR]

Builds va.c + tramp.S with engine_main.c, runs every case in one Docker container under bfin-elf-run,
then checks pitch, wave shapes, detune, filter response, aliasing, stability at extreme settings,
the output guard and CYCLES per render call. Exits non-zero if a check fails.
--keep DIR keeps the raw sample files (one per case) for listening or plotting.
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np

from simrun import CPU_BUDGET, FS, alias_ratio, db, knob_for_cutoff, level_at, load, run_cases, spectrum

# name: (wave, detune, cutoff, resonance, f_start, calls, f_end)
CASES = {
    "saw440":     (0, 0, 32767, 0, 440, 345, None),
    "square440":  (16384, 0, 32767, 0, 440, 345, None),
    "pulse440":   (32767, 0, 32767, 0, 440, 345, None),
    "detune440":  (0, 32767, 32767, 0, 440, 690, None),
    "saw_c7":     (0, 0, 32767, 0, 2093, 345, None),
    "saw_c8":     (0, 0, 32767, 0, 4186, 345, None),
    "lp_1k":      (0, 0, None, 0, 110, 345, None),          # cutoff knob for ~1 kHz, set below
    "reso_1k":    (0, 0, None, 32767, 110, 345, None),
    "glide_reso": (0, 32767, 16384, 32767, 30, 690, 12000),
    "low_note":   (0, 0, 32767, 32767, 8, 345, None),
}
# Every cutoff (17 steps) at max resonance and at zero, for three pitches and both extreme waves.
for c in range(0, 32768, 4096):
    for r in (0, 32767):
        for w in (0, 32767):
            for f in (55, 880, 7040):
                CASES[f"sweep_c{c}_r{r}_w{w}_f{f}"] = (w, 0, c, r, f, 60, None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    workdir = tempfile.mkdtemp(prefix="va_check_")
    for name, (w, d, c, r, f0, calls, f1) in CASES.items():
        if c is None:
            CASES[name] = (w, d, knob_for_cutoff(1000), r, f0, calls, f1)
    run_cases("va", CASES, workdir)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    worst_cycles, data = 0, {}
    for name in CASES:
        x, cyc, guard = load(workdir, name)
        data[name] = x
        worst_cycles = max(worst_cycles, cyc)
        if guard != "5a5a5a5a":
            check(False, f"{name}: output guard overwritten ({guard})")

    # Pitch and shapes.
    fr, m = spectrum(data["saw440"])
    peak = fr[np.argmax(m)]
    check(abs(peak - 440) < 3, f"saw440 fundamental {peak:.1f} Hz")
    h = [level_at(fr, m, 440 * n) for n in range(1, 11)]
    check(all(abs(db(h[n - 1], h[0]) + 20 * np.log10(n)) < 2.5 for n in range(2, 8)),
          "saw440 harmonics fall as 1/n: " + " ".join(f"{db(v, h[0]):.1f}" for v in h[1:7]))
    fr, m = spectrum(data["square440"])
    h = [level_at(fr, m, 440 * n) for n in range(1, 6)]
    check(db(h[1], h[0]) < -40 and db(h[3], h[0]) < -40 and abs(db(h[2], h[0]) + 9.5) < 1.5,
          f"square440: H2 {db(h[1], h[0]):.1f} dB, H3 {db(h[2], h[0]):.1f} dB (expect -9.5), H4 {db(h[3], h[0]):.1f} dB")
    fr, m = spectrum(data["pulse440"])
    h = [level_at(fr, m, 440 * n) for n in range(1, 12)]
    check(db(h[9], h[0]) < -30, f"pulse440 (10%): H10 {db(h[9], h[0]):.1f} dB (null expected)")
    fr, m = spectrum(data["detune440"])
    f2 = 440 * 2 ** (1 / 12)
    check(db(level_at(fr, m, f2, 3), level_at(fr, m, 440, 3)) > -3,
          f"detune max: second partial at {f2:.1f} Hz present")

    # Filter: -12 dB/oct above cutoff, resonance peak near cutoff.
    fr, m = spectrum(data["lp_1k"])
    ref = level_at(fr, m, 110)
    l4k, l8k = (db(level_at(fr, m, 110 * round(f / 110)), ref) + 20 * np.log10(round(f / 110)) for f in (4000, 8000))
    check(l8k - l4k < -9, f"lp 1 kHz: 4 kHz {l4k:.1f} dB, 8 kHz {l8k:.1f} dB re saw (slope {l8k - l4k:.1f} dB/oct)")
    fr, m = spectrum(data["reso_1k"])
    fr0, m0 = spectrum(data["lp_1k"])
    boost = db(level_at(fr, m, 990), level_at(fr0, m0, 990))
    check(boost > 10, f"resonance max boosts the harmonic at 990 Hz by {boost:.1f} dB")

    # Aliasing: energy away from harmonics, relative to total, for high notes.
    for name, f0 in (("saw_c7", 2093), ("saw_c8", 4186)):
        ratio = alias_ratio(data[name], f0)
        check(ratio < -25, f"{name}: aliasing {ratio:.1f} dB re harmonics")

    # Stability and range everywhere.
    bad = []
    for name, x in data.items():
        tail = x[-1280:]
        rail = np.mean(np.abs(tail) >= 32700)
        if rail > 0.2 or (name.startswith("sweep") and np.abs(tail).max() == 0 and "_c0_" not in name):
            bad.append(f"{name} (rail {rail:.0%}, peak {np.abs(tail).max()})")
    check(not bad, f"stability: {len(data)} cases, none stuck at the rails or silent" +
          ("" if not bad else ": " + ", ".join(bad[:5])))
    peaks = {n: int(np.abs(x).max()) for n, x in data.items()}
    loud = max(peaks, key=peaks.get)
    print(f"INFO peak {peaks[loud]} in {loud}; saw440 peak {peaks['saw440']}")

    check(worst_cycles <= CPU_BUDGET, f"cycles per render call: max {worst_cycles} (budget {CPU_BUDGET}, "
          f"{worst_cycles / 128:.0f} per sample)")

    if a.keep:
        os.makedirs(a.keep, exist_ok=True)
        for f in os.listdir(workdir):
            shutil.copy(os.path.join(workdir, f), a.keep)
    shutil.rmtree(workdir)
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
