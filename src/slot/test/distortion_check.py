#!/usr/bin/env python3
"""Simulator checks for the distortion FX (src/slot/distortion.c) before flashing.

  python3 src/slot/test/distortion_check.py [--keep DIR]

Runs every case on the Blackfin simulator (fxsim.py) and checks: mix 0 is the dry input; drive 0 is near-clean for
tube and clip; every character has its signature (tube: 2nd harmonic; muff: rich odd-dominant series with some
even, long sustain without a gate on a decaying note, mid scoop on noise (test/distortion_sig.c); fuzz: 2nd harmonic
above the fundamental;
clip: odd only; fold: dense upper harmonics, not monotonic in drive; crush: quantisation and aliasing/imaging); the
drive range reaches heavy saturation; loudness stays within -6 .. +4 dB of the dry input across drive (auto gain);
aliasing of tube / clip / fold at max drive; no DC (also after the input stops: the fuzz's gate); clicks on knob
jumps; range (no saturation, 32 corner cases); CYCLES per 64-frame call (typical and worst). And the screen
functions (test/distortion_curve.c): distortion_curve is monotonic for tube and clip, folds for fold, a staircase for
crush, matches the DSP's static response, costs <= 300 cycles per call (450 in a crossfade); distortion_mode_name
names each plateau.

THD, loudness and aliasing use the driver's clean sine (negative frequency, fx_main.c).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

from fxsim import FS, SLOT, rms_db, run_cases, sine_in

BIG = 32767
BUDGET = 30000                                      # cycles per stereo call (64 frames), typical settings
MUFF_GREEN_BUDGET = 36000                           # MUFF (two stages + scoop) with the green filter: ~3.5 % of a block;
                                                    # the user wants green active in every mode, so accept the extra
WORST = 45000                                       # bound for the stress cases (FIXMAX in distortion.c)
CURVE_BUDGET = 310                                  # cycles per distortion_curve call, one character (~300)
CURVE_BLEND = 500                                   # in a crossfade between two characters (two shapes)
DEFAULT = (16000, 16384, 7475, BIG)                 # fx.json default: MUFF, classic thick sustain, flat, full wet
DRIVES = (0, 8192, 16384, 24576, BIG)
CHARS = {"tube": 1536, "muff": 7475, "fuzz": 13414, "clip": 19353, "fold": 25292, "crush": 31231}   # plateau centres
FADES = {"tube_muff": 4506, "muff_fuzz": 10445, "fuzz_clip": 16384, "clip_fold": 22323, "fold_crush": 28262}
NAMES = {c: c.upper() for c in CHARS}
FSDB = 20 * np.log10(32768)                        # rms_db of a full-scale level
N = 8820                                            # analysis length: whole periods of 1 kHz / 5 kHz / 2 kHz / 100 Hz

# name: (mode, k0, k1, k2, k3, blocks, input Hz[, switch block, j0, j1, j2, j3])
CASES = {
    "dry": ("m", 20000, 0, 20000, 0, 40, 1000),
    "dry_s": ("s", BIG, BIG, BIG, 0, 40, 1000),
    "worst_fold": ("s", BIG, BIG, CHARS["fold"], BIG, 300, 5000),
    "worst_muff": ("s", BIG, BIG, CHARS["muff"], BIG, 300, 5000),
}
for f, kc in FADES.items():
    CASES[f"worst_{f}"] = ("s", BIG, BIG, kc, BIG, 300, 5000)
for c, kc in CHARS.items():
    for kd in DRIVES:
        CASES[f"thd_{c}_{kd}"] = ("m", kd, 16384, kc, BIG, 300, -1000)
    CASES[f"dc_{c}"] = ("m", BIG, 16384, kc, BIG, 400, 100)
    CASES[f"gate_{c}"] = ("m", BIG, 16384, kc, BIG, 400, 0)
for c in ("tube", "muff", "clip", "fold"):
    CASES[f"alias_{c}"] = ("m", BIG, 16384, CHARS[c], BIG, 300, -5000)
    CASES[f"alias2k_{c}"] = ("m", BIG, 16384, CHARS[c], BIG, 300, -2000)
# the static-response comparison runs at moderate drive (blue 5941 / 11178 = the taper's 12000 / 20000): heavier
# drive is dominated by the treble lift and oversampling, which the static curve leaves out on purpose
STATIC_KD = (5941, 11178)
for c in ("tube", "clip", "muff"):
    for kd in STATIC_KD:        # MUFF at 300 Hz: its coupling high-pass (110 Hz) would dominate at 100 Hz
        CASES[f"static_{c}_{kd}"] = ("m", kd, 16384, CHARS[c], BIG, 300, -300 if c == "muff" else -100)
CASES["filter_lo"] = ("m", 0, 0, CHARS["clip"], BIG, 300, -1000)
CASES["filter_hi"] = ("m", 0, BIG, CHARS["clip"], BIG, 300, -1000)
# typical settings for the CPU budget (music-like: a few hundred Hz to 1 kHz)
TYPICAL = {
    "default_200": ("s",) + DEFAULT + (100, 200),
    "default_1k": ("s",) + DEFAULT + (100, 1000),
    "tube_max_1k": ("s", BIG, 16384, CHARS["tube"], BIG, 100, 1000),
    "muff_max_1k": ("s", BIG, 16384, CHARS["muff"], BIG, 100, 1000),
    "muff_green_1k": ("s", 16000, 20480, CHARS["muff"], BIG, 100, 1000),
    "fuzz_1k": ("s", 20000, 20000, CHARS["fuzz"], BIG, 100, 1000),
    "clip_1k": ("s", 20000, 8000, CHARS["clip"], BIG, 100, 1000),
    "fold_1k": ("s", 20000, 16384, CHARS["fold"], BIG, 100, 1000),
    "fold_max_1k": ("s", BIG, 16384, CHARS["fold"], BIG, 100, 1000),
    "fuzz_max_1k": ("s", BIG, 16384, CHARS["fuzz"], BIG, 100, 1000),
    "crush_1k": ("s", 20000, 24000, CHARS["crush"], BIG, 100, 1000),
}
for name, case in TYPICAL.items():
    CASES["cpu_" + name] = case
# knob jumps: from -> to (one knob or all), 200 Hz
JUMPS = {
    "drive": ((0, 16384, CHARS["clip"], BIG), (BIG, 16384, CHARS["clip"], BIG)),
    "drive_down": ((BIG, 16384, CHARS["tube"], BIG), (0, 16384, CHARS["tube"], BIG)),
    "drive_muff": ((0, 16384, CHARS["muff"], BIG), (BIG, 16384, CHARS["muff"], BIG)),
    "green": ((16000, 0, CHARS["clip"], BIG), (16000, BIG, CHARS["clip"], BIG)),
    "green_lin": ((0, 0, CHARS["tube"], BIG), (0, BIG, CHARS["tube"], BIG)),
    "green_fuzz": ((20000, 16384, CHARS["fuzz"], BIG), (20000, 26000, CHARS["fuzz"], BIG)),
    "green_muff": ((16000, 0, CHARS["muff"], BIG), (16000, BIG, CHARS["muff"], BIG)),
    "char": ((20000, 16384, 0, BIG), (20000, 16384, BIG, BIG)),
    "char_muff": ((16000, 16384, CHARS["tube"], BIG), (16000, 16384, CHARS["fuzz"], BIG)),
    "mix": ((BIG, 16384, CHARS["clip"], 0), (BIG, 16384, CHARS["clip"], BIG)),
    "all": ((0, 0, 0, 0), (BIG, BIG, BIG, BIG)),
}


def path(a, b, every):
    """Knob settings the FX passes through after a jump a -> b (slew: drive and character 2048, green 1024 per
    block), every few blocks, and b."""
    k, rate, out = list(a), (2048, 1024, 2048, 1 << 16), []
    while k != list(b):
        for _ in range(every):
            k = [x if x == y else (min(y, x + r) if y > x else max(y, x - r)) for x, y, r in zip(k, b, rate)]
        out.append(tuple(k))
    return out or [tuple(b)]


REFS = {}
for name, (a, b) in JUMPS.items():
    CASES[f"jump_{name}"] = ("s",) + a + (400, 200, 200) + b
    REFS[name] = []
    for k in [tuple(a)] + path(a, b, 1 if name.startswith("green") else 4):    # resonance: every green step
        CASES[f"ref_{'_'.join(map(str, k))}"] = ("s",) + k + (300, 200)
        REFS[name].append(f"ref_{'_'.join(map(str, k))}")
for k in range(16):
    ks = tuple(BIG if k >> b & 1 else 0 for b in range(4))
    for f in (100, 5000):
        CASES[f"range_{k:x}_{f}"] = ("s",) + ks + (200, f)
# screen functions: knob sets for distortion_curve
CURVES = {f"{c}_{kd}": (kd, 16384, kc, BIG) for c, kc in CHARS.items() for kd in DRIVES}
CURVES.update({f"{c}_{kd}": (kd, 16384, CHARS[c], BIG) for c in ("tube", "clip", "muff") for kd in STATIC_KD})
CURVES["blend"] = (20000, 0, FADES["fold_crush"], 0)
CURVES["blend_muff"] = (20000, 0, FADES["tube_muff"], 0)
CURVES.update({f"name_{f}_{d}": (0, 0, kc + d, 0) for f, kc in FADES.items() for d in (-1, 1)})
# inputs fx_main.c does not make (test/distortion_sig.c): a decaying 200 Hz note, noise
SIGS = {f"decay_{c}": ("d", DEFAULT[0], 16384, CHARS[c], BIG, 700) for c in ("muff", "fuzz")}
SIGS.update({f"noise_{c}": ("n", DEFAULT[0], 16384, CHARS[c], BIG, 700) for c in ("muff", "clip")})
CURVES["negative"] = (-5, -5, -5, -5)


def run_curves(sets, sigs):
    """distortion_curve / distortion_mode_name on the simulator: {name: (mode name, cycles per call, y[65])}; and the
    distortion_sig.c cases: {name: output samples}."""
    w = tempfile.mkdtemp(prefix="fx_curve_")
    args = " ".join(" ".join(map(str, k)) for k in sets.values())
    cmds = "".join(f"bfin-elf-run /tmp/s {' '.join(map(str, a))} > /w/{n}.txt &\n" for n, a in sigs.items())
    open(os.path.join(w, "run.sh"), "w").write(
        "set -e; cd /tmp\n"
        "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror "
        "-c -o fx.o /src/distortion.c\n"
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -o c /src/test/distortion_curve.c fx.o\n"
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -o s /src/test/distortion_sig.c fx.o -lm\n"
        + cmds + f"bfin-elf-run /tmp/c {args} > /w/curve.txt\nwait\n")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v", f"{w}:/w",
                    "op1-bfin", "sh", "/w/run.sh"], check=True)
    lines = open(os.path.join(w, "curve.txt")).read().split("\n")
    sig = {n: np.loadtxt(os.path.join(w, n + ".txt")) for n in sigs}
    shutil.rmtree(w)
    out = {}
    for name, line in zip(sets, lines):
        f = line.split()
        out[name] = (f[0], int(f[1]), np.array(list(map(int, f[2:]))))
    return out, sig


def pure_in(n, f):
    """The driver's clean sine (fx_main.c sine_pure), to rounding."""
    inc = int(f * 1000 / 1000.0 / 44100.0 * 4294967296.0) & 0xFFFFFFFF
    ph = ((np.arange(n, dtype=np.uint64) * inc) & 0xFFFFFFFF).astype(np.uint32).view(np.int32)
    return np.round(16383.5 * np.sin(ph / 2147483648.0 * np.pi))


