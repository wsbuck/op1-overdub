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
4. STRUM's thread rule, statically (the host test checks it dynamically, chord2_test.c t_strum): in chord2_impl.h
   only these functions call TE's voice control (rel_note, te_note_on, te_note_off), each with its guard:
   c2_out (sends only while owner == 0), c2_take_back (the KeyHandler right after it set owner = 0), c2_seq_service
   (the sequencer thread), c2_keydown / c2_keyup (a key that is not a synth keyboard note: TE's path as stock; an
   untracked key-up only while owner == 0). Every other place goes through c2_out.
"""
import re
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = os.path.dirname(HERE)
ALLOWED = {"_feature_mask", "_te_make_note", "_te_note_on", "_te_note_off", "_te_synth_shiftkey", "_te_synth_press",
           "_te_synth_release", "_te_synth_knob", "_te_yaffs_open", "_te_yaffs_read", "_te_yaffs_write",
           "_te_yaffs_close", "_te_yaffs_mkdir", "_te_tempo_spb",
           "_rep_key_event"}                      # weak: the repeat FX's manual key (repeat.c), 0 when not linked


VOICE_CALLS = {
    "c2_out": [r"if \(!c2\.owner\) \{\s*rel_note\(r - REL_OFS, on\);\s*return;"],
    "c2_take_back": [],
    "c2_seq_service": [r"if \(!o \|\| VOICECTL_LOCKED\)\s*return;"],
    "c2_keydown": [r"if \(key < 0\) \{\s*te_note_on\(note, 0\);\s*return;"],
    "c2_keyup": [r"if \(key < 0\) \{\s*te_note_off\(note\);\s*return;",
                 r"if \(c2\.owner\)\s*c2\.raw_off\[key \+ REL_OFS\] = 1;[^\n]*\n\s*else\s*te_note_off\(note\);"],
}


def voice_audit():
    """Functions in chord2_impl.h that call TE's voice control, and their guards."""
    src = open(os.path.join(SLOT, "chord2_impl.h")).read()
    bodies = {}
    for m in re.finditer(r"^(?:static |void |int )[^\n(]*?\b(\w+)\(([^)]*)\)\n\{\n(.*?)^\}", src, re.S | re.M):
        bodies[m.group(1)] = m.group(3)
    callers = {f for f, b in bodies.items() if re.search(r"\b(rel_note|te_note_on|te_note_off)\(", b)}
    ok = callers == set(VOICE_CALLS)
    print(("PASS" if ok else "FAIL") + f" voice control called only from {sorted(VOICE_CALLS)} (found {sorted(callers)})")
    for f, pats in VOICE_CALLS.items():
        for p in pats:
            hit = f in bodies and re.search(p, bodies[f])
            ok &= bool(hit)
            print(("PASS" if hit else "FAIL") + f" {f}: guard /{p[:50]}.../")
    take = bodies.get("c2_owner_update", "")
    hit = re.search(r"c2\.owner = 0;\s*c2_take_back\(\);", take) and "c2_take_back()" not in \
        "".join(b for f, b in bodies.items() if f != "c2_owner_update")
    ok &= bool(hit)
    print(("PASS" if hit else "FAIL") + " c2_take_back runs only from c2_owner_update, right after owner = 0")
    return ok


def main():
    audit = voice_audit()
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
        and not extra and "UNDEF" in out and audit
    print("all passed" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
