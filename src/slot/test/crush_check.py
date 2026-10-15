#!/usr/bin/env python3
"""Simulator checks for the crush FX (src/slot/crush.c) before flashing.

  python3 src/slot/test/crush_check.py

Runs every case on the Blackfin simulator (crushsim.py, test/crush_main.c) and checks: the clean setting (every knob
at 0) is the input, bit exact, also after the knobs come back; stereo with L = R equals mono; aliasing (a tone above
half the hold rate folds back and moves against the pitch; none when clean); the bit-depth steps (linear 12 / 4 / 2
bits, the 1-bit square with its dead zone); mu-law in the middle of BITS (quietest step ~16 LSB = ~12-bit, largest
~2000); the compressor (gain reduction on a loud burst, make-up neutral at -6 dBFS, release time, quiet tails pushed
up, no reduction at SQUASH 0); GRIT (clock jitter spreads the spectrum more as it rises, no flips below half, flips
above, silence stays silent); level compensation over BITS x RATE; no DC (100 Hz, an asymmetric pulse at 1 bit, after
the input stops); no zipper on knob moves (encoder detents of 1024 per block, and jumps); CYCLES per 64-frame call.
"""
import os
import re
import sys

import numpy as np

from crushsim import FS, SLOT, rms_db, run, saw, sine

BIG = 32767
TYPICAL_BUDGET = 18000            # stereo, per call, typical settings (distortion: ~29k typical)
WORST_BUDGET = 30000              # stereo, any setting (distortion: 44.5k worst)
N = 8820                          # analysis length (0.2 s)


def rate_k(hz):
    """RATE knob for a hold rate in Hz: 44.1 kHz * 2^(-6.785 k)."""
    return int(round(np.log2(44100 / hz) / 6.785 * 32768))


def bits_k(b):
    """BITS knob for b bits: 16 -> 7 over the first half, 7 -> 1 over the second."""
    return int(round((16 - b) / 18 * 32768)) if b >= 7 else min(BIG, int(round((0.5 + (7 - b) / 12) * 32768)))


def comp_gain(kb, kr=0):
    """Output level compensation at BITS kb, RATE kr: crush.c's comp_bits / comp_rate tables (Q12), as the DSP
    interpolates them."""
    src = open(os.path.join(SLOT, "crush.c")).read()
    t = {n: [int(v) for v in re.search(n + r"\[17\] = \{([^}]*)\}", src).group(1).replace("\n", " ").split(",")]
         for n in ("comp_bits", "comp_rate")}

    def tab(v, k):
        i, f = k >> 11, k & 2047
        return v[i] + (((v[i + 1] - v[i]) * f) >> 11)
    return (tab(t["comp_bits"], kb) * tab(t["comp_rate"], kr) >> 11) / 8192


def spectrum(x):
    x = np.asarray(x[-N:], float)
    w = np.hanning(len(x))
    return np.abs(np.fft.rfft(x * w)) / (w.sum() / 2), np.fft.rfftfreq(len(x), 1 / FS)


def line_db(x, f, ref=None):
    s, fr = spectrum(x)
    k = np.argmin(np.abs(fr - f))
    v = s[max(0, k - 2):k + 3].max()
    return 20 * np.log10(v / (ref if ref else 16384) + 1e-12)


def pulse(n, f, duty, amp=16384):
    """A band-limited pulse wave without DC (asymmetric: duty != 0.5)."""
    t = np.arange(n)
    y = sum(np.sin(np.pi * k * duty) / k * np.cos(2 * np.pi * k * f * t / FS) for k in range(1, int(20000 / f) + 1))
    y -= y.mean()
    return amp * y / np.abs(y).max()