def spectrum(x):
    x = x[-N:].astype(float)
    return np.abs(np.fft.rfft(x)) / (N / 2), np.fft.rfftfreq(N, 1 / FS)


def harm(x, f0, k):
    s, fr = spectrum(x)
    return s[np.argmin(np.abs(fr - k * f0))]


def harms_db(x, f0, ks):
    h1 = harm(x, f0, 1)
    return [20 * np.log10(harm(x, f0, k) / h1 + 1e-12) for k in ks]


def thd_db(x, f0):
    h = [harm(x, f0, k) for k in range(1, int(20000 / f0) + 1)]
    return 10 * np.log10(sum(v * v for v in h[1:]) / h[0] ** 2)


def inharmonic_db(x, f0, hi):
    """Energy at non-harmonic frequencies (20 Hz .. hi) relative to the total harmonic energy. Exact bins."""
    s, fr = spectrum(x)
    p = s ** 2
    k = np.round(fr / f0)
    harmonic = (np.abs(fr - k * f0) < 1) & (k >= 1)
    sel = ~harmonic & (fr > 20) & (fr < hi)
    return 10 * np.log10(p[sel].sum() / p[harmonic & (fr < hi)].sum())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    cur, sig = run_curves(CURVES, SIGS)
    out, cyc = run_cases("distortion", CASES, keep=a.keep)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    n = len(out["dry"])
    ok = np.array_equal(out["dry"][:, 0], sine_in(n, 1000))
    ok &= np.array_equal(out["dry_s"][:, 0], sine_in(n, 1000)) and np.array_equal(out["dry_s"][:, 1],
                                                                                    sine_in(n, 1000, True))
    check(ok, "mix 0: output is the dry input, bit exact (mono and stereo)")

    dry = pure_in(len(out["thd_clip_0"]), 1000)
    for c in ("tube", "clip"):
        x = out[f"thd_{c}_0"][:, 0]
        lv = 20 * np.log10(harm(x, 1000, 1) / harm(dry, 1000, 1))
        t = thd_db(x, 1000)
        check(abs(lv) < 0.5 and t < -40, f"drive 0, {c}: level {lv:+.2f} dB, THD {t:.1f} dB: near-clean")

    thd = {c: [thd_db(out[f"thd_{c}_{kd}"][:, 0], 1000) for kd in DRIVES] for c in CHARS}
    print("     THD vs drive 0/8k/16k/24k/max: " + "; ".join(f"{c} " + "/".join(f"{v:.1f}" for v in t)
                                                      for c, t in thd.items()) + " dB")
    for c in ("tube", "clip"):
        t = thd[c]
        check(all(b > a - 1 for a, b in zip(t, t[1:])) and t[-1] > -10,
              f"drive range, {c}: THD rises to {t[-1]:.1f} dB at max (heavy saturation, > -10)")
    sq = out[f"thd_clip_{BIG}"][-N:, 0].astype(float)
    crest = np.abs(sq).max() / np.sqrt((sq ** 2).mean())
    check(crest < 1.6, f"clip at max drive: crest factor {crest:.2f} (square-like < 1.6; sine 1.41, square 1.0 "
          "before the fizz shelf)")

    h2t = harms_db(out["thd_tube_8192"][:, 0], 1000, [2])[0]
    h2c = harms_db(out["thd_clip_16384"][:, 0], 1000, [2])[0]
    check(h2t > -30 and h2c < -60, f"tube (drive 8192): 2nd harmonic {h2t:.1f} dB (clip: {h2c:.0f} dB, odd only)")

    fz = {kd: harms_db(out[f"thd_fuzz_{kd}"][:, 0], 1000, [2, 3]) for kd in DRIVES[1:]}
    check(all(v[0] > 0 for v in fz.values()), "fuzz: 2nd harmonic re the fundamental, drive 8k/16k/24k/max: "
          + "/".join(f"{v[0]:+.1f}" for v in fz.values()) + " dB (octave up: > 0)")

    mh = {kd: np.array(harms_db(out[f"thd_muff_{kd}"][:, 0], 1000, range(2, 12))) for kd in DRIVES}
    rich = int((mh[BIG] > -30).sum())
    odd = all(v[1] > v[0] and v[3] > v[2] for kd, v in mh.items() if kd)
    even = mh[8192][0] > -50 and mh[16384][0] > -50
    smooth = all(v[1] >= v[3] - 1 and v[3] >= v[5] - 1 for kd, v in mh.items() if kd)    # 3rd >= 5th >= 7th
    check(rich >= 4 and odd and even and smooth, "muff: harmonics 2-7 re the fundamental, drive 8k/16k/max: " +
          "; ".join("/".join(f"{x:.0f}" for x in mh[kd][:6]) for kd in (8192, 16384, BIG)) +
          f" dB; {rich} of 2-11 within 30 dB at max (rich), odd > even ({odd}), 2nd > -50 ({even}), odd series "
          f"decays smoothly ({smooth})")

    def env(x, t):
        i = int(t * FS)
        return rms_db(x[i - 1100:i + 1100])
    dec = {c: (env(sig[f"decay_{c}"], 0.25), env(sig[f"decay_{c}"], 0.85)) for c in ("muff", "fuzz")}
    din = 0.6 * 60
    md = dec["muff"][0] - dec["muff"][1]
    check(md < 20 and dec["muff"][1] - FSDB > -45, f"muff sustain: a 200 Hz note decaying {din:.0f} dB (0.25 -> 0.85 s) "
          f"comes out {md:.1f} dB lower, still at {dec['muff'][1] - FSDB:.1f} dBFS (no gate; fuzz: "
          f"{dec['fuzz'][0] - dec['fuzz'][1]:.1f} dB lower, at {dec['fuzz'][1] - FSDB:.1f} dBFS)")

    def bands(x):
        x = x[-32768:]
        seg = x.reshape(-1, 4096) * np.hanning(4096)
        p = (np.abs(np.fft.rfft(seg, axis=1)) ** 2).mean(axis=0)
        fr = np.fft.rfftfreq(4096, 1 / FS)
        return [10 * np.log10(p[(fr >= lo) & (fr < hi)].mean()) for lo, hi in ((80, 250), (700, 1400), (5000, 8000))]
    bm, bc = bands(sig["noise_muff"]), bands(sig["noise_clip"])
    r = [m - c for m, c in zip(bm, bc)]
    check(r[1] - r[0] < -4 and r[0] > -3, "muff scoop (noise, re clip at the same drive): 80-250 Hz "
          f"{r[0]:+.1f}, 0.7-1.4 kHz {r[1]:+.1f}, 5-8 kHz {r[2]:+.1f} dB (mid dip {r[1] - r[0]:+.1f} re the lows "
          "< -4; lows kept > -3)")

    hs = np.array([harm(out[f"thd_fold_{BIG}"][:, 0], 1000, k) for k in range(1, 20)])
    hi = 10 * np.log10((hs[4:] ** 2).sum() / (hs[1:4] ** 2).sum())
    dense = int((hs[4:] > hs.max() / 10).sum())
    tf = thd["fold"]
    check(hi > 0 and dense >= 6 and max(tf) > 3, f"fold at max: harmonics 5-19 {hi:+.1f} dB re 2-4, {dense} of them "
          f"within 20 dB of the strongest (dense upper harmonics); THD peaks at {max(tf):+.1f} dB")

    cm = {kd: inharmonic_db(out[f"thd_crush_{kd}"][:, 0], 1000, 20000) for kd in DRIVES}
    check(cm[BIG] > -15 and cm[16384] > -40 and cm[0] < -40, "crush: inharmonic (aliasing/imaging) energy, drive "
          "0/8k/16k/24k/max: " + "/".join(f"{v:.1f}" for v in cm.values()) + " dB (rises: rate and bits fall)")

    lv = {c: [rms_db(out[f"thd_{c}_{kd}"][-N:, 0]) - rms_db(dry[-N:]) for kd in DRIVES] for c in CHARS}
    lo, hi_ = min(v for vs in lv.values() for v in vs), max(v for vs in lv.values() for v in vs)
    check(lo > -6 and hi_ < 4, "loudness vs dry, drive 0/8k/16k/24k/max: " +
          "; ".join(f"{c} " + "/".join(f"{v:+.1f}" for v in vs) for c, vs in lv.items()) + " dB (window -6 .. +4)")

    for name in ("filter_lo", "filter_hi"):
        x = out[name][:, 0]
        print(f"     {name}: level {rms_db(x[-N:]) - rms_db(dry[-N:]):+.1f} dB at 1 kHz (blue 0, clip)")

    # fold at 5 kHz: up to 8 folds per side, most sample pairs straddle a fold, more than FIXMAX per block, so the
    # rest keep the plain mean: a guard, not a target
    for c, lim5, lim2 in (("tube", -30, -40), ("muff", -20, -30), ("clip", -30, -40), ("fold", -5, -24)):
        a5 = inharmonic_db(out[f"alias_{c}"][:, 0], 5000, 20000)
        a2 = inharmonic_db(out[f"alias2k_{c}"][:, 0], 2000, 20000)
        check(a5 < lim5 and a2 < lim2, f"aliasing at max drive, {c}: inharmonic {a5:.1f} dB (5 kHz, limit {lim5}), "
              f"{a2:.1f} dB (2 kHz, limit {lim2})")

    dcs = {c: out[f"dc_{c}"][-N:, 0].mean() for c in CHARS}
    check(all(abs(v) < 100 for v in dcs.values()), "DC at max drive, 100 Hz: " +
          ", ".join(f"{c} {v:+.0f}" for c, v in dcs.items()) + " LSB (blocker 7 Hz)")
    gt = {c: np.abs(out[f"gate_{c}"][-N:, 0]).max() for c in CHARS}
    check(all(v < 50 for v in gt.values()), "after the input stops (one click, then silence, 0.6 s): peak " +
          ", ".join(f"{c} {v}" for c, v in gt.items()) + " LSB: no stuck DC or noise")

    # a click: a sample step larger than any the settings passed through make in steady state
    for name in JUMPS:
        j = out[f"jump_{name}"]
        tr = np.abs(np.diff(j[64 * 200 - 64:64 * 240], axis=0)).max()
        steady = [np.abs(np.diff(out[r][64 * 200:], axis=0)).max() for r in REFS[name]]
        check(tr <= 1.25 * max(steady) + 50, f"jump {name}: largest step {tr} around the jump; steady, over the "
              f"{len(steady)} settings passed through: {min(steady)} .. {max(steady)}: no click")

    sat = [k for k, x in out.items() if k.startswith("range") and np.mean(np.abs(x) >= 32700) > 0.001]
    pk = max(np.abs(x).max() for k, x in out.items() if k.startswith("range"))
    check(not sat, f"range: 32 corner cases (-6 dBFS in), none saturating (peak {pk})"
          + (f": {sat[:4]}" if sat else ""))

    # screen functions
    xs = np.r_[-32768 + 1024 * np.arange(64), 32767]
    names_ok = all(cur[f"{c}_0"][0] == NAMES[c] for c in CHARS) and cur["negative"][0] == "TUBE"
    order = list(CHARS)
    for i, f in enumerate(FADES):
        names_ok &= cur[f"name_{f}_-1"][0] == NAMES[order[i]] and cur[f"name_{f}_1"][0] == NAMES[order[i + 1]]
    check(names_ok, "mode names at the plateaus: " + ", ".join(cur[f"{c}_0"][0] for c in CHARS) +
          "; they switch at the crossfade midpoints " + ", ".join(str(v) for v in FADES.values()))
    mono = all(np.all(np.diff(cur[f"{c}_{kd}"][2]) >= 0) for c in ("tube", "muff", "clip") for kd in DRIVES)
    turns = [int((np.diff(np.sign(np.diff(cur[f"fold_{kd}"][2]))) != 0).sum()) for kd in DRIVES]
    levels = [len(set(cur[f"crush_{kd}"][2])) for kd in DRIVES]
    check(mono and turns[-1] >= 6 and levels[-1] <= 12 and levels[0] > 40,
          f"curve: monotonic for tube, muff and clip ({mono}); fold slope turns, drive 0..max: {turns}; crush distinct "
          f"levels of 65 points: {levels}")
    full = max(np.abs(v[2]).max() for v in cur.values())
    check(full <= 32767 and cur["negative"][2][32] == 0, f"curve: within +-32767 (peak {full}), 0 at x = 0")
    err = []
    for c in ("tube", "clip"):
        for kd in STATIC_KD:
            y = out[f"static_{c}_{kd}"][-N:, 0].astype(float)
            x = pure_in(len(out[f"static_{c}_{kd}"]), 100)[-N:]
            cv = np.interp(x, xs, cur[f"{c}_{kd}"][2])
            lag = max(range(12), key=lambda L: np.dot(np.roll(y, -L), cv))
            y = np.roll(y, -lag)
            d = (y - y.mean()) - (cv - cv.mean())
            pts = [np.abs(d[np.abs(x - v) < 300]).mean() for v in (-16000, -8000, 0, 8000, 16000)]
            err.append((c, kd, lag, max(pts)))
    worst_err = max(e[3] for e in err)
    check(worst_err < 1500, "curve vs the DSP's static response (100 Hz, mean |error| at x = 0, +-8000, +-16000): "
          + ", ".join(f"{c}/{kd} {e:.0f} (lag {lag})" for c, kd, lag, e in err) + " LSB (limit 1500)")
    # MUFF: the coupling high-pass, the low-pass between the stages and the scoop shape the waveform, so compare
    # levels (rms of the DSP's output vs the curve applied to the input, 300 Hz)
    md = []
    for kd in STATIC_KD:
        y = out[f"static_muff_{kd}"][-N:, 0].astype(float)
        x = pure_in(len(out[f"static_muff_{kd}"]), 300)[-N:]
        cv = np.interp(x, xs, cur[f"muff_{kd}"][2])
        md.append(rms_db(y - y.mean()) - rms_db(cv - cv.mean()))
    check(all(abs(v + 3.5) < 2 for v in md), "curve vs DSP, muff (300 Hz, rms): " + ", ".join(
          f"{kd} {v:+.1f}" for kd, v in zip(STATIC_KD, md)) + " dB (the scoop is -3.5 dB at 300 Hz: within 2 of it)")
    c1 = max(v[1] for k, v in cur.items() if not k.startswith(("blend", "name_")))
    c2 = max(v[1] for k, v in cur.items() if k.startswith(("blend", "name_")))
    check(c1 <= CURVE_BUDGET and c2 <= CURVE_BLEND, f"curve: {c1} cycles per call at most on a plateau (budget "
          f"{CURVE_BUDGET}), {c2} in a crossfade (two shapes; budget {CURVE_BLEND})")

    typ = {k: cyc["cpu_" + k][1] for k in TYPICAL if k != "muff_green_1k"}
    tmax = max(typ.values())
    check(tmax <= BUDGET, "cycles per stereo call, typical: " + ", ".join(f"{k} {v}" for k, v in typ.items())
          + f" ({tmax / 128:.0f}/sample/channel at most); budget {BUDGET}")
    mg = cyc["cpu_muff_green_1k"][1]
    check(mg <= MUFF_GREEN_BUDGET, f"cycles per stereo call, MUFF with the green filter on (16000, 20480): {mg}; "
          f"budget {MUFF_GREEN_BUDGET}")
    st = max(c[1] for k, c in cyc.items() if CASES[k][0] == "s")
    mo = max(c[1] for k, c in cyc.items() if CASES[k][0] == "m")
    peak = max(c[0] for c in cyc.values())
    top = sorted(cyc.items(), key=lambda kv: -kv[1][1] * (2 if CASES[kv[0]][0] == "m" else 1))[:4]
    check(peak <= WORST, f"cycles, worst: stereo mean {st}, mono mean {mo}, peak {peak} (bound {WORST}); heaviest "
          "(mono doubled): " + ", ".join(f"{k} {c[1]}" for k, c in top))

    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
