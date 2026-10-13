#!/usr/bin/env python3
"""Recording-path check for tools/fsk_decode.py: simulator samples -> AIFF as tape might give it
(off block alignment, lower level, noise, leading silence, stereo, and 2x time-compressed as the
first device capture was) -> decode -> compare payloads.

  fsk_roundtrip.py SIM_SAMPLES.txt [--words N]
"""
import argparse
import os
import struct
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../tools"))
import fsk_decode as fd  # noqa: E402


def write_aiff(path, x, rate=44100):
    """16-bit stereo AIFF; x is mono, duplicated to both channels."""
    pcm = np.repeat(np.clip(np.round(x), -32768, 32767).astype(">i2"), 2).tobytes()
    exp, mant = 16383 + 63, int(rate)
    while mant < 1 << 63:
        mant, exp = mant << 1, exp - 1
    comm = struct.pack(">hIh", 2, len(x), 16) + struct.pack(">HQ", exp, mant)
    ssnd = struct.pack(">II", 0, 0) + pcm
    body = b"AIFF" + b"COMM" + struct.pack(">I", len(comm)) + comm + b"SSND" + struct.pack(">I", len(ssnd)) + ssnd
    open(path, "wb").write(b"FORM" + struct.pack(">I", len(body)) + body)


def decode(x, words):
    block, off, _ = fd.find_timing(x)
    e0, e1 = fd.block_energies(x, off, block)
    return block, off, fd.find_frames((e1 > e0).astype(np.int8), words)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("--words", type=int, default=10)
    a = ap.parse_args()
    clean = np.loadtxt(a.samples)
    _, _, ref = decode(clean, a.words)
    assert ref, "no frames in the clean simulator output"
    rng = np.random.default_rng(1)
    with tempfile.TemporaryDirectory() as d:
        for speed in (1, 2):
            for lead in (0, 1, 37, 64, 127):
                x = np.concatenate([np.zeros(4410 + lead), clean * 0.4])
                x = x[:len(x) // speed * speed].reshape(-1, speed).mean(axis=1)   # crude 2x decimation
                x = x + rng.normal(0, 300, len(x))
                write_aiff(os.path.join(d, "t.aif"), x)
                y, rate, _ = fd.read_aiff(os.path.join(d, "t.aif"))
                block, off, got = decode(y, a.words)
                ok = ([w for _, w in got] == [w for _, w in ref] and block == fd.BLOCK // speed
                      and abs(off - (4410 + lead) / speed % block) <= 0.5)
                print(f"speed {speed}x, lead {4410 + lead:5d}: block {block:3d}, offset {off:3d}, "
                      f"{len(got)}/{len(ref)} frames {'ok' if ok else 'MISMATCH'}")
                if not ok:
                    return 1
    print("roundtrip ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
