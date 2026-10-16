#!/usr/bin/env python3
"""Host test of the install CLI (tools/apply.py): applies a patch to stock 246 with several feature selections and
checks every result.

  python3 tools/apply_check.py [PATCH_DIR] [--stock firmware/stock/op1_246.op1] [--registered va,amp,...]

PATCH_DIR: a patch folder written by build.py (default: the newest dist/*/patch). Per selection:
  - te-boot.ldr byte-identical to stock; the image's other stock members unchanged;
  - OP1_vdk.ldr: equal to the all-features one except the 4-byte feature word, which holds the selected HOLD/CHORD bits;
  - names: TE's engine and FX name tables, read through the repointed code in the patched OP1_vdk.ldr (as TE's lookups
    FUN_019b97c4 / FUN_019aae44 walk them), hold every engine and FX with the same ids in every selection, hidden or not;
  - op1_factory.db: integrity ok; a hidden engine has no synth_types row, no presets and no drum kits, a hidden FX no
    fx_types row; a shown one has exactly the all-features rows; stock rows are all there (the probe's replaced dbox
    preset aside); preset counts stay within TE's boot limits;
  - saved sounds: a copy of the DB with saved sounds that use hidden engines and FX still validates against the name
    tables (the checks TE's patch validator does by name: engine type, fx_type), so they load and never stop boot.
"""
import argparse
import glob
import json
import os
import sqlite3
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import apply  # noqa: E402
import build_slot as bs  # noqa: E402
import ldr  # noqa: E402
import op1pack  # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    fails += not ok


class Mem:
    """Memory as the boot ROM leaves it: every loaded block in order, later ones over earlier."""

    def __init__(self, data):
        self.blocks = [b for b in ldr.parse(data) if b.loads_memory and not b.is_fill and not b.is_encrypted]

    def rd(self, addr, n):
        for b in reversed(self.blocks):
            if b.target <= addr and addr + n <= b.target + b.count:
                return b.payload[addr - b.target:addr - b.target + n]
        raise KeyError(hex(addr))

    def half(self, addr):
        return struct.unpack("<H", self.rd(addr, 2))[0]

    def cstr(self, addr):
        return self.rd(addr, 16).split(b"\0")[0].decode()

    def pair(self, insn):
        """The 32-bit constant built by the .L/.H pair starting at insn (as build_slot.py repoints them)."""
        first = self.half(insn)
        for p in range(insn + 4, insn + 20, 2):
            hw = self.half(p)
            if hw & 0xFFA0 == 0xE100 and hw & 0x3F == first & 0x3F and (hw ^ first) & 0x40:
                lo, hi = (insn, p) if not first & 0x40 else (p, insn)
                return self.half(lo + 2) | self.half(hi + 2) << 16
        raise ValueError(hex(insn))

    def table(self, ref, bound):
        """{name: id} of a {char *name, int id} table, its size from the loop bound `Rx = n`."""
        addr, n = self.pair(ref), (self.half(bound) >> 3) & 0x7F
        ent = [struct.unpack("<Ii", self.rd(addr + 8 * i, 8)) for i in range(n)]
        return {self.cstr(p): i for p, i in ent}


def name_tables(ldr_bytes):
    m = Mem(ldr_bytes)
    engines = m.table(bs.TABLE_REFS[bs.NAME_TABLE][0], bs.NAME_BOUNDS[0])
    if m.table(bs.TABLE_REFS[bs.NAME_TABLE][1], bs.NAME_BOUNDS[1]) != engines:
        raise ValueError("the two engine name loops disagree")
    fx = m.table(bs.FX_TABLE_REFS[(bs.FX_NAME_TABLE, 0)][1], bs.FX_NAME_BOUNDS[2])
    return engines, fx


def rows(path, table):
    con = sqlite3.connect(path)
    r = {x[0]: x[1:] for x in con.execute(f'SELECT rowid, * FROM "{table}"')}
    con.close()
    return r


def ours(r, table, name):
    """Rows of table that belong to engine/FX name (as tools/mkpatch.py attributes them)."""
    if table in ("synth_types", "fx_types"):
        return {k for k, v in r.items() if v[1] == name}
    return {k for k, v in r.items() if v[2] == name or json.loads(v[1]).get("type") == name}


