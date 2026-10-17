#!/usr/bin/env python3
"""Simulator checks for the metal drum engine (src/slot/metal.c) before flashing.

  python3 src/engine/test/metal_check.py [--keep DIR] [--level-comp]

Builds metal.c with the device flags and metal_main.c (TE's drum voice loop in miniature: slot init, note-on, render,
active, note-off, each through its C_ENTRY wrapper with fx_abi.S's register sentinels) and runs every case under
bfin-elf-run at 88.2 kHz, once with the asm loops and once with metal.c's C loops (built without __bfin__: the
reference, bit for bit). The knob curves come from src/slot/gen_metal.py. Checks:
  pitch (K0, the lowest mode) per semitone and at the range's ends; the mode ratios of every material (and SPREAD's
  stretch); the ring's T60 (DECAY) and the exciter's T60 (NDECAY), each independent of the other knob; feedback and
  fold stability over every DRIVE and GRIT detent (bounded, no DC, no runaway); zone joins on every zoned knob (no
  step, no level jump); slow sweeps and fast turns of the plain knobs (no zipper); knob changes and retriggers without
  clicks; velocity; every factory kit key's level (no clipping, none silent); the choke group (keys 10..13); the voice's
  end; cycles per render call and 12 voices at once; no runtime division or other libgcc calls; slot init; the ABI.
--level-comp: measure DRIVE's level per table point and write it into gen_metal.py (LV), then rerun gen_metal.py.
Exits non-zero on a failure.
"""
import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(HERE)
SLOT = os.path.join(os.path.dirname(ENGINE), "slot")
sys.path.insert(0, SLOT)
import gen_metal as G  # noqa: E402
from simrun import CPU_BUDGET  # noqa: E402

FS = 88200
PITCH, MATERIAL, DRIVE, DECAY, STRIKE, SPREAD, GRIT, NDECAY = range(8)
POLY_KITS = 255000                         # 12 voices, sim cycles per block: each kit's 12 heaviest keys
POLY_WORST = 280000                        # .. every heavy feature at once on all 12 (SCREAM + SCRAPE + ALIAS)


def K(hz=220, mat=G.B, drive=0, decay=4.0, strike=20, spread=16, grit=0, ndecay=0.01, raw=None):
    k = G.key(hz, mat, drive, decay, strike, spread, grit, ndecay)
    k = [min(32767, max(-32768, v)) for v in k]
    for j, v in (raw or {}).items():
        k[j] = v
    return k


def unclamped(hz=220, mat=G.B, drive=0, decay=4.0, strike=20, spread=16, grit=0, ndecay=0.01):
    """a key outside the stock dbox ranges (the knobs reach them; factory kits don't)"""
    return [G.k0_of(hz), mat, int(round(drive * 1024)), G.k_expo(decay, *G.DECAY), int(round(strike * 1024)),
            int(round(spread * 1024)), int(round(grit * 1024)), G.k_expo(ndecay, *G.NDECAY)]


JOINS = [(STRIKE, d) for d in G.mt_strike_edges()[1:-1]] + [(DRIVE, d) for d in G.mt_drive_edges()[1:-1]] \
    + [(GRIT, d) for d in G.mt_grit_edges()[1:-1]] + [(SPREAD, 16), (SPREAD, 17)] + [(MATERIAL, d) for d in (8, 16, 24)]


def join_raw(kz, d):
    """the knob value where a zone starts playing (the zoned knobs play one detent ahead)"""
    name = {STRIKE: "STRIKE", DRIVE: "DRIVE", GRIT: "GRIT"}.get(kz)
    return G.raw_of_pos(d * 1024, name) if name else d * 1024


def hf_burst(x, b):
    """a click: energy above 12 kHz in the 3 ms around block b's start against the most of the windows before and
    after it (dB)"""
    def e(i):
        y = np.asarray(x[i - 128:i + 128], float)
        fr = np.fft.rfftfreq(256, 1 / FS)
        m = np.abs(np.fft.rfft(y * np.hanning(256)))
        return float((m[fr > 12000] ** 2).sum()) + 1.0
    at = e(b * 128)
    around = max(e(i * 128) for i in list(range(b - 6, b - 1)) + list(range(b + 2, b + 7)))
    return 10 * math.log10(at / around)


