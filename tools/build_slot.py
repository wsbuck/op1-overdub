#!/usr/bin/env python3
"""Build an OP1_vdk.ldr (and optionally a factory DB) with extra synth engines, ids 15, 16, ...

  build_slot.py STOCK_OP1_vdk.ldr ENGINES.json OUT.ldr [--db-in IN.db --db-out OUT.db]
                [--slot-addr 0x01E20000] [--build DIR]

ENGINES.json (e.g. src/slot/engines.json) lists the engines in id order:
  [{"name": "va", "source": "va", "addr": "0x01E10000", "max": "0x10000",
    "default": [8 knobs], "presets": {"saw lead": [k0, k1, k2, k3], ...},
    "aliases": ["old name", ...]}, ...]
Aliases are extra name-table entries for the same id, so patches saved under an old name still load (TE asserts
on an unknown engine name, instr_types.cpp:49); only "name" gets a DB row and presets.
Each engine is src/engine/<source>.c built by src/engine/build.sh at its addr (a FREE region). Optional "ram",
"ram_size": a FREE SDRAM buffer _<source>_ram shared by the engine and the screen (ui.c), reserved like an FX's; its
first 256 bytes are zero-filled by the image (a header that must not hold stale values at boot).
src/slot/slot.S is assembled at --slot-addr with tables generated into DIR/slot_gen.inc from the stock
image (TE data, never committed). TE's name lookup, producer factory, ScreenId, knob-range and
per-engine flag-table code is repointed at the grown tables by checked halfword patches
(tools/ldr_patch.py --half). iter (id 14) stays stock. See notes/engine-slot.md.
With --db-in/--db-out, adds each engine's synth_types row and presets (tools/add_engine_db.py), optionally
enables TE's hidden filter FX (--db-enable-filter), keeps
only the first N presets of folders given with --db-trim FOLDER=N, and refuses a DB with more than
MAX_SYNTH_PRESETS non-sampler presets: patch.cpp:616 asserts on that at boot (SLOT3.OP1).
With --fx FX.json (e.g. src/slot/fx.json), adds our FX as FX ids 8, 9, ... and their fx_types rows:
  [{"name": "ensemble", "source": "ensemble", "default": [8 knobs], "aliases": ["ens"]}, ...]
  optional "ram": "0x01E80000", "ram_size": "0x60000" defines _<source>_ram there (a FREE SDRAM buffer: undefined
  contents at boot); the range is reserved, so no code block may overlap it.
Each is src/slot/<source>.c exporting <source>_mono and <source>_stereo (notes/fx-slot.md).
"""
import argparse
import json
import os
import re
import struct
import sqlite3
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRST_ID = 15
UI_KINDS = {"va": 1, "wavetable": 2, "modal": 3, "grain": 4}   # must match enum ui_kind in src/slot/ui.c
# patch.cpp:616 `synthSynthes_preset_count <= kMaxSynthSynthes_presets` (0xb0 at 0x01a0582a): fixed array
# of 0x38-byte entries at 0x01DDDB68. Counted without sampler presets (stock 160 + 11 iter + 4 va = 175
# booted; 183 asserted).
MAX_SYNTH_PRESETS = 176

