#!/usr/bin/env python3
"""Add plaintext code/data blocks to OP1_vdk.ldr, just before the FINAL block.

  ldr_patch.py STOCK_OP1_vdk.ldr OUT.ldr --block 0x019B99D4=code.bin [--block ADDR=FILE ...]
               [--within LO-HI] [--fill LO-HI=PATTERN ...] [--pointer ADDR=OLD:NEW ...]
               [--half ADDR=OLD:NEW ...] [--reserve LO-HI=WHAT ...]

Safety checks (PROMPT.md rule 1; see notes/memory-map.md):
  * every new block must lie entirely inside memory the stock app already loads with a plaintext,
    non-fill block (so we only overwrite TE-defined plaintext bytes), or entirely inside one FREE region
    (SDRAM the stock app does not load and that a canary probe showed untouched at runtime)
  * --pointer rewrites one stock plaintext 32-bit word (e.g. a vtable slot) and refuses unless it
    currently holds OLD and NEW lies inside a block of this patch
  * --half rewrites one stock plaintext 16-bit halfword (an instruction or its immediate) and refuses
    unless it currently holds OLD. Halves are merged into aligned 32-bit words (the other bytes kept
    from stock), because the boot ROM loads blocks with 32-bit DMA (DMACODE 6)
  * every block must start 4-byte aligned and be a multiple of 4 bytes, like every stock block: a
    2-byte block made SLOT1.OP1 fail to boot (notes/LOG.md 2026-10-07). --block payloads in a FREE
    region are zero-padded to a multiple of 4; elsewhere a misaligned block is refused
  * optional --within: every block must also lie inside LO-HI (e.g. the function being replaced)
  * no block may touch the ranges te-boot's init/ROM use while the app is being loaded
  * new blocks must not overlap each other, nor any --reserve range: memory our code uses at runtime but the
    image does not load (e.g. a delay line in a FREE region). Reserves must lie inside one FREE region and
    not overlap each other. Without this, a code block that grows into a buffer is overwritten at runtime
  * --fill (FILL block, 32-bit pattern, no payload) is the only way to write memory the stock app does
    not load. It must be in SDRAM, must not overlap any stock block, and is used for probes (canaries)
Prints every address range the output image writes that differs from stock.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr  # noqa: E402

FORBIDDEN = [
    (0xFFA10000, 0xFFA10128, "te-boot XTEA callback (live during app load)"),
    (0xFF804000, 0xFF806000, "te-boot init state / XTEA key"),
    (0xFFB00000, 0xFFB01000, "L1 scratchpad (ROM boot state; engine objects)"),
]
SDRAM = (0x00000000, 0x04000000)
# Note: SDRAM below 0x01000000 is te-boot's when the COM menu runs, but on a normal boot only te-boot's
# init DXE runs and it is L1-only (te-boot.ldr blocks 1-3), so nothing there is live while the ROM
# loads the app. See notes/memory-map.md.
# Verified free at runtime (CANARYLOG, notes/LOG.md 2026-10-06): 1 MB each, fw 246. Region 3
# (0x03800000) was damaged and is not listed.
FREE = [
    (0x00800000, 0x00900000, "canary region 0"),
    (0x01E10000, 0x01F10000, "canary region 1"),
    (0x02C00000, 0x02D00000, "canary region 2"),
]
BLOCK_FLAGS = 0x0006  # plain DMA block, same as stock plaintext blocks (DMACODE 6)


def parse_range(s):
    lo, hi = s.split("-")
    return int(lo, 0), int(hi, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stock")
    ap.add_argument("out")
    ap.add_argument("--block", action="append", required=True, metavar="ADDR=FILE")
    ap.add_argument("--within", type=parse_range, metavar="LO-HI")
    ap.add_argument("--fill", action="append", default=[], metavar="LO-HI=PATTERN")
    ap.add_argument("--pointer", action="append", default=[], metavar="ADDR=OLD:NEW")
    ap.add_argument("--half", action="append", default=[], metavar="ADDR=OLD:NEW")
    ap.add_argument("--reserve", action="append", default=[], metavar="LO-HI=WHAT")
    a = ap.parse_args()

    blocks = ldr.parse(open(a.stock, "rb").read())
    finals = [b for b in blocks if b.flags & ldr.F_FINAL]
    if len(finals) != 1:
        sys.exit("expected exactly one FINAL block")
    final_idx = blocks.index(finals[0])
    plain = [(b.target, b.target + b.count) for b in blocks
             if b.loads_memory and not b.is_fill and not b.is_encrypted]

    new = []
    for spec in a.block:
        addr_s, _, path = spec.partition("=")
        addr, payload = int(addr_s, 0), open(path, "rb").read()
        if any(flo <= addr < fhi for flo, fhi, _ in FREE):
            payload += bytes(-len(payload) % 4)
        lo, hi = addr, addr + len(payload)
        if not payload or len(payload) % 4 or addr % 4:
            sys.exit(f"{path}: block at 0x{addr:08x} ({len(payload)} B) must be 4-byte aligned and sized")
        free = next((why for flo, fhi, why in FREE if flo <= lo and hi <= fhi), None)
        if not free and not any(plo <= lo and hi <= phi for plo, phi in plain):
            sys.exit(f"0x{lo:08x}-0x{hi:08x} is not inside a single stock plaintext block or FREE region")
        for flo, fhi, why in FORBIDDEN:
            if lo < fhi and flo < hi:
                sys.exit(f"0x{lo:08x}-0x{hi:08x} overlaps forbidden range ({why})")
        if a.within and not (a.within[0] <= lo and hi <= a.within[1]):
            sys.exit(f"0x{lo:08x}-0x{hi:08x} outside --within 0x{a.within[0]:08x}-0x{a.within[1]:08x}")
        new.append(ldr.Block(0, 0, BLOCK_FLAGS, addr, len(payload), 0, payload))

    code = [(b.target, b.target + b.count) for b in new]
    reserves = []
    for spec in a.reserve:
        rng, _, what = spec.partition("=")
        lo, hi = parse_range(rng)
        if not any(flo <= lo < hi <= fhi for flo, fhi, _ in FREE):
            sys.exit(f"reserve 0x{lo:08x}-0x{hi:08x} ({what}) is not inside a single FREE region")
        reserves.append((lo, hi, what or "reserve"))
    spans = [(lo, hi, f"block 0x{lo:08x}") for lo, hi in code] + reserves
    for i, (lo, hi, w1) in enumerate(spans):
        for lo2, hi2, w2 in spans[i + 1:]:
            if lo < hi2 and lo2 < hi:
                sys.exit(f"0x{lo:08x}-0x{hi:08x} ({w1}) overlaps 0x{lo2:08x}-0x{hi2:08x} ({w2})")

    pointers = set()

    def stock_value(addr, size, what):
        src = next((b for b in blocks if b.loads_memory and not b.is_fill and not b.is_encrypted
                    and b.target <= addr and addr + size <= b.target + b.count), None)
        if src is None or addr % size:
            sys.exit(f"{what} 0x{addr:08x}: not an aligned {size}-byte value in a stock plaintext block")
        return int.from_bytes(src.payload[addr - src.target:addr - src.target + size], "little")

    for spec in a.pointer:
        addr_s, _, vals = spec.partition("=")
        addr, (old, val) = int(addr_s, 0), (int(v, 0) for v in vals.split(":"))
        cur = stock_value(addr, 4, "pointer")
        if cur != old:
            sys.exit(f"pointer 0x{addr:08x} holds 0x{cur:08x}, expected 0x{old:08x}")
        if not any(lo <= val < hi for lo, hi in code):
            sys.exit(f"pointer 0x{addr:08x}: new value 0x{val:08x} is not inside a --block of this patch")
        new.append(ldr.Block(0, 0, BLOCK_FLAGS, addr, 4, 0, val.to_bytes(4, "little")))
        pointers.add(addr)
    words = {}                                   # aligned word address -> patched bytes
    for spec in a.half:
        addr_s, _, vals = spec.partition("=")
        addr, (old, val) = int(addr_s, 0), (int(v, 0) for v in vals.split(":"))
        cur = stock_value(addr, 2, "half")
        if cur != old:
            sys.exit(f"half 0x{addr:08x} holds 0x{cur:04x}, expected 0x{old:04x}")
        w = addr & ~3
        word = words.setdefault(w, bytearray(stock_value(w, 4, "half").to_bytes(4, "little")))
        word[addr - w:addr - w + 2] = val.to_bytes(2, "little")
    run = None
    for w in sorted(words):                      # one block per run of consecutive words
        if run and run.target + run.count == w:
            run.payload += bytes(words[w])
            run.count += 4
        else:
            run = ldr.Block(0, 0, BLOCK_FLAGS, w, 4, 0, bytes(words[w]))
            new.append(run)

    loaded = [(b.target, b.target + b.count) for b in blocks if b.loads_memory]
    for spec in a.fill:
        rng, _, pat = spec.partition("=")
        lo, hi = parse_range(rng)
        pattern = int(pat, 0)
        if not (SDRAM[0] <= lo < hi <= SDRAM[1]) or (hi - lo) % 4:
            sys.exit(f"fill 0x{lo:08x}-0x{hi:08x}: must be a word-multiple range inside SDRAM")
        for llo, lhi in loaded:
            if lo < lhi and llo < hi:
                sys.exit(f"fill 0x{lo:08x}-0x{hi:08x} overlaps stock-loaded 0x{llo:08x}-0x{lhi:08x}")
        for flo, fhi, why in FORBIDDEN:
            if lo < fhi and flo < hi:
                sys.exit(f"fill 0x{lo:08x}-0x{hi:08x} overlaps forbidden range ({why})")
        new.append(ldr.Block(0, 0, BLOCK_FLAGS | ldr.F_FILL, lo, hi - lo, pattern, b""))
    for b in new:
        if b.target % 4 or b.count % 4:
            sys.exit(f"internal error: block 0x{b.target:08x} ({b.count} B) not 4-byte aligned")

    # FIRST block argument = byte offset from end of its header to the next DXE (here: end of stream).
    first = blocks[0]
    stock_len = sum(len(b.serialize()) for b in blocks)
    if not (first.flags & ldr.F_FIRST) or first.argument != stock_len - 16:
        sys.exit("unexpected FIRST block argument; refusing to patch")
    first.argument += sum(16 + len(b.payload) for b in new)

    out = blocks[:final_idx] + new + blocks[final_idx:]
    data = ldr.serialize(out)
    check = ldr.parse(data)
    if check[final_idx].target != new[0].target or check[0].argument != len(data) - 16:
        sys.exit("internal error: block placement / FIRST argument")
    with open(a.out, "wb") as f:
        f.write(data)

    print(f"wrote {a.out}: {len(data)} B ({len(new)} block(s) inserted before FINAL block #{final_idx})")
    print("RAM ranges changed vs stock (written by the boot ROM when loading the app):")
    for b in new:
        what = (f"FILL 0x{b.argument:08x}" if b.is_fill else
                f"pointer -> 0x{int.from_bytes(b.payload, 'little'):08x}" if b.target in pointers else
                f"patched words {b.payload.hex()}" if b.target in words else "code/data")
        what += next((f", FREE: {why}" for flo, fhi, why in FREE if flo <= b.target < fhi), "")
        print(f"  0x{b.target:08x}-0x{b.target + b.count - 1:08x} ({b.count} B) {ldr.region_of(b.target)}, {what}")
    print("NAND: app occupies blocks 4..%d (te-boot blocks 0-3 untouched)" % (4 + (len(data) - 1) // 131072))


if __name__ == "__main__":
    main()
