#!/usr/bin/env python3
"""Simulator check of our FX through their firmware entry wrappers (entry.inc), before flashing.

  python3 src/slot/test/fx_abi_check.py

For each FX (ensemble, pingpong, distortion, crush, repeat), on bfin-elf-run:
  - every call through FX_MONO_ENTRY / FX_STEREO_ENTRY preserves R4-R7, P3-P5, FP, SP, both hardware loops and
    L0-L3 (sentinel check in fx_abi.S), with knobs changing every block;
  - the stereo entry's 4th argument (params at [SP + 12]) arrives: mix 0 (crush: every knob 0; repeat: FORTUNE OFF)
    gives the dry input, bit exact;
  - mono and stereo instances are independent: interleaving them (synth FX + master FX in one callback) gives
    exactly the outputs of running each alone (catches shared scratch carrying state between calls).
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
FX = ["ensemble", "pingpong", "distortion", "crush", "repeat"]
BLOCKS = 300


def main():
    w = tempfile.mkdtemp(prefix="fx_abi_")
    cflags = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
    runs = [(i, m) for i in range(len(FX)) for m in "msid"]
    script = ["set -e; cd /tmp"] + [f"bfin-elf-gcc {cflags} -c -o {f}.o /src/{f}.c" for f in FX] + [
        "bfin-elf-as -I /src -o abi.o /src/test/fx_abi.S",
        "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -o t /src/test/fx_abi_main.c abi.o "
        + " ".join(f"{f}.o" for f in FX) + " /src/test/pingpong_ram.c"]
    script += [f"bfin-elf-run /tmp/t {i} {m} {BLOCKS} > /w/{i}{m}.txt 2> /w/{i}{m}.err; echo $? > /w/{i}{m}.rc"
               for i, m in runs]
    open(os.path.join(w, "run.sh"), "w").write("\n".join(script) + "\n")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v", f"{w}:/w",
                    "op1-bfin", "sh", "/w/run.sh"], check=True)
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    def lines(i, m, tag):
        return [l for l in open(os.path.join(w, f"{i}{m}.txt")).read().split("\n") if l.startswith(tag)]

    for i, f in enumerate(FX):
        rcs = {m: open(os.path.join(w, f"{i}{m}.rc")).read().strip() for m in "msid"}
        errs = " ".join(open(os.path.join(w, f"{i}{m}.err")).read().strip() for m in "msid").strip()
        check(all(rc == "0" for rc in rcs.values()),
              f"{f}: {4 * BLOCKS} entry calls preserve callee-saved, loop and L registers" + (f" ({errs})" if errs else ""))
        d = lines(i, "d", "s")
        dry_ok = len(d) == BLOCKS
        ph, inc = 0, 52000000
        for line in d:
            v = list(map(int, line.split()[1:]))
            for n in range(64):
                def sine(p):
                    x = ((p & 0xFFFFFFFF) ^ 0x80000000) - 0x80000000 >> 16
                    return ((x * (32767 - abs(x))) >> 13) >> 1
                y = sine(ph)
                yr = sine(ph + 0x40000000)
                if v[2 * n] != ((y + 32768) & 0xFFFF) - 32768 or v[2 * n + 1] != ((yr + 32768) & 0xFFFF) - 32768:
                    dry_ok = False
                ph += inc
        check(dry_ok, f"{f}: stereo entry passes the 4th argument (mix 0 = dry input, bit exact)")
        same = lines(i, "i", "m") == lines(i, "m", "m") and lines(i, "i", "s") == lines(i, "s", "s") \
            and len(lines(i, "m", "m")) == BLOCKS
        check(same, f"{f}: mono and stereo interleaved = each alone (no shared state between instances)")
    print(f"\n{len(fails)} failed" if fails else "\nall passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