def cases():
    c = {}
    for s in (-36, -24, -12, 0, 7, 12, 24, 36):            # pitch: semitones from A3, the lowest mode
        c[f"pitch{s}"] = (0, 127, 400, K(raw={PITCH: s * 512}, decay=8.0))
    c["pitch_min"] = (0, 127, 700, K(raw={PITCH: G.K0_MIN}, decay=16.0, strike=12))
    c["pitch_max"] = (0, 127, 300, K(raw={PITCH: G.STOCK_HI[0]}, decay=8.0))
    for m, kv in zip(G.MATERIALS, (G.B, G.PL, G.BE, G.PI, G.SH)):   # the modes of every material
        c[f"mat_{m}"] = (0, 127, 1200, K(hz=110, mat=kv, decay=16.0, strike=23.9))
    for sp in (0, 8, 24, 32):
        c[f"spread{sp}"] = (0, 127, 1200, K(hz=110, mat=G.PL, decay=16.0, strike=23.9, spread=sp))
    for d in (0.1, 0.5, 2.0):                               # the ring's T60 (the exciter short)
        c[f"ring{d}"] = (0, 127, int(d * 1.6 * FS / 128) + 100, K(hz=220, decay=d, strike=12, ndecay=0.002))
    c["ring0.5_long_ex"] = (0, 127, 450, K(hz=220, decay=0.5, strike=12, ndecay=0.3))
    for d in (0.05, 0.3, 1.0):                              # the exciter's T60 (the ring short)
        c[f"ex{d}"] = (0, 127, int(d * 1.6 * FS / 128) + 100, unclamped(hz=220, decay=0.03, strike=23, ndecay=d))
    c["ex0.3_long_ring"] = (0, 127, 450, K(hz=220, decay=4.0, strike=23, ndecay=0.3))
    # stability: sustained notes through every DRIVE detent, low and high, two materials; GRIT's detents
    for d in range(33):
        for hz, m in ((55, G.B), (880, G.SH)):
            c[f"drv{d}_{hz}"] = (0, 127, 300, K(hz=hz, mat=m, drive=min(d, 31.99), decay=16.0, strike=20))
    for gd in range(0, 33, 2):
        for drv in (0, 32):
            c[f"grit{gd}_{drv}"] = (0, 127, 200, K(hz=220, mat=G.PL, drive=min(drv, 31.99), decay=16.0, strike=20,
                                                   grit=min(gd, 31.99)))
    for sd in (0, 4, 8, 12, 16, 20, 24, 26, 28, 30, 32):    # every STRIKE zone, clean and screaming
        for drv in (0, 32):
            c[f"strike{sd}_{drv}"] = (0, 127, 300, K(hz=110, mat=G.PL, drive=min(drv, 31.99), decay=2.0,
                                                     strike=min(sd, 31.99), ndecay=0.3))
    # zone joins: one raw step across each zone edge mid-note (block 40), against the same note without the step
    for kz, d in JOINS:
        kv = join_raw(kz, d)
        base = K(hz=220, mat=G.PL, drive=6, decay=4.0, strike=12, ndecay=0.5)
        base[kz] = kv - 1
        c[f"join_{kz}_{d}"] = (0, 127, 80, base + ["chg", 40, kz, kv])
        c[f"joinref_{kz}_{d}"] = (0, 127, 80, list(base))
    # slow sweeps of the plain knobs (TE's LFO reaches K0..K3), fast turns
    for kz, lo, hi in ((PITCH, -12288, 12288), (MATERIAL, 0, 32767), (DRIVE, 0, 32767), (DECAY, 16384, 32767)):
        c[f"sweep_{kz}"] = (0, 127, 1200, K(hz=220, mat=G.PL, drive=4, decay=16.0, strike=12)
                            + ["ramp", 50, 1150, kz, lo, hi])
        c[f"fast_{kz}"] = (0, 127, 160, K(hz=220, mat=G.PL, drive=4, decay=16.0, strike=12)
                           + ["ramp", 40, 72, kz, lo, hi])
    for kz, v0 in ((STRIKE, 12), (SPREAD, 16), (GRIT, 0), (NDECAY, 20)):   # fast turns: a detent per block, 6 blocks
        k = K(hz=220, mat=G.PL, drive=4, decay=4.0, strike=12, ndecay=0.5)
        k[kz] = v0 * 1024
        c[f"chg_{kz}"] = (0, 127, 80, k + ["ramp", 40, 46, kz, v0 * 1024, (v0 + 6) * 1024])
    c["vel64"] = (0, 64, 60, K(hz=220, decay=0.5, strike=12))
    c["vel127"] = (0, 127, 60, K(hz=220, decay=0.5, strike=12))
    c["retrig"] = (0, 127, 80, K(hz=110, mat=G.PL, decay=1.0, strike=2, ndecay=0.1) + ["retrig", 30])
    c["retrig_scream"] = (0, 127, 80, K(hz=110, mat=G.PL, drive=30, decay=1.0, strike=12) + ["retrig", 30])
    c["end_short"] = (0, 127, 400, K(hz=440, decay=0.1, strike=16, ndecay=0.01))
    c["end_low"] = (0, 127, 400, K(hz=40, decay=0.1, strike=4, ndecay=0.05))
    for z, kk in (("clean", K(hz=110, decay=4.0, strike=12)), ("scream", K(hz=110, drive=30, decay=4.0, strike=12))):
        c[f"reinit_{z}"] = (0, 127, 80, kk + ["reinit", 30])
        c[f"fresh_{z}"] = (0, 127, 50, kk)
    c["worst"] = (23, 127, 30, unclamped(hz=110, mat=G.PL, drive=31.99, decay=16.0, strike=27, grit=31.99, ndecay=4.0))
    c["worst_boom"] = (23, 127, 30, unclamped(hz=110, mat=G.PL, drive=31.99, decay=16.0, strike=0, grit=31.99,
                                              ndecay=4.0))
    for name, (_, kit) in G.KITS.items():
        for k, row in enumerate(kit):
            c[f"kit_{name.replace(' ', '_')}_{k}"] = (k, 127, 500, list(row))
    rng = np.random.default_rng(86)         # random keys, knob turns and retriggers (the asm against the C loops)
    for j in range(48):
        k = [int(rng.integers(-24320, 24833))] + [int(rng.integers(0, 32768)) for _ in range(7)]
        extra = []
        for _ in range(int(rng.integers(1, 4))):
            b0 = int(rng.integers(0, 60))
            extra += ["ramp", b0, b0 + int(rng.integers(1, 40)), int(rng.integers(0, 8)), int(rng.integers(0, 32768)),
                      int(rng.integers(0, 32768))]
        extra += ["retrig", int(rng.integers(5, 90)), "chg", int(rng.integers(1, 99)), DRIVE, int(rng.integers(0, 32768))]
        c[f"fuzz{j}"] = (int(rng.integers(0, 24)), int(rng.integers(1, 128)), 100, k + extra)
    return c


