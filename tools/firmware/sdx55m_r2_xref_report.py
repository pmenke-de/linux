#!/usr/bin/env python3
#
# Build a focused radare2 xref report for stripped Qualcomm modem ELFs.
#
# apps.mbn has no section headers, and rabin2 reports strings at file offsets.
# Translate each string through the PT_LOAD program headers before asking r2
# for references.

import argparse
import json
import os
import re
import subprocess
import tempfile
from collections import defaultdict


GROUPS = {
    "watchdog_sfr": [
        "Watchdog startup timeout",
        "Watchdog detects task starvation",
        "Dog Report Information",
        "End Dog Report",
        "SFR Init",
        "ssr:retrieve:sfr",
        "No SFR data available",
        "Modem wdog bite",
        "wdog bite",
        "Time of crash",
        "ERR crash log report",
    ],
    "rfs_efs_tftp": [
        "RFS_ASSERT",
        "rfs_info.init_done",
        "rfs_cfg",
        "remotefs",
        "remoteefs",
        "rfs_tftp",
        "rfs_nodev",
        "TFTP_GET",
        "TFTP_PUT",
        "TFTP_STAT",
        "TFTP_UNLINK",
        "efs_get",
        "efs_put",
        "MHI EFS",
        "RDM_MHI_EFS_DEV",
        "RDM_MHI_QMI_DEV",
    ],
    "ipa_mhi": [
        "IPA INIT",
        "IPA_EFS",
        "ipa_smp2p",
        "QMI_IPA",
        "INIT_MODEM_DRIVER",
        "MDM Driver init is pending",
        "ipa_mhi",
        "IPA HWP",
    ],
    "ssctl_sysmon": [
        "ssctl",
        "SSCTL",
        "sys_m",
        "SUBSYS",
        "SMP2P_SFR_READY",
        "MPSS_SSCTL",
    ],
    "mcfg_pdc": [
        "mcfg_",
        "MCFG",
        "mcfg_sw",
        "mcfg_hw",
        "pdc",
        "PDC",
    ],
}


def run(cmd, **kwargs):
    return subprocess.run(cmd, check=True, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, **kwargs)


def load_segments(firmware):
    out = run(["readelf", "-l", firmware]).stdout
    segments = []
    for line in out.splitlines():
        fields = line.split()
        if len(fields) < 7 or fields[0] != "LOAD":
            continue
        off = int(fields[1], 16)
        vaddr = int(fields[2], 16)
        filesz = int(fields[4], 16)
        memsz = int(fields[5], 16)
        flags = fields[6:-1]
        segments.append({
            "off": off,
            "vaddr": vaddr,
            "filesz": filesz,
            "memsz": memsz,
            "flags": "".join(flags),
        })
    return segments


def fileoff_to_va(segments, off):
    for seg in segments:
        if seg["off"] <= off < seg["off"] + seg["filesz"]:
            return seg["vaddr"] + (off - seg["off"])
    return None


def load_strings(firmware):
    data = json.loads(run(["rabin2", "-zzj", firmware]).stdout)
    return data.get("strings", [])


def wanted_strings(strings, segments):
    hits = []
    seen = set()
    for s in strings:
        text = s.get("string") or ""
        text_l = text.lower()
        off = int(s.get("paddr", s.get("vaddr", 0)))
        va = fileoff_to_va(segments, off)
        if va is None:
            continue
        for group, needles in GROUPS.items():
            for needle in needles:
                if needle.lower() not in text_l:
                    continue
                key = (group, off, text)
                if key not in seen:
                    seen.add(key)
                    hits.append({
                        "group": group,
                        "needle": needle,
                        "off": off,
                        "va": va,
                        "text": text,
                    })
                break
    hits.sort(key=lambda h: (h["group"], h["off"], h["text"]))
    return hits


