#!/usr/bin/env python3
"""Render the space FX (src/slot/space.c, its asm kernels, in the Blackfin simulator) to WAVs for listening.

  python3 src/slot/test/space_render.py OUTDIR [--v11]

--v11: the v1.1 TAIL zones instead (SUB -12 on a pad, GATE on an 80s drum pattern, SWELL on plucks, FREEZE in each,
and the plain tail of each for A / B), on the 22.05 kHz master tank (SPACE_MASTER_HALF, as the test images) and the
synth / drum FX.

16-bit stereo 44.1 kHz. The test signal is the prototype's (design/fx/reverb_proto/proto.py: a plucked E minor
arpeggio, a pad chord, three snare-like hits), plus a 120 BPM drum loop for the duck. The master renders use the
44.1 kHz tank; `*_master22` the 22.05 kHz master tank (build option SPACE_MASTER_HALF), `*_synth` the synth / drum FX
(mono, 22.05 kHz, on L; written to both channels). Knobs are raw (0 .. 32767): SPACE, DECAY, TAIL, MIX.
"""
import os
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "design", "fx", "reverb_proto"))
import spacesim as S  # noqa: E402
import proto as P  # noqa: E402

FS = 44100
LEAD = 40 * 64


def drums(bars=4, bpm=120.0, tail=4.0):
    rng = np.random.default_rng(4)
    beat = 60 / bpm
    n = int((bars * 4 * beat + tail) * FS)
    x = np.zeros(n)
    for b in range(bars * 4):
        t0 = int(b * beat * FS)
        if b % 2 == 0:                                      # kick: a falling sine
            m = int(0.35 * FS)
            tt = np.arange(m) / FS
            f = 50 + 70 * np.exp(-tt / 0.04)
            x[t0:t0 + m] += 0.8 * np.sin(2 * np.pi * np.cumsum(f) / FS) * np.exp(-tt / 0.12)
        else:                                               # snare: noise and a 190 Hz body
            m = int(0.2 * FS)
            tt = np.arange(m) / FS
            x[t0:t0 + m] += 0.45 * (rng.uniform(-1, 1, m) * np.exp(-tt / 0.05) + np.sin(2 * np.pi * 190 * tt) *
                                    np.exp(-tt / 0.03))
        for h in (0, 1):                                    # closed hats on the eighths
            th = t0 + int(h * beat / 2 * FS)
            m = int(0.03 * FS)
            hh = rng.uniform(-1, 1, m)
            hh = hh - np.convolve(hh, np.ones(4) / 4, "same")
            x[th:th + m] += 0.25 * hh * np.exp(-np.arange(m) / FS / 0.008)
    return np.stack([x, x], 1)


