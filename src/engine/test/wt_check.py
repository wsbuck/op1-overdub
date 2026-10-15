#!/usr/bin/env python3
"""Simulator checks for the wavetable engine (src/engine/wt.c) before flashing.

  python3 src/engine/test/wt_check.py [--keep DIR]

Checks pitch, frame shapes, unison, aliasing for every bank at high notes, smooth level crossfades
during a wide glide, stability and range across all banks and extreme knobs, blue (position) detents
smoothed per sample instead of stepping (no click, no splatter, still quick), the output guard and
CYCLES per render call. Exits non-zero if a check fails. --keep DIR keeps the sample files.
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np

from simrun import CPU_BUDGET, alias_ratio, db, level_at, load, run_cases, spectrum

BANK = [b * 4096 for b in range(8)]      # green knob value selecting each bank
NAMES = ["analog", "pwm", "vowel", "organ", "sync", "fold", "bell", "digital"]

# name: (position, bank, warp, unison, f_start, calls, f_end)
CASES = {
    "saw440":    (32767, BANK[0], 0, 0, 440, 345, None),
    "sine440":   (0, BANK[0], 0, 0, 440, 345, None),
    "unison440": (0, BANK[0], 0, 32767, 440, 690, None),
    "warp_sine440": (0, BANK[0], 32767, 0, 440, 345, None),
    "glide_sine": (0, BANK[0], 0, 0, 60, 1380, 6000),   # analog's first frame is a sine
    "glide_warp": (32767, BANK[0], 32767, 0, 60, 1380, 6000),
    "glide_saw": (32767, BANK[0], 0, 0, 60, 1380, 6000),
}
# Blue detents (the encoder moves 1024 raw per detent, once per block). bell (bank 6) at its worst spot measured:
# A at position P0, B at P0 + 1024, T turned from P0 to P0 + 1024 before call DET_CALL (engine_main "turn:").
# Also a fast turn (a detent every block across all 12 frames: frame changes mid-block) and the SAMPLE bank.
BELL = 6 * 4096 + 2048
P0, DET_CALL = 12288, 32
for f in (55, 220):
    CASES[f"det_A_f{f}"] = (P0, BELL, 0, 0, f, 128, None)
    CASES[f"det_B_f{f}"] = (P0 + 1024, BELL, 0, 0, f, 128, None)
    CASES[f"det_T_f{f}"] = (P0, BELL, 0, 0, f, 128, None, f"turn:0,1024,100000,{DET_CALL}")
CASES["det_fast"] = (0, BELL, 0, 32767, 110, 128, None, "turn:0,1024,1,4")
CASES["det_fast_end"] = (32767, BELL, 0, 32767, 110, 128, None)
CASES["det_sample"] = (0, 32767, 32767, 0, 220, 64, None, "turn:0,1024,1,4")
for b in range(8):
    for p in (0, 16384, 32767):
        for wa in (0, 32767):
            for f in (1046.5, 4186):
                CASES[f"alias_b{b}_p{p}_w{wa}_f{round(f)}"] = (p, BANK[b], wa, 0, f, 345, None)
        for wa in (0, 32767):
            for u in (0, 32767):
                for f in (27.5, 880, 4186):
                    CASES[f"range_b{b}_p{p}_w{wa}_u{u}_f{round(f)}"] = (p, BANK[b], wa, u, f, 40, None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    workdir = tempfile.mkdtemp(prefix="wt_check_")
    run_cases("wt", CASES, workdir)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    data, worst_cycles = {}, 0
    for name in CASES:
        x, cyc, guard = load(workdir, name)
        data[name] = x
        worst_cycles = max(worst_cycles, cyc)
        if guard != "5a5a5a5a":
            check(False, f"{name}: output guard overwritten ({guard})")

    fr, m = spectrum(data["saw440"])
    peak = fr[np.argmax(m)]
    check(abs(peak - 440) < 3, f"saw440 fundamental {peak:.1f} Hz")
    h = [level_at(fr, m, 440 * n) for n in range(1, 8)]
    check(all(abs(db(h[n - 1], h[0]) + 20 * np.log10(n)) < 1.5 for n in range(2, 8)),
          "analog last frame (saw) harmonics fall as 1/n: " + " ".join(f"{db(v, h[0]):.1f}" for v in h[1:]))
    fr, m = spectrum(data["sine440"])
    check(db(level_at(fr, m, 880), level_at(fr, m, 440)) < -50,
          f"analog last frame is a sine: H2 {db(level_at(fr, m, 880), level_at(fr, m, 440)):.1f} dB")
    fr, m = spectrum(data["unison440"])
    f2 = 440 * 2 ** (30 / 1200)
    check(db(level_at(fr, m, f2, 2), level_at(fr, m, 440, 2)) > -3, f"unison partial at {f2:.1f} Hz present")

    worst = max(((alias_ratio(data[n], CASES[n][4]), n) for n in CASES if n.startswith("alias") and "_w0_" in n))
    check(worst[0] < -34, f"aliasing, warp 0, all banks/positions at C6 and C8: worst {worst[0]:.1f} dB ({worst[1]})")
    worst = max(((alias_ratio(data[n], CASES[n][4]), n) for n in CASES if n.startswith("alias") and "_w32767_" in n))
    check(worst[0] < -30, f"aliasing, max warp: worst {worst[0]:.1f} dB ({worst[1]})")
    for b in range(8):
        r0 = max(alias_ratio(data[n], CASES[n][4]) for n in CASES if n.startswith(f"alias_b{b}_") and "_w0_" in n)
        r1 = max(alias_ratio(data[n], CASES[n][4]) for n in CASES if n.startswith(f"alias_b{b}_") and "_w32767_" in n)
        print(f"INFO bank {b} {NAMES[b]:8s} worst aliasing {r0:.1f} dB, max warp {r1:.1f} dB")
    fr, m = spectrum(data["warp_sine440"])
    h2 = db(level_at(fr, m, 880), level_at(fr, m, 440))
    check(h2 > -20, f"warp adds harmonics to a sine: H2 {h2:.1f} dB")

    # Glides 60 Hz -> 6 kHz (increment linear in time): level crossfades must not step the level. RMS over
    # windows of 4 periods of the current pitch, hop 128; steps between neighbouring windows. Only below
    # 3 kHz: shorter windows (< 59 samples) make this measure noisy (~0.4 dB) even for a pure sine.
    def glide_step(name):
        x = data[name].astype(float)
        f0, calls, f1 = CASES[name][4], CASES[name][5], CASES[name][6]
        n = len(x)
        steps, prev = [], None
        for c in range(64, n - 4096, 128):
            f = f0 + (f1 - f0) * c / n
            if f > 3000:
                break
            half = int(2 * 44100 / f)
            if c - half < 0 or c + half > n:
                continue
            r = 20 * np.log10(np.sqrt((x[c - half:c + half] ** 2).mean()) + 1e-9)
            if prev is not None:
                steps.append(abs(r - prev))
            prev = r
        return max(steps)
    for name, limit in (("glide_sine", 0.5), ("glide_warp", 0.5)):
        step = glide_step(name)
        check(step < limit, f"{name} 60 Hz -> 6 kHz: max level change between 4-period windows {step:.2f} dB")
    x = data["glide_saw"]
    check(np.abs(np.diff(x)).max() < 20000, f"saw glide: max sample step {np.abs(np.diff(x)).max()}")

    # Detents: e = T - A is 0 before the detent and B - A = D after it. A step puts all of D into one sample of e;
    # the smoothed ramp spreads it over blocks, so e's largest sample step stays near D's own (the waveform's slope).
    # Splatter: e's spectrum around the detent (4096 samples, Hann) off D's own lines (D's bins above -60 dB, +-4 bins)
    # and above 3 kHz, relative to D's energy. Bell 220 Hz: a hard step -27 dB, a one-block ramp -52 dB, wt.c -67 dB.
    # Responsive: e reaches half of D (RMS over a block) within 10 blocks (14.5 ms at 88.2 kHz); settled after 80.
    win = np.hanning(4096)
    for f in (55, 220):
        A, Bx, T = (data[f"det_{k}_f{f}"].astype(np.int64) for k in "ABT")
        e, D = T - A, Bx - A
        step, floor, dmax = np.abs(np.diff(e)).max(), np.abs(np.diff(D)).max(), np.abs(D).max()
        check(step <= floor + dmax // 32 + 4, f"bell {f} Hz, blue detent {P0} -> {P0 + 1024}: output step {step} "
              f"(the difference's own slope {floor} + {dmax} / 32; a hard step would be ~{dmax + floor // 2})")
        me = np.abs(np.fft.rfft(e[DET_CALL * 128 - 2048:DET_CALL * 128 + 2048] * win))
        md = np.abs(np.fft.rfft(D[DET_CALL * 128 - 2048:DET_CALL * 128 + 2048] * win))
        lines = np.convolve(md > md.max() * 1e-3, np.ones(9), "same") > 0
        hf = ~lines & (np.fft.rfftfreq(4096, 1 / 44100) > 3000)
        spl = 10 * np.log10(max((me[hf] ** 2).sum(), 1e-12) / (md ** 2).sum())
        check(spl < -58, f"bell {f} Hz detent: splatter above 3 kHz {spl:.1f} dB re the frame change")
        rms = lambda v, c: np.sqrt((v[c * 128:(c + 1) * 128].astype(float) ** 2).mean())
        half = next(c for c in range(DET_CALL, 128) if rms(e, c) >= 0.5 * rms(D, c)) - DET_CALL
        tail = np.abs(T[(DET_CALL + 80) * 128:] - Bx[(DET_CALL + 80) * 128:]).max()
        check(half <= 10 and tail <= 2, f"bell {f} Hz: half way after {half} blocks, |T - B| {tail} after 80 blocks")
    x, y = data["det_fast"], data["det_fast_end"]
    check(np.abs(x[-1280:] - y[-1280:]).max() <= 2 and np.abs(np.diff(x)).max() < 20000,
          f"fast turn 0 -> 32767 (a detent every block, unison): settles on the last frame, max sample step "
          f"{np.abs(np.diff(x)).max()}")
    check(np.abs(data["det_sample"][-1280:]).max() > 1000, "SAMPLE bank (nothing built: bank 0) plays while turning")

    bad = [n for n, x in data.items() if np.mean(np.abs(x[-1280:]) >= 32700) > 0.2]
    silent = [n for n, x in data.items() if n.startswith("range") and np.abs(x[-1280:]).max() < 100]
    check(not bad and not silent, f"range: {len(data)} cases, none at the rails or silent"
          + (f"; rails: {bad[:4]}" if bad else "") + (f"; silent: {silent[:4]}" if silent else ""))
    peaks = {n: int(np.abs(x).max()) for n, x in data.items()}
    loud = max(peaks, key=peaks.get)
    print(f"INFO peak {peaks[loud]} in {loud}; saw440 peak {peaks['saw440']}")
    check(worst_cycles <= CPU_BUDGET, f"cycles per render call: max {worst_cycles} (budget {CPU_BUDGET}, "
          f"{worst_cycles / 128:.0f} per sample)")

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
