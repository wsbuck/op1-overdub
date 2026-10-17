#!/usr/bin/env python3
"""Simulator checks for CHORD v2 (src/slot/chord2.h, chord2_impl.h, hold.c; entry.inc C2_HOOKS) before flashing.

  python3 src/slot/test/chord2_check.py

1. test/chord2_test.c (the engine with stand-ins for TE: maps, budget, HOLD, page keys and knobs, learn, undo, the
   file and its corruption cases, every feature mask) built by the Blackfin compiler and run under bfin-elf-run.
2. The hook entries (test/chord2_abi.S = slot.S's C2_HOOKS) linked with hold.c built with the device flags
   (test/chord2_main.c): register preservation (fx_abi.S), TE's arguments and results passed through when CHORD v2 is
   off or the page closed, the frame hook's order (TE's render with the buffer, then c2_frame).
3. hold.c (device flags) calls no libgcc / libc function (no division, no memcpy / memset): only TE's and slot.S's
   symbols are undefined.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
ALLOWED = {"_feature_mask", "_te_make_note", "_te_note_on", "_te_note_off", "_te_synth_shiftkey", "_te_synth_press",
           "_te_synth_release", "_te_synth_knob", "_te_yaffs_open", "_te_yaffs_read", "_te_yaffs_write",
           "_te_yaffs_close", "_te_yaffs_mkdir"}


def main():
    cflags = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
    script = ("set -e; cd /tmp\n"
              "bfin-elf-gcc -std=gnu99 -msim -O2 -Wall -Wextra -I /src -o t /src/test/chord2_test.c\n"
              "bfin-elf-run t | grep -v '^PASS'\n"
              f"bfin-elf-gcc {cflags} -c -o hold.o /src/hold.c\n"
              "bfin-elf-as -I /src --defsym NO_FX=1 -o abi.o /src/test/fx_abi.S\n"
              "bfin-elf-as -I /src -o hooks.o /src/test/chord2_abi.S\n"
              "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -I /src -o m /src/test/chord2_main.c hold.o abi.o hooks.o\n"
              "bfin-elf-run m\n"
              "echo UNDEF; bfin-elf-nm -u hold.o\n")
    r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "op1-bfin",
                        "sh", "-c", script], capture_output=True, text=True)
    out = r.stdout
    print(out.split("UNDEF\n")[0] + r.stderr[-2000:])
    undef = {l.split()[-1] for l in out.split("UNDEF\n", 1)[-1].splitlines() if l.strip()}
    extra = undef - ALLOWED
    print("hold.o undefined symbols beyond TE's:", ", ".join(sorted(extra)) or "none")
    ok = r.returncode == 0 and "FAIL" not in out and out.count("all passed") == 2 and "abi 00000000" in out \
        and not extra and "UNDEF" in out
    print("all passed" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