# fw 246 addresses (notes/engine-slot.md)
NAME_BLOB = 0x018DF7BC                     # engine name strings, built into 0x01DD4D98 by FUN_019b9890
NAME_OFFS = [0, 4, 0xC, 0x14, 0x1C, 0x24, 0x2C, 0x34, 0x3C, 0x44, 0x4C, 0x50, 0x54, 0x5C, 0x64]
NAME_TABLE, FACTORY_JT, SCREEN_JT, RANGE_TABLE = 0x01DD4D98, 0x018F5A04, 0x018F803C, 0x01DE5890
ITER_VTABLE, ITER_RENDER, ITER_GET, ITER_INIT = 0x019103CC, 0x019B99D4, 0x019B9BF0, 0x019B9C64
# HOLD (notes/hold.md): keyboard key-up CALL and the synth controller's shift-key vtable slot
HOLD_KEYUP_CALL, TE_NOTE_OFF = 0x019AC3A6, 0x01A85840
CHORD_KEYDOWN_CALL, TE_NOTE_ON = 0x019AC290, 0x01A85750   # keyboard note-on CALL (notes/seq-chord.md)
SYNTH_SHIFTKEY_SLOT, TE_SYNTH_SHIFTKEY = 0x019119B0, 0x01A50904
# Screen: iter's screen page vtable 0x019103FC, slot +0x1C = update (notes/screen.md)
ITER_PAGE_UPDATE_SLOT, ITER_PAGE_UPDATE = 0x01910418, 0x019B9EA4
# FX screen: the filter FX screen page (object 0x01DD8B44) vtable 0x01910CD4, slot +0x1C = update (notes/fx-slot.md)
FILTER_PAGE_UPDATE_SLOT, FILTER_PAGE_UPDATE = 0x01910CF0, 0x019E8308
FX_UI_KINDS = {"ensemble": 1, "pingpong": 2, "distortion": 3}   # by source; must match ui.c's FX_* enum
TE_UI = {"_te_raster": 0x01A3B490, "_te_line": 0x01A24A4C, "_te_text8": 0x019C5BD4,
         "_iter_update": ITER_PAGE_UPDATE, "_filter_page_update": FILTER_PAGE_UPDATE,
         "_te_note_off": 0x01A85840, "_te_note_on": 0x01A85750, "_te_make_note": 0x01A85014, "_te_synth_shiftkey": 0x01A50904,
         "_voice_ram": 0x00810000,       # engines' per-voice RAM in FREE region 0 (src/engine/build.sh)
         "_te_engine_ids": 0xFF8016E4, "_te_ssamp": 0x01DF6F78, "_te_knobs": 0xFF8013C4,
         "_te_synth_slot": 0x01AAAC0C,   # grain_source.h
         "_te_fx_filter_mono_ctor": 0x019E77B4, "_te_fx_filter_stereo_ctor": 0x019E784C,
         # TE's retained SVG renderer (notes/svg-route.md)
         "_te_yaffs_open": 0x01A997F4, "_te_yaffs_close": 0x01A99908, "_te_svg_load": 0x019FB458,
         "_te_node_find": 0x019EB8AC, "_te_node_matrix": 0x019EAD58, "_te_svg_render": 0x01A3B4C8,
         "_te_svg_stroke": 0x01A3B854, "_te_svg_fill": 0x01A3B96C}
