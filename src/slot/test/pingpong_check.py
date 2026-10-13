#!/usr/bin/env python3
"""Simulator checks for the ping-pong delay FX (src/slot/pingpong.c) before flashing.

  python3 src/slot/test/pingpong_check.py [--keep DIR]

Runs every case on the Blackfin simulator (fxsim.py; the line buffer, test/pingpong_ram.c, starts full of garbage)
and checks: dry at mix 0, echo times and the L/R alternation (click input), echo levels, decay at max feedback with
every tone, silence until the first echo (no stale buffer), clicks on time-knob jumps, range (no saturation) and
CYCLES per 64-frame call.
"""
import argparse
import sys

import numpy as np

from fxsim import FS, rms_db, run_cases, sine_in

BIG = 32767
CLICK = 12000                                       # fx_main.c: FREQ_MHZ 0 = one sample of CLICK at frame 0
DEFAULT = (22000, 14000, 12000, 10000)              # ~330 ms, g 0.4, a little dark, 30 % wet
BUDGET = 25000                                      # cycles per stereo call (64 frames); voices use ~460 k
EXP2 = [0, 2902, 5932, 9096, 12400, 15850, 19454, 23216, 27146, 31249, 35534, 40009, 44682, 49562, 54658, 59979,
        65536]


def time_q15(k):
    """pingpong.c time_q15: the delay for blue = k, Q15 samples."""
    e = k * 6
    o, f = e >> 15, e & 32767
    i, t = f >> 11, f & 2047
    m = EXP2[i] + (((EXP2[i + 1] - EXP2[i]) * t) >> 11)
    return ((882 * (65536 + m)) >> 1) << o


def gain(k):
    return (k * 31130) >> 15


def delayed(x, d):
    """pingpong.c tap on a line holding x since t = 0 (0 before), bit exact."""
    n, f = d >> 15, d & 32767
    xp = np.concatenate([np.zeros(n + 1, dtype=np.int64), x])
    a, b = xp[1:1 + len(x)], xp[:len(x)]
    return a + (((b - a) * f) >> 15)


T_ECHO = 8000                                       # ~78 ms
T_MAX_BLOCKS = (time_q15(BIG) >> 15) // 64 + 40

# name: (mode, k0, k1, k2, k3, blocks, input Hz (0 = click)[, switch block, j0, j1, j2, j3])
CASES = {
    "dry_s": ("s", 22000, BIG, 0, 0, 40, 1000),
    "dry_m": ("m", 22000, BIG, BIG, 0, 40, 1000),
    "echo_s": ("s", T_ECHO, 16384, 16384, BIG, 220, 0),
    "echo_m": ("m", T_ECHO, 16384, 16384, BIG, 220, 0),
    "fresh_s": ("s", BIG, 0, 16384, BIG, T_MAX_BLOCKS, 1000),
    "fresh_m": ("m", BIG, 0, 16384, BIG, T_MAX_BLOCKS, 1000),
    "jump_up": ("s", 0, 0, 16384, BIG, 900, 200, 300, BIG, 0, 16384, BIG),
    "jump_down": ("s", BIG, 0, 16384, BIG, 1400, 200, 1000, 0, 0, 16384, BIG),
    "jump_fb": ("s", 0, 16384, 16384, BIG, 900, 200, 300, 20000, 16384, 16384, BIG),
}
for name, k2 in (("flat", 16384), ("dark", 0), ("bright", BIG)):
    CASES[f"decay_{name}"] = ("s", 0, BIG, k2, BIG, 1500, 0)
CASES["decay_m"] = ("m", 0, BIG, 16384, BIG, 1500, 0)
for f in (200, 1000, 5000):
    CASES[f"level_{f}"] = ("s",) + DEFAULT + (700, f)
