#!/usr/bin/env python3
"""Check a built factory DB against TE's drum sound browser (notes/drum-engine.md "Browser columns").

  python3 src/slot/test/drum_browser_check.py FACTORY.db [--stock STOCK_FACTORY.db]

Emulates the browser as read in firmware 246: FUN_01a04fe8 loads `SELECT id,patch,folder FROM drum_presets`, then
drum_user (no ORDER BY: rowid order); FUN_01a028ec adds each sound to the column named by its folder
(FUN_01a185f4), creating a column for a new folder and re-sorting the columns (FUN_01a18a58: "thru" first, then
byte order); the label is the folder printed with snprintf(buf, 14, "%s"). A snapshot is saved with folder "user"
(FUN_01995bac). Checks, for every engine in src/slot/engines.json with "kits" whose type is in the DB:
  - the engine has its own column, labelled with its name, holding exactly its kits;
  - its kits are in no other column, and the dbox and drum columns hold only TE's types (and "replace" presets);
  - with --stock: TE's columns keep every stock sound except the ones a "replace" engine swaps;
  - labels fit the 13 characters, column order is dbox, drum, then ours, then user (a simulated snapshot);
  - factory drum sounds leave room for user sounds under the 58 limit (patch.cpp:656/664).
Pure Python (no simulator): the column logic is a few lines of TE's C++, re-implemented here.
"""
import argparse
import json
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
ENGINES = os.path.join(ROOT, "src", "slot", "engines.json")
MAX_DRUM_PATCHES, LABEL = 58, 13


def columns(rows):
    """rows: (folder, name) in load order -> {folder: [names]} and the browser's column order."""
    cols = {}
    for folder, name in rows:
        cols.setdefault(folder, []).append(name)
    order = sorted(cols, key=lambda f: (f != "thru", f.encode()))
    return cols, order


def load(db):
    con = sqlite3.connect(db)
    rows = [(f, json.loads(p)) for f, p in con.execute("SELECT folder, patch FROM drum_presets ORDER BY rowid")]
    con.close()
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("db")
    ap.add_argument("--stock")
    a = ap.parse_args()
    fails = []

    def check(ok, what):
        print(("ok    " if ok else "FAIL  ") + what)
        ok or fails.append(what)

    # the comparator itself: "thru" first, then byte order (upper case before lower case)
    check(columns([("user", "a"), ("thru", "b"), ("dbox", "c"), ("Zed", "d")])[1] == ["thru", "Zed", "dbox", "user"],
          "column sort: thru first, then byte order")

    engines = json.load(open(ENGINES))
    rows = load(a.db)
    types = {p["type"] for _, p in rows}
    replaced = {e["name"] for e in engines if e.get("drum") and "replace" in e}
    kitted = [e for e in engines if e.get("drum") and "kits" in e and e["name"] in types]
    check(bool(kitted), f"DB has kits of {[e['name'] for e in kitted]}")

    snapshot = ("user", "20761015_1200")           # what FUN_01995bac inserts for any saved drum sound
    cols, order = columns([(f, p["name"]) for f, p in rows] + [snapshot])
    print("columns:", ", ".join(f"{c} ({len(cols[c])})" for c in order))
    for e in kitted:
        kits = list(json.load(open(os.path.join(ROOT, e["kits"]))))
        name = e["name"]
        check(sorted(cols.get(name, [])) == sorted(kits), f"{name}: own column {name!r} holds exactly {kits}")
        check(all(f == name for f, p in rows if p["type"] == name), f"{name}: no {name} sound in another column")
    for te in ("dbox", "drum"):
        bad = [p["name"] for f, p in rows if f == te and p["type"] not in (te, *replaced)]
        check(not bad, f"{te} column holds only {te} sounds{' (and replace presets)' if te == 'dbox' else ''}: {bad}")
    check(all(len(c) <= LABEL for c in order), f"labels fit {LABEL} characters")
    ours = sorted(e["name"] for e in kitted)
    check(order == ["dbox", "drum", *ours, "user"], f"order dbox, drum, {', '.join(ours)}, user: {order}")
    n = len(rows)
    check(n < MAX_DRUM_PATCHES, f"{n} factory drum sounds: room for {MAX_DRUM_PATCHES - n} saved")

    if a.stock:
        stock = load(a.stock)
        swapped = {e["replace"] for e in engines if e.get("drum") and "replace" in e and e["name"] in types}
        want = sorted((f, p["name"]) for f, p in stock if p["name"] not in swapped)
        have = sorted((f, p["name"]) for f, p in rows if f in ("dbox", "drum") and p["type"] in ("dbox", "drum"))
        check(have == want, f"TE's columns keep their {len(want)} stock sounds (minus {sorted(swapped)})")

    print("all passed" if not fails else f"FAILED: {len(fails)}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
