#!/usr/bin/env python3
"""Writes generated trophy data for the hle_trophy_* tests.

usage: gen_trophy_app.py <out/test dir>

shadPS4 maps a guest's trophy service labels to NP communication IDs through
sce_sys/npbind.dat and sce_sys/trophy/trophyNN.trp, and reads the trophy set
from <user dir>/trophy/<NP comm ID>/Xml once a .trp has been extracted there.
Everything here is generated: the IDs, names and XML are made up for the test,
and the .trp files are placeholders that are never decrypted.

Service labels:
  0  NPWR90000_00  a valid trophy set (TROP.XML and TROPCONF.XML)
  1  NPWR90001_00  malformed XML in both files
  2  NPWR90002_00  no extracted files; its placeholder .trp fails to extract
  3  NPWR90003_00  a valid trophy set with a title longer than the 128-byte
                   title field, whose trophy 1 has no ttype (grade) attribute

Writes <out>/sce_sys/npbind.dat, <out>/sce_sys/trophy/trophy0N.trp and
<out>/user/trophy/<id>/Xml/*.XML; scripts/run-test.py copies <out>/user into
shadPS4's user directory.
"""
from __future__ import annotations

from pathlib import Path
import struct
import sys

NP_COMM_IDS = ["NPWR90000_00", "NPWR90001_00", "NPWR90002_00", "NPWR90003_00"]

# id, grade, hidden, group (None: base game), name, detail
TROPHIES = [
    (0, "P", "no", None, "Platinum", "Unlock every other trophy"),
    (1, "G", "no", None, "Gold one", "A gold trophy"),
    (2, "S", "no", None, "Silver one", "A silver trophy"),
    (3, "B", "yes", 1, "Bronze in group", "A hidden bronze trophy in group 1"),
    (4, "B", "no", 1, "Bronze two", "A bronze trophy in group 1"),
]
GROUPS = [(1, "Group one", "The only trophy group")]
TITLE = "shadps4-open-test trophies"
LONG_TITLE = "Long title " + "x" * 189


def trophy_xml(np_comm_id: str, title: str = TITLE, no_ttype: tuple[int, ...] = ()) -> str:
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<trophyconf version="1.1" platform="ps4">',
        f"  <npcommid>{np_comm_id}</npcommid>",
        "  <trophyset-version>01.00</trophyset-version>",
        f"  <title-name>{title}</title-name>",
        "  <title-detail>Generated trophy set</title-detail>",
    ]
    for gid, name, detail in GROUPS:
        lines.append(f'  <group id="{gid:03d}"><name>{name}</name><detail>{detail}</detail></group>')
    for tid, grade, hidden, gid, name, detail in TROPHIES:
        pid = "-1" if grade == "P" else "000"
        group = f' gid="{gid:03d}"' if gid is not None else ""
        ttype = "" if tid in no_ttype else f' ttype="{grade}"'
        lines.append(
            f'  <trophy id="{tid:03d}" hidden="{hidden}"{ttype} pid="{pid}"{group}>'
            f"<name>{name}</name><detail>{detail}</detail></trophy>"
        )
    lines.append("</trophyconf>")
    return "\n".join(lines) + "\n"


MALFORMED_XML = '<?xml version="1.0" encoding="UTF-8"?>\n<trophyconf><title-name>Broken\n'


def npbind(np_comm_ids: list[str]) -> bytes:
    """npbind.dat: big-endian header, then per entry four type/size/data
    records (NP comm ID, trophy number, two unknown blocks) and 0x98 bytes of
    padding, then a 20-byte digest (zero here)."""
    bodies = b""
    for np_comm_id in np_comm_ids:
        comm = np_comm_id.encode().ljust(12, b"\0")
        bodies += struct.pack(">HH", 0x0010, 12) + comm
        bodies += struct.pack(">HH", 0x0011, 12) + b"\0" * 12
        bodies += struct.pack(">HH", 0x0012, 176) + b"\0" * 176
        bodies += struct.pack(">HH", 0x0013, 16) + b"\0" * 16
        bodies += b"\0" * 0x98
    entry_size = len(bodies) // len(np_comm_ids)
    header_size = 4 + 4 + 8 + 8 + 8 + 0x60
    total = header_size + len(bodies) + 20
    header = struct.pack(">IIQQQ", 0xD294A018, 1, total, entry_size, len(np_comm_ids))
    return header + b"\0" * 0x60 + bodies + b"\0" * 20


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    sce_sys = out / "sce_sys"
    (sce_sys / "trophy").mkdir(parents=True, exist_ok=True)
    (sce_sys / "npbind.dat").write_bytes(npbind(NP_COMM_IDS))
    for index in range(len(NP_COMM_IDS)):
        (sce_sys / "trophy" / f"trophy{index:02d}.trp").write_bytes(b"placeholder, not a TRP\n")

    valid = out / "user" / "trophy" / NP_COMM_IDS[0] / "Xml"
    valid.mkdir(parents=True, exist_ok=True)
    (valid / "TROP.XML").write_text(trophy_xml(NP_COMM_IDS[0]), encoding="utf-8")
    (valid / "TROPCONF.XML").write_text(trophy_xml(NP_COMM_IDS[0]), encoding="utf-8")

    broken = out / "user" / "trophy" / NP_COMM_IDS[1] / "Xml"
    broken.mkdir(parents=True, exist_ok=True)
    (broken / "TROP.XML").write_text(MALFORMED_XML, encoding="utf-8")
    (broken / "TROPCONF.XML").write_text(MALFORMED_XML, encoding="utf-8")

    edge = out / "user" / "trophy" / NP_COMM_IDS[3] / "Xml"
    edge.mkdir(parents=True, exist_ok=True)
    edge_xml = trophy_xml(NP_COMM_IDS[3], title=LONG_TITLE, no_ttype=(1,))
    (edge / "TROP.XML").write_text(edge_xml, encoding="utf-8")
    (edge / "TROPCONF.XML").write_text(edge_xml, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
