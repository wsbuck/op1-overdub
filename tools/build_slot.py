#!/usr/bin/env python3
"""Build an OP1_vdk.ldr (and optionally a factory DB) with extra synth engines, ids 15, 16, ...

  build_slot.py STOCK_OP1_vdk.ldr ENGINES.json OUT.ldr [--db-in IN.db --db-out OUT.db]
                [--slot-addr 0x01E20000] [--build DIR]

ENGINES.json (e.g. src/slot/engines.json) lists the engines in id order:
  [{"name": "va", "source": "va", "addr": "0x01E10000", "max": "0x10000",
    "default": [8 knobs], "presets": {"saw lead": [k0, k1, k2, k3], ...},
    "aliases": ["old name", ...]}, ...]
Optional "patch": fields set in every preset of that engine (e.g. {"lfo_active": false}); "preset_patch": {preset name:
fields} for single presets (e.g. an FX on).
Aliases are extra name-table entries for the same id, so patches saved under an old name still load (TE asserts
on an unknown engine name, instr_types.cpp:49); only "name" gets a DB row and presets.
Each engine is src/engine/<source>.c built by src/engine/build.sh at its addr (a FREE region). Optional "ram",
"ram_size": a FREE SDRAM buffer _<source>_ram shared by the engine and the screen (ui.c), reserved like an FX's; its
first 256 bytes are zero-filled by the image (a header that must not hold stale values at boot).
src/slot/slot.S is assembled at --slot-addr with tables generated into DIR/slot_gen.inc from the stock
image (TE data, never committed). TE's name lookup, producer factory, ScreenId, knob-range and
per-engine flag-table code is repointed at the grown tables by checked halfword patches
(tools/ldr_patch.py --half). iter (id 14) stays stock. See notes/engine-slot.md.
Writes DIR/features.json: the address of slot.S's feature mask word and the hooks installed (--hold; --chord installs
chord and chord2, CHORD v2's press / release / encoder slots, frame hook and sequencer-step hook), for
tools/mkpatch.py (apply.py clears the mask bits of hooks the user leaves out).
With --db-in/--db-out, adds each engine's synth_types row and presets (tools/add_engine_db.py), optionally
enables TE's hidden filter FX (--db-enable-filter), keeps
only the first N presets of folders given with --db-trim FOLDER=N, and refuses a DB with more than
MAX_SYNTH_PRESETS non-sampler presets: patch.cpp:616 asserts on that at boot (SLOT3.OP1).
With --fx FX.json (e.g. src/slot/fx.json), adds our FX as FX ids 8, 9, ... and their fx_types rows:
  [{"name": "ensemble", "source": "ensemble", "default": [8 knobs], "aliases": ["ens"]}, ...]
  optional "ram": "0x01E80000", "ram_size": "0x60000" defines _<source>_ram there (a FREE SDRAM buffer: undefined
  contents at boot); the range is reserved, so no code block may overlap it.
Each is src/slot/<source>.c exporting <source>_mono and <source>_stereo (notes/fx-slot.md).
An engine with "drum": true is a drum engine (any number, notes/drum-engine.md): drum engines get the ids after the
synth engines, run on TE's dbox voice objects, and are src/slot/<source>.c in the slot block (<source>_render,
_note_on, _active, _note_off, _slot_init; <source>_id is defined as the engine's id). TE's drum factory, drum ScreenId,
drum encoder, patch validator and the dbox screen's update are patched to accept them; the screen is drawn by kind
(DRUM_KINDS, ui.c). DB: "replace" swaps the dbox preset of that name for ours (same folder and row count) with its
patch, type and name changed ("preset") and dbox_data = 24 x "dbox_default"; "kits" (a JSON file, src/slot/...:
{name: {"template": a stock dbox preset, "dbox_data": 24 x 8}}) adds one preset per kit with the template's
envelope/FX/LFO fields, in a folder named after the engine: TE's drum browser makes one column per distinct folder,
labelled with the folder name (notes/drum-engine.md "Browser columns"). Every per-key value is checked against the stock dbox presets' per-column range (TE validates
factory presets at boot), and factory drum presets stay <= MAX_FACTORY_DRUM_PRESETS (factory + user drum sounds
must stay <= 58, patch.cpp:656: the rest is the user's room). TE's per-key loader and saver are patched to treat our
types like "dbox", so per-key values load and save with the sound.
Optional "ranges": 8 x [min, max, scale, type] for an engine's knob range row (default: RANGE_ROW, 0..0x7FFF type 0);
defaults and presets must lie inside it (TE's patch validator rejects other knob values, error 15); presets may then
give all 8 knob values (signed, checked against the row). "knob_hook": true (any number of synth engines whose source
is in HOOK_KINDS) routes those engines' engine-page encoder turns through src/slot/knobhook.c (notes/knob-hook.md).
"""
import argparse
import json
import os
import re
import shutil
import struct
import sqlite3
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRST_ID = 15
UI_KINDS = {"va": 1, "wavetable": 2, "modal": 3, "grain": 4, "inprobe": 5, "sprobe": 6, "amp": 7, "harm": 8,
            "fm4": 9}   # must match enum ui_kind in src/slot/ui.c
