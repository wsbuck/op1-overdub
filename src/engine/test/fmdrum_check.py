#!/usr/bin/env python3
"""Simulator checks for the fmdrum drum engine (src/slot/fmdrum.c) before flashing.

  python3 src/engine/test/fmdrum_check.py [--keep DIR]

Builds fmdrum.c with the device flags and fmdrum_main.c (TE's drum voice loop in miniature: slot init, note-on, render,
active, note-off, each through its C_ENTRY wrapper with fx_abi.S's register sentinels) and runs every case under
bfin-elf-run at 88.2 kHz. The knob curves come from src/slot/gen_fmdrum.py (the tables' source). Checks:
  pitch (K0) per semitone; the sweep (K4/K5) starts high and lands on the pitch; FM spectra (sidebands = Bessel J_k(I)
  of the index the tables give); the modulator's own decay (K7) dims the sidebands while the note sounds on;
  aliasing at the extremes (index limiter, modulator fade near fs/2): energy folded into 0..20 kHz; DC (a 1:1 ratio puts
  a sideband at 0 Hz); amplitude decay times (K3) and the voice's end (active() 0, then silence); the noise zones (K6:
  colour rising AIR < SNARE < HAT, tone gone in CLAP, clap bursts); velocity; knob changes and retriggers without
  clicks; every factory kit key: level (peaks, near-clip samples, audible); the hi-hat choke (keys 10..13, HAT zone);
  the RATIO table's order, NOISE's stock maximum still CLAP, the kits' shared hat keys; CYCLES per render call against
  the engines' budget; output guard; no runtime division or other libgcc calls; slot init; the ABI; the harsh path's
  asm loops against fmdrum.c's C loops (built without __bfin__: the same cases, bit for bit). Exits non-zero on
  a failure.
"""
import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(HERE)
SLOT = os.path.join(os.path.dirname(ENGINE), "slot")
sys.path.insert(0, SLOT)
import gen_fmdrum as G  # noqa: E402
from simrun import CPU_BUDGET  # noqa: E402

FS = 88200
PITCH, SWEEP, FM, DECAY, RATIO, SWDEC, FMDEC, NOISE = range(8)     # fmdrum.h FD_*
DRY = [0, 0, 0, 32767, G.k1_of(1.0), 0, 32767, 0]           # A3 sine, longest decay, no FM/sweep/noise


def P(**kw):
    """per-key values from physical settings (gen_fmdrum's inverse curves); missing ones from DRY"""
    k = list(DRY)
    if "hz" in kw:
        k[PITCH] = G.k0_of(kw["hz"])
    if "ratio" in kw:
        k[RATIO] = G.k1_of(kw["ratio"])
    if "fm" in kw:
        k[FM] = G.k2_of(kw["fm"])
    if "decay" in kw:
        k[DECAY] = G.k_expo(kw["decay"], *G.DECAY)
    if "sweep" in kw:
        k[SWEEP] = G.k4_of(kw["sweep"])
    if "snap" in kw:
        k[SWDEC] = G.k_expo(kw["snap"], *G.SNAP)
    if "noise" in kw:
        k[NOISE] = kw["noise"]
    if "mod" in kw:
        k[FMDEC] = G.k_expo(kw["mod"], *G.MODDEC)
    for j, v in kw.get("raw", {}).items():
        k[j] = v
    return k


