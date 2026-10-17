#!/usr/bin/env python3
"""Generate the fmdrum engine's tables (src/slot/fmdrum_tables.h) and factory kits (src/slot/fmdrum_kits.json).

  python3 src/slot/gen_fmdrum.py

Both outputs are committed; rerun after changing a curve or a kit below. Engine: src/slot/fmdrum.c; design and knob
mapping: notes/drum-engine.md "fmdrum". The knob curves live here once: the engine reads the tables, the screen
(ui_fmdrum.h) shows the same values, src/engine/test/fmdrum_check.py imports this module to check both.

Per key, 8 int16 (TE's dbox per-key values, range row 9: K0 -24320..24832, K1-K7 0..32767). An amount on each plain
knob, its time (or its partner) on SHIFT:
  K0 blue    PITCH   512 per semitone, 0 = A3 (220 Hz): 14 Hz .. 3.6 kHz (dbox's own convention)
  K1 green   SWEEP   pitch sweep from above, 0 .. +48 semitones, quadratic (half = +12)
  K2 white   FM      peak index 0 .. 16 rad over detents 0..19, quadratic (detent 10 = 4 rad); FEEDBACK 0..7 at
                     detents 20..27 (the modulator fed back on itself, grit -> noise); SQUARE 1..5 at 28..32 (the
                     carrier clipped into a square)
  K3 orange  DECAY   amplitude (tone and noise), 15 ms .. 4 s to -60 dB, exponential
  K4 SHIFT+blue    RATIO   modulator : carrier, 32 steps: the 15 harmonic ratios (n / 2, TONE) rising, then the 17
                           inharmonic ones (METAL) rising
  K5 SHIFT+green   SWDEC   sweep time constant, 1 .. 300 ms, exponential
  K6 SHIFT+white   FMDEC   the FM index's own decay, 3 ms .. 3 s to -60 dB, exponential
  K7 SHIFT+orange  NOISE   one knob through the noise families: OFF (detent 0), AIR 1-4 (dark, under the tone),
                           SNARE 5-9 (band, with the tone), HAT 10-14 (bright, the tone fades out), CLAP 15-19 (mid
                           band, three bursts, noise only), METAL 20-25 (the 808's six squares, band-passed, ring-
                           modulated with the voice), GRIND 26-32 (ring mod with noise and a jittery square); stock
                           dbox values stop at 32255, GRIND
The first four kits are written in the old scales (DRUMFM1 .. FX4) and remapped (key()); the fifth with key2().
Keys 10..13 are a hi-hat choke group (fmdrum.c): a hit there on a key in the HAT or METAL zone releases the group's
other HAT / METAL voices in ~5 ms. The kits share key roles: 0-2 kicks, 3/5/7 toms, 4/6 snares, 8 rim, 9 clap, 10-13 hats, 14-15
cymbals, 16-23 percussion and effects.
"""
import json
import math
import re
import os

import numpy as np

FS = 88200.0
BLOCK = 128
HERE = os.path.dirname(os.path.abspath(__file__))

# ---- curves ----
A3 = 220.0
SEMI = 512                                 # K0 units per semitone
K0_MIN, K0_MAX = -24320, 24832             # dbox's range row 9 (device, DRUMPROBE)
STOCK_LO = [-24320, 0, 0, 1296, 0, 0, 0, 0]          # the stock dbox presets' per-column range: factory kits stay inside
STOCK_HI = [24638, 32767, 32767, 32767, 32767, 32767, 32767, 32255]
HARMONIC = [0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 6.0, 7.0, 8.0, 11.0, 13.0, 15.5]   # n / 2: pitched (TONE)
INHARMONIC = [0.7071, 1.1892, 1.4142, 1.7321, 2.2361, 2.4142, 2.7565, 3.1416, 3.7321, 4.2426, 4.6, 5.1926, 5.4317,
              6.2832, 7.1268, 8.6603, 9.4248]                                            # METAL
RATIOS = HARMONIC + INHARMONIC
FM_MAX = 16.0                              # rad at the top of the musical zone (K2 = FM_FB0)
FM_FB0, FM_SQ0 = 20 * 1024, 27 * 1024      # K2: the musical index up to detent 20 (16 rad), FEEDBACK 1..7 at detents
#                                            21..27 (DX7 levels, one per detent), SQUARE 1..5 at 28..32 (the carrier
#                                            clips into a square); zones are labelled above these values (k > FM_FB0)
SQ_RAMP0 = FM_SQ0                          # the clip starts right after FEEDBACK 7
FB_BETA = [0.0, 1.0, 1.9, 2.7, 3.3, 3.9, 4.6, 6.0]   # feedback level -> beta (rad of the modulator's own phase,
#                                            applied to the mean of its last two outputs, as the DX7): grit .. noise
SQ_FB = 4.3                                # SQUARE: the feedback falls back from 7.0 to this (pitched, gritty) ..
SQ_GAIN = 32.0                             # .. while the carrier is driven into a clip, 1 .. 32 x: a square at the top
SWEEP_MAX = 48                             # semitones at K4 = 32767
DECAY = (0.015, 4.0)                       # K3: T60 range, s
SNAP = (0.001, 0.3)                        # K5: time constant range, s
MODDEC = (0.003, 3.0)                      # K7: T60 range, s
NOISE_ZONES = ["OFF", "AIR", "SNARE", "HAT", "CLAP", "METAL", "GRIND"]
NOISE_EDGES = [0, 1024, 5120, 10240, 15360, 20480, 26624, 32768]   # K7 zone starts: detents 0, 1, 5, 10, 15, 20, 26
METAL_RATIOS = [1.0, 1.4827, 1.8003, 2.5460, 2.6303, 3.8968]   # the 808's six squares (205.3 .. 800 Hz) / the lowest
METAL_TOP = 1600.0                         # Hz: the cluster's lowest square follows PITCH / 2 up to here (top 6.2 kHz)
METAL_LOW = 70.0                          # .. and down to here (the 808's own: 205.3 .. 800 Hz)
GRIND_LOW = 80.0                           # Hz: GRIND's square starts at max(this, PITCH / 2), rises to PITCH x sqrt 2
NOISE_RMS = 0.16                           # filtered noise RMS below 20 kHz at full level (Q15 full scale = 1)
TONE = 0.62                                # tone level at full velocity
CLAP_BURSTS = [0, 6, 12, 18]               # burst starts, blocks (8.7 ms apart); from the last one the main tail
CLAP_TAU = 0.0025                          # each burst's decay, s


def pitch_hz(k0):
    return A3 * 2 ** (k0 / (12 * SEMI))


def k0_of(hz):
    return int(round(12 * SEMI * math.log2(hz / A3)))


def ratio_of(k1):
    return RATIOS[max(k1, 0) >> 10]


def k1_of(ratio):
    i = min(range(32), key=lambda j: abs(math.log(RATIOS[j] / ratio)))
    return i * 1024 + 512


