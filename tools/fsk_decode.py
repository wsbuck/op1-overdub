#!/usr/bin/env python3
"""Decode FSK logger frames (src/poc/fsk.inc) from a tape/album AIFF or simulator output.

  fsk_decode.py RECORDING [--words N] [--raw] [-v]

RECORDING: .aif/.aiff/.aifc (OP-1 tape or album), or a text file with one sample per line
(src/poc/test/run.sh output). Stereo is mixed to mono.

Steps: find the 128-sample block alignment, decide each block's bit by energy at 8 vs 16 cycles per
block, find the sync word, check each frame's Fletcher sum, print the frames that pass.
With --words 10 (default) frames are also shown in canarylog.S's layout.
"""
import argparse
import collections
import struct
import sys

import numpy as np

BLOCK = 128
BIN0, BIN1 = 8, 16                 # cycles per block for bit 0 / bit 1
SYNC = 0x1ACFFC1D
BLOCKS = (BLOCK, BLOCK // 2, BLOCK * 2)   # block lengths tried: as sent, 2x fast, 2x slow
SYNC_ERRS = 3                      # sync bits allowed wrong; the check word decides
REGIONS = [0x00800000, 0x01E10000, 0x02C00000, 0x03800000]   # canarylog.S _canary0..3
FS = 44100


def ext80(b):
    """IEEE 754 80-bit extended (AIFF sample rate) -> float."""
    exp, mant = struct.unpack(">HQ", b)
    sign = -1 if exp & 0x8000 else 1
    return 0.0 if mant == 0 else sign * mant * 2.0 ** ((exp & 0x7FFF) - 16383 - 63)


def read_aiff(path):
    data = open(path, "rb").read()
    form, _, kind = struct.unpack(">4sI4s", data[:12])
    if form != b"FORM" or kind not in (b"AIFF", b"AIFC"):
        sys.exit(f"{path}: not an AIFF/AIFC file")
    comm = ssnd = None
    pos = 12
    while pos + 8 <= len(data):
        cid, size = struct.unpack(">4sI", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + size]
        if cid == b"COMM":
            comm = body
        elif cid == b"SSND":
            ssnd = body
        pos += 8 + size + (size & 1)
    if comm is None or ssnd is None:
        sys.exit(f"{path}: missing COMM or SSND chunk")
    channels, _, bits = struct.unpack(">hIh", comm[:8])
    rate = ext80(comm[8:18])
    comp = comm[18:22] if kind == b"AIFC" else b"NONE"
    if comp not in (b"NONE", b"sowt") or bits not in (16, 24):
        sys.exit(f"{path}: unsupported format {comp!r}, {bits}-bit")
    offset = struct.unpack(">I", ssnd[:4])[0]
    raw = ssnd[8 + offset:]
    width = bits // 8
    raw = raw[:len(raw) // (width * channels) * width * channels]
    if bits == 16:
        x = np.frombuffer(raw, dtype="<i2" if comp == b"sowt" else ">i2").astype(np.float64)
    else:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        if comp == b"sowt":
            b = b[:, ::-1]
        x = ((b[:, 0].astype(np.int32) << 24 | b[:, 1].astype(np.int32) << 16
              | b[:, 2].astype(np.int32) << 8) >> 16).astype(np.float64)
    x = x.reshape(-1, channels).mean(axis=1)
    return x, rate, f"AIFF {channels} ch, {bits}-bit, {rate:g} Hz"


def read_samples(path):
    if path.lower().endswith((".aif", ".aiff", ".aifc")):
        return read_aiff(path)
    x = np.loadtxt(path, dtype=np.float64, ndmin=1)
    return x, FS, "text samples"


def block_energies(x, offset, block=BLOCK):
    """Energy at BIN0 and BIN1 for each whole block starting at offset."""
    n = (len(x) - offset) // block
    blocks = x[offset:offset + n * block].reshape(n, block)
    t = np.arange(block)
    basis = np.exp(-2j * np.pi * np.outer(t, [BIN0, BIN1]) / block)
    e = np.abs(blocks @ basis) ** 2
    return e[:, 0], e[:, 1]


def find_alignment(x, block=BLOCK):
    """Offset (0..block-1) where blocks best match the encoder's symbols. Each block starts its square at
    phase 0, so correlation with the exact square templates peaks sharply at the block boundary
    (energy alone barely changes with the shift). Score 1.0 = every loud block is a perfect symbol."""
    t = np.arange(block)
    templates = np.stack([np.where(t * k // (block // 2) % 2 == 0, 1.0, -1.0) for k in (BIN0, BIN1)], 1)
    best = (-1.0, 0)
    for off in range(block):
        n = (len(x) - off) // block
        blocks = x[off:off + n * block].reshape(n, block)
        e = (blocks * blocks).sum(axis=1)
        loud = e > 0.1 * e.max()                 # same blocks at every offset; silence only adds noise
        if loud.any():
            score = np.abs(blocks[loud] @ templates).max(axis=1).sum() / np.sqrt(e[loud] * block).sum()
            best = max(best, (score, off))
    return best[1], best[0]


def find_timing(x):
    """(block length, offset, score) for the time scale that best matches the symbols. The OP-1 tape
    capture came out 2x time-compressed (64-sample blocks: 22.05 kHz audio in a 44.1 kHz file)."""
    return max(((blk, *find_alignment(x, blk)) for blk in BLOCKS), key=lambda r: r[2])


def bits_to_word(bits):
    w = 0
    for b in bits:
        w = w << 1 | int(b)
    return w


def fletcher(words):
    a, b = 1, 0
    for w in words:
        a = (a + w) & 0xFFFFFFFF
        b = (b + a) & 0xFFFFFFFF
    return b


def find_frames(bits, nwords):
    sync = np.array([SYNC >> (31 - i) & 1 for i in range(32)], dtype=np.int8)
    flen = (nwords + 2) * 32
    frames, bad = [], 0
    i = 0
    while i + flen <= len(bits):
        if np.count_nonzero(bits[i:i + 32] != sync) <= SYNC_ERRS:
            words = [bits_to_word(bits[i + 32 * k:i + 32 * k + 32]) for k in range(1, nwords + 2)]
            if fletcher(words[:-1]) == words[-1]:
                frames.append((i, tuple(words[:-1])))
                i += flen
                continue
            bad += 1
        i += 1
    return frames, bad


def canary_view(words):
    mask, sweeps = words[0], words[9]
    lines = [f"  mask 0x{mask:x}, {sweeps} sweeps completed"]
    for r, base in enumerate(REGIONS):
        if mask >> r & 1:
            addr, val = words[1 + 2 * r], words[2 + 2 * r]
            lines.append(f"  region {r} (0x{base:08x}) DAMAGED: first at 0x{addr:08x} "
                         f"(+0x{(addr - base) & 0xFFFFFFFF:x}) = 0x{val:08x}")
        else:
            lines.append(f"  region {r} (0x{base:08x}) clean")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording")
    ap.add_argument("--words", type=int, default=10, help="payload words per frame (default 10)")
    ap.add_argument("--raw", action="store_true", help="hex words only, no canary view")
    ap.add_argument("-v", action="store_true", help="list every frame")
    a = ap.parse_args()

    x, rate, desc = read_samples(a.recording)
    print(f"{a.recording}: {desc}, {len(x)} samples ({len(x) / rate:.2f} s)")
    if abs(rate - FS) > 1:
        print(f"warning: sample rate {rate:g} Hz, encoder assumes {FS} Hz")
    block, off, score = find_timing(x)
    e0, e1 = block_energies(x, off, block)
    bits = (e1 > e0).astype(np.int8)
    if block != BLOCK:
        print(f"note: {block}-sample blocks, so the recording runs at {BLOCK / block:g}x the encoder's speed")
    print(f"alignment offset {off}, symbol match {score:.3f} (1.0 = perfect), {len(bits)} blocks")

    frames, bad = find_frames(bits, a.words)
    print(f"{len(frames)} frames pass the check, {bad} sync matches fail it")
    if not frames:
        return 1
    for pos, words in frames if a.v else []:
        print(f"  t={(off + pos * block) / rate:7.2f}s  " + " ".join(f"{w:08x}" for w in words))

    canary = a.words == 10 and not a.raw
    groups = collections.defaultdict(list)       # canary: group by the record, ignoring sweeps
    for _, words in frames:
        groups[words[:9] if canary else words].append(words)
    for members in sorted(groups.values(), key=len, reverse=True):
        words = members[-1]
        print(f"\n{len(members)}x: " + " ".join(f"{w:08x}" for w in words))
        if canary:
            print(canary_view(words))
    return 0


if __name__ == "__main__":
    sys.exit(main())
