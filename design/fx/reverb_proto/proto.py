#!/usr/bin/env python3
"""Reverb FX prototype driver (notes/fx-reverb.md): the proposed knob mapping (Option A: SPACE, DECAY, TAIL,
MIX) -> tank.c parameters, renders of a dry test signal, and the checks: RT60 per octave band against the target,
shimmer and freeze stability over 60 s, and 16-bit line storage (limit cycles, noise floor).

Usage: python3 proto.py OUTDIR      (needs numpy and a C compiler; writes WAVs and prints the checks)
Not device code: float everywhere except the optional int16 line storage (tank.c "quant").
"""
import math
import os
import subprocess
import sys
import wave

import numpy as np

FS = 44100
HERE = os.path.dirname(os.path.abspath(__file__))

# --- the spaces: anchors on SPACE (blue), morphed linearly (times and sizes geometrically) -------------------
# size scales the FDN lines, the ER times and (sqrt) the diffusers; t60 is DECAY's centre (natural) mid-band.
# Review changes (notes/fx-reverb.md §10): modulation never below 4 samples (ROOM rang), ROOM's rate 1.5 Hz;
# 2nd-order low cut at 100-160 Hz for the big spaces (a kick must not become a sub drone).
SPACES = [
    # name       knob   size  t60   hi    xover  pre  erlev  diff  mdep mrate  hp   lp
    ("ROOM",      0,    0.33, 0.7,  0.50, 3500,  3,   0.80,  0.62, 4,   1.5,  150, 9000),
    ("HALL",      8192, 1.00, 2.2,  0.50, 3000,  20,  0.50,  0.70, 7,   0.6,  80,  8000),
    ("CHURCH",    16384, 1.45, 3.8, 0.45, 2500,  30,  0.55,  0.62, 6,   0.4,  90,  7000),
    ("CATHEDRAL", 24576, 2.00, 7.0, 0.38, 2000,  45,  0.40,  0.72, 8,   0.3,  110, 6000),
    ("VOID",      32767, 2.40, 18.0, 0.60, 3000, 70,  0.15,  0.75, 18,  0.2,  140, 9000),
]
FIELDS = ["knob", "size", "t60", "hi", "xover", "pre", "erlev", "diff", "mdep", "mrate", "hp", "lp"]
GEOM = {"size", "t60", "xover", "pre", "mdep", "mrate", "hp", "lp"}
BASE_MS = [31.3, 36.7, 41.9, 47.3, 53.9, 59.1, 66.7, 73.1]      # FDN lines at size 1 (sum 410 ms)
LAP_FRAC = 0.13                                                  # in-line allpass: 13 % of its line, g +-0.5
AP_MS = [5.3, 9.1, 14.7, 23.3, 6.1, 10.3, 13.1, 21.7]            # input allpasses L1..L4 R1..R4 at size 1
ER_AP_MS = [4.1, 11.3, 4.7, 12.9]                                # 2 allpasses on each side's ER sum (x sqrt size)
# early reflections per space: (ms after predelay at size 1, gain); L and R get different patterns.
# CHURCH's alternating strong / weak taps have their gains jittered (review: a periodic 25 ms flutter).
ER = {
    "ROOM": ([3, 5, 7, 10, 13, 17, 22, 28], [0.9, 0.8, 0.7, 0.62, 0.5, 0.42, 0.33, 0.25]),
    "HALL": ([8, 14, 21, 29, 38, 47, 58, 70], [0.7, 0.62, 0.55, 0.5, 0.42, 0.36, 0.3, 0.25]),
    "CHURCH": ([12, 20, 33, 41, 55, 68, 82, 97], [0.8, 0.52, 0.66, 0.43, 0.5, 0.39, 0.40, 0.3]),
    "CATHEDRAL": ([15, 31, 44, 62, 79, 101, 124, 150], [0.75, 0.5, 0.6, 0.45, 0.5, 0.38, 0.33, 0.28]),
    "VOID": ([20, 45, 70, 95, 120, 150, 180, 210], [0.4, 0.35, 0.3, 0.27, 0.24, 0.2, 0.18, 0.15]),
}
ER_R_SKEW = [1.13, 0.91, 1.07, 0.95, 1.11, 0.89, 1.05, 0.93]    # R's pattern: each tap moved, decorrelated


