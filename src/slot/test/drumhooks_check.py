#!/usr/bin/env python3
"""Simulator checks for slot.S's DRUM_HOOKS with several drum engines, before flashing (these paths run at boot for
every drum slot).

  python3 src/slot/test/drumhooks_check.py

Assembles slot.S with a test slot_gen.inc (drumhooks/: two drum engines, ids 23 and 24, as build_slot.py generates
them) and drumhooks_abi.S (TE's factory frame and assert continuation), links drumhooks_main.c (stand-ins for TE's dbox
voice constructor, index setter, name lookup and dbox screen update) and runs it under bfin-elf-run. Checks: the
factory builds 12 dbox voices on the right engine's vtable after that engine's slot init, and sends every other id to
TE's assert path; the validator reads our ids as dbox and leaves others alone; the dbox screen update draws ours and
hands every other id to TE's; registers a TE caller relies on are preserved (fx_abi.S).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)


def main():
    script = ("set -e; cd /tmp\n"
              "bfin-elf-as -I /src/test/drumhooks -I /src -o slot.o /src/slot.S\n"
              "bfin-elf-as -I /src --defsym NO_FX=1 -o abi.o /src/test/fx_abi.S\n"
              "bfin-elf-as -o te.o /src/test/drumhooks_abi.S\n"
              "bfin-elf-gcc -std=gnu99 -msim -O0 -Wall -o m /src/test/drumhooks_main.c slot.o abi.o te.o\n"   # -O0: stubs without hardware loops
              "bfin-elf-run m\n")
    r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "op1-bfin",
                        "sh", "-c", script], capture_output=True, text=True)
    print(r.stdout + r.stderr[-2000:])
    ok = r.returncode == 0 and "FAIL" not in r.stdout and "abi 00000000" in r.stdout
    print("all passed" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
