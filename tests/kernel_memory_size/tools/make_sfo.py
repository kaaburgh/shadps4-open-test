#!/usr/bin/env python3
"""Writes a minimal param.sfo for a made-up title that supports PS4 Pro mode.

usage: make_sfo.py <out.sfo>

shadPS4 runs a title in PS4 Pro (neo) mode only when its settings ask for
it and the title's ATTRIBUTE has bit 23 (support_neo_mode) set.
"""
from __future__ import annotations

from pathlib import Path
import struct
import sys

TEXT = 0x0204
INTEGER = 0x0404
SUPPORT_NEO_MODE = 1 << 23


def sfo(entries: list[tuple[str, str | int]]) -> bytes:
    """PSF 1.1 with UTF-8 text and integer entries, keys in sorted order."""
    entries = sorted(entries)
    keys = b""
    data = b""
    index = b""
    for key, value in entries:
        if isinstance(value, int):
            fmt, raw = INTEGER, struct.pack("<I", value)
        else:
            fmt, raw = TEXT, value.encode("utf-8") + b"\0"
        max_len = (len(raw) + 3) & ~3
        index += struct.pack("<HHIII", len(keys), fmt, len(raw), max_len, len(data))
        keys += key.encode("ascii") + b"\0"
        data += raw.ljust(max_len, b"\0")
    keys = keys.ljust((len(keys) + 3) & ~3, b"\0")
    key_table = 0x14 + len(index)
    data_table = key_table + len(keys)
    header = b"\0PSF" + struct.pack("<IIII", 0x101, key_table, data_table, len(entries))
    return header + index + keys + data


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(sfo([
        ("ATTRIBUTE", SUPPORT_NEO_MODE),
        ("CATEGORY", "gd"),
        ("CONTENT_ID", "IV0000-TEST00010_00-SHADTESTMEMSIZE0"),
        ("TITLE", "shadps4-open-test kernel_memory_size"),
        ("TITLE_ID", "TEST00010"),
    ]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
