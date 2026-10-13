#!/usr/bin/env python3
"""Simulator checks for the ensemble FX (src/slot/ensemble.c) before flashing.

  python3 src/slot/test/ensemble_check.py [--keep DIR]

Runs every case on the Blackfin simulator (fxsim.py) and checks: dry/wet mix, the base delay, LFO rate, R in anti-phase with L, the ensemble vibrato,
loudness, clicks on knob jumps, range (no saturation) and CYCLES per 64-frame call.
"""
import argparse
import sys

import numpy as np

from fxsim import FS, peak_hz, phase_dev, rms_db, run_cases, sine_in

BIG = 32767
DEFAULT = (13600, 16000, 0, 16384)                 # Juno I-ish: 0.5 Hz, half depth, chorus, 50/50
BUDGET = 25000                                      # cycles per stereo call (64 frames); voices use ~460 k


def rate_hz(k):
    return 0.05 * 2 ** (8 * k / 32768)


# name: (mode, k0, k1, k2, k3, blocks, input Hz[, switch block, j0, j1, j2, j3])
CASES = {
    "dry": ("m", 13600, BIG, BIG, 0, 40, 1000),
    "delay": ("m", 0, 0, 0, BIG, 40, 1000),
    "rate": ("m", 20000, BIG, 0, BIG, 2760, 1000),
    "spread": ("s", 20000, BIG, 0, BIG, 2760, 1000),
    "ensemble": ("m", 1000, 0, BIG, BIG, 2760, 1000),
    "jump": ("s", 0, 0, 0, 16384, 400, 200, 200, BIG, BIG, BIG, BIG),
}
for f in (200, 1000, 5000):
    CASES[f"level_{f}"] = ("s",) + DEFAULT + (700, f)
for k in range(16):
    ks = tuple(BIG if k >> b & 1 else 0 for b in range(4))
    for f in (100, 5000):
        CASES[f"range_{k:x}_{f}"] = ("s",) + ks + (200, f)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    out, cyc = run_cases("ensemble", CASES, keep=a.keep)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    n = len(out["dry"])
    check(np.array_equal(out["dry"][:, 0], sine_in(n, 1000)), "mix 0: output is the dry input, bit exact")
    d = out["delay"][:, 0]
    ref = sine_in(n, 1000)
    err = np.abs(d[200:] - ref[200 - 89:n - 89]).max()
    check(err <= 1, f"depth 0, wet: a pure 89-sample (2 ms) delay (max error {err})")

    pd = phase_dev(out["rate"][:, 0], 1000)
    got, want = peak_hz(pd, 0.3, 20), rate_hz(20000)
    check(abs(got / want - 1) < 0.05, f"LFO rate at blue 20000: {got:.2f} Hz (expect {want:.2f})")
    dev_ms = (pd.max() - pd.min()) / (2 * np.pi * 1000) * 1000 / 2
    check(2.4 < dev_ms < 3.3, f"depth max sweeps the delay by +-{dev_ms:.2f} ms (expect ~3)")

    pl, pr = phase_dev(out["spread"][:, 0], 1000), phase_dev(out["spread"][:, 1], 1000)
    c = np.corrcoef(pl, pr)[0, 1]
    check(c < -0.95, f"stereo: R's sweep is in anti-phase with L's (correlation {c:+.3f})")

    pe = phase_dev(out["ensemble"][:, 0], 1000)
    # three taps 120 degrees apart, summed: the 6.5 Hz wobble cancels, its 3rd harmonic remains
    got = peak_hz(pe, 2, 30)
    check(abs(got - 19.5) < 0.5, f"ensemble max: 6.5 Hz vibrato on 3 taps, summed wobble at 3x ({got:.2f} Hz, expect 19.5)")
    lv = rms_db(out["ensemble"][FS // 2:, 0]) - rms_db(sine_in(n, 1000))
    check(-8 < lv < 1, f"ensemble max (3 taps, wet): level {lv:+.1f} dB vs dry")

    for f in (200, 1000, 5000):
        x = out[f"level_{f}"]
        dry = sine_in(len(x), f)
        lv = rms_db(x[len(x) // 4:, 0]) - rms_db(dry)
        check(-4 < lv < 1, f"default knobs, {f} Hz: level {lv:+.1f} dB vs dry")

    j = out["jump"]
    slope = np.abs(np.diff(j[64 * 150:, :], axis=0)).max()
    dry_slope = np.abs(np.diff(sine_in(len(j), 200))).max()
    check(slope < 2.2 * dry_slope, f"all knobs jumping 0 -> max: largest step {slope} (dry sine {dry_slope}): no click")

    sat = [k for k, x in out.items() if k.startswith("range") and np.mean(np.abs(x) >= 32700) > 0.001]
    check(not sat, f"range: 32 corner cases, none saturating" + (f": {sat[:4]}" if sat else ""))

    st = max(c[1] for k, c in cyc.items() if CASES[k][0] == "s")
    mo = max(c[1] for k, c in cyc.items() if CASES[k][0] == "m")
    peak = max(c[0] for c in cyc.values())
    check(st <= BUDGET, f"cycles per call: stereo mean {st} ({st / 64:.0f}/frame), mono mean {mo}, peak {peak}; "
          f"budget {BUDGET}")

    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
