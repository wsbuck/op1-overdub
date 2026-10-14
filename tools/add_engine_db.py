#!/usr/bin/env python3
"""Copy an op1_factory.db and add a synth type with presets (for an engine added with build_slot.py).

  add_engine_db.py IN.db OUT.db --type va --default "[0, 6000, 20000, 9000, 0, 0, 0, 0]"
                   --preset NAME=K0,K1,K2,K3 [--preset ...] [--template-folder iter] [--patch JSON]
                   [--preset-patch NAME=JSON ...] [--ranges JSON]

Each preset copies the envelope/FX/LFO settings of one preset in --template-folder (in order, cycling)
and sets type, name and the 4 engine knobs; --patch sets more fields in every preset (e.g. '{"lfo_active": false}'),
--preset-patch in one preset (after --patch). A preset's fx_params must equal a stock preset's for its fx_type.
With --ranges (the engine's 8 x [min, max, scale, type] range row, build_slot.py), a preset may give all 8 knob values,
signed, each within its row (packed engines: notes/knob-hook.md); --default is checked against it too. Without it,
presets are 4 values 0..32767 (knobs 4..7 = 0), as before.
On 246 the engine browser lists only types with presets.
"""
import argparse
import json
import shutil
import sqlite3
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--type", required=True)
    ap.add_argument("--default", required=True)
    ap.add_argument("--preset", action="append", default=[], metavar="NAME=K0,K1,K2,K3[,K4..K7]")
    ap.add_argument("--ranges", help="JSON: the engine's range row, 8 x [min, max, scale, type]; allows 8-value presets")
    ap.add_argument("--template-folder", default="iter")
    ap.add_argument("--patch", default="{}", help="JSON object of patch fields set in every preset")
    ap.add_argument("--preset-patch", action="append", default=[], metavar="NAME=JSON",
                    help="JSON object of patch fields set in one preset (after --patch)")
    a = ap.parse_args()
    if len(json.loads(a.default)) != 8:
        sys.exit("--default must be a JSON list of 8 knob values")
    rows = json.loads(a.ranges) if a.ranges else None
    if rows is not None and (len(rows) != 8 or any(len(r) != 4 or not -32768 <= r[0] < r[1] <= 32767 for r in rows)):
        sys.exit("--ranges must be 8 x [min, max, scale, type] with -32768 <= min < max <= 32767")
    if rows and any(not r[0] <= v <= r[1] for v, r in zip(json.loads(a.default), rows)):
        sys.exit(f"--default {a.default} outside the range row (TE's validator: error 15)")
    shutil.copyfile(a.src, a.out)
    con = sqlite3.connect(a.out)
    if con.execute("SELECT count(*) FROM synth_types WHERE type=?", (a.type,)).fetchone()[0]:
        sys.exit(f"{a.type} already present")
    new_id = con.execute("SELECT max(id) + 1 FROM synth_types").fetchone()[0]
    con.execute("INSERT INTO synth_types VALUES (?,?,?)", (new_id, a.type, a.default))
    templates = [json.loads(r[0]) for r in con.execute(
        "SELECT patch FROM synth_presets WHERE folder=? ORDER BY id", (a.template_folder,))]
    if a.preset and not templates:
        sys.exit(f"no presets in folder {a.template_folder!r} to use as templates")
    # TE validates every field when a patch loads (patch.cpp:390) and stops at boot on a bad one: attack 0 gave
    # "Invalid synth patch inprobe:input - error 21" (AMPPROBE). Keep --patch's lists within the stock presets' range.
    extra = json.loads(a.patch)
    stock = [json.loads(r[0]) for r in con.execute("SELECT patch FROM synth_presets")]

    def guard(what, fields):
        for field, vals in fields.items():
            if isinstance(vals, list):
                seen = [p[field] for p in stock if isinstance(p.get(field), list) and len(p[field]) == len(vals)]
                for j, v in enumerate(vals):
                    lo, hi = min(s[j] for s in seen), max(s[j] for s in seen)
                    if not lo <= v <= hi:
                        sys.exit(f"{what} {field}[{j}] = {v} is outside the stock presets' {lo}..{hi}")

    guard("--patch", extra)
    per = {}
    for spec in a.preset_patch:
        name, _, js = spec.partition("=")
        per[name] = json.loads(js)
        guard(f"--preset-patch {name}:", per[name])
    for i, spec in enumerate(a.preset):
        name, _, knobs = spec.partition("=")
        k = [int(v) for v in knobs.split(",")]
        if rows and len(k) == 8:            # TE validates every knob against the row (patch.cpp, error 15)
            bad = [i for i, (v, r) in enumerate(zip(k, rows)) if not r[0] <= v <= r[1]]
            if bad:
                sys.exit(f"{spec}: knob {bad[0]} = {k[bad[0]]} outside its range row {rows[bad[0]][:2]}")
        elif len(k) != 4 or not all(0 <= v <= 0x7FFF for v in k):
            sys.exit(f"{spec}: need 4 knob values 0..32767" + (" or 8 within the range row" if rows else ""))
        elif rows and any(not r[0] <= v <= r[1] for v, r in zip(k + [0] * 4, rows)):
            sys.exit(f"{spec}: outside the range row")
        patch = dict(templates[i % len(templates)], **extra)
        patch.update(per.pop(name, {}))
        patch.update(type=a.type, name=name, knobs=k + [0] * (8 - len(k)))
        if patch.get("fx_active") and not any(p.get("fx_type") == patch["fx_type"] and p.get("fx_params") == patch["fx_params"]
                                              for p in stock):
            sys.exit(f"{name}: fx_params {patch['fx_params']} are not a stock {patch['fx_type']} preset's")
        con.execute("INSERT INTO synth_presets (patch, folder) VALUES (?, ?)",
                    (json.dumps(patch, separators=(",", ":")), a.type))
    if per:
        sys.exit(f"--preset-patch for unknown presets: {', '.join(per)}")
    con.commit()
    con.execute("VACUUM")
    con.close()
    con = sqlite3.connect(a.out)
    print("synth_types:", con.execute("SELECT id, type FROM synth_types ORDER BY id").fetchall())
    print(f"{a.type} presets:", [json.loads(r[0])["name"] for r in con.execute(
        "SELECT patch FROM synth_presets WHERE folder=?", (a.type,))])
    print("integrity:", con.execute("PRAGMA integrity_check").fetchone()[0])


if __name__ == "__main__":
    main()