OPEN = K(hz=1800, mat=G.SH, drive=14, decay=0.8, strike=22, spread=20, ndecay=0.1)
CLOSED = K(hz=1800, mat=G.SH, drive=14, decay=0.06, strike=22, spread=20, ndecay=0.02)
CHOKE = {                                   # (ringing key, hit key, values, values): voice 3 rings, voice 4 hits
    "choke_hat": (13, 10, OPEN, CLOSED),
    "choke_same": (13, 13, OPEN, OPEN),
    "choke_low": (10, 12, OPEN, CLOSED),
    "choke_outside": (13, 14, OPEN, CLOSED),             # key 14 is not in the group
    "choke_victim": (14, 10, OPEN, CLOSED),              # a key outside the group rings on
}


def poly_cases():
    worst = unclamped(hz=110, mat=G.PL, drive=31.99, decay=16.0, strike=27, grit=31.99, ndecay=4.0)
    boom = unclamped(hz=110, mat=G.PL, drive=31.99, decay=16.0, strike=0, grit=31.99, ndecay=4.0)
    clip = unclamped(hz=110, mat=G.SH, drive=20.9, decay=16.0, strike=4, grit=31.99, ndecay=4.0)
    c = {"poly_worst": [worst] * 12, "poly_boom": [boom] * 12, "poly_clip": [clip] * 12,
         "poly_mixed": [worst] * 4 + [boom] * 4 + [clip] * 4}

    def weight(k):
        return (k[DRIVE] >= 21 * 1024) * 3 + (k[DRIVE] >= 1024) + (k[GRIT] > 0) * 2 + (k[STRIKE] >= 24 * 1024) * 2
    for name, (_, kit) in G.KITS.items():
        c[f"poly_{name.replace(' ', '_')}"] = [list(r) for r in sorted(kit, key=weight, reverse=True)[:12]]
    return c


def rms_db(x):
    return 20 * math.log10(max(float(np.sqrt(np.mean(np.asarray(x, float) ** 2))), 1e-3) / 32768)


def spectrum(x):
    x = np.asarray(x, float)
    w = np.hanning(len(x))
    n = 1 << max(16, int(math.ceil(math.log2(len(x)))))
    m = np.abs(np.fft.rfft(x * w, n)) / (w.sum() / 2)
    return np.fft.rfftfreq(n, 1 / FS), m


def peak_near(x, f, tol=0.03):
    """the frequency of the strongest peak within f (1 +- tol), parabolic interpolation"""
    fr, m = spectrum(x)
    sel = np.nonzero((fr > f * (1 - tol)) & (fr < f * (1 + tol)))[0]
    i = sel[int(np.argmax(m[sel]))]
    a, b, c_ = m[i - 1], m[i], m[i + 1]
    d = 0.5 * (a - c_) / (a - 2 * b + c_) if a - 2 * b + c_ else 0.0
    return (i + d) * (fr[1] - fr[0]), b


def band_env(x, f0, f1, hop=441, n=4096):
    """energy (dB) in f0..f1 per hop"""
    x = np.asarray(x, float)
    w = np.hanning(n)
    fr = np.fft.rfftfreq(n, 1 / FS)
    sel = (fr >= f0) & (fr <= f1)
    out = []
    for i in range(0, len(x) - n, hop):
        m = np.abs(np.fft.rfft(x[i:i + n] * w))
        out.append(10 * math.log10(float((m[sel] ** 2).sum()) + 1e-9))
    return np.array(out), hop / FS


def t60_band(x, f0, f1, start_db=-5, span=30):
    """T60 from the band's energy: the slope between start_db and start_db - span below its peak, x 60 / span"""
    e, dt = band_env(x, f0, f1)
    p = int(np.argmax(e))
    top = e[p]
    a = next((i for i in range(p, len(e)) if e[i] < top + start_db), None)
    b = next((i for i in range(p, len(e)) if e[i] < top + start_db - span), None)
    if a is None or b is None or b <= a:
        return float("inf")
    t = np.arange(a, b + 1) * dt
    slope = np.polyfit(t, e[a:b + 1], 1)[0]                   # dB per s
    return -60.0 / slope if slope < 0 else float("inf")


