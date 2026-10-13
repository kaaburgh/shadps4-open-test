#!/usr/bin/env python3
"""Reads back the numbered frames hle_audio_queue played through SDL's disk
audio driver and reports what reached the device.

usage: analyze.py <disk audio file> <frames emitted> [--max-gap N]

The file holds 32-bit float stereo at the device rate. Each frame the guest
emitted carries its number n >= 1 (left = n & 0x7fff, right = n >> 15, as
S16 / 32768). Zero frames are silence: SDL plays them when the queue runs
dry, and the guest ends with silence.

Reports the frames played, how many were dropped and in how many gaps (frames
missing at the end count as a last gap), the largest gap, and silent runs
between numbered frames (underruns). Fails if a frame is repeated or out of
order, if a value is not a frame number, or if a gap is larger than --max-gap
frames.
"""
from __future__ import annotations

import argparse
from array import array
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("file")
    parser.add_argument("emitted", type=int)
    parser.add_argument("--max-gap", type=int, default=None)
    args = parser.parse_args()

    samples = array("f")
    with open(args.file, "rb") as f:
        samples.frombytes(f.read())
    if sys.byteorder != "little":
        samples.byteswap()

    played = 0
    last = 0
    gaps = []
    bad = []
    underruns = 0
    silent_run = 0
    for i in range(0, len(samples) - 1, 2):
        lo = samples[i] * 32768.0
        hi = samples[i + 1] * 32768.0
        if lo == 0.0 and hi == 0.0:
            silent_run += 1
            continue
        if lo != int(lo) or hi != int(hi) or not (0 <= lo < 32768 and 0 <= hi < 32768):
            bad.append(f"frame {i // 2}: not a frame number ({samples[i]}, {samples[i + 1]})")
            continue
        n = int(lo) | (int(hi) << 15)
        if last and silent_run:
            underruns += 1
        silent_run = 0
        if n <= last:
            bad.append(f"frame {i // 2}: number {n} after {last}")
        elif n != last + 1:
            gaps.append(n - last - 1)
        played += 1
        last = max(last, n)

    tail = args.emitted - last
    if tail > 0:
        gaps.append(tail)
    print(
        f"played={played} emitted={args.emitted} dropped={sum(gaps)} gaps={len(gaps)} "
        f"max_gap={max(gaps, default=0)} tail_missing={tail} underruns={underruns}"
    )
    if gaps:
        print("gap sizes:", " ".join(str(g) for g in gaps[:40]) + (" ..." if len(gaps) > 40 else ""))
    for line in bad[:10]:
        print(line)

    ok = not bad and played > 0
    if args.max_gap is not None and max(gaps, default=0) > args.max_gap:
        ok = False
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
