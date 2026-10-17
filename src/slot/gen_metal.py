#!/usr/bin/env python3
"""Generate the metal engine's tables (src/slot/metal_tables.h) and factory kits (src/slot/metal_kits.json).

  python3 src/slot/gen_metal.py

Both outputs are committed; rerun after changing a curve or a kit below. Engine: src/slot/metal.c; design and knob
mapping: notes/drum-engine.md "metal". The knob curves live here once: the engine reads the tables, the screen
(ui_metal.h) shows the same values, src/engine/test/metal_check.py imports this module to check both.

Per key, 8 int16 (TE's dbox per-key values, range row 9: K0 -24320..24832, K1-K7 0..32767). Detents are 1024 apart.
  K0 blue    PITCH     the lowest mode, 512 per semitone, 0 = A3 (220 Hz): 14 Hz .. 3.5 kHz
  K1 green   MATERIAL  BAR (detent 0), PLATE (8), BELL (16), PIPE (24), SHEET (32), morphing in between: the 8 modes'
                       ratios (geometric), levels and decay slopes
  K2 white   DRIVE     0 clean; FOLD 1-10 (a sine folder, gain 1 .. 10); CLIP 11-20 (each mode turns into a square:
                       the 808's square cluster, then a hard clip); SCREAM 21-32 (ring mod with a feedback-FM
                       operator, then that operator FMs the modes; feedback to chaos)
  K3 orange  DECAY     ring T60 of the lowest mode, 30 ms .. 16 s (higher modes shorter, by material)
  K4 SHIFT+blue    STRIKE  BOOM 0-7 (a heavy beater: pitch drop +24 .. 0 st), MALLET 8-15 (soft -> hard), HAMMER
                           16-23 (steel: bright, a click), SCRAPE 24-28 (a noise burst that rings the modes: resonant
                           noise, wide -> narrow), RATTLE 29-32 (the burst chopped into random impacts: chains)
  K5 SHIFT+green   SPREAD  mode stretch: TIGHT 0-15 (ratios^0.25 .. ^1, detuned: a beating cluster), NATURAL 16
                           (the material), WIDE 17-32 (ratios^1 .. ^2)
  K6 SHIFT+white   GRIT    0 clean; CRUSH 1-12 (14 .. 3 bits); ALIAS 13-32 (sample and hold 88 kHz .. 300 Hz, half an
                           octave per detent, the bits relax 3 .. 8)
  K7 SHIFT+orange  NDECAY  the exciter's own T60 (noise, click, thud, the BOOM drop, the scrape), 1.5 ms .. 4 s;
                           independent of DECAY
Keys 10..13 are a choke group (metal.c). Kits share the key roles: 0-2 kicks, 3/5/7 toms, 4/6 snares, 8 rim, 9 clap,
10-13 hats, 14-15 cymbals, 16-23 free.
"""
import json
import math
import os
import re

import numpy as np

FS = 88200.0
BLOCK = 128
HERE = os.path.dirname(os.path.abspath(__file__))

# ---- curves ----
A3 = 220.0
SEMI = 512
OCT = 12 * SEMI                            # K0 units per octave
K0_MIN, K0_MAX = -24320, 24832
STOCK_LO = [-24320, 0, 0, 1296, 0, 0, 0, 0]          # the stock dbox presets' per-column range: factory kits stay inside
STOCK_HI = [24638, 32767, 32767, 32767, 32767, 32767, 32767, 32255]
DECAY = (0.03, 16.0)                       # K3: ring T60 of the lowest mode, s
NDECAY = (0.0015, 4.0)                     # K7: exciter T60, s
NMODES = 8
MATERIALS = ["BAR", "PLATE", "BELL", "PIPE", "SHEET"]
# per material: mode ratios (to the lowest), levels, decay slope alpha (a mode's T60 = DECAY x ratio^-alpha)
MAT = {
    "BAR": ([1.0, 2.756, 5.404, 8.933, 13.344, 18.637, 24.812, 31.870],      # free-free beam (xylophone, anvil)
            [1.0, 0.55, 0.38, 0.27, 0.2, 0.15, 0.11, 0.08], 0.55),
    "PLATE": ([1.0, 1.73, 2.33, 3.91, 4.11, 6.30, 6.61, 8.77],               # circular plate, free edge: doublets
              [1.0, 0.85, 0.75, 0.62, 0.6, 0.48, 0.45, 0.36], 0.6),
    "BELL": ([1.0, 2.0, 2.39, 3.01, 4.0, 5.04, 5.94, 8.06],                   # hum, prime, tierce, quint, nominal ..
             [0.75, 1.0, 0.8, 0.5, 0.85, 0.42, 0.35, 0.3], 0.3),
    "PIPE": ([1.0, 1.012, 2.03, 3.09, 4.18, 5.31, 6.47, 7.66],                # stretched harmonics, a beating pair
             [1.0, 0.8, 0.7, 0.6, 0.5, 0.42, 0.35, 0.3], 0.45),
    "SHEET": ([1.0, 1.4827, 1.8003, 2.546, 2.6303, 3.8968, 5.21, 6.93],      # the 808's six squares' ratios + two
              [0.9, 1.0, 0.85, 0.9, 0.8, 0.75, 0.6, 0.5], 0.8),
}
DETUNE = [0, 9, -13, 7, -11, 15, -6, 10]   # cents at the ends of SPREAD (TIGHT 0, WIDE 32), 0 at NATURAL
MODE_TOP = 12000.0                         # Hz: modes fade out from MODE_FADE to here (the magic circle's e < 1)
MODE_FADE = 9000.0
OP_RATIO = 1.4142                          # SCREAM's FM operator: PITCH x this (ring carrier and the modes' modulator)
MODE_AMP = 20000                           # Q15: the strike weights' RMS sum (headroom for the drive)
ZONE_STRIKE = [("BOOM", 0, 8), ("MALLET", 8, 16), ("HAMMER", 16, 24), ("SCRAPE", 24, 29), ("RATTLE", 29, 33)]
ZONE_DRIVE = [("CLEAN", 0, 1), ("FOLD", 1, 11), ("CLIP", 11, 21), ("SCREAM", 21, 33)]
ZONE_GRIT = [("CLEAN", 0, 1), ("CRUSH", 1, 13), ("ALIAS", 13, 33)]
ZONE_SPREAD = [("TIGHT", 0, 16), ("NATURAL", 16, 17), ("WIDE", 17, 33)]


