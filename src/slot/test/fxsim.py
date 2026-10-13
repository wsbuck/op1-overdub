"""Shared simulator runner and analysis helpers for our FX checks (ensemble_check.py, ...).

run_cases(source, cases) builds src/slot/<source>.c with the device flags and test/fx_main.c (-DFX=<source>) for
the Blackfin simulator, runs every case under bfin-elf-run in one Docker container, 8 at a time, and returns
({name: int64 array [frames, channels]}, {name: (max, mean) CYCLES per 64-frame process call}).
A case is (mode "m"/"s", k0, k1, k2, k3, blocks, input Hz[, switch block, j0, j1, j2, j3]); the input is a
-6 dBFS sine on every channel (R a quarter period later), see fx_main.c. If test/<source>_ram.c exists it is
linked too: it defines the FX's FREE-SDRAM buffer (`<source>_ram`, a fixed address on the device; fill it with
garbage to prove the FX does not rely on zeroed memory).
"""
import os
import shutil
import subprocess
import tempfile

import numpy as np

FS = 44100
HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)


def run_cases(source, cases, keep=None):
    w = tempfile.mkdtemp(prefix=f"fx_{source}_")
    cmds = []
    for name, (mode, k0, k1, k2, k3, blocks, f, *sw) in cases.items():
        args = f"{mode} {k0} {k1} {k2} {k3} {blocks} {int(f * 1000)}" + (" " + " ".join(map(str, sw)) if sw else "")
        cmds.append(f"bfin-elf-run /tmp/m {args} > /w/{name}.txt 2> /w/{name}.err")
    open(os.path.join(w, "cmds"), "w").write("\n".join(cmds) + "\n")
    open(os.path.join(w, "run.sh"), "w").write(
        "set -e; cd /tmp\n"
        "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror "
        f"-c -o fx.o /src/{source}.c\n"
        f"bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -DFX={source} -o m /src/test/fx_main.c fx.o"
        + (f" /src/test/{source}_ram.c" if os.path.exists(os.path.join(HERE, f"{source}_ram.c")) else "") + "\n"
        "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v", f"{w}:/w",
                    "op1-bfin", "sh", "/w/run.sh"], check=True)
    out, cyc = {}, {}
    for name in cases:
        x = np.loadtxt(os.path.join(w, name + ".txt"), dtype=np.int64)
        out[name] = x if x.ndim == 2 else x[:, None]
        e = open(os.path.join(w, name + ".err")).read().split()
        cyc[name] = (int(e[2]), int(e[4]))
    if keep:
        os.makedirs(keep, exist_ok=True)
        for f in os.listdir(w):
            if f.endswith((".txt", ".err")):
                shutil.copy(os.path.join(w, f), keep)
    shutil.rmtree(w)
    return out, cyc


def sine_in(n, f, quarter=False):
    """The driver's input (fx_main.c sine_q15 >> 1), bit exact."""
    inc = int(f * 1000 / 1000.0 / 44100.0 * 4294967296.0) & 0xFFFFFFFF
    ph = (np.arange(n, dtype=np.uint64) * inc) & 0xFFFFFFFF
    if quarter:
        ph = (ph - 0x40000000) & 0xFFFFFFFF
    x = (ph.astype(np.uint32).view(np.int32) >> 16).astype(np.int64)
    y = (x * (32767 - np.abs(x))) >> 13
    return (y.astype(np.int16) >> 1).astype(np.int64)


def phase_dev(x, f):
    """Phase deviation (radians) of a sine at f: analytic signal, unwrapped, linear trend removed."""
    x = x.astype(float)
    n = len(x)
    s = np.fft.fft(x)
    h = np.zeros(n)
    h[0] = 1
    h[1:(n + 1) // 2] = 2
    if n % 2 == 0:
        h[n // 2] = 1
    a = np.fft.ifft(s * h)
    ph = np.unwrap(np.angle(a))
    t = np.arange(n)
    ph -= np.polyval(np.polyfit(t, ph, 1), t)
    cut = n // 20                                    # edges of the Hilbert transform
    return ph[cut:-cut]


def peak_hz(sig, lo, hi):
    m = np.abs(np.fft.rfft(sig * np.hanning(len(sig)), 16 * len(sig)))      # zero-padded: fine peak position
    fr = np.fft.rfftfreq(16 * len(sig), 1 / FS)
    sel = (fr > lo) & (fr < hi)
    return fr[sel][np.argmax(m[sel])]


def rms_db(x):
    x = x.astype(float)
    return 20 * np.log10(np.sqrt((x ** 2).mean()) + 1e-9)
