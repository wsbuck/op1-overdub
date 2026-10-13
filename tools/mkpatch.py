#!/usr/bin/env python3
"""Make a shareable patch from a build: the difference between stock 246 and our image, without TE's firmware.

  mkpatch.py STOCK.op1 BUILT.OP1 OUTDIR [--name NAME]

Writes OUTDIR/patch.json, blocks.bin and the added SVGs (format: tools/apply.py), then checks that apply.py
rebuilds an image with the same OP1_vdk.ldr, database rows and added files as BUILT.OP1.
Refuses unless the build only inserts blocks before FINAL (the way tools/ldr_patch.py patches), only changes
op1_factory.db rows, and only adds new members otherwise.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import apply  # noqa: E402
import ldr  # noqa: E402
import op1pack  # noqa: E402


def ldr_diff(stock, out):
    sb, ob = ldr.parse(stock), ldr.parse(out)
    final = next(i for i, b in enumerate(sb) if b.flags & ldr.F_FINAL)
    n = len(ob) - len(sb)
    same = [b.serialize() for b in sb[1:]] == [b.serialize() for b in ob[1:final] + ob[final + n:]]
    if n <= 0 or not same:
        sys.exit("built OP1_vdk.ldr is not stock plus blocks inserted before FINAL")
    blocks, blob = [], b""
    for b in ob[final:final + n]:
        if b.is_encrypted:
            sys.exit("refusing to export an encrypted block")
        blocks.append({"flags": b.flags, "target": b.target, "count": b.count, "argument": b.argument,
                       "offset": len(blob), "length": len(b.payload)})
        blob += b.payload
    return {"stock_sha256": apply.sha(stock), "out_sha256": apply.sha(out), "blocks": blocks}, blob


def db_diff(stock_path, out_path):
    (srows, sschema), (orows, oschema) = apply.db_rows(stock_path), apply.db_rows(out_path)
    if sschema != oschema:
        sys.exit("built op1_factory.db has a different schema from stock")
    delete, insert = [], []
    for t in srows:
        for rid, vals in srows[t].items():
            if orows[t].get(rid) != vals:
                delete.append([t, rid])
        for rid, vals in orows[t].items():
            if srows[t].get(rid) != vals:
                insert.append([t, rid, [apply.enc(v) for v in vals]])
    return {"stock_sha256": apply.sha(open(stock_path, "rb").read()), "delete": delete, "insert": insert,
            "out_digest": apply.db_digest(out_path)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stock")
    ap.add_argument("built")
    ap.add_argument("outdir")
    ap.add_argument("--name", default="op-1-mods")
    a = ap.parse_args()
    stock = {n: c for n, _, c in op1pack.members(op1pack.read_image(a.stock))[0]}
    built = {n: c for n, _, c in op1pack.members(op1pack.read_image(a.built))[0]}
    changed = sorted(n for n in stock if built.get(n) != stock[n])
    if set(changed) - {apply.LDR_MEMBER, apply.DB_MEMBER} or set(stock) - set(built):
        sys.exit(f"build changes members a patch cannot carry: {changed}")
    added = sorted(set(built) - set(stock))

    if os.path.exists(a.outdir):
        shutil.rmtree(a.outdir)
    os.makedirs(a.outdir)
    p = {"format": 1, "name": a.name, "stock_sha256": apply.sha(open(a.stock, "rb").read()), "add": {}}
    p["ldr"], blob = ldr_diff(stock[apply.LDR_MEMBER], built[apply.LDR_MEMBER])
    open(os.path.join(a.outdir, "blocks.bin"), "wb").write(blob)
    with tempfile.TemporaryDirectory() as tmp:
        s, o = os.path.join(tmp, "s.db"), os.path.join(tmp, "o.db")
        open(s, "wb").write(stock[apply.DB_MEMBER])
        open(o, "wb").write(built[apply.DB_MEMBER])
        p["db"] = db_diff(s, o)
        for name in added:
            f = os.path.basename(name)
            if f in p["add"].values():
                sys.exit(f"two added members named {f}")
            open(os.path.join(a.outdir, f), "wb").write(built[name])
            p["add"][name] = f
        json.dump(p, open(os.path.join(a.outdir, "patch.json"), "w"), indent=1)

        # Round trip: apply the patch to stock and compare with the build.
        img = os.path.join(tmp, "CHECK.OP1")
        subprocess.run([sys.executable, os.path.join(HERE, "apply.py"), a.stock, img, "--patch", a.outdir],
                       check=True, capture_output=True)
        got = {n: c for n, _, c in op1pack.members(op1pack.read_image(img))[0]}
        open(o, "wb").write(got[apply.DB_MEMBER])
        if set(got) != set(built) or any(got[n] != built[n] for n in built if n != apply.DB_MEMBER) \
                or apply.db_digest(o) != p["db"]["out_digest"]:
            sys.exit("round trip failed: apply.py does not reproduce the build")
    print(f"wrote {a.outdir}: {len(p['ldr']['blocks'])} LDR blocks ({len(blob)} B), "
          f"db -{len(p['db']['delete'])} +{len(p['db']['insert'])} rows, {len(added)} added files; round trip OK")


if __name__ == "__main__":
    main()
