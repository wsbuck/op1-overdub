"""Shared simulator runner and analysis helpers for engine checks (va_check.py, wt_check.py).

run_cases() builds src/engine/<source>.c (plus its generated data, if gen_<source>_data.py exists) with
tramp.S and engine_main.c, and runs every case under bfin-elf-run in one Docker container, 8 at a time.
A case is (k0, k1, k2, k3, f_start_hz, calls, f_end_hz or None[, extra driver args, e.g. "sampler"]).
"""
import os
import subprocess
import sys

import numpy as np

FS = 44100        # the engines run at 88.2 kHz; checks tuned at this rate keep it (grain_check sets 88200)
HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(HERE)
# Cycles per render call. CPULOAD (LOG.md) measured overload at ~0.96 M cycles/block for all render calls
# = ~1,250 cycles/sample/voice at 6 voices: that is 100%, not a budget. Design target: <= 600 (about half),
# and the device costs more than the sim (SDRAM table reads, screen redraws, FX).
CPU_BUDGET = 600 * 128


def inc(f):
    return round(f / FS * 2**32)


def run_cases(source, cases, workdir):
    gen = os.path.join(ENGINE, f"gen_{source}_data.py")
    data = ""
    if os.path.exists(gen):
        subprocess.run([sys.executable, gen, os.path.join(workdir, "engine_data.bin")], check=True)
        open(os.path.join(workdir, "data.S"), "w").write(
            '.section .rodata\n.align 4\n.global _engine_data\n_engine_data:\n.incbin "/w/engine_data.bin"\n')
        data = "bfin-elf-as -o data.o /w/data.S\n"
    asm = ""
    if os.path.exists(os.path.join(ENGINE, f"{source}_asm.S")):
        asm = f"bfin-elf-as -o asm.o /src/{source}_asm.S\n"
    cmds = []
    for name, (k0, k1, k2, k3, f0, calls, f1, *extra) in cases.items():
        args = f"{k0} {k1} {k2} {k3} {inc(f0)} {calls}" + (f" {inc(f1) if f1 else 0} {' '.join(extra)}" if f1 or extra else "")
        cmds.append(f"bfin-elf-run /tmp/m {args} > /w/{name}.txt 2> /w/{name}.err")
    open(os.path.join(workdir, "cmds"), "w").write("\n".join(cmds) + "\n")
    script = "set -e; cd /tmp\n" \
        "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror " \
        f"-c -o engine.o /src/{source}.c\n" \
        "bfin-elf-as -o tramp.o /src/tramp.S\n" + data + asm + \
        "bfin-elf-gcc -std=gnu99 -msim -O1 -o m /src/test/engine_main.c tramp.o engine.o" + \
        (" asm.o" if asm else "") + (" data.o" if data else "") + "\n" \
        "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n"
    open(os.path.join(workdir, "run.sh"), "w").write(script)
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{ENGINE}:/src:ro",
                    "-v", f"{workdir}:/w", "op1-bfin", "sh", "/w/run.sh"], check=True)


def load(workdir, name):
    """(samples, max cycles per call, guard hex) for one case."""
    x = np.loadtxt(os.path.join(workdir, name + ".txt"), dtype=np.int64)
    err = open(os.path.join(workdir, name + ".err")).read().split()
    return x, int(err[2]), err[6]


def spectrum(x):
    x = x[len(x) // 4:].astype(float)            # skip the start
    win = np.hanning(len(x))
    mag = np.abs(np.fft.rfft(x * win)) / (win.sum() / 2)
    return np.fft.rfftfreq(len(x), 1 / FS), mag


def level_at(freqs, mag, f, bw=6.0):
    sel = np.abs(freqs - f) <= bw
    return mag[sel].max() if sel.any() else 0.0


def db(a, ref):
    return 20 * np.log10(max(a, 1e-9) / ref)


def alias_ratio(x, f0, tol=15):
    """Energy away from the harmonics of f0 relative to energy on them, dB (ignoring < 20 Hz)."""
    fr, m = spectrum(x)
    m = np.where(fr < 20, 0, m)
    harm = np.zeros_like(m, dtype=bool)
    for n in range(1, int(FS / 2 / f0) + 1):
        harm |= np.abs(fr - n * f0) <= tol
    return 10 * np.log10(max((m[~harm] ** 2).sum(), 1e-12) / max((m[harm] ** 2).sum(), 1e-12))
