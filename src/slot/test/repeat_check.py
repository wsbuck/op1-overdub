#!/usr/bin/env python3
"""Simulator checks of the repeat FX (src/slot/repeat.c; notes/fx-beatrepeat.md) on bfin-elf-run (repeatsim.py).

  python3 src/slot/test/repeat_check.py [OUTDIR]

Signals: a time code (stereo: L = t mod 2^15, R = t >> 15, so every output sample names the input sample it read; a
crossfade mixes two codes and is skipped), sines, and silence. TE's transport and metronome are modelled by
repeatsim.Tape. Checks:
  1. grid timing: every slice boundary of 1/64 .. 1/2 (triplets too) at 40, 60, 80.8, 120, 133.3 and 220 BPM within
     one sample of the exact time, and each repeat reads the slice's start;
  2. bar alignment: from an odd tape position, after a locate back, on the metronome with the tape stopped (the gate
     ends on a click-aligned bar line), and a tape that stops mid-gate (the gate keeps its length);
  3. CHANCE fills seeded by the bar: the same bars fire on a second pass over the same tape bars; another sound slot
     or the master draws others;
  4. each FORTUNE mode: OFF never, EVERY 8 / 4 / 2 / 1 on the right bars, CHANCE's rate over 200 bars (per beat at a
     quarter of the chance with GATE <= 1/4), VARY's rate
     and picks (blue's family, within +-a, at most half the gate), ALWAYS on a 1/16 line (at once within 30 ms
     after one) and its release within one crossfade;
  5. gate lengths for every GATE value and the grid-wins stretch;
  6. the pitch staircase (whole semitones): each repeat's frequency, down and up, and the gain / low-pass with the drop;
  7. TAPE STOP: the read decelerates 1 -> 0 behind the write (never ahead), fades out; the looped version at a slow
     tempo; SPIN UP's rise;
  8. no clicks: every output step within what the read rate and a 32-sample crossfade allow, across knob settings,
     knob jumps and an LFO on every knob;
  9. pingpong's lines: neither FX ever plays what the other left (exact zeros), garbage at boot never heard, the
     stereo and mono sets independent;
 10. invalid words: tempo 0 / garbage, transport and playhead garbage, a dead metronome;
 11. no DC: dry exactly after every gate, silence after a TAPE STOP and a latched one, the HP's state; the FX adds
     no offset or rounding bias of its own (an input and its negation give opposite outputs);
 12. the fold at slow tempos; stereo channels independent;
 13. CYCLES per call (mono, stereo; idle and the heaviest settings).
"""
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "..", "design", "fx"))
from repeatsim import Case, Tape, raw, run, spb_of   # noqa: E402

FS = 44100
GRIDS = [(1, 64, "1/64"), (3, 48, "1/32T"), (6, 32, "1/32"), (9, 24, "1/16T"), (12, 16, "1/16"), (15, 12, "1/8T"),
         (18, 8, "1/8"), (21, 6, "1/4T"), (24, 4, "1/4"), (27, 3, "1/2T"), (30, 2, "1/2")]
GATES = [(0, 1), (3, 2), (6, 3), (9, 4), (12, 6), (15, 8), (18, 12), (21, 16), (24, 32), (27, 64)]
EVERY_BAR, OFF, ALWAYS = raw(19), raw(1), 0
FLAT = raw(16)
DC_CASES = [("dc_flat", "FLAT"), ("dc_down", "-12"), ("dc_up", "+12"), ("dc_stop", "TAPE STOP"), ("dc_spin", "SPIN UP"),
            ("dc_buzz", "1/64 -7")]
DC_KNOBS = {"dc_flat": (12, raw(16)), "dc_down": (12, raw(28)), "dc_up": (12, raw(3)), "dc_stop": (12, raw(31)),
            "dc_spin": (12, raw(1)), "dc_buzz": (1, raw(26))}
fails = []


def check(ok, msg):
    print(("PASS " if ok else "FAIL ") + msg)
    if not ok:
        fails.append(msg)


def code(n, t0=0):
    t = np.arange(t0, t0 + n)
    return np.stack([(t & 0x7FFF) - 16384, ((t >> 15) & 0x7FFF) - 16384], axis=1).astype(np.int16)


def decode(out):
    return (out[:, 0] + 16384) + ((out[:, 1] + 16384) << 15)


def sine(n, f, a=16000, ph=0.0):
    return np.round(a * np.sin(2 * np.pi * f * np.arange(n) / FS + ph)).astype(np.int16)


def bar_of(bpm):
    return 4 * spb_of(bpm) / 16384


def eff16(n, g):
    return min(64, max(g, math.ceil(32 / n)))


def fold_len(n, bpm):
    """slices per bar after the fold, and the slice in samples"""
    L = bar_of(bpm) / n
    while L > 65280:
        L, n = L / 2, n * 2
    return n, L


def jumps(src, lo, hi):
    """samples in [lo, hi) where the read is not one past the previous sample's (slice boundaries, wraps)"""
    d = np.diff(src[lo - 1:hi])
    return lo + np.nonzero(d != 1)[0]


def boundaries(src, lo, hi):
    """slice starts: the first sample of each run of non-continuing reads (a crossfade is 32 such samples)"""
    j = jumps(src, lo, hi)
    out = []
    for x in j:
        if not out or x - out[-1] > 40:
            out.append(int(x))
    return out


