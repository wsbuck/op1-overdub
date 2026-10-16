#!/usr/bin/env python3
"""Make a shareable patch from a build: the difference between stock 246 and our image, without TE's firmware.

  mkpatch.py STOCK.op1 BUILT.OP1 OUTDIR [--name NAME] [--features FEATURES.json]

Writes OUTDIR/patch.json, blocks.bin and the added SVGs (format: tools/apply.py), then checks that apply.py
rebuilds an image with the same OP1_vdk.ldr, database rows and added files as BUILT.OP1.
FEATURES.json (written by build.py) makes the build's features selectable at install time:
  {"engines": [{"name", "drum", "probe"}], "fx": [names], "slot": build_slot.py's features.json}
Each engine's and FX's database rows are found by content (synth_types / fx_types `type`, preset `folder` or patch
`type` = the name; the stock row a changed row replaced goes with it); engines marked "probe" are always installed.
The HOLD/CHORD mask word is located in blocks.bin (the magic before it is checked) and the OP1_vdk.ldr sha256 for
every mask value is recorded.
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


def owner(table, values, synth, drum, fx):
    """The feature a database row belongs to (an engine or FX name), or None (always installed)."""
    if table == "synth_types":
        return values[1] if values[1] in synth else None
    if table == "fx_types":
        return values[1] if values[1] in fx else None
    if table in ("synth_presets", "drum_presets"):
        names = synth if table == "synth_presets" else drum
        typ = json.loads(values[1]).get("type")
        hits = {n for n in (values[2], typ) if n in names}
        if len(hits) > 1:
            sys.exit(f"{table} row {values[0]} belongs to two features: {hits}")
        return hits.pop() if hits else None
    return None


def features(meta, p, blob):
    """patch.json's features, always and ldr.mask (format 2) from build.py's FEATURES.json."""
    eng = [e for e in meta["engines"] if not e.get("probe")]
    synth = {e["name"] for e in eng if not e.get("drum")}
    drum = {e["name"] for e in eng if e.get("drum")}
    fx = set(meta["fx"])
    out = [{"id": e["name"], "kind": "drum" if e.get("drum") else "synth", "insert": [], "delete": []} for e in eng]
    out += [{"id": f, "kind": "fx", "insert": [], "delete": []} for f in meta["fx"]]
    by_id = {f["id"]: f for f in out}
    db = p["db"]
    insert_owner = {}
    for i, (table, rowid, values) in enumerate(db["insert"]):
        o = owner(table, [apply.dec(v) for v in values], synth, drum, fx)
        if o:
            by_id[o]["insert"].append(i)
            insert_owner[table, rowid] = o
    for i, (table, rowid) in enumerate(db["delete"]):
        if (table, rowid) in insert_owner:      # a stock row this feature replaced: hidden, the stock row stays
            by_id[insert_owner[table, rowid]]["delete"].append(i)
    for f in out:
        if not f["insert"]:
            sys.exit(f"feature {f['id']}: no database rows found, so it could not be left out")
    slot = meta["slot"]
    bits = slot["mask_bits"]
    out += [{"id": h, "kind": "key", "bit": bits[h]} for h in slot["hooks"]]
    p["features"] = out
    p["always"] = [e["name"] for e in meta["engines"] if e.get("probe")]
    if not slot["hooks"]:
        return
    addr = slot["mask_addr"]
    b = next((b for b in p["ldr"]["blocks"] if b["target"] <= addr and addr + 4 <= b["target"] + b["length"]), None)
    if b is None or b["length"] != b["count"]:
        sys.exit(f"feature mask 0x{addr:08x} is not in a plain inserted block")
    off = b["offset"] + addr - b["target"]
    all_on = sum(bits.values())
    m = {"offset": off, "magic": "OVDBMASK", "all": all_on, "out_sha256": {}}
    if blob[off - 8:off] != m["magic"].encode() or int.from_bytes(blob[off:off + 4], "little") != all_on:
        sys.exit(f"feature mask at 0x{addr:08x}: unexpected bytes {blob[off - 8:off + 4].hex()}")
    p["ldr"]["mask"] = m
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stock")
    ap.add_argument("built")
    ap.add_argument("outdir")
    ap.add_argument("--name", default="overdub")
    ap.add_argument("--features", metavar="FEATURES.json", help="make features selectable at install (build.py)")
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
        if a.features:
            p["format"] = 2
            m = features(json.load(open(a.features)), p, blob)
            if m:                               # the OP1_vdk.ldr for each mask value, built from the checked full one
                full = apply.apply_ldr(stock[apply.LDR_MEMBER], p["ldr"], blob)
                for mask in range(m["all"] + 1):
                    if mask & ~m["all"]:
                        continue
                    out = full if mask == m["all"] else apply.assemble_ldr(stock[apply.LDR_MEMBER], p["ldr"],
                                                                            apply.set_mask(blob, m, mask))
                    if (out == full) != (mask == m["all"]):
                        sys.exit("feature mask change did not change OP1_vdk.ldr")
                    m["out_sha256"][str(mask)] = apply.sha(out)
        json.dump(p, open(os.path.join(a.outdir, "patch.json"), "w"), indent=1)

        # Round trip: apply the patch to stock and compare with the build.
        img = os.path.join(tmp, "CHECK.OP1")
        subprocess.run([sys.executable, os.path.join(HERE, "apply.py"), a.stock, img, "--patch", a.outdir, "--all"],
                       check=True, capture_output=True)
        got = {n: c for n, _, c in op1pack.members(op1pack.read_image(img))[0]}
        open(o, "wb").write(got[apply.DB_MEMBER])
        if set(got) != set(built) or any(got[n] != built[n] for n in built if n != apply.DB_MEMBER) \
                or apply.db_digest(o) != p["db"]["out_digest"]:
            sys.exit("round trip failed: apply.py does not reproduce the build")
    print(f"wrote {a.outdir}: {len(p['ldr']['blocks'])} LDR blocks ({len(blob)} B), "
          f"db -{len(p['db']['delete'])} +{len(p['db']['insert'])} rows, {len(added)} added files; round trip OK")
    for f in p.get("features", []):
        print(f"  feature {f['id']} ({f['kind']}): " + (f"mask bit {f['bit']}" if f["kind"] == "key" else
              f"{len(f['insert'])} rows" + (f", restores {len(f['delete'])} stock rows" if f["delete"] else "")))


if __name__ == "__main__":
    main()
