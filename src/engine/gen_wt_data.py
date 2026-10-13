#!/usr/bin/env python3
"""Generate the wavetable data for wt.c: 8 banks x 12 frames, each frame at 9 band-limited levels.

  python3 src/engine/gen_wt_data.py OUT.bin [--wav DIR]

Level L holds harmonics 1 .. 256 >> L (256, 128, ..., 1) at 8x oversampling (size 8 * harmonics,
minimum 32), so wt.c's cubic interpolation leaves images below about -44 dB (4x with linear
interpolation measured -19 dB for a strong top harmonic) and it can pick a level with no aliasing.
Total 8 * 12 * 4128 * 2 B = 792 KB, inside one 1 MB FREE region.
Frames are int16, peak-normalized per frame to 0.9 over all levels (truncation overshoot included).
Layout (must match wt.c): bank b, frame f, level L at sample offset
  (b * FRAMES + f) * FRAME_STRIDE + LEVEL_OFF[L].
--wav DIR also writes each bank as a WAV (12 frames of level 0 back to back) for listening/inspection.
"""
import argparse
import os
import wave

import numpy as np

BANKS, FRAMES, LEVELS, H0 = 8, 12, 9, 256
SIZES = [max(8 * (H0 >> L), 32) for L in range(LEVELS)]         # 2048, 1024, ..., 32, 32, 32
LEVEL_OFF = [sum(SIZES[:L]) for L in range(LEVELS)]
FRAME_STRIDE = sum(SIZES)                                        # 4128
BANK_NAMES = ["analog", "pwm", "vowel", "organ", "sync", "fold", "bell", "digital"]
N_HI = 8192                                                      # time-domain design resolution


def from_time(fn):
    """Harmonic spectrum (complex, index = harmonic) of one cycle fn(t), t in [0, 1)."""
    t = np.arange(N_HI) / N_HI
    return np.fft.rfft(fn(t))[:H0 + 1] / (N_HI / 2)


def from_amps(amps, phase=None):
    s = np.zeros(H0 + 1, complex)
    a = np.asarray(amps, float)[:H0]
    ph = np.zeros(len(a)) if phase is None else np.asarray(phase)[:len(a)]
    s[1:len(a) + 1] = a * np.exp(1j * (ph - np.pi / 2))          # sine phase
    return s


def harm():
    return np.arange(1, H0 + 1, dtype=float)