def cases():
    c = {}
    for s in (-24, -12, 0, 7, 12, 24, 36):                  # pitch: semitones from A3
        c[f"pitch{s}"] = (0, 127, 200, P(raw={0: s * 512}))
    c["pitch_min"] = (0, 127, 200, P(raw={0: G.K0_MIN}))
    c["pitch_max"] = (0, 127, 200, P(raw={0: G.STOCK_HI[0]}))
    c["sweep"] = (0, 127, 300, P(hz=110, sweep=24, snap=0.03))
    c["sweep_none"] = (0, 127, 120, P(hz=110, sweep=0, snap=0.03))
    for r, fm in ((3.5, 1.0), (3.5, 2.4), (2.7565, 4.0), (1.4142, 6.0)):
        c[f"fm_{r}_{fm}"] = (0, 127, 120, P(hz=220, ratio=r, fm=fm, mod=3.0))
    c["mod_fast"] = (0, 127, 120, P(hz=220, ratio=3.5, fm=4.0, mod=0.003))
    c["mod_slow"] = (0, 127, 120, P(hz=220, ratio=3.5, fm=4.0, mod=3.0))
    # aliasing: highest pitches, ratios and index, no sweep, sustained
    alias = [(G.STOCK_HI[0], 1.4142), (G.STOCK_HI[0], 0.5), (G.STOCK_HI[0], 2.7565), (12288, 7.1268), (18000, 4.6),
             (6144, 15.5), (0, 15.5), (20000, 2.0)]
    for i, (k0, r) in enumerate(alias):
        c[f"alias{i}"] = (0, 127, 200, P(ratio=r, fm=16.0, mod=3.0, raw={0: k0}))
    c["dc"] = (0, 127, 400, P(hz=110, ratio=1.0, fm=2.4, mod=3.0))
    c["decay_min"] = (0, 127, 60, P(raw={DECAY: 0}))
    c["decay_mid"] = (0, 127, 300, P(decay=0.25))
    c["decay_max"] = (0, 127, 3400, P(raw={DECAY: 32767}))
    for z, k6 in (("off", 0), ("air", G.NZ("AIR", 0.7)), ("snare", G.NZ("SNARE", 0.73)), ("hat", G.NZ("HAT", 0.7)),
                  ("hat_top", G.NZ("HAT", 0.98)), ("clap", G.NZ("CLAP", 1.0))):
        c[f"noise_{z}"] = (1, 127, 160, P(hz=50, decay=0.5, noise=k6))
    c["noise_stockmax"] = (1, 127, 160, P(hz=50, decay=0.5, noise=G.STOCK_HI[NOISE]))
    # FEEDBACK / SQUARE: sustained notes at each level, low and high pitch (stability, DC, the spectrum's growth)
    for lv in range(8):
        for hz in (55, 440):
            c[f"fb{lv}_{hz}"] = (0, 127, 300, P(hz=hz, ratio=1.0, raw={FM: G.k2_fb(lv)}, mod=3.0))
    for j, a_ in enumerate((0.25, 0.5, 0.75, 1.0)):
        for hz in (55, 440):
            c[f"sq{j}_{hz}"] = (0, 127, 300, P(hz=hz, ratio=1.0, raw={FM: G.k2_sq(a_)}, mod=3.0))
    for r in (0.5, 3.5, 15.5):              # extreme ratios and pitches with full feedback: bounded, no DC
        c[f"fbx_{r}"] = (0, 127, 300, P(ratio=r, raw={FM: G.k2_fb(7), PITCH: G.STOCK_HI[0]}, mod=3.0))
        c[f"fbl_{r}"] = (0, 127, 300, P(ratio=r, raw={FM: 32767, PITCH: G.K0_MIN}, mod=3.0))
    # METAL / GRIND across their zones, at a low and a mid pitch
    for z in ("METAL", "GRIND"):
        for j, t in enumerate((0.0, 0.35, 0.7, 1.0)):
            for hz in (110, 440):
                c[f"{z.lower()}{j}_{hz}"] = (1, 127, 160, P(hz=hz, decay=1.0, ratio=1.4142, fm=3.0, mod=0.3,
                                                             noise=G.NZ(z, t)))
    # zone sweeps (a slow turn, as TE's LFO), and fast turns (a detent per block)
    c["sweep_fm"] = (0, 127, 1200, P(hz=110, ratio=1.4142, decay=4.0, mod=3.0) + ["ramp", 50, 1150, FM, 0, 32767])
    c["sweep_noise"] = (0, 127, 1200, P(hz=220, ratio=1.4142, fm=2.0, decay=4.0, mod=3.0)
                        + ["ramp", 50, 1150, NOISE, 0, 32767])
    c["fast_fm"] = (0, 127, 160, P(hz=110, ratio=1.4142, decay=4.0, mod=3.0) + ["ramp", 40, 72, FM, 0, 32767])
    c["fast_noise"] = (0, 127, 160, P(hz=220, ratio=1.4142, fm=2.0, decay=4.0, mod=3.0)
                       + ["ramp", 40, 72, NOISE, 0, 32767])
    for kz, kv in ((FM, G.FM_FB0), (FM, G.FM_FB0 + 1024), (FM, G.FM_SQ0), (FM, G.FM_SQ0 + 1024),
                   (NOISE, G.NOISE_EDGES[5] - 1024), (NOISE, G.NOISE_EDGES[5]), (NOISE, G.NOISE_EDGES[6] - 1024),
                   (NOISE, G.NOISE_EDGES[6])):
        c[f"join_{kz}_{kv}"] = (0, 127, 80, P(hz=220, ratio=1.4142, fm=2.0, decay=4.0, mod=3.0, raw={kz: kv - 1})
                                + ["chg", 40, kz, kv])
    c["vel64"] = (0, 64, 60, P(hz=220, decay=0.5))
    c["vel127"] = (0, 127, 60, P(hz=220, decay=0.5))
    c["chg_pitch"] = (0, 127, 80, P(hz=220, decay=2.0) + ["chg", 40, PITCH, 12 * 512])
    c["chg_fm"] = (0, 127, 80, P(hz=220, ratio=2.0, fm=1.0, decay=2.0, mod=3.0) + ["chg", 40, FM, G.k2_of(4.0)])
    c["chg_noise"] = (0, 127, 80, P(hz=220, decay=2.0, noise=G.NZ("AIR", 0.7)) + ["chg", 40, NOISE, G.NZ("HAT", 0.7)])
    c["retrig"] = (0, 127, 80, P(hz=60, decay=1.0, sweep=12, snap=0.02, fm=1.0, mod=0.03) + ["retrig", 30])
    c["worst"] = (23, 127, 30, [G.STOCK_HI[0], 32767, 32767, 32767, 31 * 1024, 32767, 32767, G.NZ("HAT", 0.5)])
    for z in ("METAL", "GRIND"):            # the harsh path's heaviest variants: feedback, square, METAL + GRIND mixed
        c[f"worst_{z.lower()}"] = (23, 127, 30, [G.STOCK_HI[0], 32767, 32767, 32767, 31 * 1024, 32767, 32767,
                                                 G.NZ(z, 0.25 if z == "GRIND" else 0.6)])
    for name, kit in G.KITS.items():
        for k, row in enumerate(kit[1]):
            c[f"kit_{name.replace(' ', '_')}_{k}"] = (k, 127, 500, list(row))
    rng = np.random.default_rng(82)         # random keys, knob turns and retriggers (the asm against the C loops)
    for j in range(48):
        k = [int(rng.integers(-24320, 24833))] + [int(rng.integers(0, 32768)) for _ in range(7)]
        if j % 2:                           # half of them harsh: FM and NOISE in the top zones
            k[FM] = int(rng.integers(G.FM_FB0 - 2048, 32768))
            k[NOISE] = int(rng.integers(G.NOISE_EDGES[4], 32768))
        extra = []
        for _ in range(int(rng.integers(1, 4))):
            b0 = int(rng.integers(0, 60))
            extra += ["ramp", b0, b0 + int(rng.integers(1, 40)), int(rng.integers(0, 8)), int(rng.integers(0, 32768)),
                      int(rng.integers(0, 32768))]
        extra += ["retrig", int(rng.integers(5, 90)), "chg", int(rng.integers(1, 99)), FM, int(rng.integers(0, 32768))]
        c[f"fuzz{j}"] = (int(rng.integers(0, 24)), int(rng.integers(1, 128)), 100, k + extra)
    return c


OPEN_HAT = P(hz=3000, ratio=1.4142, fm=7.0, decay=0.7, mod=0.6, noise=G.NZ("HAT", 0.47))   # HAT zone
CLOSED_HAT = P(hz=3000, ratio=1.4142, fm=7.0, decay=0.07, mod=0.08, noise=G.NZ("HAT", 0.5))
METAL_OPEN = P(hz=400, ratio=1.4142, fm=3.0, decay=0.7, mod=0.3, noise=G.NZ("METAL", 0.9))   # 808-style hats
METAL_CLOSED = P(hz=400, ratio=1.4142, fm=3.0, decay=0.07, mod=0.05, noise=G.NZ("METAL", 0.9))
CHOKE = {                                   # (open key, hit key, open values, hit values): voice 3 rings, voice 4 hits
    "choke_hat": (13, 10, OPEN_HAT, CLOSED_HAT),            # open hat cut by a closed hat
    "choke_same": (13, 13, OPEN_HAT, OPEN_HAT),             # one hi-hat: the same key cuts its last hit
    "choke_outside": (13, 14, OPEN_HAT, CLOSED_HAT),        # key 14 is not in the group
    "choke_notzone": (13, 10, OPEN_HAT, P(hz=3000, decay=0.07, noise=G.NZ("SNARE"))),   # hit key in the group, SNARE
    "choke_metal": (13, 11, METAL_OPEN, METAL_CLOSED),     # METAL hats choke like HAT ones
    "choke_mixed": (13, 10, METAL_OPEN, CLOSED_HAT),        # .. and each other
    "choke_grind": (13, 10, P(hz=300, decay=0.7, noise=G.NZ("GRIND")), CLOSED_HAT),   # a GRIND key rings on
    "choke_victim": (12, 10, P(hz=300, decay=0.7, noise=0), CLOSED_HAT),       # a group key without HAT noise rings on
}