def _edges(zones):
    return [a for _, a, _ in zones] + [zones[-1][2]]


def mt_strike_edges():
    return _edges(ZONE_STRIKE)


def mt_drive_edges():
    return _edges(ZONE_DRIVE)


def mt_grit_edges():
    return _edges(ZONE_GRIT)


SOUND_LAST = {"STRIKE": 29, "DRIVE": 21, "GRIT": 13}   # the last zone's first detent (metal.h mt_*_pos)


def sound_pos(k, knob):
    """the table position a zoned knob's value plays (metal.h mt_sound): one detent ahead, the shift taken back
    across the last zone"""
    a = SOUND_LAST[knob] * 1024
    mul = int(round(1023 * 65536 / (32767 - a)))
    x = max(k, 0)
    x = x + 1023 - (((x - a) * mul) >> 16 if x > a else 0)
    return min(32767, x)


def raw_of_pos(pos, knob):
    """the smallest knob value that plays table position pos or above"""
    lo, hi = 0, 32767
    while lo < hi:
        mid = (lo + hi) // 2
        if sound_pos(mid, knob) >= pos:
            hi = mid
        else:
            lo = mid + 1
    return lo


def pitch_hz(k0):
    return A3 * 2 ** (k0 / OCT)


def k0_of(hz):
    return int(round(OCT * math.log2(hz / A3)))


def expo(k, lo, hi):
    """value of a 256-step exponential table at knob k (index k >> 7)"""
    return lo * (hi / lo) ** ((max(k, 0) >> 7) / 255.0)


def k_expo(v, lo, hi):
    i = int(round(255 * math.log(v / lo) / math.log(hi / lo)))
    return min(255, max(0, i)) * 128 + 64


def zone_of(zones, k):
    d = max(k, 0) / 1024.0
    for name, a, b in zones:
        if d < b:
            return name, a, b
    return zones[-1]


def geo(a, b, t):
    return a * (b / a) ** t


def lin(a, b, t):
    return a + (b - a) * t


def zt(k, a, b):
    """position 0..1 of knob k in a zone of detents a..b (b = the next zone's first detent; the last detent of the
    knob, 32, is 1.0)"""
    d = max(k, 0) / 1024.0
    return min(1.0, max(0.0, (d - a) / (min(b, 32) - a))) if min(b, 32) > a else 1.0


# ---- MATERIAL: per material (5) x 8 modes: log2 ratio (K0 units), level Q15; alpha Q12 ----
def mat_tables():
    L, A, AL = [], [], []
    for m in MATERIALS:
        r, a, al = MAT[m]
        assert len(r) == len(a) == NMODES and r[0] == 1.0
        L.append([int(round(OCT * math.log2(x))) for x in r])
        A.append([int(round(32767 * x)) for x in a])
        AL.append(int(round(al * 4096)))
    return L, A, AL


def material_at(k1):
    """the mode ratios, levels and alpha at K1 (the engine's morph: geometric ratios, linear levels and alpha)"""
    L, A, AL = mat_tables()
    k = min(max(k1, 0), 32767)
    i, f = min(k >> 13, 3), (k - (min(k >> 13, 3) << 13))
    return ([L[i][j] + (((L[i + 1][j] - L[i][j]) * f) >> 13) for j in range(NMODES)],
            [A[i][j] + (((A[i + 1][j] - A[i][j]) * f) >> 13) for j in range(NMODES)],
            AL[i] + (((AL[i + 1] - AL[i]) * f) >> 13))


# ---- SPREAD (K5), 129 points: stretch Q12, detune amount Q15 ----
def spread_point(k):
    d = max(k, 0) / 1024.0
    if d <= 16:
        s, det = lin(0.25, 1.0, d / 16), 1.0 - d / 16
    else:
        s, det = lin(1.0, 2.0, (d - 16) / 16), (d - 16) / 16
    return s, det


# ---- STRIKE (K4), 129 points ----
STRIKE_COLS = ["bend", "fc", "slope", "att", "swell", "exl", "exc", "rnw", "rnc", "hold", "chop"]


