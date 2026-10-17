#!/usr/bin/env python3
"""Install overdub: apply the patch to your own stock OP-1 firmware 246 image, with the features you choose.

  python3 apply.py op1_246.op1 [OUT.OP1]                    asks which features to install (all on by default)
  python3 apply.py op1_246.op1 --all                        everything, no questions
  python3 apply.py op1_246.op1 --without amp,crush          everything except these
  python3 apply.py op1_246.op1 --only va,grain,hold         only these
  python3 apply.py --list                                   the features in this patch

Download stock 246 from teenage.engineering (https://teenage.engineering/_software/op-1/op1_246.op1).
Without --all/--only/--without, apply.py shows a checklist when run in a terminal, and installs everything otherwise.

What leaving a feature out does: a synth or drum engine, or an FX, stays in the firmware (its name and id are kept, so
a sound saved with it still loads) but is not listed: its database rows (engine/FX list entry, presets, kits) are
not installed. HOLD and CHORD are turned off: their keys behave as on stock firmware. CHORD v2 (chord2) left out:
CHORD works as CHORD1 and its extra hooks pass straight to TE.

The patch (DIR, default: the `patch` folder next to this script) holds only our own code, our database rows and
our SVG screens; TE's firmware is never part of it. Every step is checked: the stock image's sha256, the patched
OP1_vdk.ldr's sha256 (with every feature, then with your selection), the patched database's rows against the rows
expected for your selection, and te-boot.ldr byte-identical to stock.

Patch format (written by tools/mkpatch.py), patch.json:
  stock_sha256                     the stock .op1 it applies to
  ldr: stock_sha256, out_sha256    OP1_vdk.ldr before and after (every feature on)
       blocks [{flags, target, count, argument, offset, length}]
                                   LDR blocks inserted before the FINAL block, payloads in blocks.bin
       mask: {offset, magic, all, out_sha256: {mask: sha256}}   (format 2)
                                   the HOLD/CHORD feature word: blocks.bin offset, the 8 bytes before it, its value
                                   with every hook on, and the OP1_vdk.ldr sha256 for each mask value
  db:  stock_sha256, delete [[table, rowid]], insert [[table, rowid, [values]]], out_digest
                                   row changes to content/op1_factory.db; out_digest = sha256 of its rows (every
                                   feature on)
  add: {tar member name: file in DIR}
  features: [{id, kind, insert: [i], delete: [i]} or {id, kind: "key", bit}]   (format 2)
                                   what can be left out: an engine's or FX's indices into db.insert/db.delete, a
                                   key mode's bit in the mask
  always: [names]                  engines always installed (probes), shown by --list
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
KINDS = {"synth": "synth engine", "drum": "drum engine", "fx": "FX", "key": "key mode"}
ABOUT = {"hold": "SHIFT+T2 latches notes", "chord": "SHIFT + keys learn a chord",
         "chord2": "CHORD v2: chord maps and the CHORD page, SHIFT+SPLIT; needs chord; without it CHORD works as CHORD1"}


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


def rows_digest(rows, schema):
    schema = [list(s) for s in schema]
    canon = {"schema": schema, "rows": {t: sorted([k, [enc(x) for x in v]] for k, v in r.items()) for t, r in rows.items()}}
    return sha(json.dumps(canon, sort_keys=True).encode())


def db_digest(path):
    return rows_digest(*db_rows(path))


def assemble_ldr(stock, p, blob):
    blocks = ldr.parse(stock)
    final = next(i for i, b in enumerate(blocks) if b.flags & ldr.F_FINAL)
    new = [ldr.Block(0, 0, b["flags"], b["target"], b["count"], b["argument"], blob[b["offset"]:b["offset"] + b["length"]])
           for b in p["blocks"]]
    blocks[0].argument += sum(16 + len(b.payload) for b in new)
    return ldr.serialize(blocks[:final] + new + blocks[final:])


def set_mask(blob, m, mask):
    """blocks.bin with the feature word set to mask, after checking the word's magic and its built value."""
    off = m["offset"]
    if blob[off - 8:off] != m["magic"].encode() or int.from_bytes(blob[off:off + 4], "little") != m["all"] \
            or mask & ~m["all"]:
        sys.exit("the patch's feature word is not where patch.json says")
    return blob[:off] + mask.to_bytes(4, "little") + blob[off + 4:]


