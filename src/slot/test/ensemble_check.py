#!/usr/bin/env python3
"""Simulator checks for the ensemble FX (src/slot/ensemble.c) before flashing.

  python3 src/slot/test/ensemble_check.py [--keep DIR]

Runs every case on the Blackfin simulator (fxsim.py) and checks: dry/wet mix, the delay per character and depth,
the LFO rate range, the wet low-pass response, levels, stereo phase (CE: R = dry - wet; DUAL: R swept in
anti-phase), the three-tap ensemble, clicks and zipper noise while knobs move, DC, range (no saturation) and CYCLES
per 64-frame call.
"""
import argparse
import sys

import numpy as np

from fxsim import FS, peak_hz, phase_dev, rms_db, run_cases, sine_in

BIG = 32767
DEFAULT = (15360, 21504, 0, 16384)                 # CE: 0.78 Hz, +-1.97 ms, 50/50 (fx.json)
CE, DUAL = 0, 16384                                 # white plateaus
BUDGET = 22000                                      # cycles per stereo call (64 frames); the old ensemble 21 k
CE_D, DUAL_D, ENS_D = 265.0, 4797235 / 65536, 89.0  # centre delays at depth 0, samples (ensemble.c)
LP = ((3108, -4944, 992), (4288, -6821, 7589))      # ensemble.c LP1 / LP2 (Q14)


def rate_hz(k):
    return 0.1 * 2 ** (((k * 25891) >> 15) / 4096)


def lp_resp(f):
    z = np.exp(-2j * np.pi * np.asarray(f, float) / FS)
    h = 1
    for b, a1, a2 in LP:
        h = h * b * (1 + 2 * z + z * z) / (16384 + a1 * z + a2 * z * z)
    return h


def lp_delay():
    """Group delay of the wet low-pass at low frequencies (samples)."""
    f = np.array([50.0, 60.0])
    ph = np.unwrap(np.angle(lp_resp(f)))
    return -(ph[1] - ph[0]) / (2 * np.pi * (f[1] - f[0]) / FS)


def group_delay(ir):
    """Low-frequency group delay of an impulse response (samples): phase slope 20 .. 400 Hz."""
    n = 1 << 15
    s = np.fft.rfft(ir.astype(float), n)
    fr = np.fft.rfftfreq(n, 1 / FS)
    sel = (fr > 20) & (fr < 400)
    ph = np.unwrap(np.angle(s[sel]))
    return -np.polyfit(2 * np.pi * fr[sel] / FS, ph, 1)[0]


