#!/usr/bin/env python3
"""List the calibration devices of a Qualcomm ACDB file.

Device ids are what the voice calibration lookup tables are keyed by, and they
are otherwise just bare numbers -- this resolves them to the names the
calibration was authored with, which is how you find the pair belonging to a
call path (for a handset call on Apollo: HANDSET_MIC -> HANDSET_SPKR_FVSAM).

    ./acdb_devices.py /path/to/Forte_Handset_cal.acdb
"""
import struct
import sys

from acdb_chunks import chunks

# Device property holding the authored name, as UTF-16.
ACDB_DEV_PROP_NAME = 0x000113B8


def devices(data):
    pools = {name: payload for name, _, _, payload in chunks(data)}
    pool = pools["DATAPOOL"]
    lut = pools["DPROPLUT"]

    (count,) = struct.unpack_from("<I", lut, 0)
    names = {}

    for i in range(count):
        dev, prop, off = struct.unpack_from("<3I", lut, 4 + i * 12)
        if prop != ACDB_DEV_PROP_NAME or dev in names:
            continue
        (length,) = struct.unpack_from("<I", pool, off)
        raw = pool[off + 4:off + 4 + length]
        names[dev] = raw.decode("utf-16-le", "replace").rstrip("\0")

    return names


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)

    for path in sys.argv[1:]:
        with open(path, "rb") as f:
            names = devices(f.read())

        print("=== %s: %d devices ===" % (path, len(names)))
        for dev in sorted(names):
            print("  0x%05x  %s" % (dev, names[dev]))


if __name__ == "__main__":
    main()
