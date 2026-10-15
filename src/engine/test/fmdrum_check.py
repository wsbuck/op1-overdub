#!/usr/bin/env python3
"""Simulator checks for the fmdrum drum engine (src/slot/fmdrum.c) before flashing.

  python3 src/engine/test/fmdrum_check.py [--keep DIR]

Builds fmdrum.c with the device flags and fmdrum_main.c (TE's drum voice loop in miniature: slot init, note-on, render,
active, note-off, each through its C_ENTRY wrapper with fx_abi.S's register sentinels) and runs every case under
bfin-elf-run at 88.2 kHz. The knob curves come from src/slot/gen_fmdrum.py (the tables' source). Checks:
  pitch (K0) per semitone; the sweep (K4/K5) starts high and lands on the pitch; FM spectra (sidebands = Bessel J_k(I)
  of the index the tables give); the modulator's own decay (K7) dims the sidebands while the note sounds on;
  aliasing at the extremes (index limiter, modulator fade near fs/2): energy folded into 0..20 kHz; DC (a 1:1 ratio puts
  a sideband at 0 Hz); amplitude decay times (K3) and the voice's end (active() 0, then silence); the noise zones (K6:
  colour rising AIR < SNARE < HAT, tone gone in CLAP, clap bursts); velocity; knob changes and retriggers without
  clicks; every factory kit key: level (peaks, near-clip samples, audible); CYCLES per render call against the engines'
  budget; output guard; no runtime division or other libgcc calls; slot init; the ABI. Exits non-zero on a failure.
"""
import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(HERE)
SLOT = os.path.join(os.path.dirname(ENGINE), "slot")
sys.path.insert(0, SLOT)
import gen_fmdrum as G  # noqa: E402
from simrun import CPU_BUDGET  # noqa: E402

FS = 88200
DRY = [0, G.k1_of(1.0), 0, 32767, 0, 0, 0, 32767]          # A3 sine, longest decay, no FM/sweep/noise


def P(**kw):
    """per-key values from physical settings (gen_fmdrum's inverse curves); missing ones from DRY"""
    k = list(DRY)
    if "hz" in kw:
        k[0] = G.k0_of(kw["hz"])
    if "ratio" in kw:
        k[1] = G.k1_of(kw["ratio"])
    if "fm" in kw:
        k[2] = G.k2_of(kw["fm"])
    if "decay" in kw:
        k[3] = G.k_expo(kw["decay"], *G.DECAY)
    if "sweep" in kw:
        k[4] = G.k4_of(kw["sweep"])
    if "snap" in kw:
        k[5] = G.k_expo(kw["snap"], *G.SNAP)
    if "noise" in kw:
        k[6] = kw["noise"]
    if "mod" in kw:
        k[7] = G.k_expo(kw["mod"], *G.MODDEC)
    for j, v in kw.get("raw", {}).items():
        k[j] = v
    return k