def bank_analog(x):          # sine -> triangle -> square -> saw
    x = 1 - x                 # built saw-first; the bank starts at the sine
    n = harm()
    saw, sq = 1 / n, np.where(n % 2 == 1, 1 / n, 0)
    tri = np.where(n % 2 == 1, (-1) ** ((n - 1) // 2) / n ** 2, 0)
    sine = (n == 1).astype(float)
    seg, f = min(int(x * 3), 2), x * 3 - min(int(x * 3), 2)
    a, b = [(saw, sq), (sq, tri), (tri, sine)][seg]
    return from_amps((1 - f) * a + f * b)


def bank_pwm(x):             # pulse width 50% -> 3%
    w = 0.5 - 0.47 * x
    return from_time(lambda t: np.where(t < w, 1.0, -1.0) - (2 * w - 1))


def bank_vowel(x):           # formant peaks over a 110 Hz series, sweeping a-e-i-o-u
    formants = [(800, 1150, 2900), (400, 1600, 2700), (300, 2300, 3000), (450, 800, 2830), (325, 700, 2530)]
    p = x * (len(formants) - 1)
    i, f = min(int(p), len(formants) - 2), p - min(int(p), len(formants) - 2)
    fr = [(1 - f) * a + f * b for a, b in zip(formants[i], formants[i + 1])]
    hz = harm() * 110
    env = sum(np.exp(-0.5 * ((hz - c) / (60 + 0.08 * c)) ** 2) * g for c, g in zip(fr, (1, 0.6, 0.3)))
    env[:2] += (0.25, 0.1)                                       # body, so high notes (few harmonics) keep level
    return from_amps(env / np.sqrt(harm()) + 0.002)


def bank_organ(x):           # drawbar registrations, crossfaded
    regs = [[8, 0, 0, 0, 0, 0, 0, 0, 0], [8, 8, 8, 0, 0, 0, 0, 0, 0], [8, 0, 8, 6, 0, 4, 0, 0, 0],
            [8, 8, 8, 8, 8, 8, 8, 8, 8], [6, 0, 0, 0, 0, 0, 3, 6, 8]]
    feet = [1, 2, 3, 4, 6, 8, 10, 12, 16]                       # harmonic numbers of the drawbars
    p = x * (len(regs) - 1)
    i, f = min(int(p), len(regs) - 2), p - min(int(p), len(regs) - 2)
    a = np.zeros(H0)
    for h, d0, d1 in zip(feet, regs[i], regs[i + 1]):
        a[h - 1] += ((1 - f) * d0 + f * d1) / 8
    return from_amps(a)


def bank_sync(x):            # hard-synced saw, slave ratio 1 -> 4.5 (whole ratios lose low harmonics at high notes)
    r = 1 + 3.5 * x
    return from_time(lambda t: 2 * ((t * r) % 1.0) - 1)


def bank_fold(x):            # sine through a wavefolder, drive 1 -> 6
    d = 1 + 5 * x
    return from_time(lambda t: np.sin(d * np.sin(2 * np.pi * t) * np.pi / 2))


def bank_bell(x):            # sparse high partials fading in
    a = np.zeros(H0)
    for h, g, start in [(1, 1, 0), (3, 0.5, 0.1), (7, 0.6, 0.2), (12, 0.5, 0.35), (19, 0.4, 0.5),
                        (27, 0.35, 0.65), (38, 0.3, 0.8)]:
        a[h - 1] = g * np.clip((x - start) * 4 + (h == 1), 0, 1)
    return from_amps(a)


def bank_digital(x):         # seeded random spectra, brightness rising
    rng = np.random.default_rng(1234)
    base = rng.random(H0)
    phase = rng.random(H0) * 2 * np.pi
    tilt = harm() ** -(2.0 - 1.6 * x)
    return from_amps(base * tilt, phase)


BANKS_FN = [bank_analog, bank_pwm, bank_vowel, bank_organ, bank_sync, bank_fold, bank_bell, bank_digital]


def render(spec, harmonics, size):
    s = np.zeros(size // 2 + 1, complex)
    s[1:harmonics + 1] = spec[1:harmonics + 1]
    return np.fft.irfft(s * (size / 2), size)


def build():
    out = np.zeros(BANKS * FRAMES * FRAME_STRIDE, np.int16)
    for b, fn in enumerate(BANKS_FN):
        for f in range(FRAMES):
            spec = fn(f / (FRAMES - 1))
            spec[0] = 0
            peak = max(np.abs(render(spec, H0 >> L, 8 * SIZES[L])).max() for L in range(LEVELS))
            gain = 0.9 * 32767 / peak
            base = (b * FRAMES + f) * FRAME_STRIDE
            for L in range(LEVELS):
                y = render(spec, H0 >> L, SIZES[L]) * gain
                out[base + LEVEL_OFF[L]: base + LEVEL_OFF[L] + SIZES[L]] = np.clip(np.round(y), -32767, 32767)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--wav")
    a = ap.parse_args()
    data = build()
    open(a.out, "wb").write(data.astype("<i2").tobytes())
    if a.wav:
        os.makedirs(a.wav, exist_ok=True)
        for b, name in enumerate(BANK_NAMES):
            frames = [data[(b * FRAMES + f) * FRAME_STRIDE:][:SIZES[0]] for f in range(FRAMES)]
            w = wave.open(os.path.join(a.wav, f"{b}_{name}.wav"), "wb")
            w.setnchannels(1), w.setsampwidth(2), w.setframerate(44100)
            w.writeframes(np.concatenate(frames).astype("<i2").tobytes())
            w.close()


if __name__ == "__main__":
    main()
