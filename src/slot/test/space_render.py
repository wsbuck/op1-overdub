#!/usr/bin/env python3
"""Render the space FX (src/slot/space.c, its asm kernels, in the Blackfin simulator) to WAVs for listening.

  python3 src/slot/test/space_render.py OUTDIR

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


def main():
    out = sys.argv[1]
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
