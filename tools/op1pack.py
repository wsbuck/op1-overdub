#!/usr/bin/env python3
"""Unpack and rebuild OP-1 (original) .op1 firmware images.

Container: u32 CRC-32 (LE, zlib) over the rest | LZMA-alone stream | GNU tar.

  op1pack.py unpack STOCK.op1 OUTDIR
  op1pack.py build  STOCK.op1 OUT.op1 [--replace NAME=PATH ...] [--add NAME=PATH ...]

`build` starts from the stock image and swaps in replacement members, keeping every tar header
field and member order identical. It refuses to touch te-boot.ldr (te-boot reflashes the NAND
boot blocks when its version changes; see notes/firmware-format.md).
`--add` appends a NEW member (e.g. our own content/display/*.svg) after the last stock member of the same
directory, with a header cloned from a sibling (mode, owner, mtime). te-boot's tar extractor writes every member to
/yaffs2/<name> (notes/svg-route.md). New names must be under content/, ASCII, < 100 bytes, and must not contain
FRC, RBT or KEY or start a path component with '.' or '_' (te-boot treats such names as commands, some of which act
irreversibly on the device).
"""
import argparse
import hashlib
import io
import lzma
import os
import sys
import tarfile
import zlib

PROTECTED = {"te-boot.ldr"}
FORBIDDEN_WORDS = ("FRC", "RBT", "KEY")
LZMA_FILTERS = [{"id": lzma.FILTER_LZMA1, "dict_size": 8 << 20, "lc": 3, "lp": 0, "pb": 2}]  # matches stock props
MAX_IMAGE = 15 * 1000 * 1000  # op1repacker comment: device runs out of RAM above ~15 MB (Hypothesis)


def read_image(path):
    data = open(path, "rb").read()
    stored, actual = int.from_bytes(data[:4], "little"), zlib.crc32(data[4:])
    if stored != actual:
        sys.exit(f"{path}: CRC mismatch (stored 0x{stored:08x}, computed 0x{actual:08x})")
    return lzma.decompress(data[4:], format=lzma.FORMAT_ALONE)


def members(tar_bytes):
    """Raw GNU tar walk: [(name, header_bytes, content_or_None)], plus trailing bytes after the last member."""
    out, off = [], 0
    while True:
        hdr = tar_bytes[off:off + 512]
        if hdr == bytes(512):
            return out, tar_bytes[off:]
        name = hdr[:100].rstrip(b"\0").decode()
        typ = hdr[156:157]
        size = int(hdr[124:136].rstrip(b"\0 ") or b"0", 8)
        if typ not in (b"0", b"\0", b"5"):
            sys.exit(f"unexpected tar entry type {typ!r}: {name}")
        if _checksum(hdr) != int(hdr[148:154], 8):
            sys.exit(f"bad tar header checksum: {name}")
        content = tar_bytes[off + 512:off + 512 + size] if typ != b"5" else None
        out.append((name, hdr, content))
        off += 512 + (size + 511) // 512 * 512


def _checksum(hdr):
    return sum(hdr[:148]) + 8 * 32 + sum(hdr[156:512])


def _with_size(hdr, size):
    h = bytearray(hdr)
    h[124:136] = b"%011o\0" % size
    h[148:156] = b"%06o\0 " % _checksum(h)
    return bytes(h)


def write_tar(entries, trailer_len):
    parts = []
    for _, hdr, content in entries:
        parts.append(hdr)
        if content is not None:
            parts.append(content + bytes(-len(content) % 512))
    body = b"".join(parts)
    end = len(body) + 1024
    return body + bytes(end + (-end % 10240) - len(body)) if trailer_len is None else body + bytes(trailer_len)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def cmd_unpack(a):
    tar_bytes = read_image(a.image)
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as t:
        t.extractall(a.outdir, filter="data")
    for name, _, content in members(tar_bytes)[0]:
        if content is not None:
            print(f"{len(content):10d}  {sha(content)[:16]}  {name}")


