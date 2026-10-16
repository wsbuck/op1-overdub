#!/usr/bin/env python3
"""Simulator checks for the ping-pong delay FX (src/slot/pingpong.c) before flashing.

  python3 src/slot/test/pingpong_check.py [--keep DIR]

Runs every case on the Blackfin simulator (test/pingpong_main.c, fxsim.py's flags; the line buffer,
test/pingpong_ram.c, starts full of garbage; te_tempo_spb is set per case) and checks: dry at mix 0, echo times and
the L/R alternation (click input), echo levels, decay at max feedback with every tone, silence until the first echo
(no stale buffer), clicks on time-knob jumps, the synced divisions at several tempos (echo times against TE's
samples-per-beat word), the fold at slow tempos (the line's length), invalid tempo words, the glide on tempo changes,
no zipper on detent-by-detent knob turns (free, synced, across the zones), range (no saturation) and CYCLES per
64-frame call.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

from fxsim import FS, HERE, SLOT, rms_db, sine_in

BIG = 32767
CLICK = 12000                                       # fx_main.c: FREQ_MHZ 0 = one sample of CLICK at frame 0
DEFAULT = (28672, 14000, 12000, 10000)              # 1/8. (375 ms at 120 BPM), g 0.4, a little dark, 30 % wet
BUDGET = 25000                                      # cycles per stereo call (64 frames); voices use ~460 k
EXP2 = [0, 2902, 5932, 9096, 12400, 15850, 19454, 23216, 27146, 31249, 35534, 40009, 44682, 49562, 54658, 59979,
        65536]


PP_SYNC0, PP_D_MIN, PP_D_MAX = 20992, 882, 65280
DIVS = [(1, 3, 0), (1, 1, 1), (1, 2, 0), (1, 0, 1), (3, 3, 0), (1, 1, 0), (2, 0, 1), (3, 2, 0), (1, 0, 0), (2, -1, 1),
        (3, 1, 0), (2, 0, 0)]                       # pingpong.h pp_div: beats a * 2^-p (/ 3)
NAMES = ["1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.", "1/4", "1/2T", "1/4.", "1/2"]
BEATS = [a * 2.0 ** -p / (3 if t else 1) for a, p, t in DIVS]
SPB_DEF = 22050 << 14


def spb_word(bpm):
    """TE's tempo word (metronome.cpp FUN_019de8f0): (int)(2646000.0f / bpm * 16384.0f), in float32."""
    return int(np.float32(np.float32(2646000.0) / np.float32(bpm)) * np.float32(16384.0))


def detent(j):
    """The raw blue value on division j's detent (21 + j)."""
    return min(PP_SYNC0 + 1024 * j + 512, 32767)


def pp_target(k, spb):
    """pingpong.h pp_target: (delay Q15 samples, division played or -1, folds), bit exact."""
    if k < PP_SYNC0:
        e = min(max(k, 0) * 39322 >> 12, 6 << 15)
        o, f = e >> 15, e & 32767
        if o == 6:
            return PP_D_MIN * 64 << 15, -1, 0
        i, t = f >> 11, f & 2047
        m = EXP2[i] + (((EXP2[i + 1] - EXP2[i]) * t) >> 11)
        return ((PP_D_MIN * (65536 + m)) >> 1) << o, -1, 0
    j = min((k - PP_SYNC0) >> 10, len(DIVS) - 1)
    a, p, t = DIVS[j]
    u = spb * a & 0xFFFFFFFF
    u = u // 3 if t else u
    u = u >> p if p >= 0 else u << 1
    fold = 0
    while u > PP_D_MAX << 14:
        u >>= 1
        fold += 1
    return u << 1, j - 3 * fold, fold


def time_q15(k):
    return pp_target(k, SPB_DEF)[0]


def gain(k):
    return (k * 31130) >> 15