def stereo_case(inp, k, tape, blocks):
    c = Case(inp, "s")
    c.run_tape(k, tape, blocks)
    return c


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else None
    cases, notes = {}, {}

    # ---- 1. grid timing (EVERY BAR, GATE 1 BAR: a fresh gate every bar; stereo time code) ----
    BPMS = [40, 60, 80.8, 120, 133.3, 220]
    for bpm in BPMS:
        for p, n, name in GRIDS:
            bars = 2 if bpm >= 80 else 1
            nb = int((bars + 1.2) * bar_of(bpm) / 64) + 2
            k = [raw(p), EVERY_BAR, raw(21), FLAT]
            cases[f"grid_{bpm}_{n}"] = stereo_case(code(nb * 64), k, Tape(bpm), nb)

    # ---- 2. bar alignment ----
    bpm = 120
    B = bar_of(bpm)
    k_fill = [raw(12), EVERY_BAR, raw(9), FLAT]                    # 1/16, every bar, 4/16
    t = Tape(bpm, pos=int(2.37 * B) + 123)
    c = Case(code(int(6 * B)), "s")
    c.run_tape(k_fill, t, int(2.5 * B / 64))
    t.pos = int(0.31 * B) * 64                                      # a locate back
    c.run_tape(k_fill, t, int(2.5 * B / 64))
    cases["align_tape"] = c
    t = Tape(bpm, rolling=False)
    t.c = 7777 << 14                                                # the metronome at an odd phase
    cases["align_metro"] = stereo_case(code(int(5 * B)), k_fill, t, int(4.5 * B / 64))
    t = Tape(bpm, pos=int(1.5 * B))
    c = Case(code(int(5 * B)), "s")
    k_long = [raw(12), raw(5), raw(24), FLAT]                       # EVERY 4 BARS, GATE 2 BARS: from bar 2 to 4
    c.run_tape(k_long, t, int(1.0 * B / 64))                        # the gate starts at 0.5 bars ...
    t.stop()                                                        # ... the tape stops at 1 bar
    c.run_tape(k_long, t, int(3.0 * B / 64))
    cases["stop_midgate"] = c

    # REPEAT1 on the device: with the tape stopped and TE's metronome in tape-follow mode the countdown holds, and no
    # fill ever fired. From boot, and after the tape played and stopped (bars continue the tape's count)
    t = Tape(bpm, rolling=False, follow=True)
    cases["metro_held"] = stereo_case(code(int(4.5 * B)), k_fill, t, int(4.2 * B / 64))
    t = Tape(bpm, follow=True)
    c = Case(code(int(5 * B)), "s")
    c.run_tape([raw(12), raw(7), raw(9), FLAT], t, int(1.5 * B / 64))      # EVERY 2 BARS
    t.stop()
    c.run_tape([raw(12), raw(7), raw(9), FLAT], t, int(3.2 * B / 64))
    cases["metro_held_after_tape"] = c
    # the interval's phase (REPEAT1 report: "the fill lands one bar early"): EVERY n from tape bar 1 (position 0), after
    # a stop with a held metronome, and from a non-zero start (relative to tape bar 1, not to where play started)
    for n, kf in ((2, raw(7)), (4, raw(5))):
        t = Tape(bpm, rolling=False, follow=True)
        c = Case(code(int(10 * B)), "s")
        c.run_tape([raw(12), kf, raw(9), FLAT], t, int(0.7 * B / 64))        # stopped first, as on the device
        t.play(0)
        c.run_tape([raw(12), kf, raw(9), FLAT], t, int(9 * B / 64))
        cases[f"phase_every{n}"] = c
        notes[f"phase_every{n}"] = int(0.7 * B / 64) * 64
    t = Tape(bpm, pos=int(2.5 * B))
    cases["phase_offset"] = stereo_case(code(int(6 * B)), [raw(12), raw(5), raw(9), FLAT], t, int(5.8 * B / 64))

    # ---- 3. CHANCE seeded by the bar ----
    k_ch = [raw(12), raw(13), raw(9), FLAT]                         # CHANCE 50 %
    for name, slot, mode in (("seed_a", 0, "m"), ("seed_b", 3, "m"), ("seed_m", 0, "s")):
        t = Tape(bpm)
        t.slot = slot
        c = Case(sine(int(33 * B), 220), mode)
        c.run_tape(k_ch, t, int(16 * B / 64))
        t.pos = 0
        c.run_tape(k_ch, t, int(16 * B / 64))
        cases[name] = c

    # ---- 4. FORTUNE modes (fast tempo: many bars) ----
    fb = 220
    FB = bar_of(fb)
    modes = {"off": raw(1), "every8": raw(3), "every4": raw(5), "every2": raw(7), "every1": raw(19),
             "chance10": raw(9), "chance30": raw(11), "chance90": raw(17), "vary1": raw(21), "vary2": raw(24),
             "vary3": raw(27), "vary4": raw(30)}
    for name, kf in modes.items():
        nbars = 200 if name.startswith(("chance", "vary")) else 40
        cases[f"f_{name}"] = stereo_case(code(int((nbars + 1) * FB)), [raw(12), kf, raw(9), FLAT], Tape(fb),
                                         int(nbars * FB / 64))
    # VARY on a long gate (picks visible): 1/16, VARY 4, GATE 1 BAR
    cases["vary_picks"] = stereo_case(code(int(41 * FB)), [raw(12), raw(31), raw(21), FLAT], Tape(fb), int(40 * FB / 64))
    cases["vary_picks_t"] = stereo_case(code(int(41 * FB)), [raw(15), raw(31), raw(21), FLAT], Tape(fb),
                                        int(40 * FB / 64))
    # ALWAYS: engaged at several phases (stopped, on the metronome), then released
    for j, ph in enumerate((0.0, 0.004, 0.02, 0.031, 0.05)):
        t = Tape(bpm, rolling=False)
        t.c = spb_of(bpm)                                           # a click at the start: our beat line at 0
        c = Case(code(int(3 * B)), "s")
        lead = int((0.5 * B / 16 * 0 + ph * FS) / 64) + 30          # dry blocks first
        c.run_tape([raw(12), OFF, raw(9), FLAT], t, lead)
        c.run_tape([raw(12), ALWAYS, raw(9), FLAT], t, int(1.0 * B / 64))
        c.run_tape([raw(12), OFF, raw(9), FLAT], t, int(0.5 * B / 64))
        cases[f"always_{j}"] = c
        notes[f"always_{j}"] = lead

    # ---- 5. gate lengths, the grid wins ----
    for gp, g in GATES:
        for p, n, name in ((1, 64, "1/64"), (12, 16, "1/16"), (24, 4, "1/4"), (30, 2, "1/2"), (27, 3, "1/2T")):
            e = eff16(n, g)
            nbars = 2 + e // 16 + 1
            k = [raw(p), raw(3), raw(gp), FLAT]                     # EVERY 8 BARS: one gate, ending on bar 8
            t = Tape(220)
            cases[f"gate_{g}_{n}"] = stereo_case(code(int((8.3) * bar_of(220))), k, t, int(8.2 * bar_of(220) / 64))

    # ---- 6. pitch staircase (1/8 at 120: 8 slices in a 1-bar gate: 7 repeats; a 441 Hz sine) ----
    for pp in (3, 5, 7, 9, 11, 13, 18, 20, 22, 24, 26, 28):
        c = Case(sine(int(2.2 * B), 441), "m")
        c.run_tape([raw(18), raw(3), raw(21), raw(pp)], Tape(bpm, pos=int(6 * B)), int(2.1 * B / 64))
        cases[f"pitch_{pp}"] = c

    # ---- 7. TAPE STOP, looped, SPIN UP ----
    c = Case(code(int(2.3 * B)), "s")
    c.run_tape([raw(12), raw(3), raw(21), raw(31)], Tape(bpm, pos=int(6 * B)), int(2.2 * B / 64))
    cases["tapestop"] = c
    c = Case(sine(int(2.3 * B), 441), "m")
    c.run_tape([raw(12), raw(3), raw(21), raw(31)], Tape(bpm, pos=int(6 * B)), int(2.2 * B / 64))
    cases["tapestop_sine"] = c
    B60 = bar_of(60)
    c = Case(sine(int(2.3 * B60), 441), "m")
    c.run_tape([raw(12), raw(3), raw(21), raw(31)], Tape(60, pos=int(6 * B60)), int(2.2 * B60 / 64))
    cases["tapestop_loop"] = c
    c = Case(sine(int(2.3 * B), 441), "m")
    c.run_tape([raw(18), raw(3), raw(21), raw(1)], Tape(bpm, pos=int(6 * B)), int(2.2 * B / 64))
    cases["spinup"] = c
    t = Tape(bpm, rolling=False)
    c = Case(np.concatenate([sine(int(0.5 * B), 441), np.zeros(int(2 * B), np.int16)]), "m")
    c.run_tape([raw(12), ALWAYS, raw(21), raw(31)], t, int(2.4 * B / 64))
    cases["tapestop_hold"] = c

    # ---- 8. clicks: sines through knob settings, jumps and an LFO ----
    rng = np.random.default_rng(7)
    combos = []
    for p, n, _ in GRIDS[::2]:
        for kf in (raw(19), raw(31), ALWAYS):
            for kp in (raw(1), raw(3), FLAT, raw(22), raw(28), raw(31)):
                combos.append((p, kf, kp))
    for j, (p, kf, kp) in enumerate(combos):
        f = [110, 441, 2500][j % 3]
        c = Case(sine(int(2.2 * B), f, 16000), "s" if j % 2 else "m")
        c.run_tape([raw(p), kf, raw(15), kp], Tape(bpm, pos=int(3.5 * B)), int(2.1 * B / 64))
        cases[f"click_{j}"] = c
        notes[f"click_{j}"] = (f, kp)
    for j in range(6):                                              # knob jumps every ~100 ms, then an LFO
        c = Case(sine(int(4.2 * B), 330, 16000), "s" if j % 2 else "m")
        t = Tape(bpm, pos=int(1.7 * B))
        nb = int(4 * B / 64)
        kk = [raw(12), raw(31), raw(15), raw(28)]
        for b in range(nb):
            if b % 70 == 0:
                kk = [int(v) for v in rng.integers(0, 32768, 4)]
                if j < 3:
                    kk[1] = raw(int(rng.choice([0, 19, 31])))
            if j >= 3:                                              # an LFO on all four knobs (~1 Hz triangle)
                ph = (b * (j + 1) * 0.0015) % 1.0
                tri = 1 - abs(2 * ph - 1)
                kk = [int(v * 32767) for v in (tri, 0.0 if j == 3 else tri, 1 - tri, tri)]
            c.block(kk, t.words())
        cases[f"jump_{j}"] = c
        notes[f"jump_{j}"] = 330

    # ---- 9. pingpong's lines ----
    t = Tape(bpm)
    nb2 = int(2 * B / 64) * 64                                      # sections of whole blocks
    loud = sine(int(8 * B), 300, 20000)
    c = Case(np.concatenate([loud[:nb2], np.zeros(nb2, np.int16), loud[:nb2], np.zeros(nb2, np.int16)]), "m")
    pp_k = [20000, 30000, 16384, 32767]                             # pingpong: long echo, high feedback, full wet
    c.run_tape(pp_k, t, int(2 * B / 64), fx="p")                    # pingpong fills the mono line
    c.run_tape([raw(12), ALWAYS, raw(9), FLAT], t, int(2 * B / 64))    # repeat on silence: must be silent
    c.run_tape([raw(12), EVERY_BAR, raw(9), FLAT], t, int(2 * B / 64))  # repeat records loud input ...
    c.run_tape(pp_k, t, int(2 * B / 64), fx="p")                    # ... pingpong on silence: must be silent
    cases["handover"] = c
    c = Case(np.zeros(int(1.2 * B), np.int16), "s")
    c.run_tape([raw(12), ALWAYS, raw(9), raw(31)], Tape(bpm), int(1.1 * B / 64))
    cases["boot_garbage"] = c
    # mono pingpong and stereo repeat interleaved (synth FX + master FX in one callback): independent sets
    t1 = Tape(bpm)
    c = Case(sine(int(3 * B), 500, 12000), "s")
    for b in range(int(2.5 * B / 64)):
        w = t1.words()
        c.block(pp_k, w, fx="p", mode="m")
        c.block([raw(12), EVERY_BAR, raw(9), FLAT], w, fx="r", mode="s")
    cases["interleave"] = c

    # ---- 10. invalid words ----
    def bad_case(fn, blocks=int(2.2 * B / 64)):
        c = Case(code(blocks * 64), "s")
        t = Tape(bpm)
        for b in range(blocks):
            c.block(k_fill, fn(t.words(), b))
        return c
    cases["bad_tempo0"] = bad_case(lambda w, b: (0, w[1], w[2], w[3], w[4]))
    cases["bad_tempo_garbage"] = bad_case(lambda w, b: (spb_of(120) if b < 200 else [5, -7, 2 ** 31 - 1][b % 3],) + w[1:])
    cases["bad_transport"] = bad_case(lambda w, b: (w[0], 7 + b, w[2], w[3], w[4]))
    cases["bad_pos"] = bad_case(lambda w, b: (w[0], w[1], 0xFFFFFF00, w[3], w[4]))
    cases["bad_metro"] = bad_case(lambda w, b: (w[0], 0, w[2], -5, w[4]))
    cases["bad_metro_huge"] = bad_case(lambda w, b: (w[0], 0, w[2], 0x7FFFFFFF, w[4]))

    # ---- 11. DC ----
    c = Case(np.concatenate([sine(int(1.5 * B), 441), np.zeros(int(1.5 * B), np.int16)]), "m")
    c.run_tape([raw(12), EVERY_BAR, raw(21), raw(3)], Tape(bpm), int(2.9 * B / 64))      # +12: the HP
    cases["quiet_up"] = c
    c = Case(np.concatenate([sine(int(1.5 * B), 441) + 3000, np.zeros(int(1.5 * B), np.int16)]), "m")
    c.run_tape([raw(12), EVERY_BAR, raw(21), raw(31)], Tape(bpm), int(2.9 * B / 64))     # TAPE STOP on a DC input
    cases["quiet_stop"] = c
    for name, what in DC_CASES:
        p, kp = DC_KNOBS[name]
        x = sine(int(2.2 * B), 400).astype(np.int32) + 1500
        for sgn in "pn":
            c = Case(np.clip(x if sgn == "p" else -x, -32767, 32767).astype(np.int16), "m")
            t = Tape(bpm, rolling=False)
            c.run_tape([raw(p), OFF, raw(9), kp], t, int(0.25 * B / 64))
            c.run_tape([raw(p), ALWAYS, raw(9), kp], t, int(1.85 * B / 64))
            cases[name + "_" + sgn] = c

    # ---- 12. fold; stereo independence ----
    for bpm_f, p in ((60, 30), (40, 24), (40, 30), (41, 24)):
        BB = bar_of(bpm_f)
        cases[f"fold_{bpm_f}_{p}"] = stereo_case(code(int(2.3 * BB)), [raw(p), EVERY_BAR, raw(21), FLAT],
                                                 Tape(bpm_f), int(2.2 * BB / 64))
    lr = np.stack([sine(int(2.2 * B), 300), sine(int(2.2 * B), 700, 9000)], axis=1)
    c = Case(lr, "s")
    c.run_tape([raw(12), EVERY_BAR, raw(15), FLAT], Tape(bpm), int(2.1 * B / 64))
    cases["stereo_lr"] = c

    # ---- 13. cycles ----
    for name, k in (("idle", [raw(12), OFF, raw(9), FLAT]), ("default", [raw(12), raw(5), raw(9), FLAT]),
                    ("buzz_pitch", [raw(1), ALWAYS, raw(9), raw(28)]), ("vary", [raw(5), raw(31), raw(27), raw(3)]),
                    ("tapestop", [raw(12), ALWAYS, raw(21), raw(31)]), ("spin", [raw(1), ALWAYS, raw(21), raw(1)])):
        for mode in "ms":                               # 0.6 bars dry (not counted), then 2.5 bars of the setting
            c = Case(sine(int(3.2 * B), 441), mode)
            t = Tape(bpm, pos=int(2.3 * B))
            c.run_tape([raw(12), OFF, raw(9), FLAT], t, int(0.6 * B / 64), fx="R")
            c.run_tape(k, t, int(2.5 * B / 64))
            cases[f"cyc_{name}_{mode}"] = c

    print(f"{len(cases)} simulator runs ...")
    R = run(cases, keep=outdir)

    # ======== 1 ========
    worst, bad = 0.0, []
    for bpm in BPMS:
        bar = bar_of(bpm)
        for p, n, name in GRIDS:
            out, st, _ = R[f"grid_{bpm}_{n}"]
            src = decode(out)
            nf, L = fold_len(n, bpm)
            g = eff16(nf, 16)
            G = g / 16 * bar
            errs = []
            for line in range(1, int(len(src) / bar) + 1):
                t0 = line * bar - G
                if t0 < 64 or line * bar >= len(src):
                    continue
                exp = [t0 + j * L for j in range(1, int(round(G / L - 1e-6)) + (1 if G / L % 1 > 1e-6 else 0))]
                got = boundaries(src, int(t0) + 2, int(line * bar) - 33)
                if len(got) != len(exp):
                    bad.append(f"{bpm} {name} bar {line}: {len(got)} boundaries, {len(exp)} expected")
                    continue
                for e, x in zip(exp, got):
                    errs.append(x - math.ceil(e - 1e-9))
                    j0 = x + 33                                     # past the crossfade: the slice's start
                    if j0 < line * bar - 1:
                        rd = src[j0] - (j0 - x)
                        errs.append(rd - math.ceil(t0 - 1e-9))
                last = np.nonzero(out[int(t0):int(line * bar) + 5, 0] != code(len(src))[int(t0):int(line * bar) + 5, 0])[0]
                if len(last):
                    errs.append(int(t0) + last[-1] + 1 - math.ceil(line * bar - 1e-9))   # dry from the bar line
            if errs:
                worst = max(worst, max(abs(e) for e in errs))
            if any(abs(e) > 1 for e in errs):
                bad.append(f"{bpm} {name}: errors {sorted(set(errs))[:6]}")
    check(not bad and worst <= 1, f"grid timing: 11 grids x {len(BPMS)} tempos, every slice boundary, each repeat's read "
          f"start and the return to dry on the bar line within {worst:.0f} sample of the exact time"
          + ("".join("\n     " + b for b in bad[:8])))

    # ======== 2 ========
    def gate_ends(out, inp_code):
        """(start, end) of each stretch where the output is not the dry input"""
        d = np.nonzero(out[:, 0] != inp_code[:len(out), 0])[0]
        if not len(d):
            return []
        seg = np.split(d, np.nonzero(np.diff(d) > 2000)[0] + 1)
        return [(int(s[0]), int(s[-1]) + 1) for s in seg]
    out, st, _ = R["align_tape"]
    ends = [e for _, e in gate_ends(out, code(len(out)))]
    p0 = int(2.37 * B) + 123
    n1 = int(2.5 * B / 64) * 64
    G4 = B / 4
    exp1 = [math.ceil(b * B - p0) for b in range(3, 6) if b * B - G4 - p0 < n1]     # started before the locate
    p1 = int(0.31 * B)
    exp2 = [n1 + math.ceil(b * B - p1) for b in range(1, 4) if b * B - p1 < n1 and b * B - G4 > p1]
    check(ends == exp1 + exp2, f"bar alignment from tape position {p0} (2.37 bars) and after a locate back to {p1}: "
          f"gates end on the tape's bar lines at {ends} (expected {exp1 + exp2}; a gate running at the locate keeps "
          "its length)")
    out, st, _ = R["align_metro"]
    ends = [e for _, e in gate_ends(out, code(len(out)))]
    clicks = 7777 + np.arange(0, 20) * (spb_of(120) / 16384)        # the metronome's beats in samples
    on_click = [min(abs(e - c) for c in clicks) for e in ends]
    gaps = np.diff(ends)
    check(len(ends) >= 3 and max(on_click) <= 1 and all(abs(g - B) <= 1 for g in gaps) and set(st["lock"]) == {2},
          f"tape stopped: the bar clock follows the metronome (lock {sorted(set(st['lock']))}): gates end on clicks "
          f"(off by {max(on_click) if on_click else '-'} samples) one bar apart ({list(gaps)})")
    out, st, _ = R["stop_midgate"]
    segs = gate_ends(out, code(len(out)))
    G2 = 2 * B
    check(len(segs) >= 1 and abs((segs[0][1] - segs[0][0]) - (G2 - B / 16)) <= 1 and abs(segs[0][0] - B / 2 - B / 16) <= 1,
          f"the tape stops mid-gate: the gate keeps its length in samples ({segs[0][1] - segs[0][0] if segs else '-'}, "
          f"expected {G2 - B / 16:.0f} after its live slice)")

    out, st, _ = R["metro_held"]
    ends = [e for _, e in gate_ends(out, code(len(out)))]
    check(len(ends) >= 3 and all(abs(g - B) <= 1 for g in np.diff(ends)) and ends[0] <= B + 1,
          f"tape stopped, TE's metronome held (tape-follow): our count runs free at the tempo and fills fire every bar "
          f"({ends})")
    out, st, _ = R["metro_held_after_tape"]
    ends = [e for _, e in gate_ends(out, code(len(out)))]
    check(ends == [math.ceil(2 * B), math.ceil(4 * B)], f"EVERY 2 BARS, the tape stopped at 1.5 bars (metronome held): "
          f"the bar count carries on from the tape's, fills end on bar lines 2 and 4 ({ends})")
    for n in (2, 4):
        out, st, _ = R[f"phase_every{n}"]
        t0 = notes[f"phase_every{n}"]
        ends = [e for _, e in gate_ends(out, code(len(out)))]
        rel = [round((e - t0) / B, 3) for e in ends if e > t0]
        check(rel[:2] == [n, 2 * n], f"EVERY {n} BARS, play from tape position 0 after a stop: the fills end on the bar "
              f"lines after bars {n} and {2 * n} (the last beat of bar {n}, {2 * n}: {rel[:3]} bars after play)")
    out, st, _ = R["phase_offset"]
    ends = [round(e / B + 2.5, 3) for _, e in gate_ends(out, code(len(out)))]
    check(ends[:2] == [4.0, 8.0], f"EVERY 4 BARS, play from tape position 2.5 bars: the fills end on tape bar lines 4 and "
          f"8, relative to tape bar 1, not to where play started ({ends[:2]})")

    # ======== 3 ========
    def fire_bars(st, nblocks_half):
        f = np.diff(st["fires"], prepend=0)
        idx = np.nonzero(f > 0)[0]
        return [(int(i >= nblocks_half), int(st["bar"][i])) for i in idx]
    half = int(16 * B / 64)
    fa = fire_bars(R["seed_a"][1], half)
    a1 = sorted(b for p, b in fa if p == 0)
    a2 = sorted(b for p, b in fa if p == 1)
    fb_ = sorted(b for p, b in fire_bars(R["seed_b"][1], half) if p == 0)
    fm_ = sorted(b for p, b in fire_bars(R["seed_m"][1], half) if p == 0)
    check(a1 == a2 and 3 <= len(a1) <= 13, f"CHANCE 50 %: the same bars fire on a second pass from tape position 0 "
          f"({a1} / {a2})")
    check(a1 != fb_ and a1 != fm_, f"another sound slot ({fb_}) and the master ({fm_}) draw other fills")

    # ======== 4 ========
    def gate_lines(name, bar):
        out, st, _ = R[name]
        f = np.diff(st["fires"], prepend=0)
        idx = np.nonzero(f > 0)[0]
        lines = [int(round((i * 64 + 64) / bar + (4 / 16))) for i in idx]
        return lines, st
    for name in ("off", "every8", "every4", "every2", "every1"):
        lines, st = gate_lines(f"f_{name}", FB)
        n = {"off": 0, "every8": 8, "every4": 4, "every2": 2, "every1": 1}[name]
        if not n:
            out = R[f"f_{name}"][0]
            check(not lines and np.array_equal(out, code(len(out)).astype(np.int64)),
                  "FORTUNE OFF: never fires, the output is the input, bit exact")
        else:
            check(lines and all(L % n == 0 for L in lines) and len(lines) == 40 // n - (1 if 40 % n == 0 else 0)
                  + (1 if 40 % n == 0 else 0) - (0 if lines[-1] <= 40 else 1),
                  f"EVERY {n}: gates end on bars {lines[:6]} .. (every {n}th bar line, {len(lines)} in 40 bars)")
    for name, pexp in (("chance10", .1), ("chance30", .3), ("chance90", .9), ("vary1", .5), ("vary2", .67),
                       ("vary3", .83), ("vary4", 1.0)):
        out, st, _ = R[f"f_{name}"]
        won, lost = int(st["fires"][-1]), int(st["lost"][-1])
        p = 4 * won / max(1, won + lost)                            # GATE 4/16: a roll per beat at a quarter
        sd = 4 * math.sqrt(pexp / 4 * (1 - pexp / 4) / 800) + 1e-3
        check(abs(p - pexp) <= 3.5 * sd and won + lost >= 790, f"{name.upper()} (GATE 4/16: rolled per beat): {won} of "
              f"{won + lost} beats fired, {100 * p:.1f} % per bar (expected {100 * pexp:.0f} %)")
    for name, fam in (("vary_picks", [64, 32, 16, 8, 4, 2]), ("vary_picks_t", [48, 24, 12, 6, 3])):
        st = R[name][1]
        grids = st["grid"][st["active"] == 1]
        ns = sorted(set(((3 if g & 0x80 else 1) << (g & 7)) for g in grids))
        blue = 16 if fam[0] == 64 else 12
        i0 = fam.index(blue)
        ok = all(n in fam and abs(fam.index(n) - i0) <= 4 and 16 / n <= 8 for n in ns) and len(ns) >= 3
        check(ok, f"VARY 4 on blue {'1/16' if blue == 16 else '1/8T'}: the repeats drew slices of {ns} slices per bar "
              "(blue's family, within 4 places, at most half the 1-bar gate)")
    lag_ok, rel_ok, desc = [], [], []
    for j in range(5):
        out, st, _ = R[f"always_{j}"]
        lead = notes[f"always_{j}"]
        t_on = lead * 64
        src = decode(out)
        d = np.nonzero(out[t_on:, 0] != code(len(out))[t_on:, 0])[0]
        start = t_on + int(d[0]) if len(d) else -1
        six = B / 16
        ph = (t_on % six)
        # within 30 ms after a line: at once (the next sample block), else on the next line
        expect_now = ph <= 1323
        line_next = (math.floor(t_on / six) + 1) * six
        ok = start == t_on if expect_now else abs(start - math.ceil(line_next)) <= 1
        # the repeat reads the 1/16 that ended on the line before its start, phase-aligned
        j0 = start + 40
        line0 = math.floor(start / six) * six if not expect_now else math.floor(t_on / six) * six
        rd_ok = abs((src[j0] - (line0 - six)) - (j0 - line0) % six) <= 1
        lag_ok.append(ok and rd_ok)
        desc.append(f"{ph / 44.1:.1f} ms after a line -> starts {start - t_on} samples later")
        t_off = t_on + int(1.0 * B / 64) * 64
        d2 = np.nonzero(out[t_off:, 0] != code(len(out))[t_off:, 0])[0]
        rel_ok.append(len(d2) == 0 or d2[-1] < 33)
    check(all(lag_ok), "ALWAYS engages on the 1/16 grid with the 1/16 that ended there: " + "; ".join(desc))
    check(all(rel_ok), "ALWAYS released: dry again within one 32-sample crossfade")

    # ======== 5 ========
    bad = []
    for gp, g in GATES:
        for p, n, name in ((1, 64, "1/64"), (12, 16, "1/16"), (24, 4, "1/4"), (30, 2, "1/2"), (27, 3, "1/2T")):
            out, st, _ = R[f"gate_{g}_{n}"]
            bar = bar_of(220)
            e = eff16(n, g)
            src = decode(out)
            d = np.nonzero(out[:, 0] != code(len(out))[:len(out), 0])[0]
            first_fire = np.nonzero(np.diff(st["fires"], prepend=0) > 0)[0]
            if not len(first_fire) or not len(d):
                bad.append(f"{g}/16 {name}: no gate")
                continue
            end = int(d[-1]) + 1
            start_exp = 8 * bar - e / 16 * bar
            if abs(end - math.ceil(8 * bar)) > 1 or abs(first_fire[0] * 64 - start_exp) > 64:
                bad.append(f"{g}/16 {name}: end {end} (bar line {8 * bar:.0f}), start block {first_fire[0]}")
            live = bar / n
            if abs(int(d[0]) - (start_exp + live)) > 1 and e > 0:
                bad.append(f"{g}/16 {name}: repeats from {int(d[0])}, expected {start_exp + live:.0f}")
    check(not bad, "GATE 1/16 .. 4 BARS x 5 grids: each gate starts eff / 16 bars before the bar line it ends on "
          "(the grid wins: at least two slices) and is dry from that line" + "".join("\n     " + b for b in bad[:8]))

    # ======== 6 ========
    rows = []
    PITCH = {3: 12, 5: 7, 7: 5, 9: 3, 11: 2, 13: 1, 18: -1, 20: -2, 22: -3, 24: -5, 26: -7, 28: -12}
    worst = 0
    for pp, S in PITCH.items():
        out, st, _ = R[f"pitch_{pp}"]
        y = out[:, 0].astype(float)
        bar = B
        t0 = 8 * bar - bar                                          # 1-bar gate ending on bar 8 (pos 6 bars in)
        t0 -= 6 * bar
        L = bar / 8
        meas = []
        for k in range(1, 8):
            a, b = int(t0 + k * L + 400), int(t0 + k * L + L * 0.5)
            if S > 0:
                b = int(t0 + k * L + L * 0.45)
            seg = y[a:b]
            zc = np.nonzero((seg[:-1] < 0) & (seg[1:] >= 0))[0]
            if len(zc) < 3:
                meas.append(0)
                continue
            fr = (len(zc) - 1) / ((zc[-1] - zc[0]) / FS)
            meas.append(fr)
        exp = [441 * 2 ** (round(S * k / 7) / 12) for k in range(1, 8)]     # whole semitones
        err = max(abs(1200 * math.log2(m / e)) if m else 999 for m, e in zip(meas, exp))
        worst = max(worst, err)
        rows.append(f"{S:+d}: {round(meas[-1])} Hz (k=7)")
    check(worst < 8, f"the pitch staircase: every repeat k of 7 at round(S x k / 7) semitones within {worst:.1f} cents ("
          + ", ".join(rows) + ")")
    out, st, _ = R["pitch_28"]
    seg_last = out[int(B + 7 * B / 8) + 400:int(2 * B) - 400, 0].astype(float)
    seg_first = out[int(B + B / 8) + 400:int(B + B / 4) - 400, 0].astype(float)
    rl, rf = np.sqrt((seg_last ** 2).mean()), np.sqrt((seg_first ** 2).mean())
    check(-9 < 20 * math.log10(rl / rf) < -4.5, f"PITCH -12: the last repeat is quieter by "
          f"{20 * math.log10(rl / rf):.1f} dB (the -6 dB fade and the closing low-pass on 441 -> 220 Hz)")

    # ======== 7 ========
    out, st, _ = R["tapestop"]
    src = decode(out).astype(float)
    t0 = int(math.ceil(7 * B - 6 * B))                              # 1-bar gate ending on bar 8: starts at 1 bar
    G = B
    tt = np.arange(t0 + 10, int(t0 + G * 0.84))
    exp = t0 + (tt - t0) - (tt - t0) ** 2 / (2 * G)
    # the DSP's ramp: the rate falls by floor(2^32 / (span8 >> 4)) / 2^28 per sample (0.03 % off 1 / G: 0.5 cent)
    dr = (2 ** 32 - 1) // ((int(round(G * 256))) >> 4)
    inc = ((2 ** 28 - dr * np.arange(1, int(G) + 2)) >> 12) / 65536
    pos = t0 + np.concatenate([[0], np.cumsum(inc)])
    exp = pos[tt - t0]
    seam = np.abs(np.diff(src[tt])) >= 2                            # the time code's 2^15 seams (the closing low-pass
    okm = ~np.convolve(seam, np.ones(49), "same").astype(bool)      # smears each over a few samples): skipped
    r_ = 1 - (tt - t0) / G                                         # the read rate, and the closing low-pass's lag
    b_ = 2 ** (-2.4375 * (tt - t0) / G)
    exp = exp - r_ * (1 - b_) / b_
    dev = np.abs(src[tt] - exp)[1:][okm]
    ahead = (src[tt] > tt)[1:][okm]
    check(dev.max() < 3 and not ahead.any() and set(st["kind"][st["active"] == 1]) == {3},
          f"TAPE STOP (1 bar at 120 BPM: the real one): the read decelerates 1 -> 0 behind the write, within "
          f"{dev.max():.2f} samples of the ramp's curve (t - t^2 / 2G with its 0.03 % step), never ahead")
    out, st, _ = R["tapestop_sine"]
    y = out[:, 0].astype(float)
    env = [np.abs(y[int(t0 + G * f):int(t0 + G * f) + 2000]).max() for f in (0.5, 0.80, 0.9, 0.97)]
    check(env[0] > 8000 and env[1] > 5000 and env[2] < env[1] and env[3] < 0.3 * env[1],
          f"TAPE STOP fades over the gate's last 15 %: peaks {[int(e) for e in env]} at 50 / 80 / 90 / 97 %")
    out, st, _ = R["tapestop_loop"]
    kinds = set(st["kind"][st["active"] == 1])
    check(2 in kinds and 3 not in kinds, f"TAPE STOP at 60 BPM (half the gate is longer than the line): the looped "
          f"version (kinds {sorted(kinds)}: 2 = ramp)")
    out, st, _ = R["spinup"]
    y = out[:, 0].astype(float)
    L = B / 8
    fr = []
    for k in (1, 4, 7):
        seg = y[int(B + k * L + 300):int(B + k * L + L * 0.4)]
        zc = np.nonzero((seg[:-1] < 0) & (seg[1:] >= 0))[0]
        fr.append((len(zc) - 1) / ((zc[-1] - zc[0]) / FS) if len(zc) > 2 else 0)
    check(fr[0] < fr[1] < fr[2] and 441 * 1.05 < fr[0] and fr[2] < 441 * 2.01,
          f"SPIN UP: the repeats rise ({', '.join(f'{f:.0f}' for f in fr)} Hz at repeats 1, 4, 7 of a 441 Hz slice)")
    out, st, _ = R["tapestop_hold"]
    tail = out[int(1.6 * B):, 0]
    check(np.abs(tail).max() == 0 and st["active"][-1] == 1, "TAPE STOP latched (ALWAYS): stops within a bar, then "
          f"silence (|max| {np.abs(tail).max()} after 1.6 bars), not a held sample")

    # ======== 8 ========
    worst, bad = 0.0, []

    def click_ratio(out, inp, f, rmax):
        """largest output step over what the read rate and a 32-sample crossfade allow"""
        a = 16000
        allow = rmax * 2 * math.pi * f / FS * a + 2 * a / 32 + 8
        dy = np.abs(np.diff(out, axis=0)).max()
        return dy / allow
    for name in [n for n in R if n.startswith("click_")]:
        out, st, _ = R[name]
        f, kp = notes[name]
        rmax = 2.05 if kp in (raw(1), raw(3)) else 1.0
        r = click_ratio(out, None, f, rmax)
        worst = max(worst, r)
        if r > 1.1:
            bad.append(f"{name} ({f} Hz): {r:.2f}")
    for name in [n for n in R if n.startswith("jump_")]:
        out, st, _ = R[name]
        r = click_ratio(out, None, notes[name], 2.05)
        worst = max(worst, r)
        if r > 1.1:
            bad.append(f"{name}: {r:.2f}")
    check(not bad, f"no clicks (within 10 %): {len([n for n in R if n.startswith(('click_', 'jump_'))])} runs (6 grids x 3 FORTUNE "
          f"x 6 PITCH, knob jumps, an LFO on all knobs): the largest step is {worst:.2f} x what the read rate and a "
          "32-sample crossfade allow" + "".join("\n     " + b for b in bad[:8]))

    # ======== 9 ========
    out, st, _ = R["handover"]
    nb = int(2 * B / 64) * 64
    seg1 = out[nb:2 * nb, 0]
    seg3 = out[3 * nb:4 * nb, 0]
    check(np.abs(seg1).max() == 0 and np.abs(seg3).max() == 0 and st["owner0"][nb // 64 - 1] == 1
          and st["owner0"][nb // 64 + 1] == 2 and st["owner0"][-1] == 1,
          f"pingpong -> repeat -> pingpong on the mono line: repeat plays nothing pingpong left (max |{np.abs(seg1).max()}|"
          f" on silence after its echoes), pingpong nothing repeat left (|{np.abs(seg3).max()}|); the owner word follows")
    seg2 = out[2 * nb + 64 * 40:3 * nb, 0]
    check(np.abs(seg2).max() > 10000, f"... and repeat takes the lines over at once (its own repeats peak "
          f"{np.abs(seg2).max()})")
    out, st, _ = R["boot_garbage"]
    check(np.abs(out).max() == 0, "the lines' garbage at boot (pingpong_ram.c) is never heard: ALWAYS on silence from "
          f"the first block, TAPE STOP: output max |{np.abs(out).max()}|")
    out, st, _ = R["interleave"]
    check(st["owner0"][-1] == 1 and st["owner1"][-1] == 2 and int(st["fires"][-1]) >= 2,
          "mono pingpong and stereo repeat in one callback: each keeps its own line set (owners 1 / 2)")

    # ======== 10 ========
    def ends_of(name):
        out = R[name][0]
        return [e for _, e in gate_ends(out, code(len(out)))]
    good = [math.ceil(b * B) for b in (1, 2)]
    e0 = ends_of("bad_tempo0")
    check(e0 == good, f"tempo word 0: 120 BPM (gates end at {e0})")
    eg = ends_of("bad_tempo_garbage")
    check(eg == good, f"tempo word garbage after a valid one: the last valid tempo (gates end at {eg})")
    for name, what in (("bad_transport", "transport garbage"), ("bad_pos", "playhead garbage while playing"),
                       ("bad_metro", "a dead metronome (stopped, countdown -5)"),
                       ("bad_metro_huge", "a countdown out of range")):
        out, st, _ = R[name]
        ends = [e for e in ends_of(name) if e < len(out)]          # (a gate cut by the run's end left out)
        gaps = np.diff(ends)
        check(len(ends) >= 1 and all(abs(g - B) <= 1 for g in gaps) and np.abs(out).max() <= 32767 and
              set(st["lock"]) == {2}, f"{what}: our own clock (lock {sorted(set(st['lock']))}), gates one bar apart "
              f"({ends})")

    # ======== 11 ========
    for name, what in (("quiet_up", "+12 (the high-pass)"), ("quiet_stop", "TAPE STOP on an input with DC")):
        out, st, _ = R[name]
        n_s = int(1.5 * B)
        after = out[n_s + int(B):, 0]
        inp = R[name][0]
        check(np.abs(after).max() == 0, f"{what}: exactly silent after the gates once the input is (|max| "
              f"{np.abs(after).max()}), dry outside them")
    worst, rows = 0.0, []
    for name, what in DC_CASES:
        a, b_ = R[name + "_p"][0][:, 0].astype(float), R[name + "_n"][0][:, 0].astype(float)
        sym = a + b_                                                # y(x) + y(-x): what the FX adds of its own
        worst = max(worst, abs(sym.mean()) / 1.5, np.abs(sym).max() / 10)
        rows.append(f"{what} {sym.mean():+.2f} (max {np.abs(sym).max():.0f})")
    check(worst < 1, "no DC of the FX's own (rounding only, |mean| < 1.5 LSB = -87 dBFS): y(x) + y(-x) over the stutter, "
          "mean (max |.|): " + ", ".join(rows))

    # ======== 12 ========
    for bpm_f, p, nexp in ((60, 30, 4), (40, 24, 8), (40, 30, 8), (41, 24, 4)):
        out, st, _ = R[f"fold_{bpm_f}_{p}"]
        n0 = 2 if p == 30 else 4
        folds = int(st["fold"][-1])
        played = (1 << (int(st["blue"][-1]) & 7))
        src = decode(out)
        BB = bar_of(bpm_f)
        bnd = boundaries(src, 100, int(BB) - 40)
        L = (bnd[1] - bnd[0]) if len(bnd) > 1 else 0
        check(played == nexp and abs(L - BB / nexp) <= 1 if len(bnd) > 1 else played == nexp,
              f"{'1/2' if p == 30 else '1/4'} at {bpm_f} BPM: {folds} fold(s), plays 1/{nexp} "
              f"(slices {L} samples)")
    out, st, _ = R["stereo_lr"]
    inp = R["stereo_lr"][0]
    lr = np.stack([sine(len(out), 300), sine(len(out), 700, 9000)], axis=1).astype(np.int64)
    dl = np.nonzero(out[:, 0] != lr[:, 0])[0]
    dr = np.nonzero(out[:, 1] != lr[:, 1])[0]
    pk_r = np.abs(out[int(B * 0.9):int(B), 1]).max()
    check(len(dl) and len(dr) and dl[0] == dr[0] and dl[-1] == dr[-1] and 8000 < pk_r < 9100,
          f"stereo: L and R repeat their own input on the same timing (R stays the 700 Hz -11 dB channel, peak {pk_r})")

    # ======== 13 ========
    rows = []
    worst_s = 0
    for name in [n for n in R if n.startswith("cyc_")]:
        mx, mean = R[name][2]
        rows.append(f"{name[4:]} {mean}/{mx}")
        if name.endswith("_s"):
            worst_s = max(worst_s, mx)
    check(worst_s < 25000, "CYCLES per call over 2.5 bars, mean / max (mono _m, stereo _s; pingpong: 16187 / 16901 "
          "stereo): " + ", ".join(rows))
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