def cmd_build(a):
    stock_tar = read_image(a.stock)
    entries, trailer = members(stock_tar)
    if write_tar(entries, len(trailer)) != stock_tar:
        sys.exit("tar writer does not reproduce stock tar byte-for-byte; refusing to build")

    repl, add = {}, {}
    names = [n for n, _, _ in entries]
    for spec in a.add or []:
        name, _, path = spec.partition("=")
        parts = name.split("/")
        if (not name.startswith("content/") or name.endswith("/") or len(name.encode()) >= 100
                or not name.isascii() or any(w in name.upper() for w in FORBIDDEN_WORDS)
                or any(c[:1] in (".", "_") for c in parts) or name in names or name in add):
            sys.exit(f"refusing to add {name!r}: must be a new ASCII name under content/ (< 100 B), "
                     f"without {'/'.join(FORBIDDEN_WORDS)} or components starting with '.' or '_'")
        add[name] = open(path, "rb").read()
    repl = {}
    for spec in a.replace or []:
        name, _, path = spec.partition("=")
        if name in PROTECTED:
            sys.exit(f"refusing to replace protected member {name}")
        repl[name] = open(path, "rb").read()

    out = []
    for name, hdr, content in entries:
        if name in repl:
            content = repl[name]
            hdr = _with_size(hdr, len(content))
        out.append((name, hdr, content))
    missing = set(repl) - {n for n, _, _ in entries}
    if missing:
        sys.exit(f"no such member(s) in stock image: {', '.join(sorted(missing))}")
    for name, content in add.items():
        d = name.rsplit("/", 1)[0] + "/"
        sib = [i for i, (n, h, c) in enumerate(out) if n.startswith(d) and c is not None and "/" not in n[len(d):]]
        if not sib:
            sys.exit(f"--add {name}: no stock file in {d} to clone a header from")
        h = bytearray(out[sib[-1]][1])
        h[0:100] = name.encode().ljust(100, b"\0")
        out.insert(sib[-1] + 1, (name, _with_size(bytes(h), len(content)), content))

    tar_bytes = write_tar(out, None if repl or add else len(trailer))
    body = lzma.compress(tar_bytes, format=lzma.FORMAT_ALONE, filters=LZMA_FILTERS)
    image = zlib.crc32(body).to_bytes(4, "little") + body

    # Verify: decode what we wrote and re-check every member against intent.
    check = {n: c for n, _, c in members(decode(image))[0]}
    stock = {n: c for n, _, c in entries}
    if check.keys() != stock.keys() | add.keys():
        sys.exit("verification failed: member list changed")
    for name in stock:
        if check[name] != repl.get(name, stock[name]):
            sys.exit(f"verification failed for {name}")
    for name in add:
        if check[name] != add[name]:
            sys.exit(f"verification failed for added {name}")
    for name in PROTECTED:
        if check[name] != stock[name]:
            sys.exit(f"verification failed: {name} differs from stock")
    if len(image) > MAX_IMAGE:
        sys.exit(f"image is {len(image)} B, above the {MAX_IMAGE} B limit")

    with open(a.out, "wb") as f:
        f.write(image)
    print(f"wrote {a.out}: {len(image)} B, sha256 {sha(image)}")
    for name in sorted(repl):
        print(f"  replaced {name}: {len(stock[name])} -> {len(repl[name])} B, sha256 {sha(repl[name])[:16]}")
    for name in sorted(add):
        print(f"  added {name}: {len(add[name])} B, sha256 {sha(add[name])[:16]}")
    print(f"  te-boot.ldr identical to stock: sha256 {sha(check['te-boot.ldr'])[:16]}")


def decode(image):
    if int.from_bytes(image[:4], "little") != zlib.crc32(image[4:]):
        sys.exit("CRC mismatch in built image")
    return lzma.decompress(image[4:], format=lzma.FORMAT_ALONE)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    u = sub.add_parser("unpack")
    u.add_argument("image")
    u.add_argument("outdir")
    b = sub.add_parser("build")
    b.add_argument("stock")
    b.add_argument("out")
    b.add_argument("--replace", action="append", metavar="NAME=PATH")
    b.add_argument("--add", action="append", metavar="NAME=PATH")
    a = ap.parse_args()
    {"unpack": cmd_unpack, "build": cmd_build}[a.cmd](a)


if __name__ == "__main__":
    main()