def strike_point(k):
    """bend (st at the start, decaying with NDECAY), fc (Hz: the strike's brightness), slope (dB per octave above fc),
    att (ms: the modes' attack), swell (x NDECAY's T60 added to it), exl (the exciter's own level), exc (Hz: its
    one-pole low-pass), rnw (resonant noise: the modes AM-ed by low-passed noise, 0..1), rnc (Hz: that noise's
    band), hold (the modes held up by the exciter's envelope while it lasts), chop (RATTLE: random impacts)"""
    name, a, b = zone_of(ZONE_STRIKE, k)
    t = zt(k, a, b)
    p = dict(bend=0.0, fc=16000.0, slope=4.0, att=0.0, swell=0.0, exl=0.45, exc=14000.0, rnw=0.0, rnc=2000.0,
             hold=0.0, chop=0.0)
    if name == "BOOM":                      # +3 st per detent less drop, the softest beater
        p.update(bend=24 * (1 - t), fc=geo(120, 300, t), slope=18.0, att=5.0, exl=lin(0.30, 0.15, t),
                 exc=geo(150, 300, t))
    elif name == "MALLET":
        p.update(fc=geo(300, 2500, t), slope=lin(18, 12, t), att=lin(5.0, 1.0, t), exl=lin(0.15, 0.12, t),
                 exc=geo(300, 2000, t))
    elif name == "HAMMER":
        p.update(fc=geo(2500, 16000, t), slope=lin(12, 4, t), att=lin(1.0, 0.0, t), exl=lin(0.12, 0.45, t),
                 exc=geo(2000, 14000, t))
    elif name == "SCRAPE":                  # the burst rings the modes: wide resonant noise, narrowing (singing)
        p.update(swell=lin(0.0, 0.3, t), exl=lin(0.45, 0.25, t), exc=geo(14000, 20000, t), rnw=lin(0.0, 1.0, t ** 0.7),
                 rnc=geo(2000, 80, t), hold=lin(0.3, 1.0, t))
    else:                                   # RATTLE: chopped into impacts
        p.update(swell=lin(0.3, 0.0, t), exl=0.25, exc=20000.0, rnw=1.0, rnc=geo(80, 600, t), hold=1.0,
                 chop=lin(0.25, 1.0, t))
    return p


# ---- DRIVE (K2), 129 points ----
DRIVE_COLS = ["fw", "fg", "sq", "cg", "rw", "fb", "fmd", "lv"]


def drive_point(k):
    """fw (fold mix), fg (fold gain: sin(pi / 2 fg s)), sq (each mode -> its square), cg (clip gain), rw (ring mod
    with the operator), fb (the operator's feedback beta, rad), fmd (the operator FMs the modes: e x (1 + fmd m)),
    lv (level after the clip; set by level_comp)"""
    name, a, b = zone_of(ZONE_DRIVE, k)
    t = zt(k, a, b)
    p = dict(fw=0.0, fg=1.0, sq=0.0, cg=1.0, rw=0.0, fb=0.0, fmd=0.0)
    if name == "FOLD":                      # (from CLEAN's values: the knob plays a detent ahead, detent 1 = t 0.1)
        p.update(fw=min(1.0, 3.0 * t), fg=geo(1.0, 10.0, t))
    elif name == "CLIP":
        p.update(fw=lin(1.0, 0.5, t), fg=geo(10.0, 2.0, t), sq=t ** 0.8, cg=geo(1.0, 8.0, t))
    elif name == "SCREAM":
        # the squares give way to the ring over detents 21..24; the operator FMs the modes from 24 (never both: the
        # pair loop for squares and FM together is the dearest)
        p.update(fw=0.5, fg=2.0, sq=max(0.0, 1.0 - t / 0.25), cg=geo(8.0, 5.0, t), rw=min(1.0, t * 2.2),
                 fb=6.0 * t ** 1.3, fmd=0.0 if t <= 0.25 else 0.55 * ((t - 0.25) / 0.75) ** 1.2)
    return p


# ---- GRIT (K6), 129 points ----
GRIT_COLS = ["bits", "hold"]


def grit_point(k):
    """bits (Q8, 16 = off), hold (sample-and-hold increment, 2^32 = every sample)"""
    name, a, b = zone_of(ZONE_GRIT, k)
    t = zt(k, a, b)
    if name == "CLEAN":
        return dict(bits=16.0, rate=FS)
    d = max(k, 0) / 1024.0
    if name == "CRUSH":                     # 16 bits (CLEAN) -> 3, fast at first: detent 1 already ~12 bits
        return dict(bits=16.0 - 13.0 * t ** 0.5, rate=FS)
    return dict(bits=lin(3.0, 8.0, t), rate=geo(FS, 300.0, t))


def per_block_t60(t60):
    return 10 ** (-3 * BLOCK / (t60 * FS))


def table129(fn, cols, conv):
    rows = []
    for i in range(129):
        p = fn(min(i * 256, 32767))
        rows.append([conv[c](p[c]) for c in cols])
    return rows


def q15(x):
    return max(0, min(32767, int(round(x * 32767))))


def onepole(fc):
    """one-pole low-pass coefficient Q15: 1 - exp(-2 pi fc / fs)"""
    return q15(1 - math.exp(-2 * math.pi * min(fc, 20000) / FS))


