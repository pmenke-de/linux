#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Make an lld-produced Hexagon shared object loadable by the FastRPC shell.

Two things the DSP loader in fastrpc_shell_unsigned_3 expects, which lld does
not produce, both established by disassembling the shell and comparing against
the stock skels on the device:

1. DT_HEXAGON_VER (0x70000001) = 3 in .dynamic. Every stock skel has it.
   There is no room to grow .dynamic afterwards and this lld has no
   --spare-dynamic-tags, so it reuses the DT_SONAME slot - safe, because
   libbenchmark_skel.so loads fine with no SONAME at all.

2. A program header table containing only PT_LOAD and PT_DYNAMIC.
   libbenchmark_skel.so, which the loader accepts, has exactly
   LOAD/LOAD/DYNAMIC; lld additionally emits PT_PHDR, PT_NOTE, PT_GNU_RELRO
   and PT_GNU_STACK. Entries are compacted in place and e_phnum reduced, which
   is safe because nothing outside the loader reads this table.

Note the loader also understands a Qualcomm hash segment, found by scanning for
(p_flags & 0x7000000) == 0x2000000 in _rtld_gethashsegment, but that is only
used on the signed path - libbenchmark_skel.so has no such segment.
"""

import struct
import sys

DT_NULL = 0
DT_SONAME = 14
DT_HEXAGON_VER = 0x70000001
HEXAGON_VER_VALUE = 3

PT_LOAD = 1
PT_DYNAMIC = 2

KEEP_PHDR_TYPES = (PT_LOAD, PT_DYNAMIC)


def read_elf(path):
    with open(path, "rb") as f:
        elf = bytearray(f.read())
    if elf[:4] != b"\x7fELF" or elf[4] != 1:
        raise SystemExit(f"{path}: not a 32-bit ELF")
    return elf


def add_dt_hexagon_ver(path, elf):
    e_shoff, = struct.unpack_from("<I", elf, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", elf, 0x2E)

    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if struct.unpack_from("<I", elf, off + 4)[0] != 6:   # SHT_DYNAMIC
            continue

        sh_offset, sh_size = struct.unpack_from("<II", elf, off + 16)
        spare = soname = None

        for pos in range(sh_offset, sh_offset + sh_size, 8):
            tag, val = struct.unpack_from("<II", elf, pos)
            if tag == DT_HEXAGON_VER:
                print(f"{path}: DT_HEXAGON_VER already present ({val})")
                return
            if tag == DT_SONAME and soname is None:
                soname = pos
            if tag == DT_NULL and spare is None and pos + 8 < sh_offset + sh_size:
                spare = pos

        target = spare if spare is not None else soname
        if target is None:
            raise SystemExit(f"{path}: no DT_NULL spare and no DT_SONAME to reuse; "
                             "relink with -Wl,-soname,<name>")

        struct.pack_into("<II", elf, target, DT_HEXAGON_VER, HEXAGON_VER_VALUE)
        how = "spare slot" if spare is not None else "reused DT_SONAME"
        print(f"{path}: DT_HEXAGON_VER={HEXAGON_VER_VALUE} ({how})")
        return

    raise SystemExit(f"{path}: no SHT_DYNAMIC section")


def prune_phdrs(path, elf):
    e_phoff, = struct.unpack_from("<I", elf, 0x1C)
    e_phentsize, e_phnum = struct.unpack_from("<HH", elf, 0x2A)

    kept, dropped = [], []
    for i in range(e_phnum):
        entry = bytes(elf[e_phoff + i * e_phentsize:
                          e_phoff + (i + 1) * e_phentsize])
        p_type, = struct.unpack_from("<I", entry, 0)
        (kept if p_type in KEEP_PHDR_TYPES else dropped).append((p_type, entry))

    if not dropped:
        print(f"{path}: program headers already minimal ({e_phnum})")
        return

    for i, (_, entry) in enumerate(kept):
        elf[e_phoff + i * e_phentsize:e_phoff + (i + 1) * e_phentsize] = entry
    # Blank the now-unused tail so stale entries cannot be misread.
    for i in range(len(kept), e_phnum):
        elf[e_phoff + i * e_phentsize:
            e_phoff + (i + 1) * e_phentsize] = b"\0" * e_phentsize

    struct.pack_into("<H", elf, 0x2C, len(kept))
    names = ", ".join(f"0x{t:x}" for t, _ in dropped)
    print(f"{path}: phdrs {e_phnum} -> {len(kept)} (dropped {names})")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <hexagon.so>")

    path = sys.argv[1]
    elf = read_elf(path)

    add_dt_hexagon_ver(path, elf)
    prune_phdrs(path, elf)

    with open(path, "wb") as f:
        f.write(elf)


if __name__ == "__main__":
    main()
