#!/usr/bin/env python3
"""Build the custom firmware image and a shareable patch from stock OP-1 firmware 246.

  python3 build.py [--engines va,wavetable,grain] [--fx ensemble,pingpong,distortion] [--version 1.0]

Needs Docker with the `op1-bfin` toolchain image (docker build -t op1-bfin tools/docker) and Python 3.10+.
Stock 246 is downloaded from teenage.engineering into firmware/stock/ if it isn't there (and checked by sha256).
Outputs:
  firmware/images/<NAME>.OP1     the image to flash
  dist/overdub-<version>.zip     the patch and tools/apply.py, to share (our code, rows and SVGs only; no TE firmware)
Everything under firmware/ is TE-derived and gitignored: never commit or share it.
"""
import argparse
import hashlib
import json
import shutil
import os
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import op1pack  # noqa: E402

STOCK_URL = "https://teenage.engineering/_software/op-1/op1_246.op1"
STOCK_SHA256 = "c5315218f825f143b415ca554516541898abee843d3a236df0b54c04e1fb13a9"
FX_SVGS = {"ensemble": "choir.svg", "pingpong": "pingpong.svg", "distortion": "pedals.svg", "crush": "crush3d.svg"}   # by source
ENGINE_SVGS = {"fm4": "fm4.svg"}   # by source: engine screens drawn through the SVG route (design/synth/)


def pick(path, names, what):
    items = json.load(open(path))
    if names is None:
        return [i for i in items if not i.get("test")]   # test engines (e.g. inprobe) only when named
    want = [n for n in names.split(",") if n]
    unknown = set(want) - {i["name"] for i in items}
    if unknown:
        sys.exit(f"unknown {what}: {', '.join(sorted(unknown))} (have {', '.join(i['name'] for i in items)})")
    return [i for i in items if i["name"] in want]          # keep the json's order


