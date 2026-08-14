#!/usr/bin/env python3
"""List and extract the global properties of a Qualcomm ACDB file.

The per-device calibration lives in the device files and acdb_voice_cal.py
walks that. A handful of things are not per-device, though, and those sit in
Forte_Global_cal.acdb under GPROPLUT: a flat table of

    [u32 count][count x (property_id, datapool_offset)]

with each offset pointing at a [u32 size][data] record in DATAPOOL.

The AVCS custom topologies -- what AVCS_CMD_REGISTER_TOPOLOGIES wants, and
without which the vendor's own vocproc topology ids cannot be used -- are one
of these properties. Their payload is:

    u32 num_topologies
    per topology:
        u32 topology_id
        u32 num_modules
        num_modules x { u32 module_id, u32 0x00010000, u32 0, u32 0, u32 0, u32 0 }

so a property can be recognised as topology data by decoding it rather than by
guessing from its id.

    ./acdb_global_prop.py Forte_Global_cal.acdb
    ./acdb_global_prop.py Forte_Global_cal.acdb --id 0x131a7 -o topologies.bin
"""
import argparse
import struct
import sys

from acdb_chunks import chunks

MODULE_WORDS = 6

# The database as ACDB stores it: [count][version] followed by per-topology
# {id, flags, num_modules, modules, terminator}. Only the module entry width
# differs between versions.
#
# Walk exactly [count] topologies. The terminator is not reliably equal to the
# version -- stopping on that read 45 of the 95 topologies in one real blob and
# made a topology that is present look absent, which cost a long detour.
ACDB_MODULE_WORDS = {0: 1, 2: 2}


def global_props(data):
    """Yield (property_id, payload) for every entry in GPROPLUT."""
    tables = {name: payload for name, _, _, payload in chunks(data)}
    for name in ("GPROPLUT", "DATAPOOL"):
        if name not in tables:
            raise SystemExit("%s missing; is this a Global_cal file?" % name)

    lut, pool = tables["GPROPLUT"], tables["DATAPOOL"]
    (count,) = struct.unpack_from("<I", lut, 0)
    for i in range(count):
        pid, off = struct.unpack_from("<II", lut, 4 + i * 8)
        if off + 4 > len(pool):
            continue
        (size,) = struct.unpack_from("<I", pool, off)
        yield pid, pool[off + 4:off + 4 + size]


def parse_acdb_topologies(payload):
    """Decode ACDB's own topology database: [count][version][topologies...]."""
    if len(payload) < 8:
        return None
    words = struct.unpack("<%dI" % (len(payload) // 4), payload[:len(payload) // 4 * 4])

    version = words[1]
    if version not in ACDB_MODULE_WORDS:
        return None
    stride = ACDB_MODULE_WORDS[version]

    count = words[0]
    i, out = 2, []
    while len(out) < count and i + 3 <= len(words):
        topo_id, flags, nmods = words[i], words[i + 1], words[i + 2]
        if nmods > 64 or i + 3 + nmods * stride + 1 > len(words):
            break
        out.append((topo_id, flags,
                    [words[i + 3 + k * stride] for k in range(nmods)]))
        i += 3 + nmods * stride + 1
    return out or None


def to_avcs_payload(topologies):
    """Re-emit topologies in the form AVCS_CMD_REGISTER_TOPOLOGIES parses.

    Same information, wider module entries: each module becomes six words, of
    which only the id and a constant are used. This is the conversion Android's
    acdb_loader performs before handing the blob to the DSP, and skipping it is
    why registering ACDB's copy verbatim is refused with ADSP_EBADPARAM.
    """
    out = [struct.pack("<I", len(topologies))]
    for topo_id, _flags, mods in topologies:
        out.append(struct.pack("<II", topo_id, len(mods)))
        for module_id in mods:
            out.append(struct.pack("<6I", module_id, 0x00010000, 0, 0, 0, 0))
    return b"".join(out)


def describe_topologies(payload):
    """Decode a payload as AVCS custom topologies, or return None."""
    if len(payload) < 4:
        return None
    words = struct.unpack("<%dI" % (len(payload) // 4), payload[:len(payload) // 4 * 4])

    count, i, found = words[0], 1, []
    if not 0 < count <= 64:
        return None
    for _ in range(count):
        if i + 1 >= len(words):
            return None
        topo_id, nmods = words[i], words[i + 1]
        i += 2
        if nmods > 64 or i + nmods * MODULE_WORDS > len(words):
            return None
        mods = [words[i + m * MODULE_WORDS] for m in range(nmods)]
        i += nmods * MODULE_WORDS
        found.append((topo_id, mods))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("acdb")
    ap.add_argument("--id", type=lambda s: int(s, 0),
                    help="property id to write out")
    ap.add_argument("-o", "--output")
    ap.add_argument("--avcs", action="store_true",
                    help="convert ACDB's topology database to the AVCS payload")
    args = ap.parse_args()

    data = open(args.acdb, "rb").read()

    for pid, payload in global_props(data):
        if args.id is not None and pid != args.id:
            continue

        topos = describe_topologies(payload)
        note = ""
        if topos is not None:
            note = "  topologies: " + ", ".join(
                "%#x(%d)" % (t, len(m)) for t, m in topos)
        print("property %#010x  %6d bytes%s" % (pid, len(payload), note))

        if args.id is not None and args.output:
            if args.avcs:
                acdb = parse_acdb_topologies(payload)
                if not acdb:
                    raise SystemExit("property %#x is not a topology database"
                                     % pid)
                print("  %d topologies -> AVCS payload" % len(acdb))
                payload = to_avcs_payload(acdb)
            with open(args.output, "wb") as f:
                f.write(payload)
            print("wrote %d bytes to %s" % (len(payload), args.output),
                  file=sys.stderr)


if __name__ == "__main__":
    main()
