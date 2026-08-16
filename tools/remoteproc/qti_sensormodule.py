#!/usr/bin/env python3
"""Extract camera sensor register tables from QTI Chromatix sensormodule blobs.

Qualcomm's CAMX keeps per-sensor register sequences in
``com.qti.sensormodule.<module>.bin`` rather than in any kernel source, so
porting a sensor to a mainline V4L2 subdev driver means reading them out of
these files.

Container layout, worked out against the Xiaomi apollo OV13B10 blob and
checked against the register tables in the in-tree ov13b10 driver:

  0x00  "QTI Chromatix Header", total size at 0x1c
  0x28  "Parameter Parser V2.0.0 (...)"
  0x98  u32 section table offset, u32 section count
        section entries are (u64 offset, u64 size, u64 type)

The second section is the data blob; the first holds a flat list of field
descriptors, each 72 bytes:

  u32 0xffffffff | u32 0 | u64 offset | u64 size | u64 index | char name[40]

``offset`` is a running cursor into the data blob and ``size`` the field's
extent, so descriptors tile the data blob in order.  Note the names are
shifted by one relative to the regions: the region belonging to descriptor N
carries the value of the field named by descriptor N+1.  That quirk does not
matter for register tables, which are found structurally instead:

A register sequence is stored as two parallel arrays.  The first holds one
80-byte record per register, of ten u64 fields::

    [addr, 1, idx, addr_bytes, data_bytes, op, 1, idx, delay, idx]

and is immediately followed by a 16-byte-per-register array of ``[value,
delay]``.  ``addr_bytes``/``data_bytes`` give the I2C address and data widths
(2/1 for OmniVision, 2/2 for the Samsung parts), which is what a driver needs
to know to replay the sequence.
"""

import argparse
import re
import struct
import sys
from collections import namedtuple

DESC_MAGIC = b"\xff\xff\xff\xff\x00\x00\x00\x00"
DESC_SIZE = 72
ADDR_STRIDE = 80
VALUE_STRIDE = 16
LAST_ADDR_SIZE = 64

Desc = namedtuple("Desc", "off size index name")
Reg = namedtuple("Reg", "addr value addr_bytes data_bytes delay")


