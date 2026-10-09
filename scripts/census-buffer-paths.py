#!/usr/bin/env python3
"""Show which BufferCache path shadPS4 took for guest address ranges.

UMA research builds of shadPS4 write an event capture when
SHADPS4_UMA_E0_CAPTURE names a new directory (schema shadps4-uma-e0/v1). Each
BufferCache::ObtainBuffer call records a Buffer event with the path it chose:
shared backing, the mirror, or the stream buffer. A range that never shows up
was not obtained at all, for example because a DMA took shadPS4's CPU fast
path.

Usage:
    census-buffer-paths.py <capture-dir> <name>=<start>-<end> [...]

Addresses are hexadecimal guest VAs, end exclusive. Prints one line per range,
path, access and size with the number of events.
"""
from __future__ import annotations

import json
from pathlib import Path
import struct
import sys

KIND_BUFFER = 7
PATHS = {0: "mirror", 1: "stream", 3: "shared"}
# position, time, tid, kind|flags<<32, context, session, tick, address, size,
# a (3 = written), b (texel buffer), c (path), cmd_seq
EVENT = struct.Struct("<13Q")


def parse_range(arg: str) -> tuple[str, int, int]:
    name, _, span = arg.partition("=")
    start, _, end = span.partition("-")
    if not name or not start or not end:
        raise SystemExit(f"bad range {arg!r}; expected name=start-end")
    return name, int(start, 16), int(end, 16)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    capture = Path(sys.argv[1])
    ranges = [parse_range(arg) for arg in sys.argv[2:]]

    metadata = json.loads((capture / "metadata.json").read_text(encoding="utf-8"))
    if metadata.get("schema") != "shadps4-uma-e0/v1":
        print(f"unexpected capture schema {metadata.get('schema')!r}", file=sys.stderr)
        return 2
    record = int(metadata["record_bytes"])
    if record < EVENT.size:
        print(f"record_bytes {record} is smaller than {EVENT.size}", file=sys.stderr)
        return 2

    counts: dict[tuple[str, str, str, int], int] = {}
    data = (capture / "events.bin").read_bytes()
    for offset in range(0, len(data) - record + 1, record):
        fields = EVENT.unpack_from(data, offset)
        if fields[3] & 0xFFFFFFFF != KIND_BUFFER:
            continue
        address, size, access, path = fields[7], fields[8], fields[9], fields[11]
        for name, start, end in ranges:
            if address < end and address + size > start:
                key = (name, PATHS.get(path, str(path)), "write" if access == 3 else "read", size)
                counts[key] = counts.get(key, 0) + 1

    for name, start, end in ranges:
        lines = [(k, n) for k, n in counts.items() if k[0] == name]
        if not lines:
            print(f"{name}: not obtained")
        for (_, path, access, size), n in sorted(lines):
            print(f"{name}: {path} {access} size=0x{size:x} events={n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