def lp_noise_rms(c):
    """RMS of metal.c's white noise (uniform +-0.5 of full scale) through y += c (x - y): sqrt(c / (2 - c)) / sqrt(12)"""
    c = c / 32768
    return math.sqrt(c / (2 - c)) / math.sqrt(12)


def strike_rows():
    rows = []
    for i in range(129):
        p = strike_point(min(i * 256, 32767))
        c1, c2 = onepole(p["exc"]), onepole(p["rnc"])
        # exciter: the low-passed noise's level, normalised (RMS of a full-scale white noise x exl)
        exg = min(32767, int(round(p["exl"] * 0.33 / lp_noise_rms(c1) * 4096)))       # Q12
        rng = min(32767, int(round(0.5 / lp_noise_rms(c2) * 512)))                     # Q9: rn RMS 0.5
        rows.append([int(round(p["bend"] * SEMI)), k0_of(p["fc"]) >> 1, int(round(p["slope"] / 6.0206 * 4096)),
                     int(round(p["att"] * FS / 1000)), q15(p["swell"]), exg, c1, q15(p["rnw"]), c2, rng,
                     q15(p["hold"]), q15(p["chop"])])
    return rows


STRIKE_TCOLS = ["bend", "fcq", "slope", "att", "swell", "exg", "exc", "rnw", "rnc", "rng", "hold", "chop"]


def drive_rows(lv):
    rows = []
    for i in range(129):
        p = drive_point(min(i * 256, 32767))
        rows.append([q15(p["fw"]), int(round(p["fg"] * 2048)), q15(p["sq"]), int(round(p["cg"] * 1024)), q15(p["rw"]),
                     int(round(p["fb"] * 256)), q15(p["fmd"]), lv[i]])
    return rows


def grit_rows():
    rows = []
    for i in range(129):
        p = grit_point(min(i * 256, 32767))
        rows.append([int(round(p["bits"] * 256)), min(0xFFFFFFFF, int(round(p["rate"] / FS * 2 ** 32)))])
    return rows


# level after the drive stage, Q12 per DRIVE point (1/129 steps): measured by metal_check.py --level-comp (the RMS of
# a reference strike through the engine at each point, matched to DRIVE 0's within LEVEL_LIFT), pasted here
def LEVEL_LIFT_DB(i):
    """how much louder (dB) than DRIVE 0 the reference strikes may be at DRIVE table point i (equal RMS made driven
    keys sound weak): 0 .. +4 dB across FOLD, +4 .. +7 across CLIP, +7 in SCREAM"""
    d = min(i * 256, 32767) / 1024
    return 0.0 if d < 1 else 4 * (d - 1) / 10 if d < 11 else 4 + 3 * (d - 11) / 10 if d < 21 else 7.0


LV = [4096, 3924, 3721, 3498, 3263, 3057, 2850, 2646, 2449, 2263, 2088, 1927, 1779, 1644, 1573, 1531,
      1493, 1458, 1426, 1397, 1371, 1347, 1325, 1306, 1288, 1272, 1259, 1246, 1236, 1226, 1218, 1211,
      1205, 1201, 1197, 1194, 1192, 1191, 1191, 1193, 1196, 1170, 1154, 1141, 1132, 1122, 1115, 1110,
      1105, 1101, 1099, 1098, 1098, 1099, 1101, 1103, 1106, 1110, 1113, 1117, 1122, 1125, 1129, 1133,
      1137, 1140, 1144, 1149, 1153, 1158, 1163, 1169, 1176, 1184, 1192, 1199, 1207, 1215, 1223, 1231,
      1238, 1267, 1303, 1350, 1416, 1486, 1587, 1700, 1774, 1797, 1805, 1817, 1821, 1799, 1779, 1760,
      1740, 1728, 1720, 1709, 1695, 1687, 1570, 1487, 1451, 1429, 1418, 1416, 1416, 1422, 1427, 1440,
      1454, 1460, 1478, 1502, 1520, 1542, 1552, 1578, 1600, 1598, 1594, 1608, 1608, 1604, 1614, 1601,
      1583]


def c_array(name, ctype, vals, per=12):
    s = f"static const {ctype} {name}[{len(vals)}] __attribute__((unused)) = {{\n"
    for i in range(0, len(vals), per):
        s += "    " + ", ".join(str(v) for v in vals[i:i + per]) + ",\n"
    return s + "};\n"


# ---- the screen's objects (ui_metal.h): per material an outline (24 points, closed) and two detail lines (8 points
# each), resampled along their corners, px about the object's centre at the largest size; MATERIAL morphs point by point
def _resample(pts, n, closed):
    pts = [np.array(p, float) for p in pts] + ([np.array(pts[0], float)] if closed else [])
    seg = [np.linalg.norm(pts[i + 1] - pts[i]) for i in range(len(pts) - 1)]
    tot, out = sum(seg), []
    for j in range(n):
        t = tot * j / (n if closed else n - 1)
        i = 0
        while i < len(seg) - 1 and t > seg[i]:
            t -= seg[i]
            i += 1
        f = t / seg[i] if seg[i] else 0
        out.append(pts[i] + (pts[i + 1] - pts[i]) * min(f, 1.0))
    return [(int(round(p[0])), int(round(p[1]))) for p in out]


