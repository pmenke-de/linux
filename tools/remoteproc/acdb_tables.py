#!/usr/bin/env python3
"""Dump the voice calibration tables of an ACDB file.

The container is a flat chunk list (see acdb.py). Inside, each table follows
the same shape: a u32 entry count followed by fixed-size records. The record
size is deduced from the chunk length, which also tells us how many u32 fields
each record has.
"""
import struct
import sys

from acdb_chunks import chunks


def dump(name, payload, limit=6):
    if len(payload) < 4:
        print(f"  {name:<10} (empty)")
        return

    (count,) = struct.unpack_from("<I", payload, 0)
    body = len(payload) - 4
    if count == 0:
        print(f"  {name:<10} 0 entries")
        return
    if body % count:
        print(f"  {name:<10} {count} entries, {body} bytes -- not a flat table")
        return

    stride = body // count
    words = stride // 4
    print(f"  {name:<10} {count} entries x {stride} bytes ({words} u32)")

    for i in range(min(count, limit)):
        off = 4 + i * stride
        vals = struct.unpack_from("<%dI" % words, payload, off)
        print("      " + "  ".join(f"0x{v:08x}" for v in vals))
    if count > limit:
        print(f"      ... {count - limit} more")


def main():
    path = sys.argv[1]
    want = sys.argv[2:] or ["VSTILUT0", "VSTIOFST", "VSTICDFT", "VSTICDOT",
                            "VDYILUT0", "VDYICDFT", "VDYICDOT"]

    with open(path, "rb") as f:
        data = f.read()

    for name, off, size, payload in chunks(data):
        if name in want:
            dump(name, payload)


if __name__ == "__main__":
    main()