def primeish(n):
    n = int(round(n)) | 1
    while any(n % p == 0 for p in range(3, int(n ** 0.5) + 1, 2)):
        n += 2
    return n


def space_at(k):
    """Space parameters at SPACE knob k (0 .. 32767): linear between anchors, geometric for times."""
    k = max(0, min(32767, k))
    for a, b in zip(SPACES, SPACES[1:]):
        if a[1] <= k <= b[1]:
            break
    t = (k - a[1]) / (b[1] - a[1])
    out = {"name": a[0] if t < 0.5 else b[0]}
    for i, f in enumerate(FIELDS[1:], start=2):
        va, vb = a[i], b[i]
        out[f] = va * (vb / va) ** t if f in GEOM else va + (vb - va) * t
    ta, ga = ER[a[0]]
    tb, gb = ER[b[0]]
    out["er_t"] = [x + (y - x) * t for x, y in zip(ta, tb)]
    out["er_g"] = [x + (y - x) * t for x, y in zip(ga, gb)]
    return out


# --- TAIL (white) zones, 32 detents of 1024 (after the review: +7 dropped, DUCK moved to MIX on the master) ----
TAIL_ZONES = [  # (first detent, last detent, name)
    (0, 9, "TONE"), (10, 14, "SHIMMER +12"), (15, 18, "SHIMMER +12+19"), (19, 22, "SUB -12"),
    (23, 27, "GATE"), (28, 31, "SWELL"),
]


