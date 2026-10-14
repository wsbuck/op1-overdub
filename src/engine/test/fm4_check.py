#!/usr/bin/env python3
"""Simulator checks for the fm4 engine (src/engine/fm4.c) before flashing.

  python3 src/engine/test/fm4_check.py [--keep DIR]

Builds fm4.c + tramp.S with fm4_main.c and runs every case under bfin-elf-run at 88.2 kHz (simdrive.py). Checks: pitch;
known spectra (a 5:1 carrier:modulator pair gives Bessel sidebands J_n(I) around the carrier; 1:1 and every one of the
11 algorithms, with feedback, against a float model of the same equations); feedback alone (saw-like); envelope times and
TIME's scaling; the v0 plain-knob mapping (ratios, detune); the Tone limiter (aliasing at C6/C8 with maximum index and
high ratios); glides; stability at extreme knobs; the output guard; CYCLES per render call. Exits non-zero if a check
fails. --keep DIR keeps the raw sample files.
"""
import argparse
import math
import os
import shutil
import sys
import tempfile

import numpy as np

from simdrive import FS, alias_ratio, db, harmonics, inc, load, run, spectrum
from simrun import CPU_BUDGET

COARSE2 = [1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48,
           52, 56, 60, 64, 72, 80, 96]
FINE_MUL = [65536, 65726, 65347, 66297, 65536, 65536, 65536, 92682]
FINE_ADD = [0, 0, 0, 0, 16384, 26870, 32768, 0]
LEVEL = [0, 260, 368, 519, 734, 1036, 1464, 2067, 2920, 4125, 5827, 8231, 11626, 16422, 23197, 32767]
MODS = [[0x2, 0x4, 0x8, 0], [0x2, 0xC, 0, 0], [0xA, 0x4, 0, 0], [0x6, 0, 0x8, 0], [0x2, 0, 0x8, 0], [0xE, 0, 0, 0],
        [0, 0x4, 0x8, 0], [0x4, 0x4, 0x8, 0], [0x8, 0x8, 0x8, 0], [0, 0, 0x8, 0], [0, 0, 0, 0]]
CARS = [1, 1, 1, 1, 5, 1, 3, 3, 7, 7, 15]
RAD = 20861


def s16(v):
    return v - 65536 if v >= 32768 else v


def op(ratio=1.0, level=15, env=0, fine=0):
    """packed operator knob (fm4.h)"""
    c = COARSE2.index(int(round(ratio * 2)))
    return s16(0x8000 | c << 10 | fine << 7 | level << 3 | env)


def alg_knob(a):
    """knob 1 value for algorithm a (1..11)"""
    return ((a - 1) * 32768 + 16384) // 11


def index_knob(index):
    """knob 0 for INDEX (0..2x): index_q15 = k^2 >> 15, 32767 = 2x"""
    return min(32767, round(math.sqrt(index / 2 * 32767 * 32768)))


