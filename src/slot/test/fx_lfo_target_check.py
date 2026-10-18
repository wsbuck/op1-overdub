#!/usr/bin/env python3
"""TE's LFO targets the knob its screen shows, on every FX (notes/fx-slot.md "LFO target flag").

  python3 src/slot/test/fx_lfo_target_check.py BUILT.OP1|OP1_vdk.ldr [--stock op1_246.op1] [--fx FX.json]

The LFO's target selector (a continuous LFO param, 0..R with R ~ 0x3FFF) has 8 detents d = round(7 * p). Per FX id,
a byte flag "8 targets" decides the knob, read as B[table + FX id] in two places that must agree:
  - UI, the five LFO screens (FUN_01968c10 and four like it): knob colour = d if flag else d >> 1
    (`CC = R0 == 0` at 0x01968CFC; `R0 = (R0 + (R0 >> 31)) >>> 1` at 0x01968F0C, 0x0193E350, ...);
    the flag comes from FUN_01a27e98 (synth FX, id [0xFF801734 + slot*4]) or FUN_01a27e70 (drum FX,
    [0xFF801754 + dslot*4]), both B[0x018F5EDC + id];
  - DSP, the five LFO modules: knob = v >>> 11 if flag else v >>> 12 (`CC = Rn == 0; IF CC Rk = R(v >>> 12)`), then
    FUN_01a1de68 on 0xFF801724 + 2 * knob; tables 0x018E143C, 0x018E152C, 0x018E1720, 0x018E183C, 0x018E1A0C.
Checks:
  1. TE's code: the stock bytes of every decode window are as read (so the arithmetic below is TE's);
     the built image keeps them, apart from the repointed table addresses.
  2. The built image's six loads (five DSP + the UI getters, synth and drum) point at tables holding, for every FX id
     (TE's 0..7 and ours), one flag value, and ours equal TE's (delay's).
  3. For every FX, module, synth / drum, R in 14336..16383 and detent: the DSP's knob = the screen's knob, and the
     four colours blue / green / white / orange (d = 2k, 2k + 1) write 0xFF801724 + 2k, k = 0..3.
  4. The model reproduces the device report on LFOFX1 (2026-10-18: UI flag 5, DSP 0 for distortion: blue, green,
     white, orange moved blue, blue, green, green), which fixes R in 14336..16383.
Pure Python on the images (no simulator): the decode is a shift and a select, re-implemented here and pinned to
TE's bytes by check 1.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ldr  # noqa: E402
import op1pack  # noqa: E402
from build_slot import FX_FLAG_TABLES, FX_ID, FX_LFO_REF  # noqa: E402

UI_TABLE = 0x018F5EDC
STOCK_WINDOWS = [   # TE's decode, firmware 246 (addresses in the module docstring)
    (0x019CA456, "82cea90193a80000"),                      # M1: R0 = R1 >>> 11
    (0x019CA604, "82cea1054a91000001a080c6414209e13c1449e18e01515a00cacb2009990000010c0207"),   # >>> 12, flag, select
    (0x019CB3FA, "82cead0f69a80000"),                      # M2: R7 = R5 >>> 11
    (0x019CB5A0, "82cea50548910000006680c6484009e12c1549e18e01415a00ca412008990000000c3a07"),
    (0x019CD26E, "82cea8054a91000009e1201749e18e0182cea0037bb500004730515a0b99030c1107"),          # M3
    (0x019CDF92, "09e13c1849e18e01d0a8695a82cea00709990000010c82cea80f509500003b07"),              # M4
    (0x019D0112, "08e10c1a48e18e01daa882cea20f59950000085a82ceaa0d03990000030c20e100203707"),      # M5
    (0x01A27E70, "083208e1541748e180ff0ae1dc5e4ae18f01485e49914a5a0899100009e1d45e49e18f0108991000"
                 "083208e1341748e180ff0ae1dc5e4ae18f01485e49914a5a08991000"),                     # UI getters
    (0x01968CFA, "6843000c98a059a004cc014059ac0000104df318180e"),          # screen: flag 0 -> halve
    (0x01968F0C, "82c608830850084d"),                                      # the halving, five screens
    (0x0193E350, "82ce0883b9ac00000850084d"),
    (0x019E518C, "82ce0883f5b000000850084d"),
    (0x01A2DB5A, "82c6088704cc030039a10000084d"),
    (0x01A4979E, "82c6088704cc0300a1b90000084d"),
]
DSP_SITES = {t: s[0] for t, s in FX_FLAG_TABLES.items() if t != UI_TABLE}
UI_SITES = FX_FLAG_TABLES[UI_TABLE]          # FUN_01a27e70 (drum), FUN_01a27e98 (synth)
COLOURS = ["blue", "green", "white", "orange"]
REPORT_LFOFX1 = [0, 0, 1, 1]                 # device, distortion, value LFO: targets blue..orange -> knob moved


def load_ldr(path):
    data = open(path, "rb").read()
    if path.lower().endswith(".op1"):
        data = {n: c for n, _, c in op1pack.members(op1pack.read_image(path))[0]}["OP1_vdk.ldr"]
    blocks = ldr.parse(data)

    def rd(addr, size):                                 # the last block loaded there wins
        for b in reversed(blocks):
            if b.loads_memory and not b.is_fill and not b.is_encrypted and b.target <= addr < b.target + b.count:
                return b.payload[addr - b.target:addr - b.target + size]
        sys.exit(f"0x{addr:08x} not in {path}'s plaintext")
    return rd


def pair(rd, insn):
    """The .L/.H immediate pair starting at insn (as build_slot.repoint): (address loaded, [imm offsets])."""
    half = lambda a: struct.unpack("<H", rd(a, 2))[0]  # noqa: E731
    first = half(insn)
    assert first & 0xFFA0 == 0xE100, f"0x{insn:08x}: not a .L/.H load"
    for p in range(insn + 4, insn + 20, 2):
        hw = half(p)
        if hw & 0xFFA0 == 0xE100 and hw & 0x3F == first & 0x3F and (hw ^ first) & 0x40:
            lo, hi = (insn, p) if not first & 0x40 else (p, insn)
            return half(lo + 2) | half(hi + 2) << 16, [lo + 2, hi + 2]
    sys.exit(f"0x{insn:08x}: no .L/.H pair")


def ui_knob(d, flag):
    return d if flag else d >> 1                       # 0x01968CFA / 0x01968F0C


def dsp_knob(v, flag):
    return v >> (11 if flag else 12)                   # IF CC Rk = R(v >>> 12), else v >>> 11


def detent_value(d, r):
    return (d * r + 3) // 7                            # round(d / 7 * R): the selector's value at detent d


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("built")
    ap.add_argument("--stock", default=os.path.join(ROOT, "firmware", "stock", "op1_246.op1"))
    ap.add_argument("--fx", default=os.path.join(ROOT, "src", "slot", "fx.json"), help="the FX built in, in id order")
    a = ap.parse_args()
    ours = [f["name"] for f in json.load(open(a.fx))]
    n_fx = len(ours)
    names = ["delay", "grid", "punch", "phone", "spring", "filter", "nitro", "cwo"] + ours
    stock, built = load_ldr(a.stock), load_ldr(a.built)
    fails = []

    def check(ok, msg):
        if not ok:
            fails.append(msg)
            print("FAIL " + msg)
        return ok

    # 1. TE's decode bytes; the built image changes only the repointed immediates
    masked = set()
    for t, sites in FX_FLAG_TABLES.items():
        for s in sites:
            addr, imms = pair(stock, s)
            check(addr == t, f"stock 0x{s:08x} loads 0x{addr:08x}, expected 0x{t:08x}")
            masked |= {i for im in imms for i in (im, im + 1)}
    for at, hx in STOCK_WINDOWS:
        want = bytes.fromhex(hx)
        check(stock(at, len(want)) == want, f"stock decode window 0x{at:08x} differs from TE 246 as read")
        got = built(at, len(want))
        check(all(got[i] == want[i] for i in range(len(want)) if at + i not in masked),
              f"built image changed TE's decode at 0x{at:08x}")
    if not fails:
        print(f"PASS TE's decode windows ({len(STOCK_WINDOWS)}) as read; built image differs only in table addresses")

    # 2. the flag each place reads, per FX id
    n = FX_ID + n_fx
    flags = {}                                          # place -> [flag per id]
    for t, s in DSP_SITES.items():
        addr, _ = pair(built, s)
        check(addr != t, f"DSP 0x{s:08x} still loads TE's 8-byte table 0x{t:08x} (ids >= 8 overrun)")
        flags[f"dsp 0x{t:08x}"] = list(built(addr, n))
    ui = [pair(built, s)[0] for s in UI_SITES]
    check(len(set(ui)) == 1, f"UI getters load different tables {[hex(u) for u in ui]}")
    check(ui[0] != UI_TABLE, f"UI getters still load TE's 8-byte table 0x{UI_TABLE:08x} (ids >= 8 overrun)")
    flags["ui (synth + drum)"] = list(built(ui[0], n))
    for place, f in flags.items():
        te = list(stock(int(place.split()[1], 16) if place.startswith("dsp") else UI_TABLE, FX_ID))
        check(f[:FX_ID] == te, f"{place}: TE's FX flags changed {f[:FX_ID]} != {te}")
        check(f[FX_ID:] == [te[FX_LFO_REF]] * n_fx, f"{place}: our FX flags {f[FX_ID:]} != TE's {te[FX_LFO_REF]}")
    print("     flags per id " + " ".join(f"{i}:{names[i]}" for i in range(n)))
    for place, f in flags.items():
        print(f"     {place:22} {f}")

    # 3. the screen's knob = the DSP's knob, every FX / module / R / detent; colours k -> 0xFF801724 + 2k
    before = len(fails)
    rs = list(range(14336, 16384, 61)) + [16383]
    for fid in range(n):
        uf = flags["ui (synth + drum)"][fid]
        for place, f in flags.items():
            if not place.startswith("dsp"):
                continue
            bad = [(r, d) for r in rs for d in range(8) if ui_knob(d, uf) != dsp_knob(detent_value(d, r), f[fid])]
            if bad:
                r, d = bad[0]
                check(False, f"{names[fid]} ({fid}) {place}: {len(bad)} (R, detent) cases, e.g. R={r} detent {d}: "
                             f"screen knob {ui_knob(d, uf)}, LFO writes 0xFF801724 + 2*"
                             f"{dsp_knob(detent_value(d, r), f[fid])}")
        if fid >= FX_ID:
            for k, c in enumerate(COLOURS):
                ds = [d for d in range(8) if ui_knob(d, uf) == k]
                writes = {0xFF801724 + 2 * dsp_knob(detent_value(d, r), f[fid])
                          for d in ds for r in rs for f in [flags[p] for p in flags if p.startswith("dsp")]}
                check(ds == [2 * k, 2 * k + 1] and writes == {0xFF801724 + 2 * k},
                      f"{names[fid]} target {c}: detents {ds}, writes {sorted(hex(w) for w in writes)}")
    if len(fails) == before:
        print(f"PASS screen knob = LFO knob for {n} FX ids x 5 modules x synth/drum x {len(rs)} R x 8 detents; "
          "our FX: blue/green/white/orange -> 0xFF801724 + 0/2/4/6")

    # 4. the model against the device: LFOFX1 had UI flag 5 (TE's overrun) and DSP 0 for distortion
    before = len(fails)
    for r in rs:
        got = [dsp_knob(detent_value(k, r), 0) for k in range(4)]   # 8-target screen: colour k at detent k
        check(got == REPORT_LFOFX1, f"model R={r}: LFOFX1 distortion moves {got}, device {REPORT_LFOFX1}")
    lo = [r for r in range(1, 32768) if [dsp_knob(detent_value(k, r), 0) for k in range(4)] == REPORT_LFOFX1
          and all(ui_knob(d, 0) == dsp_knob(detent_value(d, r), 0) for d in range(8))]
    check(lo and lo[0] == 14336 and lo[-1] == 16383, f"R consistent with device and TE's FX: {lo[:1]}..{lo[-1:]}")
    if len(fails) == before:
        print("PASS model reproduces the LFOFX1 device report (blue, blue, green, green) for R 14336..16383")

    print("FAIL" if fails else "ALL PASS", f"({len(fails)} failures)" if fails else "")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