def radare_xrefs(firmware, hits):
    commands = [
        "e scr.color=false",
        "e asm.bytes=false",
        "e asm.lines=false",
        "e bin.cache=true",
        "aaaa",
    ]
    for hit in hits:
        commands.append(f"?e __XREF_BEGIN__ {hit['va']:#x}")
        commands.append(f"axtj @ {hit['va']:#x}")
        commands.append(f"?e __XREF_END__ {hit['va']:#x}")
    commands.append("q")

    with tempfile.NamedTemporaryFile("w", delete=False) as f:
        f.write("\n".join(commands))
        f.write("\n")
        cmd_path = f.name

    try:
        proc = subprocess.run(
            ["r2", "-q", "-i", cmd_path, firmware],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    finally:
        os.unlink(cmd_path)

    by_va = {}
    current_va = None
    json_lines = []
    for line in proc.stdout.splitlines():
        begin = re.match(r"__XREF_BEGIN__ (0x[0-9a-fA-F]+)", line)
        end = re.match(r"__XREF_END__ (0x[0-9a-fA-F]+)", line)
        if begin:
            current_va = int(begin.group(1), 16)
            json_lines = []
            continue
        if end and current_va is not None:
            raw = "\n".join(json_lines).strip()
            try:
                by_va[current_va] = json.loads(raw) if raw else []
            except json.JSONDecodeError:
                by_va[current_va] = [{"parse_error": raw}]
            current_va = None
            json_lines = []
            continue
        if current_va is not None:
            json_lines.append(line)
    return by_va


def nearby_strings(strings, center, radius=0x300):
    lo = center - radius
    hi = center + radius
    out = []
    for s in strings:
        off = int(s.get("paddr", s.get("vaddr", 0)))
        if lo <= off <= hi:
            text = (s.get("string") or "").replace("\n", "\\n")
            if len(text) >= 5:
                out.append((off, text))
    return out


def md_escape(text):
    return text.replace("|", "\\|").replace("\n", "\\n")


def write_report(path, firmware, segments, hits, xrefs, strings):
    by_group = defaultdict(list)
    for hit in hits:
        by_group[hit["group"]].append(hit)

    lines = []
    lines.append(f"# SDX55M radare2 string xref report")
    lines.append("")
    lines.append(f"Firmware: `{firmware}`")
    lines.append("")
    lines.append("## Program header mapping")
    lines.append("")
    lines.append("| file off | vaddr | filesz | memsz | flags |")
    lines.append("| --- | --- | --- | --- | --- |")
    for seg in segments:
        lines.append(
            f"| `{seg['off']:#x}` | `{seg['vaddr']:#x}` | "
            f"`{seg['filesz']:#x}` | `{seg['memsz']:#x}` | `{seg['flags']}` |"
        )

    for group in GROUPS:
        group_hits = by_group.get(group, [])
        lines.append("")
        lines.append(f"## {group}")
        lines.append("")
        if not group_hits:
            lines.append("_No string hits._")
            continue
        lines.append("| off | va | xrefs | string |")
        lines.append("| --- | --- | ---: | --- |")
        for hit in group_hits:
            refs = xrefs.get(hit["va"], [])
            lines.append(
                f"| `{hit['off']:#x}` | `{hit['va']:#x}` | {len(refs)} | "
                f"{md_escape(hit['text'])} |"
            )

        lines.append("")
        lines.append("### Xrefs")
        any_refs = False
        for hit in group_hits:
            refs = xrefs.get(hit["va"], [])
            if not refs:
                continue
            any_refs = True
            lines.append("")
            lines.append(f"- `{hit['va']:#x}` `{md_escape(hit['text'])}`")
            for ref in refs:
                if "parse_error" in ref:
                    lines.append(f"  - parse error: `{md_escape(ref['parse_error'])}`")
                    continue
                src = ref.get("from")
                src_s = f"{src:#x}" if isinstance(src, int) else str(src)
                fcn = ref.get("fcn_name") or "nofunc"
                op = ref.get("opcode") or ""
                typ = ref.get("type") or ""
                lines.append(f"  - `{src_s}` `{typ}` `{fcn}` `{md_escape(op)}`")
        if not any_refs:
            lines.append("")
            lines.append("_No xrefs found by radare2 analysis for this group._")

    lines.append("")
    lines.append("## Nearby string clusters")
    lines.append("")
    cluster_centers = []
    for hit in hits:
        if hit["group"] in ("watchdog_sfr", "rfs_efs_tftp", "ipa_mhi"):
            cluster_centers.append(hit["off"])
    for center in sorted(set(cluster_centers)):
        nearby = nearby_strings(strings, center)
        if len(nearby) < 2:
            continue
        lines.append(f"### around `{center:#x}`")
        for off, text in nearby[:40]:
            lines.append(f"- `{off:#x}` {md_escape(text)}")
        lines.append("")

    with open(path, "w") as f:
        f.write("\n".join(lines))
        f.write("\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware")
    parser.add_argument(
        "-o", "--output",
        default="/tmp/sdx55m-r2-xref-report.md",
        help="Markdown report path",
    )
    args = parser.parse_args()

    segments = load_segments(args.firmware)
    strings = load_strings(args.firmware)
    hits = wanted_strings(strings, segments)
    xrefs = radare_xrefs(args.firmware, hits)
    write_report(args.output, args.firmware, segments, hits, xrefs, strings)

    total_refs = sum(len(v) for v in xrefs.values())
    print(f"wrote {args.output}")
    print(f"strings: {len(hits)}")
    print(f"xrefs: {total_refs}")


if __name__ == "__main__":
    main()