def edge_zipper(x, a, b):
    d = np.abs(np.diff(np.asarray(x[a * 128:b * 128], float)))
    edge = d[127::128]
    inner = np.delete(d, np.arange(127, len(d), 128))
    return float(np.percentile(edge, 99.5) / max(np.percentile(inner, 99.5), 1.0))


def win_jump(x, a, b, win=48, t60=None):
    slope = 60 / t60 * win * 128 / FS if t60 else 0.0
    e = [rms_db(x[i * 128:(i + win) * 128]) + slope * j for j, i in enumerate(range(a, b - win, win))]
    return max(abs(e[i + 1] - e[i]) for i in range(len(e) - 1))


def build_and_run(cs, extra_cmds, w):
    cflags = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
    cmds = [f"bfin-elf-run /tmp/m {k} {v} {b} {' '.join(map(str, p))} > /w/{n}.bin 2> /w/{n}.err; echo $? > /w/{n}.rc"
            for n, (k, v, b, p) in cs.items()]
    cmds += [f"bfin-elf-run /tmp/mc {k} {v} {b} {' '.join(map(str, p))} > /w/{n}.cref 2> /dev/null"
             for n, (k, v, b, p) in cs.items()]
    cmds += extra_cmds
    open(os.path.join(w, "cmds"), "w").write("\n".join(cmds) + "\n")
    open(os.path.join(w, "run.sh"), "w").write(
        "set -e; cd /tmp\n"
        f"bfin-elf-gcc {cflags} -I /engine -I /src -c -o mt.o /src/metal.c\n"
        "bfin-elf-nm -u mt.o > /w/undef.txt\n"
        "bfin-elf-size mt.o > /w/size.txt\n"
        "bfin-elf-as -I /src --defsym NO_FX=1 -o abi.o /src/test/fx_abi.S\n"
        "bfin-elf-as -I /src -o entries.o /engine/test/metal_abi.S\n"
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -I /src -I /engine -o m /engine/test/metal_main.c mt.o abi.o entries.o\n"
        f"bfin-elf-gcc {cflags} -U__bfin__ -I /engine -I /src -c -o mtc.o /src/metal.c\n"
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -I /src -I /engine -o mc /engine/test/metal_main.c mtc.o abi.o entries.o\n"
        "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v",
                    f"{ENGINE}:/engine:ro", "-v", f"{w}:/w", "op1-bfin", "sh", "/w/run.sh"], check=True)


def load(w, cs):
    res = {}
    for n in cs:
        err = open(os.path.join(w, n + ".err")).read()
        f = err.split()
        x = np.fromfile(os.path.join(w, n + ".bin"), dtype="<i2").astype(np.int64)
        res[n] = dict(x=x, cyc=int(f[2]), mean=int(f[4]), guard=f[6], vptr=f[8], end=int(f[10]),
                      rc=open(os.path.join(w, n + ".rc")).read().strip(), err=err)
    return res


