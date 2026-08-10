#!/usr/bin/env python3
"""List the top-level chunks of a Qualcomm ACDB file.

Layout: 'QCMSNDDB' + 8 reserved, then an 8-byte file-type id and two u32
lengths, then a flat sequence of chunks: 8-byte id, u32 length, payload.
"""
import struct
import sys


def chunks(data):
    if data[:8] != b"QCMSNDDB":
        raise SystemExit("not an ACDB file")

    off = 32  # magic(8) + reserved(8) + type(8) + len(4) + len(4)
    while off + 12 <= len(data):
        cid = data[off:off + 8]
        (size,) = struct.unpack_from("<I", data, off + 8)
        payload = data[off + 12:off + 12 + size]
        yield cid.decode("ascii", "replace").rstrip("\0 "), off, size, payload
        off += 12 + size


def main():
    for path in sys.argv[1:]:
        with open(path, "rb") as f:
            data = f.read()

        print(f"=== {path} ({len(data)} bytes, type {data[16:24]!r}) ===")
        for name, off, size, payload in chunks(data):
            head = payload[:16].hex(" ")
            print(f"  {name:<12} @0x{off:06x} {size:>8} bytes  {head}")


if __name__ == "__main__":
    main()