def apply_ldr(stock, p, blob, mask=None):
    if sha(stock) != p["stock_sha256"]:
        sys.exit("OP1_vdk.ldr in this image is not stock 246")
    out = assemble_ldr(stock, p, blob)
    if sha(out) != p["out_sha256"]:                     # every feature on: the build as tested
        sys.exit("patched OP1_vdk.ldr does not match the expected result")
    m = p.get("mask")
    if mask is None or (m and mask == m["all"]):
        return out
    out = assemble_ldr(stock, p, set_mask(blob, m, mask))
    if sha(out) != m["out_sha256"].get(str(mask)):
        sys.exit(f"patched OP1_vdk.ldr (feature mask {mask}) does not match the expected result")
    return out


def apply_db(stock_path, p, out_path, skip=frozenset()):
    """Apply the row changes except the db.insert/db.delete indices in skip (("insert", i) / ("delete", i)); check
    the result against the rows expected from stock plus exactly those changes."""
    if sha(open(stock_path, "rb").read()) != p["stock_sha256"]:
        sys.exit("op1_factory.db in this image is not stock 246")
    def expected(delete, insert):
        rows, schema = db_rows(stock_path)
        for table, rowid in delete:
            del rows[table][rowid]
        for table, rowid, values in insert:
            rows[table][rowid] = [dec(v) for v in values]
        return rows_digest(rows, schema)
    if expected(p["delete"], p["insert"]) != p["out_digest"]:     # every feature: the build as tested
        sys.exit("patch.json's database rows do not match its digest")
    delete = [d for i, d in enumerate(p["delete"]) if ("delete", i) not in skip]
    insert = [r for i, r in enumerate(p["insert"]) if ("insert", i) not in skip]
    want = expected(delete, insert)
    shutil.copyfile(stock_path, out_path)
    con = sqlite3.connect(out_path)
    for table, rowid in delete:
        con.execute(f'DELETE FROM "{table}" WHERE rowid = ?', (rowid,))
    for table, rowid, values in insert:
        cols = [r[1] for r in con.execute(f'PRAGMA table_info("{table}")')]
        con.execute(f'INSERT INTO "{table}" (rowid, {", ".join(f"[{c}]" for c in cols)}) VALUES ({", ".join("?" * (len(cols) + 1))})',
                    [rowid] + [dec(v) for v in values])
    con.commit()
    con.execute("VACUUM")
    ok = con.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    con.close()
    if not ok or db_digest(out_path) != want:
        sys.exit("patched op1_factory.db does not match the expected result")


def describe(f):
    return f"{f['id']:<12} {KINDS.get(f['kind'], f['kind'])}" + (f" ({ABOUT[f['id']]})" if f["id"] in ABOUT else "")


def checklist(features):
    """Interactive selection: everything on; type numbers to toggle."""
    on = [True] * len(features)
    while True:
        print("\nFeatures to install (all on by default):")
        for i, f in enumerate(features):
            print(f"  {i + 1:2d}. [{'x' if on[i] else ' '}] {describe(f)}")
        try:
            s = input("Numbers to toggle (e.g. 3 7), a = all, n = none, Enter = install, q = quit: ").strip().lower()
        except EOFError:
            s = "q"
        if not s:
            return {f["id"] for f, o in zip(features, on) if o}
        if s == "q":
            sys.exit("nothing written")
        if s in ("a", "n"):
            on = [s == "a"] * len(features)
            continue
        for t in s.replace(",", " ").split():
            if t.isdigit() and 1 <= int(t) <= len(features):
                on[int(t) - 1] = not on[int(t) - 1]
            else:
                print(f"  ignored {t!r}")