def decode(k):
    """fm4_decode + the engine's static gains: (alg, ratios, carrier gains Q15, modulator I in rad, fb rad)"""
    ix = max(k[0], 0)
    index = (ix * ix) >> 15
    alg = min((max(k[1], 0) * 11) >> 15, 10)
    packed = all(v < 0 for v in k[4:8])
    ratio, level = [], []
    for i in range(4):
        if packed:
            u = k[4 + i] & 0xFFFF
            c, f, lv = (u >> 10) & 31, (u >> 7) & 7, (u >> 3) & 15
        else:
            c, f, lv = (max(k[3 + i], 0) >> 10 if i else 1), 0, 15
        ratio.append(COARSE2[c] * (FINE_MUL[f] >> 1) + FINE_ADD[f])
        level.append(lv)
    if not packed:
        e = (max(k[7], 0) * 655) >> 15
        ratio[1] += ((ratio[1] >> 8) * e) >> 8
        ratio[2] -= ((ratio[2] >> 8) * e) >> 8
        ratio[3] += ((ratio[3] >> 8) * (e >> 1)) >> 8
    car = CARS[alg]
    lsum = sum((LEVEL[level[i]] * 32767) >> 15 for i in range(4) if car >> i & 1)
    norm = ((32767 << 15) // lsum) if lsum > 32767 else 32767
    gains, I = [0] * 4, [0.0] * 4
    for i in range(4):
        lv = (LEVEL[level[i]] * 32767) >> 15
        env = 16384 if not packed and not (CARS[alg] >> i & 1) else 32767   # v0 modulators: sustained at half
        a1 = (lv * env) >> 15
        if car >> i & 1:
            gains[i] = (a1 * norm) >> 15
        else:
            d1 = (a1 * index) >> 15
            g = (d1 * (8 * RAD >> 3)) >> 12
            g = (g >> 1) * 256 >> 7
            I[i] = g / 20860.757
    fb = max(k[3], 0)
    fbg = ((fb * 14603) >> 15)
    return alg, [r / 65536 for r in ratio], gains, I, fbg * 2 / 20860.757


def model(k, f0, n=24000):
    """Float model of the engine's equations (static: flat envelopes): output samples in the engine's Q15 units"""
    alg, ratio, gains, I, fbr = decode(k)
    w = [2 * math.pi * r * f0 / FS for r in ratio]
    out = np.zeros(n)
    s1 = s2 = 0.0
    sin = math.sin
    m = MODS[alg]
    car = CARS[alg]
    for t in range(n):
        sd = sin(w[3] * t + fbr * (s1 + s2) / 2)
        s2, s1 = s1, sd
        y = [0.0, 0.0, 0.0, I[3] * sd]
        s = [0.0, 0.0, 0.0, sd]
        for o in (2, 1, 0):
            mod = sum(y[j] for j in range(o + 1, 4) if m[o] >> j & 1)
            s[o] = sin(w[o] * t + mod)
            y[o] = I[o] * s[o]
        out[t] = sum(s[o] * gains[o] for o in range(4) if car >> o & 1) / 2   # Q15 x gain >> 16
    return out


def bessel(nu, x):
    return sum((-1) ** m / (math.factorial(m) * math.factorial(m + nu)) * (x / 2) ** (2 * m + nu) for m in range(30))


F0 = 440.0
OFF = op(1, 0)
CASES = {}


def case(name, k, f0=F0, calls=230, f1=None, extra=""):
    CASES[name] = (list(k), f0, calls, f1, extra)


# pitch: A alone (additive), flat
case("sine440", [0, alg_knob(11), 16384, 0, op(1), OFF, OFF, OFF])
# Bessel: A at 5, B at 1 modulating it (algorithm 6: B + C + D -> A), I = 2
case("bessel5_1", [index_knob(0.5), alg_knob(6), 16384, 0, op(5), op(1), OFF, OFF], f0=200, calls=345)
case("pm1_1", [index_knob(0.5), alg_knob(6), 16384, 0, op(1), op(1), OFF, OFF], f0=220, calls=345)
case("pm1_2_hi", [index_knob(1.5), alg_knob(6), 16384, 0, op(1), op(2), OFF, OFF], f0=220, calls=345)
# every algorithm with all four operators at integer ratios and some feedback
RAT = [1, 2, 3, 1]
for a in range(1, 12):
    case(f"alg{a}", [index_knob(0.4), alg_knob(a), 16384, 6000, op(RAT[0]), op(RAT[1], 13), op(RAT[2], 12),
                     op(RAT[3], 14)], f0=110, calls=345)
case("feedback_saw", [0, alg_knob(11), 16384, 32767, OFF, OFF, OFF, op(1)], f0=220, calls=345)
case("feedback_half", [0, alg_knob(11), 16384, 16384, OFF, OFF, OFF, op(1)], f0=220, calls=345)
# envelopes: A alone with "short" (decay to 0, 0.2 s) at TIME x1 and x4; a modulator plucking (pm 1:1)
case("env_short_t1", [0, alg_knob(11), 16384, 0, op(1, 15, 2), OFF, OFF, OFF], calls=1380)
case("env_short_t4", [0, alg_knob(11), 32767, 0, op(1, 15, 2), OFF, OFF, OFF], calls=2760)
case("env_swell", [0, alg_knob(11), 16384, 0, op(1, 15, 7), OFF, OFF, OFF], calls=1380)
case("env_retrig", [0, alg_knob(11), 16384, 0, op(1, 15, 1), OFF, OFF, OFF], calls=1380, extra="noteon 700")
# v0: plain knobs. B ratio 2 (2048), C 3 (3072), D 1 (1024); detune 0 and max
case("v0_alg1", [index_knob(0.3), alg_knob(1), 0, 0, 2048 + 512, 3072 + 512, 1024 + 512, 0], f0=110, calls=345)
case("v0_add", [0, alg_knob(11), 32767, 0, 2048 + 512, 3072 + 512, 1024 + 512, 0], f0=110, calls=345)
case("v0_detune", [0, alg_knob(11), 32767, 0, 1024 + 512, 1024 + 512, 1024 + 512, 32767], f0=110, calls=1380)
# Tone limiter: maximum index, high ratios, stacked; C6 and C8
for f in (1046.5, 2093, 4186):
    case(f"tone_stack_f{round(f)}", [32767, alg_knob(1), 16384, 32767, op(1), op(3), op(7), op(14)], f0=f)
    case(f"tone_fan_f{round(f)}", [32767, alg_knob(9), 16384, 32767, op(1), op(2), op(5), op(11)], f0=f)
    case(f"tone_pair_f{round(f)}", [32767, alg_knob(6), 16384, 0, op(1), op(24), OFF, OFF], f0=f)
    case(f"tone_fb_f{round(f)}", [0, alg_knob(11), 16384, 32767, OFF, OFF, OFF, op(1)], f0=f)
case("glide_stack", [32767, alg_knob(1), 16384, 32767, op(1), op(3), op(7), op(14)], f0=30, calls=1380, f1=12000)
case("glide_v0", [32767, alg_knob(4), 0, 32767, 32767, 32767, 32767, 32767], f0=30, calls=1380, f1=12000)
# Proposed presets (notes/harm-fm4.md). v0: plain knobs (B, C, D coarse = k >> 10, so 1024 n + 512 is entry n).
def v0(index, a, time, fb, b, c, d, detune):
    return [index_knob(index), alg_knob(a), time, fb, COARSE2.index(int(b * 2)) * 1024 + 512,
            COARSE2.index(int(c * 2)) * 1024 + 512, COARSE2.index(int(d * 2)) * 1024 + 512, detune]


PRESETS_V0 = {"e piano": v0(0.25, 5, 6000, 0, 1, 1, 14, 6000),
              "bell": v0(0.6, 6, 26000, 0, 3, 7, 11, 20000),
              "bass": v0(0.35, 1, 3000, 8000, 1, 2, 1, 0),
              "brass": v0(0.5, 2, 22000, 6000, 1, 1, 3, 3000),
              "organ": v0(0, 11, 16384, 0, 2, 4, 8, 0),
              "pluck": v0(0.7, 4, 0, 10000, 1, 3, 1, 0),
              "metallic": v0(0.8, 8, 16384, 0, 1, 13, 9, 32767),
              "pad": v0(0.2, 10, 32767, 4000, 2, 1, 1, 16000)}
# Packed (hook) versions: per operator pk(ratio, fine, level, envelope)
def pk(ratio, fine=0, level=15, env=0):
    return op(ratio, level, env, fine)


PRESETS_PACKED = {
    "e piano": [index_knob(0.3), alg_knob(5), 16384, 0, pk(1), pk(1, 0, 12, 3), pk(1, 0, 13, 4), pk(14, 0, 9, 1)],
    "bell": [index_knob(0.7), alg_knob(6), 16384, 0, pk(1, 0, 15, 4), pk(3, 5, 11, 3), pk(7, 7, 8, 2), pk(11, 0, 6, 1)],
    "bass": [index_knob(0.4), alg_knob(1), 16384, 9000, pk(1), pk(1, 0, 13, 2), pk(2, 0, 10, 1), pk(1, 0, 12, 1)],
    "brass": [index_knob(0.5), alg_knob(2), 16384, 6000, pk(1), pk(1, 0, 13, 7), pk(1, 1, 11, 7), pk(3, 0, 8, 5)],
    "organ": [0, alg_knob(11), 16384, 3000, pk(1), pk(2, 0, 13), pk(4, 0, 11), pk(8, 0, 9)],
    "pluck": [index_knob(0.8), alg_knob(4), 16384, 10000, pk(1, 0, 15, 3), pk(1, 0, 12, 1), pk(3, 0, 12, 1),
              pk(1, 0, 10, 2)],
    "metallic": [index_knob(0.9), alg_knob(8), 16384, 0, pk(1, 0, 15, 3), pk(1, 2, 13, 3), pk(13, 5, 12, 2),
                 pk(9, 7, 10, 3)],
    "pad": [index_knob(0.25), alg_knob(10), 16384, 4000, pk(1, 1, 13, 7), pk(2, 2, 12, 7), pk(1, 0, 12, 6),
            pk(1, 3, 11, 7)],
}
for name, k in list(PRESETS_V0.items()) + [(n + " packed", k) for n, k in PRESETS_PACKED.items()]:
    for f in (110, 440, 1760):
        case(f"preset_{name.replace(' ', '_')}_f{f}", k, f0=f, calls=240)
# extremes: every algorithm with everything at max / min, low and very high notes
for a in range(1, 12):
    for f in (27.5, 4186, 16000):
        case(f"range_a{a}_max_f{round(f)}", [32767, alg_knob(a), 32767, 32767, op(48), op(48), op(48), op(48)], f0=f,
             calls=60)
        case(f"range_a{a}_v0max_f{round(f)}", [32767, alg_knob(a), 0, 32767, 32767, 32767, 32767, 32767], f0=f, calls=60)
        case(f"range_a{a}_low_f{round(f)}", [32767, alg_knob(a), 0, 32767, op(0.5, 15, 1), op(0.5), op(0.5), op(0.5)],
             f0=f, calls=60)


def args(k, f0, calls, f1, extra):
    return " ".join(map(str, k)) + f" {inc(f0)} {calls} {inc(f1) if f1 else 0}" + (f" {extra}" if extra else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    a = ap.parse_args()
    workdir = tempfile.mkdtemp(prefix="fm4_check_")
    run("fm4", "fm4_main", {n: args(*c) for n, c in CASES.items()}, workdir)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    data, worst = {}, (0, "")
    for name in CASES:
        x, info = load(workdir, name)
        data[name] = x
        worst = max(worst, (info["max"], name))
        if info["guard"] != 0x5a5a5a5a:
            check(False, f"{name}: output guard overwritten ({info['guard']:08x})")

    fr, m = spectrum(data["sine440"])
    pk = fr[np.argmax(m)]
    h = harmonics(data["sine440"], F0, 5)
    check(abs(pk - 440) < 1.5 and max(db(v, h[0]) for v in h[1:]) < -70,
          f"sine440: {pk:.2f} Hz, harmonics {' '.join(f'{db(v, h[0]):.0f}' for v in h[1:])} dB")
    check(abs(h[0] - 16383) < 200, f"sine440: amplitude {h[0]:.0f} (one carrier at level 15: 16383, -6 dB)")

    # Bessel: carrier 5 x 200 Hz, modulator 200 Hz, I from the engine's integer gains
    _, _, _, I, _ = decode(CASES["bessel5_1"][0])
    hb = harmonics(data["bessel5_1"], 200, 10)
    errs = []
    def jn(n, x):
        return bessel(abs(n), x) * (-1 if n < 0 and n % 2 else 1)
    for n in range(-4, 5):
        h_ = 5 + n                              # harmonic h gets J_(h-5) and, folded from -h, -J_(-h-5)
        want = abs(jn(h_ - 5, I[1]) - jn(-h_ - 5, I[1]))
        got = hb[h_ - 1] / hb[4] * abs(bessel(0, I[1]))
        errs.append(db(got, want))
    check(max(abs(e) for e in errs) < 0.1, f"5:1 pair, I = {I[1]:.3f}: sidebands n = -4..4 vs J_n(I): "
          + " ".join(f"{e:+.2f}" for e in errs) + " dB")

    # Against the float model: harmonics above -50 dB within 0.5 dB, and nothing the model doesn't have
    def vs_model(name, f0, nh=40, tol=0.5):
        k = CASES[name][0]
        ref = model(k, f0)
        hm = harmonics(ref, f0, nh, skip=3)
        hs = harmonics(data[name], f0, nh)
        top = hm.max()
        err = max(abs(db(hs[i], hm[i])) for i in range(nh) if hm[i] > top * 10 ** (-50 / 20))
        lvl = db(hs.max(), top)
        return err, lvl
    for name, f0 in [("pm1_1", 220), ("pm1_2_hi", 220), ("feedback_saw", 220), ("feedback_half", 220)] + \
            [(f"alg{a}", 110) for a in range(1, 12)]:
        err, lvl = vs_model(name, f0)
        check(err < 0.5 and abs(lvl) < 0.3, f"{name}: harmonics within {err:.2f} dB of the float model (level "
              f"{lvl:+.2f} dB)")
    h = harmonics(data["feedback_saw"], 220, 8)
    print("INFO feedback max (D alone): harmonics " + " ".join(f"{db(v, h[0]):.1f}" for v in h[1:]) + " dB")

    # Envelopes: decay 0.2 s at TIME x1 (and x4 at TIME max): amplitude falls by e^-1 per tau
    def tau_of(x, f0=F0):
        x = x.astype(float)
        per = int(FS / f0 * 4)
        env = np.array([np.abs(x[i:i + per]).max() for i in range(0, len(x) - per, per)])
        t = np.arange(len(env)) * per / FS
        sel = (env > env.max() * 0.05) & (t > 0.01)
        slope = np.polyfit(t[sel], np.log(env[sel]), 1)[0]
        return -1 / slope
    t1, t4 = tau_of(data["env_short_t1"]), tau_of(data["env_short_t4"])
    check(abs(t1 - 0.2) < 0.02 and abs(t4 / t1 - 4) < 0.3, f"envelope 'short': tau {t1 * 1000:.0f} ms at TIME x1 "
          f"(0.2 s), {t4 * 1000:.0f} ms at x4")
    x = data["env_swell"].astype(float)
    a0, a1 = np.abs(x[:2048]).max(), np.abs(x[-4096:]).max()
    check(a0 < a1 * 0.2 and a1 > 15000, f"envelope 'swell': {a0:.0f} at the start, {a1:.0f} after 2 s")
    x = data["env_retrig"].astype(float)
    s0 = 700 * 128
    check(np.abs(x[s0 - 2048:s0]).max() < 500 and np.abs(x[s0:s0 + 2048]).max() > 15000,
          f"note-on restarts the envelopes ({np.abs(x[s0 - 2048:s0]).max():.0f} before, "
          f"{np.abs(x[s0:s0 + 2048]).max():.0f} after)")
    check(np.abs(np.diff(x[s0 - 4:s0 + 4])).max() < 3000, "note-on: carrier phase restarts at 0 (no step: "
          f"{np.abs(np.diff(x[s0 - 4:s0 + 4])).max():.0f})")

    # v0 plain knobs
    err, lvl = vs_model("v0_alg1", 110, tol=0.5)
    check(err < 0.5, f"v0 algorithm 1 (B 2, C 3, D 1 from the SHIFT knobs; modulators at their 'half' sustain): "
          f"within {err:.2f} dB of the model after the decay")
    h = harmonics(data["v0_add"], 110, 4)
    check(abs(db(h[1], h[0]) + 6.02) < 0.2 and abs(db(h[2], h[0]) + 6.02) < 0.2 and db(h[3], h[0]) < -60,
          "v0 additive: A 1, B 2, C 3, D 1: H1 twice H2, H3 (A + D): " + " ".join(f"{db(v, h[0]):.1f}" for v in h)
          + " dB")
    fr, m = spectrum(data["v0_detune"])
    near = (fr > 100) & (fr < 120)
    peaks = sorted(fr[near][np.argsort(m[near])[-3:]])
    check(max(peaks) - min(peaks) > 1.0, f"v0 detune max: partials spread around 110 Hz ({', '.join(f'{p:.2f}' for p in peaks)})")

    pp = {n: int(np.abs(data[n]).max()) for n in CASES if n.startswith("preset")}
    bad = [f"{n} {v}" for n, v in pp.items() if not 3000 <= v <= 30000]
    check(not bad, f"presets: {len(PRESETS_V0)} v0 + {len(PRESETS_PACKED)} packed at 110/440/1760 Hz peak "
          f"{min(pp.values())}..{max(pp.values())} in 0.35 s (3000..30000)" + (": " + ", ".join(bad) if bad else ""))
    for n, k in list(PRESETS_V0.items()) + [(n + " packed", k) for n, k in PRESETS_PACKED.items()]:
        assert all(-32768 <= v <= 32767 for v in k) and (all(0 <= v <= 32767 for v in k) or "packed" in n)
    # Tone limiter: aliasing below 20 kHz at C6..C8 with maximum index
    al = {n: alias_ratio(data[n], CASES[n][1]) for n in CASES if n.startswith("tone")}
    for n, v in sorted(al.items()):
        print(f"INFO {n}: {v:.1f} dB")
    w = max((v, n) for n, v in al.items())
    check(w[0] < -40, f"aliasing below 20 kHz with maximum index (Tone limiter), C6..C8: worst {w[0]:.1f} dB ({w[1]})")

    # Glides and extremes: no runaway (the knee bounds the output; feedback mustn't sit at the rails), no NaN-like noise
    rails = [n for n, x in data.items() if np.mean(np.abs(x[-1280:]) >= 32700) > 0.05]
    # silent is right for: "low" (a 12 ms pluck on every carrier, gone after the 87 ms run), every op at 48 x C8 (above
    # Nyquist, muted), anything at 16 kHz
    silent = [n for n, x in data.items() if n.startswith("range") and "f16000" not in n and "_low_" not in n
              and "_max_f4186" not in n and np.abs(x[-1280:]).max() < 300]
    check(not rails and not silent, f"range: {len(data)} cases, none at the rails, none silent that shouldn't be"
          + (f"; rails: {rails[:4]}" if rails else "") + (f"; silent: {silent[:4]}" if silent else ""))
    hi = [n for n in data if n.startswith("range") and "f16000" in n]
    loud = [n for n in hi if np.abs(data[n][-1280:]).max() > 20000]
    check(not loud, f"16 kHz notes (every op above 0.44 fs fades): none near full scale" + (f": {loud[:4]}" if loud else ""))
    dc = max(abs(data[n][len(data[n]) // 2:].mean()) for n in data if n.startswith("range") and "f28" not in n)
    check(dc < 400, f"DC after the blocker: max |mean| {dc:.0f}")
    for g in ("glide_stack", "glide_v0"):
        x = data[g].astype(float)
        r = 20 * np.log10(np.sqrt(np.array([(x[i:i + 2048] ** 2).mean() for i in range(4096, len(x) - 2048, 2048)]))
                          + 1e-9)
        st = np.abs(np.diff(r)).max()
        check(st < 3, f"{g} 30 Hz -> 12 kHz: max level change between 23 ms windows {st:.2f} dB")

    check(worst[0] <= CPU_BUDGET, f"cycles per render call: max {worst[0]} in {worst[1]} (budget {CPU_BUDGET}, "
          f"{worst[0] / 128:.0f} per sample)")
    for a_ in (1, 11):
        n = f"alg{a_}"
        print(f"INFO {n}: {load(workdir, n)[1]['max']} cycles per call")

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
