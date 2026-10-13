#!/usr/bin/env python3
"""Checks for the custom-wavetable builder (src/engine/wt_import.h) in the Blackfin simulator (or --host).

  python3 src/engine/test/wt_import_check.py [--host] [--keep DIR]

Feeds synthetic samples (as a synth sampler slot would hold them) through wt_import_main.c and checks: detected
period, every level band-limited to its harmonic count (as gen_wt_data.py), adjacent-sample deltas below 32767 (wt.c's
16-bit interpolation), 0.9 FS normalisation, harmonic 1 in sine phase, agreement with a float numpy model of the
same algorithm, and CYCLES per step. Exits non-zero if a check fails.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

FS = 44100
FRAMES, LEVELS, H0 = 12, 9, 256
SIZES = [max(8 * (H0 >> L), 32) for L in range(LEVELS)]
OFF = [sum(SIZES[:L]) for L in range(LEVELS)]
STRIDE = sum(SIZES)
HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(HERE)
FIXED = 2048 << 16


def q15(x):
    return np.round(np.clip(x, -1, 1) * 32767).astype("<i2")


def saw(f, n, amp=0.8):
    """Band-limited saw (harmonics below 20 kHz), as a recorded or resampled source would be."""
    t = np.arange(n) / FS
    k = np.arange(1, int(20000 / f) + 1)
    return amp * 2 / np.pi * sum(np.sin(2 * np.pi * kk * f * t) / kk for kk in k)


def cases():
    rng = np.random.default_rng(7)
    c = {}
    c["saw220"] = (q15(saw(220, FS * 2)), 220)
    t = np.arange(FS) / FS
    c["sine440"] = (q15(0.7 * np.sin(2 * np.pi * 440 * t)), 440)
    c["saw30"] = (q15(saw(30, FS * 3)), 30)                     # P = 1470: droop compensation path
    # pulse 110 Hz whose width sweeps 50 % -> 10 % over 3 s (frames should differ)
    tt = np.arange(FS * 3) / FS
    w = 0.5 - 0.4 * tt / tt[-1]
    c["pwm110"] = (q15(0.6 * np.where((110 * tt) % 1 < w, 1.0, -1.0)), 110)
    # Serum-style: 64 frames x 2048, sine -> saw with random phases per frame (a typical exported table)
    frames = []
    k = np.arange(1, 400)
    for i in range(64):
        a = (i / 63) / k
        a[0] = 1
        ph = rng.random(len(k)) * 2 * np.pi * (i / 63)
        n = np.arange(2048)
        frames.append((a[:, None] * np.sin(2 * np.pi * k[:, None] * n / 2048 + ph[:, None])).sum(0))
    s = np.concatenate(frames)
    c["serum64"] = (q15(0.9 * s / np.abs(s).max()), FS / 2048)
    c["noise"] = (q15(0.3 * rng.standard_normal(FS)), None)      # unpitched: fixed 2048 slices
    c["silence"] = (np.zeros(FS, "<i2"), None)
    return c


def build(host, workdir, names):
    if host:
        exe = os.path.join(workdir, "wti")
        subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror", "-o", exe,
                        os.path.join(HERE, "wt_import_main.c")], check=True)
        for n in names:
            with open(os.path.join(workdir, n + ".raw"), "rb") as i, open(os.path.join(workdir, n + ".bank"), "wb") as o, \
                    open(os.path.join(workdir, n + ".err"), "wb") as e:
                subprocess.run([exe, "0", "264600"], stdin=i, stdout=o, stderr=e)
        return
    script = "set -e; cd /tmp\n" \
        "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -msim -O2 -Wall -Wextra -Werror -o m /src/test/wt_import_main.c\n" + \
        "".join(f"bfin-elf-run /tmp/m 0 264600 < /w/{n}.raw > /w/{n}.bank 2> /w/{n}.err || true\n" for n in names)
    open(os.path.join(workdir, "run.sh"), "w").write(script)
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{ENGINE}:/src:ro",
                    "-v", f"{workdir}:/w", "op1-bfin", "sh", "/w/run.sh"], check=True)


def model(x, p_q16, table=False):
    """Float model of wt_import.h for the given period: (FRAMES, STRIDE) float array scaled like the output."""
    x = x.astype(float)
    p = p_q16 / 65536
    pint = p_q16 >> 16
    need = pint + 3
    span = (len(x) - need) // (FRAMES - 1)
    kmax = min(256, (pint * 29) >> 6)
    out = np.zeros((FRAMES, STRIDE))
    for f in range(FRAMES):
        start = span * f
        if p_q16 == FIXED:                       # buffer grid (window starts at 0 here)
            s = (start + 1024) & ~2047
            while s + 2051 > len(x) and s >= 2048:
                s -= 2048
            if s + 2051 <= len(x):
                start = s
        if not table and p_q16 != FIXED:         # wti_cut
            n = pint if len(x) - start > 2 * pint + 3 else max(len(x) - start - pint - 3, 0)
            i = np.arange(n) + start
            v = np.abs(x[i] - x[i + pint]) + np.abs(x[i + 1] - x[i])
            start += int(np.argmin(v)) if n else 0
        seg = x[start:start + pint + 3]
        pos = np.arange(2048) * p / 2048
        y = np.interp(pos, np.arange(len(seg)), seg)
        e = np.interp(p, np.arange(len(seg)), seg)
        if not table:
            y -= (e - seg[0]) * np.arange(2048) / 2048
        c = np.fft.rfft(y) / 2048
        c[0] = 0
        c[kmax + 1:] = 0
        k = np.arange(len(c))
        if pint <= 1536:
            c[1:kmax + 1] /= np.sinc(k[1:kmax + 1] / p) ** 2
        l1 = np.abs(c.real) + np.abs(c.imag)
        if kmax and l1[1] > l1[1:].max() / 32:
            c = c * np.exp(1j * k * (-np.pi / 2 - np.angle(c[1])))
        lv = []
        for L in range(LEVELS):
            h = min(H0 >> L, kmax)
            s = np.zeros(SIZES[L] // 2 + 1, complex)
            s[1:h + 1] = c[1:h + 1]
            lv.append(np.fft.irfft(s, SIZES[L]) * SIZES[L])
        lv = np.concatenate(lv)
        pk = np.abs(lv).max()
        out[f] = lv * 29490 / pk if pk else lv
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", action="store_true", help="build with the host cc instead of the simulator")
    ap.add_argument("--keep")
    a = ap.parse_args()
    workdir = a.keep or tempfile.mkdtemp(prefix="wti_check_")
    os.makedirs(workdir, exist_ok=True)
    cs = cases()
    for n, (x, _) in cs.items():
        x.tofile(os.path.join(workdir, n + ".raw"))
    build(a.host, workdir, list(cs))
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    worst = {"detect": 0, "frame": 0, "total": 0}
    for n, (x, f0) in cs.items():
        err = open(os.path.join(workdir, n + ".err")).read().split()
        p_q16, pitched = int(err[1]), int(err[3])
        detect, fmax, total, status = int(err[6]), int(err[8]), int(err[10]), int(err[12])
        worst["detect"] = max(worst["detect"], detect)
        worst["frame"] = max(worst["frame"], fmax)
        worst["total"] = max(worst["total"], total)
        bank = np.fromfile(os.path.join(workdir, n + ".bank"), "<i2").astype(float).reshape(FRAMES, STRIDE)
        if f0 and pitched == 2:
            f0 = FS / 2048
        print(f"-- {n}: period {p_q16 / 65536:.3f} pitched {pitched} cycles detect {detect} frame max {fmax} "
              f"total {total}")
        check(status == 0, f"{n}: built")
        if f0:
            check(abs(p_q16 / 65536 - FS / f0) < 0.002 * FS / f0,
                  f"{n}: period {p_q16 / 65536:.3f} vs {FS / f0:.3f}")
        if n == "silence":
            check(not bank.any(), "silence: all frames zero")
            continue
        # band limit, normalisation, deltas, sine phase
        leak, dmax, peaks, phase = -999.0, 0, [], 0.0
        for f in range(FRAMES):
            fr = bank[f]
            peaks.append(np.abs(fr).max())
            for L in range(LEVELS):
                lv = fr[OFF[L]:OFF[L] + SIZES[L]]
                m = np.abs(np.fft.rfft(lv))
                h = H0 >> L
                if m.max() > 0:
                    leak = max(leak, 20 * np.log10(m[h + 1:].max() / m.max() + 1e-12))
                dmax = max(dmax, np.abs(np.diff(np.append(lv, lv[0]))).max())
            c = np.fft.rfft(fr[:2048])
            if np.abs(c[1]) > np.abs(c[1:]).max() / 32:
                phase = max(phase, abs(np.angle(c[1]) + np.pi / 2))
        check(leak < -60, f"{n}: energy above each level's harmonic limit {leak:.1f} dB")
        check(dmax < 32767, f"{n}: max adjacent delta {dmax:.0f} (wt.c needs < 32767)")
        check(all(29000 < p <= 29495 for p in peaks), f"{n}: frame peaks {min(peaks):.0f}..{max(peaks):.0f} (0.9 FS)")
        check(phase < 0.01, f"{n}: harmonic 1 in sine phase (worst {phase:.4f} rad)")
        ref = model(x, p_q16, pitched == 2)
        e = bank - ref
        snr = 10 * np.log10((ref ** 2).sum() / max((e ** 2).sum(), 1e-9))
        fsnr = [10 * np.log10((ref[f] ** 2).sum() / max((e[f] ** 2).sum(), 1e-9)) for f in range(FRAMES)]
        check(snr > 60 or n == "noise" and min(fsnr) > 40,
              f"{n}: matches the float model, SNR {snr:.1f} dB (frames {min(fsnr):.1f}..{max(fsnr):.1f})")
        if n == "saw220":
            c = np.abs(np.fft.rfft(bank[0][:2048]))
            hs = [20 * np.log10(c[k] / c[1]) + 20 * np.log10(k) for k in (2, 5, 10, 40, 80)]
            check(all(abs(v) < 1 for v in hs), "saw220: harmonics 1/n (dB vs 1/n: "
                  + " ".join(f"{v:+.2f}" for v in hs) + ")")
        if n == "pwm110":
            c0 = np.abs(np.fft.rfft(bank[0][:2048]))
            c11 = np.abs(np.fft.rfft(bank[11][:2048]))
            check(c0[2] < c0[1] / 20 and c11[2] > c11[1] / 4, f"pwm110: frame 0 square (H2/H1 {c0[2] / c0[1]:.3f}), "
                  f"frame 11 narrow pulse (H2/H1 {c11[2] / c11[1]:.3f})")
        if n == "serum64":
            check(p_q16 == FIXED and pitched == 2, "serum64: recognised as a 2048-sample table")
            # each output frame must be the source frame at its grid position (band-limited, phase-aligned)
            src = x.astype(float).reshape(-1, 2048)
            span = (len(x) - 2051) // 11
            werr = 0
            for f in range(FRAMES):
                g = min(((span * f + 1024) & ~2047) // 2048, len(src) - 1)
                sa = np.abs(np.fft.rfft(src[g]))[1:257]
                sb = np.abs(np.fft.rfft(bank[f][:2048]))[1:257]
                werr = max(werr, np.abs(sa / sa.max() - sb / sb.max()).max())
            check(werr < 0.01, f"serum64: frames = source grid frames (worst harmonic error {werr:.4f})")
        if n in ("noise", "sine440", "saw220", "pwm110", "saw30"):
            check(pitched != 2, f"{n}: not mistaken for a 2048-sample table")
    budget = 400e6 / 60
    print(f"worst cycles: detect {worst['detect']}, frame {worst['frame']}, bank {worst['total']} "
          f"(= {worst['total'] / 400e6 * 1000:.1f} ms at 400 MHz; one 60 Hz UI frame = {budget:.0f})")
    if not a.keep:
        shutil.rmtree(workdir)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
