#!/usr/bin/env python3
"""List, summarize, verify and extract Blackfin LDR boot streams.

  ldr_parse.py FILE                    list every block
  ldr_parse.py FILE --summary          bytes loaded per memory region
  ldr_parse.py FILE --extract DIR      write merged memory images + manifest.json (for Ghidra/objdump)
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr  # noqa: E402


def block_bytes(b):
    if b.is_fill:
        return struct.pack("<I", b.argument) * (b.count // 4) + struct.pack("<I", b.argument)[: b.count % 4]
    return b.payload


def memory_spans(blocks):
    """Merge every memory-loading block into contiguous spans, applied in stream order (later wins)."""
    ivs = sorted((b.target, b.target + b.count) for b in blocks if b.loads_memory)
    merged = []
    for lo, hi in ivs:
        if merged and lo <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    spans = [{"start": lo, "end": hi, "data": bytearray(hi - lo), "enc": bytearray(hi - lo), "blocks": []}
             for lo, hi in merged]
    for b in blocks:
        if not b.loads_memory:
            continue
        s = next(s for s in spans if s["start"] <= b.target < s["end"])
        o = b.target - s["start"]
        s["data"][o:o + b.count] = block_bytes(b)
        s["enc"][o:o + b.count] = (b"\1" if b.is_encrypted else b"\0") * b.count
        s["blocks"].append(b.index)
    return spans


def runs(mask, base):
    out, start = [], None
    for i, v in enumerate(mask):
        if v and start is None:
            start = i
        elif not v and start is not None:
            out.append([base + start, base + i])
            start = None
    if start is not None:
        out.append([base + start, base + len(mask)])
    return out


def cmd_list(blocks):
    print(f"{'#':>4} {'offset':>8} {'flags':>6} {'names':<28} {'target':>10} {'count':>9} {'argument':>10}  region")
    for b in blocks:
        print(f"{b.index:4d} {b.offset:8x} {b.flags:6x} {b.flag_names():<28} {b.target:10x} {b.count:9d} "
              f"{b.argument:10x}  {ldr.region_of(b.target)}{'  ENC' if b.is_encrypted else ''}")


def cmd_summary(blocks):
    by = {}
    for b in blocks:
        if not b.loads_memory:
            continue
        r = by.setdefault(ldr.region_of(b.target), {"blocks": 0, "data": 0, "fill": 0, "enc": 0,
                                                    "lo": b.target, "hi": b.target + b.count})
        r["blocks"] += 1
        r["fill" if b.is_fill else "enc" if b.is_encrypted else "data"] += b.count
        r["lo"], r["hi"] = min(r["lo"], b.target), max(r["hi"], b.target + b.count)
    print(f"{'region':<22} {'blocks':>6} {'data':>10} {'fill':>10} {'encrypted':>10}  extent")
    for name, r in sorted(by.items(), key=lambda kv: kv[1]["lo"]):
        print(f"{name:<22} {r['blocks']:6d} {r['data']:10d} {r['fill']:10d} {r['enc']:10d}  "
              f"0x{r['lo']:08x}-0x{r['hi']:08x}")
    special = [b for b in blocks if b.flags & (ldr.F_FIRST | ldr.F_FINAL | ldr.F_INIT | ldr.F_IGNORE)]
    print("\ncontrol blocks:")
    for b in special:
        print(f"  #{b.index} {b.flag_names()} target=0x{b.target:x} count={b.count} arg=0x{b.argument:x}")


def cmd_extract(blocks, outdir):
    os.makedirs(outdir, exist_ok=True)
    manifest = []
    for s in memory_spans(blocks):
        name = f"mem_{s['start']:08x}.bin"
        with open(os.path.join(outdir, name), "wb") as f:
            f.write(s["data"])
        manifest.append({"file": name, "start": s["start"], "end": s["end"],
                         "region": ldr.region_of(s["start"]), "blocks": s["blocks"],
                         "encrypted_ranges": runs(s["enc"], s["start"])})
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    for m in manifest:
        enc = sum(hi - lo for lo, hi in m["encrypted_ranges"])
        print(f"{m['file']}  0x{m['start']:08x}-0x{m['end']:08x}  {m['end'] - m['start']:9d} B  "
              f"{m['region']}{f'  ({enc} B encrypted)' if enc else ''}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--extract", metavar="DIR")
    a = ap.parse_args()
    data = open(a.file, "rb").read()
    blocks = ldr.parse(data)
    if ldr.serialize(blocks) != data:
        sys.exit("round-trip mismatch: serializer does not reproduce the input byte-for-byte")
    if a.extract:
        cmd_extract(blocks, a.extract)
    elif a.summary:
        cmd_summary(blocks)
    else:
        cmd_list(blocks)


if __name__ == "__main__":
    main()