HOOK_KINDS = {"sprobe": 1, "harm": 2, "fm4": 3, "va": 4}   # by source; must match knobhook.c's HOOK_* enum
DRUM_KINDS = {"drumprobe": 1, "fmdrum": 2, "metal": 3}   # by source; must match ui.c's DRUM_* enum
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
# CHORD v2 (notes/chord2.md "MVP as built"): the synth controller's press / release / encoder slots (vtable 0x01911994
# +0x14, +0x24, +0x2C) and the GUI loop's frame render CALL (FUN_019ab880 -> FUN_019b2674)
SYNTH_PRESS_SLOT, TE_SYNTH_PRESS = 0x019119A8, 0x01A51058
SYNTH_RELEASE_SLOT, TE_SYNTH_RELEASE = 0x019119B8, 0x01A5056C
SYNTH_KNOB_SLOT, TE_SYNTH_KNOB = 0x019119C0, 0x01A505BC
# repeat's manual key (the green encoder's push; src/slot/repeat.h): the drum controller (vtable 0x0190FA64, built by
# FUN_019777xx with id 1) and the mixer controller (0x01910AAC, MasterController ctor 0x019D97A2, id 3): press +0x14,
# release +0x24, the same layout as the synth controller's
REPEAT_KEY_SLOTS = ((0x0190FA78, 0x01978008, "_rk_drum_press_entry"), (0x0190FA88, 0x019773E4, "_rk_drum_release_entry"),
                    (0x01910AC0, 0x019DB7C4, "_rk_master_press_entry"), (0x01910AD0, 0x019DB708, "_rk_master_release_entry"))
