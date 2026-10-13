#!/usr/bin/env python3
"""Convert an LDR boot stream to a Blackfin ELF (EM_BLACKFIN=106) with one PT_LOAD/section per
memory span, so Ghidra and objdump load every byte at its runtime address.

  ldr2elf.py IN.ldr OUT.elf [--raw]

Encrypted (XTEA) blocks are decrypted with the key from the op1dumps OTP dump (ldr.otp_key) when it is
present. With --raw, or without the dump, they stay encrypted and their spans get an `_enc` suffix: in
those, bytes 0-7 of each 24-byte chunk are ciphertext and bytes 8-23 are plaintext.

Sections are named after their region and address, e.g. `sdram_01000000`, `l1i_ffa00000`.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr  # noqa: E402
from ldr_parse import memory_spans  # noqa: E402

EM_BLACKFIN = 106
PF_X, PF_W, PF_R = 1, 2, 4
SHT_PROGBITS, SHT_STRTAB = 1, 3
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4


def short_region(addr):
    r = ldr.region_of(addr)
    if r.startswith("SDRAM"):
        return "sdram"
    if r.startswith("L1 instr"):
        return "l1i"
    if r.startswith("L1 data A"):
        return "l1da"
    if r.startswith("L1 data B"):
        return "l1db"
    return "mem"


def build_elf(blocks):
    spans = memory_spans(blocks)
    entry = next((b.target for b in blocks if b.flags & ldr.F_FINAL), 0)
    names = []
    for s in spans:
        n = f"{short_region(s['start'])}_{s['start']:08x}"
        if any(s["enc"]):
            n += "_enc"
        names.append(n)

    shstrtab = b"\0"
    name_off = []
    for n in names + [".shstrtab"]:
        name_off.append(len(shstrtab))
        shstrtab += n.encode() + b"\0"

    ehsize, phentsize, shentsize = 52, 32, 40
    phoff = ehsize
    data_off = phoff + phentsize * len(spans)
    offsets, cur = [], data_off
    for s in spans:
        cur = (cur + 3) & ~3
        offsets.append(cur)
        cur += len(s["data"])
    shstr_off = cur
    shoff = (shstr_off + len(shstrtab) + 3) & ~3
    nsec = len(spans) + 2  # null + spans + shstrtab

    out = bytearray()
    out += b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    out += struct.pack("<HHIIIIIHHHHHH", 2, EM_BLACKFIN, 1, entry, phoff, shoff, 0,
                       ehsize, phentsize, len(spans), shentsize, nsec, nsec - 1)
    for s, off in zip(spans, offsets):
        size = len(s["data"])
        out += struct.pack("<IIIIIIII", 1, off, s["start"], s["start"], size, size, PF_R | PF_W | PF_X, 4)
    for s, off in zip(spans, offsets):
        out += bytes(off - len(out))
        out += s["data"]
    out += bytes(shstr_off - len(out)) + shstrtab
    out += bytes(shoff - len(out))
    out += bytes(shentsize)
    for i, (s, off) in enumerate(zip(spans, offsets)):
        out += struct.pack("<IIIIIIIIII", name_off[i], SHT_PROGBITS, SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR,
                           s["start"], off, len(s["data"]), 0, 0, 4, 0)
    out += struct.pack("<IIIIIIIIII", name_off[-1], SHT_STRTAB, 0, 0, shstr_off, len(shstrtab), 0, 0, 1, 0)
    return bytes(out), list(zip(names, spans))


def main():
    args = [a for a in sys.argv[1:] if a != "--raw"]
    if len(args) != 2:
        sys.exit(__doc__)
    blocks = ldr.parse(open(args[0], "rb").read())
    if "--raw" not in sys.argv and os.path.exists(ldr.OTP_DUMP):
        blocks = ldr.decrypt(blocks, ldr.otp_key())
        print(f"decrypted {sum(b.plain for b in blocks)} blocks")
    elf, secs = build_elf(blocks)
    with open(args[1], "wb") as f:
        f.write(elf)
    for n, s in secs:
        print(f"{n:<24} 0x{s['start']:08x}-0x{s['end']:08x} {s['end'] - s['start']:9d} B")


if __name__ == "__main__":
    main()