# `Rx = 15` bounds: the name-table loops (entries, including aliases) and the factory/ScreenId id checks
NAME_BOUNDS = [0x019B97D0, 0x019B9826]
ID_BOUNDS = [0x01A1B7DC, 0x01A3BF44]
# code building each table's address (first instruction of the .L/.H pair)
TABLE_REFS = {
    NAME_TABLE: [0x019B97D2, 0x019B9816],
    FACTORY_JT: [0x01A1B7EA],
    SCREEN_JT: [0x01A3BF50],
    RANGE_TABLE: [0x01A1DEC2, 0x01A1E014, 0x01A1E59E, 0x01A1F274, 0x01A28132, 0x01A28314],
}
# 16-byte per-engine flag tables (1 for sampler/drum/dbox/dsynth): LFO modules and FUN_01a27eb4.
# 0x01a27e8c reads entry 8 of the last one directly and stays on the stock table.
FLAG_TABLES = {
    0x018E142C: [0x019CA012, 0x019CA24A],
    0x018E151C: [0x019CB2AC, 0x019CB35E],
    0x018E1710: [0x019CCFCA, 0x019CD096],
    0x018E182C: [0x019CDD7C, 0x019CDE52],
    0x018E19FC: [0x019CFC50, 0x019CFDE8],
    0x018F5ECC: [0x01A27EBE],
}
# Our FX types, firmware FX ids 8, 9, ... (notes/fx-slot.md). TE has 8 FX (ids 0-7); every table below is
# followed by other data, so it is copied into the slot block and grown.
FX_ID = 8                                  # first of ours = TE's FX count
FX_NAME_TABLE = 0x018DD810                 # {char *name, int id} x 8 (fx_types.cpp)
FX_MONO_JT, FX_STEREO_JT, FX_SCREEN_JT = 0x018F59AC, 0x018F5910, 0x018F7FE4
FX_RANGE_TABLE = 0x01DE5C50                # 8 x 0x30 B {min, max, scale} x 8 knobs; 0x01DE5DD0 is another table
FX_FILTER = 5                              # we construct TE's filter FX and swap its vtable
FX_FILTER_MONO_VT, FX_FILTER_STEREO_VT = 0x01910C94, 0x01910CB4
# `Rx/Px = 8` bounds: the name loops id->name, valid-name, name->id (entries, including aliases); the mono and
# stereo factories and ScreenId fx (ids)
FX_NAME_BOUNDS = [0x019AAD40, 0x019AAE02, 0x019AAE5A]
FX_ID_BOUNDS = [0x01A1B6DE, 0x01A1B5F0, 0x01A3BEC8]
FX_TABLE_REFS = {                          # (table address as loaded, offset) -> first insn of each .L/.H pair
    (FX_NAME_TABLE, 4): [0x019AAD38],      # id->name walks the id column
    (FX_NAME_TABLE, 0): [0x019AAE06, 0x019AAE4A],
    (FX_MONO_JT, 0): [0x01A1B6E6],
    (FX_STEREO_JT, 0): [0x01A1B5FE],
    (FX_SCREEN_JT, 0): [0x01A3BED4],
    (FX_RANGE_TABLE, 0): [0x01977584, 0x019CA630, 0x019CB5CC, 0x019CD298, 0x019CDFE2, 0x019CF0E4, 0x019D0160,
                          0x019DA092, 0x019DA88E, 0x01A1DF6A, 0x01A1F712, 0x01A28E62, 0x01A2907A, 0x01A29280,
                          0x01A506BC],
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stock")
    ap.add_argument("engines")
    ap.add_argument("out")
    ap.add_argument("--db-in")
    ap.add_argument("--db-out")
    ap.add_argument("--db-trim", action="append", default=[], metavar="FOLDER=N")
    ap.add_argument("--hold", action="store_true", help="install the HOLD hooks (SHIFT+T2 on the synth screen)")
    ap.add_argument("--chord", action="store_true", help="install the CHORD note-on hook (needs --hold)")
    ap.add_argument("--svg-cost", action="store_true", help="show the SVG render cost (kilocycles) on SVG FX screens")
    ap.add_argument("--debug-ui", action="store_true", help="show the profiler/diagnostic lines on our screens")
    ap.add_argument("--db-enable-filter", action="store_true",
                    help="add TE's hidden 'filter' FX (fx_types id 2, as op1repacker's 'filter' mod)")
    ap.add_argument("--fx", metavar="FX.json", help="add our FX types (ids 8, 9, ...), e.g. src/slot/fx.json")
    ap.add_argument("--slot-addr", type=lambda s: int(s, 0), default=0x01E20000)
    ap.add_argument("--build", default=os.path.join(ROOT, "firmware", "build", "slot"))
    a = ap.parse_args()
    fxs = json.load(open(a.fx)) if a.fx else []
    if a.chord and not a.hold:
        sys.exit("--chord needs --hold (chord notes are released through the key-up hook)")
    engines = json.load(open(a.engines))
    n = len(engines)
    count = FIRST_ID + n
    if not 1 <= n or count > 63:
        sys.exit("need 1..48 engines")
    os.makedirs(a.build, exist_ok=True)
    blocks = ldr.parse(open(a.stock, "rb").read())

    def rd(addr, size):
        for b in blocks:
            if b.loads_memory and not b.is_fill and not b.is_encrypted and b.target <= addr < b.target + b.count:
                return b.payload[addr - b.target:addr - b.target + size]
        sys.exit(f"0x{addr:08x} not in stock plaintext")

    def half(addr):
        return struct.unpack("<H", rd(addr, 2))[0]

    fjt = struct.unpack("<15I", rd(FACTORY_JT, 60))
    sjt = struct.unpack("<15I", rd(SCREEN_JT, 60))
    vt = struct.unpack("<12I", rd(ITER_VTABLE, 48))
    names = [rd(NAME_BLOB + o, 16).split(b"\0")[0].decode() for o in NAME_OFFS]
    aliases = [(alias, FIRST_ID + i) for i, e in enumerate(engines) for alias in e.get("aliases", [])]
    ours = [e["name"] for e in engines] + [a_ for a_, _ in aliases]
    name_count = count + len(aliases)
    if names[14] != "iter" or vt[5] != ITER_RENDER or set(ours) & set(names) or len(set(ours)) != len(ours) \
            or name_count > 63:
        sys.exit(f"unexpected stock tables (names {names}, render 0x{vt[5]:08x}) or engine name clash")

    # Build each engine at its address; remember each one's data blob address for the screen (ui.c).
    data_syms = {}
    for e in engines:
        out = os.path.join(a.build, e["name"])
        os.makedirs(out, exist_ok=True)
        ram = f"--defsym=_{e['source']}_ram={e['ram']}" if "ram" in e else ""
        r = subprocess.run([os.path.join(ROOT, "src", "engine", "build.sh"), e["addr"], out, e["max"]],
                           env=dict(os.environ, ENGINE=e["source"], ENGINE_DEFSYMS=ram), capture_output=True, text=True)
        open(os.path.join(out, "engine.dis"), "w").write(r.stdout + r.stderr)
        if r.returncode:
            sys.exit(f"engine {e['name']} build failed:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
        m = re.search(r"^([0-9a-f]{8}) \w _engine_data$", r.stdout, re.M)
        if m:
            data_syms[f"_{e['source']}_data"] = int(m.group(1), 16)   # by source file, so renames are safe
        print(f"engine {FIRST_ID + engines.index(e)} {e['name']}: {os.path.getsize(out + '/engine.bin')} B "
              f"at {e['addr']} ({e['source']}.c)")

    # Generated tables.
    def longs(ws):
        return "".join(f"    .long 0x{w:08x}\n" for w in ws)

    g = ["/* Generated by tools/build_slot.py from the stock image and engines.json. TE data: do not commit. */",
         "    .text", "    .align 4"]
    for i in range(n):
        g += [f"_factory_case{FIRST_ID + i}:", f"    FACTORY_CASE _vtable{FIRST_ID + i}"]
    g += ["    .align 4"]
    for i, e in enumerate(engines):
        g += [f"_vtable{FIRST_ID + i}:", longs(vt[:5]) + f"    .long {e['addr']}    /* render */\n" + longs(vt[6:])]
    g += ["    .global _names", "_names:"]
    g += [f"    .long 0x{NAME_BLOB + o:08x}, {i}    /* {nm} */" for i, (o, nm) in enumerate(zip(NAME_OFFS, names))]
    g += [f"    .long _name{FIRST_ID + i}, {FIRST_ID + i}" for i in range(n)]
    g += [f"    .long _alias{j}, {eid}    /* alias */" for j, (_, eid) in enumerate(aliases)]
    g += ["    .global _factory", "_factory:", longs(fjt)] + [f"    .long _factory_case{FIRST_ID + i}" for i in range(n)]
    g += ["    .global _screens", "_screens:", longs(sjt)] + [f"    .long 0x{sjt[14]:08x}" for _ in range(n)]
    # Screen kind per engine id 15.. (ui.c draws by kind, so ids can change): see UI_KINDS
    g += ["    .global _ui_kind", "_ui_kind:", "    .byte " + ", ".join(str(UI_KINDS[e.get("ui", e["source"])]) for e in engines), "    .align 4"]
    g += ["    .global _ranges", "_ranges:", "    .space 15 * 0x40"] + ["    RANGE_ROW"] * n
    for j, t in enumerate(FLAG_TABLES):
        flags = rd(t, 16) + bytes(count - 16)
        g += [f"    .global _flags{j}", f"_flags{j}:", "    .byte " + ", ".join(str(b) for b in flags), "    .align 4"]
    for i, e in enumerate(engines):
        g += ["    .align 4", f"_name{FIRST_ID + i}:", f"    .asciz \"{e['name']}\""]   # aligned like TE's
    for j, (alias, _) in enumerate(aliases):
        g += ["    .align 4", f"_alias{j}:", f"    .asciz \"{alias}\""]
    if fxs:
        fx_names_blob = struct.unpack("<16I", rd(FX_NAME_TABLE, 64))
        fx_names = [rd(fx_names_blob[2 * i], 12).split(b"\0")[0].decode() for i in range(FX_ID)]
        mono_jt = struct.unpack(f"<{FX_ID}I", rd(FX_MONO_JT, 4 * FX_ID))
        stereo_jt = struct.unpack(f"<{FX_ID}I", rd(FX_STEREO_JT, 4 * FX_ID))
        screen_jt = struct.unpack(f"<{FX_ID}I", rd(FX_SCREEN_JT, 4 * FX_ID))
        vmono = struct.unpack("<10I", rd(FX_FILTER_MONO_VT - 8, 40))
        vstereo = struct.unpack("<10I", rd(FX_FILTER_STEREO_VT - 8, 40))
        fx_aliases = [(al, FX_ID + i) for i, f in enumerate(fxs) for al in f.get("aliases", [])]
        ours_fx = [f["name"] for f in fxs] + [al for al, _ in fx_aliases]
        if fx_names_blob[1::2] != tuple(range(FX_ID)) or fx_names[FX_FILTER] != "filter" \
                or set(ours_fx) & set(fx_names) or len(set(ours_fx)) != len(ours_fx) \
                or vmono[7] != 0x019E7934 or vstereo[7] != 0x019E7EE4 or len(ours_fx) + FX_ID > 63:
            sys.exit(f"unexpected stock FX tables ({fx_names}) or FX name clash")
        g += ["    .text", "    .align 4"]
        for i, f in enumerate(fxs):
            fid, src = FX_ID + i, f["source"]
            g += [f"    FX_MONO_ENTRY _fx{fid}_mono_entry, _{src}_mono",
                  f"    FX_STEREO_ENTRY _fx{fid}_stereo_entry, _{src}_stereo",
                  f"_fx{fid}_mono_case:", f"    FX_CASE _te_fx_filter_mono_ctor, _fx{fid}_vt_mono",
                  f"_fx{fid}_stereo_case:", f"    FX_CASE _te_fx_filter_stereo_ctor, _fx{fid}_vt_stereo", "    .align 4"]
            for tag, v in (("mono", vmono), ("stereo", vstereo)):   # copied from vptr - 8; process (+0x14) is ours
                g += [longs(v[:2]).rstrip("\n"), f"_fx{fid}_vt_{tag}:", longs(v[2:7]).rstrip("\n"),
                      f"    .long _fx{fid}_{tag}_entry    /* process */", longs(v[8:]).rstrip("\n")]
        n_fx = len(fxs)
        g += ["    .global _fx_names", "_fx_names:", longs(fx_names_blob).rstrip("\n")]
        g += [f"    .long _fx_name{FX_ID + i}, {FX_ID + i}" for i in range(n_fx)]
        g += [f"    .long _fx_alias{j}, {fid}    /* alias */" for j, (_, fid) in enumerate(fx_aliases)]
        g += ["    .global _fx_mono_jt", "_fx_mono_jt:", longs(mono_jt).rstrip("\n")]
        g += [f"    .long _fx{FX_ID + i}_mono_case" for i in range(n_fx)]
        g += ["    .global _fx_stereo_jt", "_fx_stereo_jt:", longs(stereo_jt).rstrip("\n")]
        g += [f"    .long _fx{FX_ID + i}_stereo_case" for i in range(n_fx)]
        g += ["    .global _fx_screens", "_fx_screens:", longs(screen_jt).rstrip("\n")]
        g += [f"    .long 0x{screen_jt[FX_FILTER]:08x}    /* filter's screen */" for _ in range(n_fx)]
        g += ["    .global _fx_ranges", "_fx_ranges:", f"    .space {FX_ID} * 0x30"] + ["    FX_RANGE_ROW"] * n_fx
        for i, f in enumerate(fxs):
            g += ["    .align 4", f"_fx_name{FX_ID + i}:", f"    .asciz \"{f['name']}\""]
        for j, (al, _) in enumerate(fx_aliases):
            g += ["    .align 4", f"_fx_alias{j}:", f"    .asciz \"{al}\""]
    g += ["    .align 4", "    .global _fx_count", "_fx_count:", f"    .long {len(fxs)}",
          "    .global _fx_kind", "_fx_kind:", "    .byte " + ", ".join(str(FX_UI_KINDS.get(f["source"], 0)) for f in fxs + [{"source": ""}])]
    g += ["    .align 4", ""]
    open(os.path.join(a.build, "slot_gen.inc"), "w").write("\n".join(g))
    open(os.path.join(a.build, "slot.ld"), "w").write(
        f"SECTIONS {{ .text 0x{a.slot_addr:08x} : {{ *(.text) *(.text.*) *(.rodata) *(.rodata.*) *(.data) }}\n"
        "  .bss : { *(.bss) *(.bss.*) *(COMMON) } /DISCARD/ : { *(.comment) } }\n"
        "ASSERT(SIZEOF(.bss) == 0, \"ui.c must not use writable globals\")\n")

    fx_ram = {f"_{f['source']}_ram": int(f["ram"], 16) for f in fxs + engines if "ram" in f}   # FREE SDRAM buffers
    syms = f"--defsym=_iter_get=0x{ITER_GET:08x} --defsym=_iter_init=0x{ITER_INIT:08x} " + \
        " ".join(f"--defsym={k}=0x{v:08x}" for k, v in {**TE_UI, **data_syms, **fx_ram}.items())
    out = subprocess.run(
        ["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{ROOT}/src/slot:/src:ro",
         "-v", f"{ROOT}/src/engine:/engine:ro",
         "-v", f"{os.path.abspath(a.build)}:/out", "op1-bfin", "sh", "-c",
         "set -e; bfin-elf-as -I /out -I /src -o /out/slot.o /src/slot.S; "
         "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror "
         f"-I /engine {'-DUI_DEBUG ' if a.debug_ui else ''}{'-DSVG_COST ' if a.svg_cost else ''}-c -o /out/ui.o /src/ui.c; "
         "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror "
         "-c -o /out/hold.o /src/hold.c; "
         + "".join("bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror "
                   f"-c -o /out/fx_{f['source']}.o /src/{f['source']}.c; " for f in fxs) +
         f"bfin-elf-ld -nostdlib -T /out/slot.ld {syms} -o /out/slot.elf /out/slot.o /out/ui.o /out/hold.o "
         + "".join(f"/out/fx_{f['source']}.o " for f in fxs) + "; "
         "bfin-elf-objcopy -O binary /out/slot.elf /out/slot.bin; "
         "bfin-elf-objdump -d /out/slot.elf > /out/slot.dis; bfin-elf-nm /out/slot.elf"],
        capture_output=True, text=True)
    if out.returncode:
        sys.exit(f"slot build failed:\n{out.stdout[-3000:]}{out.stderr[-3000:]}")
    out = out.stdout
    sym = {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3}

    # Halfword patches, each checked against stock.
    halves = []
    for addr, value in [(a_, name_count) for a_ in NAME_BOUNDS] + [(a_, count) for a_ in ID_BOUNDS]:
        old = half(addr)                         # `Rx = 15` (imm7 in bits 3..9) -> `Rx = value`
        if (old >> 10) not in (0x18, 0x1A) or (old >> 3) & 0x7F != 15:
            sys.exit(f"0x{addr:08x}: expected `Rx = 15`, found 0x{old:04x}")
        halves.append((addr, old, (old & ~0x3F8) | (value << 3)))

    if fxs:
        n_names = FX_ID + len(fxs) + sum(len(f.get("aliases", [])) for f in fxs)
        for addr, value in [(x, n_names) for x in FX_NAME_BOUNDS] + [(x, FX_ID + len(fxs)) for x in FX_ID_BOUNDS]:
            old = half(addr)                     # `Rx/Px = 8` -> value
            if (old >> 10) not in (0x18, 0x1A) or (old >> 3) & 0x7F != FX_ID:
                sys.exit(f"0x{addr:08x}: expected `Rx = {FX_ID}`, found 0x{old:04x}")
            halves.append((addr, old, (old & ~0x3F8) | (value << 3)))

    def repoint(insn, old, new):
        """`Rx.L/.H = old` at insn with its partner within 16 bytes after it: patch both immediates."""
        first = half(insn)
        if first & 0xFFA0 != 0xE100:
            sys.exit(f"0x{insn:08x}: not a .L/.H load (0x{first:04x})")
        for p in range(insn + 4, insn + 20, 2):
            hw = half(p)
            if hw & 0xFFA0 == 0xE100 and hw & 0x3F == first & 0x3F and (hw ^ first) & 0x40:
                pair = {first & 0x40: insn, hw & 0x40: p}
                if half(pair[0] + 2) != old & 0xFFFF or half(pair[0x40] + 2) != old >> 16:
                    continue
                return [(pair[0] + 2, old & 0xFFFF, new & 0xFFFF), (pair[0x40] + 2, old >> 16, new >> 16)]
        sys.exit(f"0x{insn:08x}: no matching .L/.H pair for 0x{old:08x}")

    for table, sites in TABLE_REFS.items():
        new = sym[{NAME_TABLE: "_names", FACTORY_JT: "_factory", SCREEN_JT: "_screens", RANGE_TABLE: "_ranges"}[table]]
        halves += [p for s in sites for p in repoint(s, table, new)]
    for j, (table, sites) in enumerate(FLAG_TABLES.items()):
        halves += [p for s in sites for p in repoint(s, table, sym[f"_flags{j}"])]
    if fxs:
        new_of = {FX_NAME_TABLE: "_fx_names", FX_MONO_JT: "_fx_mono_jt", FX_STEREO_JT: "_fx_stereo_jt",
                  FX_SCREEN_JT: "_fx_screens", FX_RANGE_TABLE: "_fx_ranges"}
        for (table, off), sites in FX_TABLE_REFS.items():
            halves += [p for s in sites for p in repoint(s, table + off, sym[new_of[table]] + off)]

    cmd = [sys.executable, os.path.join(ROOT, "tools", "ldr_patch.py"), a.stock, a.out]
    # Runtime RAM that is not loaded but must not be overlapped by code (ldr_patch.py checks it).
    cmd += ["--reserve", "0x00800000-0x00800020=prof_stats", "--reserve", "0x00810000-0x00818000=voice_ram"]
    for f in fxs:
        if "ram" in f:
            if "ram_size" not in f:
                sys.exit(f"fx {f['name']}: \"ram\" needs \"ram_size\"")
            lo = int(f["ram"], 16)
            cmd += ["--reserve", f"0x{lo:08x}-0x{lo + int(f['ram_size'], 16):08x}={f['source']}_ram"]
    for e in engines:
        if "ram" in e:
            lo = int(e["ram"], 16)
            cmd += ["--reserve", f"0x{lo:08x}-0x{lo + int(e['ram_size'], 16):08x}={e['source']}_ram",
                    "--fill", f"0x{lo:08x}-0x{lo + 0x100:08x}=0"]
        cmd += ["--block", f"{e['addr']}={os.path.join(a.build, e['name'], 'engine.bin')}"]
    cmd += ["--block", f"0x{a.slot_addr:08x}={os.path.join(a.build, 'slot.bin')}"]
    cmd += ["--pointer", f"0x{ITER_PAGE_UPDATE_SLOT:08x}=0x{ITER_PAGE_UPDATE:08x}:0x{sym['_ui_update']:08x}"]
    if fxs:
        cmd += ["--pointer", f"0x{FILTER_PAGE_UPDATE_SLOT:08x}=0x{FILTER_PAGE_UPDATE:08x}:0x{sym['_fx_ui_update']:08x}"]
    if a.hold:
        cmd += ["--pointer", f"0x{SYNTH_SHIFTKEY_SLOT:08x}=0x{TE_SYNTH_SHIFTKEY:08x}:0x{sym['_hold_shiftkey_entry']:08x}"]
        # CALL pcrel25m2: hw0 = 0xE300 | off[23:16], hw1 = off[15:0], off = (target - pc) / 2
        def call_halves(pc, target):
            off = (target - pc) >> 1
            if not -(1 << 23) <= off < (1 << 23):
                sys.exit(f"CALL at 0x{pc:08x} cannot reach 0x{target:08x}")
            off &= 0xFFFFFF
            return 0xE300 | (off >> 16), off & 0xFFFF
        calls = [(HOLD_KEYUP_CALL, TE_NOTE_OFF, "_hold_keyup_entry")]
        if a.chord:
            calls.append((CHORD_KEYDOWN_CALL, TE_NOTE_ON, "_chord_keydown_entry"))
        for pc, stock, ours in calls:
            old = call_halves(pc, stock)
            new = call_halves(pc, sym[ours])
            if (half(pc), half(pc + 2)) != old:
                sys.exit(f"0x{pc:08x} is not the expected CALL 0x{stock:08x}")
            cmd += ["--half", f"0x{pc:08x}=0x{old[0]:04x}:0x{new[0]:04x}",
                    "--half", f"0x{pc + 2:08x}=0x{old[1]:04x}:0x{new[1]:04x}"]
    cmd += [x for addr, old, new in halves for x in ("--half", f"0x{addr:08x}=0x{old:04x}:0x{new:04x}")]
    subprocess.run(cmd, check=True)
    print(f"{len(halves)} halfword patches; slot symbols:",
          ", ".join(f"{k} 0x{v:08x}" for k, v in sorted(sym.items(), key=lambda kv: kv[1]) if v >= a.slot_addr))

    if a.db_in and a.db_out:
        src = a.db_in
        for i, e in enumerate(engines):
            dst = a.db_out if i == n - 1 else os.path.join(a.build, f"db_{i}.db")
            cmd = [sys.executable, os.path.join(ROOT, "tools", "add_engine_db.py"), src, dst,
                   "--type", e["name"], "--default", json.dumps(e["default"])]
            cmd += [x for name, k in e["presets"].items() for x in ("--preset", f"{name}={','.join(map(str, k))}")]
            subprocess.run(cmd, check=True)
            src = dst
        con = sqlite3.connect(a.db_out)
        for f in fxs:
            con.execute("INSERT INTO fx_types VALUES ((SELECT max(id) + 1 FROM fx_types), ?, ?)",
                        (f["name"], json.dumps(f["default"])))
        if a.db_enable_filter and not con.execute("SELECT count(*) FROM fx_types WHERE type='filter'").fetchone()[0]:
            con.execute("INSERT INTO fx_types VALUES (2, 'filter', '[7548, 0, 8272, 19572, 8000, 8000, 8000, 8000]')")
        for spec in a.db_trim:
            folder, _, keep = spec.partition("=")
            con.execute("DELETE FROM synth_presets WHERE folder=? AND id NOT IN "
                        "(SELECT id FROM synth_presets WHERE folder=? ORDER BY id LIMIT ?)", (folder, folder, int(keep)))
        con.commit()
        con.execute("VACUUM")
        counted = con.execute("SELECT count(*) FROM synth_presets WHERE folder != 'sampler'").fetchone()[0]
        per = con.execute("SELECT folder, count(*) FROM synth_presets GROUP BY folder ORDER BY folder").fetchall()
        con.close()
        print(f"synth presets (without sampler): {counted} of {MAX_SYNTH_PRESETS}:", dict(per))
        if counted > MAX_SYNTH_PRESETS:
            os.remove(a.db_out)
            sys.exit(f"too many synth presets ({counted} > {MAX_SYNTH_PRESETS}): the OP-1 asserts at boot. "
                     "Use fewer presets or --db-trim")


if __name__ == "__main__":
    main()
