#!/usr/bin/env python3
"""Generate the granular engine's source bank (grain.c): 6 sounds, joined end to end, all rooted at C4.

  python3 src/engine/gen_grain_data.py OUT.bin [--wav DIR]

Layout (must match grain_layout.h): int16 overview[OVERVIEW] (peak of each 1/OVERVIEW of the bank, Q15),
then int16 samples[SOURCES * SOURCE_LEN + 1] (44.1 kHz mono, Q15; the last is a copy of the first, so the
interpolating read at the end needs no wrap test). Each source is synthesised here (no recorded
audio), peak-normalised to -3 dBFS, with 20 ms fades at both ends so grains crossing a boundary don't click.
--wav DIR writes each source as a WAV for listening.
"""
import argparse
import os
import wave

import numpy as np

FS = 44100
SOURCES = 6
SOURCE_LEN = 70560                      # 1.6 s
OVERVIEW = 256
ROOT = 261.6256                         # C4
NAMES = ["choir", "strings", "bells", "glass", "air", "plucks"]
rng = np.random.default_rng(2026)
t = np.arange(SOURCE_LEN) / FS


def formant_voice(f0, formants, vib_rate, vib_depth, phase):
    """Glottal-ish source (band-limited saw with -12 dB/oct tilt) through formant peaks, with vibrato."""
    inst = f0 * (1 + vib_depth * np.sin(2 * np.pi * vib_rate * t + phase))
    ph = 2 * np.pi * np.cumsum(inst) / FS
    out = np.zeros(SOURCE_LEN)
    for n in range(1, int(8000 / f0)):
        hf = n * inst
        amp = sum(g * np.exp(-0.5 * ((hf - fc) / bw) ** 2) for fc, bw, g in formants) / n
        out += amp * np.sin(n * ph)
    return out


def choir():
    aah = [(700, 90, 1.0), (1220, 110, 0.5), (2600, 160, 0.25), (3300, 200, 0.1)]
    out = sum(formant_voice(ROOT * 2 ** (c / 1200), aah, 5.0 + rng.random(), 0.006, rng.random() * 6)
              for c in (-12, -5, 0, 4, 9))
    return out * np.minimum(1, t / 0.3)


def strings():
    out = np.zeros(SOURCE_LEN)
    for c in (-8, -3, 0, 3, 8):
        f = ROOT * 2 ** (c / 1200) * (1 + 0.003 * np.sin(2 * np.pi * 5.5 * t + rng.random() * 6))
        ph = 2 * np.pi * np.cumsum(f) / FS
        for n in range(1, int(10000 / ROOT)):
            out += np.sin(n * ph) / n * np.exp(-n / 18)
    return out * np.minimum(1, t / 0.4)


def bells():
    out = np.zeros(SOURCE_LEN)
    for start, mult in ((0.0, 1.0), (0.4, 2.0), (0.8, 1.5), (1.2, 1.0)):
        tt = t - start
        on = tt >= 0
        for ratio, g, dec in ((0.56, 0.6, 1.2), (0.92, 0.4, 1.0), (1.0, 1.0, 1.5), (1.19, 0.5, 0.9),
                              (1.71, 0.4, 0.6), (2.0, 0.35, 0.7), (2.74, 0.3, 0.4), (3.0, 0.25, 0.35)):
            out += on * g * np.exp(-np.where(on, tt, 0) / dec) * np.sin(2 * np.pi * ROOT * mult * ratio * np.where(on, tt, 0))
    return out


def glass():
    out = np.zeros(SOURCE_LEN)
    for n, g in ((1, 1.0), (2.76, 0.5), (5.4, 0.35), (8.93, 0.2), (13.3, 0.12)):
        trem = 1 + 0.3 * np.sin(2 * np.pi * (0.7 + n * 0.13) * t + rng.random() * 6)
        out += g * trem * np.sin(2 * np.pi * ROOT * n * t + rng.random() * 6)
    return out * np.minimum(1, t / 0.05)


def air():
    noise = rng.standard_normal(SOURCE_LEN)
    spec = np.fft.rfft(noise)
    f = np.fft.rfftfreq(SOURCE_LEN, 1 / FS)
    shape = np.zeros_like(f)
    for n in range(1, 24):                                  # breathy noise with soft peaks on C harmonics
        shape += np.exp(-0.5 * ((f - n * ROOT) / (6 + 2 * n)) ** 2) / n ** 0.7
    shape += 0.05 * np.exp(-f / 4000)
    out = np.fft.irfft(spec * shape, SOURCE_LEN)
    return out * (0.6 + 0.4 * np.sin(2 * np.pi * 0.8 * t))


def plucks():
    out = np.zeros(SOURCE_LEN)
    for i, c in enumerate((0, 7, 12, 16, 19, 24, 12, 7)):
        start = int(i * 0.2 * FS)
        f = ROOT * 2 ** (c / 12)
        period = int(round(FS / f))
        buf = rng.uniform(-1, 1, period)
        y = np.zeros(SOURCE_LEN - start)
        for n in range(len(y)):
            v = buf[n % period]
            y[n] = v
            buf[n % period] = 0.5 * (v + buf[(n + 1) % period]) * 0.996
        out[start:] += y
    return out


def build():
    bank = []
    fade = int(0.02 * FS)
    ramp = np.linspace(0, 1, fade)
    for fn in (choir, strings, bells, glass, air, plucks):
        x = fn()
        a, y = np.exp(-2 * np.pi * 20 / FS), 0.0          # 20 Hz high-pass: no DC (Karplus noise has some)
        prev, hp = 0.0, np.empty_like(x)
        for i, v in enumerate(x):
            y = a * (y + v - prev)
            prev = v
            hp[i] = y
        x = hp / np.abs(hp).max() * 0.707
        x[:fade] *= ramp
        x[-fade:] *= ramp[::-1]
        bank.append(x)
    samples = np.clip(np.round(np.concatenate(bank) * 32767), -32767, 32767).astype(np.int16)
    step = len(samples) / OVERVIEW
    overview = np.array([np.abs(samples[int(i * step):int((i + 1) * step)]).max() for i in range(OVERVIEW)], np.int16)
    return overview, samples, bank


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--wav")
    a = ap.parse_args()
    overview, samples, bank = build()
    with open(a.out, "wb") as f:
        f.write(overview.astype("<i2").tobytes())
        f.write(samples.astype("<i2").tobytes())
        f.write(samples[:1].astype("<i2").tobytes())       # guard sample
    if a.wav:
        os.makedirs(a.wav, exist_ok=True)
        for name, x in zip(NAMES, bank):
            w = wave.open(os.path.join(a.wav, f"{name}.wav"), "wb")
            w.setnchannels(1), w.setsampwidth(2), w.setframerate(FS)
            w.writeframes(np.clip(np.round(x * 32767), -32767, 32767).astype("<i2").tobytes())
            w.close()


if __name__ == "__main__":
    main()