def delayed(x, d):
    """pingpong.c tap on a line holding x since t = 0 (0 before), bit exact."""
    n, f = d >> 15, d & 32767
    xp = np.concatenate([np.zeros(n + 1, dtype=np.int64), x])
    a, b = xp[1:1 + len(x)], xp[:len(x)]
    return a + (((b - a) * f) >> 15)


T_ECHO = 6000                                       # free, ~73 ms
T_MAX = 20480                                       # free, 1.28 s (the longest free time)
T_MAX_BLOCKS = (time_q15(T_MAX) >> 15) // 64 + 40

# name: (mode, k0, k1, k2, k3, blocks, input Hz (0 = click)[, switch block, j0, j1, j2, j3[, spb0, tsw, spb1, ramp]])
CASES = {
    "dry_s": ("s", 22000, BIG, 0, 0, 40, 1000),
    "dry_m": ("m", 22000, BIG, BIG, 0, 40, 1000),
    "echo_s": ("s", T_ECHO, 16384, 16384, BIG, 300, 0),
    "echo_m": ("m", T_ECHO, 16384, 16384, BIG, 300, 0),
    "fresh_s": ("s", T_MAX, 0, 16384, BIG, T_MAX_BLOCKS, 1000),
    "fresh_m": ("m", T_MAX, 0, 16384, BIG, T_MAX_BLOCKS, 1000),
    "jump_up": ("s", 0, 0, 16384, BIG, 900, 200, 300, T_MAX, 0, 16384, BIG),
    "jump_down": ("s", T_MAX, 0, 16384, BIG, 1400, 200, 1000, 0, 0, 16384, BIG),
    "jump_sync": ("s", 0, 0, 16384, BIG, 1400, 200, 300, detent(11), 0, 16384, BIG),
    "jump_fb": ("s", 0, 16384, 16384, BIG, 900, 200, 300, 20000, 16384, 16384, BIG),
}
for name, k2 in (("flat", 16384), ("dark", 0), ("bright", BIG)):
    CASES[f"decay_{name}"] = ("s", 0, BIG, k2, BIG, 1500, 0)
CASES["decay_m"] = ("m", 0, BIG, 16384, BIG, 1500, 0)
for f in (200, 1000, 5000):
    CASES[f"level_{f}"] = ("s",) + DEFAULT + (700, f)
# Synced: one click echo (feedback 0, wet) at each division and tempo; mono, and stereo at 120 BPM.
SYNC_BPM = (60, 90, 120, 133.3, 200)
SYNC_DIVS = (2, 3, 5, 7, 8, 10, 11)                 # 1/16 1/8T 1/8 1/8. 1/4 1/4. 1/2
for bpm in SYNC_BPM:
    for j in SYNC_DIVS:
        n = (pp_target(detent(j), spb_word(bpm))[0] >> 15) // 64 + 4
        CASES[f"sync_{bpm}_{j}"] = ("m", detent(j), 0, 16384, BIG, n, 0, -1, 0, 0, 0, 0, spb_word(bpm), -1, 0, 0)
for j in SYNC_DIVS:
    n = 2 * (pp_target(detent(j), SPB_DEF)[0] >> 15) // 64 + 4
    CASES[f"sync_s_{j}"] = ("s", detent(j), 16384, 16384, BIG, n, 0, -1, 0, 0, 0, 0, SPB_DEF, -1, 0, 0)
# Slow tempos: what does not fit the line plays its half (or quarter).
FOLDS = [(40, 11), (40, 10), (40, 9), (40, 8), (41, 8), (60, 11), (80, 11), (40, 7)]
for bpm, j in FOLDS:
    n = (pp_target(detent(j), spb_word(bpm))[0] >> 15) // 64 + 4
    CASES[f"fold_{bpm}_{j}"] = ("m", detent(j), 0, 16384, BIG, n, 0, -1, 0, 0, 0, 0, spb_word(bpm), -1, 0, 0)
