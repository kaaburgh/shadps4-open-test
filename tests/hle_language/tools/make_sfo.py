#!/usr/bin/env python3
"""Writes a minimal param.sfo for a made-up title.

usage: make_sfo.py <out.sfo> <TITLE_ID>

shadPS4 takes the title's serial from CONTENT_ID (characters 7..15), or from
TITLE_ID when there is no CONTENT_ID, and loads per-title settings from
<user dir>/custom_configs/<serial>.json. Both keys are written, consistent
with each other, like on a real title.
"""
from __future__ import annotations

from pathlib import Path
import struct
import sys

TEXT = 0x0204


def sfo(entries: list[tuple[str, str]]) -> bytes:
    """PSF 1.1 with UTF-8 text entries, keys in sorted order."""
    entries = sorted(entries)
    keys = b""
    data = b""
    index = b""
    for key, value in entries:
        raw = value.encode("utf-8") + b"\0"
        max_len = (len(raw) + 3) & ~3
        index += struct.pack("<HHIII", len(keys), TEXT, len(raw), max_len, len(data))
        keys += key.encode("ascii") + b"\0"
        data += raw.ljust(max_len, b"\0")
    keys = keys.ljust((len(keys) + 3) & ~3, b"\0")
    key_table = 0x14 + len(index)
    data_table = key_table + len(keys)
    header = b"\0PSF" + struct.pack("<IIII", 0x101, key_table, data_table, len(entries))
    return header + index + keys + data


def main() -> int:
    if len(sys.argv) != 3 or len(sys.argv[2]) != 9:
        print(__doc__, file=sys.stderr)
        return 2
    out, title_id = Path(sys.argv[1]), sys.argv[2]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(sfo([
        ("CATEGORY", "gd"),
        ("CONTENT_ID", f"IV0000-{title_id}_00-SHADTESTLANGUAGE"),
        ("TITLE", "shadps4-open-test hle_language"),
        ("TITLE_ID", title_id),
    ]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
