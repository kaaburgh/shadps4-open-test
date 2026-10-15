#!/usr/bin/env python3
"""Writes a lossless H.264 MP4 of the hle_avplayer_frame test pattern.

usage: make_video.py <out.mp4> <width> <height> [frames]

Builds raw YUV 4:2:0 frames with the pattern src/main.c checks and encodes
them with ffmpeg/libx264 at qp 0, which is lossless.
"""
from __future__ import annotations

import subprocess
import sys


def frame(width: int, height: int) -> bytes:
    y = bytes(16 + (x * 7 + row * 13) % 220 for row in range(height) for x in range(width))
    cw, ch = width // 2, height // 2
    u = bytes(16 + (cx * 5 + cy * 3 + 17) % 224 for cy in range(ch) for cx in range(cw))
    v = bytes(16 + (cx * 11 + cy * 19 + 101) % 224 for cy in range(ch) for cx in range(cw))
    return y + u + v


def main() -> int:
    if len(sys.argv) not in (4, 5):
        print(__doc__, file=sys.stderr)
        return 2
    out, width, height = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    frames = int(sys.argv[4]) if len(sys.argv) == 5 else 3
    if width % 2 or height % 2:
        print("width and height must be even for 4:2:0", file=sys.stderr)
        return 2
    raw = frame(width, height) * frames
    subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p",
         "-s", f"{width}x{height}", "-r", "30", "-i", "-", "-c:v", "libx264", "-qp", "0",
         "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-movflags", "+faststart", out],
        input=raw, check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