def names(arg, features):
    want = [n.strip() for n in arg.split(",") if n.strip()]
    unknown = [n for n in want if n not in {f["id"] for f in features}]
    if unknown:
        sys.exit(f"unknown feature(s): {', '.join(unknown)}. This patch has: {', '.join(f['id'] for f in features)}")
    return set(want)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stock", nargs="?", help="stock op1_246.op1")
    ap.add_argument("out", nargs="?", default="OVERDUB.OP1")
    ap.add_argument("--patch", default=os.path.join(HERE, "patch"))
    ap.add_argument("--list", action="store_true", help="list the features in this patch and exit")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--all", action="store_true", help="install everything without asking")
    g.add_argument("--only", metavar="A,B", help="install only these features")
    g.add_argument("--without", metavar="A,B", help="install everything except these features")
    a = ap.parse_args()

    p = json.load(open(os.path.join(a.patch, "patch.json")))
    features = p.get("features", [])
    if a.list:
        print(f"{p.get('name', 'patch')}\n\nFeatures (all installed by default; leave out with --without):")
        for f in features:
            print(f"  {describe(f)}")
        if p.get("always"):
            print(f"\nAlways installed (test probes): {', '.join(p['always'])}")
        return
    if not a.stock:
        ap.error("the stock image (op1_246.op1) is needed")
    name = os.path.basename(a.out)
    if any(w in name.upper() for w in op1pack.FORBIDDEN_WORDS) or name[:1] in (".", "_"):
        sys.exit(f"refusing output name {name!r}: the OP-1's updater treats names containing FRC, RBT or KEY, "
                 "or starting with '.' or '_', as commands")
    if sha(open(a.stock, "rb").read()) != p["stock_sha256"]:
        sys.exit(f"{a.stock} is not stock OP-1 firmware 246 (sha256 {p['stock_sha256'][:16]}…)")

    every = {f["id"] for f in features}
    if a.only is not None:
        chosen = names(a.only, features)
    elif a.without is not None:
        chosen = every - names(a.without, features)
    elif not a.all and features and sys.stdin.isatty() and sys.stdout.isatty():
        chosen = checklist(features)
    else:
        chosen = every
    hidden = [f for f in features if f["id"] not in chosen]
    skip = frozenset((what, i) for f in hidden for what in ("insert", "delete") for i in f.get(what, []))
    mask = None
    if any(f["kind"] == "key" for f in features):
        mask = p["ldr"]["mask"]["all"] & ~sum(f["bit"] for f in hidden if f["kind"] == "key")

    members = {n: c for n, _, c in op1pack.members(op1pack.read_image(a.stock))[0]}
    with tempfile.TemporaryDirectory() as tmp:
        ldr_out = os.path.join(tmp, "OP1_vdk.ldr")
        blob = open(os.path.join(a.patch, "blocks.bin"), "rb").read()
        open(ldr_out, "wb").write(apply_ldr(members[LDR_MEMBER], p["ldr"], blob, mask))
        db_in, db_out = os.path.join(tmp, "stock.db"), os.path.join(tmp, "op1_factory.db")
        open(db_in, "wb").write(members[DB_MEMBER])
        apply_db(db_in, p["db"], db_out, skip)
        op1pack.cmd_build(argparse.Namespace(
            stock=a.stock, out=a.out,
            replace=[f"{LDR_MEMBER}={ldr_out}", f"{DB_MEMBER}={db_out}"],
            add=[f"{n}={os.path.join(a.patch, f)}" for n, f in p["add"].items()]))
    if features:
        print(f"\ninstalled: {', '.join(f['id'] for f in features if f['id'] in chosen) or 'none'}")
        print(f"left out:  {', '.join(f['id'] for f in hidden) or 'none'}")
    print(f"\n{p.get('name', 'patch')} applied. Copy {a.out} to the OP-1 in update mode (hold COM at power-on, then 1).")


if __name__ == "__main__":
    main()