def cases():
    c = {}
    for s in (-24, -12, 0, 7, 12, 24, 36):                  # pitch: semitones from A3
        c[f"pitch{s}"] = (0, 127, 200, P(raw={0: s * 512}))
    c["pitch_min"] = (0, 127, 200, P(raw={0: G.K0_MIN}))
    c["pitch_max"] = (0, 127, 200, P(raw={0: G.STOCK_HI[0]}))
    c["sweep"] = (0, 127, 300, P(hz=110, sweep=24, snap=0.03))
    c["sweep_none"] = (0, 127, 120, P(hz=110, sweep=0, snap=0.03))
    for r, fm in ((3.5, 1.0), (3.5, 2.4), (2.7565, 4.0), (1.4142, 6.0)):
        c[f"fm_{r}_{fm}"] = (0, 127, 120, P(hz=220, ratio=r, fm=fm, mod=3.0))
    c["mod_fast"] = (0, 127, 120, P(hz=220, ratio=3.5, fm=4.0, mod=0.003))
    c["mod_slow"] = (0, 127, 120, P(hz=220, ratio=3.5, fm=4.0, mod=3.0))
    # aliasing: highest pitches, ratios and index, no sweep, sustained
    alias = [(G.STOCK_HI[0], 1.4142), (G.STOCK_HI[0], 0.5), (G.STOCK_HI[0], 2.7565), (12288, 7.1268), (18000, 4.6),
             (6144, 15.5), (0, 15.5), (20000, 2.0)]
    for i, (k0, r) in enumerate(alias):
        c[f"alias{i}"] = (0, 127, 200, P(ratio=r, fm=16.0, mod=3.0, raw={0: k0}))
    c["dc"] = (0, 127, 400, P(hz=110, ratio=1.0, fm=2.4, mod=3.0))
    c["decay_min"] = (0, 127, 60, P(raw={3: 0}))
    c["decay_mid"] = (0, 127, 300, P(decay=0.25))
    c["decay_max"] = (0, 127, 3400, P(raw={3: 32767}))
    for z, k6 in (("off", 0), ("air", 6000), ("snare", 14000), ("hat", 22000), ("hat_top", 24400), ("clap", 32767)):
        c[f"noise_{z}"] = (1, 127, 160, P(hz=50, decay=0.5, noise=k6))
    c["vel64"] = (0, 64, 60, P(hz=220, decay=0.5))
    c["vel127"] = (0, 127, 60, P(hz=220, decay=0.5))
    c["chg_pitch"] = (0, 127, 80, P(hz=220, decay=2.0) + ["chg", 40, 0, 12 * 512])
    c["chg_fm"] = (0, 127, 80, P(hz=220, ratio=2.0, fm=1.0, decay=2.0, mod=3.0) + ["chg", 40, 2, G.k2_of(4.0)])
    c["chg_noise"] = (0, 127, 80, P(hz=220, decay=2.0, noise=6000) + ["chg", 40, 6, 22000])
    c["retrig"] = (0, 127, 80, P(hz=60, decay=1.0, sweep=12, snap=0.02, fm=1.0, mod=0.03) + ["retrig", 30])
    c["worst"] = (23, 127, 30, [G.STOCK_HI[0], 31 * 1024, 32767, 32767, 32767, 32767, 20000, 32767])
    for name, kit in G.KITS.items():
        for k, row in enumerate(kit[1]):
            c[f"kit_{name.replace(' ', '_')}_{k}"] = (k, 127, 500, list(row))
    return c


def spectrum(x, f0=None):
    x = x.astype(float)
    w = np.hanning(len(x))
    n = 1 << max(16, int(math.ceil(math.log2(len(x)))))
    m = np.abs(np.fft.rfft(x * w, n)) / (w.sum() / 2)
    return np.fft.rfftfreq(n, 1 / FS), m


def peak_freq(x):
    fr, m = spectrum(x)
    i = int(np.argmax(m[1:])) + 1
    a, b, c = m[i - 1], m[i], m[i + 1]
    return (i + 0.5 * (a - c) / (a - 2 * b + c)) * FS / (2 * (len(m) - 1))


def level(fr, m, f, bw=8.0):
    sel = np.abs(fr - f) <= bw
    return m[sel].max() if sel.any() else 0.0


def bessel(k, x, terms=40):
    k = abs(k)
    return sum((-1) ** m / (math.factorial(m) * math.factorial(m + k)) * (x / 2) ** (2 * m + k) for m in range(terms))