# Invalid tempo words: 120 BPM before any valid one; after one, it stays (90 BPM read in block 0, then 0 / garbage).
INVALID = {"inv_zero": (0, -1, 0), "inv_low": (5, -1, 0), "inv_neg": (-361267200, -1, 0),
           "inv_high": (0x7FFFFFFF, -1, 0), "inv_after_valid": (spb_word(90), 1, 0),
           "inv_after_valid_garbage": (spb_word(90), 1, 123456)}
for name, (s0, tsw, s1) in INVALID.items():
    want = spb_word(90) if tsw >= 0 else SPB_DEF
    n = (pp_target(detent(8), want)[0] >> 15) // 64 + 4
    CASES[name] = ("m", detent(8), 0, 16384, BIG, n, 0, -1, 0, 0, 0, 0, s0, tsw, s1, 0)
TSW = 800                                           # after 1.16 s: the line holds input at every delay
# Tempo changes while a 200 Hz sine plays through the wet line (feedback 0): 1/8 at 120 -> 125 BPM (a nudge) and
# 1/4 at 120 -> 60 BPM (an octave), stereo; and 1/8. at 120 -> 220 BPM with feedback (mono).
GLIDES = {"glide_nudge": ("s", 5, 0, 120, 125), "glide_octave": ("s", 8, 0, 120, 60), "glide_fb": ("m", 7, 20000, 120, 220)}
for name, (mode, j, g, b0, b1) in GLIDES.items():
    CASES[name] = (mode, detent(j), g, 16384, BIG, 2400, 200, -1, 0, 0, 0, 0, spb_word(b0), TSW, spb_word(b1), 0)
# Knob turned one detent at a time (every 4 blocks, ~6 ms: a fast turn), 200 Hz wet: free, synced, across the zones.
RSW = 1000                                          # after 1.45 s (the line full: no read before t = 0)
RAMPS = {"ramp_free": (2000, 19000), "ramp_sync": (detent(0), detent(11)), "ramp_cross": (16000, detent(4)),
         "ramp_down": (detent(11), 4000)}
for name, (k0, k1) in RAMPS.items():
    CASES[name] = ("s", k0, 0, 16384, BIG, 1600, 200, RSW, k1, 0, 16384, BIG, SPB_DEF, -1, 0, 4)
for k in range(16):
    ks = tuple(BIG if k >> b & 1 else 0 for b in range(4))
    for f in (100, 5000):
        CASES[f"range_{k:x}_{f}"] = ("s",) + ks + (1000, f)


def run_cases(cases, keep=None):
    """fxsim.run_cases with pingpong_main.c (the tempo word and knob ramps): every case padded to its 16 arguments."""
    w = tempfile.mkdtemp(prefix="fx_pingpong_")
    cmds = []
    for name, c in cases.items():
        mode, k0, k1, k2, k3, blocks, f, *rest = c
        rest = list(rest) + [-1, 0, 0, 0, 0, SPB_DEF, -1, 0, 0][len(rest):]
        args = f"{mode} {k0} {k1} {k2} {k3} {blocks} {int(f * 1000)} " + " ".join(map(str, rest))
        cmds.append(f"bfin-elf-run /tmp/m {args} > /w/{name}.txt 2> /w/{name}.err")
    open(os.path.join(w, "cmds"), "w").write("\n".join(cmds) + "\n")
    open(os.path.join(w, "run.sh"), "w").write(
        "set -e; cd /tmp\n"
        "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror "
        "-c -o fx.o /src/pingpong.c\n"
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -o m /src/test/pingpong_main.c fx.o /src/test/pingpong_ram.c\n"
        "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v", f"{w}:/w",
                    "op1-bfin", "sh", "/w/run.sh"], check=True)
    out, cyc = {}, {}
    for name in cases:
        x = np.loadtxt(os.path.join(w, name + ".txt"), dtype=np.int64)
        out[name] = x if x.ndim == 2 else x[:, None]
        e = open(os.path.join(w, name + ".err")).read().split()
        cyc[name] = (int(e[2]), int(e[4]))
    if keep:
        os.makedirs(keep, exist_ok=True)
        for f in os.listdir(w):
            if f.endswith((".txt", ".err")):
                shutil.copy(os.path.join(w, f), keep)
    shutil.rmtree(w)
    return out, cyc