def tail_at(k):
    d = min(31, max(0, k) // 1024)
    for lo, hi, name in TAIL_ZONES:
        if lo <= d <= hi:
            frac = (k - lo * 1024) / ((hi - lo + 1) * 1024)
            return name, max(0.0, min(1.0, frac))
    raise ValueError


def params(space_k, decay_k, tail_k, mix_k, master=True, **over):
    s = space_at(space_k)
    p = {}
    sz = s["size"]
    lens = [primeish(ms * sz * FS / 1000) for ms in BASE_MS]
    for i, n in enumerate(lens):
        p[("len", i)] = n
        p[("lap_len", i)] = primeish(n * LAP_FRAC)
    for i, ms in enumerate(AP_MS):
        p[("ap_len", i)] = primeish(ms * math.sqrt(sz) * FS / 1000)
    for i, ms in enumerate(ER_AP_MS):
        p[("er_ap_len", i)] = primeish(ms * math.sqrt(sz) * FS / 1000)
    p["ap_g"] = s["diff"]
    p["predelay"] = s["pre"] * FS / 1000
    for i, (ms, g) in enumerate(zip(s["er_t"], s["er_g"])):
        tl = ms * min(sz, 1.6) ** 0.5 * FS / 1000
        p[("er_t", i)] = tl
        p[("er_tr", i)] = tl * ER_R_SKEW[i]
        p[("er_g", i)] = g * 0.35
    p["er_level"] = s["erlev"]
    p["er_send"] = 0.7
    p["direct_send"] = 0.5
    # DECAY (green): x0.25 .. x4 of the space's natural RT60 over detents 0 .. 29.99, centre (16384) = natural;
    # detent 30 (30720 .. 31743) INFINITE (input open, lossless-ish, level held); detent 31 (>= 31744) FREEZE
    freeze = decay_k >= 31744
    infinite = 30720 <= decay_k < 31744
    mult = 2 ** (4 * (min(decay_k, 30719) / 30719) - 2) if decay_k < 30720 else 4.0
    t60 = s["t60"] * mult
    name, amt = tail_at(tail_k)
    hi = s["hi"]
    p["in_hp"] = s["hp"]
    if name == "TONE":                       # dark (x0.25 of natural HF decay) .. natural .. bright (flat)
        x = tail_k / (10 * 1024) * 2 - 1     # -1 .. 1
        hi = hi * 4 ** x if x < 0 else hi + (1.0 - hi) * x
        p["in_lp"] = s["lp"] * 2 ** x
        if x < 0:
            p["in_hp"] = s["hp"] * 2 ** -x   # dark also thins the low end (x2 at the far left)
    else:
        p["in_lp"] = s["lp"]
    p["t60_lo"] = t60
    p["t60_hi"] = t60 * min(hi, 1.0)
    p["xover"] = s["xover"]
    p["mod_depth"] = s["mdep"]               # not scaled down with size (review: ROOM rang at 1 sample)
    p["mod_rate"] = s["mrate"]
    p[("sh_ratio", 0)] = 1
    p[("sh_ratio", 1)] = 1
    p[("sh_gain", 0)] = 0
    p[("sh_gain", 1)] = 0
    p["sh_win"] = 3000
    p["sh_jitter"] = 0.2                     # each head's window +-20 % at its reset: no steady 29 Hz AM line
    p["sh_hp"] = 300
    p["sh_lp"] = 7000                        # biquad; r = 2 aliases above 11 kHz
    p["sh_limit"] = 0.5
    p["sh_kmul"] = 4.0                       # shimmer feedback x min(1, 4 sqrt(1 - g^2)): audible, never grows
    if name.startswith("SHIMMER") or name.startswith("SUB"):
        g = 0.4 + 0.6 * amt                  # 0.4 .. 1.0 of the normalised feedback
        if name == "SHIMMER +12":
            p[("sh_ratio", 0)], p[("sh_gain", 0)] = 2.0, g
        elif name == "SHIMMER +12+19":       # octave and the fifth above it, one ring: filter for r = 3
            p[("sh_ratio", 0)], p[("sh_gain", 0)] = 2.0, g * 0.7
            p[("sh_ratio", 1)], p[("sh_gain", 1)] = 3.0, g * 0.5
            p["sh_lp"] = 6000
        else:
            p[("sh_ratio", 0)], p[("sh_gain", 0)] = 0.5, g
            p["sh_hp"] = 120
            p["sh_lp"] = 4000
    if name == "SWELL":
        p["swell"] = 0.15 * 10 ** amt        # 0.15 .. 1.5 s, on the wet output (the tank stays fed)
    elif name == "GATE":
        p["gate"] = 1
        p["gate_hold"] = 0.05 + 0.30 * amt   # 50 .. 350 ms after the input drops (synced holds in the design)
    # MIX (orange), 0 .. 16384: dry 1, wet 0 -> 1. Above the middle: master: wet 1, dry 1, DUCK depth 0 -> 1
    # (ducking is the master move); synth / drum (mono): the dry falls to 0 (kill dry at the top).
    m = min(mix_k, 32767) / 32767
    up = max(0.0, 2 * m - 1)
    if master:
        p["dry"] = 1.0
        if up > 0:
            p["duck"] = 1 - 10 ** (-1.2 * up)    # 0 .. -24 dB while the input is above -24 dBFS; -12 at mid
    else:
        p["dry"] = 1.0 - up
    p["wet"] = min(1.0, 2 * m)
    # the late level grows with RT60; tame it to ~sqrt(RT60), at the tank INPUT (headroom where it's needed)
    p["in_gain"] = 0.5 * (max(t60, 0.3) / 1.5) ** -0.25
    p["late_level"] = 2.0                    # the 0.5 (-6 dB) input headroom made up at the output
    if freeze:
        p["freeze_on"] = 0.0
    if infinite:
        p["infinite"] = 1
    p["quant"] = 0
    p.update(over)
    return p, s["name"], name


def write_params(p, path):
    with open(path, "w") as f:
        for k, v in p.items():
            if isinstance(k, tuple):
                f.write(f"{k[0]} {k[1]} {v}\n")
            else:
                f.write(f"{k} {v}\n")


def build():
    exe = os.path.join(TMP, "tank")
    subprocess.check_call(["cc", "-O2", "-o", exe, os.path.join(HERE, "tank.c"), "-lm"])
    return exe


def run(p, x):
    pp = os.path.join(TMP, "p.txt")
    write_params(p, pp)
    fi, fo = os.path.join(TMP, "in.f32"), os.path.join(TMP, "out.f32")
    x.astype(np.float32).tofile(fi)
    r = subprocess.run([EXE, pp, fi, fo], capture_output=True, text=True, check=True)
    y = np.fromfile(fo, dtype=np.float32).reshape(-1, 2)
    return y, r.stderr.strip()


def wav(path, y):
    y = np.clip(y, -1, 1)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes((y * 32767).astype("<i2").tobytes())


# --- test signal: plucked-string arpeggio, a synth chord, two snare hits, then silence ---------------------------
def pluck(freq, dur, rng):
    n = int(dur * FS)
    p = int(FS / freq)
    buf = rng.uniform(-1, 1, p)
    out = np.zeros(n)
    for i in range(n):
        out[i] = buf[i % p]
        buf[i % p] = 0.996 * 0.5 * (buf[i % p] + buf[(i + 1) % p])
    return out


def test_signal(tail):
    rng = np.random.default_rng(1)
    n = int((6.5 + tail) * FS)
    x = np.zeros(n)
    notes = [82.4, 123.5, 164.8, 196.0, 246.9, 329.6, 246.9, 196.0]       # E minor arpeggio
    for i, f in enumerate(notes):
        t = int(i * 0.25 * FS)
        s = pluck(f, 1.2, rng) * 0.35
        x[t:t + len(s)] += s
    t0 = int(2.5 * FS)                                                    # saw-ish pad chord, 1.2 s
    tt = np.arange(int(1.2 * FS)) / FS
    env = np.minimum(1, tt / 0.02) * np.minimum(1, (1.2 - tt) / 0.15)
    chord = sum(np.sin(2 * np.pi * f * tt) + 0.5 * np.sin(4 * np.pi * f * tt) + 0.3 * np.sin(6 * np.pi * f * tt)
                for f in (220.0, 261.6, 329.6, 392.0))
    x[t0:t0 + len(tt)] += 0.08 * chord * env
    for ts in (4.2, 4.7, 5.2):                                           # snare-ish: noise + 190 Hz body
        t = int(ts * FS)
        m = int(0.18 * FS)
        tt = np.arange(m) / FS
        s = (rng.uniform(-1, 1, m) * np.exp(-tt / 0.04) * 0.5 + np.sin(2 * np.pi * 190 * tt) * np.exp(-tt / 0.03))
        x[t:t + m] += 0.45 * s
    x[int(5.6 * FS):int(5.6 * FS) + 1] += 0.0
    return np.stack([x, x * 0.98], axis=1)


# --- analysis -----------------------------------------------------------------------------------------------------
BANDS = [125, 250, 500, 1000, 2000, 4000, 8000]


def band_energy(h, fc, nfft=2048, hop=256):
    """Energy per STFT frame in the octave band around fc (Hann frames: compact in time, no brick-wall leakage)."""
    win = np.hanning(nfft)
    f = np.fft.rfftfreq(nfft, 1 / FS)
    sel = (f >= fc / 2 ** 0.5) & (f < fc * 2 ** 0.5)
    frames = range(0, len(h) - nfft, hop)
    return np.array([np.sum(np.abs(np.fft.rfft(h[i:i + nfft] * win))[sel] ** 2) for i in frames]), hop


def edc_rt60(e, hop):
    """RT60 from a frame-energy series: Schroeder integration, slope from -5 to -35 dB (T30), or -25 (T20)."""
    c = np.cumsum(e[::-1])[::-1]
    c = 10 * np.log10(c / c[0] + 1e-300)
    i5 = np.argmax(c <= -5)
    for lo, mul in ((-35, 2), (-25, 3)):
        j = np.argmax(c <= lo)
        if j > i5:
            return mul * (j - i5) * hop / FS
    return float("nan")


def target_band(p, fc):
    """Expected RT60 at fc: per line |g_lo LP + g_hi (1 - LP)| per pass of m samples, averaged over the lines."""
    a = 1 - math.exp(-2 * math.pi * p["xover"] / FS)
    z = np.exp(-2j * np.pi * fc / FS)
    lp = a / (1 - (1 - a) * z)
    ts = []
    for i in range(8):
        m = p[("len", i)] + p[("lap_len", i)]
        glo, ghi = 10 ** (-3 * m / (p["t60_lo"] * FS)), 10 ** (-3 * m / (p["t60_hi"] * FS))
        mag = abs(glo * lp + ghi * (1 - lp))
        ts.append(-3 * m / (FS * math.log10(mag)))
    return sum(ts) / len(ts)


def rt60_report(label, p):
    n = int(max(4, 2.5 * p["t60_lo"]) * FS)
    x = np.zeros((n, 2))
    x[0] = 1
    q = dict(p, dry=0.0, wet=1.0, er_level=0.0)
    y, _ = run(q, x)
    h = y[:, 0] + y[:, 1]
    row = []
    for fc in BANDS:
        m = edc_rt60(*band_energy(h, fc))
        t = target_band(p, fc)
        row.append(f"{fc}:{m:.2f}/{t:.2f}")
    print(f"  {label:<22} " + "  ".join(row) + f"   NED 0.9 at {ned_mixing_time(h):.0f} ms")
    return h


def fine(y, a, b):
    """Fine spectral structure of y[a s .. b s] in dB against a 1/3-octave smooth, 200 Hz .. 5 kHz."""
    seg = y[int(a * FS):int(b * FS)]
    P = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    f = np.fft.rfftfreq(len(seg), 1 / FS)
    c = np.cumsum(P)
    lo = np.searchsorted(f, f * 2 ** (-1 / 6))
    hi = np.maximum(np.searchsorted(f, f * 2 ** (1 / 6)), lo + 1)
    sm = (c[np.minimum(hi, len(c) - 1)] - c[lo]) / (hi - lo)
    sel = (f > 200) & (f < 5000)
    return 10 * np.log10(P[sel] / sm[sel] + 1e-20)


def ned_mixing_time(h, win_ms=20):
    """Abel & Huang normalised echo density: fraction of samples outside 1 std in a sliding window, / 0.3173.
    Returns the time (ms) where NED first reaches 0.9 (the response sounds like dense noise from there on)."""
    w = int(win_ms * FS / 1000)
    win = np.hanning(w)
    win /= win.sum()
    for i in range(0, len(h) - w, w // 4):
        seg = h[i:i + w]
        sd = math.sqrt(np.sum(win * seg ** 2))
        if sd == 0:
            continue
        ned = np.sum(win * (np.abs(seg) > sd)) / 0.3173
        if ned >= 0.9:
            return 1000 * (i + w / 2) / FS
    return float("nan")


def windows_rms(y, sec):
    w = int(sec * FS)
    return [20 * math.log10(math.sqrt(np.mean(y[i:i + w] ** 2)) + 1e-12) for i in range(0, len(y) - w + 1, w)]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    global TMP, EXE
    TMP = os.path.join(out, ".tmp")
    os.makedirs(TMP, exist_ok=True)
    EXE = build()

    dry = test_signal(1.0)
    print("== renders (dry test signal, then the wet; knob values raw 0..32767) ==")
    renders = [
        ("dry", None),
        ("room", (0, 16384, 5120, 9000)),
        ("hall", (8192, 16384, 5120, 11000)),
        ("church", (16384, 16384, 5120, 12000)),
        ("cathedral", (24576, 16384, 4000, 13000)),
        ("extreme_void", (32767, 26000, 7000, 20000)),
        ("shimmer_plus12", (20480, 22000, 12800, 15000)),
        ("shimmer_plus19", (24576, 22000, 17000, 15000)),
        ("sub_minus12", (16384, 18000, 20800, 13000)),
        ("freeze", None),
        ("gated_room", (3000, 24000, 26000, 14000)),
        ("swell_hall", (10000, 20000, 31000, 16000)),
        ("duck_cathedral", (24576, 20000, 4000, 24576)),
        ("infinite_hall", (8192, 31000, 5120, 13000)),
        ("frozen_shimmer", None),
    ]
    for name, knobs in renders:
        if name == "dry":
            wav(os.path.join(out, "00_dry.wav"), np.concatenate([dry, np.zeros((int(8 * FS), 2))]))
            continue
        if name == "freeze":       # hall; freeze engaged at 2.6 s (the pad chord), released at 10 s
            p, sp, tl = params(8192, 16384, 5120, 12000, freeze_on=2.9, freeze_off=10.0)
            tail = 6
        elif name == "frozen_shimmer":   # CATHEDRAL, +12 zone; freeze at 2.9 s: the frozen pad climbs in octaves
            p, sp, tl = params(24576, 20000, 12800, 14000, freeze_on=2.9, freeze_off=14.0)
            p["frz_shimmer"] = 0.3 + 0.4 * tail_at(12800)[1]
            tail = 10
        else:
            p, sp, tl = params(*knobs)
            tail = min(20, max(4, 1.3 * p["t60_lo"]))
        x = np.concatenate([dry, np.zeros((int(tail * FS), 2))])
        y, info = run(p, x)
        fade = int(1.5 * FS)
        y[-fade:] *= np.linspace(1, 0, fade)[:, None]
        pk = 20 * math.log10(np.max(np.abs(y)) + 1e-12)
        idx = [r[0] for r in renders].index(name)
        fn = f"{idx:02d}_{name}.wav"
        wav(os.path.join(out, fn), y)
        print(f"  {fn:<24} {sp:<10} {tl:<12} RT60 {p['t60_lo']:.2f} s (HF {p['t60_hi']:.2f})  peak {pk:+.1f} dBFS"
              f"  {info}")

    print("\n== RT60 per octave band: measured (T30 on the wet IR, ERs off) / target of the two-band filter ==")
    for label, k in [("ROOM", (0, 16384, 5120, 16384)), ("HALL", (8192, 16384, 5120, 16384)),
                     ("CHURCH", (16384, 16384, 5120, 16384)), ("CATHEDRAL", (24576, 16384, 5120, 16384)),
                     ("HALL dark (TONE 0)", (8192, 16384, 0, 16384)),
                     ("HALL bright (TONE 9)", (8192, 16384, 10239, 16384)),
                     ("ROOM x0.25", (0, 0, 5120, 16384)), ("CATHEDRAL x4", (24576, 30719, 5120, 16384))]:
        p, _, _ = params(*k)
        rt60_report(label, p)

    print("\n== shimmer stability: 0.5 s noise burst, then 60 s; wet RMS per 5 s window (dBFS) ==")
    rng = np.random.default_rng(2)
    burst = np.zeros((int(60.5 * FS), 2))
    burst[:int(0.5 * FS)] = rng.uniform(-0.5, 0.5, (int(0.5 * FS), 2))
    for label, k, over in [
            ("+12 max, CATHEDRAL x4", (24576, 30719, 15359, 16384), {}),
            ("+12 max, VOID x4", (32767, 30719, 15359, 16384), {}),
            ("+12+19 max, VOID x4", (32767, 30719, 19455, 16384), {}),
            ("-12 max, VOID x4", (32767, 30719, 23551, 16384), {}),
            ("+12 max, VOID x4, no norm", (32767, 30719, 15359, 16384), {"sh_norm": 0, "sh_limit": 0}),
            ("+12 max, VOID x4, int16", (32767, 30719, 15359, 16384), {"quant": 1})]:
        p, _, _ = params(*k, **over)
        p = dict(p, dry=0.0, wet=1.0)
        y, info = run(p, burst)
        r = windows_rms(y[:, 0], 5)
        print(f"  {label:<28} " + " ".join(f"{v:6.1f}" for v in r) + f"   {info}")

    print("\n== freeze: 0.5 s noise burst, freeze at 0.6 s for 60 s; RMS per 10 s window (dBFS) ==")
    for q in (0, 1, 3, 4):
        p, _, _ = params(8192, 16384, 5120, 32767, freeze_on=0.6, freeze_off=1e9, quant=q)
        p = dict(p, dry=0.0, wet=1.0)
        y, info = run(p, burst)
        r = windows_rms(y[int(0.6 * FS):, 0], 10)
        print(f"  quant {q}: " + " ".join(f"{v:6.1f}" for v in r) + f"   {info}")

    print("\n== 16-bit lines: ROOM / HALL, burst then 60 s of silence; last 10 s RMS and peak (dBFS) ==")
    for label, k in [("ROOM", (0, 16384, 5120, 16384)), ("HALL x4", (8192, 30719, 5120, 16384))]:
        for q, qn in [(1, "round"), (2, "floor"), (3, "toward 0"), (4, "round+dither")]:
            p, _, _ = params(*k, quant=q)
            p = dict(p, dry=0.0, wet=1.0)
            y, _ = run(p, burst)
            last = y[-10 * FS:, 0]
            rms = 20 * math.log10(math.sqrt(np.mean(last ** 2)) + 1e-12)
            pk = 20 * math.log10(np.max(np.abs(last)) + 1e-12)
            print(f"  {label:<8} {qn:<13} rms {rms:7.1f}  peak {pk:7.1f}")

    print("\n== diffusion and ringing (review metrics): crest factor per 25 ms after the predelay (noise ~4); "
          "late fine structure vs a 1/3-oct smooth: std (Gaussian 5.6 dB) and window-to-window persistence ==")
    for label, k, a, b, c in [("ROOM", (0, 16384, 5120, 16384), 0.25, 0.75, 1.25),
                              ("ROOM x4", (0, 30719, 5120, 16384), 0.5, 1.5, 2.5),
                              ("HALL", (8192, 16384, 5120, 16384), 0.5, 1.25, 2.0),
                              ("CHURCH", (16384, 16384, 5120, 16384), 0.5, 1.5, 2.5),
                              ("CATHEDRAL", (24576, 16384, 5120, 16384), 1, 2.5, 4)]:
        p, _, _ = params(*k)
        q = dict(p, dry=0.0, wet=1.0)
        x = np.zeros((int((c + 0.1) * FS), 2))
        x[0] = 1
        y, _ = run(q, x)
        h = y[:, 0]
        pre = int(p["predelay"])
        crest = []
        for t0 in range(0, 200, 25):
            sg = h[pre + int(t0 * FS / 1000):pre + int((t0 + 25) * FS / 1000)]
            crest.append(np.abs(sg).max() / (math.sqrt(np.mean(sg ** 2)) + 1e-15))
        y2, _ = run(dict(q, er_level=0.0), x)
        r1, r2 = fine(y2[:, 0], a, b), fine(y2[:, 0], b, c)
        n = min(len(r1), len(r2))
        print(f"  {label:<10} crest " + " ".join(f"{v:4.1f}" for v in crest) +
              f"   std {r1.std():4.1f} dB  persistence {np.corrcoef(r1[:n], r2[:n])[0, 1]:+.2f}"
              f"   NED 0.9 at {ned_mixing_time(h):.0f} ms (ERs on)")

    print("\n== headroom: sustained -3 dBFS saw pad (4 s); line peak (FS) ==")
    tt = np.arange(4 * FS) / FS
    pad = sum(((f * tt) % 1 * 2 - 1) for f in (110, 165, 220, 277)) / 4
    pad = 0.708 * pad / np.max(np.abs(pad))
    padx = np.concatenate([np.stack([pad, pad], 1), np.zeros((2 * FS, 2))])
    for label, k in [("CATHEDRAL x4", (24576, 30719, 5120, 16384)), ("VOID x4 +12", (32767, 30719, 15359, 16384)),
                     ("HALL INFINITE", (8192, 31000, 5120, 16384))]:
        p, _, _ = params(*k)
        y, info = run(dict(p, dry=0.0, wet=1.0), padx)
        print(f"  {label:<14} {info.split()[0]} {info.split()[1]}  wet peak {20 * math.log10(np.abs(y).max()):+.1f} dBFS")


if __name__ == "__main__":
    main()