def _arc(cx, cy, rx, ry, a0, a1, n):
    return [(cx + rx * math.cos(a0 + (a1 - a0) * i / (n - 1)), cy + ry * math.sin(a0 + (a1 - a0) * i / (n - 1)))
            for i in range(n)]


def shapes():
    pi = math.pi
    sh = {}
    sh["BAR"] = (_resample([(-60, 12), (-60, -4), (-46, -14), (66, -14), (66, 2), (52, 12)], 24, True),
                 _resample([(-60, -4), (52, -4), (66, -14)], 8, False), _resample([(52, -4), (52, 12)], 8, False))
    sh["PLATE"] = (_resample(_arc(0, -2, 54, 17, pi, 3 * pi, 48)[:-1], 24, True),
                   _resample(_arc(0, 3, 54, 17, 0, pi, 16), 8, False), _resample(_arc(0, -2, 7, 2.5, 0, 2 * pi, 16), 8, False))
    bell = [(-46, 32), (-34, 22), (-26, 4), (-22, -14), (-18, -30), (-12, -38), (12, -38), (18, -30), (22, -14), (26, 4),
            (34, 22), (46, 32)]
    sh["BELL"] = (_resample(bell, 24, True), _resample(_arc(0, 32, 46, 6, 0, pi, 16), 8, False),
                  _resample([(-6, -38), (-5, -44), (0, -47), (5, -44), (6, -38)], 8, False))
    pipe = _arc(0, -42, 14, 5, pi, 2 * pi, 12) + [(14, 42)] + _arc(0, 42, 14, 5, 0, pi, 12)
    sh["PIPE"] = (_resample(pipe, 24, True), _resample(_arc(0, -42, 14, 5, 0, pi, 12), 8, False),
                  _resample([(-7, -34), (-7, 34)], 8, False))
    edge = lambda y: 4 * math.sin(y / 9.0)
    sheet = [(-42, -36), (42, -36)] + [(42 + edge(y), y) for y in range(-30, 41, 10)] \
        + [(x, 40 + 3 * math.sin(x / 8.0)) for x in range(36, -43, -12)] + [(-42 + edge(y), y) for y in range(34, -31, -10)]
    sh["SHEET"] = (_resample(sheet, 24, True), _resample([(-30, -36), (0, -47), (30, -36)], 8, False),
                   _resample([(-20 + 3 * math.sin(t), -30 + t * 9.5) for t in range(8)], 8, False))
    for m, (o, a, b) in sh.items():
        assert len(o) == 24 and len(a) == 8 and len(b) == 8 and all(-80 <= v <= 80 for p in o + a + b for v in p), m
    return sh


# ---- factory kits ----
def _clamp(k):
    for j, v in enumerate(k):
        k[j] = min(STOCK_HI[j], max(STOCK_LO[j], v))
    return k


def det(d):
    """a knob value on detent d (0..32), the stock maximum for 32 on K7"""
    return min(32767, int(round(d * 1024)))


def MATK(name, toward=None, amount=0.0):
    """K1 for a material, optionally part of the way to the next one"""
    i = MATERIALS.index(name)
    return min(32767, int(round(i * 8192 + amount * 8192)))


def key(hz, mat, drive=0, decay=1.0, strike=12, spread=16, grit=0, ndecay=0.05):
    """pitch Hz, MATERIAL raw K1 (MATK), DRIVE / STRIKE / SPREAD / GRIT in detents (float ok), DECAY / NDECAY in s"""
    return _clamp([k0_of(hz), mat, int(round(drive * 1024)), k_expo(decay, *DECAY), int(round(strike * 1024)),
                   int(round(spread * 1024)), int(round(grit * 1024)), k_expo(ndecay, *NDECAY)])


