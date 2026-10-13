#!/usr/bin/env python3
"""Makes a copy of the linked guest that requests a flexible memory size.

usage: set_flexible_size.py <in.raw.elf> <out.raw.elf> <size>

Works on the ELF from the linker, before create-fself. The process
parameters (.data.sce_process_param) point to their kernel memory
parameters through a relative relocation at offset 0x40; this repoints it
to the guest's shadtest_mem_param and stores <size> in
shadtest_flexible_size, which that block's flexible_memory_size points to.
"""
from __future__ import annotations

import struct
import sys

MEM_PARAM_OFFSET = 0x40
R_X86_64_RELATIVE = 8


def sections(data: bytes) -> dict[str, tuple[int, int, int, int]]:
    """name -> (address, file offset, size, entry size)"""
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    headers = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    names_offset = headers[shstrndx][4]
    result = {}
    for name, _, _, addr, offset, size, _, _, _, entsize in headers:
        end = data.index(b"\0", names_offset + name)
        result[data[names_offset + name:end].decode()] = (addr, offset, size, entsize)
    return result


def symbol(data: bytes, secs: dict, wanted: str) -> int:
    _, symoff, symsize, _ = secs[".symtab"]
    _, stroff, _, _ = secs[".strtab"]
    for pos in range(symoff, symoff + symsize, 24):
        name, _, _, _, value, _ = struct.unpack_from("<IBBHQQ", data, pos)
        end = data.index(b"\0", stroff + name)
        if data[stroff + name:end].decode() == wanted:
            return value
    raise SystemExit(f"no symbol {wanted}")


def file_offset(secs: dict, addr: int) -> int:
    for name, (start, offset, size, _) in secs.items():
        if name and start and start <= addr < start + size:
            return offset + addr - start
    raise SystemExit(f"address {addr:#x} not in a section")


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    data = bytearray(open(sys.argv[1], "rb").read())
    size = int(sys.argv[3], 0)
    secs = sections(data)
    slot = secs[".data.sce_process_param"][0] + MEM_PARAM_OFFSET
    mem_param = symbol(data, secs, "shadtest_mem_param")
    flexible_size = symbol(data, secs, "shadtest_flexible_size")

    _, relaoff, relasize, _ = secs[".rela.dyn"]
    for pos in range(relaoff, relaoff + relasize, 24):
        offset, info, _ = struct.unpack_from("<QQq", data, pos)
        if offset == slot and info & 0xFFFFFFFF == R_X86_64_RELATIVE:
            struct.pack_into("<q", data, pos + 16, mem_param)
            break
    else:
        raise SystemExit("no relocation for the process parameters' mem_param")
    struct.pack_into("<Q", data, file_offset(secs, flexible_size), size)
    open(sys.argv[2], "wb").write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
