#!/usr/bin/env python3
"""Generate src/engine/amp_tables.h for the amp engine (amp.c).

  python3 src/engine/gen_amp_tables.py

Both headers are committed; rerun after changing a voicing below. Design: notes/amp.md.
Filters are Simper's trapezoidal state-variable filters (src/engine/dsp.h) at 88.2 kHz, stored as g = tan(pi fc / fs)
(prewarped, shelves scaled by sqrt(A)), k = 1 / Q and the output mix m0, m1, m2 (Andy Simper, "Linear trapezoidal
integrated SVF", 2013). response() gives the same filters' exact magnitudes (the bilinear transform of the analog
prototypes); amp_check.py compares the simulator against it.
"""
import os

import numpy as np

FS = 88200.0
N = 33                                  # table points per knob (0, 1024, .. 32768)
HERE = os.path.dirname(os.path.abspath(__file__))

# ---- blue: drive (clean -> crunch -> lead) ----
DRIVE_DB = 44.0                         # pre-gain 0 .. +44 dB, linear in dB along the knob
TIGHT = (30.0, 160.0)                   # tight high-pass before the shaper, Hz (exponential along the knob)
EMPH_FC = 900.0                         # pre-emphasis: x + e (x - lp(x)), e = 0 .. EMPH_MAX
EMPH_MAX = 1.0                          # +6 dB above ~1 kHz at full drive (the mid push of a boosted amp)
BIAS = (0.12, 0.30)                     # stage-1 bias (asymmetry: even harmonics)
K2 = (0.6, 4.0)                         # stage-2 gain, quadratic along the knob (the lead's second gain stage)
INTER_FC = 7000.0                       # interstage low-pass
# Loudness compensation (dB) at blue = 0, 1/8, .. 1, measured by amp_check.py's "calibrate" run (rms of the output for
# a -15 dBFS guitar-like note, tone and cab neutral) so every drive setting comes out at about the same loudness.
COMP_DB = [12.7, 8.0, 2.9, -3.5, -8.7, -11.2, -12.8, -13.5, -14.1]

# ---- green: tone (tilt: dark .. flat .. bright) ----
LOW_SHELF = (220.0, 0.7, 5.0)           # fc, Q, dB at full dark (negated at full bright)
HIGH_SHELF = (2200.0, 0.6, -10.0)       # fc, Q, dB at full dark

# ---- orange: cab ----
# anchors at orange 0, 1/3, 2/3, 1: (hp fc, hp Q, mid fc, mid Q, mid dB, pres fc, pres Q, pres dB, lp1 fc, lp1 Q,
# lp2 fc, lp2 Q). 0 = direct (no cab), 1x12 open back, 2x12, 4x12 closed back.
CAB = [
    (20, 0.707, 700, 0.8, 0.0, 2800, 1.2, 0.0, 19000, 0.6, 19000, 0.6),
    (80, 1.0, 700, 0.8, -3.0, 2800, 1.2, 3.0, 6200, 0.8, 7000, 0.6),
    (85, 1.3, 600, 0.8, -4.0, 2500, 1.4, 4.0, 5200, 0.9, 6000, 0.6),
    (95, 1.8, 500, 0.9, -5.0, 2200, 1.5, 4.0, 4300, 1.0, 5000, 0.6),
]
CAB_NAMES = ["DI", "1X12", "2X12", "4X12"]

# ---- white: gate ----
GATE_DB = (-84.0, -30.0)                # threshold at white 1 .. 32767 (0 = off)

# ---- upsampler: half-band, 2K taps per odd phase ----
HB_K = 12
HB_BETA = 7.0

# ---- checks (amp_check.py) ----
RESP_F = np.geomspace(40.0, 16000.0, 48)


def q(v, bits):
    return int(round(v * (1 << bits)))


def svf_mag(f, g, k, m0, m1, m2):
    """|H| of Simper's SVF at frequency f: m0 + m1 BP + m2 LP with s = j tan(pi f / fs) / g."""
    s = 1j * np.tan(np.pi * f / FS) / g
    den = s * s + k * s + 1
    return np.abs(m0 + m1 * s / den + m2 / den)


def lowshelf(fc, qq, db):
    a = 10 ** (db / 40)
    g = np.tan(np.pi * fc / FS) / np.sqrt(a)
    k = 1 / qq
    return g, k, 1.0, k * (a - 1), a * a - 1