def level_comp():
    """DRIVE's level per table point: three reference strikes, RMS over their first 0.25 s, matched to DRIVE 0's"""
    w = tempfile.mkdtemp(prefix="metal_lv_")
    refs = [K(hz=110, mat=G.B, decay=1.0, strike=12), K(hz=220, mat=G.PL, decay=1.0, strike=18),
            K(hz=660, mat=G.SH, decay=0.6, strike=22)]
    cs = {}
    for i in range(129):
        k2 = min(i * 256, 32767)
        for j, r in enumerate(refs):
            cs[f"lv{i}_{j}"] = (0, 127, 180, r[:DRIVE] + [k2] + r[DRIVE + 1:])
    G.LV[:] = [4096] * 129                  # measure with level 1.0
    open(os.path.join(SLOT, "metal_tables.h"), "w").write(G.tables_h())
    try:
        build_and_run(cs, [], w)
        res = load(w, cs)
    finally:
        pass
    base = [rms_db(res[f"lv0_{j}"]["x"][:FS // 4]) for j in range(len(refs))]
    lv = []
    for i in range(129):
        d = np.mean([rms_db(res[f"lv{i}_{j}"]["x"][:FS // 4]) - base[j] for j in range(len(refs))]) - G.LEVEL_LIFT_DB(i)
        lv.append(int(round(min(8191, max(256, 4096 * 10 ** (-d / 20))))))
    lv[0] = 4096
    src = open(os.path.join(SLOT, "gen_metal.py")).read()
    rows = ",\n      ".join(", ".join(str(v) for v in lv[i:i + 16]) for i in range(0, 129, 16))
    src = re.sub(r"LV = \[[^\]]*\]( \* 129)?", f"LV = [{rows}]", src, count=1)
    open(os.path.join(SLOT, "gen_metal.py"), "w").write(src)
    subprocess.run([sys.executable, os.path.join(SLOT, "gen_metal.py")], check=True)
    print("LV written:", lv)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep")
    ap.add_argument("--level-comp", action="store_true")
    a = ap.parse_args()
    if a.level_comp:
        level_comp()
        return
    w = tempfile.mkdtemp(prefix="metal_")
    cs = cases()
    extra = ["bfin-elf-run /tmp/m init > /w/init.txt 2> /w/init.err; echo $? > /w/init.rc"]
    for n, (k1, k2, p1, p2) in CHOKE.items():
        extra.append(f"bfin-elf-run /tmp/m choke {k1} {k2} 20 200 {' '.join(map(str, p1 + p2))} > /w/{n}.bin "
                     f"2> /w/{n}.err; echo $? > /w/{n}.rc")
    for n, rows in poly_cases().items():
        extra.append(f"bfin-elf-run /tmp/m poly 60 {len(rows)} {' '.join(str(v) for r in rows for v in r)} > /w/{n}.bin "
                     f"2> /w/{n}.err; echo $? > /w/{n}.rc")
    build_and_run(cs, extra, w)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    res = load(w, cs)

    # ---- build, ABI, cost ----
    undef = set(open(os.path.join(w, "undef.txt")).read().split()) - {"U"}
    check(undef <= {"_te_drum_key", "_te_drum_vel", "_ranges", "_metal_id"},
          f"no runtime division, libc or other libgcc calls (undefined: {', '.join(sorted(undef))})")
    print("  " + open(os.path.join(w, "size.txt")).read().strip().replace("\n", "\n  "))
    abi = all(r["rc"] == "0" for r in res.values())
    check(abi, f"ABI: every entry call preserves R4-R7, P3-P5, FP, SP, loops, L ({len(cs)} cases)"
          + ("" if abi else " (" + "; ".join(r["err"].strip() for r in res.values() if r["rc"] != "0")[:300] + ")"))
    check(all(r["guard"] == "5a5a5a5a" for r in res.values()), "output guard intact (128 samples per call)")
    check(all(r["vptr"] == "0190f8a4" for r in res.values()), "TE's vptr never written")
    worst = max(r["cyc"] for r in res.values())
    who = max(res, key=lambda n: res[n]["cyc"])
    check(worst <= CPU_BUDGET, f"cycles per render call: max {worst} = {worst / 128:.0f} per sample ({who}; budget "
          f"{CPU_BUDGET})")
    same = [n for n in cs if np.array_equal(np.fromfile(os.path.join(w, n + ".cref"), dtype="<i2"), res[n]["x"])]
    check(len(same) == len(cs), f"the asm loops render bit for bit what metal.c's C loops do ({len(same)} of {len(cs)} "
          "cases)" + ("" if len(same) == len(cs) else ": not " + ", ".join(sorted(set(cs) - set(same))[:8])))

    # ---- pitch: the lowest mode ----
    errs = []
    for s in (-36, -24, -12, 0, 7, 12, 24, 36):
        f0 = 220 * 2 ** (s / 12)
        f, _ = peak_near(res[f"pitch{s}"]["x"][FS // 10:FS // 10 + 2 ** 16], f0)
        errs.append(abs(1200 * math.log2(f / f0)))
    check(max(errs) < 2, f"PITCH: the lowest mode at 512 per semitone from A3 = 220 Hz, -36..+36 semitones: worst "
          f"{max(errs):.2f} cents ({' '.join(f'{e:.2f}' for e in errs)})")
    lo = G.pitch_hz(G.K0_MIN)
    flo, _ = peak_near(res["pitch_min"]["x"][FS // 4:FS // 4 + 2 ** 18], lo, 0.05)
    hi = G.pitch_hz(G.STOCK_HI[0])
    fhi, _ = peak_near(res["pitch_max"]["x"][FS // 10:FS // 10 + 2 ** 15], hi)
    check(abs(1200 * math.log2(flo / lo)) < 5 and abs(1200 * math.log2(fhi / hi)) < 3,
          f"PITCH's range: {flo:.2f} Hz (want {lo:.2f}) .. {fhi:.0f} Hz (want {hi:.0f})")

    # ---- the materials' mode ratios, SPREAD ----
    worst_c, report = 0.0, []
    for m in G.MATERIALS:
        r = G.MAT[m][0]
        x = res[f"mat_{m}"]["x"][FS // 20:FS // 20 + 2 ** 17]
        cents = []
        for j, rr in enumerate(r):
            near = min(abs(math.log(rr / o)) for i, o in enumerate(r) if i != j)
            f = 110 * rr
            if near < 0.03 or f > G.MODE_FADE:
                continue
            fm, _ = peak_near(x, f, 0.012)
            cents.append(abs(1200 * math.log2(fm / f)))
        worst_c = max(worst_c, max(cents))
        report.append(f"{m} {len(cents)} modes {max(cents):.1f}")
    check(worst_c < 6, "MATERIAL: each material's modes at its ratios (110 Hz, modes > 3 % from a neighbour): "
          + ", ".join(report) + " cents worst")
    worst_s, rep = 0.0, []
    for sp in (0, 8, 24, 32):
        s_, det = G.spread_point(sp * 1024)
        r = G.MAT["PLATE"][0]
        x = res[f"spread{sp}"]["x"][FS // 20:FS // 20 + 2 ** 17]
        cents = []
        for j, rr in enumerate(r):
            want = 110 * rr ** s_ * 2 ** (G.DETUNE[j] * det / 1200)
            others = [110 * o ** s_ * 2 ** (G.DETUNE[i] * det / 1200) for i, o in enumerate(r) if i != j]
            if min(abs(math.log(want / o)) for o in others) < 0.03 or want > G.MODE_FADE:
                continue
            fm, _ = peak_near(x, want, 0.012)
            cents.append(abs(1200 * math.log2(fm / want)))
        worst_s = max(worst_s, max(cents) if cents else 0)
        rep.append(f"{sp}: x{s_:.2f} {len(cents)} modes")
    check(worst_s < 8, f"SPREAD: ratios^stretch (+ the detune toward the ends), PLATE: {', '.join(rep)}; worst "
          f"{worst_s:.1f} cents")

    # ---- the ring's and the exciter's T60, independent ----
    ring_err = []
    for d in (0.1, 0.5, 2.0):
        k = cs[f"ring{d}"][3]
        want = G.expo(k[DECAY], *G.DECAY)
        got = t60_band(res[f"ring{d}"]["x"], 200, 240)
        ring_err.append((want, got))
    g_long = t60_band(res["ring0.5_long_ex"]["x"], 200, 240)
    check(all(abs(g / w_ - 1) < 0.15 for w_, g in ring_err) and abs(g_long / ring_err[1][1] - 1) < 0.1,
          "DECAY: the lowest mode's T60 " + ", ".join(f"{g:.3f} s (want {w_:.3f})" for w_, g in ring_err)
          + f"; with NDECAY 0.3 s instead of 2 ms: {g_long:.3f} s")
    ex_err = []
    for d in (0.05, 0.3, 1.0):
        k = cs[f"ex{d}"][3]
        want = G.expo(k[NDECAY], *G.NDECAY)
        got = t60_band(res[f"ex{d}"]["x"], 8000, 16000, start_db=-8)
        ex_err.append((want, got))
    e_long = t60_band(res["ex0.3_long_ring"]["x"], 8000, 16000, start_db=-8)
    check(all(abs(g / w_ - 1) < 0.2 for w_, g in ex_err) and abs(e_long / ex_err[1][1] - 1) < 0.15,
          "NDECAY: the exciter's T60 (8..16 kHz, above the modes) " + ", ".join(f"{g:.3f} s (want {w_:.3f})"
                                                                           for w_, g in ex_err)
          + f"; with DECAY 4 s instead of 30 ms: {e_long:.3f} s")

    # ---- stability: every DRIVE and GRIT detent, every STRIKE zone ----
    worst_dc, runaway, clipped, who_dc, who_run = 0.0, -99.0, 0.0, "", ""
    names = [n for n in cs if n.startswith(("drv", "grit", "strike"))]
    for n in names:
        x = res[n]["x"].astype(float)
        seg = x[FS // 10:]
        alias = n.startswith("grit") and int(n[4:].split("_")[0]) >= 13
        dc = abs(seg.mean()) / max(np.sqrt((seg ** 2).mean()), 1)
        if dc > worst_dc and not alias:
            worst_dc, who_dc = dc, n
        win = FS // 5 if alias else FS // 20     # (ALIAS: its sample-and-hold beats, slow aliases, not a rise)
        e = [rms_db(x[i:i + win]) for i in range(FS // 20, len(x) - win, FS // 20)]
        r = max(e[i + 1] - e[i] for i in range(len(e) - 1)) if len(e) > 1 else 0
        r = r if not alias else max(e) - e[0]
        if r > runaway:
            runaway, who_run = r, n
        clipped = max(clipped, float((np.abs(x) >= 32700).mean()))
    check(worst_dc < 0.05 and runaway < 1.5 and clipped < 0.05,
          f"stable over {len(names)} sustained notes (DRIVE 0..32 at 55 / 880 Hz, GRIT 0..32 clean and at the top, "
          f"every STRIKE zone clean and screaming): DC at most {100 * worst_dc:.2f} % of the RMS ({who_dc}), the level "
          f"never rises (worst {runaway:+.2f} dB per 50 ms, {who_run}), at most {100 * clipped:.2f} % of samples at full "
          "scale")
    lv = {d: rms_db(res[f"drv{d}_55"]["x"][:FS // 4]) for d in range(33)}
    lv8 = {d: rms_db(res[f"drv{d}_880"]["x"][:FS // 4]) for d in range(33)}
    print("  DRIVE 0..32 RMS dBFS at 55 Hz: " + " ".join(f"{lv[d]:.1f}" for d in range(33)))
    print("  DRIVE 0..32 RMS dBFS at 880 Hz: " + " ".join(f"{lv8[d]:.1f}" for d in range(33)))
    spread_lv = max(max(lv.values()) - min(lv.values()), max(lv8.values()) - min(lv8.values()))
    check(spread_lv < 12, f"DRIVE's level across its 33 detents within {spread_lv:.1f} dB (55 and 880 Hz)")

    def flat(x):
        fr, m = spectrum(np.asarray(x, float))
        sel = (fr > 100) & (fr < 20000)
        p = m[sel] ** 2 + 1e-12
        return float(np.exp(np.mean(np.log(p))) / np.mean(p))
    fl = [flat(res[f"drv{d}_55"]["x"][FS // 10:FS // 10 + 32768]) for d in (0, 10, 20, 32)]
    check(fl[0] < fl[1] < fl[3] and fl[3] > 0.05, "DRIVE grows harsher by zone (spectral flatness at 55 Hz, CLEAN "
          f"{fl[0]:.4f} -> FOLD 10 {fl[1]:.4f} -> CLIP 20 {fl[2]:.4f} -> SCREAM 32 {fl[3]:.4f})")

    # ---- zone joins, sweeps, fast turns, changes ----
    knob = ['PITCH', 'MATERIAL', 'DRIVE', 'DECAY', 'STRIKE', 'SPREAD', 'GRIT', 'NDECAY']
    jres = []
    for kz, d in JOINS:
        x, ref = res[f"join_{kz}_{d}"]["x"].astype(float), res[f"joinref_{kz}_{d}"]["x"].astype(float)
        seg = slice(40 * 128, 43 * 128)         # (the first 4 ms: a step shows at once; later a frequency's drift)
        resid = 10 * math.log10(max(((x[seg] - ref[seg]) ** 2).sum(), 1e-9) / max((ref[seg] ** 2).sum(), 1e-9))
        jres.append((f"{knob[kz]} {d}", resid))
    check(all(r < -30 for _, r in jres), "zone joins: one raw step across each zone edge mid-note against the same note "
          "without it, the residual over the next 4 ms: " + ", ".join(f"{n} {r:.0f}" for n, r in jres) + " dB")
    for kz in (PITCH, MATERIAL, DRIVE, DECAY):
        x = res[f"sweep_{kz}"]["x"]
        t60 = None if kz == DECAY else G.expo(cs[f"sweep_{kz}"][3][DECAY], *G.DECAY)
        j = win_jump(x, 60, 1150, t60=t60) if kz != DECAY else 0.0
        z = edge_zipper(x, 50, 1150)
        zf = edge_zipper(res[f"fast_{kz}"]["x"], 40, 72)
        check(z < 1.3 and zf < 1.3 and j < 3.0, f"{['PITCH', 'MATERIAL', 'DRIVE', 'DECAY'][kz]}: a slow turn (1.6 s, "
              f"as TE's LFO): block-edge steps {z:.2f} x the in-block ones, neighbouring 70 ms windows within {j:.2f} dB; "
              f"a detent per block: {zf:.2f} x")
    clicks = [(n, hf_burst(res[n]["x"], b)) for n, b in [(f"chg_{kz}", 40) for kz in (STRIKE, SPREAD, GRIT, NDECAY)]
              + [("retrig", 30), ("retrig_scream", 30)]]
    check(all(v < 3 for _, v in clicks), "no clicks on fast turns (a detent per block, STRIKE / SPREAD / GRIT / NDECAY) or "
          "a retrigger (energy above 12 kHz around the change against the windows before and after): "
          + ", ".join(f"{n} {v:+.1f}" for n, v in clicks) + " dB")
    p127, p64 = np.abs(res["vel127"]["x"]).max(), np.abs(res["vel64"]["x"]).max()
    check(0.45 < p64 / p127 < 0.56, f"velocity 64 vs 127: level ratio {p64 / p127:.2f}")
    for z in ("clean", "scream"):
        x, f = res[f"reinit_{z}"]["x"][30 * 128:50 * 128].astype(float), res[f"fresh_{z}"]["x"][:20 * 128].astype(float)
        check(abs(x[0]) <= 256 and np.abs(np.diff(x[:64])).max() <= 2 * np.abs(np.diff(f[:64])).max() + 64,
              f"a kit loaded mid-note ({z}), then the key again: a new note from silence (first sample {x[0]:.0f})")
    for n in ("end_short", "end_low"):
        r = res[n]
        x = r["x"]
        tail = np.abs(x[(r["end"] - 1) * 128:(r["end"] + 1) * 128]).max() if r["end"] > 0 else 99999
        check(0 < r["end"] < 400 and tail < 64 and np.abs(x[(r["end"] + 1) * 128:]).max() == 0,
              f"{n}: the voice ends (active 0 after block {r['end']}), its last samples under {tail} LSB, then silence")

    # ---- the factory kits ----
    pk, nc, quiet, total = [], 0, [], 0
    for n in cs:
        if n.startswith("kit_"):
            x = res[n]["x"]
            pk.append((int(np.abs(x).max()), n))
            nc += int((np.abs(x) >= 32000).sum())
            total += len(x)
            if pk[-1][0] < 4000:
                quiet.append(n)
    rmsk = {n: rms_db(res[n]["x"][:FS // 5]) for n in cs if n.startswith("kit_")}
    check(not quiet and nc < total * 2e-4 and max(p for p, _ in pk) < 32767,
          f"factory kits ({len(pk)} keys): peaks {20 * math.log10(min(pk)[0] / 32768):.1f} ({min(pk)[1]}) to "
          f"{20 * math.log10(max(pk)[0] / 32768):.1f} dBFS ({max(pk)[1]}), {nc} samples >= 32000 of {total}"
          + (f"; quiet: {', '.join(quiet)}" if quiet else ""))
    for name in G.KITS:
        ks = [f"kit_{name.replace(' ', '_')}_{k}" for k in range(24)]
        print(f"  {name}: first 0.2 s RMS dBFS by key: " + " ".join(f"{rmsk[k]:.0f}" for k in ks))
    check(all(G.STOCK_LO[j] <= v <= G.STOCK_HI[j] for _, kit in G.KITS.values() for row in kit for j, v in enumerate(row)),
          "every factory value inside the stock dbox per-column range (K7 <= 32255)")

    # ---- the choke group ----
    ends = {n: int(open(os.path.join(w, n + ".err")).read().split()[1]) for n in CHOKE}
    ok_rc = all(open(os.path.join(w, n + ".rc")).read().strip() == "0" for n in CHOKE)
    x = np.fromfile(os.path.join(w, "choke_hat.bin"), dtype="<i2").astype(np.int64)
    d = np.abs(np.diff(x[20 * 128 - 4:22 * 128]))
    fr = np.fft.rfftfreq(1024, 1 / FS)

    def e300(y):
        m = np.abs(np.fft.rfft(np.asarray(y, float)[:1024] * np.hanning(1024)))
        return float((m[fr > 300] ** 2).sum()) + 1e-9
    gone = 10 * math.log10(e300(x[24 * 128:32 * 128]) / e300(x[12 * 128:20 * 128]))
    check(ok_rc and 20 <= ends["choke_hat"] <= 130 and 20 <= ends["choke_same"] <= 130 and 20 <= ends["choke_low"] <= 130
          and gone < -50 and ends["choke_outside"] < 0 and ends["choke_victim"] < 0
          and d.max() < 2 * np.abs(np.diff(x[18 * 128:20 * 128])).max(),
          f"choke: an open hat (key 13) is {gone:.0f} dB down 6 ms after a closed hit on 10 and ends "
          f"{ends['choke_hat'] - 20} blocks after it (with the DC blocker's tail, steal-first; the same key: "
          f"{ends['choke_same'] - 20}, 10 by 12: {ends['choke_low'] - 20}), no click; key 14 neither chokes nor is choked "
          f"(ends {ends['choke_outside']}, {ends['choke_victim']})")

    # ---- 12 voices ----
    out = []
    for n in poly_cases():
        f = open(os.path.join(w, n + ".err")).read().split()
        ok = open(os.path.join(w, n + ".rc")).read().strip() == "0"
        out.append((n, int(f[1]), int(f[3]), ok))
    print("  12 voices, cycles per block (sum of 12 render calls) / worst call: "
          + "; ".join(f"{n[5:]} {b} / {c}" for n, b, c, _ in out))
    kits = max(b for n, b, _, _ in out if n[5:].replace("_", " ") in G.KITS)
    synth = max(b for n, b, _, _ in out if n[5:].replace("_", " ") not in G.KITS)
    check(all(o[3] for o in out) and kits <= POLY_KITS and synth <= POLY_WORST,
          f"12 voices at once: each kit's 12 heaviest keys at most {kits} cycles per block (target {POLY_KITS}), every "
          f"heavy feature at once {synth} ({synth / 128 / 12:.0f} per sample per voice; allowed {POLY_WORST})")

    # ---- knob layout ----
    hdr = open(os.path.join(SLOT, "metal.h")).read()

    def hedges(name):
        return [int(v) for v in re.search(name + r"\[\d+\][^=]*= \{([^}]*)\}", hdr).group(1).split(",")]
    st, dr, gr = G.mt_strike_edges(), G.mt_drive_edges(), G.mt_grit_edges()
    check(st == hedges("mt_strike_edge") and dr == hedges("mt_drive_edge") and gr == hedges("mt_grit_edge"),
          f"zones start on detents, the same in gen_metal.py and metal.h: STRIKE {st}, DRIVE {dr}, GRIT {gr}")
    r = open(os.path.join(w, "init.txt")).read().split()
    check(open(os.path.join(w, "init.rc")).read().strip() == "0" and r == ["kept", "1", "row", "1", "norow", "1"],
          f"slot init: loaded per-key values never rewritten, row 9 copied only when sane ({' '.join(r)})")

    if a.keep:
        shutil.copytree(w, a.keep, dirs_exist_ok=True)
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