WORST = [G.STOCK_HI[0], 32767, 32767, 32767, 31 * 1024, 32767, 32767, None]


def poly_cases():
    """12 voices at once: the heaviest settings, and each kit's 12 heaviest keys (by their single-call cycles, filled
    in after a first pass would be circular: the kits' keys sorted by how much of the harsh path they use)"""
    c = {"poly_worst": [WORST[:7] + [G.NZ("GRIND", 0.25)]] * 6 + [WORST[:7] + [G.NZ("METAL", 0.6)]] * 6}

    def weight(k):
        return (G.fm_zone(k[FM]) > 0) * 2 + (G.noise_zone(k[NOISE]) >= 5) * 2 + (k[NOISE] >= 1024) + (k[FM] > 0)
    for name, kit in G.KITS.items():
        rows = sorted(kit[1], key=weight, reverse=True)[:12]
        c[f"poly_{name.replace(' ', '_')}"] = [list(r) for r in rows]
    return c


def rms_db(x):
    return 20 * math.log10(max(float(np.sqrt(np.mean(x.astype(float) ** 2))), 1e-3) / 32768)


def flatness(x, lo=50, hi=20000):
    fr, m = spectrum(x)
    sel = (fr > lo) & (fr < hi)
    p = m[sel] ** 2 + 1e-12
    return float(np.exp(np.mean(np.log(p))) / np.mean(p))


def edge_zipper(x, a, b):
    """the largest sample step at block edges over the largest step inside blocks, blocks a..b"""
    d = np.abs(np.diff(x[a * 128:b * 128].astype(float)))
    edge = d[127::128]
    inner = np.delete(d, np.arange(127, len(d), 128))
    return float(np.percentile(edge, 99.5) / max(np.percentile(inner, 99.5), 1.0))


def win_jump(x, a, b, win=48, t60=None):
    """the largest RMS change (dB) between neighbouring windows of `win` blocks, blocks a..b; t60: the note's decay
    (s), taken out first"""
    slope = 60 / t60 * win * 128 / FS if t60 else 0.0
    e = [rms_db(x[i * 128:(i + win) * 128]) + slope * j for j, i in enumerate(range(a, b - win, win))]
    return max(abs(e[i + 1] - e[i]) for i in range(len(e) - 1))


