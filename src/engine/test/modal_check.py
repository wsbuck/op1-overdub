#!/usr/bin/env python3
"""Simulator checks for the modal engine (src/engine/modal.c) before flashing.

  python3 src/engine/test/modal_check.py [--keep DIR]

Checks mode frequencies (String = harmonic, Bar = ((2n+1)/3)^2), ring time against krystal's
T60 = 0.05 * 400^ring, that every exciter sounds (bow and breath sustain, mallet and pluck decay), stability
and range over materials x exciters x ring x brightness x pitch, the output guard and CYCLES per call.
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np

from simrun import CPU_BUDGET, FS, level_at, load, run_cases, spectrum

MAT = [round(i * 32767 / 7) for i in range(8)]          # knob value at each material
EXC = [round((i + 0.5) * 32767 / 5) for i in range(5)]  # centre of each exciter zone
NAMES_M = ["string", "bar", "bell", "gong", "membrane", "glass", "plate", "impossible"]
NAMES_E = ["mallet", "pluck", "bow", "breath", "rain"]
RING_INF = 32000                                        # > 95%: infinite


def ring_knob(t60):
    return round(np.log(t60 / 0.05) / np.log(400) * 32767)


# name: (material, exciter, ring, bright, f_start, calls, f_end)
CASES = {
    "string220": (MAT[0], EXC[0], ring_knob(1.5), 16384, 220, 345, None),
    "bar220": (MAT[1], EXC[0], ring_knob(1.5), 16384, 220, 345, None),
    "decay_0.3": (MAT[0], EXC[0], ring_knob(0.3), 16384, 220, 345, None),
    "decay_1.0": (MAT[0], EXC[0], ring_knob(1.0), 16384, 220, 690, None),
}
for e in range(5):
    CASES[f"exc_{NAMES_E[e]}"] = (MAT[2], EXC[e], ring_knob(1.0), 16384, 330, 690, None)
for m in range(8):
    for e in range(5):
        for r in (0, RING_INF):
            for b in (0, 32767):
                for f in (55, 1760):
                    CASES[f"range_m{m}_e{e}_r{r}_b{b}_f{f}"] = (MAT[m], EXC[e], r, b, f, 60, None)


def env_db(x, win=441):
    x = x.astype(float)
    n = len(x) // win
    return 20 * np.log10(np.sqrt((x[: n * win].reshape(n, win) ** 2).mean(axis=1)) + 1e-9)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    workdir = tempfile.mkdtemp(prefix="modal_check_")
    run_cases("modal", CASES, workdir)
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

    # Mode frequencies: strongest peak near each expected mode.
    for name, ratios in (("string220", [1, 2, 3, 4]), ("bar220", [((2 * n + 1) / 3) ** 2 for n in (1, 2, 3)])):
        fr, m = spectrum(data[name])
        found = []
        for r in ratios:
            f = 220 * r
            sel = np.abs(fr - f) < f * 0.03
            found.append(fr[sel][np.argmax(m[sel])] if sel.any() else 0)
        cents = max(abs(1200 * np.log2(max(f, 1) / (220 * r))) for f, r in zip(found, ratios))
        check(cents < 10, f"{name}: modes at " + ", ".join(f"{f:.1f}" for f in found) + f" Hz (worst {cents:.1f} cents)")

    # Ring time: level drop of the output envelope vs T60 (60 dB) from the peak.
    for name, t60 in (("decay_0.3", 0.3), ("decay_1.0", 1.0)):
        e = env_db(data[name])
        i0 = int(np.argmax(e))
        drop_rate = None
        tail = e[i0:]
        idx = np.where(tail < tail[0] - 30)[0]
        if len(idx):
            drop_rate = 30 / (idx[0] * 441 / FS)                  # dB per second
        measured = 60 / drop_rate if drop_rate else float("inf")
        check(drop_rate is not None and abs(measured / t60 - 1) < 0.35,
              f"{name}: ring T60 {measured:.2f} s (expect ~{t60} s for the fundamental; higher modes decay faster)")

    # Exciters: all sound; bow/breath sustain to the end, mallet/pluck decay.
    for e in range(5):
        x = data[f"exc_{NAMES_E[e]}"]
        env = env_db(x)
        peak, end = env.max(), env[-5:].mean()
        sustained = end > peak - 20
        expect_sustain = NAMES_E[e] in ("bow", "breath", "rain")
        check(peak > 20 * np.log10(500) and sustained == expect_sustain,
              f"exciter {NAMES_E[e]}: peak {peak:.1f} dB, end {end:.1f} dB ({'sustains' if sustained else 'decays'})")

    # Instability, not loudness: krystal's tanh saturates hot low notes by design. Unstable = the last 1280
    # samples are loud and flip sign almost every sample (a Nyquist-rate oscillation), or a decaying
    # exciter's tail is louder than its start.
    bad, sat = [], []
    for n, x in data.items():
        tail = x[-1280:].astype(float)
        flips = np.mean(np.sign(tail[1:]) != np.sign(tail[:-1]))
        if np.sqrt((tail ** 2).mean()) > 1000 and flips > 0.8:
            bad.append(n)
        if n.startswith("range") and "_e0_" in n or "_e1_" in n:
            head = x[:1280].astype(float)
            if np.sqrt((tail ** 2).mean()) > 2 * np.sqrt((head ** 2).mean()) + 100:
                bad.append(n)
        if np.mean(np.abs(tail) >= 32700) > 0.2:
            sat.append(n)
    silent = [n for n, x in data.items() if n.startswith("range") and np.abs(x).max() < 50]
    check(not bad, f"stability: {len(data)} cases, no Nyquist oscillation or growing decay" + (f": {bad[:5]}" if bad else ""))
    if sat:
        print(f"INFO {len(sat)} cases saturate the soft clipper at the end (hot low notes), e.g. {sat[:4]}")
    if silent:
        print(f"INFO {len(silent)} quiet range cases (expected for some exciter/pitch combinations), e.g. {silent[:4]}")
    peaks = {n: int(np.abs(x).max()) for n, x in data.items()}
    loud = max(peaks, key=peaks.get)
    print(f"INFO peak {peaks[loud]} in {loud}; string220 peak {peaks['string220']}, exc peaks "
          + ", ".join(f"{n} {peaks['exc_' + n]}" for n in NAMES_E))
    # Sustained load against the design target; the one-block peak (first strike after an exciter or
    # brightness change measures its burst) against a ceiling well under the ~1250/sample overload point.
    mean_worst = max(means)
    check(mean_worst <= CPU_BUDGET, f"cycles per render call, worst case mean {mean_worst:.0f} "
          f"({mean_worst / 128:.0f}/sample, target {CPU_BUDGET // 128})")
    check(worst <= 1000 * 128, f"cycles per render call, single-block peak {worst} ({worst / 128:.0f}/sample, ceiling 1000)")

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
