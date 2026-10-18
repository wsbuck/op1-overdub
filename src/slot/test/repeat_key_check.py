#!/usr/bin/env python3
"""The repeat FX's manual key (the green encoder's push; src/slot/repeat.h) on the Blackfin simulator.

  python3 src/slot/test/repeat_key_check.py

repeat_key_main.c with repeat.c, pingpong.c (the lines' owner word), the entries as build_slot.py instantiates them
(repeat_key_abi.S) and fx_abi.S's register harness: every key through the drum and mixer controllers' press / release
entries goes to TE's function with the same arguments and return value, registers kept, unless it is green's press on
that context's repeat screen (taken; its release clears and still reaches TE); a stale screen gives the key back to
TE; the counters; the DSP's latch on blue's grid line while held, dry within a crossfade on release or when the screen
stops drawing. The synth controller's slots are CHORD v2's c2_press / c2_release (hold_impl.h REP_KEY; chord2_check.py
covers their pass-through).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
flags = "-std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror"
script = "\n".join(["set -e; cd /tmp",
                    f"bfin-elf-gcc {flags} -c -o r.o /src/repeat.c",
                    f"bfin-elf-gcc {flags} -c -o p.o /src/pingpong.c",
                    "bfin-elf-as -I /src -o k.o /src/test/repeat_key_abi.S",
                    "bfin-elf-as -I /src --defsym NO_FX=1 -o a.o /src/test/fx_abi.S",
                    "bfin-elf-gcc -std=gnu99 -msim -O1 -Wall -I /src -o t /src/test/repeat_key_main.c r.o p.o k.o a.o "
                    "/src/test/pingpong_ram.c -lm",
                    "bfin-elf-run /tmp/t"])
r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SLOT}:/src:ro", "op1-bfin", "sh", "-c",
                    script], capture_output=True, text=True)
print(r.stdout + r.stderr[-3000:])
sys.exit(0 if r.returncode == 0 and "FAIL" not in r.stdout and "all passed" in r.stdout else 1)