class SensorModule:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        self._parse_header()
        self._parse_descriptors()

    def _parse_header(self):
        d = self.data
        if not d.startswith(b"QTI Chromatix Header"):
            raise ValueError(f"{self.path}: not a QTI Chromatix blob")
        declared = struct.unpack_from("<I", d, 0x1C)[0]
        if declared != len(d):
            print(f"warning: size field {declared:#x} != file size {len(d):#x}",
                  file=sys.stderr)
        tbl, count = struct.unpack_from("<II", d, 0x98)
        self.sections = [struct.unpack_from("<QQQ", d, tbl + 8 + i * 24)
                         for i in range(count)]
        # Sections are (offset, size, type); type 1 holds the descriptors and
        # type 2 the data they point into.
        data = [s for s in self.sections if s[2] == 2]
        if not data:
            raise ValueError(f"{self.path}: no data section")
        self.data_base = data[0][0]
        self.schema_end = self.data_base

    def _parse_descriptors(self):
        self.descs = []
        for m in re.finditer(re.escape(DESC_MAGIC), self.data):
            o = m.start()
            if o >= self.data_base:
                break
            off, size, index = struct.unpack_from("<QQQ", self.data, o + 8)
            name = self.data[o + 32:o + DESC_SIZE].split(b"\x00")[0]
            self.descs.append(Desc(off, size, index,
                                   name.decode("ascii", "replace")))

    def _u64(self, off):
        return struct.unpack_from("<Q", self.data, off)[0]

    def _is_reg_record(self, o):
        """One 80-byte address record: address then I2C address/data widths."""
        if o + ADDR_STRIDE > len(self.data):
            return False
        return (self._u64(o) <= 0xFFFF
                and self._u64(o + 3 * 8) in (1, 2)
                and self._u64(o + 4 * 8) in (1, 2)
                and self._u64(o + 5 * 8) <= 2)

    def register_groups(self, min_regs=4):
        """Every register sequence in the blob.

        The address records are found by scanning rather than from the
        descriptor boundaries: a sequence's region starts with a couple of
        bookkeeping words, so the array is not aligned to the region.
        """
        groups = []
        o = self.data_base
        end = len(self.data)
        while o + ADDR_STRIDE <= end:
            if not self._is_reg_record(o):
                o += 8
                continue
            n = 0
            while self._is_reg_record(o + n * ADDR_STRIDE):
                n += 1
            if n < min_regs:
                o += ADDR_STRIDE
                continue
            # The serializer omits trailing null fields, so the final address
            # record is two u64 short of the others.
            vbase = o + (n - 1) * ADDR_STRIDE + LAST_ADDR_SIZE
            if vbase + n * VALUE_STRIDE > end:
                break
            regs = []
            for i in range(n):
                rec = o + i * ADDR_STRIDE
                val = vbase + i * VALUE_STRIDE
                regs.append(Reg(addr=self._u64(rec),
                                value=self._u64(val),
                                addr_bytes=self._u64(rec + 3 * 8),
                                data_bytes=self._u64(rec + 4 * 8),
                                delay=self._u64(val + 8)))
            groups.append((o - self.data_base, regs))
            o = vbase + n * VALUE_STRIDE
        return groups

    def geometry(self, group_off, search=64):
        """Width, height and bit depth for the mode a register group belongs to.

        Each mode's metadata precedes its register array, but how much of it
        there is varies per sensor, so look back a bounded window for the last
        (width, height, depth) triple that makes sense as a sensor readout.
        """
        base = self.data_base + group_off
        best = None
        for i in range(search, 2, -1):
            o = base - i * 8
            if o < self.data_base:
                continue
            w, h, depth = (self._u64(o), self._u64(o + 8), self._u64(o + 16))
            if (depth in (8, 10, 12, 14)
                    and 16 <= h <= w <= 20000):
                best = (w, h, depth)
        return best

    def strings(self, limit=40):
        """Printable field values, useful for names and slave addresses."""
        out = []
        for desc in self.descs:
            if not 1 <= desc.size <= 64:
                continue
            raw = self.data[self.data_base + desc.off:
                            self.data_base + desc.off + desc.size]
            txt = raw.split(b"\x00")[0]
            if len(txt) >= 4 and all(32 <= c < 127 for c in txt):
                out.append((desc.name, txt.decode()))
        return out[:limit]


def emit_c(regs, name, data_bytes):
    """Print a register table as a C array for a mainline sensor driver."""
    width = 4 if data_bytes == 2 else 2
    print(f"static const struct sensor_reg {name}[] = {{")
    for r in regs:
        print(f"\t{{0x{r.addr:04x}, 0x{r.value:0{width}x}}},"
              + (f"\t/* delay {r.delay} us */" if r.delay else ""))
    print("};")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("blob")
    ap.add_argument("--list", action="store_true",
                    help="summarise the register groups and exit")
    ap.add_argument("--strings", action="store_true",
                    help="dump printable fields (names, slave address)")
    ap.add_argument("--group", type=int,
                    help="emit this group (index from --list) as a C array")
    ap.add_argument("--name", default="sensor_mode_regs",
                    help="identifier for the emitted C array")
    args = ap.parse_args()

    sm = SensorModule(args.blob)
    groups = sm.register_groups()

    if args.strings:
        for n, v in sm.strings():
            print(f"  {n:24s} {v}")
        return

    if args.group is not None:
        _off, regs = groups[args.group]
        emit_c(regs, args.name, regs[0].data_bytes if regs else 1)
        return

    print(f"{args.blob}")
    print(f"  data blob at {sm.data_base:#x}, {len(sm.descs)} descriptors")
    print(f"  {len(groups)} register groups")
    for i, (off, regs) in enumerate(groups):
        aw = regs[0].addr_bytes
        dw = regs[0].data_bytes
        print(f"  [{i:2d}] {len(regs):5d} regs  addr@{off:#08x} "
              f"{aw}-byte addr / {dw}-byte data  "
              f"first={regs[0].addr:#06x}={regs[0].value:#x} "
              f"last={regs[-1].addr:#06x}={regs[-1].value:#x}")


if __name__ == "__main__":
    main()