def t60_index(x, start=0):
    """samples from the peak until the 2 ms RMS last falls below peak RMS - 60 dB"""
    e = np.sqrt(np.convolve(x.astype(float) ** 2, np.ones(176) / 176, "same"))
    p = int(np.argmax(e))
    below = np.nonzero(e[p:] < e[p] * 1e-3)[0]
    return (below[0] if len(below) else len(x) - p) / FS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep")
    a = ap.parse_args()
    w = tempfile.mkdtemp(prefix="fmdrum_")
    cs = cases()
    cflags = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
    cmds = [f"bfin-elf-run /tmp/m {k} {v} {b} {' '.join(map(str, p))} > /w/{n}.bin 2> /w/{n}.err; echo $? > /w/{n}.rc"
            for n, (k, v, b, p) in cs.items()]
    cmds.append("bfin-elf-run /tmp/m init > /w/init.txt 2> /w/init.err; echo $? > /w/init.rc")
    open(os.path.join(w, "cmds"), "w").write("\n".join(cmds) + "\n")
    open(os.path.join(w, "run.sh"), "w").write(
        "set -e; cd /tmp\n"
        f"bfin-elf-gcc {cflags} -I /engine -I /src -c -o fd.o /src/fmdrum.c\n"
        "bfin-elf-nm -u fd.o > /w/undef.txt\n"
        "bfin-elf-objdump -d fd.o > /w/fd.dis\n"
        "bfin-elf-as -I /src --defsym NO_FX=1 -o abi.o /src/test/fx_abi.S\n"
        "bfin-elf-as -I /src -o entries.o /engine/test/fmdrum_abi.S\n"
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -I /src -I /engine -o m /engine/test/fmdrum_main.c fd.o abi.o entries.o\n"
        "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v",
                    f"{ENGINE}:/engine:ro", "-v", f"{w}:/w", "op1-bfin", "sh", "/w/run.sh"], check=True)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    res = {}
    for n in cs:
        err = open(os.path.join(w, n + ".err")).read()
        f = err.split()
        x = np.fromfile(os.path.join(w, n + ".bin"), dtype="<i2").astype(np.int64)
        res[n] = dict(x=x, cyc=int(f[2]), guard=f[4], vptr=f[6], end=int(f[8]),
                      rc=open(os.path.join(w, n + ".rc")).read().strip(), err=err)

    # ---- build, ABI, cost ----
    undef = set(open(os.path.join(w, "undef.txt")).read().split()) - {"U"}
    check(undef <= {"_te_drum_key", "_te_drum_vel", "_ranges", "_fmdrum_id"},
          f"no runtime division or other libgcc calls (undefined: {', '.join(sorted(undef))})")
    abi = all(r["rc"] == "0" for r in res.values())
    check(abi, f"ABI: {sum(3 + cs[n][2] * 2 for n in cs)} entry calls preserve R4-R7, P3-P5, FP, SP, loops, L"
          + ("" if abi else " (" + "; ".join(r["err"].strip() for r in res.values() if r["rc"] != "0")[:300] + ")"))
    check(all(r["guard"] == "5a5a5a5a" for r in res.values()), "output guard intact (128 samples per call)")
    check(all(r["vptr"] == "0190f8a4" for r in res.values()), "TE's vptr never written")
    worst = max(r["cyc"] for r in res.values())
    who = max(res, key=lambda n: res[n]["cyc"])
    check(worst <= CPU_BUDGET, f"cycles per render call: max {worst} = {worst / 128:.0f} per sample ({who}; budget "
          f"{CPU_BUDGET} = 600 per sample per voice)")

    # ---- pitch ----
    errs = []
    for s in (-24, -12, 0, 7, 12, 24, 36):
        x = res[f"pitch{s}"]["x"]
        f = peak_freq(x[FS // 10:FS // 10 + 16384])
        errs.append(abs(1200 * math.log2(f / (220 * 2 ** (s / 12)))))
    check(max(errs) < 3, f"K0 pitch, 512 per semitone from A3 = 220 Hz, -24..+36 semitones: worst {max(errs):.2f} cents")
    lo, hi = peak_freq(res["pitch_min"]["x"][FS // 10:FS // 10 + 32768]), peak_freq(res["pitch_max"]["x"][FS // 10:FS // 10 + 16384])
    check(abs(lo - G.pitch_hz(G.K0_MIN)) < 0.3 and abs(1200 * math.log2(hi / G.pitch_hz(G.STOCK_HI[0]))) < 3,
          f"K0 range: {lo:.1f} Hz .. {hi:.0f} Hz")

    # ---- sweep ----
    def inst_freq(x, a, b):         # zero crossings per second in samples a..b
        y = x[a:b]
        z = np.nonzero(np.diff(np.signbit(y).astype(int)))[0]
        return (len(z) - 1) / 2 / ((z[-1] - z[0]) / FS) if len(z) > 2 else 0.0
    x = res["sweep"]["x"]
    k = cs["sweep"][3]
    tau, semis, f0 = G.expo(k[5], *G.SNAP), G.sweep_semis(k[4]), G.pitch_hz(k[0])

    def model(a, b):                # mean frequency of f0 x 2^(semis e^(-t / tau) / 12) over samples a..b
        t = np.arange(a, b) / FS
        return float(np.mean(f0 * 2 ** (semis * np.exp(-t / tau) / 12)))
    sw_err = max(abs(1200 * math.log2(inst_freq(x, a_, b_) / model(a_, b_))) for a_, b_ in
                 ((0, 441), (882, 1764), (2646, 4410), (8820, 13230)))
    f_late = inst_freq(x, FS // 4, FS // 4 + 4410)
    check(sw_err < 40 and abs(1200 * math.log2(f_late / f0)) < 10,
          f"K4/K5 sweep +{semis:.1f} st, tau {tau * 1000:.0f} ms: follows f0 2^(S e^(-t/tau) / 12) within {sw_err:.0f} "
          f"cents over 0..150 ms; {f_late:.1f} Hz after 250 ms (pitch {f0:.1f})")
    x = res["sweep_none"]["x"]
    check(abs(1200 * math.log2(inst_freq(x, 0, 2205) / 110)) < 30, "no sweep: the pitch from the start")

    # ---- FM spectra: sidebands fc + k fm with |J_k(I)| ----
    worst_db = 0.0
    for r, fm in ((3.5, 1.0), (3.5, 2.4), (2.7565, 4.0), (1.4142, 6.0)):
        k = P(hz=220, ratio=r, fm=fm, mod=3.0)
        fc = G.pitch_hz(k[0])
        fmod = fc * G.ratio_of(k[1])
        x = res[f"fm_{r}_{fm}"]["x"]
        a0, n = 441, 4096                                    # 5 ms .. 51 ms
        mid = (a0 + n / 2) / FS
        menv = 10 ** (-3 * mid / G.expo(k[7], *G.MODDEC))   # the modulator envelope at the window's centre
        I = G.fm_rad(k[2]) * (16384 + (32512 >> 1)) / 32768 * menv
        fr, m = spectrum(x[a0:a0 + n])
        got, want = [], []
        for j in range(-3, 4):
            f = fc + j * fmod
            if abs(f) < 30:
                continue
            got.append(level(fr, m, abs(f), 25))
            want.append(abs(bessel(j, I)))
        g, wv = np.array(got), np.array(want)
        g, wv = g / g.max(), wv / wv.max()
        sel = wv > 0.1
        d = np.abs(20 * np.log10(g[sel] / wv[sel]))
        worst_db = max(worst_db, d.max())
    check(worst_db < 1.5, f"FM sidebands = Bessel J_k(I) of the tables' index (ratios 3.5, 2.76, 1.41; I 1..6 rad): "
          f"worst {worst_db:.2f} dB")

    # ---- modulator decay ----
    def bright(x, a, b, fc):
        fr, m = spectrum(x[a:b])
        return float((m[fr > 1.5 * fc] ** 2).sum() / max((m ** 2).sum(), 1e-9))
    f_ = res["mod_fast"]["x"]
    s_ = res["mod_slow"]["x"]
    bf, bs = bright(f_, 4410, 8820, 220), bright(s_, 4410, 8820, 220)
    check(bf < 1e-3 and bs > 0.3, f"K7 modulator decay: sideband energy 50..100 ms {bf:.1e} (3 ms) vs {bs:.2f} (3 s)")

    # ---- aliasing ----
    worst_a, alias_db = -999.0, []
    for i in range(8):
        k = cs[f"alias{i}"][3]
        x = res[f"alias{i}"]["x"][4410:4410 + 16384]
        fc = G.pitch_hz(k[0])
        fmod = fc * G.ratio_of(k[1])
        win = np.blackman(len(x))                           # low sidelobes: many strong sidebands
        m = np.abs(np.fft.rfft(x.astype(float) * win, 1 << 16))
        fr = np.fft.rfftfreq(1 << 16, 1 / FS)
        true_ = np.zeros_like(m, dtype=bool)
        for j in range(-200, 201):
            f = abs(fc + j * fmod)
            if f < FS / 2:
                true_ |= np.abs(fr - f) <= 40                # Blackman main lobe and the decaying index's spread
        band = fr < 20000
        e_true, e_other = (m[band & true_] ** 2).sum(), (m[band & ~true_] ** 2).sum()
        ar = 10 * math.log10(max(e_other, 1e-12) / max(e_true, 1e-12))
        worst_a = max(worst_a, ar)
        alias_db.append(ar)
    print("  aliasing per case (dB):", " ".join(f"{v:.0f}" for v in alias_db))
    check(worst_a < -40, f"aliasing at the extremes (pitch up to 3.5 kHz, ratios to 15.5, FM 16 rad): energy off the "
          f"true sidebands in 0..20 kHz {worst_a:.1f} dB")

    # ---- DC ----
    x = res["dc"]["x"]
    seg = x[FS // 2:FS * 3 // 2].astype(float)
    dc = abs(seg.mean()) / np.abs(seg).max()
    fr, m = spectrum(x[:FS // 2])           # without the blocker the 0 Hz sideband J_1(2.4) would be ~ 0.5 x the peak
    check(dc < 0.02, f"DC with a 1:1 ratio at 2.4 rad (a sideband at 0 Hz, decaying with the index): mean "
          f"{100 * dc:.2f} % of the peak over 0.5 .. 1.5 s")
    dcs = []
    for n in cs:
        if n.startswith("kit_"):
            x = res[n]["x"].astype(float)
            dcs.append(abs(x.sum()) / max(np.abs(x).sum(), 1))
    check(max(dcs) < 0.02, f"DC over every kit key's whole note: |sum| / sum |x| at most {100 * max(dcs):.2f} %")

    # ---- amplitude decay and the voice's end ----
    for n, want in (("decay_min", G.DECAY[0]), ("decay_mid", 0.25), ("decay_max", G.DECAY[1])):
        x = res[n]["x"]
        t = t60_index(x)
        end = res[n]["end"]
        tail = x[(end + 1) * 128:] if end >= 0 else np.array([1])
        check(abs(t - want) < 0.15 * want + 0.004 and end > 0 and not np.abs(tail).any(),   # + 0.004: block ramps
              f"K3 decay {want * 1000:.0f} ms: -60 dB after {t * 1000:.0f} ms; active() 0 after block {end}, then silence")

    # ---- noise zones ----
    def centroid(x):
        fr, m = spectrum(x[:8192])
        sel = (fr > 200) & (fr < 20000)
        return float((fr[sel] * m[sel] ** 2).sum() / max((m[sel] ** 2).sum(), 1e-9))
    cen = {z: centroid(res[f"noise_{z}"]["x"]) for z in ("air", "snare", "hat")}
    check(cen["air"] < cen["snare"] < cen["hat"], f"K6 colour: centroid AIR {cen['air']:.0f} < SNARE "
          f"{cen['snare']:.0f} < HAT {cen['hat']:.0f} Hz")
    fr, m = spectrum(res["noise_off"]["x"][:8192])
    hf_off = (m[fr > 500] ** 2).sum() / (m ** 2).sum()
    check(hf_off < 1e-6, f"K6 OFF: no noise ({10 * math.log10(max(hf_off, 1e-12)):.0f} dB above 500 Hz)")

    def tone50(n):
        fr, m = spectrum(res[n]["x"][2048:2048 + 16384])
        return level(fr, m, 50, 4)
    t_off, t_top, t_clap = tone50("noise_off"), tone50("noise_hat_top"), tone50("noise_clap")
    check(t_top < 0.1 * t_off and t_clap < 0.02 * t_off, f"K6: the tone fades through HAT ({t_top / t_off:.3f}) and is "
          f"gone in CLAP ({t_clap / t_off:.4f})")
    x = res["noise_clap"]["x"][:int(0.035 * FS)].astype(float)
    e = np.sqrt(np.convolve(x ** 2, np.ones(88) / 88, "same"))
    peaks = [i for i in range(1, len(e) - 1) if e[i] >= e[i - 1] and e[i] > e[i + 1] and e[i] > 0.3 * e.max()]
    groups = len([p for j, p in enumerate(peaks) if j == 0 or p - peaks[j - 1] > 300])
    check(groups >= 4, f"CLAP: {groups} bursts in the first 35 ms (want 4: three short, then the tail)")

    # ---- velocity ----
    p127, p64 = np.abs(res["vel127"]["x"]).max(), np.abs(res["vel64"]["x"]).max()
    check(0.45 < p64 / p127 < 0.56, f"velocity 64 vs 127: level ratio {p64 / p127:.2f}")

    # ---- clicks: knob changes and a retrigger, against the largest step in the blocks around ----
    for n, b in (("chg_pitch", 40), ("chg_fm", 40), ("chg_noise", 40), ("retrig", 30)):
        x = res[n]["x"]
        d = np.abs(np.diff(x))
        at = d[b * 128 - 2:b * 128 + 130].max()
        around = max(d[(b - 4) * 128:(b - 1) * 128].max(), d[(b + 2) * 128:(b + 5) * 128].max())
        check(at <= 1.25 * around, f"{n}: largest step at the change {at} vs {around} around it (no click)")
    x = res["retrig"]["x"]
    p0, p1 = np.abs(x[:30 * 128]).max(), np.abs(x[30 * 128:]).max()
    check(abs(p1 / p0 - 1) < 0.1, f"retrigger restarts the note (peak {p1} vs first {p0})")

    # ---- the factory kits ----
    pk, nc, quiet = [], 0, []
    for n in cs:
        if n.startswith("kit_"):
            x = res[n]["x"]
            pk.append(int(np.abs(x).max()))
            nc += int((np.abs(x) >= 32000).sum())
            if pk[-1] < 3300:
                quiet.append(n)
    total = sum(len(res[n]["x"]) for n in cs if n.startswith("kit_"))
    check(not quiet and nc < total * 2e-4, f"factory kits ({len(pk)} keys): peaks {20 * math.log10(min(pk) / 32768):.1f} "
          f"to {20 * math.log10(max(pk) / 32768):.1f} dBFS, {nc} samples >= 32000 of {total}"
          + (f"; quiet: {', '.join(quiet)}" if quiet else ""))
    for n in cs:
        if n.startswith("kit_"):
            x = res[n]["x"][-128 * 20:]
            assert x.size
    r = open(os.path.join(w, "init.txt")).read().split()
    check(open(os.path.join(w, "init.rc")).read().strip() == "0" and r == ["kept", "1", "row", "1", "norow", "1"],
          f"slot init: loaded per-key values never rewritten, row 9 copied only when sane ({' '.join(r)})")

    if a.keep:
        shutil.copytree(w, a.keep, dirs_exist_ok=True)
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
