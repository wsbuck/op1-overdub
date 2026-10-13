#!/usr/bin/env python3
"""Copy an op1_factory.db and add a synth type with presets (for an engine added with build_slot.py).

  add_engine_db.py IN.db OUT.db --type va --default "[0, 6000, 20000, 9000, 0, 0, 0, 0]"
                   --preset NAME=K0,K1,K2,K3 [--preset ...] [--template-folder iter]

Each preset copies the envelope/FX/LFO settings of one preset in --template-folder (in order, cycling)
and sets type, name and the 4 engine knobs. On 246 the engine browser lists only types with presets.
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
    ap.add_argument("--preset", action="append", default=[], metavar="NAME=K0,K1,K2,K3")
    ap.add_argument("--template-folder", default="iter")
    a = ap.parse_args()
    if len(json.loads(a.default)) != 8:
        sys.exit("--default must be a JSON list of 8 knob values")
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
    for i, spec in enumerate(a.preset):
        name, _, knobs = spec.partition("=")
        k = [int(v) for v in knobs.split(",")]
        if len(k) != 4 or not all(0 <= v <= 0x7FFF for v in k):
            sys.exit(f"{spec}: need 4 knob values 0..32767")
        patch = dict(templates[i % len(templates)], type=a.type, name=name, knobs=k + [0, 0, 0, 0])
        con.execute("INSERT INTO synth_presets (patch, folder) VALUES (?, ?)",
                    (json.dumps(patch, separators=(",", ":")), a.type))
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
