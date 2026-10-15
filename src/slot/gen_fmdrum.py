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
  K2 white   FM      peak index 0 .. 16 rad, quadratic (half = 4 rad)
  K3 orange  DECAY   amplitude (tone and noise), 15 ms .. 4 s to -60 dB, exponential
  K4 SHIFT+blue    RATIO   modulator : carrier, 32 steps: the 15 harmonic ratios (n / 2, TONE) rising, then the 17
                           inharmonic ones (METAL) rising
  K5 SHIFT+green   SWDEC   sweep time constant, 1 .. 300 ms, exponential
  K6 SHIFT+white   FMDEC   the FM index's own decay, 3 ms .. 3 s to -60 dB, exponential
  K7 SHIFT+orange  NOISE   one knob through the noise families: OFF, AIR (dark, under the tone), SNARE (band, with
                           the tone), HAT (bright, the tone fades out), CLAP (mid band, three bursts, noise only);
                           stock dbox values stop at 32255, still CLAP
Keys 10..13 are a hi-hat choke group (fmdrum.c): a hit there on a key in the HAT zone releases the group's other HAT
voices in ~5 ms. The kits share key roles: 0-2 kicks, 3/5/7 toms, 4/6 snares, 8 rim, 9 clap, 10-13 hats, 14-15
cymbals, 16-23 percussion and effects.
"""
import json
import math
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
FM_MAX = 16.0                              # rad at K2 = 32767
SWEEP_MAX = 48                             # semitones at K4 = 32767
DECAY = (0.015, 4.0)                       # K3: T60 range, s
SNAP = (0.001, 0.3)                        # K5: time constant range, s
MODDEC = (0.003, 3.0)                      # K7: T60 range, s
NOISE_ZONES = ["OFF", "AIR", "SNARE", "HAT", "CLAP"]   # K6 < 1024, then 4 zones of 8192 (AIR from 1024)
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


def fm_rad(k2):
    k = max(k2, 0)
    return ((((k * k) >> 15) * 4096) >> 15) / 256.0       # as the engine: Q8 rad


def k2_of(rad):
    return min(32767, int(round(math.sqrt(rad / FM_MAX) * 32767)))


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


def noise_zone(k6):
    k = max(k6, 0)
    return 0 if k < 1024 else 1 + min(3, k >> 13)


# ---- K6: the noise path, 65 points (k6 = i * 512), interpolated per block ----
def noise_point(k):
    """(tone level, noise level, fc Hz, damping q, lp/bp/hp blend, clap amount) at k6 = k"""
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


def noise_table():
    rng = np.random.default_rng(1)
    seed, xs = 12345, []
    for _ in range(1 << 14):                # the engine's LCG, top bits >> 17: +-16384
        seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
        xs.append((seed - (1 << 32) if seed >> 31 else seed) >> 17)
    x = np.array(xs)
    del rng
    rows = []
    for i in range(65):
        k = min(i * 512, 32767)
        tone, level, fc, q, blend, clap = noise_point(k)
        f = int(round(2 * math.sin(math.pi * fc / FS) * 32768))
        qq = int(round(q * 32768))
        # weights: blend x norm (Q12), so the output RMS is NOISE_RMS x 32768 at level 1 (level applied as a gain)
        w0 = [int(round(b * 4096)) for b in blend]
        y, _ = svf_run(f, qq, w0, x)
        spec = np.fft.rfft(y[2048:].astype(float))          # RMS of what is heard: TE's 2:1 decimation keeps < 22 kHz
        spec[np.fft.rfftfreq(len(y) - 2048, 1 / FS) > 20000] = 0
        rms = float(np.sqrt(np.mean(np.fft.irfft(spec, len(y) - 2048) ** 2))) or 1.0
        norm = NOISE_RMS * 32768 / rms
        w = [int(round(b * 4096 * norm)) for b in blend]
        y, amax = svf_run(f, qq, w, x)
        assert amax < 1 << 30, (i, amax)    # int32 headroom in the engine's blend
        assert max(w) < 32768, (i, w)
        rows.append((int(round(tone * 32767)), int(round(level * 32767)), f, qq, w[0], w[1], w[2],
                     int(round(clap * 32767))))
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


def c_array(name, ctype, vals, per=12):
    s = f"static const {ctype} {name}[{len(vals)}] __attribute__((unused)) = {{\n"
    for i in range(0, len(vals), per):
        s += "    " + ", ".join(str(v) for v in vals[i:i + per]) + ",\n"
    return s + "};\n"


# ---- factory kits: per key (pitch Hz, ratio, FM rad, decay s, sweep semitones, sweep time s, noise K7, FM decay s) ----
# Knob order (fmdrum.h): PITCH, SWEEP, FM, DECAY, RATIO, SWDEC, FMDEC, NOISE.
def key(hz, ratio=1.0, fm=0.0, decay=0.3, sweep=0.0, snap=0.02, noise=0, mod=0.05):
    k = [k0_of(hz), k4_of(sweep), k2_of(fm), k_expo(decay, *DECAY), k1_of(ratio), k_expo(snap, *SNAP),
         k_expo(mod, *MODDEC), noise]
    for j, v in enumerate(k):              # TE validates factory presets: stay inside what stock dbox kits hold
        k[j] = min(STOCK_HI[j], max(STOCK_LO[j], v))
    return k


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
DEFAULT_KIT = "fm 808"


def main():
    exp2, ratio, amp, mod, swp = tables()
    noise, clap = noise_table(), clap_table()
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
          "/* NOISE >> 9 (65 points, interpolated): tone level Q15, noise level Q15, SVF f = 2 sin(pi fc / fs) Q15, damping q\n"
          " * Q15, output weights lp/bp/hp Q12 (blend x norm: RMS 0.16 at level 1), clap amount Q15 */",
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
          "#define FD_NOISE_W 8", c_array("fd_noise", "int32_t", [v for r in noise for v in r], 8),
          f"/* clap: noise gain at the end of block b (bursts {CLAP_BURSTS} blocks), Q15; from the last one 32767 */",
          f"#define FD_CLAP_N {len(clap)}", c_array("fd_clap", "int16_t", clap),
          f"static const char *const fd_noise_zone[{len(NOISE_ZONES)}] __attribute__((unused)) = {{ {names[:-2]} }};",
          f"#define FD_TONE {int(round(TONE * 32767))}", "", "#endif", ""]
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