FRAME_RENDER_CALL, TE_FRAME_RENDER = 0x019AB8B2, 0x019B2674
# CHORD v2 STRUM (notes/chord2.md phase 2): the sequencer thread's loop FUN_01a4318c calls its step FUN_01a0d2c0 here
SEQ_STEP_CALL, TE_SEQ_STEP = 0x01A431AA, 0x01A0D2C0
# Screen: iter's screen page vtable 0x019103FC, slot +0x1C = update (notes/screen.md)
ITER_PAGE_UPDATE_SLOT, ITER_PAGE_UPDATE = 0x01910418, 0x019B9EA4
# FX screen: the filter FX screen page (object 0x01DD8B44) vtable 0x01910CD4, slot +0x1C = update (notes/fx-slot.md)
FILTER_PAGE_UPDATE_SLOT, FILTER_PAGE_UPDATE = 0x01910CF0, 0x019E8308
# Drum engine (notes/drum-engine.md): dbox's voice ctor/index setter, vtable and screen update; the code sites patched
DBOX = 9
DBOX_VTABLE = 0x0190F8A4                   # +0x0C dtor, +0x14 render, +0x1C note-on, +0x24 active, +0x2C note-off
DBOX_FNS = (0x01970EBC, 0x01970EE8, 0x01970DE6, 0x01970E5C, 0x01970E7E)
DBOX_PAGE_UPDATE_SLOT, DBOX_PAGE_UPDATE = 0x0190F8F0, 0x01972470
DRUM_FACTORY_ASSERT = 0x01A1BA38           # `R0.L = 0x5A40` (the assert message): -> JUMP.L _drum_factory
DRUM_SCREEN_R1, DRUM_SCREEN_CC = 0x01A3BFE8, 0x01A3BFEA   # `R1 = 9`, `CC = R0 == R1` in FUN_01a3bfda
DRUM_KNOB_R0, DRUM_KNOB_CC = 0x01A1E59A, 0x01A1E59C       # `R0 = 9`, `CC = R6 == R0` in FUN_01a1e57c
VALIDATE_LOOKUP_CALL, TE_ENGINE_LOOKUP = 0x01A03884, 0x019B9810   # in the patch validator FUN_01a037a8
# per-key loader FUN_01a0036c / saver FUN_019ffc50: CALL FUN_019bdea4 (JSON value ==) of "type" with Value("dbox")
TYPE_EQ_LOAD_CALL, TYPE_EQ_SAVE_CALL, TE_JSON_EQ = 0x01A00630, 0x019FFFBA, 0x019BDEA4
TE_JSON_STR, TE_JSON_FREE = 0x019BDC4C, 0x019BDD18      # Value(const char *), Value destructor (TE passes R1 = 2)
# synth engine-page encoder (notes/knob-hook.md): CALL FUN_01a1dfec (Ranges.cpp:810) before FUN_01a00d14 stores knobs
KNOB_APPLY_CALL, TE_KNOB_APPLY = 0x01A5063E, 0x01A1DFEC
MAX_DRUM_PATCHES = 58                      # patch.cpp:656/664: factory drum presets + drum_user rows
STOCK_DRUM_PRESETS = 24
MAX_FACTORY_DRUM_PRESETS = 32              # stock 24 + 8 kits (fmdrum 5, metal 3): leaves the user room for 26 saved drum
#                                            sounds (stock 34)
MAX_FOLDER_LABEL = 13                      # the preset browser prints a column label with snprintf(buf, 14, "%s")
FEATURE_BITS = {"hold": 1, "chord": 2, "chord2": 4}   # slot.S _feature_mask; must match hold_impl.h / chord2_impl.h FEAT_*
FX_UI_KINDS = {"ensemble": 1, "pingpong": 2, "distortion": 3, "crush": 4, "repeat": 5}   # by source; = ui.c's FX_* enum
TE_UI = {"_te_raster": 0x01A3B490, "_te_line": 0x01A24A4C, "_te_text8": 0x019C5BD4,
         "_iter_update": ITER_PAGE_UPDATE, "_filter_page_update": FILTER_PAGE_UPDATE,
         "_te_note_off": 0x01A85840, "_te_note_on": 0x01A85750, "_te_make_note": 0x01A85014, "_te_synth_shiftkey": 0x01A50904,
         "_te_synth_press": TE_SYNTH_PRESS, "_te_synth_release": TE_SYNTH_RELEASE, "_te_synth_knob": TE_SYNTH_KNOB,
         "_te_frame_render": TE_FRAME_RENDER, "_te_seq_tick": TE_SEQ_STEP,
         # yaffs direct API (TE's file copy 0x0199FD40..: open, read, write, close; mkdir at boot 0x0196F874)
         "_te_yaffs_read": 0x01A99C90, "_te_yaffs_write": 0x01A99F84, "_te_yaffs_mkdir": 0x01A9A886,
         "_voice_ram": 0x00810000,       # engines' per-voice RAM in FREE region 0 (src/engine/build.sh)
         "_te_engine_ids": 0xFF8016E4, "_te_ssamp": 0x01DF6F78, "_te_knobs": 0xFF8013C4,
         "_te_synth_slot": 0x01AAAC0C,   # grain_source.h
         "_te_tempo_spb": 0x01AADAD8,    # the tempo, samples per beat Q14 (pingpong.c; notes/fx-pingpong.md "Tempo")
         # repeat.c's bar clock and ui.c's probe (notes/fx-beatrepeat.md "Sync"): transport, playhead (samples x 64),
         # metronome countdown (Q14), the keyboard's synth slot, the sequencer's 96-PPQN tick (the probe only)
         "_te_tape_state": 0xFF802960, "_te_tape_pos": 0xFF802974, "_te_metro_count": 0x01AADADC,
         "_te_play_slot": 0xFF8016D4, "_te_ppqn_tick": 0x018C1884, "_te_blocks": 0x01AADAF4,
         # repeat's manual key (src/slot/repeat.h): TE's drum and mixer controllers' press / release functions
         "_te_drum_press": 0x01978008, "_te_drum_release": 0x019773E4,
         "_te_master_press": 0x019DB7C4, "_te_master_release": 0x019DB708,
         # audio-input buffers (src/engine/inprobe.h, notes/input.md; as src/engine/build.sh)
         "_te_audio_in": 0xFF800CB4, "_te_rx_sel": 0xFF800364, "_te_rx": 0xFF800370, "_te_alt_in": 0x01AB2E2C,
         # drum side (notes/drum-engine.md, src/slot/drumprobe.h)
         "_te_drum_key": 0xFF801334, "_te_drum_vel": 0xFF8012EC, "_te_drum_ratio": 0xFF801304,
         "_te_drum_edit": 0xFF801044, "_te_drum_params": 0x01AAADB4,
         "_te_drum_slot": 0xFF8016D8, "_te_drum_ids": 0xFF801704,   # current drum slot, drum engine id per slot
         "_te_fx_filter_mono_ctor": 0x019E77B4, "_te_fx_filter_stereo_ctor": 0x019E784C,
         # TE's retained SVG renderer (notes/svg-route.md)
         "_te_yaffs_open": 0x01A997F4, "_te_yaffs_close": 0x01A99908, "_te_svg_load": 0x019FB458,
         "_te_node_find": 0x019EB8AC, "_te_node_matrix": 0x019EAD58, "_te_svg_render": 0x01A3B4C8,
         "_te_svg_stroke": 0x01A3B854, "_te_svg_fill": 0x01A3B96C,
         "_te_drum_assert": DRUM_FACTORY_ASSERT + 4, "_te_dbox_voice": 0x0197189C, "_te_dbox_index": 0x01970DA8,
         "_te_engine_lookup": TE_ENGINE_LOOKUP, "_dbox_page_update": DBOX_PAGE_UPDATE,
         "_te_json_eq": TE_JSON_EQ, "_te_json_str": TE_JSON_STR, "_te_json_free": TE_JSON_FREE,
         "_te_knob_apply": TE_KNOB_APPLY}
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
# Per-FX byte flags "8 LFO targets" (notes/fx-slot.md "LFO target flag"), read as B[table + FX id], one 8-byte
# table each for the five LFO modules (DSP) and one for the five LFO screens (UI, FUN_01a27e70 drum / FUN_01a27e98
# synth). The LFO's target selector has 8 detents; with 0 the screen shows knob detent / 2 and the DSP modulates knob
# target >> 12 (4 knobs: what TE's FX ids 0-7 all have); nonzero: knob detent and target >> 11 (8 params, like the
# engine flags for sampler / drum). DSP and UI must agree. Each table is followed by other data (id 10 read 5), so
# all six are copied, grown, and our ids get TE's FX value (FX_LFO_REF's); the 6th LFO module (range read
# 0x019CF0E4) has no such table.
FX_FLAG_TABLES = {
    0x018E143C: [0x019CA612],
    0x018E152C: [0x019CB5AE],
    0x018E1720: [0x019CD276],
    0x018E183C: [0x019CDF92],
    0x018E1A0C: [0x019D0112],
    0x018F5EDC: [0x01A27E7A, 0x01A27EA2],  # UI: the LFO screens' target colour (LFOFX1 missed it: test image 81)
}
FX_LFO_REF = 0                             # delay: 4 knobs, the same flags as every TE FX
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


