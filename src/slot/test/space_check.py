#!/usr/bin/env python3
"""Checks for the space (reverb) FX (src/slot/space.c) before flashing.

  python3 src/slot/test/space_check.py [--quick] [--host] [--keep DIR]

Runs src/slot/space.c (its asm kernels) in the Blackfin simulator (spacesim.py; pingpong.c, repeat.c and the lines
full of garbage at boot linked in), with the master tank at 44.1 kHz and, as a second build, at 22.05 kHz
(SPACE_MASTER_HALF); the synth / drum (mono) tank is always 22.05 kHz. --host: the C reference natively (fast;
skips the asm and CYCLES checks). Compares with the float prototype (design/fx/reverb_proto, tank.c via proto.py).

  1. asm = the C reference, sample for sample (both builds; spaces, holds, shimmer, the green key, every knob moving)
  2. RT60 per octave band per space (pink-noise bursts at -9 dBFS RMS, two seeds ensemble-averaged, Schroeder T30)
     against the target of the two-band filters (proto.py target_band at this FX's DECAY mapping: noon = the space's
     own RT60): within 10 % at 125 Hz .. 2 kHz, the 22.05 kHz tanks 15 % at 2 kHz (4 / 8 kHz reported: linear
     interpolation darkens the modulated lines, as in the prototype); and within the same of the float prototype run
     the same way (the TONE variants against the prototype only)
  3. the spectrum of the tail matches the prototype's (octave bands relative to 1 kHz, within 3 dB, 125 Hz .. 4 kHz)
  4. early reflections: an impulse's first energy arrives at the predelay plus the first tap, none before
  5. 60 s at the extremes (VOID x4 with SHIMMER +12 / +12+19 / SUB -12 at the top, FREEZE and INFINITE with shimmer
     and SUB): bounded,
     no 5 s window louder than the first by more than 1 dB, no line saturating
  6. FREEZE holds within 2 dB over 60 s (plain, frozen shimmer, frozen SUB); INFINITE layers a second chord on the first
     (both present at the end) with the line peak < 0.5 FS on a 0 dBFS pad
  7. the tail ends in exact silence (every space, x1 and x4, both rates), and nothing above -90 dBFS on the way
     (no limit cycle sits there)
  8. DC: a DC offset on the input leaves the wet free of DC (the low cut), and the tail has no DC of its own
  9. MIX: 0 is the input bit exact; the wet rises with the dry at unity to the middle; above it, on the master, the
     wet ducks -12 dB one detent up and -24 dB at the top while playing and recovers; on synth / drum the dry fades
  10. knobs turned detent by detent across every range and zone join: no zipper (no click or step in the output)
  11. the lines shared with pingpong and repeat: neither the boot garbage nor another FX's samples are ever heard;
      pingpong / repeat after space start from silence too
  12. re-enabled after a gap: a clean start (the old tail is gone); the green key freezes while held
  13. TE's LFO on white (synth): the zone stays the stored knob's
  14. CYCLES per 64-frame call at both rates: idle, busy, shimmer, freeze, SUB, GATE, SWELL (reported; sanity bounds;
      every v1.1 zone's peak at or below SHIMMER +12+19's)
  15. the v1.1 TAIL zones (zones()): SUB -12 exactly an octave down and stable 60 s at VOID x4 (and frozen), no
      sub-bass build-up; GATE's hold times 50 / 200 / 350 ms, the close (60 dB in ~30 ms) without clicks, a sustained
      pad (loud and quiet) held to its end; SWELL's rise times 0.15 .. 1.5 s and the dry untouched; FREEZE in each
      zone; the zone joins glide (no click); TE's LFO keeps GATE's zone

  --zones runs only 15 (and 14's zone rows in the simulator)
"""
import argparse
import math
import os
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "design", "fx", "reverb_proto"))
import spacesim as S  # noqa: E402
import proto as P  # noqa: E402

FS = 44100
LEAD = 40 * 64                              # silence first: a clean start clears the line set (wet muted)
BANDS = [125, 250, 500, 1000, 2000, 4000, 8000]
SPACES = [("ROOM", 0), ("HALL", 8192), ("CHURCH", 16384), ("CATHEDRAL", 24576), ("VOID", 32767)]
fails = []


def check(ok, msg):
    print(("PASS " if ok else "FAIL ") + msg)
    if not ok:
        fails.append(msg)


def pink(n, rms, seed=3):
    r = np.random.default_rng(seed)
    X = np.fft.rfft(r.standard_normal((n, 2)), axis=0)
    f = np.fft.rfftfreq(n, 1 / FS)
    f[0] = 1
    X *= (1 / np.sqrt(f))[:, None]
    X[f < 40] = 0
    x = np.fft.irfft(X, n, axis=0)
    return x / np.sqrt(np.mean(x ** 2)) * rms


def l2m(k):                                 # space.h sp_decay_l2, as a float
    if k <= 16384:
        return -2 + 2 * k / 16384
    return min(2.0, 2 * (k - 16384) / 14335)


def target(k):
    """proto.py params at this FX's DECAY mapping (noon = the space's own RT60)."""
    p, _, _ = P.params(*k)
    r = p["t60_hi"] / p["t60_lo"]
    t = P.space_at(k[0])["t60"] * 2 ** l2m(min(k[1], 30719))
    p["t60_lo"], p["t60_hi"] = t, t * r
    p["in_gain"] = 0.5 * (max(t, 0.3) / 1.5) ** -0.25
    x = k[2] / (10 * 1024) * 2 - 1
    if x < 0:                               # dark raises the low cut x1.3 at most here (x2 in the prototype)
        p["in_hp"] = P.space_at(k[0])["hp"] * 1.3 ** -x
    return p


def rt60s(hs):
    """RT60 per band of one or more responses (their band energies summed: an ensemble average)"""
    hs = hs if isinstance(hs, list) else [hs]
    out = []
    for fc in BANDS:
        es = [P.band_energy(h, fc) for h in hs]
        n = min(len(e) for e, _ in es)
        out.append(P.edc_rt60(sum(e[:n] for e, _ in es), es[0][1]))
    return out


def band_levels(h):
    e = [10 * math.log10(P.band_energy(h, fc)[0].sum() + 1e-30) for fc in BANDS]
    return [v - e[3] for v in e]


def wet_of(out, x):
    """the wet: the output minus the dry input (MIX <= 16384 keeps the dry at unity, bit exact)"""
    x = x[:len(out)].astype(float)
    if len(x) < len(out):
        x = np.concatenate([x, np.zeros((len(out) - len(x), x.shape[1]))])
    return out.astype(float) - x

SUB_TOP, GATE_LO, GATE_MID, GATE_HI, SWELL_LO, SWELL_MID, SWELL_HI = 23551, 23552, 26112, 28671, 28672, 30720, 32767