def index_q8(k2):
    """the engine's peak index, rad Q8 (fmdrum.h fd_index_q8): 16 rad x (k / 20480)^2, then 16 rad"""
    k = min(max(k2, 0), FM_FB0)
    return (((k * k) >> 15) * 10486) >> 15


def fm_rad(k2):
    return index_q8(k2) / 256.0


def k2_of(rad):
    """the smallest K2 whose index reaches `rad` (musical zone)"""
    want = int(round(rad * 256)) if rad < FM_MAX else 4096
    lo, hi = 0, FM_FB0
    while lo < hi:
        mid = (lo + hi) // 2
        if index_q8(mid) >= want:
            hi = mid
        else:
            lo = mid + 1
    return lo


def fm_zone(k2):
    k = max(k2, 0)
    return 0 if k <= FM_FB0 else 1 if k <= FM_SQ0 else 2


def fm_level(k2):
    """the readout's FEEDBACK 1..7 / SQUARE 1..5 (rounded up: a value past a detent shows the next level)"""
    k = max(k2, 0)
    z = fm_zone(k)
    return 0 if z == 0 else (k - FM_FB0 + 1023) >> 10 if z == 1 else (k - FM_SQ0 + 1023) >> 10


def k2_fb(level):
    """K2 for FEEDBACK level 0..7 (a detent each)"""
    return FM_FB0 + int(round(level * 1024))


def k2_sq(amount):
    """K2 for SQUARE amount 0..1 (0 = FEEDBACK 7, a sine carrier; 0.2 .. 1 = SQUARE 1 .. 5, detents 28 .. 32)"""
    return min(32767, SQ_RAMP0 + int(round(amount * (32767 - SQ_RAMP0))))


# Old kits (DRUMFM1..3 / FX4) stored FM as 16 rad x (k / 32767)^2 and NOISE with OFF < 1024, then AIR, SNARE, HAT,
# CLAP 8192 wide each from 1024 / 8192 / 16384 / 24576. Their values are remapped to the new scales here, so they
# sound the same: FM to the K2 whose engine index is the same Q8 value (exact), NOISE zone by zone (linear).
def old_index_q8(k2):
    k = max(k2, 0)
    return (((k * k) >> 15) * 4096) >> 15


def old_k2_of(rad):
    return min(32767, int(round(math.sqrt(rad / FM_MAX) * 32767)))


def remap_fm(k_old):
    want = old_index_q8(k_old)
    k = k2_of(want / 256.0)
    assert index_q8(k) == want, (k_old, k)
    return k


OLD_NOISE_EDGES = [0, 1024, 8192, 16384, 24576, 32768]


def remap_noise(k_old):
    """the K7 whose sound (the curve at K7 + NOISE_SHIFT) is the old value's"""
    k = max(k_old, 0)
    for z in range(5):
        a, b = OLD_NOISE_EDGES[z], OLD_NOISE_EDGES[z + 1]
        if k < b or z == 4:
            na, nb = NOISE_EDGES[z], NOISE_EDGES[z + 1]
            if z == 0:
                return 0
            t = min(1.0, (k - a) / (8191 if z == 4 else b - a))
            return knob_k7(int(round(na + t * (nb - na))))


def expo(k, lo, hi):
    """value of a 256-step exponential table at knob k (index k >> 7)"""
    return lo * (hi / lo) ** ((max(k, 0) >> 7) / 255.0)


def k_expo(v, lo, hi):
    i = int(round(255 * math.log(v / lo) / math.log(hi / lo)))
    return min(255, max(0, i)) * 128 + 64


def sweep_semis(k4):
    k = max(k4, 0)
    return (((k * k) >> 15) * (SWEEP_MAX * SEMI) >> 15) / SEMI


def k4_of(semis):
    return min(32767, int(round(math.sqrt(semis / SWEEP_MAX) * 32767)))


NOISE_SHIFT = 1023                         # K7 plays the curve one detent ahead: a zone's first detent is already
#                                            1 / n into it and its last detent is the zone at full (fmdrum.c)


def sound_k7(k7):
    """the curve point a K7 value plays (fmdrum.h fd_noise_sound): one detent ahead, the shift taken back across GRIND
    so the knob's last detent is GRIND at full and the one before it differs"""
    k = max(k7, 0)
    return min(32767, k + NOISE_SHIFT - (((k - 26624) * 10914) >> 16 if k > 26624 else 0))


def knob_k7(c):
    """the smallest K7 that plays curve point c or above"""
    lo, hi = 0, 32767
    while lo < hi:
        mid = (lo + hi) // 2
        if sound_k7(mid) >= c:
            hi = mid
        else:
            lo = mid + 1
    return lo


def curve_zone(c):
    c = max(c, 0)
    return max(z for z in range(7) if c >= NOISE_EDGES[z])


def noise_zone(k7):
    """the zone a K7 value shows (its sound's zone): on detents, zone z from NOISE_EDGES[z]"""
    return curve_zone(sound_k7(k7))


def k7_of(zone, t):
    """K7 whose sound is at position t (0 .. 1) of a zone (an index into NOISE_ZONES); t >= 1 / n stays in the zone"""
    a, b = NOISE_EDGES[zone], NOISE_EDGES[zone + 1]
    return min(STOCK_HI[7], knob_k7(int(round(a + t * (b - 1 - a)))))


# ---- K7: the noise path, 129 points (k7 = i * 256), interpolated per block ----
def old_noise_point(k):
    """the old scale's (DRUMFM1..FX4) OFF .. CLAP: (tone, level, fc Hz, damping q, lp/bp/hp blend, clap amount)"""
    def geo(a, b, t):
        return a * (b / a) ** t
    if k < 1024:
        return 1.0, 0.0, 300.0, 1.0, (1.0, 0.0, 0.0), 0.0
    if k < 8192:                            # AIR: dark noise creeping in under the tone
        t = (k - 1024) / 7168
        return 1.0, 0.55 * t ** 0.8, geo(300, 1500, t), 1.0, (1 - t, t, 0.0), 0.0
    if k < 16384:                           # SNARE: a band rising with the knob, tone kept
        t = (k - 8192) / 8192
        return 1.0, 0.55 + 0.45 * t, geo(1500, 5000, t), 1.0, (0.0, 1 - 0.5 * t, 0.5 * t), 0.0
    if k < 24576:                           # HAT: bright, the tone fades out
        t = (k - 16384) / 8192
        return 1.0 - t, 1.0, geo(5000, 9000, t), 1.0, (0.0, 0.5 - 0.5 * t, 0.5 + 0.5 * t), 0.0
    t = min(1.0, (k - 24576) / 8191)        # CLAP: noise only, back down to a narrower mid band, bursts in
    return 0.0, 1.0, geo(9000, 1200, t), 1.0 - 0.4 * t, (0.0, t, 1 - t), t


