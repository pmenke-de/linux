#!/usr/bin/env python3
#
# Build a focused strings/pointer report for SDX55 qdsp6sw.mbn.
#
# The local radare2 build does not disassemble Hexagon, so this report stays
# deterministic: map strings through PT_LOAD headers, cluster nearby strings,
# and search for little-endian 32-bit pointer references to high-value strings.

import argparse
import re
import struct
import subprocess
from collections import defaultdict


GROUPS = {
    "mcfg_rfs": [
        "mcfg_",
        "/dev/mcfg",
        "mcfg_remote",
        "mcfg_ext_set_rfs_params",
        "rfs_params.",
        "oem_sw_path_info",
        "oem_hw_path_info",
        "mbn_sw",
        "mbn_hw",
        "rfs not accessible",
        "cust_path_info",
        "path_info",
    ],
    "pdc": [
        "pdc",
        "PDC",
        "set_selected",
        "activate",
        "load_config",
        "QMI_PDC",
    ],
    "rfs_efs_tftp": [
        "remotefs",
        "remoteefs",
        "rfs_",
        "RFS",
        "tftp",
        "TFTP",
        "/nv/item_files",
        "/readwrite",
        "EFS",
        "efs_",
    ],
    "ipa_mhi": [
        "IPA",
        "ipa_",
        "MHI",
        "mhi_",
        "QMI_IPA",
        "INIT_MODEM_DRIVER",
        "HWP",
    ],
    "watchdog_sfr": [
        "watchdog",
        "Watchdog",
        "wdog",
        "Wdog",
        "dog_",
        "/dev/dog",
        "starvation",
        "SFR",
        "coredump.err",
        "aux_msg",
        "ERR_FATAL",
    ],
    "sysmon_ssctl": [
        "sys_m",
        "SSCTL",
        "ssctl",
        "smp2p",
        "SMP2P",
        "SSR",
        "subsys",
    ],
}

POINTER_TARGETS = [
    "mcfg_ext_set_rfs_params",
    "rfs_params.",
    "oem_sw_path_info",
    "oem_hw_path_info",
    "mcfg_remote",
    "/dev/mcfg",
    "rfs not accessible",
    "cust_path_info",
    "path_info",
    "/nv/item_files/mcfg",
    "Process (Virtual Dog) starvation",
    "Task starvation",
    "coredump.err.aux_msg",
    "SFR Init",
    "INIT_MODEM_DRIVER",
    "QMI_IPA",
]


def run(cmd):
    return subprocess.run(cmd, check=True, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE).stdout


def load_segments(firmware):
    out = run(["readelf", "-l", firmware])
    segments = []
    for line in out.splitlines():
        fields = line.split()
        if len(fields) < 7 or fields[0] != "LOAD":
            continue
        segments.append({
            "off": int(fields[1], 16),
            "vaddr": int(fields[2], 16),
            "paddr": int(fields[3], 16),
            "filesz": int(fields[4], 16),
            "memsz": int(fields[5], 16),
            "flags": "".join(fields[6:-1]),
        })
    return segments


def map_off(segments, off):
    for seg in segments:
        if seg["off"] <= off < seg["off"] + seg["filesz"]:
            delta = off - seg["off"]
            return seg["vaddr"] + delta, seg["paddr"] + delta
    return None, None


def load_strings(firmware):
    out = run(["strings", "-a", "-t", "x", firmware])
    strings = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-fA-F]+)\s+(.*)", line)
        if not m:
            continue
        off = int(m.group(1), 16)
        text = m.group(2)
        strings.append({"off": off, "text": text})
    return strings


def matching_groups(text):
    text_l = text.lower()
    groups = []
    for group, needles in GROUPS.items():
        if any(needle.lower() in text_l for needle in needles):
            groups.append(group)
    return groups


def collect_hits(strings, segments):
    hits = []
    for item in strings:
        groups = matching_groups(item["text"])
        if not groups:
            continue
        vaddr, paddr = map_off(segments, item["off"])
        hits.append({
            "off": item["off"],
            "vaddr": vaddr,
            "paddr": paddr,
            "text": item["text"],
            "groups": groups,
        })
    return hits


def cluster_hits(hits, group, radius):
    group_hits = [h for h in hits if group in h["groups"]]
    group_hits.sort(key=lambda h: h["off"])
    clusters = []
    current = None
    for hit in group_hits:
        if current is None or hit["off"] > current["end"] + radius:
            current = {"start": hit["off"], "end": hit["off"], "hits": []}
            clusters.append(current)
        current["end"] = max(current["end"], hit["off"])
        current["hits"].append(hit)
    return clusters


def nearby_strings(strings, start, end, radius):
    lo = max(0, start - radius)
    hi = end + radius
    return [s for s in strings if lo <= s["off"] <= hi]


def wants_pointer_target(text):
    text_l = text.lower()
    return any(needle.lower() in text_l for needle in POINTER_TARGETS)


def find_all(data, needle, limit):
    refs = []
    pos = data.find(needle)
    while pos != -1:
        refs.append(pos)
        if len(refs) >= limit:
            break
        pos = data.find(needle, pos + 1)
    return refs