def run(*cmd):
    print("+", " ".join(os.path.relpath(c, ROOT) if c.startswith(ROOT) else c for c in cmd))
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engines", help="comma-separated engine names from src/slot/engines.json (default: all but "
                    "test engines)")
    ap.add_argument("--fx", help="comma-separated FX names from src/slot/fx.json (default: all; '' for none)")
    ap.add_argument("--no-hold", action="store_true", help="leave out HOLD (SHIFT+T2) and CHORD")
    ap.add_argument("--no-chord", action="store_true", help="leave out CHORD")
    ap.add_argument("--svg-cost", action="store_true", help="show the screen render cost on the FX screens (development)")
    ap.add_argument("--debug-ui", action="store_true", help="show profiler lines on the engine screens (development)")
    ap.add_argument("--db-trim", action="append", default=[], metavar="FOLDER=N",
                    help="keep only the first N presets of a folder (e.g. to stay within MAX_SYNTH_PRESETS in a test image)")
    ap.add_argument("--name", default="OVERDUB", help="image name (default OVERDUB -> firmware/images/OVERDUB.OP1)")
    ap.add_argument("--version", default="dev", help="release version, used in the zip name (default dev)")
    ap.add_argument("--stock", default=os.path.join(ROOT, "firmware", "stock", "op1_246.op1"))
    a = ap.parse_args()
    if not a.name.isalnum() or any(w in a.name.upper() for w in op1pack.FORBIDDEN_WORDS):
        sys.exit("--name must be letters and digits, without FRC, RBT or KEY (the OP-1's updater acts on those)")

    if not os.path.exists(a.stock):
        os.makedirs(os.path.dirname(a.stock), exist_ok=True)
        print(f"downloading {STOCK_URL}")
        urllib.request.urlretrieve(STOCK_URL, a.stock)
    if hashlib.sha256(open(a.stock, "rb").read()).hexdigest() != STOCK_SHA256:
        sys.exit(f"{a.stock} is not stock OP-1 firmware 246")

    build = os.path.join(ROOT, "firmware", "build", "release", a.name.lower())
    os.makedirs(build, exist_ok=True)
    stock = {n: c for n, _, c in op1pack.members(op1pack.read_image(a.stock))[0]}
    stock_ldr, stock_db = os.path.join(build, "stock_OP1_vdk.ldr"), os.path.join(build, "stock_op1_factory.db")
    open(stock_ldr, "wb").write(stock["OP1_vdk.ldr"])
    open(stock_db, "wb").write(stock["content/op1_factory.db"])

    engines = pick(os.path.join(ROOT, "src", "slot", "engines.json"), a.engines, "engines")
    fxs = pick(os.path.join(ROOT, "src", "slot", "fx.json"), a.fx, "FX")
    if not engines:
        sys.exit("at least one engine is needed")
    ej, fj = os.path.join(build, "engines.json"), os.path.join(build, "fx.json")
    json.dump(engines, open(ej, "w"), indent=1)
    json.dump(fxs, open(fj, "w"), indent=1)

    iter_db, db, out_ldr = (os.path.join(build, f) for f in ("iter.db", "op1_factory.db", "OP1_vdk.ldr"))
    if os.path.exists(iter_db):
        os.remove(iter_db)
    run(sys.executable, os.path.join(ROOT, "tools", "enable_iter_db.py"), stock_db, iter_db,
        "--presets", os.path.join(ROOT, "src", "slot", "iter_presets.json"))   # templates for our presets
    cmd = [sys.executable, os.path.join(ROOT, "tools", "build_slot.py"), stock_ldr, ej, out_ldr,
           "--db-in", iter_db, "--db-out", db, "--db-enable-filter", "--db-trim", "iter=0", "--build", os.path.join(build, "slot")]
    cmd += [x for t in a.db_trim for x in ("--db-trim", t)]
    cmd += [] if a.no_hold else ["--hold"] + ([] if a.no_chord else ["--chord"])
    cmd += ["--fx", fj] if fxs else []
    cmd += ["--svg-cost"] * a.svg_cost + ["--debug-ui"] * a.debug_ui
    run(*cmd)

    image = os.path.join(ROOT, "firmware", "images", f"{a.name}.OP1")
    os.makedirs(os.path.dirname(image), exist_ok=True)
    cmd = [sys.executable, os.path.join(ROOT, "tools", "op1pack.py"), "build", a.stock, image,
           "--replace", f"content/op1_factory.db={db}", "--replace", f"OP1_vdk.ldr={out_ldr}"]
    for f in fxs:
        if f["source"] in FX_SVGS:
            svg = FX_SVGS[f["source"]]
            cmd += ["--add", f"content/display/{svg}={os.path.join(ROOT, 'design', 'fx', svg)}"]
    for e in engines:
        if e["source"] in ENGINE_SVGS:
            svg = ENGINE_SVGS[e["source"]]
            cmd += ["--add", f"content/display/{svg}={os.path.join(ROOT, 'design', 'synth', svg)}"]
    run(*cmd)

    # The shareable bundle: apply.py and the two modules it uses, plus the patch (no TE firmware).
    dist = os.path.join(ROOT, "dist", f"overdub-{a.version}" + ("" if a.name == "OVERDUB" else f"-{a.name.lower()}"))
    run(sys.executable, os.path.join(ROOT, "tools", "mkpatch.py"), a.stock, image, os.path.join(dist, "patch"), "--name",
        f"overdub {a.version}: {', '.join(e['name'] for e in engines)}; FX {', '.join(f['name'] for f in fxs) or 'none'}")
    for f in ("apply.py", "ldr.py", "op1pack.py"):
        shutil.copyfile(os.path.join(ROOT, "tools", f), os.path.join(dist, f))
    shutil.copyfile(os.path.join(ROOT, "LICENSE"), os.path.join(dist, "LICENSE"))
    # The image to flash is the one the bundle produces, so what is tested is what others get.
    run(sys.executable, "-B", os.path.join(dist, "apply.py"), a.stock, image)   # -B: no __pycache__ in the bundle
    zipf = shutil.make_archive(dist, "zip", os.path.dirname(dist), os.path.basename(dist))
    print(f"\nimage: {os.path.relpath(image, ROOT)}\npatch: {os.path.relpath(zipf, ROOT)} "
          f"(unzip, then: python3 apply.py op1_246.op1)")


if __name__ == "__main__":
    main()
