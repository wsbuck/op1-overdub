#!/usr/bin/env python3
"""Inspect an FSK logger recording block by block (diagnostics for tools/fsk_decode.py).

  fsk_inspect.py RECORDING [--block N] [--samples K] [--top T]

RECORDING: .aif/.aiff/.aifc (OP-1 tape or album) or simulator text output (src/poc/test/run.sh).

Prints, for the loud 128-sample blocks at the decoder's alignment:
  * how much of each block's energy lies in a valid symbol (8 or 16 cycles/block plus harmonics)
  * the strongest frequency bins, in cycles per block
  * the fraction of blocks the decoder would read as bit 1
  * the closest match to the sync word anywhere in the bit stream
  * the raw samples of one loud block, to compare with fsk.inc's output (square wave at +-8192)
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsk_decode as fd  # noqa: E402

# Square-wave harmonics (odd multiples) of each symbol frequency, below Nyquist (64 cycles/block).
BIT0_SERIES = [fd.BIN0 * k for k in (1, 3, 5, 7)]   # 8, 24, 40, 56
BIT1_SERIES = [fd.BIN1 * k for k in (1, 3)]         # 16, 48


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording")
    ap.add_argument("--block", type=int, default=200, help="which loud block to dump (default 200)")
    ap.add_argument("--samples", type=int, default=40, help="samples to print from that block (default 40)")
    ap.add_argument("--top", type=int, default=8, help="number of strongest bins to list (default 8)")
    a = ap.parse_args()

    samples, rate, desc = fd.read_samples(a.recording)
    print(f"{a.recording}: {desc}, {len(samples)} samples ({len(samples) / rate:.2f} s)")

    offset, match = fd.find_alignment(samples)
    n_blocks = (len(samples) - offset) // fd.BLOCK
    blocks = samples[offset:offset + n_blocks * fd.BLOCK].reshape(n_blocks, fd.BLOCK)
    print(f"alignment offset {offset}, symbol match {match:.3f}, {n_blocks} blocks")

    rms = np.sqrt((blocks ** 2).mean(axis=1))
    loud = rms > 0.1 * rms.max()
    loud_idx = np.nonzero(loud)[0]
    if not loud_idx.size:
        sys.exit("no signal found")
    print(f"loud blocks: {loud_idx.size} (blocks {loud_idx[0]}..{loud_idx[-1]}), peak |sample| {np.abs(samples).max():.0f}")

    # Power spectrum per loud block; bin k = k cycles per 128-sample block.
    spectrum = np.abs(np.fft.rfft(blocks[loud], axis=1)) ** 2
    total = spectrum.sum(axis=1)
    in_bit0 = spectrum[:, BIT0_SERIES].sum(axis=1)
    in_bit1 = spectrum[:, BIT1_SERIES].sum(axis=1)
    symbol_fraction = np.maximum(in_bit0, in_bit1) / total
    print(f"energy in a valid symbol: median {np.median(symbol_fraction):.2f}, "
          f"10th percentile {np.percentile(symbol_fraction, 10):.2f}  (1.00 = clean symbols)")

    strongest = np.argsort(spectrum.mean(axis=0))[::-1][:a.top]
    print(f"strongest bins (cycles/block): {strongest.tolist()}  (symbols: bit 0 = {fd.BIN0}, bit 1 = {fd.BIN1})")

    e0, e1 = fd.block_energies(samples, offset)
    bits = (e1 > e0).astype(np.int8)
    print(f"blocks read as bit 1: {bits[loud].mean():.2f} of loud blocks")

    sync = np.array([fd.SYNC >> (31 - i) & 1 for i in range(32)], dtype=np.int8)
    distances = [np.count_nonzero(bits[i:i + 32] != sync) for i in range(len(bits) - 32)]
    best = int(np.argmin(distances))
    print(f"closest sync match: {distances[best]} of 32 bits wrong, at block {best}  "
          f"(decoder accepts <= {fd.SYNC_ERRS})")

    k = loud_idx[min(a.block, loud_idx.size - 1)]
    print(f"block {k}, first {a.samples} samples: {blocks[k, :a.samples].astype(int).tolist()}")


if __name__ == "__main__":
    main()