for k in range(16):
    ks = tuple(BIG if k >> b & 1 else 0 for b in range(4))
    for f in (100, 5000):
        CASES[f"range_{k:x}_{f}"] = ("s",) + ks + (1000, f)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    out, cyc = run_cases("pingpong", CASES, keep=a.keep)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    n = len(out["dry_s"])
    ok = (np.array_equal(out["dry_s"][:, 0], sine_in(n, 1000)) and np.array_equal(out["dry_s"][:, 1],
          sine_in(n, 1000, True)) and np.array_equal(out["dry_m"][:, 0], sine_in(n, 1000)))
    check(ok, "mix 0: output is the dry input, bit exact (stereo and mono)")

    # Click in, wet only, feedback g ~ 0.475, tone flat: echo k at k * d, on L for odd k, R for even (stereo).
    d = time_q15(T_ECHO) / 32768
    g = gain(16384) / 32768
    for mode in ("s", "m"):
        x = out[f"echo_{mode}"]
        rest = x.copy()
        bad = []
        for k in range(1, 6):
            c = int(k * d)
            lo, hi = c - k - 1, c + k + 2
            ch = 0 if mode == "m" or k % 2 else 1
            got, want = x[lo:hi, ch].sum(), CLICK * g ** (k - 1)
            if abs(got - want) > 0.01 * want + 2 * k:
                bad.append(f"echo {k}: {got} (expect {want:.0f})")
            rest[lo:hi, ch] = 0
        stray = np.abs(rest[:int(5.5 * d)]).max()
        what = "L, R, L, R, L" if mode == "s" else "mono"
        check(not bad and stray == 0, f"{'stereo' if mode == 's' else 'mono'}: click echoes at k x {d:.1f} samples "
              f"({what}), level g^(k-1), nothing else (stray {stray})" + (f": {bad}" if bad else ""))

    # The line starts full of garbage: wet output must be 0 until the first echo, then exactly the delayed input.
    dq = time_q15(BIG)
    for mode in ("s", "m"):
        x = out[f"fresh_{mode}"]
        m = len(x)
        src = (sine_in(m, 1000) + sine_in(m, 1000, True)) >> 1 if mode == "s" else sine_in(m, 1000)
        ok = np.array_equal(x[:, 0], delayed(src, dq)) and (mode == "m" or not x[:, 1].any())
        first = int(np.argmax(x[:, 0] != 0))
        check(ok, f"{'stereo' if mode == 's' else 'mono'}, garbage buffer, time max: silent for {first} samples "
              f"({first / FS:.3f} s), then exactly the delayed input" + (", R silent" if mode == "s" else ""))

    for name in ("decay_flat", "decay_dark", "decay_bright", "decay_m"):
        x = np.abs(out[name]).max(axis=1)
        dn = time_q15(0) / 32768
        pk = [x[int(k * dn) - 30:int(k * dn) + 30].max() for k in range(1, int(len(x) / dn) - 1)]
        mono = all(b <= a for a, b in zip(pk, pk[1:]))
        check(mono and pk[-1] < 0.02 * pk[0], f"feedback max, {name}: {len(pk)} echoes decay monotonically, "
              f"{pk[0]} -> {pk[-1]} ({20 * np.log10(max(pk[-1], 1) / pk[0]):.0f} dB)")

    for name, sw in (("jump_up", 300), ("jump_down", 1000), ("jump_fb", 300)):
        j = out[name]
        tail = j[64 * (sw - 20):]
        slope = np.abs(np.diff(tail, axis=0)).max()
        dry_slope = np.abs(np.diff(sine_in(len(j), 200))).max()
        lim = 1.6 if name != "jump_fb" else 2.6
        check(slope < lim * dry_slope, f"{name}: time knob jump, wet: largest step {slope} (dry sine {dry_slope}, "
              f"limit {lim}x): tape glide, no click")

    for f in (200, 1000, 5000):
        x = out[f"level_{f}"]
        lv = rms_db(x[len(x) // 2:, 0]) - rms_db(sine_in(len(x), f))
        check(-8 < lv < 3, f"default knobs, {f} Hz: level {lv:+.1f} dB vs dry")

    sat = [k for k, x in out.items() if np.mean(np.abs(x) >= 32700) > 0.001]
    pk = max(np.abs(x).max() for k, x in out.items() if k.startswith("range"))
    check(not sat, f"range: 32 corner cases (+ all others), none saturating, peak {pk}" + (f": {sat[:4]}" if sat else ""))

    st = max(c[1] for k, c in cyc.items() if CASES[k][0] == "s")
    mo = max(c[1] for k, c in cyc.items() if CASES[k][0] == "m")
    peak = max(c[0] for c in cyc.values())
    check(st <= BUDGET, f"cycles per call: stereo mean {st} ({st / 64:.0f}/frame), mono mean {mo}, peak {peak}; "
          f"budget {BUDGET}")

    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
