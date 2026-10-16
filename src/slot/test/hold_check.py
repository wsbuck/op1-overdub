#!/usr/bin/env python3
"""Simulator checks for HOLD and CHORD (src/slot/hold.c, hold_impl.h), including the feature mask that tools/apply.py
sets when the user leaves HOLD or CHORD out.

  python3 src/slot/test/hold_check.py

Builds test/hold_test.c (stand-ins for TE's note functions; every mask combination) with the Blackfin compiler and runs
it under bfin-elf-run. Also builds hold.c with the device flags and checks that the object reads the mask from
slot.S's _feature_mask (an undefined symbol there, so the compiler cannot fold it to a constant).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)


def main():
    cflags = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
    script = ("set -e; cd /tmp\n"
              "bfin-elf-gcc -std=gnu99 -msim -O2 -Wall -Wextra -I /src -o m /src/test/hold_test.c\n"
              "bfin-elf-run m\n"
              f"bfin-elf-gcc {cflags} -c -o hold.o /src/hold.c\n"
              "echo NM; bfin-elf-nm hold.o\n")
    r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "op1-bfin",
                        "sh", "-c", script], capture_output=True, text=True)
    print(r.stdout + r.stderr[-2000:])
    nm = r.stdout.split("NM\n", 1)[-1].split()
    reads_mask = "_feature_mask" in nm and nm[nm.index("_feature_mask") - 1] == "U"
    print("device hold.o reads _feature_mask:", "yes" if reads_mask else "NO")
    ok = r.returncode == 0 and "FAIL" not in r.stdout and "all passed" in r.stdout and reads_mask
    print("all passed" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