def pointer_refs(firmware, hits, segments, per_value_limit=16):
    with open(firmware, "rb") as f:
        data = f.read()

    refs_by_off = defaultdict(list)
    seen_targets = set()
    for hit in hits:
        if not wants_pointer_target(hit["text"]):
            continue
        target_key = (hit["off"], hit["text"])
        if target_key in seen_targets:
            continue
        seen_targets.add(target_key)
        values = []
        if hit["vaddr"] is not None:
            values.append(("vaddr", hit["vaddr"]))
        if hit["paddr"] is not None:
            values.append(("paddr", hit["paddr"]))
        for kind, value in values:
            if not 0 <= value <= 0xffffffff:
                continue
            raw = struct.pack("<I", value)
            for ref_off in find_all(data, raw, per_value_limit):
                ref_vaddr, ref_paddr = map_off(segments, ref_off)
                refs_by_off[hit["off"]].append({
                    "kind": kind,
                    "value": value,
                    "ref_off": ref_off,
                    "ref_vaddr": ref_vaddr,
                    "ref_paddr": ref_paddr,
                })
    return refs_by_off


def md(text):
    return text.replace("|", "\\|")


def write_report(path, firmware, segments, strings, hits, refs_by_off, radius):
    by_group = defaultdict(list)
    for hit in hits:
        for group in hit["groups"]:
            by_group[group].append(hit)

    lines = []
    lines.append("# SDX55M qdsp6 string cluster report")
    lines.append("")
    lines.append(f"Firmware: `{firmware}`")
    lines.append("")
    lines.append("## Program header mapping")
    lines.append("")
    lines.append("| file off | vaddr | paddr | filesz | memsz | flags |")
    lines.append("| --- | --- | --- | --- | --- | --- |")
    for seg in segments:
        lines.append(
            f"| `{seg['off']:#x}` | `{seg['vaddr']:#x}` | `{seg['paddr']:#x}` | "
            f"`{seg['filesz']:#x}` | `{seg['memsz']:#x}` | `{seg['flags']}` |"
        )

    lines.append("")
    lines.append("## Hit Summary")
    lines.append("")
    lines.append("| group | hits | clusters |")
    lines.append("| --- | ---: | ---: |")
    for group in GROUPS:
        clusters = cluster_hits(hits, group, radius)
        lines.append(f"| `{group}` | {len(by_group[group])} | {len(clusters)} |")

    for group in GROUPS:
        clusters = cluster_hits(hits, group, radius)
        lines.append("")
        lines.append(f"## {group}")
        lines.append("")
        if not clusters:
            lines.append("_No hits._")
            continue
        lines.append("| range | hits | sample strings |")
        lines.append("| --- | ---: | --- |")
        for cl in clusters:
            samples = "; ".join(md(h["text"]) for h in cl["hits"][:4])
            if len(samples) > 260:
                samples = samples[:257] + "..."
            lines.append(f"| `{cl['start']:#x}-{cl['end']:#x}` | {len(cl['hits'])} | {samples} |")

        lines.append("")
        lines.append("### Cluster Details")
        for cl in clusters:
            lines.append("")
            lines.append(f"#### `{cl['start']:#x}-{cl['end']:#x}`")
            for s in nearby_strings(strings, cl["start"], cl["end"], radius):
                vaddr, paddr = map_off(segments, s["off"])
                addr = ""
                if vaddr is not None:
                    addr = f" vaddr=`{vaddr:#x}` paddr=`{paddr:#x}`"
                marker = ""
                if matching_groups(s["text"]):
                    marker = " *"
                lines.append(f"- `{s['off']:#x}`{addr}{marker} {md(s['text'])}")

    lines.append("")
    lines.append("## Pointer References")
    lines.append("")
    lines.append("These are little-endian 32-bit references to selected strings. In qdsp6sw this often points at string tables or code/data tables near the actual use site.")
    lines.append("")
    any_refs = False
    for hit in sorted(hits, key=lambda h: h["off"]):
        refs = refs_by_off.get(hit["off"], [])
        if not refs:
            continue
        any_refs = True
        lines.append(f"### `{hit['off']:#x}` `{md(hit['text'])}`")
        lines.append("")
        lines.append("| ref file off | ref vaddr | ref paddr | target kind | target value |")
        lines.append("| --- | --- | --- | --- | --- |")
        for ref in refs:
            ref_vaddr = f"`{ref['ref_vaddr']:#x}`" if ref["ref_vaddr"] is not None else ""
            ref_paddr = f"`{ref['ref_paddr']:#x}`" if ref["ref_paddr"] is not None else ""
            lines.append(
                f"| `{ref['ref_off']:#x}` | {ref_vaddr} | {ref_paddr} | "
                f"`{ref['kind']}` | `{ref['value']:#x}` |"
            )
        lines.append("")
    if not any_refs:
        lines.append("_No selected pointer references found._")

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
        f.write("\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware")
    parser.add_argument("-o", "--output", default="/tmp/sdx55m-qdspsw-strings-report.md")
    parser.add_argument("--radius", type=lambda x: int(x, 0), default=0x300)
    args = parser.parse_args()

    segments = load_segments(args.firmware)
    strings = load_strings(args.firmware)
    hits = collect_hits(strings, segments)
    refs_by_off = pointer_refs(args.firmware, hits, segments)
    write_report(args.output, args.firmware, segments, strings, hits, refs_by_off, args.radius)

    print(f"wrote {args.output}")
    print(f"strings: {len(strings)}")
    print(f"hits: {len(hits)}")
    print(f"pointer target strings with refs: {sum(1 for v in refs_by_off.values() if v)}")


if __name__ == "__main__":
    main()