B, PL, BE, PI, SH = (MATK(m) for m in MATERIALS)
KITS = {
    "metal foundry": ("lo-fi", [
        key(48, B, 6, 0.5, 2, 16, 0, 0.18),          # 0 F   anvil kick (BOOM, folded)
        key(42, PL, 3, 0.8, 0, 12, 0, 0.25),         # 1 F#  tank kick
        key(55, B, 14, 0.35, 4, 16, 0, 0.12),        # 2 G   square kick (CLIP)
        key(110, PI, 4, 0.7, 8, 16, 0, 0.06),        # 3 G#  pipe tom low
        key(190, PL, 8, 0.3, 26, 14, 0, 0.12),       # 4 A   sheet snare (SCRAPE)
        key(160, PI, 4, 0.6, 9, 16, 0, 0.06),        # 5 A#  pipe tom mid
        key(230, SH, 13, 0.25, 25, 18, 2, 0.1),      # 6 B   clank snare (CLIP + SCRAPE)
        key(235, PI, 4, 0.5, 10, 16, 0, 0.05),       # 7 C   pipe tom high
        key(1250, B, 2, 0.12, 21, 18, 0, 0.01),      # 8 C#  rim: anvil tick
        key(500, SH, 10, 0.18, 30, 14, 0, 0.1),      # 9 D   clap: rattled sheet
        key(1800, SH, 14, 0.06, 22, 20, 0, 0.02),    # 10 D# closed hat (808 cluster)
        key(1900, SH, 14, 0.035, 22, 20, 0, 0.01),   # 11 E  pedal chick
        key(1800, SH, 14, 0.18, 22, 20, 0, 0.05),    # 12 F  half-open hat
        key(1800, SH, 14, 0.8, 22, 20, 0, 0.1),      # 13 F# open hat
        key(330, PL, 9, 4.0, 25, 10, 0, 0.4),        # 14 G  sheet crash
        key(95, PL, 2, 9.0, 6, 8, 0, 0.3),           # 15 G# gong
        key(880, B, 0, 1.6, 20, 16, 0, 0.004),       # 16 A  anvil (clean)
        key(310, BE, 1, 3.5, 14, 16, 0, 0.01),       # 17 A# bell
        key(600, PI, 0, 2.5, 18, 17, 0, 0.004),      # 18 B  chime
        key(150, SH, 18, 0.6, 31, 15, 0, 0.6),       # 19 C  chains (RATTLE)
        key(70, PL, 22, 1.2, 27, 12, 0, 1.2),        # 20 C# grinder (SCRAPE + SCREAM)
        key(440, BE, 6, 2.0, 17, 22, 0, 0.01),       # 21 D  struck bell, folded
        key(120, PI, 10, 1.2, 1, 16, 0, 0.3),        # 22 D# pipe drop (BOOM)
        key(2600, B, 0, 0.5, 23, 16, 0, 0.003),      # 23 E  ping
    ]),
    "metal scrap": ("dem drums", [
        key(52, SH, 12, 0.4, 1, 16, 4, 0.15),        # 0 drum-can kick
        key(45, PI, 8, 0.6, 0, 16, 0, 0.2),          # 1 oil drum kick
        key(60, B, 20, 0.3, 3, 16, 0, 0.1),          # 2 clipped thud
        key(100, PL, 6, 0.6, 12, 10, 0, 0.05),       # 3 trash tom low
        key(210, SH, 12, 0.25, 27, 16, 4, 0.12),     # 4 trash snare
        key(140, PL, 6, 0.5, 13, 10, 0, 0.05),       # 5 trash tom mid
        key(260, PL, 16, 0.2, 24, 20, 8, 0.08),      # 6 can snare (crushed)
        key(200, PL, 6, 0.4, 14, 10, 0, 0.05),       # 7 trash tom high
        key(1600, PI, 6, 0.08, 20, 16, 0, 0.008),    # 8 can rim
        key(700, PL, 4, 0.15, 31, 18, 6, 0.08),      # 9 clap: hubcap rattle
        key(2200, PL, 12, 0.05, 22, 24, 0, 0.015),   # 10 closed lid hat
        key(2400, PL, 12, 0.03, 22, 24, 0, 0.008),   # 11 pedal
        key(2200, PL, 12, 0.15, 22, 24, 0, 0.04),    # 12 half-open
        key(2200, PL, 12, 0.6, 22, 24, 0, 0.08),     # 13 open lid
        key(260, SH, 17, 3.0, 26, 12, 3, 0.6),       # 14 trash crash
        key(400, SH, 23, 2.0, 25, 20, 0, 0.3),       # 15 china scream
        key(1100, PI, 2, 0.9, 19, 18, 0, 0.005),     # 16 pipe ping
        key(85, PL, 4, 6.0, 9, 6, 0, 0.4),           # 17 sheet thunder
        key(180, SH, 9, 0.8, 32, 14, 0, 1.0),        # 18 chain drag
        key(320, B, 26, 0.6, 18, 16, 10, 0.02),      # 19 bit-crushed clang
        key(130, PL, 28, 1.5, 28, 14, 0, 1.6),       # 20 angle grinder
        key(75, PI, 12, 1.0, 2, 16, 0, 0.25),        # 21 dropped pipe (BOOM)
        key(520, BE, 3, 1.5, 20, 30, 0, 0.004),      # 22 bent bell (WIDE)
        key(950, SH, 6, 0.4, 30, 12, 16, 0.2),       # 23 aliased rattle
    ]),
    "metal extreme": ("lo-fi", [
        key(40, B, 30, 0.6, 0, 16, 0, 0.2),          # 0 scream kick (SCREAM)
        key(50, SH, 20, 0.5, 2, 12, 14, 0.15),       # 1 crushed square kick
        key(36, PL, 26, 0.9, 1, 16, 0, 0.25),        # 2 ring-mod sub boom
        key(105, PI, 22, 0.6, 8, 16, 0, 0.08),       # 3 ring tom low
        key(200, SH, 28, 0.3, 27, 16, 6, 0.15),      # 4 scream snare
        key(150, PI, 22, 0.5, 9, 16, 0, 0.08),       # 5 ring tom mid
        key(240, PL, 18, 0.25, 26, 24, 20, 0.12),    # 6 aliased snare
        key(215, PI, 22, 0.45, 10, 16, 0, 0.08),     # 7 ring tom high
        key(1400, B, 24, 0.1, 22, 18, 12, 0.01),     # 8 rim: zap tick
        key(600, SH, 20, 0.2, 31, 16, 8, 0.12),      # 9 clap: shredded rattle
        key(2000, SH, 20, 0.05, 23, 22, 4, 0.015),   # 10 closed hat: buzz
        key(2100, SH, 20, 0.03, 23, 22, 4, 0.008),   # 11 pedal
        key(2000, SH, 20, 0.16, 23, 22, 4, 0.04),    # 12 half-open
        key(2000, SH, 20, 0.7, 23, 22, 4, 0.08),     # 13 open hat: buzz
        key(300, SH, 28, 3.5, 25, 10, 6, 0.5),       # 14 noise crash
        key(140, PL, 31, 6.0, 7, 4, 0, 0.3),         # 15 chaos gong
        key(800, B, 26, 1.2, 20, 16, 0, 0.005),      # 16 screaming anvil
        key(60, PL, 32, 2.0, 28, 14, 0, 2.0),        # 17 grinder storm
        key(250, SH, 24, 0.7, 32, 14, 16, 1.2),      # 18 chain shredder
        key(420, BE, 30, 2.0, 16, 26, 0, 0.01),      # 19 hell bell
        key(90, PI, 16, 1.0, 0, 16, 22, 0.35),       # 20 aliased drop (BOOM + ALIAS)
        key(1200, PL, 29, 0.4, 24, 30, 10, 0.05),    # 21 feedback zap
        key(55, SH, 32, 0.8, 30, 8, 26, 0.6),        # 22 broken machine
        key(2900, B, 18, 0.3, 22, 16, 26, 0.004),    # 23 aliased ping
    ]),
}