def lfo_hz(pd):
    """LFO rate from a triangle-swept delay: the phase's slope flips sign at each LFO extreme."""
    w = 441
    sl = np.convolve(np.diff(pd), np.ones(w) / w, mode="valid")
    mid = (sl.max() + sl.min()) / 2
    s = np.sign(sl - mid)
    flips = np.nonzero(np.diff(s))[0]
    flips = flips[np.concatenate(([True], np.diff(flips) > FS // 50))]   # one per extreme
    if len(flips) < 2:
        return 0.0
    return FS / (2 * np.mean(np.diff(flips)))


def band_db(x, lo):
    """Energy above lo Hz relative to the whole (dB), Hann window."""
    x = x.astype(float) - x.mean()
    s = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    fr = np.fft.rfftfreq(len(x), 1 / FS)
    return 10 * np.log10(s[fr > lo].sum() / s.sum() + 1e-30)


# name: (mode, k0, k1, k2, k3, blocks, input Hz (0 click, < 0 clean sine)[, switch block, j0, j1, j2, j3[, ramp]])
CASES = {
    "dry_m": ("m", 15360, BIG, BIG, 0, 40, 1000),
    "dry_s": ("s", 15360, BIG, CE, 0, 40, 1000),
    "ir_ce": ("s", 0, 0, CE, BIG, 40, 0),
    "ir_dual": ("s", 0, 0, DUAL, BIG, 40, 0),
    "ir_ens": ("m", 0, 0, BIG, BIG, 40, 0),
    "depth_ce": ("s", 20000, BIG, CE, BIG, 2760, 1000),
    "depth_def": ("m", 20000, DEFAULT[1], CE, BIG, 2760, 1000),
    "depth_dual": ("s", 20000, BIG, DUAL, BIG, 2760, 1000),
    "rate_0": ("m", 0, BIG, CE, BIG, 8270, 1000),
    "rate_j1": ("m", 12288, BIG, CE, BIG, 3450, 1000),
    "rate_j2": ("m", 16384, BIG, CE, BIG, 2070, 1000),
    "rate_max": ("m", BIG, BIG, CE, BIG, 700, 1000),
    "ensemble": ("m", 1000, 0, BIG, BIG, 2760, 1000),
    "jump": ("s", 0, 0, 0, 16384, 400, 200, 200, BIG, BIG, BIG, BIG),
    "jump_back": ("s", BIG, BIG, BIG, BIG, 400, 200, 200, 0, 0, 0, 0),
    "dc_tail": ("s",) + DEFAULT + (400, 0),
    "dc_mean": ("s",) + DEFAULT + (700, 1000),
    "wet_1k": ("s", 15360, DEFAULT[1], CE, BIG, 700, 1000),
    "interp_5k": ("m", 20000, BIG, CE, BIG, 700, -5000),
}
for f in (1000, 3000, 5000, 7000, 9000, 12000, 15000):
    CASES[f"lp_{f}"] = ("m", 0, 0, CE, BIG, 60, -f)
LEVEL_F = (200, 300, 450, 700, 1000, 1500, 2200, 3300, 5000)
for f in LEVEL_F:
    CASES[f"level_{f}"] = ("s",) + DEFAULT + (700, f)
ZIP = {"static": None, "blue": 0, "green": 1, "white": 2, "orange": 3, "all": 4}
for name, kn in ZIP.items():
    k = list(DEFAULT)
    j = list(DEFAULT)
    if kn is not None:
        idx = range(4) if kn == 4 else [kn]
        for i in idx:
            k[i], j[i] = 0, BIG
    CASES[f"zip_{name}"] = ("s", *k, 260, -200, 100, *j, 1024)
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

    n = len(out["dry_m"])
    check(np.array_equal(out["dry_m"][:, 0], sine_in(n, 1000)), "mix 0, mono: output is the dry input, bit exact")
    check(np.array_equal(out["dry_s"][:, 0], sine_in(n, 1000)) and
          np.array_equal(out["dry_s"][:, 1], sine_in(n, 1000, quarter=True)), "mix 0, stereo: L and R dry, bit exact")

    # delay per character (depth 0, fully wet; the low-pass adds its own group delay)
    tau = lp_delay()
    # white max: 2 ms + 1 sample + the 0.2 ms vibrato offset; its three taps' wobble does not quite cancel
    for name, want, tol in (("ir_ce", CE_D, 0.3), ("ir_dual", DUAL_D, 0.3), ("ir_ens", ENS_D + 9, 1.5)):
        gd = group_delay(out[name][:, 0]) - tau
        check(abs(gd - want) < tol, f"{name[3:]}: depth 0 delay {gd:.2f} samples = {gd / 44.1:.3f} ms "
              f"(expect {want:.1f} = {want / 44.1:.2f} ms; low-pass {tau:.2f} samples removed)")

    def dev_ms(x):
        pd = phase_dev(x, 1000)
        return (pd.max() - pd.min()) / (2 * np.pi * 1000) * 1000 / 2

    for name, want in (("depth_ce", 3.0), ("depth_def", DEFAULT[1] / 32768 * 3), ("depth_dual", 3.0)):
        got = dev_ms(out[name][:, 0])
        check(abs(got / want - 1) < 0.1, f"{name}: sweep +-{got:.2f} ms (expect +-{want:.2f})")

    for name, k in (("rate_0", 0), ("rate_j1", 12288), ("rate_j2", 16384), ("rate_max", BIG)):
        got, want = lfo_hz(phase_dev(out[name][:, 0], 1000)), rate_hz(k)
        check(abs(got / want - 1) < 0.05, f"LFO rate at blue {k}: {got:.3f} Hz (expect {want:.3f})")

    # wet low-pass (CE, depth 0, wet, clean sines; relative to 1 kHz)
    ref = rms_db(out["lp_1000"][1000:, 0])
    resp = []
    for f in (3000, 5000, 7000, 9000, 12000, 15000):
        got = rms_db(out[f"lp_{f}"][1000:, 0]) - ref
        want = 20 * np.log10(abs(lp_resp(f)) / abs(lp_resp(1000)))
        resp.append(f"{f // 1000}k {got:+.1f}")
        check(abs(got - want) < 1.0, f"wet low-pass {f} Hz: {got:+.1f} dB (4-pole Butterworth 9 kHz: {want:+.1f})")
    print("     wet response: " + ", ".join(resp))

    # linear interpolation while sweeping: its loss swings with the fractional delay (0 .. -0.6 dB at 5 kHz)
    x = out["interp_5k"][FS // 4:, 0].astype(float)
    sp = np.fft.fft(x)
    h = np.zeros(len(x))
    h[0] = 1
    h[1:(len(x) + 1) // 2] = 2
    env = np.abs(np.fft.ifft(sp * h))[2000:-2000]
    env = np.convolve(env, np.ones(9) / 9, mode="valid")    # one 5 kHz period
    rip = 20 * np.log10(env.max() / env.min())
    check(rip < 1.0, f"interpolation: 5 kHz, depth max, wet: amplitude ripple {rip:.2f} dB (linear: <= 0.6 expected)")

    # stereo phase
    il, ir = out["ir_ce"][:, 0], out["ir_ce"][:, 1]
    c = np.corrcoef(il, ir)[0, 1]
    check(c < -0.99 and il.max() > -il.min(), f"CE stereo: R's wet = -L's wet (IR correlation {c:+.3f}, L positive)")
    c = np.corrcoef(out["ir_dual"][:, 0], out["ir_dual"][:, 1])[0, 1]
    check(c > 0.99, f"DUAL stereo: R's wet not inverted (IR correlation {c:+.3f})")
    pl, pr = phase_dev(out["depth_ce"][:, 0], 1000), phase_dev(out["depth_ce"][:, 1], 1000)
    c = np.corrcoef(pl, pr)[0, 1]
    check(c > 0.95, f"CE stereo: one delay line, R swept in phase with L (correlation {c:+.3f})")
    pl, pr = phase_dev(out["depth_dual"][:, 0], 1000), phase_dev(out["depth_dual"][:, 1], 1000)
    c = np.corrcoef(pl, pr)[0, 1]
    check(c < -0.95, f"DUAL stereo: R swept in anti-phase with L (correlation {c:+.3f})")

    pe = phase_dev(out["ensemble"][:, 0], 1000)
    got = peak_hz(pe, 2, 30)
    check(abs(got - 19.5) < 0.5, f"white max (old ensemble): 6.5 Hz vibrato on 3 taps, summed wobble at 3x "
          f"({got:.2f} Hz, expect 19.5)")
    lv = rms_db(out["ensemble"][FS // 2:, 0]) - rms_db(sine_in(n, 1000))
    check(-8 < lv < 1, f"white max (3 taps, wet): level {lv:+.1f} dB vs dry")

    # levels: a 50/50 chorus combs (L = dry + wet peaks where R = dry - wet dips), so judge the average over
    # frequency; single frequencies only within the comb's range
    lvs = np.zeros((len(LEVEL_F), 2))
    for i, f in enumerate(LEVEL_F):
        x = out[f"level_{f}"]
        m = len(x)
        for ch, q in ((0, False), (1, True)):
            lvs[i, ch] = rms_db(x[m // 4:, ch]) - rms_db(sine_in(m, f, quarter=q))
    for ch in (0, 1):
        avg = 10 * np.log10(np.mean(10 ** (lvs[:, ch] / 10)))
        check(abs(avg) < 1.5 and lvs[:, ch].min() > -6 and lvs[:, ch].max() < 3.5,
              f"default knobs, {'LR'[ch]}: level {avg:+.1f} dB vs bypass, power average 200 .. 5000 Hz "
              f"(each {lvs[:, ch].min():+.1f} .. {lvs[:, ch].max():+.1f})")
    print("     levels L/R by Hz: " + ", ".join(f"{f} {lvs[i, 0]:+.1f}/{lvs[i, 1]:+.1f}"
                                            for i, f in enumerate(LEVEL_F)))
    x = out["wet_1k"]
    lv = rms_db(x[len(x) // 4:, 0]) - rms_db(sine_in(len(x), 1000))
    check(-1 < lv < 0.5, f"fully wet, 1 kHz: level {lv:+.1f} dB vs bypass")

    # clicks and zipper
    for name in ("jump", "jump_back"):
        j = out[name]
        slope = np.abs(np.diff(j[64 * 150:, :], axis=0)).max()
        dry_slope = np.abs(np.diff(sine_in(len(j), 200))).max()
        check(slope < 2.2 * dry_slope, f"{name}: all knobs {'0 -> max' if name == 'jump' else 'max -> 0'}: "
              f"largest step {slope} (dry sine {dry_slope}): no click")
    floor = max(band_db(out["zip_static"][64 * 100:64 * 228, ch], 3000) for ch in (0, 1))
    for name in ZIP:
        if name == "static":
            continue
        x = out[f"zip_{name}"]
        z = max(band_db(x[64 * 100:64 * 228, ch], 3000) for ch in (0, 1))
        check(z < -70, f"zipper, {name} turned 0 -> max at 1024 per block: energy above 3 kHz {z:.1f} dB "
              f"(static {floor:.1f}; 200 Hz sine)")

    # DC
    t = out["dc_tail"][FS // 4:, :]
    check(np.abs(t).max() <= 1, f"no DC / limit cycle: after a click, silence decays to {np.abs(t).max()} LSB")
    x = out["dc_mean"][FS // 4:, :].astype(float)
    hw = np.hanning(len(x))
    dl = (x * hw[:, None]).sum(axis=0) / hw.sum()  # Hann-weighted: the swept sine leaks no mean of its own
    n = len(out["dc_mean"])
    ins = [sine_in(n, 1000)[FS // 4:], sine_in(n, 1000, quarter=True)[FS // 4:]]
    dc_in = [(v * hw).sum() / hw.sum() for v in ins]   # the driver's sine has its own -0.5 LSB (>> 1)
    g = np.sqrt(0.5)                                   # dry and wet gains at 50/50; CE: L = dry + wet, R = dry - wet
    dl = dl - np.array([dc_in[0] * 2 * g, dc_in[1] * 0])
    check(np.abs(dl).max() < 0.25, f"no DC: default knobs, 1 kHz: added mean {dl[0]:+.2f} / {dl[1]:+.2f} LSB "
          f"(input's own {dc_in[0]:+.2f} removed)")

    sat = [k for k, x in out.items() if k.startswith("range") and np.mean(np.abs(x) >= 32700) > 0.001]
    check(not sat, f"range: 32 corner cases, none saturating" + (f": {sat[:4]}" if sat else ""))

    st = max(c[1] for k, c in cyc.items() if CASES[k][0] == "s")
    mo = max(c[1] for k, c in cyc.items() if CASES[k][0] == "m")
    peak = max(c[0] for c in cyc.values())
    ce = cyc["level_1000"][1]
    check(st <= BUDGET, f"cycles per call: default (CE) stereo mean {ce} ({ce / 64:.0f}/frame); worst stereo mean "
          f"{st} ({st / 64:.0f}/frame), mono mean {mo}, peak {peak}; budget {BUDGET}")

    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
