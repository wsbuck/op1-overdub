#!/usr/bin/env python3
"""Simulator checks for the VA engine (src/engine/va.c) before flashing.

  python3 src/engine/test/va_check.py [--keep DIR]

Builds va.c + tramp.S with engine_main.c, runs every case in one Docker container under bfin-elf-run,
then checks pitch, wave shapes (sine, saw, square, pulse, the sine → saw morph), detune, the sub, noise,
aliasing, levels at extreme settings, the output guard and CYCLES per render call. Exits non-zero if a check fails.
--keep DIR keeps the raw sample files (one per case) for listening or plotting.
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np

from simrun import CPU_BUDGET, alias_ratio, db, level_at, load, run_cases, spectrum

T = 10923   # va.c THIRD: sine | saw at T | square at 2T | pulse at 32767

# name: (wave, detune, noise, sub, f_start, calls, f_end)
CASES = {
    "sine440":    (0, 0, 0, 0, 440, 345, None),
    "morph440":   (T // 2, 0, 0, 0, 440, 345, None),
    "saw440":     (T, 0, 0, 0, 440, 345, None),
    "square440":  (2 * T, 0, 0, 0, 440, 345, None),
    "pulse440":   (32767, 0, 0, 0, 440, 345, None),
    "detune440":  (T, 32767, 0, 0, 440, 690, None),
    "sub440":     (0, 0, 0, 32767, 440, 345, None),
    "noise440":   (0, 0, 32767, 0, 440, 345, None),
    "saw_c7":     (T, 0, 0, 0, 2093, 345, None),
    "saw_c8":     (T, 0, 0, 0, 4186, 345, None),
    "sub_c8":     (2 * T, 0, 0, 32767, 4186, 345, None),
    "glide_all":  (32767, 32767, 32767, 32767, 30, 690, 12000),
    "low_note":   (2 * T, 0, 0, 32767, 8, 345, None),
}
# Levels: every wave corner with noise and sub off and full, at three pitches.
for w in (0, T, 2 * T, 32767):
    for n in (0, 32767):
        for s in (0, 32767):
            for f in (55, 880, 7040):
                CASES[f"lvl_w{w}_n{n}_s{s}_f{f}"] = (w, 0, n, s, f, 60, None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    workdir = tempfile.mkdtemp(prefix="va_check_")
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

    # Shapes.
    fr, m = spectrum(data["sine440"])
    h = [level_at(fr, m, 440 * n) for n in range(1, 6)]
    check(abs(fr[np.argmax(m)] - 440) < 3 and max(db(v, h[0]) for v in h[1:]) < -40,
          "sine440: pure, harmonics " + " ".join(f"{db(v, h[0]):.1f}" for v in h[1:]))
    sine_h1 = h[0]
    fr, m = spectrum(data["saw440"])
    h = [level_at(fr, m, 440 * n) for n in range(1, 11)]
    check(abs(fr[np.argmax(m)] - 440) < 3, f"saw440 fundamental {fr[np.argmax(m)]:.1f} Hz")
    check(all(abs(db(h[n - 1], h[0]) + 20 * np.log10(n)) < 2.5 for n in range(2, 8)),
          "saw440 harmonics fall as 1/n: " + " ".join(f"{db(v, h[0]):.1f}" for v in h[1:7]))
    saw_h1 = h[0]
    check(abs(db(saw_h1, sine_h1) - db(2 / np.pi, 1)) < 1,
          f"saw fundamental {db(saw_h1, sine_h1):.1f} dB re sine (expect {db(2 / np.pi, 1):.1f}: same peak level)")
    fr, m = spectrum(data["morph440"])
    mid = level_at(fr, m, 440)
    check(mid > saw_h1 * 0.95, f"sine → saw halfway: fundamental {db(mid, sine_h1):.1f} dB re sine (in phase, no dip)")
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

    # Sub: a square an octave down, as loud as the oscillators; nothing lower.
    fr, m = spectrum(data["sub440"])
    s1, s3, osc, low = (level_at(fr, m, f) for f in (220, 660, 440, 110))
    check(db(s1, osc) > 0 and abs(db(s3, s1) + 9.5) < 1.5 and db(low, s1) < -40,
          f"sub440: 220 Hz {db(s1, osc):+.1f} dB re osc, its H3 {db(s3, s1):.1f} dB (expect -9.5), 110 Hz {db(low, s1):.1f} dB")
    fr, m = spectrum(data["sub_c8"])
    check(db(level_at(fr, m, 2093), level_at(fr, m, 4186)) > -3, "sub at C8: 2093 Hz present")

    # Noise: broadband, centred.
    x = data["noise440"][len(data["noise440"]) // 4:].astype(float)
    fr, m = spectrum(data["noise440"])
    hi = np.sqrt(np.sum(m[(fr > 5000) & (fr < 15000)] ** 2) / np.sum(m ** 2))
    check(20 * np.log10(hi) > -10 and abs(x.mean()) < 100,
          f"noise: 5-15 kHz holds {20 * np.log10(hi):.1f} dB of the energy, mean {x.mean():.0f}")

    for name, f0 in (("saw_c7", 2093), ("saw_c8", 4186)):
        ratio = alias_ratio(data[name], f0)
        check(ratio < -25, f"{name}: aliasing {ratio:.1f} dB re harmonics")

    # Levels: never at the rails, never silent. Only the 10% pulse (peak 1.8) reaches into the knee.
    peaks = {n: int(np.abs(x[len(x) // 4:]).max()) for n, x in data.items()}
    bad = [f"{n} ({p})" for n, p in peaks.items() if p > 30000 or p < 3000 or ("w32767" not in n and "pulse" not in
           n and "glide" not in n and p > 26000)]
    check(not bad, f"levels: {len(data)} cases peak within 3000..26000 (pulse 30000)" +
          ("" if not bad else ": " + ", ".join(bad[:6])))
    loud = max(peaks, key=peaks.get)
    print(f"INFO peak {peaks[loud]} in {loud}; sine440 {peaks['sine440']}, saw440 {peaks['saw440']}")

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