def tables_h():
    L, A, AL = mat_tables()
    exp2 = [int(round(32768 * 2 ** (i / 64))) for i in range(65)]
    exp2n = [int(round((1 << 30) * 2 ** (-i / 256))) for i in range(257)]       # 2^-(i/256), Q30
    ring = []                               # DECAY >> 7: the lowest mode's per-block attenuation, log2 units Q20
    exdec = []                              # NDECAY >> 7: per-block factor Q30
    for i in range(256):
        t = DECAY[0] * (DECAY[1] / DECAY[0]) ** (i / 255)
        ring.append(int(round(-math.log2(per_block_t60(t)) * (1 << 20))))
        t = NDECAY[0] * (NDECAY[1] / NDECAY[0]) ** (i / 255)
        exdec.append(int(round(per_block_t60(t) * (1 << 30))))
    ms = [[int(round(10000 * lo * (hi / lo) ** (i / 255))) for i in range(256)] for lo, hi in (DECAY, NDECAY)]
    sp = [spread_point(min(i * 256, 32767)) for i in range(129)]
    sp_rows = [[int(round(s * 4096)), q15(d)] for s, d in sp]
    det_q = [int(round(c / 1200 * OCT)) for c in DETUNE]
    st, dr, gr = strike_rows(), drive_rows(LV), grit_rows()
    assert all(0 <= v <= 32767 for r in st for j, v in enumerate(r) if j != 1), "strike columns"
    assert all(-32768 <= r[1] <= 32767 for r in st), "strike's corner (K0 / 2)"
    assert all(0 <= v <= 65535 for r in dr for v in r), "drive columns"
    h = ["/* Generated by gen_metal.py; do not edit. Tables for metal.c and its screen (ui_metal.h). */",
         "#ifndef METAL_TABLES_H", "#define METAL_TABLES_H", "#include <stdint.h>", "",
         f"#define MT_NMODES {NMODES}", f"#define MT_OCT {OCT}",
         "/* 2^(i / 64), Q15 (65536 = 2.0); 2^-(i / 256), Q30 */", c_array("mt_exp2", "uint32_t", exp2),
         c_array("mt_exp2n", "int32_t", exp2n, 8),
         "/* MATERIAL: per material (BAR PLATE BELL PIPE SHEET) the 8 modes' log2 ratio (K0 units, 6144 per octave), level\n"
         " * Q15, and the decay slope alpha Q12 (a mode's T60 = DECAY x ratio^-alpha) */",
         "static const int16_t mt_mat_l[5][8] __attribute__((unused)) = {"]
    h += ["    { " + ", ".join(map(str, r)) + " }," for r in L] + ["};"]
    h += ["static const int16_t mt_mat_a[5][8] __attribute__((unused)) = {"]
    h += ["    { " + ", ".join(map(str, r)) + " }," for r in A] + ["};"]
    h += [c_array("mt_mat_alpha", "int16_t", AL),
          f"static const char *const mt_mat_name[5] __attribute__((unused)) = {{ {', '.join(chr(34) + m + chr(34) for m in MATERIALS)} }};",
          "/* SPREAD >> 8 (129 points): stretch of the log ratios Q12, detune amount Q15; detune per mode (K0 units at 1.0) */",
          c_array("mt_spread", "uint16_t", [v for r in sp_rows for v in r], 12),
          c_array("mt_detune", "int16_t", det_q),
          f"#define MT_MODE_FADE {k0_of(MODE_FADE)}", f"#define MT_MODE_TOP {k0_of(MODE_TOP)}",
          f"#define MT_OP_RATIO {int(round(OCT * math.log2(OP_RATIO)))}", f"#define MT_MODE_AMP {MODE_AMP}",
          "/* DECAY >> 7: the lowest mode's attenuation per block, log2 units Q20; NDECAY >> 7: per-block factor Q30 */",
          c_array("mt_ring_rate", "int32_t", ring, 8), c_array("mt_ex_dec", "int32_t", exdec, 8),
          "/* times for the screen, 0.1 ms units: [0] DECAY T60, [1] NDECAY T60 */",
          "static const uint32_t mt_time_10us[2][256] __attribute__((unused)) = {"]
    for row in ms:
        h += ["  {"] + ["    " + ", ".join(str(v) for v in row[i:i + 16]) + "," for i in range(0, 256, 16)] + ["  },"]
    h += ["};",
          "/* the screen's graph: dB per block (Q16) by knob >> 7 for [0] DECAY (the lowest mode), [1] NDECAY */",
          "static const int32_t mt_db_blk[2][256] __attribute__((unused)) = {"]
    for lo, hi in (DECAY, NDECAY):
        row = [int(round(60 * BLOCK / (lo * (hi / lo) ** (i / 255) * FS) * 65536)) for i in range(256)]
        h += ["  {"] + ["    " + ", ".join(str(v) for v in row[i:i + 12]) + "," for i in range(0, 256, 12)] + ["  },"]
    h += ["};",
          "/* STRIKE >> 8 (129 points): " + ", ".join(STRIKE_TCOLS) + ":\n"
          " * bend (K0 units at the start, decaying with NDECAY), the brightness corner (K0 units / 2), the roll-off above it\n"
          " * (octaves of level per octave, Q12), attack (samples), swell (x NDECAY's T60, Q15), the exciter's gain (Q12, its\n"
          " * noise normalised), its one-pole coefficient Q15, resonant noise mix Q15, its one-pole Q15, its gain (Q9: RMS 0.5),\n"
          " * hold Q15, chop Q15 */",
          f"#define MT_STRIKE_W {len(STRIKE_TCOLS)}",
          c_array("mt_strike", "int16_t", [v for r in st for v in r], len(STRIKE_TCOLS)),
          "/* DRIVE >> 8 (129 points): " + ", ".join(DRIVE_COLS) + ":\n"
          " * fold mix Q15, fold gain Q11, square blend Q15, clip gain Q10, ring mix Q15, operator feedback beta Q8, the\n"
          " * operator's FM of the modes Q15, level Q12 */",
          f"#define MT_DRIVE_W {len(DRIVE_COLS)}",
          c_array("mt_drive", "uint16_t", [v for r in dr for v in r], len(DRIVE_COLS)),
          "/* GRIT >> 8 (129 points): bits Q8 (16.0 = off), the sample-and-hold increment (2^32 - 1 = every sample) */",
          c_array("mt_grit_bits", "uint16_t", [r[0] for r in gr]),
          c_array("mt_grit_hold", "uint32_t", [r[1] for r in gr], 8),
          "/* 2^15 sqrt(2^14 / m), m = 2^14 + 1024 i (i = 0 .. 48), Q15 (the strike's normalisation) */",
          c_array("mt_rsqrt", "int32_t", [min(32767, int(round(32768 * math.sqrt(16384 / (16384 + 1024 * i))))) for i in range(49)]),
          "/* 2^22 / m for m = 1024 + 16 i (i = 0 .. 64): the attack's steps (metal.c recip_q22) */",
          c_array("mt_recip", "int32_t", [int(round(2 ** 22 / (1024 + 16 * i))) for i in range(65)]),
          "/* runs of n samples: 65536 / n for n = 16 .. 128 (n / 16 - 1) */",
          c_array("mt_recip16", "uint32_t", [65536 // n for n in range(16, 129, 16)]),
          "/* the screen's objects by material: an outline (24 points, closed) and two details (8 each), x y px */",
          "static const int8_t mt_shape[5][40][2] __attribute__((unused)) = {"]
    shp = shapes()
    for m in MATERIALS:
        o, a_, b_ = shp[m]
        h += ["  { " + ", ".join(f"{{ {x}, {y} }}" for x, y in o + a_ + b_) + " },"]
    h += ["};",
          "/* the fold's and the operator's sine: sin(2 pi i / 1024), Q15, no interpolation (a drive stage) */",
          c_array("mt_sin1k", "int16_t", [int(round(32767 * math.sin(2 * math.pi * i / 1024))) for i in range(1024)], 16),
          "#endif", ""]
    return "\n".join(h)


def main():
    open(os.path.join(HERE, "metal_tables.h"), "w").write(tables_h())
    kits = {}
    for name, (template, keys) in KITS.items():
        assert len(keys) == 24, name
        kits[name] = {"template": template, "dbox_data": keys}
    lines = ["{"]
    for j, (name, kit) in enumerate(kits.items()):
        rows = ",\n".join("    " + json.dumps(r) for r in kit["dbox_data"])
        lines.append(f' "{name}": {{"template": "{kit["template"]}", "dbox_data": [\n{rows}\n ]}}'
                     + ("," if j < len(kits) - 1 else ""))
    open(os.path.join(HERE, "metal_kits.json"), "w").write("\n".join(lines + ["}", ""]))
    assert json.load(open(os.path.join(HERE, "metal_kits.json"))) == kits
    print("wrote metal_tables.h, metal_kits.json:", ", ".join(kits))


if __name__ == "__main__":
    main()