def harsh(res, cs, check, w):
    # ---- FEEDBACK: bounded, no DC, grit -> noise, steady loudness ----
    worst_dc, runaway, flat, lev, dc_who = 0.0, 0.0, {}, {}, ""
    names = [n for n in cs if n.startswith(("fb", "sq"))]
    for n in names:
        x = res[n]["x"].astype(float)
        if G.pitch_hz(cs[n][3][PITCH]) < 20:            # below hearing: the 8 Hz blocker can't hold a 14 Hz square
            continue
        k = cs[n][3]
        per = int(round(2 * FS / G.pitch_hz(k[PITCH])))         # the waveform's period (ratios are n / 2 or METAL)
        seg = x[FS // 10:]
        seg = seg[:len(seg) // per * per] if len(seg) >= per else seg
        dcn = abs(seg.mean()) / max(np.abs(seg).max(), 1)
        if dcn > worst_dc:
            worst_dc, dc_who = dcn, n
        e = [rms_db(x[i:i + FS // 10]) for i in range(FS // 10, len(x) - FS // 10, FS // 10)]
        runaway = max(runaway, max(e[i + 1] - e[i] for i in range(len(e) - 1)))
        flat[n] = flatness(res[n]["x"][FS // 10:FS // 10 + 32768])
        lev[n] = rms_db(res[n]["x"][FS // 10:FS // 10 + 32768])
    clipped = max(float((np.abs(res[n]["x"]) >= 32000).mean()) for n in names)
    check(worst_dc < 0.02 and runaway < 1.0 and clipped < 0.01,
          f"FEEDBACK/SQUARE stable over {len(names)} sustained notes (DC from 20 Hz up) (levels 0..7, square 0.25..1, ratios 0.5..15.5, "
          f"pitch 14 Hz .. 3.5 kHz): DC at most {100 * worst_dc:.2f} % of the peak ({dc_who}), level never rises (worst "
          f"{runaway:+.2f} dB per 0.1 s), {100 * clipped:.2f} % of samples at full scale")
    for hz in (55, 440):
        fl = [flat[f"fb{lv}_{hz}"] for lv in range(8)]
        lv_db = [lev[f"fb{lv}_{hz}"] for lv in range(8)] + [lev[f"sq{j}_{hz}"] for j in range(4)]
        print(f"  {hz} Hz flatness FB0..7: " + " ".join(f"{v:.3f}" for v in fl)
              + "; SQ: " + " ".join(f"{flat[f'sq{j}_{hz}']:.3f}" for j in range(4)))
        print(f"  {hz} Hz RMS dBFS FB0..7, SQ1..4: " + " ".join(f"{v:.1f}" for v in lv_db))
        check(fl[0] < fl[3] < fl[7] and fl[7] > 0.2 and max(lv_db) - min(lv_db) < 6,
              f"FEEDBACK at {hz} Hz: grit to noise (spectral flatness {fl[0]:.3f} -> {fl[3]:.3f} -> {fl[7]:.3f}); "
              f"loudness across FEEDBACK and SQUARE within {max(lv_db) - min(lv_db):.1f} dB")

    def odd_ratio(x, f0):           # energy in harmonics 3, 5, 7 .. over the fundamental's (a clipped carrier)
        fr, m = spectrum(x)
        f1 = level(fr, m, f0, 6) ** 2
        return sum(level(fr, m, f0 * k, 6) ** 2 for k in range(3, 40, 2) if f0 * k < 20000) / max(f1, 1e-9)
    sqr = [odd_ratio(res[f"sq{j}_55"]["x"][FS // 10:FS // 10 + 32768], 55.0) for j in range(4)]
    # (the feedback's harmonics are odd and even; a square adds the odd ones on top)
    check(sqr[3] > sqr[0], f"SQUARE: odd harmonics over the fundamental rise with the clip at 55 Hz "
          + " ".join(f"{10 * math.log10(max(v, 1e-9)):.1f}" for v in sqr) + " dB")

    # ---- METAL: dense, inharmonic, a band that rises across the zone ----
    def centroid(x):
        fr, m = spectrum(x)
        sel = (fr > 200) & (fr < 20000)
        return float((fr[sel] * m[sel] ** 2).sum() / max((m[sel] ** 2).sum(), 1e-9))

    def peak_share(x):              # the strongest 20 Hz band's share of the energy 200 Hz .. 20 kHz
        fr, m = spectrum(x)
        sel = (fr > 200) & (fr < 20000)
        p = m[sel] ** 2
        k = max(1, int(20 / (fr[1] - fr[0])))
        return float(np.convolve(p, np.ones(k), "same").max() / max(p.sum(), 1e-9))
    for hz in (110, 440):
        cen = [centroid(res[f"metal{j}_{hz}"]["x"][:16384]) for j in range(4)]
        share = [peak_share(res[f"metal{j}_{hz}"]["x"][2048:2048 + 16384]) for j in range(1, 4)]
        check(cen[1] < cen[2] < cen[3] and share[2] < 0.2,
              f"METAL at {hz} Hz: band rising through the zone (centroid " + " ".join(f"{v:.0f}" for v in cen)
              + f" Hz); the FM body early, unpitched at the top (the strongest 20 Hz band holds "
              + " / ".join(f"{100 * v:.0f}" for v in share) + " % at t 0.35 / 0.7 / 1)")
        gl = [rms_db(res[f"grind{j}_{hz}"]["x"][:16384]) for j in range(4)]
        ml = [rms_db(res[f"metal{j}_{hz}"]["x"][:16384]) for j in range(4)]
        cl = rms_db(res["noise_clap"]["x"][:16384])
        gf = [flatness(res[f"grind{j}_{hz}"]["x"][2048:2048 + 16384]) for j in range(4)]
        check(max(gl + ml[1:]) - min(gl + ml[1:]) < 6 and gf[1] > 0.1,
              f"METAL/GRIND at {hz} Hz: levels " + " ".join(f"{v:.1f}" for v in ml) + " / "
              + " ".join(f"{v:.1f}" for v in gl) + f" dBFS (CLAP {cl:.1f}); GRIND's noise broadband, its square a buzz (flatness "
              + " ".join(f"{v:.2f}" for v in gf) + ")")

    # ---- joins, slow sweeps and fast turns ----
    for n in [n for n in cs if n.startswith("join_")]:
        x = res[n]["x"]
        d = np.abs(np.diff(x))
        at = d[40 * 128 - 2:40 * 128 + 130].max()
        around = max(d[36 * 128:39 * 128].max(), d[42 * 128:45 * 128].max())
        jump = abs(rms_db(x[41 * 128:57 * 128]) - rms_db(x[23 * 128:39 * 128]))   # 23 ms each side (noise)
        _, kz, kv = n.split("_")
        what = ("FM " if int(kz) == FM else "NOISE ") + str(int(kv) // 1024)
        check(at <= 1.25 * around and jump < 1.5,
              f"zone join at {what} (one step across it): step {at} vs {around} around, level {jump:.2f} dB")
    for n, lim in (("sweep_fm", 2.0), ("sweep_noise", 3.0)):     # (HAT's tone fade, old: -2.8 dB)
        x = res[n]["x"]
        j = win_jump(x, 50, 1150, t60=G.expo(cs[n][3][DECAY], *G.DECAY))
        z = edge_zipper(x, 50, 1150)
        check(j < lim and z < 1.3, f"{n}: a slow turn 0 .. 32767 (1.6 s): neighbouring 70 ms windows within {j:.2f} dB (the decay taken out), "
              f"block-edge steps {z:.2f} x the in-block ones (no zipper)")
    for n in ("fast_fm", "fast_noise"):
        z = edge_zipper(res[n]["x"], 40, 72)
        check(z < 1.3, f"{n}: a detent per block across the whole knob: block-edge steps {z:.2f} x the in-block ones")

    # ---- 12 voices at once ----
    out = []
    for n in poly_cases():
        f = open(os.path.join(w, n + ".err")).read().split()
        ok = open(os.path.join(w, n + ".rc")).read().strip() == "0"
        out.append((n, int(f[1]), int(f[3]), ok))
    worst = max(o[1] for o in out)
    print("  12 voices, cycles per block (sum of 12 render calls) / worst call: "
          + "; ".join(f"{n[5:]} {b} / {c}" for n, b, c, _ in out))
    check(all(o[3] for o in out) and max(o[2] for o in out) <= CPU_BUDGET,
          f"12 voices with FEEDBACK + SQUARE + METAL/GRIND: worst block {worst} cycles ({worst / 128 / 12:.0f} per "
          f"sample per voice), worst call {max(o[2] for o in out)}")


def legacy_match(check):
    """the first four kits: their remapped values decode to what the old scales gave (FM exactly, NOISE's path)"""
    worst_fc, worst_lev, n = 0.0, 0.0, 0
    for name in ("fm 808", "fm 909", "fm metal", "fm zaps"):
        for old, new in zip(G.LEGACY[name], G.KITS[name][1]):
            assert G.old_index_q8(old[FM]) == G.index_q8(new[FM]), (name, old, new)
            a = G.old_noise_point(max(old[NOISE], 0))
            b = G.noise_point(G.sound_k7(new[NOISE]))
            worst_fc = max(worst_fc, abs(math.log2(b["fc"] / a[2])) * 1200)
            worst_lev = max(worst_lev, abs(a[0] - b["tone"]), abs(a[1] - b["level"]), abs(a[5] - b["clap"]))
            assert b["wm"] == 0 and b["wsq"] == 0 and b["ring"] == 0 and b["wx"] == 1.0
            n += 1
    check(worst_fc < 5 and worst_lev < 0.005,
          f"old kits remapped ({n} keys): FM index identical; NOISE path within {worst_fc:.1f} cents (band), "
          f"{worst_lev:.4f} (levels), and never METAL/GRIND")


def ref_match(res, ref, check):
    """--ref DIR: each old kit key against a render from before the change (fmdrum_check --keep): residual, spectra"""
    worst_null, worst_spec, n = -999.0, 0.0, 0
    for name in ("fm 808", "fm 909", "fm metal", "fm zaps"):
        for k in range(24):
            c = f"kit_{name.replace(' ', '_')}_{k}"
            a = np.fromfile(os.path.join(ref, c + ".bin"), dtype="<i2").astype(float)
            b = res[c]["x"].astype(float)
            m = min(len(a), len(b))
            a, b = a[:m], b[:m]
            null = 10 * math.log10(max(((a - b) ** 2).sum(), 1e-9) / max((a ** 2).sum(), 1e-9))
            fa, ma = spectrum(a[:32768])
            _, mb = spectrum(b[:32768])
            bands = [(fa >= lo) & (fa < lo * 2 ** (1 / 3)) for lo in 25 * 2 ** (np.arange(29) / 3)]
            da = [10 * math.log10(max((ma[s] ** 2).sum(), 1e-9)) for s in bands]
            db = [10 * math.log10(max((mb[s] ** 2).sum(), 1e-9)) for s in bands]
            top = max(da)
            spec = max(abs(x - y) for x, y in zip(da, db) if x > top - 40)
            worst_null, worst_spec = max(worst_null, null), max(worst_spec, spec)
            n += 1
    check(worst_spec < 0.5, f"old kits against the reference renders ({n} keys): residual at most {worst_null:.1f} dB "
          f"re the signal, third-octave spectra (within 40 dB of the top band) within {worst_spec:.2f} dB")


def spectrum(x, f0=None):
    x = x.astype(float)
    w = np.hanning(len(x))
    n = 1 << max(16, int(math.ceil(math.log2(len(x)))))
    m = np.abs(np.fft.rfft(x * w, n)) / (w.sum() / 2)
    return np.fft.rfftfreq(n, 1 / FS), m


def peak_freq(x):
    fr, m = spectrum(x)
    i = int(np.argmax(m[1:])) + 1
    a, b, c = m[i - 1], m[i], m[i + 1]
    return (i + 0.5 * (a - c) / (a - 2 * b + c)) * FS / (2 * (len(m) - 1))


def level(fr, m, f, bw=8.0):
    sel = np.abs(fr - f) <= bw
    return m[sel].max() if sel.any() else 0.0


def bessel(k, x, terms=40):
    k = abs(k)
    return sum((-1) ** m / (math.factorial(m) * math.factorial(m + k)) * (x / 2) ** (2 * m + k) for m in range(terms))


def peak_groups(x):
    """bursts in the first 35 ms: RMS peaks above 0.3 of the largest, more than 3.4 ms apart"""
    x = x[:int(0.035 * FS)].astype(float)
    e = np.sqrt(np.convolve(x ** 2, np.ones(88) / 88, "same"))
    peaks = [i for i in range(1, len(e) - 1) if e[i] >= e[i - 1] and e[i] > e[i + 1] and e[i] > 0.3 * e.max()]
    return len([p for j, p in enumerate(peaks) if j == 0 or p - peaks[j - 1] > 300])


def t60_index(x, start=0):
    """samples from the peak until the 2 ms RMS last falls below peak RMS - 60 dB"""
    e = np.sqrt(np.convolve(x.astype(float) ** 2, np.ones(176) / 176, "same"))
    p = int(np.argmax(e))
    below = np.nonzero(e[p:] < e[p] * 1e-3)[0]
    return (below[0] if len(below) else len(x) - p) / FS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep")
    ap.add_argument("--ref", help="a --keep folder from before a change: the old kits must render the same")
    a = ap.parse_args()
    w = tempfile.mkdtemp(prefix="fmdrum_")
    cs = cases()
    cflags = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
    cmds = [f"bfin-elf-run /tmp/m {k} {v} {b} {' '.join(map(str, p))} > /w/{n}.bin 2> /w/{n}.err; echo $? > /w/{n}.rc"
            for n, (k, v, b, p) in cs.items()]
    cmds += [f"bfin-elf-run /tmp/mc {k} {v} {b} {' '.join(map(str, p))} > /w/{n}.cref 2> /dev/null"     # C reference
             for n, (k, v, b, p) in cs.items()]
    cmds.append("bfin-elf-run /tmp/m init > /w/init.txt 2> /w/init.err; echo $? > /w/init.rc")
    for n, (k1, k2, p1, p2) in CHOKE.items():
        cmds.append(f"bfin-elf-run /tmp/m choke {k1} {k2} 20 200 {' '.join(map(str, p1 + p2))} > /w/{n}.bin "
                    f"2> /w/{n}.err; echo $? > /w/{n}.rc")
    for n, rows in poly_cases().items():
        cmds.append(f"bfin-elf-run /tmp/m poly 60 {len(rows)} {' '.join(str(v) for r in rows for v in r)} > /w/{n}.bin "
                    f"2> /w/{n}.err; echo $? > /w/{n}.rc")
    open(os.path.join(w, "cmds"), "w").write("\n".join(cmds) + "\n")
    open(os.path.join(w, "run.sh"), "w").write(
        "set -e; cd /tmp\n"
        f"bfin-elf-gcc {cflags} -I /engine -I /src -c -o fd.o /src/fmdrum.c\n"
        "bfin-elf-nm -u fd.o > /w/undef.txt\n"
        "bfin-elf-objdump -d fd.o > /w/fd.dis\n"
        "bfin-elf-as -I /src --defsym NO_FX=1 -o abi.o /src/test/fx_abi.S\n"
        "bfin-elf-as -I /src -o entries.o /engine/test/fmdrum_abi.S\n"
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -I /src -I /engine -o m /engine/test/fmdrum_main.c fd.o abi.o entries.o\n"
        f"bfin-elf-gcc {cflags} -U__bfin__ -I /engine -I /src -c -o fdc.o /src/fmdrum.c\n"   # the C loops, no asm
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -I /src -I /engine -o mc /engine/test/fmdrum_main.c fdc.o abi.o entries.o\n"
        "xargs -P 8 -I CMD sh -c CMD < /w/cmds\n")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v",
                    f"{ENGINE}:/engine:ro", "-v", f"{w}:/w", "op1-bfin", "sh", "/w/run.sh"], check=True)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    res = {}
    for n in cs:
        err = open(os.path.join(w, n + ".err")).read()
        f = err.split()
        x = np.fromfile(os.path.join(w, n + ".bin"), dtype="<i2").astype(np.int64)
        res[n] = dict(x=x, cyc=int(f[2]), guard=f[4], vptr=f[6], end=int(f[8]),
                      rc=open(os.path.join(w, n + ".rc")).read().strip(), err=err)

    # ---- build, ABI, cost ----
    undef = set(open(os.path.join(w, "undef.txt")).read().split()) - {"U"}
    check(undef <= {"_te_drum_key", "_te_drum_vel", "_ranges", "_fmdrum_id"},
          f"no runtime division or other libgcc calls (undefined: {', '.join(sorted(undef))})")
    abi = all(r["rc"] == "0" for r in res.values())
    check(abi, f"ABI: {sum(3 + cs[n][2] * 2 for n in cs)} entry calls preserve R4-R7, P3-P5, FP, SP, loops, L"
          + ("" if abi else " (" + "; ".join(r["err"].strip() for r in res.values() if r["rc"] != "0")[:300] + ")"))
    check(all(r["guard"] == "5a5a5a5a" for r in res.values()), "output guard intact (128 samples per call)")
    check(all(r["vptr"] == "0190f8a4" for r in res.values()), "TE's vptr never written")
    worst = max(r["cyc"] for r in res.values())
    who = max(res, key=lambda n: res[n]["cyc"])
    check(worst <= CPU_BUDGET, f"cycles per render call: max {worst} = {worst / 128:.0f} per sample ({who}; budget "
          f"{CPU_BUDGET} = 600 per sample per voice)")

    same = [n for n in cs if np.array_equal(np.fromfile(os.path.join(w, n + ".cref"), dtype="<i2"), res[n]["x"])]
    check(len(same) == len(cs), f"the asm loops render bit for bit what fmdrum.c's C loops do ({len(same)} of {len(cs)} "
          "cases)" + ("" if len(same) == len(cs) else ": not " + ", ".join(sorted(set(cs) - set(same))[:8])))

    # ---- pitch ----
    errs = []
    for s in (-24, -12, 0, 7, 12, 24, 36):
        x = res[f"pitch{s}"]["x"]
        f = peak_freq(x[FS // 10:FS // 10 + 16384])
        errs.append(abs(1200 * math.log2(f / (220 * 2 ** (s / 12)))))
    check(max(errs) < 3, f"K0 pitch, 512 per semitone from A3 = 220 Hz, -24..+36 semitones: worst {max(errs):.2f} cents")
    lo, hi = peak_freq(res["pitch_min"]["x"][FS // 10:FS // 10 + 32768]), peak_freq(res["pitch_max"]["x"][FS // 10:FS // 10 + 16384])
    check(abs(lo - G.pitch_hz(G.K0_MIN)) < 0.3 and abs(1200 * math.log2(hi / G.pitch_hz(G.STOCK_HI[0]))) < 3,
          f"K0 range: {lo:.1f} Hz .. {hi:.0f} Hz")

    # ---- sweep ----
    def inst_freq(x, a, b):         # zero crossings per second in samples a..b
        y = x[a:b]
        z = np.nonzero(np.diff(np.signbit(y).astype(int)))[0]
        return (len(z) - 1) / 2 / ((z[-1] - z[0]) / FS) if len(z) > 2 else 0.0
    x = res["sweep"]["x"]
    k = cs["sweep"][3]
    tau, semis, f0 = G.expo(k[SWDEC], *G.SNAP), G.sweep_semis(k[SWEEP]), G.pitch_hz(k[PITCH])

    def model(a, b):                # mean frequency of f0 x 2^(semis e^(-t / tau) / 12) over samples a..b
        t = np.arange(a, b) / FS
        return float(np.mean(f0 * 2 ** (semis * np.exp(-t / tau) / 12)))
    sw_err = max(abs(1200 * math.log2(inst_freq(x, a_, b_) / model(a_, b_))) for a_, b_ in
                 ((0, 441), (882, 1764), (2646, 4410), (8820, 13230)))
    f_late = inst_freq(x, FS // 4, FS // 4 + 4410)
    check(sw_err < 40 and abs(1200 * math.log2(f_late / f0)) < 10,
          f"K4/K5 sweep +{semis:.1f} st, tau {tau * 1000:.0f} ms: follows f0 2^(S e^(-t/tau) / 12) within {sw_err:.0f} "
          f"cents over 0..150 ms; {f_late:.1f} Hz after 250 ms (pitch {f0:.1f})")
    x = res["sweep_none"]["x"]
    check(abs(1200 * math.log2(inst_freq(x, 0, 2205) / 110)) < 30, "no sweep: the pitch from the start")

    # ---- FM spectra: sidebands fc + k fm with |J_k(I)| ----
    worst_db = 0.0
    for r, fm in ((3.5, 1.0), (3.5, 2.4), (2.7565, 4.0), (1.4142, 6.0)):
        k = P(hz=220, ratio=r, fm=fm, mod=3.0)
        fc = G.pitch_hz(k[PITCH])
        fmod = fc * G.ratio_of(k[RATIO])
        x = res[f"fm_{r}_{fm}"]["x"]
        a0, n = 441, 4096                                    # 5 ms .. 51 ms
        mid = (a0 + n / 2) / FS
        menv = 10 ** (-3 * mid / G.expo(k[FMDEC], *G.MODDEC))   # the modulator envelope at the window's centre
        I = G.fm_rad(k[FM]) * (16384 + (32512 >> 1)) / 32768 * menv
        fr, m = spectrum(x[a0:a0 + n])
        got, want = [], []
        for j in range(-3, 4):
            f = fc + j * fmod
            if abs(f) < 30:
                continue
            got.append(level(fr, m, abs(f), 25))
            want.append(abs(bessel(j, I)))
        g, wv = np.array(got), np.array(want)
        g, wv = g / g.max(), wv / wv.max()
        sel = wv > 0.1
        d = np.abs(20 * np.log10(g[sel] / wv[sel]))
        worst_db = max(worst_db, d.max())
    check(worst_db < 1.5, f"FM sidebands = Bessel J_k(I) of the tables' index (ratios 3.5, 2.76, 1.41; I 1..6 rad): "
          f"worst {worst_db:.2f} dB")

    # ---- modulator decay ----
    def bright(x, a, b, fc):
        fr, m = spectrum(x[a:b])
        return float((m[fr > 1.5 * fc] ** 2).sum() / max((m ** 2).sum(), 1e-9))
    f_ = res["mod_fast"]["x"]
    s_ = res["mod_slow"]["x"]
    bf, bs = bright(f_, 4410, 8820, 220), bright(s_, 4410, 8820, 220)
    check(bf < 1e-3 and bs > 0.3, f"K7 modulator decay: sideband energy 50..100 ms {bf:.1e} (3 ms) vs {bs:.2f} (3 s)")

    # ---- aliasing ----
    worst_a, alias_db = -999.0, []
    for i in range(8):
        k = cs[f"alias{i}"][3]
        x = res[f"alias{i}"]["x"][4410:4410 + 16384]
        fc = G.pitch_hz(k[PITCH])
        fmod = fc * G.ratio_of(k[RATIO])
        win = np.blackman(len(x))                           # low sidelobes: many strong sidebands
        m = np.abs(np.fft.rfft(x.astype(float) * win, 1 << 16))
        fr = np.fft.rfftfreq(1 << 16, 1 / FS)
        true_ = np.zeros_like(m, dtype=bool)
        for j in range(-200, 201):
            f = abs(fc + j * fmod)
            if f < FS / 2:
                true_ |= np.abs(fr - f) <= 40                # Blackman main lobe and the decaying index's spread
        band = fr < 20000
        e_true, e_other = (m[band & true_] ** 2).sum(), (m[band & ~true_] ** 2).sum()
        ar = 10 * math.log10(max(e_other, 1e-12) / max(e_true, 1e-12))
        worst_a = max(worst_a, ar)
        alias_db.append(ar)
    print("  aliasing per case (dB):", " ".join(f"{v:.0f}" for v in alias_db))
    check(worst_a < -40, f"aliasing at the extremes (pitch up to 3.5 kHz, ratios to 15.5, FM 16 rad): energy off the "
          f"true sidebands in 0..20 kHz {worst_a:.1f} dB")

    # ---- DC ----
    x = res["dc"]["x"]
    seg = x[FS // 2:FS * 3 // 2].astype(float)
    dc = abs(seg.mean()) / np.abs(seg).max()
    fr, m = spectrum(x[:FS // 2])           # without the blocker the 0 Hz sideband J_1(2.4) would be ~ 0.5 x the peak
    check(dc < 0.02, f"DC with a 1:1 ratio at 2.4 rad (a sideband at 0 Hz, decaying with the index): mean "
          f"{100 * dc:.2f} % of the peak over 0.5 .. 1.5 s")
    dcs = []
    for n in cs:
        if n.startswith("kit_"):
            x = res[n]["x"].astype(float)
            dcs.append(abs(x.sum()) / max(np.abs(x).sum(), 1))
    check(max(dcs) < 0.02, f"DC over every kit key's whole note: |sum| / sum |x| at most {100 * max(dcs):.2f} %")

    # ---- amplitude decay and the voice's end ----
    for n, want in (("decay_min", G.DECAY[0]), ("decay_mid", 0.25), ("decay_max", G.DECAY[1])):
        x = res[n]["x"]
        t = t60_index(x)
        end = res[n]["end"]
        tail = x[(end + 1) * 128:] if end >= 0 else np.array([1])
        check(abs(t - want) < 0.15 * want + 0.004 and end > 0 and not np.abs(tail).any(),   # + 0.004: block ramps
              f"K3 decay {want * 1000:.0f} ms: -60 dB after {t * 1000:.0f} ms; active() 0 after block {end}, then silence")

    # ---- noise zones ----
    def centroid(x):
        fr, m = spectrum(x[:8192])
        sel = (fr > 200) & (fr < 20000)
        return float((fr[sel] * m[sel] ** 2).sum() / max((m[sel] ** 2).sum(), 1e-9))
    cen = {z: centroid(res[f"noise_{z}"]["x"]) for z in ("air", "snare", "hat")}
    check(cen["air"] < cen["snare"] < cen["hat"], f"K6 colour: centroid AIR {cen['air']:.0f} < SNARE "
          f"{cen['snare']:.0f} < HAT {cen['hat']:.0f} Hz")
    fr, m = spectrum(res["noise_off"]["x"][:8192])
    hf_off = (m[fr > 500] ** 2).sum() / (m ** 2).sum()
    check(hf_off < 1e-6, f"K6 OFF: no noise ({10 * math.log10(max(hf_off, 1e-12)):.0f} dB above 500 Hz)")

    def tone50(n):
        fr, m = spectrum(res[n]["x"][2048:2048 + 16384])
        return level(fr, m, 50, 4)
    t_off, t_top, t_clap = tone50("noise_off"), tone50("noise_hat_top"), tone50("noise_clap")
    check(t_top < 0.1 * t_off and t_clap < 0.02 * t_off, f"K6: the tone fades through HAT ({t_top / t_off:.3f}) and is "
          f"gone in CLAP ({t_clap / t_off:.4f})")
    groups = peak_groups(res["noise_clap"]["x"])
    check(groups >= 4, f"CLAP: {groups} bursts in the first 35 ms (want 4: three short, then the tail)")

    # ---- velocity ----
    p127, p64 = np.abs(res["vel127"]["x"]).max(), np.abs(res["vel64"]["x"]).max()
    check(0.45 < p64 / p127 < 0.56, f"velocity 64 vs 127: level ratio {p64 / p127:.2f}")

    # ---- clicks: knob changes and a retrigger, against the largest step in the blocks around ----
    for n, b in (("chg_pitch", 40), ("chg_fm", 40), ("chg_noise", 40), ("retrig", 30)):
        x = res[n]["x"]
        d = np.abs(np.diff(x))
        at = d[b * 128 - 2:b * 128 + 130].max()
        around = max(d[(b - 4) * 128:(b - 1) * 128].max(), d[(b + 2) * 128:(b + 5) * 128].max())
        check(at <= 1.25 * around, f"{n}: largest step at the change {at} vs {around} around it (no click)")
    x = res["retrig"]["x"]
    p0, p1 = np.abs(x[:30 * 128]).max(), np.abs(x[30 * 128:]).max()
    check(abs(p1 / p0 - 1) < 0.1, f"retrigger restarts the note (peak {p1} vs first {p0})")

    # ---- the factory kits ----
    pk, nc, quiet = [], 0, []
    for n in cs:
        if n.startswith("kit_"):
            x = res[n]["x"]
            pk.append(int(np.abs(x).max()))
            nc += int((np.abs(x) >= 32000).sum())
            if pk[-1] < 3300:
                quiet.append(n)
    total = sum(len(res[n]["x"]) for n in cs if n.startswith("kit_"))
    check(not quiet and nc < total * 2e-4, f"factory kits ({len(pk)} keys): peaks {20 * math.log10(min(pk) / 32768):.1f} "
          f"to {20 * math.log10(max(pk) / 32768):.1f} dBFS, {nc} samples >= 32000 of {total}"
          + (f"; quiet: {', '.join(quiet)}" if quiet else ""))
    ind = [n for n in cs if n.startswith("kit_fm_industrial_")]
    ipk = max(int(np.abs(res[n]["x"]).max()) for n in ind)
    inc = sum(int((np.abs(res[n]["x"]) >= 32000).sum()) for n in ind)
    check(ipk < 32000 and inc == 0, f"fm industrial ({len(ind)} keys): peak {20 * math.log10(ipk / 32768):.1f} dBFS, no "
          f"sample in the soft knee's top ({inc} >= 32000)")
    # ---- hi-hat choke (keys 10..13, HAT zone) ----
    ends = {n: int(open(os.path.join(w, n + ".err")).read().split()[1]) for n in CHOKE}
    ok_rc = all(open(os.path.join(w, n + ".rc")).read().strip() == "0" for n in CHOKE)
    x = np.fromfile(os.path.join(w, "choke_hat.bin"), dtype="<i2").astype(np.int64)
    d = np.abs(np.diff(x[20 * 128 - 4:22 * 128]))
    check(ok_rc and 20 <= ends["choke_hat"] <= 25 and 20 <= ends["choke_same"] <= 25
          and ends["choke_outside"] < 0 and ends["choke_notzone"] < 0 and ends["choke_victim"] < 0
          and d.max() < 2 * np.abs(np.diff(x[18 * 128:20 * 128])).max(),
          f"hi-hat choke: open hat ends {ends['choke_hat'] - 20} blocks ({(ends['choke_hat'] - 19) * 1.45:.0f} ms) after a "
          f"closed hit (same key {ends['choke_same'] - 20}), no click; not choked by key 14, by a SNARE-zone hit, or "
          f"without HAT noise (ends {ends['choke_outside']}, {ends['choke_notzone']}, {ends['choke_victim']})")
    xm = np.fromfile(os.path.join(w, "choke_metal.bin"), dtype="<i2").astype(np.int64)
    dm = np.abs(np.diff(xm[20 * 128 - 4:22 * 128]))
    check(20 <= ends["choke_metal"] <= 25 and 20 <= ends["choke_mixed"] <= 25 and ends["choke_grind"] < 0
          and dm.max() < 2 * np.abs(np.diff(xm[18 * 128:20 * 128])).max(),
          f"METAL hats on 10..13 choke too: METAL by METAL {ends['choke_metal'] - 20} blocks, METAL by HAT "
          f"{ends['choke_mixed'] - 20}, no click; a GRIND key rings on ({ends['choke_grind']})")

    # ---- knob layout and tables ----
    rs = G.RATIOS
    check(rs[:15] == sorted(rs[:15]) and rs[15:] == sorted(rs[15:]) and all(r in G.HARMONIC for r in rs[:15])
          and not any(r in G.HARMONIC for r in rs[15:]) and len(rs) == 32,
          "RATIO: 15 harmonic ratios rising, then 17 inharmonic rising, one per 1024")
    check(G.noise_zone(G.STOCK_HI[NOISE]) == 6 and np.abs(res["noise_stockmax"]["x"]).max() > 1000,
          f"NOISE at the stock maximum {G.STOCK_HI[NOISE]} is GRIND (and sounds)")
    roles_ok = all(G.noise_zone(kit[1][k][NOISE]) in (3, 5) for kit in G.KITS.values() for k in range(10, 14))
    check(roles_ok, "every kit's keys 10..13 are hats in the HAT or METAL zone (the choke group)")
    edges = [G.noise_zone(G.NOISE_EDGES[z]) == z and G.noise_zone(G.NOISE_EDGES[z] - 1024) == z - 1 for z in range(1, 7)]
    lv = [G.fm_level(d * 1024) for d in range(20, 33)]
    check(all(edges) and all(e % 1024 == 0 for e in G.NOISE_EDGES) and G.FM_FB0 % 1024 == 0 and G.FM_SQ0 % 1024 == 0
          and G.fm_zone(G.FM_FB0) == 0 and G.fm_zone(G.FM_FB0 + 1024) == 1 and G.fm_zone(G.FM_SQ0) == 1
          and G.fm_zone(G.FM_SQ0 + 1024) == 2 and lv == [0, 1, 2, 3, 4, 5, 6, 7, 1, 2, 3, 4, 5]
          and G.noise_point(G.sound_k7(0))["level"] == 0 and G.sound_k7(31 * 1024) < G.sound_k7(32767) == 32767,
          "zones start on detents (1024): NOISE " + " ".join(f"{z} {e // 1024}" for z, e in zip(G.NOISE_ZONES, G.NOISE_EDGES))
          + f"; FM 16 rad at 20, FEEDBACK 1..7 at 21..27, SQUARE 1..5 at 28..32 (levels {lv}); detent 0 silent")
    joins = []
    for z in range(1, 7):                   # every zone starts where the one below ends (the path's parameters)
        a_, b_ = G.noise_point(G.NOISE_EDGES[z] - 1), G.noise_point(G.NOISE_EDGES[z])
        d_ = max(abs(a_[c] - b_[c]) for c in ("tone", "level", "q", "clap", "wx", "wm", "wsq", "ring", "gu"))
        d_ = max(d_, max(abs(x_ - y_) for x_, y_ in zip(a_["blend"], b_["blend"])),
                 abs(math.log2(a_["fc"] / b_["fc"])), abs(a_.get("mhp", 0) - b_.get("mhp", 0)) / 3000)
        joins.append(d_)
    check(max(joins) < 0.01, "NOISE zones join in their parameters (largest jump " + " ".join(f"{v:.3f}" for v in joins)
          + "; detent 31 and 32 differ, 32 = the top)")
    harsh(res, cs, check, w)
    legacy_match(check)
    if a.ref:
        ref_match(res, a.ref, check)

    r = open(os.path.join(w, "init.txt")).read().split()
    check(open(os.path.join(w, "init.rc")).read().strip() == "0" and r == ["kept", "1", "row", "1", "norow", "1"],
          f"slot init: loaded per-key values never rewritten, row 9 copied only when sane ({' '.join(r)})")

    if a.keep:
        shutil.copytree(w, a.keep, dirs_exist_ok=True)
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