def cycles(run, builds, zones_only=False):
    """14: CYCLES per 64-frame call (simulator), mean / max; the v1.1 zones' peaks at or below SHIMMER +12+19's"""
    nc = LEAD + 600 * 64
    rng = np.random.default_rng(9)
    busy = rng.uniform(-12000, 12000, (nc, 2))
    idle = np.zeros((nc, 2))
    hits = np.zeros((nc, 2))                    # snare-like hits every 0.25 s: the gate opens and shuts, swells
    for t0 in range(LEAD, nc, FS // 4):
        m = min(int(0.08 * FS), nc - t0)
        hits[t0:t0 + m] = (rng.uniform(-16000, 16000, m) * np.exp(-np.arange(m) / FS / 0.02))[:, None]
    cyc_cases = {"idle": (idle, (8192, 16384, 5120, 16384)), "busy": (busy, (8192, 16384, 5120, 16384)),
                 "busy_void4": (busy, (32767, 30719, 5120, 24576)),
                 "shimmer_12_19": (busy, (24576, 22000, 17500, 16384)),
                 "freeze_shimmer": (busy, (24576, 32767, 12800, 16384)), "infinite": (busy, (8192, 31000, 5120, 16384)),
                 "sub": (busy, (24576, 22000, SUB_TOP, 16384)), "frozen_sub": (busy, (24576, 32767, SUB_TOP, 16384)),
                 "gate": (hits, (8192, 16384, GATE_MID, 16384)), "swell": (hits, (24576, 16384, SWELL_MID, 16384))}
    if zones_only:
        cyc_cases = {k: v for k, v in cyc_cases.items() if k in ("shimmer_12_19", "sub", "frozen_sub", "gate", "swell")}
    for rate, defs in builds:
        cases = {}
        for nm, (sig, k) in cyc_cases.items():
            for m in "sm":
                if m == "m" and defs:
                    continue
                c = S.Case(sig, m)
                for b in range(nc // 64):
                    c.block(k, fx="S" if b < LEAD // 64 + 40 else "s")
                cases[f"{nm}_{m}"] = c
        res = run(cases, defs)
        for m in "sm":
            if m == "m" and defs:
                continue
            tag = "mono 22.05 kHz" if m == "m" else f"stereo {rate} kHz"
            row = ", ".join(f"{nm} {res[f'{nm}_{m}'][2][1] / 1000:.1f}k / {res[f'{nm}_{m}'][2][0] / 1000:.1f}k"
                            for nm in cyc_cases)
            worst = max(res[f"{nm}_{m}"][2][0] for nm in cyc_cases)
            bound = 110000 if m == "s" and not defs else 80000
            check(worst < bound, f"CYCLES per call, {tag} (mean / max): {row}")
            shp = res[f"shimmer_12_19_{m}"][2][0]
            zp = {nm: res[f"{nm}_{m}"][2][0] for nm in ("sub", "frozen_sub", "gate", "swell")}
            check(max(zp.values()) <= shp, f"CYCLES, {tag}: the v1.1 zones' peaks "
                  + " ".join(f"{k} {v / 1000:.1f}k" for k, v in zp.items())
                  + f" at or below SHIMMER +12+19's {shp / 1000:.1f}k")


def env_db(x, w):
    """RMS envelope in dB, windows of w samples"""
    n = len(x) // w
    r = np.sqrt(np.mean(x[:n * w].reshape(n, w) ** 2, axis=1))
    return 20 * np.log10(r / 32768 + 1e-12)


def peak_freq(x, lo, hi):
    """the strongest frequency in lo .. hi (Hz), interpolated (Hann, parabolic on the log magnitude)"""
    n = len(x)
    X = np.abs(np.fft.rfft(x * np.hanning(n), 8 * n))
    f = np.fft.rfftfreq(8 * n, 1 / FS)
    sel = np.nonzero((f >= lo) & (f <= hi))[0]
    i = sel[np.argmax(X[sel])]
    a, b, c = np.log(X[i - 1:i + 2] + 1e-12)
    return f[i] + (a - c) / (2 * (a - 2 * b + c)) * (f[1] - f[0]), X, f


def zones(run, quick):
    """15: the v1.1 TAIL zones (SUB -12, GATE, SWELL)"""
    rng = np.random.default_rng(15)
    tt = np.arange(LEAD + 40 * FS) / FS

    # -- SUB -12, the shifter alone (space_shtest, the C path: SUB's heads are C on the device too): an 880 Hz sine ->
    # 440 Hz exactly. Its mean frequency over 11 s with the grains' jitter (what plays), and the in-grain frequency
    # with the jitter off (SPACE_SH_STILL: the heads coherent, so the crossfades add no phase drift) --
    import subprocess
    for half in (0, 1):
        fs = FS >> half
        t = np.arange(12 * fs) / fs
        xs = np.round(np.sin(2 * np.pi * 880 * t) * 8000)
        got = []
        for d in ("", "-DSPACE_SH_STILL"):
            w = tempfile.mkdtemp(prefix="space_sh_")
            exe = os.path.join(w, "m")
            subprocess.run(["cc", "-std=gnu99", "-O2", "-w", "-DSPACE_HOST", "-DSPACE_NO_KEY", "-DSPACE_SHTEST", *d.split(),
                            "-I", S.SLOT, "-o", exe, os.path.join(HERE, "space_main.c"), os.path.join(S.SLOT, "space.c")],
                           check=True)
            nb = len(xs) // (64 >> half)
            xs.astype("<i2").tofile(os.path.join(w, "i"))
            subprocess.run([exe, "shtest", str(half), "2", os.path.join(w, "i"), os.path.join(w, "o"), str(nb)], check=True)
            y = np.fromfile(os.path.join(w, "o"), dtype="<i2").astype(float)[fs:]
            Y = np.fft.fft(y)
            fy = np.fft.fftfreq(len(y), 1 / fs)
            Y[(fy < 396) | (fy > 484)] = 0
            fi = np.diff(np.unwrap(np.angle(np.fft.ifft(Y)))) * fs / (2 * np.pi)
            got.append(1200 * math.log2((np.mean(fi) if not d else np.median(fi)) / 440))
        check(abs(got[0]) < 1 and abs(got[1]) < (0.1 if half == 0 else 1.0),
              f"SUB -12's shifter alone at {44.1 / (1 + half):.2f} kHz: an 880 Hz sine comes out {got[0]:+.2f} cents "
              f"from 440 Hz on average (the grains jittered), in-grain {got[1]:+.2f} cents (jitter off; at 22.05 kHz "
              "the half window, 551 samples, isn't a whole number of 880 Hz periods: its crossfades drift ~1 cent)")
    # ... and in the tank: an 880 Hz tone's tail holds a strong 440 Hz the plain tail doesn't
    n = LEAD + int(6 * FS)
    x = np.zeros((n, 2))
    on = slice(LEAD, LEAD + int(0.8 * FS))
    x[on] = (np.sin(2 * np.pi * 880 * tt[on]) * 12000 * np.minimum(1, (tt[on] - tt[LEAD]) / 0.01))[:, None]
    cases = {}
    for m in "sm":
        for nm, kt in (("sub", SUB_TOP), ("tone", 5120)):
            c = S.Case(x, m)
            c.fill((24576, 24000, kt, 16384))
            cases[f"{nm}{m}"] = c
    res = run(cases)
    for m in "sm":
        tag = "master" if m == "s" else "synth"
        seg = slice(LEAD + int(1.5 * FS), LEAD + int(5.5 * FS))
        X = peak_freq(wet_of(res[f"sub{m}"][0], x)[seg, 0], 300, 600)[1]
        Xt, f = peak_freq(wet_of(res[f"tone{m}"][0], x)[seg, 0], 300, 600)[1:]
        lvl = lambda XX, fc: 20 * math.log10(XX[(f > fc - 5) & (f < fc + 5)].max() + 1e-9)   # noqa: E731
        rel, gain = lvl(X, 440) - lvl(X, 880), lvl(X, 440) - lvl(Xt, 440)
        check(rel > -12 and gain > 25, f"{tag} SUB -12 at the top of its zone, CATHEDRAL: an 880 Hz tone's tail has "
              f"440 Hz {rel:+.0f} dB vs 880 Hz, {gain:.0f} dB above the plain tail's")

    # -- SUB at VOID x4 on a pad (110 .. 330 Hz), 60 s: no sub-bass build-up (the energy below 60 Hz never grows, and
    # stays a small share of the tail); frozen on the pad: no more creep than the frozen shimmer's --
    T = 20 if quick else 60
    nb = LEAD + int((T + 1) * FS)
    tl = np.arange(nb) / FS
    padx = np.zeros((nb, 2))
    pon = slice(LEAD, LEAD + 2 * FS)
    padx[pon] = (sum(np.sin(2 * np.pi * fr * tl[pon]) for fr in (110.0, 220.0, 277.2, 329.6)) / 4 * 20000)[:, None]
    cases = {}
    for m in "sm":
        c = S.Case(padx, m)
        c.fill((32767, 30719, SUB_TOP, 16384))
        cases[f"voidsub{m}"] = c
        for nm, kt in (("sub", SUB_TOP), ("sh12", 14000)):
            c = S.Case(padx, m)
            for b in range(nb // 64):
                c.block((24576, 32767 if b * 64 >= LEAD + int(1.0 * FS) else 16384, kt, 16384))
            cases[f"frz{nm}{m}"] = c
    res = run(cases, "-DSPACE_DIAG")

    def low(y):                                     # energy below 60 Hz (dB) and its share of the tail's
        Y = np.abs(np.fft.rfft(y)) ** 2
        fy = np.fft.rfftfreq(len(y), 1 / FS)
        lo, tot = Y[(fy > 10) & (fy < 60)].sum(), Y[fy > 10].sum()
        return 10 * math.log10(lo + 1e-9), 10 * math.log10(lo / (tot + 1e-9) + 1e-12)
    for m in "sm":
        tag = "master" if m == "s" else "synth"
        win = 5 * FS
        ws = res[f"voidsub{m}"][0][LEAD + 2 * FS:, 0].astype(float)
        ls = [low(ws[i:i + win]) for i in range(0, len(ws) - win + 1, win)]
        audible = [k for k in range(len(ls)) if 10 * math.log10(np.mean(ws[k * win:(k + 1) * win] ** 2) + 1e-9) > 40]
        grow = max(ls[k][0] - ls[0][0] for k in audible)
        share = max(ls[k][1] for k in audible)
        check(grow <= 3.0 and share <= -18, f"{tag} SUB -12 at VOID x4 on a pad (110 .. 330 Hz: its octave down reaches "
              f"55 Hz), {T} s: below 60 Hz at most {share:.0f} dB of the tail's energy (per 5 s: "
              + " ".join(f"{v[1]:.0f}" for v in ls) + f"), its level {grow:+.1f} dB at most over the first 5 s's while "
              "the tail decays (no sub-bass drone)")
        drift = []
        for nm in ("sub", "sh12"):
            o = res[f"frz{nm}{m}"][0][LEAD + 3 * FS:, 0].astype(float)
            rr = [20 * math.log10(math.sqrt(np.mean(o[i:i + FS] ** 2)) + 1e-9) for i in range(0, len(o) - FS + 1, FS)]
            drift.append(max(rr) - min(rr))
        check(drift[0] <= drift[1] + 1.0, f"{tag} FREEZE in SUB on the pad, {T - 2} s: the level within "
              f"{drift[0]:.1f} dB (frozen shimmer on the same pad {drift[1]:.1f} dB; on a noise burst: 6)")

    # -- GATE: hold times, the close, a pad held to its end --
    n = LEAD + int(3 * FS)
    x = np.zeros((n, 2))
    on = slice(LEAD, LEAD + int(0.3 * FS))
    x[on] = rng.uniform(-8000, 8000, (int(0.3 * FS), 2))
    cases = {}
    for m in "sm":
        for nm, kt in (("g50", GATE_LO), ("g200", GATE_MID), ("g350", GATE_HI), ("open", 5120)):
            c = S.Case(x, m)
            c.fill((24576, 16384, kt, 16384))
            cases[f"{nm}{m}"] = c
    res = run(cases, "-DSPACE_GATE_FLAT")          # (GATE's own low cut off: its tank = TONE's, the gain exact)
    end = LEAD + int(0.3 * FS)
    for m in "sm":
        tag = "master" if m == "s" else "synth"
        wo = wet_of(res[f"open{m}"][0], x)[:, 0]
        for nm, hold in (("g50", 50), ("g200", 200), ("g350", 350)):
            wg = wet_of(res[f"{nm}{m}"][0], x)[:, 0]
            g = env_db(wg[end:], 44) - env_db(wo[end:], 44)          # the gate's gain, 1 ms steps after the input
            t20 = int(np.argmax(g < -20))                              # ms after the input stopped
            t60 = int(np.argmax(g < -60)) if (g < -60).any() else 999
            open_ok = np.all(np.abs(g[:hold]) < 0.5)
            # the close without clicks: the 2nd difference never above the open tail's
            d2g, d2o = np.abs(np.diff(wg[end:end + int(0.6 * FS)], 2)), np.abs(np.diff(wo[end:end + int(0.6 * FS)], 2))
            k = 44
            mg = d2g[:len(d2g) // k * k].reshape(-1, k).max(1)
            mo = d2o[:len(d2o) // k * k].reshape(-1, k).max(1)
            click = np.max(mg - 1.05 * mo)
            # the input's follower takes ~15 ms to fall 15 dB (under the threshold), then the hold, then 60 dB in
            # ~30 ms (-20 dB ~10 ms in)
            exp = 15 + hold + 10
            check(open_ok and abs(t20 - exp) <= 8 and t60 - t20 <= 25 and click <= 4,
                  f"{tag} GATE {hold} ms: the tail at full through the hold after the input stops, -20 dB at {t20} ms, "
                  f"-60 dB at {t60} ms (expected ~{exp} and {t20} + 20), no click (the 2nd difference never above the "
                  f"open tail's: {click:+.0f})")
    # a pad held to its end (loud and quiet); and drums: every hit opens it
    n = LEAD + int(7 * FS)
    t = np.arange(n) / FS - LEAD / FS
    envp = np.clip(t / 0.08, 0, 1) * np.where(t < 4.0, 1.0, np.exp(-(t - 4.0) / 0.12))
    envp[t < 0] = 0
    sawc = sum(((fr * t) % 1) * 2 - 1 for fr in (130.8, 164.8, 196.0)) / 3
    cases = {}
    for m in "sm":
        for lv in (20000, 600):
            for nm, kt in (("gate", GATE_MID), ("open", 5120)):
                c = S.Case(sawc * envp * lv, m)
                c.fill((16384, 20000, kt, 16384))
                cases[f"pad{lv}{nm}{m}"] = c
    res = run(cases, "-DSPACE_GATE_FLAT")
    for m in "sm":
        tag = "master" if m == "s" else "synth"
        for lv in (20000, 600):
            xin = cases[f"pad{lv}gate{m}"].inp
            g = env_db(wet_of(res[f"pad{lv}gate{m}"][0], xin)[LEAD:, 0], 441) - \
                env_db(wet_of(res[f"pad{lv}open{m}"][0], xin)[LEAD:, 0], 441)       # 10 ms steps
            held = g[10:400]                                                   # 0.1 .. 4 s: the pad
            after = g[470:520]                                                 # 4.7 .. 5.2 s
            check(np.all(np.abs(held) < 0.5) and np.all(after < -50), f"{tag} GATE 200 ms on a 4 s pad at "
                  f"{20 * math.log10(lv / 32768):.0f} dBFS: open through the pad (gain {held.min():+.1f} .. "
                  f"{held.max():+.1f} dB), shut after its release (<= {after.max():.0f} dB from 4.7 s)")

    # a pluck (a note decaying, no release) shuts the gate too: the reference follows the recent peak slowly (~5 s)
    n = LEAD + int(4 * FS)
    tp = np.arange(n - LEAD) / FS
    x = np.zeros((n, 2))
    x[LEAD:] = (np.sin(2 * np.pi * 330 * tp) * np.exp(-tp / 0.25) * 16000)[:, None]
    cases = {}
    for m in "sm":
        for nm, kt in (("gate", GATE_MID), ("open", 5120)):
            c = S.Case(x, m)
            c.fill((16384, 16384, kt, 16384))
            cases[f"pl{nm}{m}"] = c
    res = run(cases, "-DSPACE_GATE_FLAT")
    for m in "sm":
        tag = "master" if m == "s" else "synth"
        g = env_db(wet_of(res[f"plgate{m}"][0], x)[LEAD:, 0], 441) - env_db(wet_of(res[f"plopen{m}"][0], x)[LEAD:, 0], 441)
        t = int(np.argmax(g < -20)) * 10
        check(400 <= t <= 1100, f"{tag} GATE 200 ms on a pluck (a 0.25 s decay, no release): shut (-20 dB) {t} ms after "
              "its attack, once it's 15 dB down plus the hold")

    # -- SWELL: the rise times, the dry untouched --
    n = LEAD + int(9 * FS)
    x = np.zeros((n, 2))
    onsets = [LEAD + int(0.2 * FS), LEAD + int(3.2 * FS), LEAD + int(6.2 * FS)]
    for o in onsets:
        m_ = int(2.5 * FS)
        tp = np.arange(m_) / FS
        x[o:o + m_] += (np.sin(2 * np.pi * 392 * tp) * np.exp(-tp / 0.6) * 12000 * np.minimum(1, tp / 0.002))[:, None]
    cases = {}
    for m in "sm":
        for nm, kt in (("s150", SWELL_LO), ("s470", SWELL_MID), ("s1500", SWELL_HI), ("open", 5120)):
            c = S.Case(x, m)
            c.fill((16384, 16384, kt, 16384))
            cases[f"{nm}{m}"] = c
    res = run(cases)
    for m in "sm":
        tag = "master" if m == "s" else "synth"
        wo = wet_of(res[f"open{m}"][0], x)[:, 0]
        for nm, ms in (("s150", 150), ("s470", 474), ("s1500", 1496)):
            out = res[f"{nm}{m}"][0]
            wg = wet_of(out, x)[:, 0]
            t90 = []
            for o in onsets[1:]:                    # (the first: the wet starts from nothing anyway)
                g = env_db(wg[o:o + 2 * FS], 44) - env_db(wo[o:o + 2 * FS], 44)
                t90.append(int(np.argmax(g[5:] > 20 * math.log10(0.9))) + 5)
            exp = ms * math.sqrt(0.9)               # rise^2 reaches 0.9 at 0.95 of the rise
            o = onsets[1]
            dr = out[o + int(0.02 * FS):o + int(0.04 * FS), 0].astype(float)
            di = x[o + int(0.02 * FS):o + int(0.04 * FS), 0]
            dry_db = 20 * math.log10(np.std(dr) / np.std(di))
            res_db = 20 * math.log10(np.std(dr - di) / np.std(di))
            check(all(abs(v / exp - 1) < 0.12 for v in t90) and abs(dry_db) < 0.5 and res_db < -20,
                  f"{tag} SWELL {ms / 1000:.2f} s: the wet back to 90 % {', '.join(map(str, t90))} ms after each "
                  f"onset (~{exp:.0f} expected); 20 .. 40 ms after an onset the output is the dry ({dry_db:+.2f} dB, "
                  f"the wet {res_db:.0f} dB under it)")
    # busy playing (8th-note plucks, 0.25 s apart) at a 1.5 s rise: each onset ducks the wet to 0.42 of the rise (-15 dB)
    # at most, so the reverb never vanishes; the first note (the tank quiet) swells from silence
    n = LEAD + int(5 * FS)
    x = np.zeros((n, 2))
    for i in range(12):
        o = LEAD + int(i * 0.25 * FS)
        tp = np.arange(int(0.25 * FS)) / FS
        x[o:o + len(tp)] += (np.sin(2 * np.pi * (220, 277.2, 329.6, 440)[i % 4] * tp) * np.exp(-tp / 0.08) * 12000
                             * np.minimum(1, tp / 0.002))[:, None]
    cases = {}
    for m in "sm":
        for nm, kt in (("sw", SWELL_HI), ("open", 5120)):
            c = S.Case(x, m)
            c.fill((16384, 16384, kt, 16384))
            cases[f"busy{nm}{m}"] = c
    res = run(cases)
    for m in "sm":
        tag = "master" if m == "s" else "synth"
        eo = env_db(wet_of(res[f"busyopen{m}"][0], x)[LEAD:, 0], 441)
        g = env_db(wet_of(res[f"busysw{m}"][0], x)[LEAD:, 0], 441) - eo
        i0 = int(np.argmax(eo > -70))               # the wet's first 30 ms (after the predelay)
        first, busy = g[i0:i0 + 3].max(), g[100:300]
        check(first < -20 and busy.min() > -17, f"{tag} SWELL 1.5 s on 8th-note plucks: the first note swells from "
              f"silence ({first:.0f} dB at most in the wet's first 30 ms), then the wet stays within {busy.min():.0f} dB of the plain tail "
              "(each onset ducks it to the rise's 0.42 at most: -15 dB)")
    # MIX 0 in SWELL / GATE is the input, bit exact
    cases = {}
    for m in "sm":
        for nm, kt in (("sw", SWELL_MID), ("gt", GATE_MID)):
            c = S.Case(x, m)
            c.fill((16384, 16384, kt, 0))
            cases[f"{nm}{m}"] = c
    res = run(cases)
    check(all(np.array_equal(res[k][0][:len(x)], cases[k].inp.astype(np.int64)) for k in cases),
          "MIX 0 in SWELL and GATE: the input, bit exact (the dry never touched)")

    # -- FREEZE in GATE and SWELL; the zone joins; the LFO --
    n = LEAD + int(8 * FS)
    x = np.zeros((n, 2))
    x[LEAD:LEAD + FS] = (sum(np.sin(2 * np.pi * fr * tt[LEAD:LEAD + FS]) for fr in (220.0, 277.2, 329.6)) / 3
                         * 12000)[:, None]
    hit = LEAD + int(5 * FS)
    x[hit:hit + int(0.4 * FS)] = (np.sin(2 * np.pi * 523.3 * tt[:int(0.4 * FS)]) * 8000)[:, None]
    cases = {}
    for m in "sm":
        for nm, kt in (("gate", GATE_MID), ("swell", SWELL_MID)):
            c = S.Case(x, m)
            for b in range(n // 64):
                c.block((24576, 32767 if b * 64 >= LEAD + int(0.9 * FS) else 16384, kt, 16384))
            cases[f"frz{nm}{m}"] = c
    res = run(cases)
    for m in "sm":
        tag = "master" if m == "s" else "synth"
        out, st, _ = res[f"frzgate{m}"]
        w = wet_of(out, x)[:, 0]
        e = env_db(w, 441)
        gap = e[(LEAD + int(2.5 * FS)) // 441:(hit - FS // 10) // 441]
        play = e[(hit + FS // 10) // 441:(hit + int(0.4 * FS)) // 441]
        lvb = st["level"][(hit - FS // 10) // 64]
        check(gap.max() < -70 and play.min() > -40 and lvb > 100, f"{tag} FREEZE in GATE: the frozen tail is kept "
              f"(tank level {lvb}) but gated shut while nothing plays (<= {gap.max():.0f} dBFS), heard while a note "
              f"plays over it (>= {play.min():.0f} dBFS)")
        out, st, _ = res[f"frzswell{m}"]
        w = wet_of(out, x)[:, 0]
        e = env_db(w, 441)
        before = e[(hit - FS // 2) // 441:hit // 441].mean()
        dip = e[(hit + int(0.03 * FS)) // 441:(hit + int(0.06 * FS)) // 441].max()
        back = e[(hit + int(1.5 * FS)) // 441:(hit + int(2.0 * FS)) // 441].mean()
        check(before - 20 < dip < before - 4 and abs(back - before) < 3, f"{tag} FREEZE in SWELL: the frozen tail "
              f"ducks under a new note ({before:.0f} -> {dip:.0f} dBFS: from 0.42 of the rise, -15 dB at most, as the "
              f"tank isn't quiet) and swells back ({back:.0f} dBFS 1.5 s later)")
    # zone joins mid-tail: no click (the 2nd difference vs the plain tail's)
    n = LEAD + int(6 * FS)
    x = np.zeros((n, 2))
    x[LEAD:LEAD + 2 * FS] = (np.sin(2 * np.pi * 220 * tt[LEAD:LEAD + 2 * FS]) * 9000)[:, None]
    joins = [("SHIMMER+12+19 > SUB", 17500, 20500), ("SUB > GATE", 22000, GATE_MID), ("GATE > SWELL", 27000, 29500),
             ("SWELL > TONE", 30000, 5120), ("TONE > GATE", 5120, GATE_MID), ("GATE > TONE", GATE_MID, 5120),
             ("TONE > SWELL", 5120, SWELL_HI), ("SUB > TONE", SUB_TOP, 9000)]
    cases = {}
    jb = (LEAD + int(2.4 * FS)) // 64                 # the join 0.4 s after the tone stops: the gate shut, the tail on
    for m in "sm":
        for i, (nm, k0, k1) in enumerate(joins):
            c = S.Case(x, m)
            for b in range(n // 64):
                c.block((16384, 22000, k0 if b < jb else k1, 16384))
            cases[f"j{i}{m}"] = c
            c = S.Case(x, m)
            c.fill((16384, 22000, 5120, 16384))
        cases[f"jref{m}"] = c
    res = run(cases)
    for m in "sm":
        tag = "master" if m == "s" else "synth"
        ref = res[f"jref{m}"][0][LEAD + int(2.1 * FS):LEAD + int(3.5 * FS), 0].astype(float)
        r0 = np.abs(np.diff(ref, 2)).max()
        worst = []
        for i, (nm, _, _) in enumerate(joins):
            y = res[f"j{i}{m}"][0][jb * 64 - FS // 10:jb * 64 + FS // 2, 0].astype(float)
            pre = np.abs(np.diff(res[f"j{i}{m}"][0][jb * 64 - FS // 4:jb * 64 - 64, 0].astype(float), 2)).max()
            worst.append((np.abs(np.diff(y, 2)).max() / max(pre, r0, 1.0), nm))
        mx = max(worst)
        check(mx[0] < 1.6, f"{tag}: zone joins mid-tail glide (the 2nd difference at the join <= {mx[0]:.2f} x the "
              f"tail's before it; worst {mx[1]}): " + ", ".join(f"{nm} {v:.2f}" for v, nm in worst))
    cases = {}
    c = S.Case(np.zeros((LEAD + 2 * FS, 2)), "m")
    for b in range((LEAD + 2 * FS) // 64):
        c.block((8192, 16384, 26000 + int(4000 * math.sin(b / 20)), 16384), stored=26000)   # GATE; the LFO into SWELL
    cases["lfo_gate"] = c
    res = run(cases)
    z = res["lfo_gate"][1]["zone"][LEAD // 64:]
    check(set(z.tolist()) == {4}, f"synth: TE's LFO swinging white from GATE into SWELL (stored GATE): the zone stays "
          f"GATE ({sorted(set(z.tolist()))})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true", help="fewer and shorter cases")
    ap.add_argument("--host", action="store_true", help="the C reference natively (no asm, no CYCLES)")
    ap.add_argument("--keep")
    ap.add_argument("--zones", action="store_true", help="only the v1.1 TAIL zones (15) and their CYCLES")
    a = ap.parse_args()
    sim = not a.host
    tmp = tempfile.mkdtemp(prefix="space_proto_")
    P.TMP = tmp
    P.EXE = P.build()
    builds = [("44.1", "")] + ([("22.05", "-DSPACE_MASTER_HALF=1")] if not a.quick else [])

    def run(cases, defs="", asm=True):
        return S.run(cases, sim=sim, defs=defs, asm=asm and sim, keep=a.keep)

    if a.zones:
        zones(run, a.quick)
        if sim:
            cycles(run, builds, zones_only=True)
        print(f"\n{len(fails)} failed" if fails else "\nall passed")
        sys.exit(1 if fails else 0)

    # ---- 1. asm = the C reference ----
    if sim:
        import subprocess
        flags = S.FLAGS.replace("-Wextra -Werror", "") + " -DSPACE_NO_KEY -Wno-unused-function -Wno-unused-variable"
        r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{S.SLOT}:/src:ro", "op1-bfin",
                            "sh", "-c", f"cd /tmp && bfin-elf-gcc {flags} -I /src -c -o ka.o /src/test/space_kernels.c && "
                            f"bfin-elf-gcc {flags} -I /src -DKT_C -c -o kc.o /src/test/space_kernels.c && bfin-elf-gcc "
                            "-std=gnu99 -msim -O1 -o kt /src/test/space_kernels_main.c ka.o kc.o && bfin-elf-run kt"],
                           capture_output=True, text=True)
        out = r.stdout.strip().splitlines()
        check(r.returncode == 0 and out and out[-1] == "0 failures", "every kernel, asm = the C reference on random "
              f"inputs (300 trials each, a third at full scale): {out[-1] if out else r.stderr[-300:]}")
        rng = np.random.default_rng(1)
        n = LEAD + 260 * 64
        x = rng.uniform(-14000, 14000, (n, 2))
        x[LEAD + 130 * 64:] = 0
        cases = {}
        for nm, k in (("hall", (8192, 16384, 5120, 16384)), ("sh19", (24576, 20000, 17000, 24000)),
                      ("sub", (16384, 22000, 21000, 20000)), ("gate", (8192, 16384, 25000, 16384)),
                      ("swell", (24576, 16384, 30000, 16384)), ("subfrz", (24576, 32767, 20500, 16384)),
                      ("dark", (3000, 8000, 1000, 30000)), ("frz", (12000, 32000, 12000, 20000)),
                      ("inf", (30000, 31000, 13000, 24000)), ("void4", (32767, 30719, 13000, 32767))):
            for m in "sm":
                c = S.Case(x, m)
                c.fill(k)
                cases[nm + m] = c
        for m in "sm":
            c = S.Case(x, m)
            for b in range(n // 64):
                c.block(((b * 397) % 32768, (b * 211) % 32768, (b * 53) % 32768, (b * 101) % 32768),
                        key=1 if 150 < b < 180 else 0)
            cases["sweep" + m] = c
        for rate, defs in builds:
            ra, rc = run(cases, defs, True), run(cases, defs, False)
            bad = [k for k in cases if not np.array_equal(ra[k][0], rc[k][0])]
            check(not bad, f"{rate} kHz master: the asm kernels = the C reference, sample for sample, in "
                  f"{len(cases)} cases (spaces, FREEZE, INFINITE, shimmer, every knob moving, the green key)"
                  + (f": {bad}" if bad else ""))

    # ---- 2, 3. RT60 per band, and the prototype ----
    sets = [(nm, (k, 16384, 5120, 16384)) for nm, k in SPACES] + [
        ("HALL dark", (8192, 16384, 0, 16384)), ("HALL bright", (8192, 16384, 10239, 16384)),
        ("ROOM x1/4", (0, 0, 5120, 16384)), ("CATHEDRAL x4", (24576, 30719, 5120, 16384))]
    if a.quick:
        sets = sets[:3]
    B = int(0.5 * FS)
    bursts = [pink(B, 0.35, seed) for seed in (3, 4)]   # two seeds, ensemble-averaged
    proto = {}
    for nm, k in sets:
        p = target(k)
        dur = max(4, 2.2 * p["t60_lo"])
        hs = []
        for burst in bursts:
            xx = np.zeros((B + int(dur * FS), 2))
            xx[:B] = burst
            y, _ = P.run(dict(p, dry=0.0, wet=1.0), xx)
            hs.append(y[B:])
        proto[nm] = (rt60s([y[:, 0] + y[:, 1] for y in hs]), band_levels(hs[0][:, 0] + hs[0][:, 1]),
                     band_levels(hs[0][:, 0]))
    for rate, defs in builds:
        for mode in "sm":
            if mode == "m" and defs:
                continue                    # the mono tank doesn't depend on the build option
            cases = {}
            for nm, k in sets:
                p = target(k)
                for j, burst in enumerate(bursts):
                    xx = np.zeros((LEAD + B + int(max(4, 2.2 * p["t60_lo"]) * FS), 2))
                    xx[LEAD:LEAD + B] = burst * 32767
                    c = S.Case(xx, mode)
                    c.fill(k)
                    cases[nm.replace(" ", "_").replace("/", "") + f"_{j}"] = c
            res = run(cases, defs)
            label = "mono 22.05 kHz" if mode == "m" else f"stereo {rate} kHz"
            print(f"  RT60 (s), {label}: measured / target / prototype, octave bands {BANDS}")
            for nm, k in sets:
                hs = []
                for j in range(len(bursts)):
                    out = res[nm.replace(" ", "_").replace("/", "") + f"_{j}"][0][LEAD + B:].astype(float)
                    hs.append(out[:, 0] + (out[:, 1] if mode == "s" else 0))
                h = hs[0]
                m, (pr, pl, pl1) = rt60s(hs), proto[nm]
                if mode == "m":
                    pl = pl1                        # mono: the prototype's L (one output row, as the mono FX)
                t = [P.target_band(target(k), fc) for fc in BANDS]
                print(f"    {nm:<13} " + "  ".join(f"{a_:.2f}/{b_:.2f}/{c_:.2f}" for a_, b_, c_ in zip(m, t, pr)))
                if "x1/4" in nm:            # the diffusers' own ring floors it (~0.22 s), as in the prototype
                    check(all(v < 0.3 for v in m[:5]), f"{label} {nm}: RT60 {max(m[:5]):.2f} s at most (the "
                          "diffusers' floor; the prototype too)")
                    continue
                # the target: within 10 % at 125 Hz .. 2 kHz (the 22.05 kHz tanks: to 1 kHz, and 15 % at 2 kHz, where
                # linear interpolation on the modulated lines darkens a band-limited tank); the TONE variants against
                # the prototype only (the two-band model's first-order slope misses dark's crossover by ~10 %)
                half = mode == "m" or rate == "22.05"
                lim = [0.10] * 4 + [0.15 if half else 0.10]
                dev = [abs(a_ / b_ - 1) for a_, b_ in zip(m[:5], t[:5])]
                devp = [abs(a_ / b_ - 1) for a_, b_ in zip(m[:5], pr[:5])]
                if "dark" not in nm and "bright" not in nm:
                    check(all(d <= l for d, l in zip(dev, lim)), f"{label} {nm}: RT60 within "
                          + " ".join(f"{100 * d:.0f}" for d in dev) + " % of the target at 125 Hz .. 2 kHz "
                          f"(10 % allowed{', 15 at 2 kHz' if half else ''})")
                check(all(d <= l for d, l in zip(devp, lim)), f"{label} {nm}: within "
                      + " ".join(f"{100 * d:.0f}" for d in devp) + " % of the float prototype")
                lv = band_levels(h)
                dl = max(abs(u - w) for u, w in zip(lv[:6], pl[:6]))
                check(dl <= (4.0 if "dark" in nm else 3.0), f"{label} {nm}: the tail's spectrum within {dl:.1f} dB of the prototype's (octave "
                      "bands 125 Hz .. 4 kHz relative to 1 kHz: " + " ".join(f"{u - w:+.1f}" for u, w in zip(lv[:6], pl[:6]))
                      + (")" if "dark" not in nm else "; 4 dB allowed: dark also darkens the ERs here, not there)"))

    # ---- 4. early reflections ----
    cases = {}
    for nm, k in SPACES[:4]:
        xx = np.zeros((LEAD + FS // 2, 2))
        xx[LEAD] = 20000
        c = S.Case(xx, "s")
        c.fill((k, 16384, 5120, 16384))
        cases[nm] = c
    res = run(cases)
    for nm, k in SPACES[:4]:
        w = wet_of(res[nm][0], cases[nm].inp)[LEAD:, 0]
        s_ = P.space_at(k)
        first = (s_["pre"] + s_["er_t"][0] * min(s_["size"], 1.6) ** 0.5) * FS / 1000
        nz = np.nonzero(np.abs(w) > 30)[0]
        t0 = nz[0] if len(nz) else -1
        er = np.sqrt(np.mean(w[int(first):int(first + 0.04 * FS)] ** 2))
        check(abs(t0 - first) <= 3 and er > 20, f"{nm}: the first reflection at {t0} samples (predelay + tap "
              f"{first:.0f}), none before; ER rms {er:.0f} in the 40 ms after")

    # ---- 5, 6. the extremes, FREEZE, INFINITE ----
    T = 20 if a.quick else 60
    rng = np.random.default_rng(2)
    nb = LEAD + int((T + 1) * FS)
    bst = np.zeros((nb, 2))
    bst[LEAD:LEAD + B] = rng.uniform(-0.5, 0.5, (B, 2)) * 32767
    ext = {"void4_sh12": ((32767, 30719, 14000, 16384), None), "void4_sh19": ((32767, 30719, 18000, 16384), None),
           "cath4_sh12": ((24576, 30719, 14000, 16384), None), "inf_sh12": ((24576, 31000, 14000, 16384), None),
           "void4_sub": ((32767, 30719, SUB_TOP, 16384), None), "inf_sub": ((24576, 31000, SUB_TOP, 16384), None),
           "frz_hall": ((8192, 16384, 5120, 16384), (8192, 32767, 5120, 16384)),
           "frz_sh12": ((24576, 16384, 12800, 16384), (24576, 32767, 12800, 16384)),
           "frz_sub": ((24576, 16384, SUB_TOP, 16384), (24576, 32767, SUB_TOP, 16384))}
    for rate, defs in builds:
        for mode in "sm":
            if mode == "m" and defs:
                continue
            cases = {}
            for nm, (k, k2) in ext.items():
                c = S.Case(bst, mode)
                for b in range(nb // 64):
                    c.block(k2 if k2 and b * 64 >= LEAD + int(0.6 * FS) else k)
                cases[nm] = c
            res = run(cases, defs + " -DSPACE_DIAG")
            label = "mono" if mode == "m" else f"stereo {rate} kHz"
            for nm in ext:
                out, st, _ = res[nm]
                w = out[LEAD + B:, 0].astype(float)
                win = 5 * FS
                r = [20 * math.log10(math.sqrt(np.mean(w[i:i + win] ** 2)) + 1e-9)
                     for i in range(0, len(w) - win + 1, win)]
                pk = st.get("peak", 0)
                if nm.startswith("frz"):
                    rr = [20 * math.log10(math.sqrt(np.mean(w[i:i + FS] ** 2)) + 1e-9)
                          for i in range(2 * FS, len(w) - FS + 1, FS)]
                    check(max(rr) - min(rr) <= 2.0, f"{label} {nm}: frozen {T - 2} s, the level within "
                          f"{max(rr) - min(rr):.1f} dB (per second, from 2 s after the burst)")
                else:
                    check(max(r[1:]) <= r[0] + 1.0 and pk < 32767, f"{label} {nm}: {T} s, rms per 5 s "
                          + " ".join(f"{v:.0f}" for v in r) + f" dBFS: bounded, line peak {pk}")
    # INFINITE layering and headroom
    t1, t2 = 1.0, 4.0
    nn = LEAD + int(10 * FS)
    tt = np.arange(nn) / FS
    ch1 = sum(np.sin(2 * np.pi * f * tt) for f in (220.0, 277.2, 329.6)) / 3
    ch2 = sum(np.sin(2 * np.pi * f * tt) for f in (392.0, 493.9)) / 2
    lay = np.zeros(nn)
    on1 = (tt >= LEAD / FS + 0.1) & (tt < LEAD / FS + t1)
    on2 = (tt >= LEAD / FS + t2) & (tt < LEAD / FS + t2 + 0.9)
    lay[on1] = ch1[on1] * 16000
    lay[on2] = ch2[on2] * 16000
    pad = np.zeros(nn)
    pad[LEAD:LEAD + 4 * FS] = (((110 * tt[LEAD:LEAD + 4 * FS]) % 1) * 2 - 1) * 32767
    cases = {}
    for nm, sig in (("layer", lay), ("pad0db", pad)):
        c = S.Case(sig, "s")
        c.fill((8192, 31000, 5120, 16384))
        cases[nm] = c
    res = run(cases, "-DSPACE_DIAG")
    w = wet_of(res["layer"][0], cases["layer"].inp)[-2 * FS:, 0]
    sp = np.abs(np.fft.rfft(w * np.hanning(len(w))))
    fr = np.fft.rfftfreq(len(w), 1 / FS)

    def at(f):
        return 20 * math.log10(sp[(fr > f - 3) & (fr < f + 3)].max() / (np.median(sp) + 1e-9))
    lv = [at(f) for f in (220.0, 277.2, 329.6, 392.0, 493.9)]
    check(min(lv) > 20, "INFINITE (HALL): a chord, then another 3 s later: at the end both are held (each partial "
          + ", ".join(f"{v:.0f}" for v in lv) + " dB over the median)")
    pk = res["pad0db"][1].get("peak", 0)
    check(pk < 16384, f"INFINITE on a 0 dBFS saw pad: line peak {pk / 32768:.2f} FS (< 0.5 FS)")

    # ---- 7. exact silence, no limit cycle ----
    T2 = 25 if a.quick else 45
    nb = LEAD + B + int(T2 * FS)
    bst = np.zeros((nb, 2))
    bst[LEAD:LEAD + B] = rng.uniform(-0.6, 0.6, (B, 2)) * 32767
    sil = [(nm, (k, 16384, 5120, 16384)) for nm, k in SPACES[:4]] + [
        ("HALL x4", (8192, 30719, 5120, 16384)), ("CATHEDRAL x4", (24576, 30719, 5120, 16384)),
        ("ROOM dark", (0, 16384, 0, 16384)), ("HALL bright", (8192, 16384, 10239, 16384))]
    for rate, defs in builds:
        for mode in "sm":
            if mode == "m" and defs:
                continue
            cases = {}
            for nm, k in sil:
                c = S.Case(bst, mode)
                c.fill(k)
                cases[nm.replace(" ", "_")] = c
            res = run(cases, defs)
            label = "mono" if mode == "m" else f"stereo {rate} kHz"
            for nm, k in sil:
                out = res[nm.replace(" ", "_")][0][LEAD + B:].astype(np.int64)
                nzi = np.nonzero(np.any(out != 0, axis=1))[0]
                end = (nzi[-1] + 1) / FS if len(nzi) else 0.0
                # no limit cycle: once the tail is below -90 dBFS (100 ms rms) it never comes back above
                w = FS // 10
                r = [20 * math.log10(math.sqrt(np.mean(out[i:i + w].astype(float) ** 2)) / 32768 + 1e-12)
                     for i in range(0, len(out) - w + 1, w)]
                below = next((i for i, v in enumerate(r) if v < -90), len(r))
                back = max(r[below:], default=-240)
                check(end < T2 - 2 and back < -90, f"{label} {nm}: exact silence {end:.1f} s after the burst; below "
                      f"-90 dBFS from {below / 10:.1f} s, never above again (max {back:.0f})")

    # ---- 8. DC ----
    nd = LEAD + 4 * FS
    xd = np.zeros((nd, 2))
    xd[LEAD:LEAD + 2 * FS] = 8000 + np.random.default_rng(5).uniform(-6000, 6000, (2 * FS, 2))
    cases = {"dc": S.Case(xd, "s"), "dcm": S.Case(xd, "m")}
    for c in cases.values():
        c.fill((16384, 16384, 5120, 16384))
    res = run(cases)
    for nm in cases:
        w = wet_of(res[nm][0], xd)[LEAD + FS // 2:LEAD + 2 * FS, 0]
        tl = res[nm][0][LEAD + 2 * FS + FS // 4:, 0].astype(float)
        dcin = xd[LEAD:LEAD + 2 * FS, 0].mean()
        check(abs(w.mean()) < 0.5 * dcin and abs(tl.mean()) < 30, f"{'stereo' if nm == 'dc' else 'mono'}: a 0.24 FS DC "
              f"offset in: the wet's DC {w.mean() / dcin:+.2f} of it while playing (the tank's input low-cut, the ERs "
              f"DC-blocked), the tail's {tl.mean():+.1f} LSB")

    # ---- 9. MIX ----
    nm_ = LEAD + 5 * FS
    tm = np.arange(nm_) / FS
    sig = np.zeros((nm_, 2))
    sig[LEAD:LEAD + 2 * FS] = (np.sin(2 * np.pi * 330 * tm[LEAD:LEAD + 2 * FS]) * 12000)[:, None]
    cases = {}
    for m in "sm":
        for mix in (0, 8192, 16384, 17408, 32767):
            c = S.Case(sig, m)
            c.fill((8192, 16384, 5120, mix))
            cases[f"mix{m}{mix}"] = c
    res = run(cases)
    for m in "sm":
        o0, i0 = res[f"mix{m}0"][0], cases[f"mix{m}0"].inp
        check(np.array_equal(o0[:len(i0)], i0.astype(np.int64)), f"{'master' if m == 's' else 'synth'} MIX 0: "
              "the input, bit exact")
        win = slice(LEAD + FS, LEAD + 2 * FS)
        w1 = wet_of(res[f"mix{m}16384"][0], sig)[win, 0]
        w5 = wet_of(res[f"mix{m}8192"][0], sig)[win, 0]
        r = 20 * math.log10(np.std(w5) / np.std(w1))
        check(abs(r + 6.0) < 0.5, f"{'master' if m == 's' else 'synth'} MIX 8192: the wet {r:+.1f} dB vs the middle "
              "(-6 expected), the dry at unity")
        if m == "s":
            for mix, want in ((17408, -12.0), (32767, -24.0)):
                wd = wet_of(res[f"mixs{mix}"][0], sig)[win, 0]
                d = 20 * math.log10(np.std(wd) / np.std(w1))
                check(abs(d - want) < 1.0, f"master MIX {mix}: the wet ducks {d:+.1f} dB while playing ({want:+.0f})")
            wa = res["mixs32767"][0][LEAD + 4 * FS:LEAD + 4 * FS + FS // 2, 0].astype(float)
            wb = res["mixs16384"][0][LEAD + 4 * FS:LEAD + 4 * FS + FS // 2, 0].astype(float)
            d = 20 * math.log10(np.std(wa) / np.std(wb))
            check(abs(d) < 1.0, f"master MIX top: 2 s after the playing stops the tail is back ({d:+.1f} dB; the duck "
                  "holds 150 ms, then recovers over ~0.4 s)")
        else:
            o = res["mixm32767"][0][win, 0].astype(float)
            w = wet_of(res["mixm16384"][0], sig)[win, 0]
            c_ = np.corrcoef(o, w)[0, 1]
            check(c_ > 0.999, f"synth MIX top: the dry gone, the output is the wet (correlation {c_:.4f})")

    # ---- 10. zipper ----
    nz_ = LEAD + 5 * FS
    tz = np.arange(nz_) / FS
    tone = (np.sin(2 * np.pi * 220 * tz) * 9000)[:, None] * np.ones((1, 2))
    tone[:LEAD] = 0
    ramps = {"space": (0, 0, 32767), "decay": (1, 0, 30719), "tail": (2, 0, 32767), "mix": (3, 0, 32767),
             "decay_holds": (1, 29000, 32767)}
    base = [8192, 16384, 5120, 16384]
    cases = {}
    for m in "sm":
        for nm, (ki, k0, k1) in ramps.items():
            c = S.Case(tone, m)
            nbk = nz_ // 64
            for b in range(nbk):
                k = list(base)
                rb = b - (LEAD // 64 + FS // 64)
                k[ki] = k0 if rb < 0 else min(k1, k0 + (rb // 4) * 1024) if k1 > k0 else k0
                c.block(k)
            cases[f"z{m}_{nm}"] = c
        c = S.Case(tone, m)
        c.fill(base)
        cases[f"z{m}_static"] = c
    res = run(cases)
    for m in "sm":
        ref = res[f"z{m}_static"][0][LEAD + FS:, 0].astype(float)

        def hfmax(x):
            d = np.diff(x, 2)
            return max(np.sqrt(np.mean(d[i:i + 441] ** 2)) for i in range(0, len(d) - 441, 441))
        r0 = hfmax(ref)
        for nm in ramps:
            x = res[f"z{m}_{nm}"][0][LEAD + FS:, 0].astype(float)
            r1 = hfmax(x)
            check(r1 < 2.5 * r0 + 40, f"{'master' if m == 's' else 'synth'} {nm} turned detent by detent: the largest "
                  f"10 ms of the output's 2nd difference {r1:.0f} (static {r0:.0f}): no click, no zipper")

    # ---- 11. shared lines ----
    nh = LEAD + 4 * FS
    sh = np.zeros((nh, 2))
    sh[LEAD:LEAD + FS] = np.random.default_rng(7).uniform(-12000, 12000, (FS, 2))
    cases = {}
    for m in "sm":
        c = S.Case(np.zeros((nh, 2)), m)
        c.fill((32767, 30719, 5120, 32767))     # first call ever: the lines hold the boot garbage
        cases[f"boot{m}"] = c
        for other in "pr":
            c = S.Case(sh, m)
            nb_ = nh // 64
            for b in range(nb_):
                if b < nb_ // 2:
                    c.block((28000, 30000, 16384, 32767) if other == "p" else (12288, 0, 9216, 16384), fx=other)
                else:
                    c.block((32767, 30719, 5120, 32767), fx="s")
            cases[f"{other}2s{m}"] = c
            c = S.Case(sh, m)
            for b in range(nb_):
                if b < nb_ // 2:
                    c.block((32767, 30719, 5120, 32767), fx="s")
                else:
                    c.block((28000, 30000, 16384, 32767) if other == "p" else (12288, 0, 9216, 16384), fx=other)
            cases[f"s2{other}{m}"] = c
    res = run(cases)
    for m in "sm":
        tag = "master" if m == "s" else "synth"
        check(np.abs(res[f"boot{m}"][0][:, 0]).max() == 0, f"{tag}: the boot garbage in the lines is never heard "
              "(silence in, silence out, VOID x4 all wet)")
        for other, name in (("p", "pingpong"), ("r", "repeat")):
            o = res[f"{other}2s{m}"][0][(nh // 128) * 64:, 0]
            st = res[f"{other}2s{m}"][1]
            check(np.abs(o).max() == 0 and st["owner" + ("1" if m == "s" else "0")][-1] == 3,
                  f"{tag}: space after {name}: nothing {name} left plays (silence in, exact silence out); "
                  "the owner word is space's")
            o = res[f"s2{other}{m}"][0][(nh // 128) * 64:, 0]
            check(np.abs(o).max() == 0, f"{tag}: {name} after space: nothing space left plays")

    # ---- 12. re-enable, the key ----
    ne = LEAD + 4 * FS
    xe = np.zeros((ne, 2))
    xe[LEAD:LEAD + FS // 2] = np.random.default_rng(8).uniform(-12000, 12000, (FS // 2, 2))
    cases = {}
    for m in "sm":
        c = S.Case(xe, m)
        for b in range(ne // 64):
            off = (LEAD + FS) // 64 <= b < (LEAD + FS) // 64 + 20
            c.block((24576, 30719, 5120, 32767), fx="-" if off else "s", gap=100 if b == (LEAD + FS) // 64 + 20 else 0)
        cases[f"re{m}"] = c
        c = S.Case(xe, m)
        for b in range(ne // 64):
            c.block((8192, 16384, 5120, 16384), key=1 if (LEAD + FS // 4) // 64 <= b < (LEAD + 3 * FS) // 64 else 0)
        cases[f"key{m}"] = c
    res = run(cases)
    for m in "sm":
        tag = "master" if m == "s" else "synth"
        o = res[f"re{m}"][0][(LEAD + FS) // 64 * 64 + 20 * 64:, 0]
        st = res[f"re{m}"][1]
        check(np.abs(o).max() == 0 and st["clearing"][(LEAD + FS) // 64 + 21] == 1,
              f"{tag}: switched off for 100+ blocks and on again: a clean start (the line set cleared, the old tail gone)")
        if sim:
            w = res[f"key{m}"][0][:, 0].astype(float)
            a1 = np.sqrt(np.mean(w[LEAD + int(2.0 * FS):LEAD + int(2.5 * FS)] ** 2))
            a2 = np.sqrt(np.mean(w[LEAD + int(3.6 * FS):LEAD + int(4.0 * FS)] ** 2))
            st = res[f"key{m}"][1]
            check(a1 > 300 and st["frz"][(LEAD + 2 * FS) // 64] == 32768 and st["frz"][-1] == 0 and a2 < a1 / 8,
                  f"{tag}: the green push held freezes the HALL tail ({20 * math.log10(a1 / 32768):.0f} dBFS 1.75 s "
                  f"on), let go it decays ({20 * math.log10(a2 / 32768 + 1e-9):.0f} dBFS 1 s after)")

    # ---- 13. the LFO on white keeps the zone ----
    cases = {}
    c = S.Case(np.zeros((LEAD + 2 * FS, 2)), "m")
    for b in range((LEAD + 2 * FS) // 64):
        c.block((8192, 16384, 9000 + int(4000 * math.sin(b / 20)), 16384), stored=9000)   # TONE; the LFO into +12
    cases["lfo_tone"] = c
    res = run(cases)
    z = res["lfo_tone"][1]["zone"][LEAD // 64:]
    check(set(z.tolist()) == {0}, f"synth: TE's LFO swinging white from TONE into SHIMMER +12 (stored TONE): the zone "
          f"stays TONE ({sorted(set(z.tolist()))})")

    # the screen's copy of the RT60 per detent = the generated table
    import re
    t = open(os.path.join(S.SLOT, "space_tables.h")).read().split("sp_pt[33][SP_NF] = {")[1].split("};")[0]
    gen = [int(r.split(",")[3]) for r in re.findall(r"\{ ([-0-9, ]+) \}", t)]
    h = open(os.path.join(S.SLOT, "space.h")).read().split("sp_l2t60_at[33] = {")[1].split("};")[0]
    check(gen == [int(v) for v in re.findall(r"-?\d+", h)], "space.h sp_l2t60_at = space_tables.h's RT60 column (the "
          "screen's DECAY seconds)")

    # ---- 14. CYCLES ----
    if sim:
        cycles(run, builds)

    # ---- 15. the v1.1 zones ----
    zones(run, a.quick)

    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
