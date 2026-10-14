"""Simulator runner for engines with their own driver (harm_check.py, fm4_check.py), at the device's 88.2 kHz.

simrun.py (va/wt/grain checks) builds engine_main.c and converts frequencies at 44.1 kHz. Engines really render at
88.2 kHz (notes/fx-slot.md: TE decimates the voices 2:1). Pitch increments are cycles per sample, so pitch-relative
results are the same at either rate; absolute times (envelopes, the aliasing limiter) are not, so these checks use
FS = 88200 throughout.

run(source, driver, cases, workdir): builds src/engine/<source>.c (-O2, as build.sh) + tramp.S + test/<driver>.c in one
Docker container and runs every case (a list of argument strings for the driver) under bfin-elf-run, 8 at a time.
load(workdir, name) -> (samples, info dict from the driver's stderr "key value ..." line).
"""
import os
import subprocess

import numpy as np

FS = 88200
HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(HERE)


def inc(f):
    return round(f / FS * 2**32)


def run(source, driver, cases, workdir):
    cmds = [f"bfin-elf-run /tmp/m {args} > /w/{name}.txt 2> /w/{name}.err" for name, args in cases.items()]
    open(os.path.join(workdir, "cmds"), "w").write("\n".join(cmds) + "\n")
    script = "set -e; cd /tmp\n" \
        "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror " \
        f"-c -o engine.o /src/{source}.c\n" \
        "bfin-elf-as -o tramp.o /src/tramp.S\n" \
        f"bfin-elf-gcc -std=gnu99 -mcpu=bf524 -msim -O1 -Wall -Wextra -Werror -o m /src/test/{driver}.c tramp.o " \
        "engine.o\n" \
        "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n"
    open(os.path.join(workdir, "run.sh"), "w").write(script)
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{ENGINE}:/src:ro",
                    "-v", f"{workdir}:/w", "op1-bfin", "sh", "/w/run.sh"], check=True)


def load(workdir, name):
    x = np.loadtxt(os.path.join(workdir, name + ".txt"), dtype=np.int64, ndmin=1)
    f = open(os.path.join(workdir, name + ".err")).read().split()
    info = {}
    for i in range(0, len(f) - 1):
        if not f[i].lstrip("-").isdigit() and f[i] not in info:
            try:
                info[f[i]] = int(f[i + 1], 16) if f[i] == "guard" else int(float(f[i + 1]))
            except ValueError:
                pass
    return x, info


def spectrum(x, skip=4):
    """Hann-windowed amplitude spectrum (a full-scale sine of amplitude A reads A) of x after its first 1/skip."""
    x = x[len(x) // skip:].astype(float)
    win = np.hanning(len(x))
    mag = np.abs(np.fft.rfft(x * win)) / (win.sum() / 2)
    return np.fft.rfftfreq(len(x), 1 / FS), mag


def harmonics(x, f0, n, skip=4):
    """Amplitudes of harmonics 1..n of f0 (Hz), flat-top window: no scalloping error (< 0.01 dB) wherever the
    harmonic falls between bins. A sine of amplitude A reads A."""
    x = x[len(x) // skip:].astype(float)
    t = np.arange(len(x)) / len(x)
    win = 0.21557895 - 0.41663158 * np.cos(2 * np.pi * t) + 0.277263158 * np.cos(4 * np.pi * t) \
        - 0.083578947 * np.cos(6 * np.pi * t) + 0.006947368 * np.cos(8 * np.pi * t)
    mag = np.abs(np.fft.rfft(x * win)) / (win.sum() / 2)
    fr = np.fft.rfftfreq(len(x), 1 / FS)
    df = fr[1]
    return np.array([mag[max(0, int(round(k * f0 / df)) - 2):int(round(k * f0 / df)) + 3].max() for k in range(1, n + 1)])


def level_at(fr, m, f, bw=8.0):
    sel = np.abs(fr - f) <= bw
    return m[sel].max() if sel.any() else 0.0


def db(a, ref):
    return 20 * np.log10(max(a, 1e-9) / max(ref, 1e-12))


def alias_ratio(x, f0, band=20000, tol=20):
    """Energy below `band` Hz away from the harmonics of f0, relative to the energy on them, dB (ignoring < 20 Hz).
    At 88.2 kHz only what lands below ~22 kHz survives TE's 2:1 decimation, so that is the band that counts."""
    fr, m = spectrum(x)
    keep = (fr >= 20) & (fr <= band)
    harm = np.zeros_like(m, dtype=bool)
    for n in range(1, int(band / f0) + 2):
        harm |= np.abs(fr - n * f0) <= tol
    off = (m[keep & ~harm] ** 2).sum()
    on = (m[keep & harm] ** 2).sum()
    return 10 * np.log10(max(off, 1e-12) / max(on, 1e-12))