METAL_FC = (1200.0, 11000.0)               # METAL's band: from CLAP's last band up to a hat's
METAL_HP = 3000.0                          # the squares' one-pole high-pass before the band, over METAL's top 40 %
#                                            (an 808 hat: band-pass then high-pass; the band-pass keeps the edges soft)
METAL_Q = 0.5                              # damping at METAL's top (CLAP ends at 0.6)
METAL_TONE = 0.45                          # the FM voice (the body) early in METAL: 0 at CLAP's end, up by t = 0.2 ..
METAL_TONE_TOP = 0.0                       # .. then down to this at the top (a hat)
METAL_LEVEL = 1.0                          # the band-passed cluster
METAL_RING = 0.3                           # METAL's ring-mod (1.0 = x2), reached by t = 0.3
GRIND_TONE, GRIND_LEVEL, GRIND_RING = 0.35, 0.55, 0.3   # GRIND's top: the dry voice, the direct noise/square, the ring
GRIND_FC = 150.0                           # GRIND's filter falls to a high-pass here (no sub from the square)


def noise_point(k):
    """the path at K7 = k: dict of tone, level, fc, q, blend (lp, bp, hp), clap, and the source weights wx (white
    noise), wm (METAL's six squares), wsq (GRIND's square), ring (source x voice, 1.0 = x2), gu (GRIND's square pitch,
    0 = GRIND_LOW .. 1 = PITCH x sqrt 2)"""
    def geo(a, b, t):
        return a * (b / a) ** t
    z = curve_zone(k)
    d = dict(wx=1.0, wm=0.0, wsq=0.0, ring=0.0, gu=0.0, mhp=0.0)
    if z <= 4:                              # OFF .. CLAP: the old curves, each zone squeezed into its new width
        if z == 0:
            ko = k
        else:
            t = (k - NOISE_EDGES[z]) / (NOISE_EDGES[z + 1] - NOISE_EDGES[z])
            ko = OLD_NOISE_EDGES[z] + t * (8191 if z == 4 else OLD_NOISE_EDGES[z + 1] - OLD_NOISE_EDGES[z])
        tone, level, fc, q, blend, clap = old_noise_point(ko)
        d.update(tone=tone, level=level, fc=fc, q=q, blend=blend, clap=clap)
        return d
    t = (k - NOISE_EDGES[z]) / (NOISE_EDGES[z + 1] - NOISE_EDGES[z])
    if z == 5:                              # METAL: the 808's six squares replace the noise, band-passed (high-passed
        u1 = u2 = min(1.0, t / 0.5)         # over the top 40 %, as an 808 hat), ringing with the voice
        hp = max(0.0, (t - 0.6) / 0.4)
        tone = METAL_TONE * u1 if t < 0.5 else METAL_TONE + (METAL_TONE_TOP - METAL_TONE) * (t - 0.5) / 0.5
        d.update(tone=tone, level=1.0 + (METAL_LEVEL - 1.0) * t, fc=geo(*METAL_FC, t),
                 q=0.6 + (METAL_Q - 0.6) * t, blend=(0.0, 1.0, 0.0), clap=1.0 - u1, mhp=METAL_HP * hp,
                 wx=math.cos(u1 * math.pi / 2), wm=math.sin(u1 * math.pi / 2), ring=METAL_RING * u2)
        return d
    v1, v2 = min(1.0, t / 0.3), max(0.0, (t - 0.2) / 0.8)   # GRIND: back to noise, the band falls to a high-pass at
    q = METAL_Q                             # 250 Hz (no sub), then a square comes in
    d.update(tone=METAL_TONE_TOP + (GRIND_TONE - METAL_TONE_TOP) * t,
             level=METAL_LEVEL + (GRIND_LEVEL - METAL_LEVEL) * t,
             fc=geo(METAL_FC[1], GRIND_FC, v1), q=q, blend=(0.0, 1.0 - 0.7 * v1, v1), clap=0.0, mhp=METAL_HP * (1 - v1),
             wx=math.sin(v1 * math.pi / 2) * math.cos(v2 * math.pi / 2 * 0.7), wm=math.cos(v1 * math.pi / 2),
             wsq=math.sin(v2 * math.pi / 2),
             ring=METAL_RING + (GRIND_RING - METAL_RING) * v1, gu=v2, boost=1.0 + 0.4 * max(0.0, v1 - 0.6) / 0.4 * (1 - v2))
    return d


def svf_run(f_q15, q_q15, w, x):
    """the engine's integer Chamberlin SVF (fmdrum.c), outputs blended with Q12 weights; returns y and max |acc|"""
    lp = bp = 0
    wl, wb, wh = w
    y = np.zeros(len(x), dtype=np.int64)
    amax = 0
    for i, xi in enumerate(x):
        lp += (f_q15 * bp) >> 15
        hp = int(xi) - lp - ((q_q15 * bp) >> 15)
        bp += (f_q15 * hp) >> 15
        acc = wl * lp + wb * bp + wh * hp
        amax = max(amax, abs(acc))
        y[i] = acc >> 12
    return y, amax


NOISE_COLS = ["tone", "level", "f", "q", "wl", "wb", "wh", "clap", "wx", "wm", "wsq", "ring", "gu", "mhp"]
NOISE_N = 129


