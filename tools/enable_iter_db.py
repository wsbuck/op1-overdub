#!/usr/bin/env python3
"""Copy op1_factory.db and add the hidden `iter` synth type (same row op1repacker inserts).

  enable_iter_db.py STOCK_op1_factory.db OUT.db [--presets DIR]

--presets DIR adds every *.aif patch in DIR to synth_presets with folder 'iter' (op1repacker's
`presets-iter`); --presets FILE.json adds the patches listed in it (src/slot/iter_presets.json, the same 11 presets). On 246 the engine browser appears to list only engines that have a preset folder.
"""
import argparse
import json
import os
import shutil
import sqlite3
import struct
import sys

ITER_ROW = (11, "iter", "[1516, 16704, 0, 15168, 0, 0, 0, 0]")


def read_patch(path):
    """Return the OP-1 patch JSON stored in an AIFF 'APPL' chunk ("op-1" + JSON)."""
    data = open(path, "rb").read()
    if data[:4] != b"FORM":
        sys.exit(f"{path}: not an AIFF file")
    off = 12
    while off + 8 <= len(data):
        cid, size = data[off:off + 4], struct.unpack(">I", data[off + 4:off + 8])[0]
        body = data[off + 8:off + 8 + size]
        if cid == b"APPL" and body[:4] == b"op-1":
            return json.loads(body[4:].rstrip(b"\0 \n"))
        off += 8 + size + (size & 1)
    sys.exit(f"{path}: no op-1 APPL chunk")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--presets", metavar="DIR|FILE.json")
    a = ap.parse_args()
    src, out = a.src, a.out
    shutil.copyfile(src, out)
    con = sqlite3.connect(out)
    if con.execute("SELECT count(*) FROM synth_types WHERE type='iter' OR id=?", (ITER_ROW[0],)).fetchone()[0]:
        sys.exit("iter (or id 11) already present")
    con.execute("INSERT INTO synth_types VALUES (?,?,?)", ITER_ROW)
    if a.presets and a.presets.endswith(".json"):
        patches = json.load(open(a.presets))["presets"]
    elif a.presets:
        patches = []
        for name in sorted(os.listdir(a.presets)):
            if name.lower().endswith(".aif"):
                patches.append(dict(read_patch(os.path.join(a.presets, name)), name=os.path.splitext(name)[0]))
    for patch in patches if a.presets else []:
        if patch.get("type") != "iter":
            sys.exit(f"{patch.get('name')}: patch type is {patch.get('type')!r}, not 'iter'")
        con.execute("INSERT INTO synth_presets (patch, folder) VALUES (?, ?)", (json.dumps(patch, separators=(",", ":")), "iter"))
    con.commit()
    con.execute("VACUUM")
    con.close()
    rows = sqlite3.connect(out).execute("SELECT id, type FROM synth_types ORDER BY id").fetchall()
    print("synth_types:", rows)
    print("iter presets:", sqlite3.connect(out).execute(
        "SELECT count(*) FROM synth_presets WHERE folder='iter'").fetchone()[0])


if __name__ == "__main__":
    main()