def validate(db, engines, fx):
    """TE's by-name checks on every sound in the DB: engine type (instr_types.cpp:49 asserts on an unknown name) and
    fx_type (fx_types.cpp:42); and the boot-time preset limits (patch.cpp:616, :656)."""
    bad = []
    con = sqlite3.connect(db)
    for table in ("synth_presets", "drum_presets"):
        for rid, patch in con.execute(f"SELECT id, patch FROM {table}"):
            p = json.loads(patch)
            if p.get("type") not in engines and not (table == "synth_presets" and p.get("type") == "sampler") \
                    and not (table == "drum_presets" and p.get("type") == "drum"):
                bad.append(f"{table} {rid}: engine {p.get('type')!r}")
            if "fx_type" in p and p["fx_type"] not in fx:
                bad.append(f"{table} {rid}: fx {p['fx_type']!r}")
    synth = con.execute("SELECT count(*) FROM synth_presets WHERE folder != 'sampler'").fetchone()[0]
    drum = con.execute("SELECT count(*) FROM drum_presets").fetchone()[0]
    con.close()
    return bad, synth, drum


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("patch", nargs="?")
    ap.add_argument("--stock", default=os.path.join(ROOT, "firmware", "stock", "op1_246.op1"))
    ap.add_argument("--registered", metavar="A,B", help="engines that must stay registered in every selection, e.g. "
                    "the ones a device's saved sounds use (default: every engine in the patch)")
    a = ap.parse_args()
    patch = a.patch or max(glob.glob(os.path.join(ROOT, "dist", "*", "patch")), key=os.path.getmtime)
    p = json.load(open(os.path.join(patch, "patch.json")))
    feats = {f["id"]: f for f in p["features"]}
    keys = [f for f in p["features"] if f["kind"] == "key"]
    m = p["ldr"]["mask"]
    print(f"patch {patch}: {p['name']}\nfeatures {', '.join(feats)}; always {', '.join(p['always'])}")
    stock = {n: c for n, _, c in op1pack.members(op1pack.read_image(a.stock))[0]}

    r = subprocess.run([sys.executable, "-B", os.path.join(HERE, "apply.py"), "--list", "--patch", patch],
                       capture_output=True, text=True)
    check(r.returncode == 0 and all(f in r.stdout for f in feats), "--list shows every feature")
    r = subprocess.run([sys.executable, "-B", os.path.join(HERE, "apply.py"), a.stock, "X.OP1", "--patch", patch,
                        "--without", "nosuch"], capture_output=True, text=True)
    check(r.returncode != 0 and "unknown feature" in r.stderr, "an unknown feature name is refused")

    selections = [("all", ["--all"]), ("without amp,crush", ["--without", "amp,crush"]),
                  ("only va,grain,hold", ["--only", "va,grain,hold"]), ("without hold,chord", ["--without", "hold,chord"]),
                  ("without chord,fmdrum,ensemble", ["--without", "chord,fmdrum,ensemble"]), ("none", ["--only", ""])]
    selections = [(n, s) for n, s in selections if all(x in feats for arg in s[1:] for x in arg.split(",") if x)]
    with tempfile.TemporaryDirectory() as tmp:
        results = {}
        for label, args in selections:
            out = os.path.join(tmp, "PICK.OP1")
            r = subprocess.run([sys.executable, "-B", os.path.join(HERE, "apply.py"), a.stock, out, "--patch", patch]
                               + args, capture_output=True, text=True, stdin=subprocess.DEVNULL)
            check(r.returncode == 0, f"[{label}] apply.py succeeds" + ("" if r.returncode == 0 else f": {r.stderr[-500:]}"))
            if r.returncode:
                continue
            img = {n: c for n, _, c in op1pack.members(op1pack.read_image(out))[0]}
            if args[0] == "--all":
                shown = set(feats)
            elif args[0] == "--only":
                shown = {x for x in args[1].split(",") if x}
            else:
                shown = set(feats) - set(args[1].split(","))
            db = os.path.join(tmp, f"{len(results)}.db")
            open(db, "wb").write(img[apply.DB_MEMBER])
            results[label] = (img, shown, db)
        full_img, _, full_db = results["all"]
        full_engines, full_fx = name_tables(full_img[apply.LDR_MEMBER])
        device = a.registered.split(",") if a.registered else \
            [f["id"] for f in p["features"] if f["kind"] in ("synth", "drum")] + p["always"]
        for label, (img, shown, db) in results.items():
            check(img["te-boot.ldr"] == stock["te-boot.ldr"], f"[{label}] te-boot.ldr identical to stock")
            check(all(img[n] == c for n, c in stock.items() if n not in (apply.LDR_MEMBER, apply.DB_MEMBER)),
                  f"[{label}] other stock members unchanged")
            # LDR: the all-features one with only the feature word changed
            mask = m["all"] & ~sum(f["bit"] for f in keys if f["id"] not in shown)
            out_ldr, full_ldr = img[apply.LDR_MEMBER], full_img[apply.LDR_MEMBER]
            diff = [i for i in range(len(full_ldr)) if out_ldr[i] != full_ldr[i]] if len(out_ldr) == len(full_ldr) else None
            mem = Mem(out_ldr)
            blk = next(b for b in p["ldr"]["blocks"] if b["offset"] <= m["offset"] < b["offset"] + b["length"])
            addr = blk["target"] + m["offset"] - blk["offset"]
            word = struct.unpack("<I", mem.rd(addr, 4))[0]
            check(word == mask and mem.rd(addr - 8, 8) == b"OVDBMASK" and diff is not None and len(diff) <= 4
                  and all(abs(i - diff[0]) < 4 for i in diff) and apply.sha(out_ldr) == m["out_sha256"][str(mask)],
                  f"[{label}] OP1_vdk.ldr = all-features + feature word {word} at 0x{addr:08x} (expected {mask}), "
                  f"{len(diff or [])} bytes differ")
            engines, fx = name_tables(out_ldr)
            check(engines == full_engines and fx == full_fx and all(e in engines for e in device),
                  f"[{label}] every engine and FX registered with the same id ({len(engines)} engine names, "
                  f"{len(fx)} FX names; {', '.join(f'{e} {engines.get(e)}' for e in device)})")
            # DB rows
            con = sqlite3.connect(db)
            check(con.execute("PRAGMA integrity_check").fetchone()[0] == "ok", f"[{label}] DB integrity ok")
            con.close()
            for f in p["features"]:
                if f["kind"] == "key":
                    continue
                tables = ["fx_types"] if f["kind"] == "fx" else ["synth_types", "synth_presets"] if f["kind"] == "synth" \
                    else ["drum_presets"]
                got = {t: ours(rows(db, t), t, f["id"]) for t in tables}
                want = {t: ours(rows(full_db, t), t, f["id"]) if f["id"] in shown else set() for t in tables}
                same = all({k: rows(db, t)[k] for k in got[t]} == {k: rows(full_db, t)[k] for k in want[t]}
                           for t in tables)
                check(got == want and same, f"[{label}] {f['id']}: {'shown' if f['id'] in shown else 'hidden'} "
                      f"({', '.join(f'{t} {len(got[t])}' for t in tables)})")
            sdb = os.path.join(tmp, "stock.db")
            open(sdb, "wb").write(stock[apply.DB_MEMBER])
            replaced = {(t, rid) for t, rid in p["db"]["delete"]}
            lost = [(t, k) for t in ("synth_types", "fx_types", "drum_types", "synth_presets", "drum_presets")
                    for k, v in rows(sdb, t).items() if (t, k) not in replaced and rows(db, t).get(k) != v]
            check(not lost, f"[{label}] every stock row kept {lost[:3]}")
            bad, synth, drum = validate(db, engines, fx)
            check(not bad and synth <= bs.MAX_SYNTH_PRESETS and drum <= bs.MAX_FACTORY_DRUM_PRESETS,
                  f"[{label}] DB validates: {synth} synth presets (<= {bs.MAX_SYNTH_PRESETS}), {drum} drum presets "
                  f"(<= {bs.MAX_FACTORY_DRUM_PRESETS}) {bad[:3]}")
            # saved sounds using what this selection hides
            hidden = [f for f in p["features"] if f["id"] not in shown and f["kind"] != "key"]
            if hidden:
                saved = os.path.join(tmp, "saved.db")
                open(saved, "wb").write(open(db, "rb").read())
                con = sqlite3.connect(saved)
                hfx = [f["id"] for f in hidden if f["kind"] == "fx"] or ["delay"]
                for f in hidden:
                    full = sqlite3.connect(full_db)
                    if f["kind"] == "synth":
                        tpl = json.loads(full.execute("SELECT patch FROM synth_presets WHERE folder=? LIMIT 1",
                                                      (f["id"],)).fetchone()[0])
                        tpl.update(name="saved", fx_type=hfx[0], fx_active=True)
                        con.execute("INSERT INTO synth_presets (patch, folder) VALUES (?, 'user')", (json.dumps(tpl),))
                    elif f["kind"] == "drum":
                        tpl = json.loads(full.execute("SELECT patch FROM drum_presets WHERE folder=? LIMIT 1",
                                                      (f["id"],)).fetchone()[0])
                        tpl.update(name="saved", fx_type=hfx[0])
                        con.execute("INSERT INTO drum_presets (patch, folder) VALUES (?, 'user')", (json.dumps(tpl),))
                    full.close()
                con.commit()
                con.close()
                bad, _, _ = validate(saved, engines, fx)
                check(not bad, f"[{label}] saved sounds with hidden {', '.join(f['id'] for f in hidden)} still "
                      f"validate (engine and FX names resolve) {bad[:3]}")
                con = sqlite3.connect(saved)            # the check itself: an unknown engine name must fail
                con.execute("""INSERT INTO synth_presets (patch, folder) VALUES ('{"type": "nosuch"}', 'user')""")
                con.commit()
                con.close()
                check(len(validate(saved, engines, fx)[0]) == 1, f"[{label}] ... and an unknown engine name does not")
    print(f"\n{fails} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
