#!/usr/bin/env python3
"""Simulator checks for the guitar amp engine (src/engine/amp.c) before flashing.

  python3 src/engine/test/amp_check.py [--keep DIR] [--calibrate]

Builds amp.c + tramp.S with amp_main.c (several voices, scripted note-on / release / stop, TE IN from a file written
here) and runs every case under bfin-elf-run in one Docker container. Checks:
  gain staging   a -15 dBFS guitar note through every preset and drive corner: sensible loudness, no rail hits;
                 a 0 dBFS input: the limiter holds the peaks
  gate           opens above the threshold, stays closed between the thresholds (hysteresis), stays open there once
                 open, closes after the hold below the close threshold, and is always open at white 0
  drive          harmonic distortion rises with blue; clean is clean
  cab and tone   the measured response (white noise, clean drive) against gen_amp_tables.py's design
  ownership      one voice plays the input whatever the chord; handovers (new note, release, stop, steal) crossfade
                 without a step; the mix equals the single-voice reference within the crossfade's rounding;
                 TE calling render twice per block and a te_rx_sel that never toggles change nothing
  output guard, cycles per render call (CPU_BUDGET, simrun.py), the trampoline ABI.
--calibrate prints the loudness per drive setting with the compensation taken out, for COMP_DB in gen_amp_tables.py.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simrun import CPU_BUDGET, ENGINE  # noqa: E402

sys.path.insert(0, ENGINE)
import gen_amp_tables as gat  # noqa: E402

FI, FO = 44100, 88200
RNG = np.random.default_rng(7)


def dbfs(db):
    return 32767 * 10 ** (db / 20)


def guitar(f, db, seconds, period=2.0, floor_db=None):
    """Plucked note: harmonics 1..12 falling 6 dB per octave and decaying faster up high, re-plucked every period."""
    t = np.arange(int(seconds * FI)) / FI
    tp = t % period
    v = sum(np.sin(2 * np.pi * f * h * tp + 0.7 * h) / h * np.exp(-tp * h * 0.8) for h in range(1, 13))
    v *= np.exp(-tp / 0.6) * (1 - np.exp(-tp / 0.002))
    v *= dbfs(db) / np.abs(v).max()
    if floor_db is not None:
        v += RNG.normal(0, dbfs(floor_db), len(v))
    return v


def sine(f, db, seconds):
    t = np.arange(int(seconds * FI)) / FI
    return dbfs(db) * np.sin(2 * np.pi * f * t)


def write(workdir, name, v):
    np.clip(np.round(v), -32768, 32767).astype("<i2").tofile(os.path.join(workdir, name + ".raw"))


def gate_signal():
    """Gate threshold -56 dBFS (close -62): segments (dB rms of a 330 Hz sine, seconds); noise floor -90 dBFS."""
    segs = [(None, 0.2), (-59 - 3, 0.3), (-50, 0.2), (-59 - 3, 0.4), (-72, 0.4), (-45, 0.15), (None, 0.4)]
    out, bounds, t0 = [], [], 0
    for db, sec in segs:
        n = int(sec * FI)
        v = RNG.normal(0, dbfs(-90), n)
        if db is not None:
            v += dbfs(db + 3) * np.sin(2 * np.pi * 330 * (t0 + np.arange(n)) / FI)    # peak = rms + 3 dB
        out.append(v)
        bounds.append((t0, t0 + n, db))
        t0 += n
    return np.concatenate(out), bounds


GATE_WHITE = int(round((-56 + 84) / 54 * 32767))
PRESETS = {                                 # engines.json "amp" presets (knobs), kept in sync by main()
}
DRIVES = [0, 8192, 16384, 24576, 32767]


def cases(workdir):
    """name: (k0, k1, k2, k3, blocks, input, events, flags)"""
    c = {}
    write(workdir, "gtr", guitar(110, -15, 3.0))
    write(workdir, "gtr_hot", guitar(82.4, 0, 3.0))
    write(workdir, "gtr_noise", guitar(196, -15, 3.0, period=1.0, floor_db=-70))
    write(workdir, "sine", sine(220, -15, 1.0))
    write(workdir, "noise", RNG.normal(0, dbfs(-24), int(1.5 * FI)))
    write(workdir, "silence", np.zeros(FI // 2))
    g, _ = gate_signal()
    write(workdir, "gate", g)
    nb = lambda sec: int(sec * FI / 64)
    for name, k in PRESETS.items():
        c[f"preset_{name}"] = (*k, nb(3.0), "gtr", "0n0", "")
        c[f"hot_{name}"] = (*k, nb(3.0), "gtr_hot", "0n0", "")
    for b in DRIVES:
        c[f"drive_{b}"] = (b, 16384, 0, 0, nb(1.0), "sine", "0n0", "")
        c[f"loud_{b}"] = (b, 16384, 0, 0, nb(3.0), "gtr", "0n0", "")
    for o in (0, 11264, 22528, 32767):
        c[f"cab_{o}"] = (0, 16384, 0, o, nb(1.5), "noise", "0n0", "")
    for t in (0, 32767):
        c[f"tone_{t}"] = (0, t, 0, 0, nb(1.5), "noise", "0n0", "")
    c["gate"] = (16384, 16384, GATE_WHITE, 16384, nb(len(g) / FI), "gate", "0n0", "")
    c["gate_off"] = (16384, 16384, 0, 16384, nb(len(g) / FI), "gate", "0n0", "")
    # ownership (6 voices; render order 0..5)
    k = (16384, 16384, 0, 16384)
    n = nb(2.0)
    c["own_ref"] = (*k, n, "gtr_noise", "0n0", "")
    c["own_chord"] = (*k, n, "gtr_noise", "0n0,0n1,0n2,0n3", "")
    c["own_new"] = (*k, n, "gtr_noise", "0n0,300n1", "")
    c["own_release"] = (*k, n, "gtr_noise", "0n0,0n1,300r1,700s1", "")
    c["own_stop"] = (*k, n, "gtr_noise", "0n0,0n1,300s1", "")
    c["own_steal"] = (*k, n, "gtr_noise", "0n0,0n1,300n1,600n0", "")
    c["own_late"] = (*k, n, "gtr_noise", "0n0,200n3,400n1,600s3,800r1,900n5", "")
    c["own_dbl"] = (*k, n, "gtr_noise", "0n0,0n1,0n2,300n4,600s2", "dbl")
    c["own_dbl_ref"] = (*k, n, "gtr_noise", "0n0,0n1,0n2,300n4,600s2", "")
    c["own_nosel"] = (*k, n, "gtr_noise", "0n0,0n1,0n2,300n4,600s2", "nosel,rev")
    c["own_nosel_ref"] = (*k, n, "gtr_noise", "0n0,0n1,0n2,300n4,600s2", "rev")
    c["own_silence"] = (*k, nb(0.5), "silence", "0n0,0n1,100n2", "dbl,nosel")
    c["worst"] = (32767, 32767, 32767, 32767, nb(1.0), "gtr_hot", "0n0,0n1,0n2,0n3,0n4,0n5", "")
    return c


def run(workdir, cs):
    cmds = [f"bfin-elf-run /tmp/m {k0} {k1} {k2} {k3} {blocks} /w/{inp}.raw {ev} {fl or '-'} > /w/{name}.txt 2> /w/{name}.err"
            for name, (k0, k1, k2, k3, blocks, inp, ev, fl) in cs.items()]
    open(os.path.join(workdir, "cmds"), "w").write("\n".join(cmds) + "\n")
    script = ("set -e; cd /tmp\n"
              "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror -c -o engine.o /src/amp.c\n"
              "bfin-elf-as -o tramp.o /src/tramp.S\n"
              "bfin-elf-gcc -std=gnu99 -msim -O1 -o m /src/test/amp_main.c tramp.o engine.o\n"
              "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n"
              "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror -c -o abi_e.o /src/amp.c\n"
              "bfin-elf-gcc -std=gnu99 -msim -O1 -DAMP_ABI -o abi /src/test/abi_main.c /src/test/abi_check.S tramp.o abi_e.o\n"
              "bfin-elf-run abi > /w/abi.txt || true\n")
    open(os.path.join(workdir, "run.sh"), "w").write(script)
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{ENGINE}:/src:ro",
                    "-v", f"{workdir}:/w", "op1-bfin", "sh", "/w/run.sh"], check=True)


def load(workdir, name):
    a = np.loadtxt(os.path.join(workdir, name + ".txt"), dtype=np.int64)
    f = open(os.path.join(workdir, name + ".err")).read().split()
    info = {f[i]: f[i + 1] for i in range(1, len(f) - 1, 2)}
    info["cycles"] = int(f[2])
    return a[:, 0], a[:, 1], a[:, 2], info


def rms_db(x):
    return 20 * np.log10(max(np.sqrt(np.mean(x.astype(float) ** 2)), 1e-9) / 32768)


def thd(x, f0, fs=FO):
    x = x[len(x) // 4:].astype(float)
    w = np.hanning(len(x))
    m = np.abs(np.fft.rfft(x * w))
    fr = np.fft.rfftfreq(len(x), 1 / fs)
    h = [m[np.abs(fr - f0 * n) < 4].max() for n in range(1, 11)]
    return 10 * np.log10(sum(v * v for v in h[1:]) / h[0] ** 2), h


def response(workdir, name, inp):
    """|Y / X| in dB at gat.RESP_F (input at 44.1 kHz, output at 88.2 kHz), cross-spectral over 4096-sample frames."""
    x = np.fromfile(os.path.join(workdir, inp + ".raw"), dtype="<i2").astype(float)
    y = load(workdir, name)[0].astype(float)
    n, skip = 4096, 8192
    sxy = sxx = 0
    w, w2 = np.hanning(n), np.hanning(2 * n)
    for s in range(skip, len(x) - n, n // 2):
        if 2 * (s + n) > len(y):
            break
        X = np.fft.rfft(x[s:s + n] * w)
        Y = np.fft.rfft(y[2 * s:2 * s + 2 * n] * w2)[:len(X)] / 2
        sxy = sxy + Y * np.conj(X)
        sxx = sxx + np.abs(X) ** 2
    fr = np.fft.rfftfreq(n, 1 / FI)
    h = np.abs(sxy / sxx)
    return np.array([20 * np.log10(np.mean(h[(fr > f / 1.06) & (fr < f * 1.06)])) for f in gat.RESP_F])


def onepole_hp(f, fc):
    a = 1 - np.exp(-2 * np.pi * fc / FO)
    z = np.exp(-1j * 2 * np.pi * f / FO)
    return np.abs(1 - a / (1 - (1 - a) * z))


def onepole_lp(f, fc):
    a = 1 - np.exp(-2 * np.pi * fc / FO)
    z = np.exp(-1j * 2 * np.pi * f / FO)
    return np.abs(a / (1 - (1 - a) * z))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep")
    ap.add_argument("--calibrate", action="store_true")
    a = ap.parse_args()
    import json
    eng = [e for e in json.load(open(os.path.join(os.path.dirname(ENGINE), "slot", "engines.json"))) if e["name"] == "amp"]
    if eng:
        PRESETS.update({n.replace(" ", "_"): k for n, k in eng[0]["presets"].items()})
    workdir = tempfile.mkdtemp(prefix="amp_check_")
    cs = cases(workdir)
    run(workdir, cs)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    comp = gat.blue_tables()[5]
    if a.calibrate:
        for b in DRIVES:
            y = load(workdir, f"loud_{b}")[0]
            c = 20 * np.log10(comp[round(b / 1024)])
            print(f"blue {b:5}: rms {rms_db(y):6.1f} dBFS (compensation {c:+.1f} dB, without {rms_db(y) - c:6.1f})")
        return

    worst = dsp_worst = 0
    for name in cs:
        out, mask, gate, info = load(workdir, name)
        worst = max(worst, info["cycles"])
        dsp_worst = max(dsp_worst, int(info["dspmax"]))
        if info["guard"] != "ok" or np.abs(out).max() > 32767 * 6:
            check(False, f"{name}: output guard {info['guard']}")

    # ---- gain staging ----
    for name in [n for n in cs if n.startswith(("preset_", "loud_"))]:
        y = load(workdir, name)[0]
        r, pk = rms_db(y), np.abs(y).max()
        rails = int((np.abs(y) >= 32700).sum())
        check(-24 < r < -9 and rails == 0, f"{name}: -15 dBFS note -> rms {r:.1f} dBFS, peak {20*np.log10(pk/32768):.1f} dBFS, "
              f"{rails} samples at the rails")
    loud = [rms_db(load(workdir, f"loud_{b}")[0]) for b in DRIVES]
    check(max(loud) - min(loud) < 5, "loudness across drive (clean is peak-bound): " + " ".join(f"{v:.1f}" for v in loud) + " dBFS")
    for name in [n for n in cs if n.startswith("hot_")]:
        y = load(workdir, name)[0]
        rails = (np.abs(y) >= 32700).mean()
        check(rails < 0.002, f"{name}: 0 dBFS note -> peak {20*np.log10(np.abs(y).max()/32768):.1f} dBFS, "
              f"{100*rails:.2f} % of samples at the rails (limiter + knee)")

    # ---- drive ----
    th = [thd(load(workdir, f"drive_{b}")[0], 220)[0] for b in DRIVES]
    check(all(th[i + 1] > th[i] for i in range(len(th) - 1)) and th[0] < -30 and th[-1] > -12,
          "THD rises with blue: " + " ".join(f"{v:.1f}" for v in th) + " dB")

    # ---- cab and tone ----
    f = gat.RESP_F
    sel = (f >= 60) & (f <= 10000)
    base = onepole_hp(f, gat.TIGHT[0]) * onepole_lp(f, gat.INTER_FC)
    i1k = np.argmin(np.abs(f - 1000))
    for o in (0, 11264, 22528, 32767):
        h = response(workdir, f"cab_{o}", "noise")
        want = 20 * np.log10(gat.response(gat.cab_filters(round(o / 1024)), f) * base)
        err = (h - h[i1k]) - (want - want[i1k])
        check(np.abs(err[sel]).max() < 1.0, f"cab {o}: response vs design, 60 Hz..10 kHz, max error {np.abs(err[sel]).max():.2f} dB "
              f"(at 100 Hz {want[np.argmin(np.abs(f-100))]-want[i1k]:+.1f}, 5 kHz {want[np.argmin(np.abs(f-5000))]-want[i1k]:+.1f} dB re 1 kHz)")
    for t in (0, 32767):
        h = response(workdir, f"tone_{t}", "noise")
        want = 20 * np.log10(gat.response(gat.tone_filters(round(t / 1024)), f) * base
                             * gat.response(gat.cab_filters(0), f))
        err = (h - h[i1k]) - (want - want[i1k])
        check(np.abs(err[sel]).max() < 1.0, f"tone {t}: response vs design, max error {np.abs(err[sel]).max():.2f} dB")

    # ---- gate ----
    _, bounds = gate_signal()
    _, _, gate, _ = load(workdir, "gate")
    gb = gate[::128]                                    # per block (block b = input samples 64 b .. 64 b + 63)
    def gate_in(t0, t1, frac=(0.3, 0.95)):
        b0, b1 = int((t0 + frac[0] * (t1 - t0)) / 64), int((t0 + frac[1] * (t1 - t0)) / 64)
        return gb[b0:b1]
    st = [gate_in(t0, t1) for t0, t1, _ in bounds]
    check(not st[0].any() and not st[1].any(), "gate: closed in the noise floor and between the thresholds from closed")
    check(st[2].all() and st[3].all(), "gate: opens above the threshold and stays open between the thresholds")
    t0, t1, _ = bounds[4]
    closed_at = t0 + 64 * np.argmax(gb[t0 // 64:t1 // 64] == 0)
    hold_ms = (closed_at - t0) / FI * 1000
    check(40 < hold_ms < 80 and not gate_in(t0, t1, (0.5, 0.95)).any(),
          f"gate: closes {hold_ms:.0f} ms after dropping below the close threshold (50 ms hold + detector decay)")
    check(st[5].all() and not st[6].any(), "gate: reopens and closes again")
    check(load(workdir, "gate_off")[2].all(), "gate off (white 0): always open")
    y = load(workdir, "gate")[0]
    t0, t1, _ = bounds[4]
    tail = np.abs(y[2 * (t0 + 8000):2 * t1]).max()
    check(tail < 40, f"gate: output below the close threshold after the release: peak {tail}")

    # ---- ownership ----
    ref = load(workdir, "own_ref")[0]
    for name in ("own_chord", "own_new", "own_release", "own_steal", "own_late", "own_dbl", "own_nosel"):
        y, mask, _, info = load(workdir, name)
        bits = np.array([bin(m).count("1") for m in mask[::128]])
        r = load(workdir, name + "_ref")[0] if name + "_ref" in cs else ref
        d = np.abs(y - r)
        tol = 3 if name in ("own_new", "own_release") else None
        msg = f"{name}: voices sounding per block max {bits.max()}, takeovers {info['takeovers']}"
        if name in ("own_dbl", "own_nosel"):
            check(np.array_equal(y, r), f"{name}: identical to the normal run ({np.count_nonzero(y != r)} samples differ)")
            continue
        if tol is not None:
            check(bits.max() <= 2 and d.max() <= tol, msg + f", |mix - single voice| max {d.max()}")
        else:
            check(bits.max() <= 3 and np.abs(y).max() <= 1.13 * np.abs(ref).max() + 2,
                  msg + f", peak {np.abs(y).max()} vs single voice {np.abs(ref).max()}")
    # steady state: one voice only
    y, mask, _, _ = load(workdir, "own_chord")
    bits = np.array([bin(m).count("1") for m in mask[::128]])
    check((bits[20:] <= 1).all() and np.abs(y[128 * 20:] - ref[128 * 20:]).max() <= 2,
          "chord of 4: one voice plays, the mix equals one voice (after the first crossfade)")
    # handovers move ownership to the expected voice, and the gain ramps have no steps
    y, mask, _, info = load(workdir, "own_stop")
    owner_after = mask[128 * 320:][::128]
    gap = np.abs(y[128 * 300:128 * 330]).astype(float)
    env = np.abs(ref[128 * 300:128 * 330]).astype(float)
    ok = (owner_after == 1).all()
    seg_y, seg_r = y[128 * 300:128 * 330].astype(float), ref[128 * 300:128 * 330].astype(float)
    ratio = [np.dot(seg_y[i:i + 128], seg_r[i:i + 128]) / max(np.dot(seg_r[i:i + 128], seg_r[i:i + 128]), 1)
             for i in range(0, len(seg_y), 128)]        # the voice gain per block
    steps = np.diff(ratio)
    check(ok and max(steps) < 0.2, f"stop (no release flag): voice 0 takes over, fades in (largest step {max(steps):.2f} of "
          f"full per block, blocks muted {sum(r < 0.01 for r in ratio)})")
    y, mask, _, info = load(workdir, "own_release")
    check((mask[128 * 320:128 * 690][::128] == 1).all(), "release flag: the held voice takes over while the released one fades")
    y, mask, _, _ = load(workdir, "own_steal")
    check((mask[128 * 320:128 * 590][::128] == 2).all() and (mask[128 * 620:][::128] == 1).all(),
          "steal / retrigger: the re-triggered voice keeps or takes ownership")
    y, mask, _, info = load(workdir, "own_silence")
    check(info["guard"] == "ok" and not y.any(), f"digital silence, double renders, no te_rx_sel: silent ({info['blocks']} blocks "
          f"for 172 callbacks: each repeat render over digital silence counts one, harmless)")
    check(int(load(workdir, "own_dbl")[3]["blocks"]) == int(load(workdir, "own_dbl_ref")[3]["blocks"]),
          "double renders per callback: one block per callback")

    check(dsp_worst <= CPU_BUDGET, f"cycles per render call: max {worst} (with the DSP {dsp_worst}; budget {CPU_BUDGET})")
    abi = open(os.path.join(workdir, "abi.txt")).read().strip()
    check("abi mask 00000000" in abi, f"ABI: {abi}")
    if a.keep:
        shutil.copytree(workdir, a.keep, dirs_exist_ok=True)
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
