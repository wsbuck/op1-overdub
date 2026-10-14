#!/usr/bin/env python3
"""Simulator checks for the harmonic engine (src/engine/harm.c) and its table builder (harm_build.h) before flashing.

  python3 src/engine/test/harm_check.py [--keep DIR]

Builds harm.c + tramp.S with harm_main.c (which also builds and publishes the tables, as the screen does) and runs every
case under bfin-elf-run at 88.2 kHz (simdrive.py). Checks: pitch; drawn harmonic levels within +-1 dB (saw, square,
sparse, tilt, extend); every built level band-limited (nothing above its harmonic limit, so nothing passes Nyquist at the
level the engine picks), peak <= 0.9 FS, interpolation deltas, agreement with a float model; the RMS loudness rule;
aliasing at high notes with warp and unison; level steps in a wide glide; a click-free crossfade when the drawing
changes; the sine before the first build; stability at extreme knobs; the output guard; CYCLES per render call and per
table build. Exits non-zero if a check fails. --keep DIR keeps the raw sample files.
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np

from simdrive import FS, alias_ratio, db, harmonics, inc, level_at, load, run, spectrum
from simrun import CPU_BUDGET

LEVELS, H0 = 9, 256
SIZES = [max(8 * (H0 >> L), 32) for L in range(LEVELS)]
OFF = [sum(SIZES[:L]) for L in range(LEVELS)]


def pack(levels, uni=0, warp=0, tilt=0, extend=0):
    """harm.h packing -> 8 int16 knob values"""
    lv = list(levels) + [0] * (24 - len(levels))
    k = [(uni & 63) | (warp & 63) << 6 | 1 << 12, (tilt & 31) | (extend & 15) << 5]
    for j in range(6):
        k.append(sum((lv[4 * j + i] & 15) << (4 * i) for i in range(4)))
    return [v - 65536 if v >= 32768 else v for v in k]


def lvl_of(a):
    if a <= 0:
        return 0
    s = 15 - round(-20 * np.log10(a) / 3)
    return s if s >= 1 else 0


SAW = [lvl_of(1 / n) for n in range(1, 25)]
SQUARE = [lvl_of(1 / n) if n % 2 else 0 for n in range(1, 25)]
SPARSE = [15, 0, 0, 9, 0, 0, 12, 0, 0, 0, 6, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 1]   # bell-ish, every step size
SINE = [15]
TOP = [0] * 12 + [15, 0, 12, 0, 9, 0, 15, 0, 12, 0, 9, 0]                                  # bars 13..24 only
FULL = [15] * 24


def model(levels, tilt=0, extend=0):
    """Float model of harm_build: (frame of FRAME_STRIDE floats, requested amplitudes a[1..256])"""
    att = np.full(H0 + 1, np.nan)
    for i, v in enumerate(levels):
        if v:
            att[i + 1] = (15 - v) * 3 / 6.0206
    if extend:
        h, r = 25, 1
        while h <= H0:
            for j in range(12):
                if h > H0:
                    break
                if not np.isnan(att[13 + j]):
                    att[h] = att[13 + j] + r * (16 - extend) * 1.5 / 6.0206
                h += 1
            r += 1
    k = np.arange(H0 + 1)
    if tilt:
        att[2:] -= np.log2(k[2:]) * tilt * 0.75 / 6.0206
    a = np.where(np.isnan(att), 0, 2.0 ** (20 - (att - np.nanmin(att))) if np.any(~np.isnan(att)) else 0)
    a[a < 16] = 0
    top = int(np.max(np.nonzero(a)[0])) if a.any() else 0
    lv = []
    for L in range(LEVELS):
        h = min(H0 >> L, top)
        s = np.zeros(SIZES[L] // 2 + 1, complex)
        s[1:h + 1] = -1j * a[1:h + 1]
        lv.append(np.fft.irfft(s, SIZES[L]) * SIZES[L])
    lv = np.concatenate(lv)
    denom = max(np.abs(lv).max(), 3 * np.sqrt((a ** 2).sum())) if a.any() else 1
    return lv * 29490 / denom, a


F220, A4 = 220.0, 440.0
CASES = {
    # name: (knobs, f_start, calls, f_end, extra)
    "sine440": (pack(SINE), A4, 345, None, ""),
    "nobuild440": (pack(SINE), A4, 200, None, "nobuild"),
    "saw220": (pack(SAW), F220, 690, None, ""),
    "square220": (pack(SQUARE), F220, 690, None, ""),
    "sparse220": (pack(SPARSE), F220, 690, None, ""),
    "tilt_dark220": (pack(SAW, tilt=-12), F220, 690, None, ""),
    "tilt_bright220": (pack(SPARSE, tilt=8), F220, 690, None, ""),
    "extend110": (pack(TOP, extend=12), 110, 1380, None, ""),
    "unison440": (pack(SINE, uni=63), A4, 690, None, ""),
    "warp440": (pack(SINE, warp=63), A4, 345, None, ""),
    "glide_saw": (pack(SAW), 60, 1380, 6000, ""),
    "glide_warp": (pack(SINE, warp=63), 60, 1380, 6000, ""),
    "switch220": (pack(SAW), F220, 200, None, "switch 100 " + " ".join(map(str, pack(SQUARE)))),
    "switch_steady_saw": (pack(SAW), F220, 200, None, ""),
    "switch_steady_sq": (pack(SQUARE), F220, 200, None, ""),
    "silent": (pack([]), A4, 100, None, ""),
}
DUMPS = {"saw": pack(SAW), "square": pack(SQUARE), "sparse": pack(SPARSE), "tilt_dark": pack(SAW, tilt=-12),
         "tilt_bright": pack(SPARSE, tilt=8), "extend": pack(TOP, extend=12), "full_bright": pack(FULL, tilt=15, extend=15),
         "full_dark": pack(FULL, tilt=-16, extend=15), "sine": pack(SINE), "h24": pack([0] * 23 + [15])}
for n in DUMPS:
    CASES["dump_" + n] = (DUMPS[n], A4, 1, None, "dump")
for name, lv, fs in (("saw", SAW, (2093, 4186)), ("full", FULL, (2093, 4186)), ("sparse", SPARSE, (2093, 4186)),
                     ("h24", [0] * 23 + [15], (440, 784))):         # H24 at G5 is 19 kHz: above 20 kHz nothing is left to measure
    for wa in (0, 63):
        for f in fs:
            CASES[f"alias_{name}_w{wa}_f{f}"] = (pack(lv, warp=wa, extend=15 if name == "full" else 0), f, 345,
                                                 None, "")
# Proposed presets (notes/harm-fm4.md): every one is checked for level and cycles at three pitches.
PRESETS = {"organ": pack([15, 15, 0, 15]),
           "clarinet": pack([15, 0, 13, 0, 11, 0, 10, 0, 8, 0, 7, 0, 5, 0, 4, 0, 3, 0, 2, 0, 1, 0, 1, 0], uni=8),
           "hollow": pack([15, 14, 0, 12, 0, 11, 0, 10, 0, 9, 0, 9, 0, 8, 0, 8, 0, 7, 0, 7, 0, 6, 0, 6]),
           "bell": pack([15, 0, 0, 10, 0, 0, 12, 0, 0, 0, 9, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 5], warp=6, extend=4),
           "saw": pack(SAW, uni=32),
           "square": pack(SQUARE, uni=12),
           "choir": pack([12, 13, 14, 15, 14, 12, 9, 6, 4, 2, 1, 0, 3, 5, 6, 5, 3, 1], uni=48),
           "glass": pack([12, 0, 0, 14, 0, 0, 0, 12, 0, 0, 0, 10, 0, 0, 0, 9, 0, 0, 0, 7, 0, 0, 0, 6], tilt=2, extend=10,
                         uni=20)}
for name, k in PRESETS.items():
    for f in (110, 440, 1760):
        CASES[f"preset_{name}_f{f}"] = (k, f, 120, None, "")
EXTREMES = {"full_bright": pack(FULL, uni=63, warp=63, tilt=15, extend=15),
            "full_dark": pack(FULL, uni=63, warp=63, tilt=-16, extend=15),
            "h24_bright": pack([0] * 23 + [15], uni=63, warp=63, tilt=15, extend=15),
            "saw_plain": pack(SAW), "sine_warp": pack(SINE, warp=63, uni=63)}
for name, k in EXTREMES.items():
    for f in (27.5, 880, 4186, 12000):
        CASES[f"range_{name}_f{round(f)}"] = (k, f, 60, None, "")


def args(k, f0, calls, f1, extra):
    return " ".join(map(str, k)) + f" {inc(f0)} {calls} {inc(f1) if f1 else 0}" + (f" {extra}" if extra else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    workdir = tempfile.mkdtemp(prefix="harm_check_")
    run("harm", "harm_main", {n: args(*c) for n, c in CASES.items()}, workdir)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    data, worst, build = {}, (0, ""), 0
    for name in CASES:
        x, info = load(workdir, name)
        data[name] = x
        build = max(build, info.get("build", 0))
        if not name.startswith("dump"):
            worst = max(worst, (info["max"], name))
            if info["guard"] != 0x5a5a5a5a:
                check(False, f"{name}: output guard overwritten ({info['guard']:08x})")

    # Builder (on the Blackfin): band limits, peak, deltas, float model, loudness rule.
    leak, dmax, snr_min, peaks = -999.0, 0, 999.0, []
    for n, k in DUMPS.items():
        fr = data["dump_" + n].astype(float)
        lv = {"saw": SAW, "square": SQUARE, "sparse": SPARSE, "tilt_dark": SAW, "tilt_bright": SPARSE, "extend": TOP,
              "full_bright": FULL, "full_dark": FULL, "sine": SINE, "h24": [0] * 23 + [15]}[n]
        tilt = {"tilt_dark": -12, "tilt_bright": 8, "full_bright": 15, "full_dark": -16}.get(n, 0)
        ext = {"extend": 12, "full_bright": 15, "full_dark": 15}.get(n, 0)
        ref, amps = model(lv, tilt, ext)
        for L in range(LEVELS):
            seg = fr[OFF[L]:OFF[L] + SIZES[L]]
            m = np.abs(np.fft.rfft(seg))
            if len(m) > (H0 >> L) + 1:                  # re a full-scale (0.9 FS) sine at this level's size
                leak = max(leak, db(m[(H0 >> L) + 1:].max(), 29490 * SIZES[L] / 2))
            dmax = max(dmax, np.abs(np.diff(np.append(seg, seg[0]))).max())
        peaks.append(np.abs(fr).max())
        e = fr - ref
        snr = 10 * np.log10((ref ** 2).sum() / max((e ** 2).sum(), 1e-9))
        snr_min = min(snr_min, snr)
        if n == "sine":
            check(abs(np.abs(fr[:2048]).max() - 0.6 * 32767) < 40, f"builder: one harmonic peaks at 0.6 FS "
                  f"({np.abs(fr[:2048]).max():.0f}, expect {0.6 * 32767:.0f})")
        if n == "saw":
            rms_saw = np.sqrt((fr[:2048] ** 2).mean())
            rms_sine = np.sqrt((data["dump_sine"].astype(float)[:2048] ** 2).mean()) if "dump_sine" in data else 0
    check(leak < -80, f"builder: energy above each level's harmonic limit {leak:.1f} dB re 0.9 FS (every drawing)")
    check(max(peaks) <= 29495, f"builder: frame peaks {min(peaks):.0f}..{max(peaks):.0f} (<= 0.9 FS)")
    check(dmax < 32767, f"builder: max adjacent delta {dmax:.0f} (wt_osc.h needs < 32767)")
    check(snr_min > 60, f"builder: matches the float model, worst SNR {snr_min:.1f} dB")
    check(abs(db(rms_saw, rms_sine)) < 1.5, f"builder: saw RMS {db(rms_saw, rms_sine):+.2f} dB re sine (RMS rule: "
          "same loudness unless the 0.9 FS peak cap applies)")
    check(build <= 2_500_000, f"builder: max {build} CYCLES per table ({build / 400e3:.1f} ms at 400 MHz)")

    # Pitch and the sine before the first build.
    for name in ("sine440", "nobuild440"):
        fr, m = spectrum(data[name])
        pk = fr[np.argmax(m)]
        h2 = db(level_at(fr, m, 880), m.max())
        check(abs(pk - 440) < 1.5 and h2 < -60, f"{name}: fundamental {pk:.2f} Hz, H2 {h2:.1f} dB")
    pk = np.abs(data["sine440"][len(data["sine440"]) // 4:]).max()
    check(abs(pk - 0.3 * 32767) < 300, f"sine440: output peak {pk} (0.6 FS table at -6 dB = {0.3 * 32767:.0f})")

    # Drawn levels within +-1 dB of the request, relative to harmonic 1.
    def levels(name, f0, lv, tilt=0, ext=0, upto=24):
        _, amps = model(lv, tilt, ext)
        got = harmonics(data[name], f0, upto)
        want = amps[1:upto + 1]
        ref = int(np.argmax(want))
        err = [db(got[i], got[ref]) - db(want[i], want[ref]) for i in range(upto) if want[i] > want[ref] * 10 ** (-45 / 20)]
        off = [db(got[i], got[ref]) for i in range(upto) if want[i] == 0]
        return max(np.abs(err)), max(off) if off else -999
    for name, f0, lv, tilt, ext, upto in (("saw220", F220, SAW, 0, 0, 24), ("square220", F220, SQUARE, 0, 0, 24),
                                          ("sparse220", F220, SPARSE, 0, 0, 24),
                                          ("tilt_dark220", F220, SAW, -12, 0, 24),
                                          ("tilt_bright220", F220, SPARSE, 8, 0, 24),
                                          ("extend110", 110, TOP, 0, 12, 60)):
        err, off = levels(name, f0, lv, tilt, ext, upto)
        check(err < 1.0 and off < -50, f"{name}: harmonics 1..{upto} within {err:.2f} dB of the request "
              f"(off harmonics {off:.1f} dB)")

    fr, m = spectrum(data["unison440"])
    f2 = 440 * 2 ** (30 / 1200)
    check(db(level_at(fr, m, f2, 2), level_at(fr, m, 440, 2)) > -3, f"unison: partial at {f2:.1f} Hz present")
    fr, m = spectrum(data["warp440"])
    check(db(level_at(fr, m, 880), level_at(fr, m, 440)) > -20, "warp adds harmonics to a sine: H2 "
          f"{db(level_at(fr, m, 880), level_at(fr, m, 440)):.1f} dB")

    # Aliasing at high notes (what survives TE's 2:1 decimation: below 20 kHz).
    al = {n: alias_ratio(data[n], CASES[n][1]) for n in CASES if n.startswith("alias")
          and np.abs(data[n]).max() > 100}            # (H24 with max warp at G5: band-limited away, silent)
    for n, v in sorted(al.items()):
        print(f"INFO {n}: {v:.1f} dB")
    w0 = max((v, n) for n, v in al.items() if "_w0_" in n)
    w1 = max((v, n) for n, v in al.items() if "_w63_" in n)
    check(w0[0] < -40, f"aliasing, warp 0, C7/C8 (H24 at A4/G5): worst {w0[0]:.1f} dB ({w0[1]})")
    check(w1[0] < -30, f"aliasing, warp max: worst {w1[0]:.1f} dB ({w1[1]})")

    # Glides: the level crossfade must not step the level (RMS over 4 periods, as wt_check.py).
    def glide_step(name):
        x = data[name].astype(float)
        f0, calls, f1 = CASES[name][1], CASES[name][2], CASES[name][3]
        n = len(x)
        steps, prev = [], None
        for c in range(64, n - 4096, 128):
            f = f0 + (f1 - f0) * c / n
            if f > 3000:
                break
            half = int(2 * FS / f)
            if c - half < 0 or c + half > n:
                continue
            r = 20 * np.log10(np.sqrt((x[c - half:c + half] ** 2).mean()) + 1e-9)
            if prev is not None:
                steps.append(abs(r - prev))
            prev = r
        return max(steps)
    for name in ("glide_saw", "glide_warp"):
        st = glide_step(name)
        check(st < 0.5, f"{name} 60 Hz -> 6 kHz: max level change between 4-period windows {st:.2f} dB")

    # Crossfade: saw -> square at call 100: no step beyond the steady waveforms' own, done in ~1024 samples.
    x = data["switch220"].astype(float)
    s0 = 100 * 128
    jump = np.abs(np.diff(x[s0 - 256:s0 + 1300])).max()
    steady = max(np.abs(np.diff(data[n].astype(float)[s0 - 256:s0 + 1300])).max()
                 for n in ("switch_steady_saw", "switch_steady_sq"))
    hard = np.concatenate([data["switch_steady_saw"][:s0], data["switch_steady_sq"][s0:]]).astype(float)
    hard_jump = np.abs(np.diff(hard[s0 - 256:s0 + 1300])).max()
    check(jump <= steady * 1.05 + 4, f"crossfade saw -> square: max sample step {jump:.0f} (steady {steady:.0f}; "
          f"a hard switch: {hard_jump:.0f})")
    pre, post = x[s0 - 8192:s0], x[s0 + 1100:s0 + 1100 + 8192]
    sq = data["switch_steady_sq"].astype(float)[s0 + 1100:s0 + 1100 + 8192]
    check(np.abs(post - sq).max() < 64, f"crossfade complete after 1100 samples (max diff to a steady square "
          f"{np.abs(post - sq).max():.0f})")
    h2_pre = db(np.abs(np.fft.rfft(pre * np.hanning(8192)))[round(2 * F220 * 8192 / FS)],
                np.abs(np.fft.rfft(pre * np.hanning(8192)))[round(F220 * 8192 / FS)])
    check(h2_pre > -10, f"before the switch it is the saw (H2 {h2_pre:.1f} dB)")

    # Stability and range.
    rails = [n for n, x in data.items() if not n.startswith("dump") and np.mean(np.abs(x[-1280:]) >= 32700) > 0.05]
    # a drawing whose energy is all near the top (full_bright, h24_bright) rightly goes quiet once that passes 22 kHz
    silent = [n for n, x in data.items() if n.startswith("range") and "bright" not in n and np.abs(x[-1280:]).max() < 300]
    check(not rails and not silent, f"range: {sum(1 for n in CASES if not n.startswith('dump'))} cases, none at the "
          "rails or silent" + (f"; rails: {rails[:4]}" if rails else "") + (f"; silent: {silent[:4]}" if silent else ""))
    check(not data["silent"].any(), "all bars off: silence")
    dc = max(abs(data[n][len(data[n]) // 2:].mean()) for n in CASES if n.startswith("range") and "f28" not in n)
    check(dc < 300, f"DC after the blocker: max |mean| {dc:.0f}")
    pp = {n: int(np.abs(data[n][len(data[n]) // 4:]).max()) for n in CASES if n.startswith("preset")}
    bad = [f"{n} {v}" for n, v in pp.items() if not 3000 <= v <= 26000]
    check(not bad, f"presets: {len(PRESETS)} at 110/440/1760 Hz peak {min(pp.values())}..{max(pp.values())} "
          "(3000..26000)" + (": " + ", ".join(bad) if bad else ""))
    peaks = {n: int(np.abs(x).max()) for n, x in data.items() if not n.startswith("dump")}
    loud = max(peaks, key=peaks.get)
    print(f"INFO peak {peaks[loud]} in {loud}; saw220 {peaks['saw220']}, sine440 {peaks['sine440']}")

    check(worst[0] <= CPU_BUDGET, f"cycles per render call: max {worst[0]} in {worst[1]} (budget {CPU_BUDGET}, "
          f"{worst[0] / 128:.0f} per sample)")
    steady = max(load(workdir, n)[1]["max"] for n in ("saw220", "sparse220"))
    print(f"INFO plain drawing (no unison/warp): {steady} cycles per call ({steady / 128:.0f} per sample)")

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