def wav(path, y):
    y = np.clip(np.round(y), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(y.tobytes())


def pad(dur=4.0, rel=0.4):
    """a saw pad, C minor 9 (C2 .. D4), slow attack, released at dur"""
    n = int((dur + rel + 0.1) * FS)
    t = np.arange(n) / FS
    env = np.clip(t / 0.25, 0, 1) * np.where(t < dur, 1.0, np.exp(-(t - dur) / (rel / 4)))
    x = sum(((f * t * (1 + 0.0007 * k)) % 1) * 2 - 1 for k, f in enumerate((65.41, 130.8, 155.6, 196.0, 233.1, 293.7)))
    x = np.convolve(x / 6, np.ones(6) / 6, "same") * env * 0.6         # a little darker
    return np.stack([x, x], 1)


def drums80(bars=4, bpm=100.0, tail=2.5):
    """kick on 1 and 3, a big snare on 2 and 4, 8th hats: the 80s gated-snare pattern"""
    rng = np.random.default_rng(80)
    beat = 60 / bpm
    n = int((bars * 4 * beat + tail) * FS)
    x = np.zeros(n)
    for b in range(bars * 4):
        t0 = int(b * beat * FS)
        if b % 2 == 0:
            m = int(0.4 * FS)
            tt = np.arange(m) / FS
            f = 48 + 90 * np.exp(-tt / 0.03)
            x[t0:t0 + m] += 0.85 * np.sin(2 * np.pi * np.cumsum(f) / FS) * np.exp(-tt / 0.15)
        else:
            m = int(0.25 * FS)
            tt = np.arange(m) / FS
            nz = rng.uniform(-1, 1, m)
            nz = nz - 0.6 * np.concatenate([[0], nz[:-1]])
            x[t0:t0 + m] += 0.6 * (nz * np.exp(-tt / 0.06) + 0.8 * np.sin(2 * np.pi * 185 * tt) * np.exp(-tt / 0.04))
        for h in (0, 1):
            th = t0 + int(h * beat / 2 * FS)
            m = int(0.03 * FS)
            hh = rng.uniform(-1, 1, m)
            hh = hh - np.convolve(hh, np.ones(4) / 4, "same")
            x[th:th + m] += 0.18 * hh * np.exp(-np.arange(m) / FS / 0.008)
    return np.stack([x, x], 1) * 0.72                      # peaks under 0 dBFS (x 0.9 later)


def plucks(tail=3.0):
    """guitar-like plucks (Karplus-Strong) and synth plucks: an arpeggio with gaps, then chords"""
    rng = np.random.default_rng(11)
    notes = [(0.0, 196.0), (0.6, 246.9), (1.2, 293.7), (1.8, 392.0), (3.0, 220.0), (3.6, 261.6), (4.2, 329.6),
             (6.0, 196.0), (6.0, 246.9), (6.0, 293.7), (8.0, 174.6), (8.0, 220.0), (8.0, 261.6)]
    n = int((10 + tail) * FS)
    x = np.zeros(n)
    for i, (t0, f) in enumerate(notes):
        m = int(2.5 * FS)
        if i % 2 == 0 or t0 >= 6:                                       # Karplus-Strong
            p = int(FS / f)
            buf = rng.uniform(-1, 1, p)
            y = np.zeros(m)
            for k in range(m):
                y[k] = buf[k % p]
                buf[k % p] = 0.4985 * (buf[k % p] + buf[(k + 1) % p])
        else:                                                           # a synth pluck: a decaying filtered saw
            tt = np.arange(m) / FS
            y = (((f * tt) % 1) * 2 - 1) * np.exp(-tt / 0.35)
            y = np.convolve(y, np.ones(4) / 4, "same")
        a = int(t0 * FS)
        x[a:a + m] += 0.35 * y[:n - a]
    return np.stack([x, x], 1)


def main_v11(out):
    os.makedirs(out, exist_ok=True)
    pd_, dr, pl = pad(), drums80(), plucks()
    SUB, GATE200, GATE120, GATE350, SWELL05, SWELL15, SWELL02 = 23551, 26112, 24576, 28671, 30720, 32767, 28672

    def frz(k, at):                                                    # FREEZE from `at` s on
        return lambda ts: (k[0], 32767 if ts >= at else k[1], k[2], k[3])
    renders = [  # name, signal, mode, knobs (or a function of the time), tail (s)
        ("20_pad_plain_cathedral", pd_, "s", (24576, 18000, 5120, 14000), 10),
        ("21_pad_sub100_cathedral", pd_, "s", (24576, 18000, SUB, 14000), 10),
        ("22_pad_sub60_hall", pd_, "s", (8192, 20000, 21000, 14000), 8),
        ("23_pad_sub100_void_x4", pd_, "s", (32767, 30719, SUB, 13000), 20),
        ("24_pad_frozen_sub_cathedral", pd_, "s", frz((24576, 18000, SUB, 14000), 3.0), 14),
        ("25_pad_sub_synth", pd_, "m", (24576, 18000, SUB, 14000), 10),
        ("30_drums_plain_hall", dr, "s", (8192, 16384, 5120, 15000), 2),
        ("31_drums_gate200_hall", dr, "s", (8192, 16384, GATE200, 15000), 2),
        ("32_drums_gate120_church", dr, "s", (16384, 22000, GATE120, 16384), 2),
        ("33_drums_gate350_cathedral", dr, "s", (24576, 24000, GATE350, 16384), 2),
        ("34_drums_gate200_drumfx", dr, "m", (16384, 20000, GATE200, 16384), 2),
        ("35_pad_gate200_hall", pd_, "s", (8192, 20000, GATE200, 14000), 4),
        ("36_pad_frozen_gate", np.concatenate([pd_[:int(2.5 * FS)] * np.minimum(1, (int(2.5 * FS) - np.arange(int(2.5 * FS)))
                                                                   / (0.05 * FS))[:, None],
                                               np.zeros((int(1.5 * FS), 2)), dr[:int(4 * FS)]]),
         "s", frz((24576, 18000, GATE200, 14000), 2.0), 3),
        ("40_plucks_plain_church", pl, "s", (16384, 16384, 5120, 15000), 3),
        ("41_plucks_swell05_church", pl, "s", (16384, 16384, SWELL05, 15000), 3),
        ("42_plucks_swell15_cathedral", pl, "s", (24576, 20000, SWELL15, 16384), 4),
        ("43_plucks_swell02_hall", pl, "s", (8192, 16384, SWELL02, 15000), 3),
        ("44_plucks_swell05_synth", pl, "m", (16384, 16384, SWELL05, 15000), 3),
        ("45_plucks_frozen_swell", pl, "s", frz((24576, 18000, SWELL05, 15000), 2.0), 3),
        ("50_zone_sweep_plucks", pl, "s", lambda ts: (16384, 18000, 19456 + int(min(1, max(0, ts / 10)) * 13311), 15000),
         3),
    ]
    wav(os.path.join(out, "00_pad_dry.wav"), pd_ * 32767 * 0.9)
    wav(os.path.join(out, "00_drums80_dry.wav"), dr * 32767 * 0.9)
    wav(os.path.join(out, "00_plucks_dry.wav"), pl * 32767 * 0.9)
    cases, meta = {}, {}
    for name, x, mode, k, tail in renders:
        xx = np.concatenate([np.zeros((LEAD, 2)), x * 32767 * 0.9, np.zeros((int(tail * FS), 2))])
        c = S.Case(xx, mode)
        for i in range((len(xx) + 63) // 64):
            ts = (i * 64 - LEAD) / FS
            c.block(k(ts) if callable(k) else k)
        cases[name] = c
        meta[name] = (len(xx), mode)
    res = S.run(cases, sim=True, defs="-DSPACE_MASTER_HALF=1")
    for name, (n, mode) in meta.items():
        y = res[name][0][LEAD:n].astype(float)
        if mode == "m":
            y[:, 1] = y[:, 0]
        fade = int(1.0 * FS)
        y[-fade:] *= np.linspace(1, 0, fade)[:, None]
        wav(os.path.join(out, name + ".wav"), y)
        pk = 20 * np.log10(np.abs(y).max() / 32768 + 1e-12)
        print(f"{name}.wav: {len(y) / FS:.1f} s, peak {pk:+.1f} dBFS, CYCLES max/mean {res[name][2]}")


def main():
    out = sys.argv[1]
    if "--v11" in sys.argv[2:]:
        return main_v11(out)
    os.makedirs(out, exist_ok=True)
    sig = P.test_signal(0.0)
    dr = drums()
    renders = [  # name, signal, mode, knobs, tail (s), build
        ("01_room", sig, "s", (0, 16384, 5120, 9000), 3, ""),
        ("02_hall", sig, "s", (8192, 16384, 5120, 11000), 5, ""),
        ("03_church", sig, "s", (16384, 16384, 5120, 12000), 7, ""),
        ("04_cathedral", sig, "s", (24576, 16384, 4000, 13000), 11, ""),
        ("05_void", sig, "s", (32767, 20000, 5120, 16000), 20, ""),
        ("06_shimmer_plus12", sig, "s", (20480, 22000, 12800, 15000), 14, ""),
        ("07_shimmer_plus12_19", sig, "s", (24576, 22000, 17000, 15000), 14, ""),
        ("08_freeze_hall", sig, "s", "freeze", 8, ""),
        ("09_infinite_hall", sig, "s", (8192, 31000, 5120, 13000), 8, ""),
        ("10_drums_cathedral_noduck", dr, "s", (24576, 16384, 5120, 16384), 3, ""),
        ("11_drums_cathedral_duck", dr, "s", (24576, 16384, 5120, 26000), 3, ""),
        ("12_frozen_shimmer_cathedral", sig, "s", "frozen_shimmer", 10, ""),
        ("13_hall_synth", sig, "m", (8192, 16384, 5120, 11000), 5, ""),
        ("14_cathedral_master22", sig, "s", (24576, 16384, 4000, 13000), 11, "-DSPACE_MASTER_HALF=1"),
        ("15_room_dark_synth", sig, "m", (2000, 12000, 1500, 12000), 3, ""),
    ]
    wav(os.path.join(out, "00_dry.wav"), sig * 32767 * 0.9)
    wav(os.path.join(out, "00_drums_dry.wav"), dr * 32767 * 0.9)
    for build in ("", "-DSPACE_MASTER_HALF=1"):
        cases, meta = {}, {}
        for name, x, mode, k, tail, b in renders:
            if b != build:
                continue
            xx = np.concatenate([np.zeros((LEAD, 2)), x * 32767 * 0.9, np.zeros((int(tail * FS), 2))])
            c = S.Case(xx, mode)
            nb = (len(xx) + 63) // 64
            for i in range(nb):
                ts = (i * 64 - LEAD) / FS
                if k == "freeze":                           # HALL; frozen 2.9 .. 10 s (the pad chord)
                    kk = (8192, 32767 if 2.9 <= ts < 10 else 16384, 5120, 12000)
                elif k == "frozen_shimmer":                 # CATHEDRAL, +12 zone; frozen 2.9 .. 14 s
                    kk = (24576, 32767 if 2.9 <= ts < 14 else 20000, 12800, 14000)
                else:
                    kk = k
                c.block(kk)
            cases[name] = c
            meta[name] = (len(xx), mode)
        if not cases:
            continue
        res = S.run(cases, sim=True, defs=build)
        for name, (n, mode) in meta.items():
            y = res[name][0][LEAD:n].astype(float)
            if mode == "m":
                y[:, 1] = y[:, 0]
            fade = int(1.0 * FS)
            y[-fade:] *= np.linspace(1, 0, fade)[:, None]
            wav(os.path.join(out, name + ".wav"), y)
            pk = 20 * np.log10(np.abs(y).max() / 32768 + 1e-12)
            print(f"{name}.wav: {len(y) / FS:.1f} s, peak {pk:+.1f} dBFS, CYCLES max/mean {res[name][2]}")


if __name__ == "__main__":
    main()
