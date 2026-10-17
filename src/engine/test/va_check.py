#!/usr/bin/env python3
"""Simulator checks for the VA engine (src/engine/va.c) before flashing.

  python3 src/engine/test/va_check.py [--keep DIR]

Builds va.c + tramp.S with engine_main.c, runs every case in one Docker container under bfin-elf-run,
then checks pitch, wave shapes (sine, saw, square, pulse, the sine → saw morph), detune, the sub, noise,
aliasing, levels at extreme settings, the output guard and CYCLES per render call. Exits non-zero if a check fails.
The sub's shapes (SHIFT + orange = knob 7, va.c SUB_*): each shape's spectrum, its phase lock to osc 1 (the sub
taken out of the output against the same run without it, compared with the ideal shape at osc 1's phase), the
level match, aliasing at C8, crossfade on a shape change (no click; bit-exact to the old shape before and the new one
after), and the null test: knob 7 = 0 (every older sound), or out of range, gives exactly the output of va.c as it
was before the shapes (REF_COMMIT's va.c, built alongside; skipped with a note if git can't give it).
--keep DIR keeps the raw sample files (one per case) for listening or plotting.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

from simrun import CPU_BUDGET, ENGINE, FS, alias_ratio, db, inc, level_at, load, run_cases, spectrum

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
# The null test against REF_COMMIT's va.c runs every case above (knob 7 = 0), plus these: knob 7 out of range plays
# SQUARE, and the fine-adjust knobs 4..6 (TE writes knobs 0..3 for them) change nothing.
REF_COMMIT = "c36e9eb"      # the last va.c with only the square sub
NULL_EXTRA = {
    "null_k7_big":  (2 * T, 9000, 4000, 32767, 440, 120, None, "k4:0,0,0,32767"),
    "null_k7_neg":  (T, 0, 0, 20000, 220, 120, None, "k4:0,0,0,-1"),
    "null_k7_6":    (0, 0, 0, 32767, 880, 120, None, "k4:0,0,0,6"),
    "null_k456":    (T, 3000, 0, 32767, 330, 120, None, "k4:5000,6000,7000,0"),
}
CASES.update(NULL_EXTRA)

# Sub shapes (va.c SUB_*): name, the ideal shape at the sub's phase t (turns; at -1 octave t = osc 1's / 2), its level.
SHAPES = ["SQUARE", "SINE", "TRIANGLE", "SAW", "PULSE 25", "SQUARE -2"]   # = ui.c va_sub_name
G_PULSE = 0x3B6DB6DB * 2 / 2**31


def ideal(sh, phi1):
    """Shape sh at osc 1's unwrapped phase phi1 (turns), naive (no PolyBLEP), with va.c's gains."""
    t = np.mod(phi1 / (4 if sh == 5 else 2), 1)
    sq = np.where(t < 0.5, -1.0, 1.0)
    if sh in (0, 5):
        return sq
    if sh == 1:
        return -1.4 * np.sin(2 * np.pi * t)
    if sh == 2:
        return 1.4 * (4 * np.abs(np.mod(t + 0.25, 1) - 0.5) - 1)
    if sh == 3:
        return 1.4 * (2 * t - 1)
    q = np.mod(t + 0.125, 1)
    return G_PULSE * ((2 * q - 1) - (2 * np.mod(q + 0.25, 1) - 1))


SUB_HALF = 16384            # sub level for the shape tests: the sum stays below the knee (the sub taken out exactly)
CASES["nosub440"] = (0, 0, 0, 0, 440, 345, None)
CASES["nosub_c8"] = (0, 0, 0, 0, 4186, 345, None)
CASES["nosub110"] = (0, 0, 0, 0, 110, 345, None)
for sh in range(6):
    CASES[f"shape{sh}"] = (0, 0, 0, SUB_HALF, 440, 345, None, f"k4:0,0,0,{sh}")
    CASES[f"shape{sh}_c8"] = (0, 0, 0, SUB_HALF, 4186, 345, None, f"k4:0,0,0,{sh}")
    CASES[f"steady{sh}_110"] = (0, 0, 0, SUB_HALF, 110, 345, None, f"k4:0,0,0,{sh}")
    for w in (0, T, 2 * T, 32767):      # levels with the sub, the noise and the wave all full
        for f in (55, 880, 7040):
            CASES[f"lvl_w{w}_n32767_s32767_f{f}_sh{sh}"] = (w, 0, 32767, 32767, f, 60, None, f"k4:0,0,0,{sh}")
    CASES[f"cyc_sh{sh}"] = (32767, 32767, 32767, 32767, 4186, 30, 30, f"k4:0,0,0,{sh}")
# Shape changes at call SW_CALL while a 110 Hz note plays (sub 55 Hz): pairs (from, to), and a fast turn (one shape
# per block from SQUARE, past the last into the out-of-range values, which play SQUARE).
SW_CALL = 200
SW_PAIRS = [(1, 2), (0, 1), (1, 3), (2, 5), (3, 4), (4, 0), (5, 1), (0, 3)]
for a_, b_ in SW_PAIRS:
    CASES[f"sw{a_}{b_}"] = (0, 0, 0, SUB_HALF, 110, 345, None, f"k4:0,0,0,{a_}", f"turn:7,{b_ - a_},100000,{SW_CALL}")
CASES["sw_fast"] = (0, 0, 0, SUB_HALF, 110, 345, None, "turn:7,1,1,200")
CASES["cyc_fade"] = (32767, 32767, 32767, 32767, 4186, 40, None, "turn:7,1,4,0")
for a_ in range(6):             # every crossfade at full load, the most expensive pair (PULSE 25 <-> SQUARE -2) included
    for b_ in range(6):
        if a_ != b_:
            CASES[f"cyc_f{a_}{b_}"] = (32767, 32767, 32767, 32767, 4186, 6, None, f"k4:0,0,0,{a_}",
                                       f"turn:7,{b_ - a_},100,2")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    workdir = tempfile.mkdtemp(prefix="va_check_")
    run_cases("va", CASES, workdir)
    # The reference: REF_COMMIT's va.c in a copy of src/engine, every case with knob 7 = 0 (or out of range)
    refdir, refwork = tempfile.mkdtemp(prefix="va_ref_src_"), tempfile.mkdtemp(prefix="va_ref_")
    shutil.rmtree(refdir)
    shutil.copytree(ENGINE, refdir, ignore=shutil.ignore_patterns("__pycache__"))
    try:
        old = subprocess.run(["git", "show", f"{REF_COMMIT}:src/engine/va.c"], cwd=ENGINE, check=True,
                             capture_output=True).stdout
    except (OSError, subprocess.CalledProcessError):
        old = None
    null_cases = {n: c for n, c in CASES.items()
                  if not n.startswith(("shape", "steady", "sw", "cyc_")) and "_sh" not in n}
    if old:
        open(os.path.join(refdir, "va.c"), "wb").write(old)
        run_cases("va", null_cases, refwork, engine_dir=refdir)
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

    # Levels: never at the rails, never silent. Only the 10% pulse (peak 1.8) reaches into the knee, and the sub's
    # brighter shapes (peak 1.4 on top of the oscillators' at the wrap) with sub and noise full, a little.
    peaks = {n: int(np.abs(x[len(x) // 4:]).max()) for n, x in data.items()}
    bad = [f"{n} ({p})" for n, p in peaks.items() if p > 30000 or p < 3000 or ("w32767" not in n and "pulse" not in
           n and "glide" not in n and not n.startswith("cyc") and p > (28000 if "_sh" in n else 26000))]
    check(not bad, f"levels: {len(data)} cases peak within 3000..26000 (sub shapes with noise 28000, pulse 30000)" +
          ("" if not bad else ": " + ", ".join(bad[:6])))
    sh_peak = max((p, n) for n, p in peaks.items() if "_sh" in n and "w32767" not in n)
    print(f"INFO sub shapes, sub and noise full: peak {sh_peak[0]} in {sh_peak[1]} (the knee starts at 24576)")
    loud = max(peaks, key=peaks.get)
    print(f"INFO peak {peaks[loud]} in {loud}; sine440 {peaks['sine440']}, saw440 {peaks['saw440']}")

    # ---- the sub's shapes ----
    # Null test: knob 7 = 0 (every sound saved before the shapes, the factory preset) and out-of-range values play
    # exactly what REF_COMMIT's va.c played.
    if old:
        diff = [n for n in null_cases if not np.array_equal(data[n], load(refwork, n)[0])]
        check(not diff, f"null test: {len(null_cases)} cases (knob 7 = 0, 32767, -1, 6; knobs 4..6 set) bit-exact to "
              f"{REF_COMMIT}'s va.c" + ("" if not diff else ": differ " + ", ".join(diff[:6])))
    else:
        print(f"SKIP null test: git show {REF_COMMIT}:src/engine/va.c failed")

    def norm(sub, noise=0):         # va.c's output scale 1 / (1 + (sub + noise) / 2), Q31 -> float
        den = (1 << 28) + (((sub << 16) >> 3) >> 1) + ((((noise * noise) >> 15) << 16 >> 3) >> 1)
        return ((1 << 59) // den if den > 1 << 28 else 0x7FFFFFFF) / 2**31

    def sub_part(name, ref):        # the sub alone, in output units
        return data[name].astype(float) - norm(SUB_HALF) * data[ref].astype(float)

    def phi1(f, n):                 # osc 1's unwrapped phase (turns) at output samples 0..n-1, note-on at 1/2
        return 0.5 + np.arange(n) * (inc(f) / 2**32)

    amp = norm(SUB_HALF) * (SUB_HALF / 32768) * 16384       # a sub value of 1.0 in output units
    rms, ph220 = {}, {}
    for sh in range(6):
        y = sub_part(f"shape{sh}", "nosub440")
        ref = amp * ideal(sh, phi1(440, len(y)))
        q = len(y) // 4
        c_all = np.corrcoef(y[q:], ref[q:])[0, 1]
        c_end = np.corrcoef(y[-q:], ref[-q:])[0, 1]
        gain = np.dot(y[q:], ref[q:]) / np.dot(ref[q:], ref[q:])
        check(c_all > 0.995 and c_end > 0.995 and abs(gain - 1) < 0.02,
              f"{SHAPES[sh]}: phase-locked to osc 1 (correlation with the ideal shape {c_all:.4f}, last quarter "
              f"{c_end:.4f}), level x{gain:.3f} of va.c's")
        rms[sh] = np.sqrt(np.mean(y[q:] ** 2))
        fr, m = spectrum(y)
        f0 = 110 if sh == 5 else 220
        h = [level_at(fr, m, f0 * n, 3) for n in range(1, 7)]
        below = level_at(fr, m, f0 / 2, 3)
        r = [db(v, h[0]) for v in h]
        txt = f"{SHAPES[sh]}: H2..H5 " + " ".join(f"{v:.1f}" for v in r[1:5]) + f" dB, {f0 / 2:.0f} Hz {db(below, h[0]):.1f}"
        exp = {0: r[1] < -40 and abs(r[2] + 9.5) < 1 and r[3] < -40,
               1: max(r[1:6]) < -40,
               2: r[1] < -40 and abs(r[2] + 19.1) < 1.5 and r[3] < -40 and abs(r[4] + 28.0) < 2,
               3: abs(r[1] + 6.0) < 1 and abs(r[2] + 9.5) < 1 and abs(r[3] + 12.0) < 1,
               4: abs(r[1] + 3.0) < 1 and abs(r[2] + 9.5) < 1 and r[3] < -30,
               5: r[1] < -40 and abs(r[2] + 9.5) < 1 and r[3] < -40}[sh]
        check(exp and db(below, h[0]) < -40 and abs(fr[np.argmax(m)] - f0) < 3, txt)
        if sh < 5:
            win = np.hanning(len(y) - q)
            spec = np.fft.rfft(y[q:] * win)
            k = int(round(220 * (len(y) - q) / FS))
            ph220[sh] = np.angle(spec[k - 2 + np.argmax(np.abs(spec[k - 2:k + 3]))])
    off = [abs((np.degrees(ph220[sh] - ph220[0]) + 180) % 360 - 180) for sh in range(1, 5)]
    check(max(off) < 5, "-1 octave shapes' fundamentals in phase with the square's (crossfades without a dip): "
          + " ".join(f"{SHAPES[sh]} {o:.1f} deg" for sh, o in zip(range(1, 5), off)))
    lv = {sh: db(rms[sh], rms[0]) for sh in range(6)}
    check(all(-2.5 < v < 0.5 for v in lv.values()), "level match, rms re SQUARE: " +
          " ".join(f"{SHAPES[sh]} {v:+.2f}" for sh, v in lv.items()) + " dB")
    al = {}
    for sh in range(6):
        y = sub_part(f"shape{sh}_c8", "nosub_c8")
        al[sh] = alias_ratio(y, 4186 / (4 if sh == 5 else 2))
    check(all(v < -25 for v in al.values()), "aliasing at C8 (sub 2093 Hz, SQUARE -2 1047 Hz), dB re harmonics: " +
          " ".join(f"{SHAPES[sh]} {v:.1f}" for sh, v in al.items()))

    # Shape changes: before the change the old shape bit for bit, SUB_FADE samples later the new one bit for bit;
    # high-band energy (> 2 kHz: the click) around the change no more than the louder steady shape's there.
    s0, n_fade = SW_CALL * 128, 512

    def hf(x, a, b):
        X = np.fft.rfft(x.astype(float))
        X[np.fft.rfftfreq(len(x), 1 / FS) < 2000] = 0
        y = np.fft.irfft(X, len(x))
        return np.sum(y[a:b] ** 2)
    a_w, b_w = s0 - 256, s0 + n_fade + 256
    for a_, b_ in SW_PAIRS:
        x, xa, xb = data[f"sw{a_}{b_}"], data[f"steady{a_}_110"], data[f"steady{b_}_110"]
        exact = np.array_equal(x[:s0], xa[:s0]) and np.array_equal(x[s0 + n_fade:], xb[s0 + n_fade:])
        splice = np.concatenate([xa[:s0], xb[s0:]])
        e_sw, e_ref, e_cut = hf(x, a_w, b_w), max(hf(xa, a_w, b_w), hf(xb, a_w, b_w)), hf(splice, a_w, b_w)
        step = int(np.max(np.abs(np.diff(x[s0 - 64:s0 + n_fade + 64].astype(int)))))
        steady = int(max(np.max(np.abs(np.diff(xa.astype(int)))), np.max(np.abs(np.diff(xb.astype(int))))))
        check(exact and db(np.sqrt(e_sw), np.sqrt(e_ref)) < 1 and step <= steady,
              f"{SHAPES[a_]} -> {SHAPES[b_]}: old / new shape bit-exact outside the {n_fade}-sample fade, > 2 kHz "
              f"{db(np.sqrt(e_sw), np.sqrt(e_ref)):+.1f} dB re steady (an instant switch "
              f"{db(np.sqrt(e_cut), np.sqrt(e_ref)):+.1f}), largest step {step} (steady {steady})")
    x = data["sw_fast"]
    a_w, b_w = s0 - 256, s0 + 16 * 128
    e_ref = max(hf(data[f"steady{sh}_110"], a_w, b_w) for sh in range(6))
    check(db(np.sqrt(hf(x, a_w, b_w)), np.sqrt(e_ref)) < 1,
          f"fast turn (a shape per block): > 2 kHz {db(np.sqrt(hf(x, a_w, b_w)), np.sqrt(e_ref)):+.1f} dB re the "
          "brightest steady shape")
    cyc = {sh: load(workdir, f"cyc_sh{sh}")[1] for sh in range(6)}
    print("INFO cycles per sample, all knobs full at C8: " + " ".join(f"{SHAPES[sh]} {c / 128:.0f}" for sh, c in cyc.items())
          + f"; during crossfades max {max(load(workdir, n)[1] for n in CASES if n.startswith('cyc_f')) / 128:.0f}")

    check(worst_cycles <= CPU_BUDGET, f"cycles per render call: max {worst_cycles} (budget {CPU_BUDGET}, "
          f"{worst_cycles / 128:.0f} per sample)")

    if a.keep:
        os.makedirs(a.keep, exist_ok=True)
        for f in os.listdir(workdir):
            shutil.copy(os.path.join(workdir, f), a.keep)
    shutil.rmtree(workdir)
    shutil.rmtree(refdir)
    shutil.rmtree(refwork)
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
