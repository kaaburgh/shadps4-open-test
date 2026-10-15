#!/usr/bin/env python3
"""Sets the size a FSELF's PT_SCE_PROCPARAM program header declares.

usage: set_procparam_size.py <in.elf> <out.elf> <size>

Rewrites p_filesz and p_memsz of the PT_SCE_PROCPARAM header in the ELF
header embedded in the (F)SELF; the segment's data is left as it is. Prints
the original size.
"""
from __future__ import annotations

import struct
import sys

PT_SCE_PROCPARAM = 0x61000001


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    data = bytearray(open(sys.argv[1], "rb").read())
    size = int(sys.argv[3], 0)
    elf = data.find(b"\x7fELF")
    if elf < 0:
        print("no ELF header", file=sys.stderr)
        return 1
    phoff, = struct.unpack_from("<Q", data, elf + 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, elf + 0x36)
    for i in range(phnum):
        entry = elf + phoff + i * phentsize
        if struct.unpack_from("<I", data, entry)[0] == PT_SCE_PROCPARAM:
            old, = struct.unpack_from("<Q", data, entry + 32)
            struct.pack_into("<QQ", data, entry + 32, size, size)
            open(sys.argv[2], "wb").write(data)
            print(hex(old))
            return 0
    print("no PT_SCE_PROCPARAM header", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