def centroid(x):
    """Where a click echo sits: the weighted mean of its (two, interpolated) samples."""
    i = np.nonzero(x)[0]
    return float((i * x[i]).sum() / x[i].sum()) if len(i) else -1.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    out, cyc = run_cases(CASES, keep=a.keep)
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
    dq = time_q15(T_MAX)
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

    for name, sw in (("jump_up", 300), ("jump_down", 1000), ("jump_fb", 300), ("jump_sync", 300)):
        j = out[name]
        tail = j[64 * (sw - 20):]
        slope = np.abs(np.diff(tail, axis=0)).max()
        dry_slope = np.abs(np.diff(sine_in(len(j), 200))).max()
        lim = 1.6 if name != "jump_fb" else 2.6
        check(slope < lim * dry_slope, f"{name}: time knob jump, wet: largest step {slope} (dry sine {dry_slope}, "
              f"limit {lim}x): tape glide, no click")

    # Synced divisions: the echo of a click lands on TE's beat word x the division (bit exact against pingpong.h's
    # map), within a sample of the exact time 60 / bpm x beats.
    for bpm in SYNC_BPM:
        bad, worst = [], 0.0
        for j in SYNC_DIVS:
            dq, div, fold = pp_target(detent(j), spb_word(bpm))
            got = centroid(out[f"sync_{bpm}_{j}"][:, 0])
            exact = FS * 60 / bpm * BEATS[j] / 2 ** fold
            worst = max(worst, abs(got - exact))
            if abs(got - dq / 32768) > 0.01 or abs(got - exact) > 1 or div != j - 3 * fold:
                bad.append(f"{NAMES[j]}: {got:.2f} (map {dq / 32768:.2f}, exact {exact:.2f})")
        folded = [NAMES[j] for j in SYNC_DIVS if pp_target(detent(j), spb_word(bpm))[2]]
        check(not bad, f"{bpm} BPM: " + ", ".join(f"{NAMES[j]} {pp_target(detent(j), spb_word(bpm))[0] / 32768:.0f}"
              for j in SYNC_DIVS) + f" samples; within {worst:.3f} of 60/bpm x beats"
              + (f"; folded: {folded}" if folded else "") + (f": {bad}" if bad else ""))
    bad = []
    for j in SYNC_DIVS:
        x = out[f"sync_s_{j}"]
        dq = pp_target(detent(j), SPB_DEF)[0] / 32768
        gl, gr = centroid(x[:int(1.5 * dq), 0]), centroid(x[:int(2.5 * dq), 1])
        if abs(gl - dq) > 0.01 or abs(gr - 2 * dq) > 0.02:
            bad.append(f"{NAMES[j]}: L {gl:.2f} R {gr:.2f} (expect {dq:.2f}, {2 * dq:.2f})")
    check(not bad, "stereo, 120 BPM: every division's first echo on L at d, the next on R at 2d" + (f": {bad}" if bad else ""))

    # Slow tempos: a division longer than the line plays its half (quarter), never beyond PP_D_MAX.
    bad, rows = [], []
    for bpm, j in FOLDS:
        dq, div, fold = pp_target(detent(j), spb_word(bpm))
        got = centroid(out[f"fold_{bpm}_{j}"][:, 0])
        full = FS * 60 / bpm * BEATS[j]
        rows.append(f"{NAMES[j]}@{bpm} {full:.0f} -> {got:.0f}" + (f" ({NAMES[div]})" if fold else ""))
        if abs(got - full / 2 ** fold) > 1 or got > PP_D_MAX or (full <= PP_D_MAX) != (fold == 0):
            bad.append(f"{NAMES[j]} at {bpm}: {got:.1f}")
    check(not bad, "fold at slow tempos (line " + f"{PP_D_MAX}): " + ", ".join(rows) + (f": {bad}" if bad else ""))

    # Invalid tempo words.
    bad = []
    for name, (s0, tsw, s1) in INVALID.items():
        want = pp_target(detent(8), spb_word(90) if tsw >= 0 else SPB_DEF)[0] / 32768
        got = centroid(out[name][:, 0])
        if abs(got - want) > 0.01:
            bad.append(f"{name}: {got:.1f} (expect {want:.1f})")
    check(not bad, "invalid tempo words (0, 5, negative, 2^31 - 1): 120 BPM until a valid word; after one (90 BPM) "
          "0 or garbage keep it" + (f": {bad}" if bad else ""))

    # Tempo changes: no click, the delay glides (tape), then settles on the new division.
    for name, (mode, j, g, b0, b1) in GLIDES.items():
        x = out[name]
        m = len(x)
        src = (sine_in(m, 200) + sine_in(m, 200, True)) >> 1 if mode == "s" else sine_in(m, 200)
        dry_slope = np.abs(np.diff(sine_in(m, 200))).max()
        slope = np.abs(np.diff(x[64 * (TSW - 20):], axis=0)).max()
        d0, d1 = pp_target(detent(j), spb_word(b0))[0], pp_target(detent(j), spb_word(b1))[0]
        msg = f"{name}: {NAMES[j]} {b0} -> {b1} BPM ({d0 / 32768:.0f} -> {d1 / 32768:.0f} samples): largest step " \
              f"{slope} (dry {dry_slope})"
        if g:   # the repeats sum (comb gain up to 1 / (1 - g)), so the slope says little: a click is a corner, so
            # compare the curvature per 10 ms window (largest second difference / peak) after the change with before
            xf = x[:, 0].astype(float)
            d2 = np.abs(np.diff(xf, 2))
            r = [(d2[i:i + 441].max() / max(np.abs(xf[i:i + 441]).max(), 1), i) for i in range(0, len(d2) - 441, 441)]
            pre = max(v for v, i in r if 64 * (TSW - 200) <= i < 64 * TSW)
            post = max(v for v, i in r if i >= 64 * TSW)
            check(post <= 2.5 * pre, f"{name}: {NAMES[j]} {b0} -> {b1} BPM ({d0 / 32768:.0f} -> {d1 / 32768:.0f} "
                  f"samples), feedback on: curvature / peak {post:.4f} after, {pre:.4f} before (a 1.5x pitch rise "
                  "allows 2.25x): no click")
            continue
        err = np.abs(x[:, 0] - delayed(src, d1))
        before = np.abs(x[64 * (TSW - 100):64 * TSW, 0] - delayed(src, d0)[64 * (TSW - 100):64 * TSW]).max()
        late = err[-64 * 100:].max()
        settle = int(np.nonzero(err[64 * TSW:] > 80)[0].max()) if (err[64 * TSW:] > 80).any() else 0
        check(slope < 1.6 * dry_slope and before == 0 and late <= 80 and settle > 64 * 4,
              msg + f"; exact old delay before, glides {settle / FS * 1000:.0f} ms, then within {late} LSB of the new "
              "(< 0.13 sample)")

    # Knob turned detent by detent: no zipper (no click, no buzz at the block / glide-step rates).
    for name, (k0, k1) in RAMPS.items():
        x = out[name][64 * (RSW - 10):, 0].astype(float)
        dry_slope = np.abs(np.diff(sine_in(len(x), 200))).max()
        slope = np.abs(np.diff(x)).max()
        sp = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
        fr = np.fft.rfftfreq(len(x), 1 / FS)
        hf = 10 * np.log10(sp[fr > 2500].sum() / sp.sum())
        check(slope < 1.6 * dry_slope and hf < -50, f"{name}: blue {k0} -> {k1} one detent per 4 blocks: largest step "
              f"{slope} (dry {dry_slope}), energy above 2.5 kHz {hf:.0f} dB (no zipper)")

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