def noise_table():
    seed, xs = 12345, []
    for _ in range(1 << 14):                # the engine's LCG, top bits >> 17: +-16384
        seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
        xs.append((seed - (1 << 32) if seed >> 31 else seed) >> 17)
    x = np.array(xs)
    sqs = [np.where((np.arange(1 << 14) // h) % 2, 49151, -49151) for h in (3, 10, 40, 150)]   # SVF headroom
    rows = []
    for i in range(NOISE_N):
        k = min(i * 256, 32767)
        d = noise_point(k)
        f = int(round(2 * math.sin(math.pi * d["fc"] / FS) * 32768))
        qq = int(round(d["q"] * 32768))
        # weights: blend x norm (Q12), so the output RMS is NOISE_RMS x 32768 at level 1 for white noise
        w0 = [int(round(b * 4096)) for b in d["blend"]]
        y, _ = svf_run(f, qq, w0, x)
        spec = np.fft.rfft(y[2048:].astype(float))          # RMS of what is heard: TE's 2:1 decimation keeps < 22 kHz
        spec[np.fft.rfftfreq(len(y) - 2048, 1 / FS) > 20000] = 0
        rms = float(np.sqrt(np.mean(np.fft.irfft(spec, len(y) - 2048) ** 2))) or 1.0
        norm = NOISE_RMS * 32768 / rms
        w = [int(round(b * 4096 * norm * d.get("boost", 1.0))) for b in d["blend"]]   # (GRIND: the high-passed noise up)
        for src in [x] + (sqs if d["wm"] or d["wsq"] else []):   # squares only reach METAL / GRIND
            y, amax = svf_run(f, qq, w, src)
            assert amax < 1 << 30, (i, amax)    # int32 headroom in the engine's blend
        assert max(w) < 32768, (i, w)
        q15 = lambda v: int(round(min(1.0, max(0.0, v)) * 32767))   # noqa: E731
        rows.append((q15(d["tone"]), q15(d["level"]), f, qq, w[0], w[1], w[2], q15(d["clap"]), q15(d["wx"]),
                     q15(d["wm"]), q15(d["wsq"]), q15(d["ring"]), q15(d["gu"]),
                     int(round(32767 * (1 - math.exp(-2 * math.pi * max(d["mhp"], 40.0) / FS))))))
    return rows


METAL_OCT0 = 25.0                          # the gain table's lowest cluster pitch (its lowest square), Hz
METAL_OCTS = 7                             # 25 Hz .. 1.6 kHz in octaves (8 points)
METAL_BOOST = 1.15                         # METAL / GRIND's squares above noise in the same band (+1.2 dB: hats cut)
METAL_TN = 25                              # points across METAL and GRIND, 512 apart (GRIND fades the squares out)


def metal_gain_table():
    """[octave of the cluster's lowest square][(K7 - METAL's start) / 512] -> gain Q12 that makes the band-passed six squares as
    loud as white noise through the same band (the squares' harmonics thin out above them: up to ~20 dB quieter)"""
    n = 1 << 13
    seed, xs = 777, []
    for _ in range(n):
        seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
        xs.append((seed - (1 << 32) if seed >> 31 else seed) >> 17)
    white = np.array(xs, dtype=np.int64)
    tbl = []
    for o in range(METAL_OCTS + 1):
        f0 = METAL_OCT0 * 2 ** o
        incs = [int(f0 * r / FS * 2 ** 32) for r in METAL_RATIOS]
        ph = [(0x2545F491 * (2 * j + 1)) & 0xFFFFFFFF for j in range(6)]
        m = np.zeros(n, dtype=np.int64)
        for j in range(6):
            p_ = (ph[j] + incs[j] * np.arange(1, n + 1, dtype=np.int64)) & 0xFFFFFFFF
            m += np.where(p_ >= 1 << 31, -3861, 3861)
        row = []
        for i in range(METAL_TN):
            d = noise_point(min(32767, NOISE_EDGES[5] + i * 512))
            f = int(round(2 * math.sin(math.pi * d["fc"] / FS) * 32768))
            qq = int(round(d["q"] * 32768))
            w = [int(round(b * 4096)) for b in d["blend"]]
            yw, _ = svf_run(f, qq, w, white)
            ym, _ = svf_run(f, qq, w, m)
            g = math.sqrt(np.mean(yw[1024:].astype(float) ** 2) / max(np.mean(ym[1024:].astype(float) ** 2), 1e-9))
            row.append(int(round(min(8.0, max(0.25, g * METAL_BOOST)) * 4096)))
        tbl.append(row)
    return tbl


def fm_table():
    """K2 from FM_FB0, (k - FM_FB0) >> 7: (feedback beta rad Q8, carrier clip gain Q10, tone level Q15, index scale Q12:
    FEEDBACK x (1 + beta / 4), so the early levels add bite; SQUARE keeps FEEDBACK 7's)"""
    th = np.linspace(0, 2 * math.pi, 4096, endpoint=False)
    rows = []
    for i in range((32767 - FM_FB0 >> 7) + 1):
        k = FM_FB0 + i * 128
        if k < SQ_RAMP0:
            lv = (k - FM_FB0) / 1024
            j = min(int(lv), 6)
            a, b = FB_BETA[j], FB_BETA[j + 1]
            f = lv - j
            beta = a + (b - a) * f if j == 0 else a * (b / a) ** f
            g = 1.0
            isc = 1 + beta / 4
        else:
            a = (k - SQ_RAMP0) / (32767 - SQ_RAMP0)
            beta = FB_BETA[7] * (SQ_FB / FB_BETA[7]) ** a
            g = SQ_GAIN ** a
            isc = 1 + FB_BETA[7] / 4            # FEEDBACK 7's index throughout
        y = np.clip(g * np.sin(th), -1, 1)
        comp = math.sqrt(0.5) / math.sqrt(np.mean(y ** 2))        # the square as loud (RMS) as the sine
        rows.append((int(round(beta * 256)), int(round(g * 1024)), int(round(comp * 32767)), int(round(isc * 4096))))
    return rows


def clap_table():
    out = []
    for b in range(CLAP_BURSTS[-1] + 1):    # value at the end of block b (age b + 1)
        t = (b + 1) * BLOCK / FS
        if b >= CLAP_BURSTS[-1]:
            out.append(32767)
            continue
        tk = max(s * BLOCK / FS for s in CLAP_BURSTS if s <= b)
        out.append(int(round(32767 * math.exp(-(t - tk) / CLAP_TAU))))
    return out


def per_block_t60(t60):
    return 10 ** (-3 * BLOCK / (t60 * FS))


def per_block_tau(tau):
    return math.exp(-BLOCK / (tau * FS))


def tables():
    exp2 = [int(round(32768 * 2 ** (i / 64))) for i in range(65)]
    ratio = [int(round(r * 65536)) for r in RATIOS]
    amp = [int(round(per_block_t60(DECAY[0] * (DECAY[1] / DECAY[0]) ** (i / 255)) * (1 << 30))) for i in range(256)]
    mod = [int(round(per_block_t60(MODDEC[0] * (MODDEC[1] / MODDEC[0]) ** (i / 255)) * (1 << 30))) for i in range(256)]
    swp = [int(round(per_block_tau(SNAP[0] * (SNAP[1] / SNAP[0]) ** (i / 255)) * (1 << 30))) for i in range(256)]
    return exp2, ratio, amp, mod, swp


def sine_pairs():
    """sine256 (src/engine/sine_table.h) as (next - this) << 16 | this, one word per step (fmdrum.c's fsin in asm)"""
    t = open(os.path.join(HERE, "..", "engine", "sine_table.h")).read()
    v = [int(x) for x in re.findall(r"-?\d+", t[t.index("{"):t.index("}")])]
    assert len(v) == 257
    return [f"0x{((v[i + 1] - v[i]) & 0xFFFF) << 16 | (v[i] & 0xFFFF):08x}" for i in range(256)]


def c_array(name, ctype, vals, per=12):
    s = f"static const {ctype} {name}[{len(vals)}] __attribute__((unused)) = {{\n"
    for i in range(0, len(vals), per):
        s += "    " + ", ".join(str(v) for v in vals[i:i + per]) + ",\n"
    return s + "};\n"


# ---- factory kits: per key (pitch Hz, ratio, FM, decay s, sweep semitones, sweep time s, NOISE, FM decay s) ----
# Knob order (fmdrum.h): PITCH, SWEEP, FM, DECAY, RATIO, SWDEC, FMDEC, NOISE.
def _clamp(k):
    for j, v in enumerate(k):              # TE validates factory presets: stay inside what stock dbox kits hold
        k[j] = min(STOCK_HI[j], max(STOCK_LO[j], v))
    return k


def key(hz, ratio=1.0, fm=0.0, decay=0.3, sweep=0.0, snap=0.02, noise=0, mod=0.05):
    """the first four kits, written in the old scales (FM rad on 16 (k / 32767)^2, NOISE on the old zones) as they
    were for DRUMFM1 .. FX4, then remapped to the new scales: the same index and noise path"""
    k = _clamp([k0_of(hz), k4_of(sweep), old_k2_of(fm), k_expo(decay, *DECAY), k1_of(ratio), k_expo(snap, *SNAP),
                k_expo(mod, *MODDEC), noise])
    _OLD.append(list(k))
    k[FM_K], k[NOISE_K] = remap_fm(k[FM_K]), remap_noise(k[NOISE_K])
    return _clamp(k)


def key2(hz, ratio=1.0, fm=0.0, decay=0.3, sweep=0.0, snap=0.02, noise=0, mod=0.05):
    """the new scales: fm = rad (float, musical zone) or a raw K2 (FB(level), SQ(amount)); noise = a raw K7 (NZ())"""
    return _clamp([k0_of(hz), k4_of(sweep), fm if isinstance(fm, int) else k2_of(fm), k_expo(decay, *DECAY),
                   k1_of(ratio), k_expo(snap, *SNAP), k_expo(mod, *MODDEC), noise])


FM_K, NOISE_K = 2, 7
_OLD = []                                  # the old-scale rows key() made, in order (LEGACY below)
FB, SQ = k2_fb, k2_sq


def NZ(zone, t=0.5):
    return k7_of(NOISE_ZONES.index(zone), t)


AIR, SNARE, HAT, CLAP = 4600, 12000, 20480, 32767    # NOISE points inside each zone (HAT 20480: half tone, half noise;
#                                                        CLAP clamps to the stock 32255, still CLAP)
# Shared key roles: 0-2 kicks, 3/5/7 toms, 4/6 snares, 8 rim, 9 clap, 10-13 hats (choke group: HAT zone), 14-15
# cymbals, 16-23 free (percussion, effects).
KITS = {
    # 808-ish: long sine kicks with a gentle sweep, toms, a noisy snare, clap, square-ish metal hats and cowbell
    "fm 808": ("dem drums", [
        key(49, 1.0, 0.3, 1.2, 24, 0.035, 0, 0.02),          # 0 F   kick long
        key(55, 1.0, 0.6, 0.6, 26, 0.025, 0, 0.015),         # 1 F#  kick short
        key(46, 0.5, 1.2, 0.9, 30, 0.03, AIR, 0.01),         # 2 G   kick dirty
        key(90, 1.0, 0.5, 0.6, 8, 0.05, 0, 0.06),            # 3 G#  low tom
        key(185, 1.5, 1.6, 0.28, 6, 0.02, SNARE, 0.03),      # 4 A   snare
        key(110, 1.0, 0.5, 0.55, 8, 0.05, 0, 0.06),          # 5 A#  mid tom
        key(170, 2.0, 1.0, 0.18, 4, 0.015, SNARE + 3000, 0.02),  # 6 B   snare tight
        key(140, 1.0, 0.5, 0.5, 8, 0.05, 0, 0.06),           # 7 C   high tom
        key(1700, 1.0, 1.0, 0.05, 2, 0.01, 0, 0.02),         # 8 C#  rim
        key(150, 1.0, 0.0, 0.35, 0, 0.02, CLAP, 0.02),       # 9 D   clap
        key(3000, 1.4142, 7.0, 0.07, 0, 0.01, HAT, 0.08),    # 10 D# closed hat
        key(3200, 1.4142, 7.0, 0.04, 0, 0.01, HAT + 1000, 0.05),    # 11 E  closed hat tight
        key(3000, 1.4142, 7.0, 0.18, 0, 0.01, HAT, 0.15),    # 12 F  half-open hat
        key(3000, 1.4142, 7.0, 0.7, 0, 0.01, HAT, 0.6),      # 13 F# open hat
        key(3200, 2.7565, 9.0, 1.6, 0, 0.01, HAT + 2000, 1.2),   # 14 G  cymbal
        key(2600, 3.1416, 6.0, 1.3, 0, 0.01, HAT - 2500, 1.3),   # 15 G# ride (was 808's fifth kick)
        key(2500, 1.0, 0.4, 0.06, 0, 0.01, 0, 0.02),         # 16 A  clave
        key(560, 1.5, 1.6, 0.35, 0, 0.01, 0, 0.35),          # 17 A# cowbell
        key(260, 1.0, 0.4, 0.3, 3, 0.01, 0, 0.03),           # 18 B  conga low
        key(370, 1.0, 0.4, 0.25, 3, 0.01, 0, 0.03),          # 19 C  conga high
        key(400, 1.0, 0.0, 0.09, 0, 0.01, 24000, 0.01),      # 20 C# maracas
        key(65, 1.0, 0.4, 0.7, 12, 0.08, 0, 0.05),           # 21 D  floor tom
        key(41, 1.0, 0.2, 2.0, 20, 0.05, 0, 0.02),           # 22 D# sub boom
        key(880, 3.5, 2.5, 1.2, 0, 0.01, 0, 0.6),            # 23 E  ping
    ]),
    # 909-ish: punchier kicks (FM click, faster sweep), brighter snares, metallic hats; TE's punch FX on
    "fm 909": ("lo-fi", [
        key(55, 1.0, 1.8, 0.5, 30, 0.012, 0, 0.012),         # 0 kick
        key(58, 1.0, 2.6, 0.35, 34, 0.009, AIR - 2000, 0.008),   # 1 kick hard
        key(50, 0.5, 2.0, 0.7, 28, 0.015, 0, 0.01),          # 2 kick deep
        key(100, 1.0, 1.0, 0.45, 10, 0.03, AIR - 3000, 0.03),   # 3 low tom
        key(200, 1.5, 2.2, 0.25, 7, 0.012, SNARE + 2500, 0.02),  # 4 snare
        key(130, 1.0, 1.0, 0.4, 10, 0.03, AIR - 3000, 0.03),    # 5 mid tom
        key(230, 2.0, 2.5, 0.2, 7, 0.01, SNARE + 4500, 0.015),  # 6 snare bright
        key(165, 1.0, 1.0, 0.35, 10, 0.03, AIR - 3000, 0.03),   # 7 high tom
        key(1200, 1.4142, 2.5, 0.04, 0, 0.01, SNARE + 2000, 0.02),  # 8 rim
        key(150, 1.0, 0.0, 0.4, 0, 0.02, CLAP, 0.02),        # 9 clap
        key(3300, 2.4142, 6.0, 0.05, 0, 0.01, HAT - 1500, 0.05),    # 10 hat tight
        key(3300, 1.4142, 8.0, 0.09, 0, 0.01, HAT, 0.1),     # 11 closed hat
        key(3300, 2.7565, 10.0, 0.06, 0, 0.01, HAT + 1500, 0.05),   # 12 pedal hat
        key(3300, 1.4142, 8.0, 0.8, 0, 0.01, HAT, 0.8),      # 13 open hat
        key(2800, 3.1416, 11.0, 2.0, 0, 0.01, HAT + 2500, 1.5),     # 14 crash
        key(3000, 2.7565, 9.0, 1.4, 0, 0.01, HAT - 2000, 1.4),      # 15 ride
        key(62, 1.0, 1.5, 0.3, 40, 0.008, 0, 0.01),          # 16 kick short
        key(300, 1.0, 0.0, 0.12, 0, 0.01, 22000, 0.01),      # 17 shaker
        key(45, 1.0, 1.2, 1.6, 24, 0.02, 0, 0.015),          # 18 kick long
        key(180, 1.0, 0.0, 0.3, 0, 0.02, 29000, 0.02),       # 19 clap bright
        key(75, 1.0, 1.0, 0.55, 14, 0.04, AIR - 3000, 0.03),    # 20 floor tom
        key(440, 1.5, 1.4, 0.3, 0, 0.01, 0, 0.3),            # 21 cowbell
        key(200, 1.0, 3.0, 0.2, 24, 0.02, SNARE, 0.02),      # 22 snare zap
        key(800, 1.0, 1.0, 0.12, 12, 0.006, 0, 0.02),        # 23 blip
    ]),
    # FM metal: its own kick/snare/clap/hats on the shared keys, then bells, chimes and bars dbox can't make
    "fm metal": ("cuckoos nest", [
        key(75, 2.7565, 6.0, 1.2, 12, 0.03, 0, 0.4),         # 0 metal kick
        key(65, 1.4142, 4.0, 3.0, 0, 0.01, 0, 2.5),          # 1 gong
        key(110, 6.2832, 7.0, 0.8, 0, 0.01, 0, 0.3),         # 2 spring hit
        key(98, 2.7565, 3.5, 2.0, 0, 0.01, 0, 1.2),          # 3 plate low
        key(170, 1.1892, 8.0, 0.25, 4, 0.02, SNARE - 2000, 0.12),   # 4 metal snare
        key(147, 1.4142, 5.0, 1.5, 0, 0.01, 0, 0.8),         # 5 clang
        key(440, 1.7321, 6.0, 0.3, 0, 0.01, SNARE + 2000, 0.2),     # 6 anvil snare
        key(330, 2.7565, 4.0, 1.0, 0, 0.01, 0, 0.5),         # 7 bar
        key(660, 2.2361, 5.0, 0.4, 0, 0.01, 0, 0.15),        # 8 tin rim
        key(150, 1.0, 0.0, 0.45, 0, 0.02, 30500, 0.02),      # 9 clap (wide)
        key(1800, 1.4142, 9.0, 0.15, 0, 0.01, HAT + 1000, 0.15),    # 10 hat metal
        key(2200, 2.2361, 8.0, 0.06, 0, 0.01, HAT + 2000, 0.05),    # 11 hat tight
        key(2400, 8.6603, 6.0, 0.4, 0, 0.01, HAT - 3000, 0.3),      # 12 sizzle
        key(1800, 1.4142, 9.0, 0.9, 0, 0.01, HAT + 1000, 0.8),      # 13 open hat metal
        key(392, 9.4248, 4.0, 2.2, 0, 0.01, 0, 1.6),         # 14 shimmer
        key(2600, 3.1416, 12.0, 2.5, 0, 0.01, HAT + 3000, 2.0),     # 15 crash metal
        key(131, 3.5, 3.0, 2.5, 0, 0.01, 0, 2.0),            # 16 bell low
        key(196, 3.5, 3.0, 2.5, 0, 0.01, 0, 2.0),            # 17 bell
        key(294, 3.5, 3.0, 2.5, 0, 0.01, 0, 2.0),            # 18 bell D
        key(392, 3.5, 3.0, 2.5, 0, 0.01, 0, 2.0),            # 19 bell G
        key(523, 3.5, 2.5, 2.0, 0, 0.01, 0, 1.5),            # 20 bell C
        key(587, 7.1268, 2.0, 1.2, 0, 0.01, 0, 0.7),         # 21 chime
        key(1319, 5.4317, 2.0, 1.0, 0, 0.01, 0, 0.6),        # 22 glock
        key(880, 1.4142, 4.0, 0.9, 0, 0.01, 0, 0.5),         # 23 triangle-ish
    ]),
    # zaps: its own kicks/snares/clap/hats on the shared keys, then lasers, blips, glitches
    "fm zaps": ("dem drums", [
        key(60, 1.0, 0.5, 0.4, 48, 0.03, 0, 0.02),           # 0 zap kick
        key(70, 1.0, 3.0, 0.25, 40, 0.015, 0, 0.01),         # 1 punch
        key(45, 1.0, 0.0, 1.5, 36, 0.15, 0, 0.02),           # 2 drop
        key(80, 1.0, 2.0, 0.3, 48, 0.05, 0, 0.05),           # 3 zap
        key(180, 2.0, 5.0, 0.18, 30, 0.03, SNARE, 0.04),     # 4 zap snare
        key(55, 1.0, 1.0, 0.8, 48, 0.1, 0, 0.05),            # 5 dive
        key(200, 1.0, 4.0, 0.2, 48, 0.02, SNARE + 3000, 0.03),      # 6 snap snare
        key(250, 1.0, 0.0, 0.3, 48, 0.2, 0, 0.02),           # 7 slide
        key(400, 1.0, 1.0, 0.08, 24, 0.01, 0, 0.02),         # 8 tick
        key(160, 1.0, 0.0, 0.3, 30, 0.04, CLAP, 0.02),       # 9 clap zap
        key(2000, 1.4142, 6.0, 0.05, 24, 0.01, HAT, 0.04),   # 10 zap hat
        key(2400, 2.4142, 5.0, 0.04, 12, 0.006, HAT + 1500, 0.03),  # 11 zap hat tight
        key(1600, 1.7321, 7.0, 0.12, 30, 0.02, HAT - 1500, 0.08),   # 12 zap hat swept
        key(2000, 1.4142, 6.0, 0.6, 24, 0.02, HAT, 0.4),     # 13 zap hat open
        key(1200, 2.7565, 9.0, 1.4, 36, 0.06, HAT + 2500, 1.0),     # 14 zap crash
        key(120, 7.0, 8.0, 0.35, 36, 0.05, 0, 0.1),          # 15 metal zap
        key(110, 2.0, 3.0, 0.3, 40, 0.08, 0, 0.08),          # 16 laser
        key(150, 1.5, 2.0, 0.25, 36, 0.12, 0, 0.1),          # 17 pew
        key(300, 3.0, 2.0, 0.15, 30, 0.04, 0, 0.06),         # 18 blip down
        key(90, 0.5, 6.0, 0.5, 24, 0.06, 0, 0.2),            # 19 growl
        key(700, 1.5, 3.0, 0.12, 20, 0.015, 0, 0.05),        # 20 chirp
        key(500, 4.0, 3.0, 0.4, 36, 0.1, 0, 0.3),            # 21 ufo
        key(600, 11.0, 4.0, 0.2, 18, 0.02, 0, 0.1),          # 22 glitch
        key(1500, 1.0, 0.0, 0.1, 48, 0.008, 0, 0.02),        # 23 bleep
    ]),
}
# fm industrial (new scales, key2): the harsh zones on the shared roles, with a few clean keys for contrast
KITS["fm industrial"] = ("lo-fi", [
    key2(50, 1.0, SQ(0.4), 0.5, 30, 0.015, 0, 0.1),              # 0 F   drive kick (SQUARE 2, short FMDEC)
    key2(48, 1.0, SQ(1.0), 0.4, 24, 0.02, 0, 0.09),              # 1 F#  square kick (SQUARE: a clipped carrier)
    key2(45, 1.0, 0.4, 0.9, 20, 0.04, 0, 0.02),                  # 2 G   sub kick, clean (contrast)
    key2(110, 2.7565, 8.0, 0.6, 4, 0.03, NZ("METAL", 0.40), 0.25),   # 3 G#  pipe tom low
    key2(200, 3.1416, FB(4), 0.25, 6, 0.02, NZ("SNARE", 0.85), 0.08),  # 4 A   snare (SNARE zone, FEEDBACK 4: cuts)
    key2(165, 2.7565, 8.0, 0.5, 4, 0.03, NZ("METAL", 0.45), 0.2),    # 5 A#  pipe tom mid
    key2(220, 1.0, FB(3), 0.22, 8, 0.015, NZ("GRIND", 0.4), 0.06),   # 6 B   grind snare
    key2(245, 2.7565, 8.0, 0.45, 4, 0.03, NZ("METAL", 0.45), 0.18),   # 7 C   pipe tom high
    key2(900, 5.1926, SQ(0.3), 0.12, 0, 0.01, NZ("GRIND", 0.7), 0.03),  # 8 C#  machine clank (rim)
    key2(150, 1.0, 0.0, 0.4, 0, 0.02, NZ("CLAP", 1.0), 0.02),        # 9 D   clap (CLAP at full)
    key2(900, 1.4142, 1.0, 0.10, 0, 0.01, NZ("METAL", 0.95), 0.05),   # 10 D# closed hat (the 808's six squares)
    key2(1000, 1.4142, 1.0, 0.05, 0, 0.01, NZ("METAL", 0.95), 0.03),   # 11 E  pedal chick
    key2(900, 1.4142, 1.0, 0.15, 0, 0.01, NZ("METAL", 0.95), 0.12),   # 12 F  half-open hat
    key2(900, 1.4142, 1.0, 0.7, 0, 0.01, NZ("METAL", 0.95), 0.5),    # 13 F# open hat
    key2(350, 2.7565, FB(4), 2.5, 0, 0.01, NZ("METAL", 0.8), 1.5),   # 14 G  sheet-metal crash
    key2(520, 3.1416, FB(6), 2.0, 0, 0.01, NZ("METAL", 0.9), 1.2),   # 15 G# china (trashy crash)
    key2(1100, 3.1416, 4.0, 0.8, 0, 0.01, NZ("METAL", 0.4), 0.8),    # 16 A  anvil
    key2(280, 1.0, FB(2), 0.3, 0, 0.01, NZ("GRIND", 0.6), 0.1),      # 17 A# ratchet (GRIND's square)
    key2(2000, 1.4142, 2.0, 0.4, 0, 0.01, NZ("GRIND", 0.2), 0.1),    # 18 B  steam hiss
    key2(300, 1.0, FB(7), 0.5, 0, 0.01, 0, 2.0),                    # 19 C  scream (FEEDBACK 7)
    key2(90, 1.0, FB(4), 1.2, 0, 0.01, NZ("GRIND", 0.9), 1.0),       # 20 C# broken motor
    key2(150, 2.7565, 4.0, 0.5, 36, 0.06, NZ("METAL", 0.3), 0.3),    # 21 D  pipe drop
    key2(70, 1.0, SQ(0.6), 0.35, 24, 0.03, NZ("SNARE", 0.3), 0.08),  # 22 D# slam (SQUARE, a snap of noise)
    key2(880, 3.5, 1.5, 0.3, 0, 0.01, 0, 0.3),                      # 23 E  ping, clean (contrast)
])
DEFAULT_KIT = "fm 808"
LEGACY = {n: _OLD[24 * i:24 * i + 24] for i, n in enumerate(("fm 808", "fm 909", "fm metal", "fm zaps"))}
assert len(_OLD) == 96


def main():
    exp2, ratio, amp, mod, swp = tables()
    noise, clap, fmz, mgain = noise_table(), clap_table(), fm_table(), metal_gain_table()
    ms = [[int(round(10000 * lo * (hi / lo) ** (i / 255))) for i in range(256)] for lo, hi in (DECAY, SNAP, MODDEC)]
    names = "".join(f"\"{z}\", " for z in NOISE_ZONES)
    harm = sum(1 << i for i, r in enumerate(RATIOS) if r in HARMONIC)
    h = ["/* Generated by gen_fmdrum.py; do not edit. Tables for fmdrum.c and its screen (ui_fmdrum.h). */",
         "#ifndef FMDRUM_TABLES_H", "#define FMDRUM_TABLES_H", "#include <stdint.h>", "",
         "/* 2^(i / 64), Q15 (65536 = 2.0) */", c_array("fd_exp2", "uint32_t", exp2),
         "/* RATIO >> 10: modulator : carrier ratio, Q16 (15 harmonic, then 17 inharmonic); x100 for the screen; bit i set =\n"
         " * harmonic (n / 2) */",
         c_array("fd_ratio", "uint32_t", ratio, 8), c_array("fd_ratio_x100", "uint16_t", [round(r * 100) for r in RATIOS]),
         f"#define FD_RATIO_HARMONIC 0x{harm:08x}u",
         "/* knob >> 7: per-block envelope factor, Q30: DECAY (T60 15 ms .. 4 s), FMDEC (the index, T60 3 ms .. 3 s), SWDEC\n"
         " * (the sweep, time constant 1 .. 300 ms) */",
         c_array("fd_amp_dec", "int32_t", amp, 8), c_array("fd_mod_dec", "int32_t", mod, 8),
         c_array("fd_swp_dec", "int32_t", swp, 8),
         "/* the same times for the screen, 0.1 ms units: [0] DECAY T60, [1] SWDEC tau, [2] FMDEC T60 */",
         "static const uint16_t fd_time_10us[3][256] __attribute__((unused)) = {"]
    for row in ms:
        h += ["  {"] + ["    " + ", ".join(str(v) for v in row[i:i + 16]) + "," for i in range(0, 256, 16)] + ["  },"]
    h += ["};",
          "/* the screen's envelope graph: dB per block (Q16) for [0] DECAY (T60), [1] SWDEC (e^(-t / tau)), [2] FMDEC (T60),\n"
          " * by knob >> 7; and its time points, 1 .. 4096 blocks in quarter octaves (blocks, Q8) */",
          "static const int32_t fd_db_blk[3][256] __attribute__((unused)) = {"]
    for lo, hi, tau in ((DECAY[0], DECAY[1], False), (SNAP[0], SNAP[1], True), (MODDEC[0], MODDEC[1], False)):
        row = []
        for i in range(256):
            t = lo * (hi / lo) ** (i / 255)
            db = (20 * math.log10(math.e) * BLOCK / (t * FS)) if tau else 60 * BLOCK / (t * FS)
            row.append(int(round(db * 65536)))
        h += ["  {"] + ["    " + ", ".join(str(v) for v in row[i:i + 12]) + "," for i in range(0, 256, 12)] + ["  },"]
    h += ["};", c_array("fd_t_q8", "uint32_t", [int(round(256 * 2 ** (i / 4))) for i in range(49)]),
          "/* NOISE >> 8 (129 points, interpolated): " + ", ".join(NOISE_COLS) + ":\n"
          " * tone level Q15, noise level Q15, SVF f = 2 sin(pi fc / fs) Q15, damping q Q15, output weights lp/bp/hp Q12\n"
          " * (blend x norm: RMS 0.16 at level 1 for white noise), clap amount Q15, the filter's source weights Q15: white\n"
          " * noise, METAL's six squares, GRIND's square; ring-mod of the filtered source with the voice Q15 (32767 = x2),\n"
          " * GRIND's square pitch Q15 (0 = max(80 Hz, PITCH / 2) .. 32767 = PITCH x sqrt 2), the squares' one-pole\n"
          " * high-pass coefficient Q15 (40 Hz .. 3 kHz) */",
          f"#define FD_NOISE_W {len(NOISE_COLS)}", f"#define FD_NOISE_N {NOISE_N}",
          c_array("fd_noise", "uint16_t", [v for r in noise for v in r], len(NOISE_COLS)),
          "/* FM from the FEEDBACK zone (K2 >= FD_FM_FB0), (K2 - FD_FM_FB0) >> 7: feedback beta (rad, Q8), the carrier's\n"
          " * clip gain (Q10), its level (Q15: a clipped carrier as loud as the sine), the index's scale (Q12) */",
          f"#define FD_FM_FB0 {FM_FB0}", f"#define FD_FM_SQ0 {FM_SQ0}", f"#define FD_FMZ_N {len(fmz)}",
          f"#define FD_SQ_RAMP0 {SQ_RAMP0}",
          c_array("fd_fmz", "uint16_t", [v for r in fmz for v in r], 12),
          "#define FD_FMZ_W 4",
          "/* METAL's six squares: frequency / the lowest, Q16 (the 808's 205.3 .. 800 Hz); the lowest follows PITCH / 2\n"
          f" * within {METAL_LOW:.0f} .. {METAL_TOP:.0f} Hz (FD_METAL_LOW / TOP, K0 units); GRIND's square from {GRIND_LOW:.0f} Hz"
          " (FD_GRIND_LOW) */",
          c_array("fd_metal_ratio", "uint32_t", [int(round(r * 65536)) for r in METAL_RATIOS]),
          f"/* METAL's level: gain Q12 by the cluster's octave above {METAL_OCT0:.0f} Hz (FD_METAL_OCT0, K0 units; "
          f"{METAL_OCTS + 1} points)\n * and (NOISE - METAL's start) >> 9 ({METAL_TN} points, to the top), so the band-passed squares "
          "are 1.15 x as loud as white noise in the same band;\n * fmdrum.c applies it to the filter's output, in proportion to the squares' share of its source */",
          f"#define FD_METAL_OCT0 {k0_of(METAL_OCT0)}", f"#define FD_NOISE_SHIFT {NOISE_SHIFT}", f"#define FD_METAL_OCTS {METAL_OCTS}",
          f"#define FD_METAL_TN {METAL_TN}",
          c_array("fd_metal_gain", "uint16_t", [v for r in mgain for v in r], METAL_TN),
          f"#define FD_METAL_TOP {k0_of(METAL_TOP)}", f"#define FD_METAL_LOW {k0_of(METAL_LOW)}",
          f"#define FD_GRIND_LOW {k0_of(GRIND_LOW)}",
          "/* FEEDBACK levels' beta for the screen, Q8 */",
          c_array("fd_fb_beta", "int16_t", [int(round(v * 256)) for v in FB_BETA]),
          f"/* clap: noise gain at the end of block b (bursts {CLAP_BURSTS} blocks), Q15; from the last one 32767 */",
          f"#define FD_CLAP_N {len(clap)}", c_array("fd_clap", "int16_t", clap),
          f"static const char *const fd_noise_zone[{len(NOISE_ZONES)}] __attribute__((unused)) = {{ {names[:-2]} }};",
          f"#define FD_TONE {int(round(TONE * 32767))}",
          "/* fmdrum.c's sine in its assembly loops: sine256[i] (low half) and sine256[i + 1] - sine256[i] (high half) */",
          c_array("fd_sin2", "uint32_t", sine_pairs(), 8), "#endif", ""]
    open(os.path.join(HERE, "fmdrum_tables.h"), "w").write("\n".join(h))
    kits = {}
    for name, (template, keys) in KITS.items():
        assert len(keys) == 24, name
        kits[name] = {"template": template, "dbox_data": keys}
    lines = ["{"]
    for j, (name, kit) in enumerate(kits.items()):
        rows = ",\n".join("    " + json.dumps(r) for r in kit["dbox_data"])
        lines.append(f' "{name}": {{"template": "{kit["template"]}", "dbox_data": [\n{rows}\n ]}}'
                     + ("," if j < len(kits) - 1 else ""))
    open(os.path.join(HERE, "fmdrum_kits.json"), "w").write("\n".join(lines + ["}", ""]))
    assert json.load(open(os.path.join(HERE, "fmdrum_kits.json"))) == kits
    print("wrote fmdrum_tables.h, fmdrum_kits.json:", ", ".join(kits))


if __name__ == "__main__":
    main()