def highshelf(fc, qq, db):
    a = 10 ** (db / 40)
    g = np.tan(np.pi * fc / FS) * np.sqrt(a)
    k = 1 / qq
    return g, k, a * a, k * (1 - a) * a, 1 - a * a


def bell(fc, qq, db):
    a = 10 ** (db / 40)
    g = np.tan(np.pi * fc / FS)
    k = 1 / (qq * a)
    return g, k, 1.0, k * (a * a - 1), 0.0


def highpass(fc, qq):
    g, k = np.tan(np.pi * fc / FS), 1 / qq
    return g, k, 1.0, -k, -1.0


def lowpass(fc, qq):
    g, k = np.tan(np.pi * fc / FS), 1 / qq
    return g, k, 0.0, 0.0, 1.0


def tone_filters(i):
    t = 2 * i / (N - 1) - 1                                     # -1 dark .. +1 bright
    return [lowshelf(LOW_SHELF[0], LOW_SHELF[1], -LOW_SHELF[2] * t),
            highshelf(HIGH_SHELF[0], HIGH_SHELF[1], -HIGH_SHELF[2] * t)]


def cab_params(i):
    x = 3 * i / (N - 1)
    j = min(int(x), 2)
    f = x - j
    a, b = CAB[j], CAB[j + 1]
    out = []
    for n, (u, v) in enumerate(zip(a, b)):
        is_fc = n in (0, 2, 5, 8, 10)
        out.append(np.exp(np.log(u) + f * (np.log(v) - np.log(u))) if is_fc else u + f * (v - u))
    return out


def cab_filters(i):
    hp, hq, mf, mq, md, pf, pq, pd, l1, l1q, l2, l2q = cab_params(i)
    return [highpass(hp, hq), bell(mf, mq, md), bell(pf, pq, pd), lowpass(l1, l1q), lowpass(l2, l2q)]


def response(filters, f):
    m = np.ones_like(f)
    for g, k, m0, m1, m2 in filters:
        m = m * svf_mag(f, g, k, m0, m1, m2)
    return m


def halfband():
    n = np.arange(HB_K) + 0.5
    w = np.kaiser(2 * HB_K + 1, HB_BETA)[HB_K + 1:]            # window at |n| = 1 .. K (odd taps at half steps)
    w = np.interp(n, np.arange(1, HB_K + 1) - 0.5, w)
    c = np.sinc(n) * w
    return c / (2 * c.sum())


def fmt(name, ctype, vals, per=8):
    body = ",\n    ".join(", ".join(str(v) for v in vals[i:i + per]) for i in range(0, len(vals), per))
    return f"static const {ctype} {name}[{len(vals)}] = {{\n    {body} }};\n"


def blue_tables():
    b = np.arange(N) / (N - 1)
    gain = 10 ** (DRIVE_DB * b / 20)
    tight = TIGHT[0] * (TIGHT[1] / TIGHT[0]) ** b
    tight_a = 1 - np.exp(-2 * np.pi * tight / FS)
    emph = EMPH_MAX * b
    bias = BIAS[0] + (BIAS[1] - BIAS[0]) * b
    k2 = K2[0] + (K2[1] - K2[0]) * b * b
    comp = 10 ** (np.interp(b, np.arange(9) / 8, COMP_DB) / 20)
    return gain, tight_a, emph, bias, k2, comp