def main():
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    L = 64 * 300
    x1k, x440, xsaw = sine(L, 1000), sine(L, 440), saw(L, 220)
    st = np.stack([sine(L, 1000), sine(L, 1000, quarter=True)], 1)
    C = {}
    # clean
    C["clean_m"] = ("m", (0, 0, 0, 0), xsaw)
    C["clean_s"] = ("s", (0, 0, 0, 0), st)
    C["back_m"] = ("m", (0, 0, 0, 0), xsaw, ["j:20:20000:20000:20000:20000", "j:100:0:0:0:0"])
    C["lr_m"] = ("m", (6000, 20000, 20000, 20000), xsaw)
    C["lr_s"] = ("s", (6000, 20000, 20000, 20000), np.stack([xsaw, xsaw], 1))
    # aliasing: hold at 8 kHz, no anti-alias filter
    ka = rate_k(8000)
    for f in (5000, 6000):
        C[f"alias_{f}"] = ("m", (ka, 0, 0, 0), sine(L, f))
        C[f"clean_{f}"] = ("m", (0, 1, 0, 0), sine(L, f))
    # bit depth (RATE top: every sample a hold); a slow full-scale sine, and a quiet one for mu-law's first step
    slow, quiet = sine(L, 200, 28000), sine(L, 30, 300)     # +-28000: no level reaches the clip at 32767
    for b in (4, 3, 3.5):          # linear, mid-tread (mu-law only between 11.5 and 4 bits, mid-rise only below 3)
        C[f"bits_{b}"] = ("m", (0, bits_k(b), 0, 0), slow)
    C["bits_12"] = ("m", (0, bits_k(12), 0, 0), sine(L, 30, 2000))
    C["bits_1"] = ("m", (0, BIG, 0, 0), x1k)
    C["bits_1q"] = ("m", (0, BIG, 0, 0), sine(L, 1000, 1500))
    C["mu"] = ("m", (0, 16384, 0, 0), slow)
    C["mu_q"] = ("m", (0, 16384, 0, 0), quiet)
    C["lin7_q"] = ("m", (0, bits_k(4), 0, 0), quiet)
    # compressor: -6 dBFS for half the run, then -54 dBFS
    burst = sine(L, 1000) * np.where(np.arange(L) < L // 2, 1, 1 / 256)
    C["comp_max"] = ("m", (0, 0, BIG, 0), burst)
    C["comp_mid"] = ("m", (0, 0, 16384, 0), burst)
    C["comp_off"] = ("m", (0, 1, 0, 0), burst)
    # GRIT: jitter spreads the hold clock's lines (hold at 4 kHz, 440 Hz in); flips above half
    kj = rate_k(4000)
    for g in (0, 1024, 4096, 8192, 16384):
        C[f"jit_{g}"] = ("m", (kj, 0, 0, g), sine(L, 1500))
    C["sizzle_0"] = ("m", (0, 1, 0, 0), x440)
    C["sizzle_1"] = ("m", (0, 1, 0, 16384), x440)
    for g in (12000, 16384, 24576, BIG):
        C[f"flip_{g}"] = ("m", (0, bits_k(8), 0, g), x1k)
    C["flip_silence"] = ("m", (kj, bits_k(6), BIG, BIG), np.zeros(L))
    # level: BITS x RATE grid, two signals
    LV_B = (0, 8192, 16384, 24576, BIG)
    LV_R = (0, 8192, 16384, 24576, BIG)
    for kb in LV_B:
        for kr in LV_R:
            C[f"lv_{kb}_{kr}_s"] = ("m", (kr, kb, 0, 0), x440)
            C[f"lv_{kb}_{kr}_w"] = ("m", (kr, kb, 0, 0), xsaw)
    # DC
    LD = 64 * 800
    x100 = sine(LD, 100)
    for name, k in {"bits1": (0, BIG, 0, 0), "mu": (0, 16384, 0, 0), "rate": (BIG, 0, 0, 0),
                    "flips": (rate_k(2000), bits_k(5), 16384, BIG), "all": (BIG, BIG, BIG, BIG)}.items():
        C[f"dc_{name}"] = ("m", k, x100)
    C["dc_pulse"] = ("m", (0, BIG, 0, 0), pulse(LD, 100, 0.25))
    C["dc_pulse_r"] = ("m", (rate_k(3000), bits_k(3), 20000, 0), pulse(LD, 100, 0.25))
    stop = sine(L, 200) * (np.arange(L) < 64 * 60)
    C["stop"] = ("m", (rate_k(3000), bits_k(3), BIG, 20000), stop)
    # zipper: encoder detents (1024 per block every 2 blocks, 16 detents) on a 200 Hz sine; jumps; with references
    # at the settings passed through
    x200 = sine(L, 200)
    TURNS = {"rate": (0, (0, 0, 0, 0)), "bits": (1, (0, 0, 0, 0)), "squash": (2, (0, 0, 0, 0)),
             "rate_lo": (0, (16384, 0, 0, 0)), "bits_mu": (1, (0, 8192, 0, 0))}
    REFS = {}
    for name, (k, base) in TURNS.items():
        C[f"turn_{name}"] = ("m", base, x200, [f"t:{k}:1024:2:100:16"])
        REFS[name] = []
        for d in range(0, 17, 2):
            kk = list(base)
            kk[k] = min(BIG, kk[k] + 1024 * d)
            C[f"ref_{name}_{d}"] = ("m", tuple(kk), x200)
            REFS[name].append(f"ref_{name}_{d}")
    C["jump_all"] = ("m", (0, 0, 0, 0), x200, ["j:100:20000:12000:20000:0"])
    # cycles: stereo, typical and corner settings
    TYP = {"default": (14000, 16384, 12000, 4000), "warm": (20000, 8000, 8000, 0), "lofi": (10000, 22000, 16000, 6000),
           "full_mu": (0, 16384, 0, 0), "full_lin": (0, 6000, 0, 0), "sp1200": (rate_k(26040), bits_k(12), 8000, 0)}
    for n, k in TYP.items():
        C[f"cpu_{n}"] = ("s", k, st)
    for kr in (0, 300, 2048):
        for kb in (8192, 16384, 30000):
            for kg in (0, 12000, BIG):
                C[f"worst_{kr}_{kb}_{kg}"] = ("s", (kr, kb, BIG, kg), st)

    out, cyc = run(C)
    y = {k: v[0] for k, v in out.items()}
    gr = {k: v[1] for k, v in out.items()}

    # ---- clean ----
    r = np.round
    ok = np.array_equal(y["clean_m"][:, 0], r(xsaw).astype(int)) and np.array_equal(y["clean_s"], r(st).astype(int))
    check(ok, "clean (every knob 0): the input, bit exact (mono saw, stereo sine)")
    tail = slice(64 * 200, None)
    ok = np.array_equal(y["back_m"][tail, 0], r(xsaw).astype(int)[tail])
    check(ok, "back to clean after the knobs were up: bit exact again once the smoothing has settled")
    check(np.array_equal(y["lr_s"][:, 0], y["lr_m"][:, 0]) and np.array_equal(y["lr_s"][:, 1], y["lr_m"][:, 0]),
          "stereo with L = R: both channels equal the mono instance (linked clock and compressor)")

    # ---- aliasing ----
    a5 = line_db(y["alias_5000"][:, 0], 3000) - line_db(y["alias_5000"][:, 0], 5000)
    a6 = line_db(y["alias_6000"][:, 0], 2000) - line_db(y["alias_6000"][:, 0], 6000)
    c5 = line_db(y["clean_5000"][:, 0], 3000)
    check(a5 > -6 and a6 > -6 and c5 < -90, f"aliasing, hold at 8 kHz: 5 kHz in -> 3 kHz alias {a5:+.1f} dB re 5 kHz, "
          f"6 kHz in -> 2 kHz alias {a6:+.1f} dB (the alias falls as the pitch rises); RATE top: 3 kHz at {c5:.0f} dBFS")

    # ---- bit depth: at full rate the sample-to-sample steps are multiples of the step size (times the level
    # compensation); the DC blocker tilts the treads by at most amplitude / 2048 per sample, so smaller steps are drift
    def steps(v, drift):
        d = np.abs(np.diff(v[64 * 150:, 0]))                     # after the knob smoothing has settled
        return d[d > 2 * drift + 2]

    for b, amp in ((12, 2000), (4, 28000), (3.5, 28000), (3, 28000)):
        step = 2 ** (16 - b) * comp_gain(bits_k(b))
        drift = amp / 2048
        name = f"bits_{b}"
        d = steps(y[name], drift)
        frac = np.mean(np.abs(d - step * np.round(d / step)) <= 2 * drift + 2)
        lv = (y[name][64 * 150:, 0].max() - y[name][64 * 150:, 0].min()) / step
        check(frac > 0.99 and abs(d.min() - step) <= 2 * drift + 2,
              f"{b} bits: smallest step {d.min()} (2^{16 - b:g} x level compensation = {step:.0f}); {100 * frac:.1f} % "
              f"of the steps are multiples of it; {lv + 1:.0f} levels over a +-{amp} sine")
    v1 = y["bits_1"][64 * 150:, 0]
    a1 = 16384 * comp_gain(BIG)
    near = np.minimum(np.abs(v1), np.abs(np.abs(v1) - a1))
    zero = np.mean(np.abs(v1) < 100)
    vq = np.abs(y["bits_1q"][64 * 150:, 0]).max()
    check(near.max() < 100 and 0.05 < zero < 0.25 and vq <= 2,
          f"1 bit: a square at +-{np.abs(v1).max()} (16384 x level compensation = {a1:.0f}), {100 * zero:.0f} % in the "
          f"dead zone (|x| < 1/8 of full scale: 16 % of a -6 dBFS sine); a -27 dBFS sine gates to silence (peak {vq})")

    # ---- mu-law in the middle of BITS ----
    dq_ = steps(y["mu_q"], 1)
    dm = steps(y["mu"], 16)
    dl = steps(y["lin7_q"], 1)
    check(12 <= dq_.min() <= 20 and 1800 <= dm.max() <= 2300,
          f"mu-law at the centre (7-bit codes, mu 255, G.711 segments): quietest step {dq_.min()} LSB (~"
          f"{16 - np.log2(dq_.min()):.1f}-bit resolution for quiet signals), largest {dm.max()} (top segment, ~5 bits); "
          f"linear 4 bits on the same quiet sine: {'silent' if len(dl) == 0 else f'smallest step {dl.min()}'}")

    # ---- compressor ----
    h = L // 2

    def lvl(v, a, b):
        return rms_db(v[a:b, 0])
    g_loud = gr["comp_max"][h - 64 * 10:h].mean()
    loud = lvl(y["comp_max"], h - N, h) - rms_db(burst[h - N:h])
    after = gr["comp_max"][h:] / 32767 * 30
    gr_db0 = g_loud / 32767 * 30
    half_ms = np.argmax(after < gr_db0 / 2) / FS * 1000
    gone_ms = np.argmax(after < 0.5) / FS * 1000
    up = lvl(y["comp_max"], L - N, L) - rms_db(burst[L - N:L])
    check(gr_db0 > 25 and abs(loud) < 1.5, f"SQUASH max, -6 dBFS: gain reduction {gr_db0:.1f} dB (screen value "
          f"{g_loud:.0f} / 32767), output level {loud:+.2f} dB (make-up neutral at -6 dBFS)")
    check(30 < half_ms < 150 and 0 < gone_ms < 400, f"release after a 48 dB drop: gain reduction halves in {half_ms:.0f} ms, "
          f"under 0.5 dB after {gone_ms:.0f} ms (fast, pumps)")
    upm = lvl(y["comp_mid"], L - N, L) - rms_db(burst[L - N:L])
    check(up > 20 and 3 < upm < up, f"quiet tail (-54 dBFS) pushed up: +{up:.1f} dB at SQUASH max, +{upm:.1f} dB at half")
    check(gr["comp_off"].max() == 0 and gr["comp_mid"][h - 640:h].mean() > 1000,
          f"SQUASH 0: no gain reduction (max {gr['comp_off'].max()}); half: {gr['comp_mid'][h - 640:h].mean() / 32767 * 30:.1f} dB")

    # ---- GRIT ----
    def conc(v):                  # share of the 2-3 kHz band's energy within 15 Hz of the 2.5 kHz alias line
        s_, fr = spectrum(v)
        band = (fr > 2000) & (fr < 3000)
        return (s_[band & (np.abs(fr - 2500) < 15)] ** 2).sum() / (s_[band] ** 2).sum()

    def flat(v):                  # spectral flatness 50 Hz .. 20 kHz
        s_, fr = spectrum(v)
        p = s_[(fr > 50) & (fr < 20000)] ** 2 + 1e-9
        return np.exp(np.log(p).mean()) / p.mean()
    GJ = (0, 1024, 4096, 8192, 16384)
    cc = [conc(y[f"jit_{g}"][-N:, 0]) for g in GJ]
    fl_ = [flat(y[f"jit_{g}"][-N:, 0]) for g in GJ]
    check(max(cc[1:]) < 0.5 * cc[0] and all(b > a for a, b in zip(fl_, fl_[1:])),
          "jitter (hold at 4 kHz, 1.5 kHz in: alias at 2.5 kHz), GRIT 0 / 1k / 4k / 8k / 16k: share of the alias line "
          + " / ".join(f"{v:.2f}" for v in cc) + " (it wobbles and smears), spectral flatness "
          + " / ".join(f"{v:.4f}" for v in fl_) + " (rises: from wobble to broadband sizzle at half)")
    n0 = rms_db(y["sizzle_1"][-N:, 0] - y["sizzle_0"][-N:, 0]) - rms_db(y["sizzle_0"][-N:, 0])
    check(n0 > -30, f"jitter at RATE top (GRIT half): sizzle {n0:.1f} dB re the signal")
    fl = {g: cyc[f"flip_{g}"][2] for g in (12000, 16384, 24576, BIG)}
    check(fl[12000] == 0 and fl[16384] == 0 and 0 < fl[24576] < fl[BIG], "bit flips over 300 blocks (8 bits, 1 kHz), "
          "GRIT 12k / 16384 / 24576 / max: " + " / ".join(str(v) for v in fl.values()) + " (none up to half)")
    dev = np.abs(y[f"flip_{BIG}"][-N:, 0] - y["flip_16384"][-N:, 0]).max()
    check(dev > 4096, f"flips are glitches: largest deviation from GRIT half {dev} LSB")
    check(np.abs(y["flip_silence"]).max() == 0, "silence in, every knob up (flips at max): silence out")

    # ---- level ----
    lv = {}
    for kb in LV_B:
        for kr in LV_R:
            for t, xin in (("s", x440), ("w", xsaw)):
                lv[(kb, kr, t)] = rms_db(y[f"lv_{kb}_{kr}_{t}"][-N:, 0]) - rms_db(xin[-N:])
    worst = max(lv.items(), key=lambda kv: abs(kv[1]))
    check(abs(worst[1]) < 4, "level vs dry over BITS x RATE (0/8k/16k/24k/max each; 440 Hz sine, 220 Hz saw at -6 dBFS): "
          f"{min(lv.values()):+.1f} .. {max(lv.values()):+.1f} dB (worst {worst[0]}), window +-4")
    print("     BITS sweep at RATE top, sine/saw: " + ", ".join(f"{lv[(kb, 0, 's')]:+.1f}/{lv[(kb, 0, 'w')]:+.1f}"
                                                            for kb in LV_B)
          + " dB; RATE sweep at 16 bits: " + ", ".join(f"{lv[(0, kr, 's')]:+.1f}/{lv[(0, kr, 'w')]:+.1f}" for kr in LV_R))

    # ---- DC ----
    dcs = {k[3:]: y[k][-N:, 0].mean() for k in y if k.startswith("dc_")}
    rnd = {k: y["dc_" + k][-3 * N:, 0].mean() / np.sqrt((y["dc_" + k][-3 * N:, 0].astype(float) ** 2).mean())
           for k in ("flips", "all")}
    check(all(abs(v) < 100 for k, v in dcs.items() if k not in rnd) and all(abs(v) < 0.05 for v in rnd.values()),
          "DC (1.16 s runs, last 0.2 s): " + ", ".join(f"{k} {v:+.0f}" for k, v in dcs.items() if k not in rnd)
          + " LSB (blocker 3.4 Hz; pulse = 25 % duty, asymmetric); random clocks (jitter + flips), mean over 0.6 s re "
          "rms: " + ", ".join(f"{k} {100 * v:+.1f} %" for k, v in rnd.items()))
    pk = np.abs(y["stop"][-N:, 0]).max()
    check(pk < 50, f"after the input stops (0.2 s later, SQUASH max): peak {pk} LSB")

    # ---- zipper ----
    for name in TURNS:
        j = y[f"turn_{name}"][:, 0]
        tr = np.abs(np.diff(j[64 * 98:64 * 140])).max()
        steady = [np.abs(np.diff(y[rf][64 * 100:, 0])).max() for rf in REFS[name]]
        check(tr <= 1.25 * max(steady) + 50, f"turn {name} (16 detents, one every 2 blocks): largest step {tr}; steady "
              f"over the settings passed: {min(steady)} .. {max(steady)}: no zipper")
    j = y["jump_all"][:, 0]
    tr = np.abs(np.diff(j[64 * 98:64 * 200])).max()
    s0 = max(np.abs(np.diff(j[64 * 40:64 * 98])).max(), np.abs(np.diff(j[64 * 220:])).max())
    check(tr <= 1.25 * s0 + 50, f"jump (all knobs at once): largest step {tr}, steady before/after {s0}")

    # ---- cycles ----
    typ = {n: cyc[f"cpu_{n}"][1] for n in TYP}
    tmax = max(typ.values())
    check(tmax <= TYPICAL_BUDGET, "cycles per stereo call, typical: " + ", ".join(f"{k} {v}" for k, v in typ.items())
          + f"; budget {TYPICAL_BUDGET}")
    wst = max((v[1], k) for k, v in cyc.items() if k.startswith("worst_"))
    peak = max(v[0] for v in cyc.values())
    mono = max(v[1] for k, v in cyc.items() if C[k][0] == "m")
    check(peak <= WORST_BUDGET, f"cycles, worst: stereo mean {wst[0]} ({wst[1]}), peak {peak} over every case, mono mean "
          f"{mono}; bound {WORST_BUDGET}")

    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