def branch_halves(pc, target, op):
    """CALL (op 0xE300) / JUMP.L (0xE200) pcrel25m2: hw0 = op | off[23:16], hw1 = off[15:0], off = (target - pc) / 2"""
    off = (target - pc) >> 1
    if not -(1 << 23) <= off < (1 << 23):
        sys.exit(f"0x{pc:08x} cannot reach 0x{target:08x}")
    off &= 0xFFFFFF
    return op | (off >> 16), off & 0xFFFF


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stock")
    ap.add_argument("engines")
    ap.add_argument("out")
    ap.add_argument("--db-in")
    ap.add_argument("--db-out")
    ap.add_argument("--db-trim", action="append", default=[], metavar="FOLDER=N")
    ap.add_argument("--hold", action="store_true", help="install the HOLD hooks (SHIFT+T2 on the synth screen)")
    ap.add_argument("--chord", action="store_true", help="install the CHORD hooks, CHORD1 and v2 (needs --hold)")
    ap.add_argument("--svg-cost", action="store_true", help="show the SVG render cost (kilocycles) on SVG FX screens")
    ap.add_argument("--staged-screens", action="store_true", help="crush screen: layer-by-layer bring-up (CRUSH_STAGED)")
    ap.add_argument("--test-card", action="store_true", help="crush screen: the SVG test card first (CRUSH_TEST_CARD)")
    ap.add_argument("--debug-ui", action="store_true", help="show the profiler/diagnostic lines on our screens")
    ap.add_argument("--db-enable-filter", action="store_true",
                    help="add TE's hidden 'filter' FX (fx_types id 2, as op1repacker's 'filter' mod)")
    ap.add_argument("--fx", metavar="FX.json", help="add our FX types (ids 8, 9, ...), e.g. src/slot/fx.json")
    ap.add_argument("--slot-addr", type=lambda s: int(s, 0), default=0x01E20000)
    ap.add_argument("--build", default=os.path.join(ROOT, "firmware", "build", "slot"))
    a = ap.parse_args()
    fxs = json.load(open(a.fx)) if a.fx else []
    if any(f["source"] == "repeat" for f in fxs) and not any(f["source"] == "pingpong" for f in fxs):
        sys.exit("fx repeat plays from pingpong's lines (pingpong_ram, fx_line_owner): build it with pingpong")
    if a.chord and not a.hold:
        sys.exit("--chord needs --hold (chord notes are released through the key-up hook)")
    engines = json.load(open(a.engines))
    drums = [e for e in engines if e.get("drum")]
    engines = [e for e in engines if not e.get("drum")] + drums      # ids: synth engines first, then the drum engines
    n = len(engines)
    count = FIRST_ID + n
    drum_first = count - len(drums)
    drum_ids = {e["name"]: drum_first + j for j, e in enumerate(drums)}
    if not 1 <= n or count > 62:
        sys.exit("need 1..47 engines")
    if any(e["source"] not in DRUM_KINDS for e in drums):
        sys.exit(f"drum engines need a screen kind in DRUM_KINDS ({', '.join(DRUM_KINDS)})")
    hooked = [FIRST_ID + i for i, e in enumerate(engines) if e.get("knob_hook")]
    if any(e.get("drum") or e["source"] not in HOOK_KINDS for e in engines if e.get("knob_hook")):
        sys.exit(f"\"knob_hook\" only on synth engines with a knobhook.c handler ({', '.join(HOOK_KINDS)})")
    for e in engines:                   # TE's validator rejects knob values outside the row (patch.cpp, error 15)
        if e.get("drum"):
            continue
        rows = e.get("ranges", [[0, 0x7FFF, 0x7FFF, 0]] * 8)
        if len(rows) != 8 or any(len(r) != 4 or not -32768 <= r[0] < r[1] <= 32767 for r in rows):
            sys.exit(f"engine {e['name']}: \"ranges\" must be 8 x [min, max, scale, type] with min < max")
        for what, k in [("default", e["default"])] + [(n, v + [0] * (8 - len(v))) for n, v in e["presets"].items()]:
            if any(not r[0] <= v <= r[1] for v, r in zip(k, rows)):
                sys.exit(f"engine {e['name']}: {what} {k} outside its range row")
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

    dvt = struct.unpack("<12I", rd(DBOX_VTABLE, 48))
    if drums and tuple(dvt[3::2]) != DBOX_FNS:
        sys.exit(f"unexpected stock dbox vtable {[hex(w) for w in dvt]}")

    # Build each engine at its address; remember each one's data blob address for the screen (ui.c).
    data_syms = {}
    for e in engines:
        if e.get("drum"):
            continue                                              # built into the slot block below
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
    nsyn = n - len(drums)
    for i in range(nsyn):
        g += [f"_factory_case{FIRST_ID + i}:", f"    FACTORY_CASE _vtable{FIRST_ID + i}"]
    g += ["    .align 4"]
    for i, e in enumerate(engines[:nsyn]):
        g += [f"_vtable{FIRST_ID + i}:", longs(vt[:5]) + f"    .long {e['addr']}    /* render */\n" + longs(vt[6:])]
    if drums:
        g += [f"    DRUM_HOOKS {drum_first}, {len(drums)}, " + ", ".join(f"_name{drum_ids[e['name']]}" for e in drums)]
    for e in drums:                     # TE's dbox vtable with render, note-on, active and note-off ours
        src, did = e["source"], drum_ids[e["name"]]
        g += [f"    C_ENTRY _drum{did}_{fn}_entry, _{src}_{fn}" for fn in ("render", "note_on", "active", "note_off", "slot_init")]
        g += ["    .align 4", f"_drum_vtable{did}:", longs(dvt[:5]) + f"    .long _drum{did}_render_entry\n" + longs(dvt[6:7])
              + f"    .long _drum{did}_note_on_entry\n" + longs(dvt[8:9]) + f"    .long _drum{did}_active_entry\n"
              + longs(dvt[10:11]) + f"    .long _drum{did}_note_off_entry",
              f"    .global _{src}_id", f"_{src}_id:", f"    .long {did}"]
    if drums:
        g += ["    .align 4", "_drum_vtables:"] + [f"    .long _drum_vtable{drum_ids[e['name']]}" for e in drums]
        g += ["_drum_inits:"] + [f"    .long _drum{drum_ids[e['name']]}_slot_init_entry" for e in drums]
    # the drum screen's kind per drum id from _drum_first (ui.c drum_ui_draw)
    g += ["    .align 4", "    .global _drum_first", "_drum_first:", f"    .long {drum_first}",
          "    .global _drum_count", "_drum_count:", f"    .long {len(drums)}",
          "    .global _drum_kind", "_drum_kind:", "    .byte " + (", ".join(str(DRUM_KINDS[e["source"]]) for e in drums) or "0"),
          "    .align 4"]
    if hooked:
        g += [f"    KNOB_HOOK {', '.join(map(str, hooked))}", "    .align 4"]
        # knobhook.c's handler per engine id 15.. (0 = not hooked)
        g += ["    .global _hook_kind", "_hook_kind:", "    .byte " + ", ".join(
            str(HOOK_KINDS[e["source"]] if e.get("knob_hook") else 0) for e in engines), "    .align 4"]
    g += ["    .global _names", "_names:"]
    g += [f"    .long 0x{NAME_BLOB + o:08x}, {i}    /* {nm} */" for i, (o, nm) in enumerate(zip(NAME_OFFS, names))]
    g += [f"    .long _name{FIRST_ID + i}, {FIRST_ID + i}" for i in range(n)]
    g += [f"    .long _alias{j}, {eid}    /* alias */" for j, (_, eid) in enumerate(aliases)]
    # A drum id on the synth side gets TE's synth-side cases for dbox (asserts), as TE does for 8 and 9.
    g += ["    .global _factory", "_factory:", longs(fjt)] + [f"    .long _factory_case{FIRST_ID + i}" for i in range(nsyn)]
    g += [f"    .long 0x{fjt[DBOX]:08x}    /* drum engine: dbox's case */" for _ in drums]
    g += ["    .global _screens", "_screens:", longs(sjt)] + [f"    .long 0x{sjt[14]:08x}" for _ in range(nsyn)]
    g += [f"    .long 0x{sjt[DBOX]:08x}    /* drum engine: dbox's case */" for _ in drums]
    # Screen kind per engine id 15.. (ui.c draws by kind, so ids can change): see UI_KINDS; 0 = none (drum)
    g += ["    .global _ui_kind", "_ui_kind:", "    .byte " + ", ".join(
        str(0 if e.get("drum") else UI_KINDS[e.get("ui", e["source"])]) for e in engines), "    .align 4"]
    g += ["    .global _ranges", "_ranges:", "    .space 15 * 0x40"]
    for e in engines:
        g += ["    .short " + ", ".join(f"{v}" for r in e["ranges"] for v in r)] if "ranges" in e else ["    RANGE_ROW"]
    for j, t in enumerate(FLAG_TABLES):
        flags = bytearray(rd(t, 16) + bytes(count - 16))
        for did in drum_ids.values():
            flags[did] = flags[DBOX]                              # the LFO modules treat our drum ids as dbox
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
        if any(f["source"] == "repeat" for f in fxs):
            g += [f"    C_ENTRY {ours}, {ours[:-6]}" for _, _, ours in REPEAT_KEY_SLOTS]
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
        for j, t in enumerate(FX_FLAG_TABLES):
            if rd(t, FX_ID) != bytes(FX_ID):                     # all TE FX 0 in 246 (fx_lfo_target_check.py)
                sys.exit(f"unexpected stock FX flag table 0x{t:08x}")
            flags = rd(t, FX_ID) + rd(t + FX_LFO_REF, 1) * n_fx
            g += [f"    .global _fx_flags{j}", f"_fx_flags{j}:", "    .byte " + ", ".join(str(b) for b in flags),
                  "    .align 4"]
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
         f"-I /engine {'-DUI_DEBUG ' if a.debug_ui else ''}{'-DSVG_COST ' if a.svg_cost else ''}{'-DCRUSH_STAGED ' if a.staged_screens else ''}{'-DCRUSH_TEST_CARD ' if a.test_card else ''}-c -o /out/ui.o /src/ui.c; "
         "bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror "
         "-c -o /out/hold.o /src/hold.c; "
         + ("bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror "
            "-I /engine -c -o /out/knobhook.o /src/knobhook.c; " if hooked else "")
         + "".join("bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror "
                   f"-I /engine -c -o /out/drum_{e['source']}.o /src/{e['source']}.c; " for e in drums)
         + "".join("bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror "
                   f"-c -o /out/fx_{f['source']}.o /src/{f['source']}.c; " for f in fxs) +
         f"bfin-elf-ld -nostdlib -T /out/slot.ld {syms} -o /out/slot.elf /out/slot.o /out/ui.o /out/hold.o "
         + ("/out/knobhook.o " if hooked else "")
         + "".join(f"/out/fx_{f['source']}.o " for f in fxs) + "".join(f"/out/drum_{e['source']}.o " for e in drums) + "; "
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
        for j, (table, sites) in enumerate(FX_FLAG_TABLES.items()):
            halves += [p for s in sites for p in repoint(s, table, sym[f"_fx_flags{j}"])]

    cmd = [sys.executable, os.path.join(ROOT, "tools", "ldr_patch.py"), a.stock, a.out]
    # Runtime RAM that is not loaded but must not be overlapped by code (ldr_patch.py checks it).
    cmd += ["--reserve", "0x00800000-0x00800020=prof_stats", "--reserve", "0x00810000-0x00818000=voice_ram"]
    for f in fxs:
        if "ram" in f:
            if "ram_size" not in f:
                sys.exit(f"fx {f['name']}: \"ram\" needs \"ram_size\"")
            lo = int(f["ram"], 16)
            cmd += ["--reserve", f"0x{lo:08x}-0x{lo + int(f['ram_size'], 16):08x}={f['source']}_ram"]
    for e in engines[:nsyn]:
        if "ram" in e:
            lo = int(e["ram"], 16)
            cmd += ["--reserve", f"0x{lo:08x}-0x{lo + int(e['ram_size'], 16):08x}={e['source']}_ram",
                    "--fill", f"0x{lo:08x}-0x{lo + 0x100:08x}=0"]
        cmd += ["--block", f"{e['addr']}={os.path.join(a.build, e['name'], 'engine.bin')}"]
    cmd += ["--block", f"0x{a.slot_addr:08x}={os.path.join(a.build, 'slot.bin')}"]
    cmd += ["--pointer", f"0x{ITER_PAGE_UPDATE_SLOT:08x}=0x{ITER_PAGE_UPDATE:08x}:0x{sym['_ui_update']:08x}"]
    if fxs:
        cmd += ["--pointer", f"0x{FILTER_PAGE_UPDATE_SLOT:08x}=0x{FILTER_PAGE_UPDATE:08x}:0x{sym['_fx_ui_update']:08x}"]
    if any(f["source"] == "repeat" for f in fxs):         # the manual key: every controller's press / release
        for slot_addr, stock, ours in REPEAT_KEY_SLOTS:
            cmd += ["--pointer", f"0x{slot_addr:08x}=0x{stock:08x}:0x{sym[ours]:08x}"]
        if not a.chord:                                   # the synth's: CHORD v2's wrappers (TE's path with it off)
            cmd += ["--pointer", f"0x{SYNTH_PRESS_SLOT:08x}=0x{TE_SYNTH_PRESS:08x}:0x{sym['_c2_press_entry']:08x}",
                    "--pointer", f"0x{SYNTH_RELEASE_SLOT:08x}=0x{TE_SYNTH_RELEASE:08x}:0x{sym['_c2_release_entry']:08x}"]
    calls = []                                                    # (pc, stock target, our symbol)
    if a.hold:
        cmd += ["--pointer", f"0x{SYNTH_SHIFTKEY_SLOT:08x}=0x{TE_SYNTH_SHIFTKEY:08x}:0x{sym['_hold_shiftkey_entry']:08x}"]
        calls.append((HOLD_KEYUP_CALL, TE_NOTE_OFF, "_hold_keyup_entry"))
        if a.chord:
            calls.append((CHORD_KEYDOWN_CALL, TE_NOTE_ON, "_chord_keydown_entry"))
            # CHORD v2: every hook passes through to TE's function when the mask leaves chord2 out
            for slot_addr, stock, ours in ((SYNTH_PRESS_SLOT, TE_SYNTH_PRESS, "_c2_press_entry"),
                                           (SYNTH_RELEASE_SLOT, TE_SYNTH_RELEASE, "_c2_release_entry"),
                                           (SYNTH_KNOB_SLOT, TE_SYNTH_KNOB, "_c2_knob_entry")):
                cmd += ["--pointer", f"0x{slot_addr:08x}=0x{stock:08x}:0x{sym[ours]:08x}"]
            calls.append((FRAME_RENDER_CALL, TE_FRAME_RENDER, "_c2_frame_hook"))
            calls.append((SEQ_STEP_CALL, TE_SEQ_STEP, "_c2_seq_hook"))
    if hooked:
        calls.append((KNOB_APPLY_CALL, TE_KNOB_APPLY, "_knob_hook"))
    if drums:
        calls.append((VALIDATE_LOOKUP_CALL, TE_ENGINE_LOOKUP, "_drum_validate_lookup"))
        calls.append((TYPE_EQ_LOAD_CALL, TE_JSON_EQ, "_drum_type_eq_load"))
        calls.append((TYPE_EQ_SAVE_CALL, TE_JSON_EQ, "_drum_type_eq_save"))
        cmd += ["--pointer", f"0x{DBOX_PAGE_UPDATE_SLOT:08x}=0x{DBOX_PAGE_UPDATE:08x}:0x{sym['_drum_ui_update']:08x}"]
        # the factory's assert path: `R0.L = 0x5A40` -> JUMP.L _drum_factory (which redoes it for other ids)
        new = branch_halves(DRUM_FACTORY_ASSERT, sym["_drum_factory"], 0xE200)
        halves += [(DRUM_FACTORY_ASSERT, 0xE100, new[0]), (DRUM_FACTORY_ASSERT + 2, 0x5A40, new[1])]
        # drum ScreenId: `R1 = 9; CC = R0 == R1` -> `R1 = last + 1; CC = R0 < R1 (IU)`: 9..last -> dbox's screen
        halves += [(DRUM_SCREEN_R1, 0x6049, 0x6001 | (count) << 3), (DRUM_SCREEN_CC, 0x0808, 0x0988)]
        # drum encoder: `R0 = 9; CC = R6 == R0` -> `R0 = 8; CC = R0 < R6 (IU)`: ids > 8 take dbox's path (row 9)
        halves += [(DRUM_KNOB_R0, 0x6048, 0x6040), (DRUM_KNOB_CC, 0x0806, 0x09B0)]
        for addr, old, _ in halves[-6:]:
            if half(addr) != old:
                sys.exit(f"0x{addr:08x}: expected 0x{old:04x}, found 0x{half(addr):04x}")
    for pc, stock, ours in calls:
        old = branch_halves(pc, stock, 0xE300)
        new = branch_halves(pc, sym[ours], 0xE300)
        if (half(pc), half(pc + 2)) != old:
            sys.exit(f"0x{pc:08x} is not the expected CALL 0x{stock:08x}")
        cmd += ["--half", f"0x{pc:08x}=0x{old[0]:04x}:0x{new[0]:04x}",
                "--half", f"0x{pc + 2:08x}=0x{old[1]:04x}:0x{new[1]:04x}"]
    cmd += [x for addr, old, new in halves for x in ("--half", f"0x{addr:08x}=0x{old:04x}:0x{new:04x}")]
    subprocess.run(cmd, check=True)
    # Where the HOLD/CHORD feature mask lives (slot.S _feature_mask, hold_impl.h FEAT_*), for tools/mkpatch.py
    if sym.get("_feature_magic") != sym["_feature_mask"] - 8:
        sys.exit("slot.S: _feature_magic must sit right before _feature_mask")
    json.dump({"mask_addr": sym["_feature_mask"], "mask_bits": FEATURE_BITS,
               "hooks": [h for h, on in (("hold", a.hold), ("chord", a.chord), ("chord2", a.chord)) if on]},
              open(os.path.join(a.build, "features.json"), "w"), indent=1)
    print(f"{len(halves)} halfword patches; slot symbols:",
          ", ".join(f"{k} 0x{v:08x}" for k, v in sorted(sym.items(), key=lambda kv: kv[1]) if v >= a.slot_addr))

    if a.db_in and a.db_out:
        src = a.db_in
        for i, e in enumerate(engines[:nsyn]):
            dst = a.db_out if i == nsyn - 1 else os.path.join(a.build, f"db_{i}.db")
            cmd = [sys.executable, os.path.join(ROOT, "tools", "add_engine_db.py"), src, dst,
                   "--type", e["name"], "--default", json.dumps(e["default"])]
            cmd += [x for name, k in e["presets"].items() for x in ("--preset", f"{name}={','.join(map(str, k))}")]
            cmd += ["--patch", json.dumps(e["patch"])] if "patch" in e else []
            cmd += ["--ranges", json.dumps(e["ranges"])] if "ranges" in e else []
            cmd += [x for name, pp in e.get("preset_patch", {}).items() for x in ("--preset-patch", f"{name}={json.dumps(pp)}")]
            subprocess.run(cmd, check=True)
            src = dst
        if not nsyn:
            shutil.copyfile(src, a.db_out)
        con = sqlite3.connect(a.db_out)
        # TE validates factory presets at boot: every per-key value stays inside the stock dbox presets' column range
        dbox = {json.loads(p)["name"]: (rid, json.loads(p))
                for rid, p in con.execute("SELECT id, patch FROM drum_presets WHERE folder='dbox'")}
        stock_rows = [r for _, p in dbox.values() for r in p.get("dbox_data", [])]
        lo, hi = [min(r[k] for r in stock_rows) for k in range(8)], [max(r[k] for r in stock_rows) for k in range(8)]

        def dbox_data(what, rows):
            if len(rows) != 24 or any(len(r) != 8 or any(not lo[k] <= r[k] <= hi[k] for k in range(8)) for r in rows):
                sys.exit(f"{what}: dbox_data must be 24 x 8 inside the stock dbox presets' {lo}..{hi}")
            return [list(r) for r in rows]
        for e in drums:
            did = drum_ids[e["name"]]
            if "replace" in e:          # replace a dbox preset: keeps the drum preset count
                if e["replace"] not in dbox:
                    sys.exit(f"drum engine {e['name']}: no dbox preset named {e['replace']!r} to replace")
                rid, old = dbox[e["replace"]]
                patch = dict(old, type=e["name"], name=e["preset"])
                if "dbox_default" in e:
                    patch["dbox_data"] = dbox_data(f"drum engine {e['name']}", [e["dbox_default"]] * 24)
                con.execute("UPDATE drum_presets SET patch=? WHERE id=?", (json.dumps(patch, separators=(",", ":")), rid))
                print(f"drum engine {did} {e['name']}: drum preset {rid} {e['replace']!r} -> {e['preset']!r}")
            if "kits" in e:             # one preset per kit in the engine's own folder (= browser column), fields from a stock template
                folder = e["name"]
                if len(folder) > MAX_FOLDER_LABEL or folder in ("drum", "dbox", "user", "thru"):
                    sys.exit(f"drum engine {folder!r}: kit folder must be a new name of at most {MAX_FOLDER_LABEL} characters "
                             "(the browser shows it as the column label; drum/dbox/user are TE's columns)")
                kits = json.load(open(os.path.join(ROOT, e["kits"])))
                for name, kit in kits.items():
                    if kit["template"] not in dbox or any(name == json.loads(p)["name"] for (p,) in
                                                          con.execute("SELECT patch FROM drum_presets")):
                        sys.exit(f"drum kit {name!r}: unknown template {kit['template']!r} or name taken")
                    patch = dict(dbox[kit["template"]][1], type=e["name"], name=name,
                                 dbox_data=dbox_data(f"drum kit {name!r}", kit["dbox_data"]))
                    con.execute("INSERT INTO drum_presets (patch, folder) VALUES (?, ?)",
                                (json.dumps(patch, separators=(",", ":")), folder))
                print(f"drum engine {did} {e['name']}: kits {', '.join(kits)} (folder {folder!r})")
        drum_presets = con.execute("SELECT count(*) FROM drum_presets").fetchone()[0]
        if drum_presets > MAX_FACTORY_DRUM_PRESETS:
            sys.exit(f"{drum_presets} drum presets: more than {MAX_FACTORY_DRUM_PRESETS} would leave users fewer than "
                     f"{MAX_DRUM_PATCHES - MAX_FACTORY_DRUM_PRESETS} saved drum sounds before patch.cpp:656 asserts at boot")
        print(f"drum presets: {drum_presets} (room for {MAX_DRUM_PATCHES - drum_presets} saved drum sounds; "
              f"patch.cpp:656 asserts at boot above {MAX_DRUM_PATCHES} in all)")
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
