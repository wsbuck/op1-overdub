#!/usr/bin/env python3
"""Apply an op-1-mods patch to your own stock OP-1 firmware 246 image.

  python3 apply.py op1_246.op1 [OUT.OP1] [--patch DIR]

Download stock 246 from teenage.engineering (https://teenage.engineering/_software/op-1/op1_246.op1).
The patch (DIR, default: the `patch` folder next to this script) holds only our own code, our database rows and
our SVG screens; TE's firmware is never part of it. Every step is checked: the stock image's sha256, the patched
OP1_vdk.ldr's sha256, the patched database's contents, and te-boot.ldr byte-identical to stock.

Patch format (written by tools/mkpatch.py), patch.json:
  stock_sha256                     the stock .op1 it applies to
  ldr: stock_sha256, out_sha256    OP1_vdk.ldr before and after
       blocks [{flags, target, count, argument, offset, length}]
                                   LDR blocks inserted before the FINAL block, payloads in blocks.bin
  db:  stock_sha256, delete [[table, rowid]], insert [[table, rowid, [values]]], out_digest
                                   row changes to content/op1_factory.db; out_digest = sha256 of its rows
  add: {tar member name: file in DIR}
"""
import argparse
import base64
import hashlib
import json
import os
import shutil
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ldr  # noqa: E402
import op1pack  # noqa: E402

LDR_MEMBER, DB_MEMBER = "OP1_vdk.ldr", "content/op1_factory.db"


def sha(b):
    return hashlib.sha256(b).hexdigest()


def enc(v):
    return {"b64": base64.b64encode(v).decode()} if isinstance(v, bytes) else v


def dec(v):
    return base64.b64decode(v["b64"]) if isinstance(v, dict) else v


def db_rows(path):
    """{table: {rowid: [values]}} and the schema of an SQLite file."""
    con = sqlite3.connect(path)
    schema = sorted(con.execute("SELECT type, name, sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'"))
    tables = [n for t, n, _ in schema if t == "table"]
    rows = {t: {r[0]: list(r[1:]) for r in con.execute(f'SELECT rowid, * FROM "{t}"')} for t in tables}
    con.close()
    return rows, schema


def db_digest(path):
    rows, schema = db_rows(path)
    canon = {"schema": schema, "rows": {t: sorted([k, [enc(x) for x in v]] for k, v in r.items()) for t, r in rows.items()}}
    return sha(json.dumps(canon, sort_keys=True).encode())


def apply_ldr(stock, p, blob):
    if sha(stock) != p["stock_sha256"]:
        sys.exit("OP1_vdk.ldr in this image is not stock 246")
    blocks = ldr.parse(stock)
    final = next(i for i, b in enumerate(blocks) if b.flags & ldr.F_FINAL)
    new = [ldr.Block(0, 0, b["flags"], b["target"], b["count"], b["argument"], blob[b["offset"]:b["offset"] + b["length"]])
           for b in p["blocks"]]
    blocks[0].argument += sum(16 + len(b.payload) for b in new)
    out = ldr.serialize(blocks[:final] + new + blocks[final:])
    if sha(out) != p["out_sha256"]:
        sys.exit("patched OP1_vdk.ldr does not match the expected result")
    return out


def apply_db(stock_path, p, out_path):
    if sha(open(stock_path, "rb").read()) != p["stock_sha256"]:
        sys.exit("op1_factory.db in this image is not stock 246")
    shutil.copyfile(stock_path, out_path)
    con = sqlite3.connect(out_path)
    for table, rowid in p["delete"]:
        con.execute(f'DELETE FROM "{table}" WHERE rowid = ?', (rowid,))
    for table, rowid, values in p["insert"]:
        cols = [r[1] for r in con.execute(f'PRAGMA table_info("{table}")')]
        con.execute(f'INSERT INTO "{table}" (rowid, {", ".join(f"[{c}]" for c in cols)}) VALUES ({", ".join("?" * (len(cols) + 1))})',
                    [rowid] + [dec(v) for v in values])
    con.commit()
    con.execute("VACUUM")
    con.close()
    if db_digest(out_path) != p["out_digest"]:
        sys.exit("patched op1_factory.db does not match the expected result")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stock", help="stock op1_246.op1")
    ap.add_argument("out", nargs="?", default="MODS.OP1")
    ap.add_argument("--patch", default=os.path.join(HERE, "patch"))
    a = ap.parse_args()
    name = os.path.basename(a.out)
    if any(w in name.upper() for w in op1pack.FORBIDDEN_WORDS) or name[:1] in (".", "_"):
        sys.exit(f"refusing output name {name!r}: the OP-1's updater treats names containing FRC, RBT or KEY, "
                 "or starting with '.' or '_', as commands")

    p = json.load(open(os.path.join(a.patch, "patch.json")))
    if sha(open(a.stock, "rb").read()) != p["stock_sha256"]:
        sys.exit(f"{a.stock} is not stock OP-1 firmware 246 (sha256 {p['stock_sha256'][:16]}…)")
    members = {n: c for n, _, c in op1pack.members(op1pack.read_image(a.stock))[0]}

    with tempfile.TemporaryDirectory() as tmp:
        ldr_out = os.path.join(tmp, "OP1_vdk.ldr")
        open(ldr_out, "wb").write(apply_ldr(members[LDR_MEMBER], p["ldr"], open(os.path.join(a.patch, "blocks.bin"), "rb").read()))
        db_in, db_out = os.path.join(tmp, "stock.db"), os.path.join(tmp, "op1_factory.db")
        open(db_in, "wb").write(members[DB_MEMBER])
        apply_db(db_in, p["db"], db_out)
        op1pack.cmd_build(argparse.Namespace(
            stock=a.stock, out=a.out,
            replace=[f"{LDR_MEMBER}={ldr_out}", f"{DB_MEMBER}={db_out}"],
            add=[f"{n}={os.path.join(a.patch, f)}" for n, f in p["add"].items()]))
    print(f"\n{p.get('name', 'patch')} applied. Copy {a.out} to the OP-1 in update mode (hold COM at power-on, then 1).")


if __name__ == "__main__":
    main()
