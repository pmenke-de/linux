#!/usr/bin/env python3
"""Extract voice calibration from a Qualcomm ACDB file.

The ADSP will not resolve a volume step until the vocproc calibration has been
registered with it, so a call comes up fully configured and silent. The
calibration lives in these files, reachable through five tables that reference
each other by byte offset:

    LUT   sorted records of `stride` u32; the first `nkeys` are the key and
          the rest are offsets into OFST and CVD0. Looked up by binary search.
    OFST  [count][count x (cdft_off, cdot_off)] -- one record per volume step
    CDFT  [count][count x (module_id, instance_id, param_id)]
    CDOT  [count][count x datapool_off], one per entry in the matching CDFT
    POOL  [u32 size][data] at each of those offsets

CDFT and CDOT always agree on their count; that is the check that the walk has
stayed on the rails.

Device ids are the ones acdb_devices.py prints -- for a handset call on Apollo,
HANDSET_MIC (0x04) to HANDSET_SPKR_FVSAM (0x07).

    ./acdb_voice_cal.py Forte_Handset_cal.acdb --tx 0x04 --rx 0x07
    ./acdb_voice_cal.py Forte_Handset_cal.acdb --tx 0x04 --rx 0x07 -o vol.bin
"""
import argparse
import struct
import sys

from acdb_chunks import chunks

# Table families. The suffixed names are the "instance" variants, which carry
# an instance id per parameter; the older VPDY/VPST tables do not and are not
# what current calibration files use.
FAMILIES = {
    "dynamic": ("VDYILUT0", "VDYIOFST", "VDYICDFT", "VDYICDOT", 5, 3),
    "static": ("VSTILUT0", "VSTIOFST", "VSTICDFT", "VSTICDOT", 6, 4),
}

# How the calibration table is indexed. The ADSP needs this to turn a volume
# step into a row, and it does not come from the file: the vendor library
# carries it as constants, so it is reproduced here. The volume table is keyed
# by one column more than the plain vocproc table -- the step itself, which is
# the column with no "not applicable" value.
COLUMNS = {
    "static": [(0x00011350, 0x0001135C, 0x0001135E),
               (0x00011352, 0x0001135C, 0x00000000),
               (0x00011351, 0x0001135C, 0x00000000)],
    "dynamic": [(0x00011350, 0x0001135C, 0x0001135E),
                (0x00011352, 0x0001135C, 0x00000000),
                (0x00011351, 0x0001135C, 0x00000000),
                (0x00011358, 0x0001135C, 0xFFFFFFFF)],
}


def column_info(family):
    cols = COLUMNS[family]
    out = struct.pack("<I", len(cols))
    for cid, ctype, na in cols:
        out += struct.pack("<3I", cid, ctype, na)
    return out


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def lookup(lut, stride, nkeys, key):
    """Binary search the LUT, which the loader relies on being sorted."""
    count = u32(lut, 0)
    lo, hi = 0, count - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        rec = struct.unpack_from("<%dI" % stride, lut, 4 + mid * stride * 4)
        if rec[:nkeys] < key:
            lo = mid + 1
        elif rec[:nkeys] > key:
            hi = mid - 1
        else:
            return rec
    return None


def extract(path, family, key):
    with open(path, "rb") as f:
        data = f.read()

    pools = {name: payload for name, _, _, payload in chunks(data)}
    lut_n, ofst_n, cdft_n, cdot_n, stride, nkeys = FAMILIES[family]

    missing = [n for n in (lut_n, ofst_n, cdft_n, cdot_n, "DATAPOOL")
               if n not in pools]
    if missing:
        raise SystemExit("%s: missing tables %s" % (path, ", ".join(missing)))

    lut, ofst = pools[lut_n], pools[ofst_n]
    cdft, cdot, pool = pools[cdft_n], pools[cdot_n], pools["DATAPOOL"]

    if len(key) != nkeys:
        raise SystemExit("%s needs %d key fields, got %d"
                         % (family, nkeys, len(key)))

    rec = lookup(lut, stride, nkeys, key)
    if rec is None:
        raise SystemExit("no %s entry for key %s"
                         % (family, [hex(k) for k in key]))

    v0 = rec[nkeys]
    steps = u32(ofst, v0)
    records = []

    for j in range(steps):
        cdft_off, cdot_off = struct.unpack_from("<2I", ofst, v0 + 4 + j * 8)
        n_fmt = u32(cdft, cdft_off)
        n_off = u32(cdot, cdot_off)
        if n_fmt != n_off:
            raise SystemExit("record %d: CDFT says %d params, CDOT says %d"
                             % (j, n_fmt, n_off))

        params = []
        for k in range(n_fmt):
            mid, iid, pid = struct.unpack_from("<3I", cdft,
                                               cdft_off + 4 + k * 12)
            off = u32(cdot, cdot_off + 4 + k * 4)
            size = u32(pool, off)
            params.append((mid, iid, pid, pool[off + 4:off + 4 + size]))
        records.append(params)

    return records


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("acdb")
    ap.add_argument("--family", choices=sorted(FAMILIES), default="dynamic")
    ap.add_argument("--tx", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--rx", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--flag", type=lambda s: int(s, 0), default=0,
                    help="dynamic: third key field")
    ap.add_argument("--tx-rate", type=lambda s: int(s, 0), default=8000)
    ap.add_argument("--rx-rate", type=lambda s: int(s, 0), default=8000)
    ap.add_argument("--step", type=int, default=0,
                    help="which volume step to emit with -o")
    ap.add_argument("-o", "--output", help="write one step's parameters")
    ap.add_argument("--col-info", help="override the built-in column description")
    ap.add_argument("--raw", action="store_true",
                    help="omit the header the kernel expects")
    args = ap.parse_args()

    if args.family == "dynamic":
        key = (args.tx, args.rx, args.flag)
    else:
        key = (args.tx, args.rx, args.tx_rate, args.rx_rate)

    records = extract(args.acdb, args.family, key)
    print("%s: %d records for key %s"
          % (args.family, len(records), [hex(k) for k in key]))

    total = sum(len(p[3]) for r in records for p in r)
    print("  %d parameters, %d bytes of calibration"
          % (sum(len(r) for r in records), total))

    for mid, iid, pid, payload in records[args.step][:8]:
        print("    mid=0x%08x iid=%d pid=0x%08x size=%d"
              % (mid, iid, pid, len(payload)))

    if args.output:
        blob = bytearray()
        for mid, iid, pid, payload in records[args.step]:
            blob += struct.pack("<4I", mid, iid, pid, len(payload))
            blob += payload
            if len(payload) % 4:
                blob += b"\0" * (4 - len(payload) % 4)

        if args.col_info:
            with open(args.col_info, "rb") as f:
                col = f.read()
        else:
            col = column_info(args.family)

        with open(args.output, "wb") as f:
            if args.raw:
                f.write(blob)
            else:
                # Header the kernel's q6voice-cal.c expects.
                f.write(struct.pack("<4I", 0x43563651, 1, len(col), len(blob)))
                f.write(col)
                f.write(blob)
        print("  wrote step %d to %s (%d bytes%s)"
              % (args.step, args.output, len(blob),
                 "" if args.raw else ", %d byte header + %d column"
                 % (16, len(col))))


if __name__ == "__main__":
    main()