def main():
    gain, tight_a, emph, bias, k2, comp = blue_tables()
    emph_a = 1 - np.exp(-2 * np.pi * EMPH_FC / FS)
    inter_a = 1 - np.exp(-2 * np.pi * INTER_FC / FS)
    hb = halfband()

    # Engine: per knob point one row of int32 values, interpolated per block (amp.c knob_row()).
    blue_rows = [[q(gain[i], 23), q(tight_a[i], 31), q(emph[i], 15), q(bias[i], 20), q(k2[i], 12), q(comp[i], 12)]
                 for i in range(N)]

    def svf_row(fl):
        row = []
        for g, k, m0, m1, m2 in fl:
            row += [q(g, 28), q(k, 28), q(m0, 29), q(m1, 29), q(m2, 29)]
        return row

    def tone_engine(i):              # the high shelf's m1, m2 divided by its m0 = A^2 (amp.c multiplies A^2 into the loudness)
        lo, (g, k, m0, m1, m2) = tone_filters(i)
        return [lo, (g, k, m0, m1 / m0, m2 / m0)]

    green_rows = [svf_row(tone_engine(i)) for i in range(N)]
    orange_rows = [svf_row(cab_filters(i)) for i in range(N)]
    gate = [q(10 ** ((GATE_DB[0] + (GATE_DB[1] - GATE_DB[0]) * i / (N - 1)) / 20), 15) for i in range(N)]
    for rows in (green_rows, orange_rows):
        for r in rows:
            assert all(-2**31 <= v < 2**31 for v in r), r
            for j in range(0, len(r), 5):
                g, k = r[j] / 2**28, r[j + 1] / 2**28
                assert g * (g + k) < 7 and all(abs(m) < 4 * 2**29 for m in r[j + 2:j + 5])

    flat = lambda rows: [v for r in rows for v in r]
    h = ["/* Generated by gen_amp_tables.py; do not edit. Tables for amp.c (notes/amp.md). */",
         "#ifndef AMP_TABLES_H", "#define AMP_TABLES_H", "#include <stdint.h>", "",
         f"#define AMP_N {N}                         /* points per knob: 0, 1024, .. 32768 */",
         f"#define AMP_HB_K {HB_K}                   /* upsampler taps per side of the odd phase */",
         f"#define AMP_EMPH_A {q(emph_a, 31)}        /* pre-emphasis low-pass ({EMPH_FC:.0f} Hz), Q31 */",
         f"#define AMP_INTER_A {q(inter_a, 31)}      /* interstage low-pass ({INTER_FC:.0f} Hz), Q31 */",
         "/* half-band odd-phase taps (Q15): mid = sum c[k] (x[c - k] + x[c + 1 + k]) */",
         fmt("amp_hb", "int16_t", [q(v, 15) for v in hb], 12),
         "/* blue rows: gain Q23, tight high-pass a Q31, pre-emphasis e Q15, bias Q20, stage-2 gain Q12, loudness Q12 */",
         "#define AMP_BLUE_W 6",
         fmt("amp_blue", "int32_t", flat(blue_rows), 6),
         "/* green rows: low shelf, high shelf (m1, m2 divided by m0 = A^2); per filter g Q28, k Q28, m0 m1 m2 Q29 */",
         "#define AMP_GREEN_W 10",
         fmt("amp_green", "int32_t", flat(green_rows), 10),
         "/* orange rows: high-pass, mid bell, presence bell, low-pass, low-pass; per filter g Q28, k Q28, m0 m1 m2 Q29 */",
         "#define AMP_ORANGE_W 25",
         fmt("amp_orange", "int32_t", flat(orange_rows), 10),
         "/* white: gate open threshold (Q15 peak) at white 0 .. 32768 (0 = gate off; amp.c) */",
         fmt("amp_gate_thr", "int32_t", gate, 11),
         "#endif", ""]
    open(os.path.join(HERE, "amp_tables.h"), "w").write("\n".join(h))

    # Summary for the notes
    for i in (0, 16, 32):
        print(f"blue {i:2}: gain {20*np.log10(gain[i]):5.1f} dB, tight {TIGHT[0] * (TIGHT[1] / TIGHT[0]) ** (i / 32):5.1f} Hz, "
              f"emph {emph[i]:.2f}, bias {bias[i]:.2f}, k2 {k2[i]:.2f}, comp {20*np.log10(comp[i]):+.1f} dB")
    for i in (0, 11, 22, 32):
        r = 20 * np.log10(response(cab_filters(i), np.array([60, 100, 500, 1000, 2500, 5000, 8000, 12000])))
        print(f"cab {i:2}:", " ".join(f"{v:+.1f}" for v in r))
    for i in (0, 16, 32):
        r = 20 * np.log10(response(tone_filters(i), np.array([60, 200, 1000, 3000, 8000])))
        print(f"tone {i:2}:", " ".join(f"{v:+.1f}" for v in r))
    w = np.linspace(0, np.pi, 4096)
    taps = np.zeros(4 * HB_K + 1)
    taps[2 * HB_K] = 1
    for k, c in enumerate(hb):
        taps[2 * HB_K + 1 + 2 * k] = taps[2 * HB_K - 1 - 2 * k] = c
    H = np.abs(np.exp(-1j * np.outer(w, np.arange(len(taps)))) @ taps) / 2
    f = w / np.pi * FS / 2
    print(f"half-band: passband ripple to 18 kHz {20*np.log10(H[f < 18000].max()):+.3f} / {20*np.log10(H[f < 18000].min()):+.3f} dB, "
          f"stop band from 26 kHz {20*np.log10(H[f > 26000].max()):.1f} dB, from 34 kHz {20*np.log10(H[f > 34000].max()):.1f} dB")


if __name__ == "__main__":
    main()
