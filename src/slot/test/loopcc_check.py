#!/usr/bin/env python3
"""The bfin GCC 4.3 hardware-loop CC miscompile (loopcc.py): its simulator repro, and a scan of every device object.

  python3 src/slot/test/loopcc_check.py [--scan]

--scan: step 2 only (build.py runs it before every firmware build and stops on a hit).

1. test/loopcc_main.c at -O2 under bfin-elf-run: the trigger (CHORD v2's first factory restore) still comes out
   wrong, and loopcc.py flags it and only it; the forms we use instead (a flat copy, a mask select) come out right.
2. Every C file in src/engine and src/slot, built as the firmware builds them (build_slot.py, src/engine/build.sh:
   -mcpu=bf524 -O2 -ffreestanding -fno-builtin; ui.c also with each of its -D options), compiled to assembly:
   loopcc.py finds no CC read reached from a rewritten loop counter test.
"""
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import loopcc  # noqa: E402

FLAGS = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
UI_DEFS = ["UI_DEBUG", "SVG_COST", "CRUSH_STAGED", "CRUSH_TEST_CARD"]   # build_slot.py's ui.c options


def main():
    scan_only = "--scan" in sys.argv[1:]
    out = tempfile.mkdtemp()
    engine = sorted(os.path.basename(p) for p in glob.glob(os.path.join(SRC, "engine", "*.c")))
    slot = sorted(os.path.basename(p) for p in glob.glob(os.path.join(SRC, "slot", "*.c")))
    script = ["cd /tmp",
              "bfin-elf-gcc -std=gnu99 -msim -O2 -Wall -o r /src/slot/test/loopcc_main.c && bfin-elf-run r",
              "bfin-elf-gcc -std=gnu99 -msim -O2 -S -o /out/repro.s /src/slot/test/loopcc_main.c"]
    if scan_only:
        script = script[:1]
    script += [f"bfin-elf-gcc {FLAGS} -S -o /out/engine_{f[:-2]}.s /src/engine/{f} || echo FAILED {f}" for f in engine]
    script += [f"bfin-elf-gcc {FLAGS} -I /src/engine -S -o /out/slot_{f[:-2]}.s /src/slot/{f} || echo FAILED {f}"
               for f in slot]
    script += [f"bfin-elf-gcc {FLAGS} -I /src/engine -D{d} -S -o /out/slot_ui_{d}.s /src/slot/ui.c || echo FAILED ui {d}"
               for d in UI_DEFS]
    r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SRC}:/src:ro", "-v",
                        f"{out}:/out", "op1-bfin", "sh", "-c", "\n".join(script)], capture_output=True, text=True)
    print(r.stdout + r.stderr[-2000:])
    fails = 0

    def check(ok, what):
        nonlocal fails
        print(("PASS " if ok else "FAIL ") + what)
        fails += not ok

    if not scan_only:
        res = dict(l.split(" ", 1) for l in r.stdout.splitlines() if l.startswith(("restore_", "energy_")))
        check(res.get("restore_bad", "0").strip() not in ("", "0"),
              f"repro: the per-key loop around a hoisted test still miscompiles at -O2 ({res.get('restore_bad')} keys wrong)")
        check(res.get("restore_flat", "").strip() == "0 0", "repro: the flat copy and the per-key clear are right")
        check(res.get("energy_mask", "").strip() == "0", "repro: the mask select matches `plain ? w : lp`")
        repro = loopcc.find(open(os.path.join(out, "repro.s")).read()) if os.path.exists(os.path.join(out, "repro.s")) else []
        check([h[0] for h in repro] == ["_restore_bad"], f"loopcc.py flags the repro's restore_bad and nothing else: {repro}")
    check("FAILED" not in r.stdout, "every device source compiles")
    files = sorted(glob.glob(os.path.join(out, "*_*.s")))
    check(len(files) == len(engine) + len(slot) + len(UI_DEFS), f"{len(files)} device objects scanned")
    for f in files:
        hits = loopcc.find(open(f).read())
        check(not hits, f"{os.path.basename(f)[:-2]}: no CC read after a rewritten loop counter test"
              + "".join(f"\n     {fn} line {n}: {s}" for fn, n, s in hits))
    print("all passed" if r.returncode == 0 and not fails else "FAILED")
    sys.exit(0 if r.returncode == 0 and not fails else 1)


if __name__ == "__main__":
    main()
