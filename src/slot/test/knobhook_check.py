#!/usr/bin/env python3
"""Simulator checks for the synth encoder hook (src/slot/knobhook.c, entry.inc KNOB_HOOK) and the drum per-key
loader/saver type compare (entry.inc DRUM_TYPE_EQ) before flashing.

  python3 src/slot/test/knobhook_check.py

Builds knobhook.c with the device flags, the two asm macros as slot.S instantiates them (knobhook_abi.S) and
knobhook_main.c (stand-ins for TE's FUN_01a1dfec and JSON value functions) and runs it under bfin-elf-run. Checks: other
engines reach TE's function with all four arguments; the probe's knobs 0..5 still go through TE, 6..7 don't; the packing
(sprobe.h) and the hook's record; "dbox" gets TE's result with no extra work; our type compares equal and is counted;
every call preserves the registers a TE caller relies on (fx_abi.S).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
ENGINE = os.path.join(os.path.dirname(SLOT), "engine")


def main():
    cflags = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
    script = ("set -e; cd /tmp\n"
              f"bfin-elf-gcc {cflags} -I /engine -c -o kh.o /src/knobhook.c\n"
              "bfin-elf-as -I /src --defsym NO_FX=1 -o abi.o /src/test/fx_abi.S\n"
              "bfin-elf-as -I /src -o hooks.o /src/test/knobhook_abi.S\n"
              "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -I /engine -o m /src/test/knobhook_main.c kh.o abi.o hooks.o\n"
              "bfin-elf-run m\n")
    r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "-v",
                        f"{ENGINE}:/engine:ro", "op1-bfin", "sh", "-c", script], capture_output=True, text=True)
    print(r.stdout + r.stderr[-2000:])
    ok = r.returncode == 0 and "FAIL" not in r.stdout and "abi 00000000" in r.stdout
    print("all passed" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
