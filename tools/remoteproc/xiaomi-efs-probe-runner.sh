#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

PROBE=${PROBE:-/usr/local/bin/xiaomi_efs_probe}
LOG_PREFIX="xiaomi-efs-probe-runner:"

log()
{
	printf '%s %s\n' "$LOG_PREFIX" "$*"
}

run_probe()
{
	mode=$1
	sub=$2
	path=$3

	log "probe mode=$mode sub=$sub path=$path"
	"$PROBE" -t 800 -m "$mode" -u "$sub" "$path" || true
}

run_probe_write_u32()
{
	mode=$1
	sub=$2
	path=$3
	value=$4

	# Writes can sit behind PDC LOAD's NV-manager activity during early
	# MISSION, so allow up to 4s for the modem to RESP.  Still leaves margin
	# before MISSION+~11s when the MCFG-Refresh STM watchdog fires.
	log "probe-write mode=$mode sub=$sub path=$path value=$value"
	"$PROBE" -t 4000 -m "$mode" -u "$sub" -o write -D "$value" "$path" || true
}

uptime_int()
{
	cut -d. -f1 /proc/uptime
}

log "starting uptime=$(cut -d' ' -f1 /proc/uptime)"

timeout_ms=38000
if "$PROBE" -t "$timeout_ms" --lookup-only; then
	log "service found uptime=$(cut -d' ' -f1 /proc/uptime)"
else
	log "service not found before timeout=${timeout_ms}ms uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_EFS_DUAL_DISCOVERY:-0}" = 1 ]; then
	# Discover dual-path layout for op=11 (compare) / op=12 (copy).
	#
	# Firmware printf strings prove these ops carry TWO paths in the inner
	# 512B buffer: "xiaomi_efs_compare, path_len_src = %d, path_len_tgt = %d".
	# V4 OPCODE_SCAN saw op=11/12 return -111 (not_supported) when only the
	# single-path slot was populated — modem rejects a malformed dual-path
	# request, but the dispatcher itself is reachable.
	#
	# Strategy: send op=11 compare(autoselect, autoselect) — same path twice,
	# both already on disk, both readable. Compare is read-only, so even a
	# misinterpreted layout cannot corrupt state. With each candidate
	# layout, expect:
	#   wrong layout : status=-111 / -1 (rejected, same as V4)
	#   right layout : status=0 with equal=true (or non-(-111) structured
	#                  response) — modem decoded both paths.
	#
	# Layouts under test (set in xiaomi_efs_probe enum dual_layout):
	#   slot80 : path_len_tgt @ BUF[0x80], path_tgt @ BUF[0x81..]
	#   slot40 : path_len_tgt @ BUF[0x40]
	#   slot4c : path_len_tgt @ BUF[0x4c] (mirrors op=5 data_len_off)
	#   packed : path_len_tgt @ BUF[5+src_len]
	#
	# Budget: pre-canary + 4 layouts + post-canary = 6 probes × ~700ms = 4.2s.
	# Modem ERRFATAL hits at MISSION+~10s and the service appears at MISSION+~3s,
	# so 4.2s fits comfortably.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "dual-discovery: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "dual-discovery: pre-canary read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	# Same path src and tgt — compare(autoselect, autoselect) should match
	# trivially when the layout decodes correctly.
	for layout in slot80 slot40 slot4c packed; do
		log "dual-discovery: op=11 compare layout=$layout uptime=$(cut -d' ' -f1 /proc/uptime)"
		"$PROBE" -t 800 -m vendor -u 0 -o op11 \
			--tgt-path "$canary_path" --dual-layout "$layout" \
			"$canary_path" || true
	done

	log "dual-discovery: post-canary read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "dual-discovery: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_EFS_NV_PROBE:-0}" = 1 ]; then
	# Re-mapped op codes (verified by r2ghidra of /vendor/bin/mtb on 2026-04-29):
	#   op=1 = xiaomi_nv_read   (numeric NV; nv_id at BUF[2..3])
	#   op=2 = xiaomi_nv_write_ex (numeric NV write; data at BUF[0x58])
	#   op=3 = xiaomi_nv_delete (numeric NV delete)
	#
	# v1 NV scan (2026-04-29) results:
	#   nv_id=1, 21, 453: op=1 read returns clean response. Wire format
	#     confirmed: response inner buf[0..3]=op_echo, buf[4..7]=0x19b
	#     fixed quota, buf[0x10..0x13]=status (0=ok, -108 for nv_id=21
	#     "not present"), buf[0x14..]=data.
	#   nv_id=453: data byte 0 = 0x01 (Operating Mode = FTM if NV 453 has
	#     standard QC mapping).
	#   nv_id=455: read WEDGES port 3:8. Skip in further tests.
	#   nv_id=460, 480, 4964: untested (port already wedged).
	#
	# This pass: round-trip NV 453 through op=2 to determine if op=2
	# wedges (like path-EFS op=5 does) or works.  If op=2 succeeds, we
	# have a fresh, working write surface for any modem behavior
	# controlled by a numeric NV.  Write the SAME byte we just read so
	# this is a strict no-op.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "nv-probe: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	scan_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "nv-probe: pre-canary read sub=0 (op=4) on $scan_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$scan_path" || true

	# Re-confirm NV 453 read first (cheap canary on the NV path).
	log "nv-probe: pre-write op=1 nv_read nv_id=453"
	"$PROBE" -t 1500 -u 0 -o nv_read --nv-id 453 || true

	log "nv-probe: mid-canary read sub=0 (op=4) on $scan_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$scan_path" || true

	# THE TEST: write the same byte (0x01) back to NV 453 via op=2.  4s
	# timeout to leave margin before MISSION+~15s ERRFATAL.  If op=2
	# returns success cleanly and the next read shows the same value, we
	# have a working numeric-NV write surface.  If it times out + the
	# canary read also times out, op=2 is dead like op=5.
	log "nv-probe: op=2 nv_write nv_id=453 data=0x01 (no-op self-write)"
	"$PROBE" -t 4000 -u 0 -o nv_write --nv-id 453 -d 01 || true

	log "nv-probe: post-write op=4 path-EFS canary on $scan_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$scan_path" || true

	log "nv-probe: post-write op=1 nv_read nv_id=453 (expect same 0x01)"
	"$PROBE" -t 1500 -u 0 -o nv_read --nv-id 453 || true

	log "nv-probe: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_CMN_HANDLER_SCAN_V10:-0}" = 1 ]; then
	# V10: extend op=6 multiplexer enumeration past sub-cmd=10 + test
	# alternate request layout for path-handler discovery.
	#
	# V9 strongly suggests sub-cmds 5/6/7/8/10 are STUBS (return 0 with no
	# data regardless of 200-byte input).  sub-cmd=4 is a real validator
	# (only arg1=0 accepts).  But mcfg_ext_set_rfs_params is the prime
	# target and may live at a higher sub-cmd, or use a different request
	# layout than the simple `BUF[4..7]=sub-cmd, BUF[8..]=u32 args` we've
	# been testing.
	#
	# Three tests in one boot:
	#
	# 1. Enumerate sub-cmds 11..15 (0x0b..0x0f) empty.  Find upper bound or
	#    hidden live handlers.  If any returns non-zero data at BUF[0x10+],
	#    that's likely `sw_version` reading (last string in source order
	#    at qdsp6sw.mbn 26245146+).
	#
	# 2. sub-cmd=8 with EFS-style path layout: BUF[8]=path_len(u8),
	#    BUF[9..]=path bytes.  If mcfg_ext_set_rfs_params uses this
	#    layout, expect different response shape (status echo, path echoed
	#    in BUF[0x10+], or rejection with -1).
	#
	#      Payload hex: 08000000 0a 2f6566732f74657374
	#        BUF[4..7] = 08 00 00 00  (sub-cmd 8)
	#        BUF[8]    = 0a           (path_len = 10)
	#        BUF[9..18]= "/efs/test"  + nul
	#
	# 3. sub-cmd=11 with same EFS-style path layout.  If 11 is the live
	#    path-handler we're after, this catches it directly.
	#
	# Safety: enumerate-only approach.  No single probe is destructive.
	# Budget: 1 canary + 5 empty-enum + 2 path-layout + 1 canary
	#   = 9 probes × ~700ms ≈ 6.9s, fits MISSION+~8.3s budget.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "cmn-scan-v10: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "cmn-scan-v10: pre-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	# Enumerate sub-cmds 11..15 empty.  Note: sub-cmd values are u32 LE,
	# so 11=0x0b, 12=0x0c, 13=0x0d, 14=0x0e, 15=0x0f.
	for subcmd_hex in 0b 0c 0d 0e 0f; do
		log "cmn-scan-v10: sub-cmd=0x$subcmd_hex empty"
		"$PROBE" -t 600 -C -o op6 -d "${subcmd_hex}000000" || true
	done

	# EFS-style path layout for sub-cmd=8 and sub-cmd=11.
	# Path: "/efs/test" = 9 chars + nul = 10 bytes.
	# Hex: "/efs/test\0" = 2f6566732f7465737400
	# Full payload: subcmd(4B) + path_len(1B) + path(10B) = 15B
	log "cmn-scan-v10: sub-cmd=8 with EFS-layout path (path_len=10, '/efs/test')"
	"$PROBE" -t 800 -C -o op6 -d "080000000a2f6566732f7465737400" || true

	log "cmn-scan-v10: sub-cmd=11 with EFS-layout path (path_len=10, '/efs/test')"
	"$PROBE" -t 800 -C -o op6 -d "0b0000000a2f6566732f7465737400" || true

	log "cmn-scan-v10: post-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "cmn-scan-v10: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_CMN_HANDLER_SCAN_V9:-0}" = 1 ]; then
	# V9: disambiguate stub-vs-real for op=6 sub-cmds 5/6/7/8 + confirm
	# sub-cmd=4 identity + extend enumeration to sub-cmd=10.
	#
	# V8 found:
	#   sub-cmd=4 + 12-byte path → status=-1 (real validator)
	#   sub-cmd=5/6/7 + 12-byte path → status=0 with arg1 echoed
	#   sub-cmd=8/9 empty → status=0 (untested with payload)
	#
	# Two open questions V9 resolves:
	#
	# 1. **Is sub-cmd=4 = xiaomi_rf_tx_test_set_hdl?**  Send valid args:
	#    v_ant=1, v_sys=2, v_band_list=3 — small u32s that should pass the
	#    `Xiaomi, sys is invalid, can not > %d` validator.  If status flips
	#    from -1 to 0, identity confirmed.
	#
	# 2. **Are sub-cmds 5/6/7/8 real path-handlers or stubs?**  Send a
	#    200-byte ASCII path.  A real path-handler (e.g., mcfg_ext_set_rfs_params
	#    taking oem_sw_path_info) should either reject (path_len overflow,
	#    status=-1) or accept with response data containing path info.  A stub
	#    would still return 0 with empty data — same as the 12-byte case.
	#
	#    mcfg_ext_set_rfs_params strings appear AFTER SIM detection in source
	#    order (qdsp6sw.mbn 26244739), so it likely lives at sub-cmd 8+.  We
	#    test 5/6/7/8 to find it.
	#
	# 3. **Extend enumeration**: sub-cmd=10 with empty payload.
	#
	# Safety:
	#  - mcfg_ext_set_rfs_params writes rfs_params struct in-RAM only (not
	#    persisted to flash), so corruption resets on reboot.
	#  - rf_tx_test with v_ant=1/v_sys=2/v_band_list=3 may toggle RF antennas
	#    momentarily — observable but harmless and reverts at reboot.
	#
	# Budget: 1 canary + 1 sub4-valid + 4 sub5/6/7/8-longpath + 1 sub10-empty
	#   + 1 canary = 8 probes ≈ 7.5s, fits in MISSION+~10s heavy-probe window.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "cmn-scan-v9: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "cmn-scan-v9: pre-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	# Question 1: sub-cmd=4 with valid rf_tx_test args.
	# Payload at BUF[4..]:
	#   BUF[4..7]   = 04 00 00 00  (sub-cmd 4)
	#   BUF[8..11]  = 01 00 00 00  (v_ant=1)
	#   BUF[12..15] = 02 00 00 00  (v_sys=2)
	#   BUF[16..19] = 03 00 00 00  (v_band_list=3)
	log "cmn-scan-v9: sub-cmd=4 with valid rf_tx_test args (v_ant=1, v_sys=2, v_band_list=3)"
	"$PROBE" -t 1500 -C -o op6 -d "04000000010000000200000003000000" || true

	# Question 2: 200-byte ASCII 'A' path (0x41) at BUF[8..207].
	# 32 'A' = 64 hex chars; 6× = 192 'A'; +8 'A' = 200.
	a32="4141414141414141414141414141414141414141414141414141414141414141"
	long_path_hex="${a32}${a32}${a32}${a32}${a32}${a32}4141414141414141"
	for subcmd in 05 06 07 08; do
		log "cmn-scan-v9: sub-cmd=$subcmd with 200-byte path"
		"$PROBE" -t 800 -C -o op6 -d "${subcmd}000000${long_path_hex}" || true
	done

	# Question 3: extend to sub-cmd=10 (0x0a).
	log "cmn-scan-v9: sub-cmd=10 empty"
	"$PROBE" -t 800 -C -o op6 -d "0a000000" || true

	log "cmn-scan-v9: post-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "cmn-scan-v9: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_CMN_HANDLER_SCAN_V8:-0}" = 1 ]; then
	# V8: probe op=6 sub-cmds 4-7 with structured PATH payloads + enumerate
	# sub-cmds 8, 9.
	#
	# V7 found sub-cmds 4, 5, 6, 7 all return status=0 with empty payload,
	# but with NO return data — could be real handlers waiting for input,
	# or stub no-ops.  Decisive test: send a 12-byte ASCII path string at
	# BUF[8..].  If a handler is `mcfg_ext_set_rfs_params`, it parses the
	# path as `oem_sw_path_info` or `oem_hw_path_info` and may produce
	# different response data (or wedge if the path triggers a flash op).
	#
	# Path payload: "/test_path\0\0" = 12 bytes hex 2f746573745f706174680000
	# Per probe data: 4-byte sub-cmd at BUF[4..7] + 12-byte path at BUF[8..19]
	#   = 16 bytes hex.
	#
	# Concerns:
	#   - mcfg_ext_set_rfs_params writes rfs_params struct in-memory (not
	#     persistent EFS), so corruption is reset by reboot. SAFE.
	#   - Non-mcfg handlers may interpret BUF[8..] differently and produce
	#     undefined behavior.  Mitigated by 1000ms timeout + post-canary.
	#
	# Budget: 1 canary + 4 sub-cmd-with-path + 2 sub-cmd-empty + 1 canary
	#   = 8 probes ≈ 9s, fits in MISSION+~10s heavy-probe window.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "cmn-scan-v8: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "cmn-scan-v8: pre-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	# Probe sub-cmds 4..7 with structured path at BUF[8..].  Path =
	# "/test_path\0\0" = 0x2f 0x74 0x65 0x73 0x74 0x5f 0x70 0x61 0x74 0x68 0x00 0x00
	path_hex="2f746573745f706174680000"
	for subcmd in 04 05 06 07; do
		log "cmn-scan-v8: cmn op=6 sub-cmd=$subcmd with path /test_path"
		"$PROBE" -t 1000 -C -o op6 -d "${subcmd}000000${path_hex}" || true
	done

	# Extend enumeration to sub-cmds 8, 9 (empty payload).
	for subcmd in 08 09; do
		log "cmn-scan-v8: cmn op=6 sub-cmd=$subcmd empty"
		"$PROBE" -t 1000 -C -o op6 -d "${subcmd}000000" || true
	done

	log "cmn-scan-v8: post-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "cmn-scan-v8: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_CMN_HANDLER_SCAN_V7:-0}" = 1 ]; then
	# V7: enumerate op=6 sub-cmds 4-7.  V6 confirmed op=6 is the
	# `xiaomi_modem_common_data_msg_hdl` multiplexer:
	#   sub-cmd=1: status=0 (clean dispatch — likely efs_sync)
	#   sub-cmd=2: status=0 (clean dispatch — likely efs_make_golden_copy)
	#   sub-cmd=3: WEDGES port 3:8 (likely GPIO control DalTlmm)
	#   sub-cmd=0/empty: status=-1 (unknown sub-cmd)
	#
	# Source-order in qdsp6sw.mbn after the multiplexer string suggests:
	#   sub-cmd 1-3: efs_sync, efs_make_golden_copy, GPIO control
	#   sub-cmd 4-7: SIM card detection, mcfg_ext_set_rfs_params (sw + hw),
	#                version reading
	# `mcfg_ext_set_rfs_params` is THE prime target — it directly manipulates
	# rfs_params struct that controls MCFG-Refresh-STM behavior.
	#
	# Strategy: probe sub-cmds 4, 5, 6, 7 in sequence.  If one wedges, the
	# rest will lookup-fail but the journal tells us which.  Use 1000ms
	# timeout to keep total budget < 10s (MISSION+~10s ERRFATAL window).
	# Run sub-cmd=1 first as positive control (port-alive marker).
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "cmn-scan-v7: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "cmn-scan-v7: pre-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	# Positive control: sub-cmd=1 known good (status=0, fast).
	log "cmn-scan-v7: positive-control op=6 sub-cmd=1 (known good)"
	"$PROBE" -t 800 -C -o op6 -d "01000000" || true

	# Probe sub-cmds 4..7 each with 1000ms timeout.  Skip sub-cmd=3 (wedger).
	for subcmd in 04000000 05000000 06000000 07000000; do
		log "cmn-scan-v7: cmn op=6 sub-cmd BUF[4..7]=$subcmd"
		"$PROBE" -t 1000 -C -o op6 -d "$subcmd" || true
	done

	log "cmn-scan-v7: post-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "cmn-scan-v7: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_CMN_HANDLER_SCAN_V6:-0}" = 1 ]; then
	# V6: disambiguate op=6 — is it (a) the second-level multiplexer
	# `xiaomi_modem_common_data_msg_hdl` rejecting an empty sub-cmd with -1,
	# or (b) `xiaomi_rf_tx_test_set_hdl` rejecting an empty payload because
	# v_sys validation fails?
	#
	# Firmware strings (qdsp6sw.mbn 26242951-26243561) show the cmn_handler
	# has TWO `not supported cmd` messages:
	#   26243459: xiaomi_extend_qmi_cmn_handler, not supported cmd(%d)  → -4093
	#   26243561: xiaomi_modem_common_data_msg_hdl, not supported cmd(%d) → ?
	# Op=6 returning -1 (FF FF FF FF), which is DIFFERENT from -4093, fits the
	# second-level dispatcher's unknown-cmd path.
	#
	# Test plan:
	#   1. Re-baseline op=6 empty (expect BUF[4..7]=ff ff ff ff)
	#   2. op=6 with sub-cmd 1, 2, 3, 4, 5, 6, 7, 8 at BUF[4..7] as u32 LE.
	#      If op=6 is the multiplexer, some sub-cmd values should produce
	#      DIFFERENT responses (recognized handlers).  If op=6 is rf_tx_test,
	#      all permutations should still return -1 because v_sys=0 fails
	#      validation independently.
	#   3. op=6 with structured rf_tx_test shape (v_ant=1, v_sys=0, v_band_list=0).
	#      If op=6 is rf_tx_test and we pass v_sys=0, validation passes —
	#      response should differ from -1.
	#
	# Op=6 is non-wedging per V1, so we can chain many probes in one boot.
	# Each probe ~500ms; total budget ~6s well within the MISSION+~10s window.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "cmn-scan-v6: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "cmn-scan-v6: pre-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	# Positive control: confirm wire format still works (op=1 imei_protect magic).
	log "cmn-scan-v6: positive-control cmn op=1 with imei_protect magic"
	"$PROBE" -t 1500 -C -o op1 -d "ca1a00009a9999999999b93f" || true

	# Baseline: re-run op=6 with empty payload.  Expected: BUF[4..7]=ff ff ff ff.
	log "cmn-scan-v6: baseline cmn op=6 empty payload"
	"$PROBE" -t 800 -C -o op6 || true

	# Sub-command sweep at BUF[4..7].  If op=6 is the multiplexer, recognized
	# sub-cmds produce different status; unrecognized still -1.
	for subcmd in 01000000 02000000 03000000 04000000 05000000 06000000 07000000 08000000; do
		log "cmn-scan-v6: cmn op=6 sub-cmd BUF[4..7]=$subcmd"
		"$PROBE" -t 800 -C -o op6 -d "$subcmd" || true
	done

	# rf_tx_test shape: v_ant=1, v_sys=0, v_band_list=0 (3 u32 LE = 12 bytes).
	# v_sys=0 should pass validation (firmware checks `sys > N`).
	log "cmn-scan-v6: cmn op=6 rf_tx_test-shaped payload v_ant=1,v_sys=0,v_band_list=0"
	"$PROBE" -t 1500 -C -o op6 -d "01000000000000000000000000000000" || true

	log "cmn-scan-v6: post-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "cmn-scan-v6: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_CMN_HANDLER_SCAN_V5:-0}" = 1 ]; then
	# V5: enumerate ops 13/14/15.  V4 confirmed op=12 is fast not_supported
	# (-4093) — V3's timeout was ERRFATAL coincidence.  V4 also confirmed
	# that a single cmn op preserves the natural MISSION+15s ERRFATAL
	# baseline; the MISSION+10s acceleration in V1/V2/V3 came from cumulative
	# probe activity.  So 3 ops + canaries is reasonable budget.
	#
	# Given the pattern (ops 9, 10, 11, 12 all fast not_supported), 13-15
	# are most likely also not_supported.  But we should confirm — they
	# could be mcfg_ext_set_rfs_params, efs_sync, efs_make_golden_copy,
	# GPIO ops, etc.  Any non-(-4093) status, or a wedge like op=8, is
	# strategically interesting.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "cmn-scan-v5: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "cmn-scan-v5: pre-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	for op in 13 14 15; do
		log "cmn-scan-v5: cmn op=$op uptime=$(cut -d' ' -f1 /proc/uptime)"
		"$PROBE" -t 1200 -C -o "op${op}" || true
	done

	log "cmn-scan-v5: post-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "cmn-scan-v5: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_CMN_HANDLER_SCAN_V4:-0}" = 1 ]; then
	# V4: isolate op=12.  V3 found ops 9/10/11 are fast not_supported (-4093,
	# same as 0/3/7) but op=12 timed out at 1200ms.  Critically, ERRFATAL
	# fired at uptime 37.57s while op=12 was sent at 36.55s — that's ~1.0s
	# into the op=12 wait, so we cannot tell whether op=12 wedged or whether
	# the modem just crashed coincident with normal MISSION+~10s ERRFATAL.
	# Ops 13/14/15 + post-canary all "service lookup failed" because the
	# modem was crashed, not because op=12 destroyed the port.
	#
	# V4 strategy: probe op=12 FIRST (no pre-canary) with 4000ms timeout.
	# Service comes up at MISSION+~5s, ERRFATAL at MISSION+~10s, so 4s gives
	# op=12 the longest possible window to respond cleanly before ERRFATAL.
	# Skip op=8 (known wedger).  After op=12, do a canary to see whether
	# op=12 wedged the port (canary EAGAIN) or worked (canary OK).
	#
	# Three possible outcomes for op=12:
	#   1. Fast not_supported (<1s, BUF[4..7]=03 f0 ff 0f): not a real op,
	#      V3's "timeout" was just ERRFATAL coincidence.
	#   2. Slow not_supported (~1s, same data): same as op=7, harmless.
	#   3. Real handler: structured non-(-4093) status, OR 4s timeout +
	#      canary EAGAIN = wedger like op=8.  Either is INTERESTING.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "cmn-scan-v4: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim

	# Run op=12 first so it has the most time before ERRFATAL.
	log "cmn-scan-v4: cmn op=12 with 4000ms timeout uptime=$(cut -d' ' -f1 /proc/uptime)"
	"$PROBE" -t 4000 -C -o op12 || true

	log "cmn-scan-v4: post-op12 canary EFS read on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "cmn-scan-v4: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_CMN_HANDLER_SCAN_V3:-0}" = 1 ]; then
	# V3: enumerate cmn_handler ops 9-15.  V1/V2 left them untested because
	# op=8 wedged port 3:8 and 9+ all received EAGAIN.  V3 SKIPS op=8
	# entirely and probes 9-15 in sequence.  The V2 result for op=7
	# (delayed -4093 not_supported) tells us many ops respond in ~1s, so
	# 1200ms per op gives slow-not_supported responses room to arrive.
	#
	# Total budget: 7 ops × 1200ms = 8.4s worst case + 800ms pre-canary +
	# 1500ms post-canary = 10.7s.  Probe activity compresses MISSION→ERRFATAL
	# to ~10s, so this is tight.  If a wedge happens mid-scan, post-canary
	# EAGAINs and we'll need a follow-up V4 to bisect which op wedged.
	#
	# Goal: find another live cmn_handler op besides op=8.  Real handlers
	# return non-(-4093) status in BUF[4..7], like op=1 imei_protect did
	# (status=5, length=124).  Anything that returns -4093 is just the
	# firmware's "not supported cmd" path.
	#
	# Strategic value: cmn_handler exposes mcfg_ext_set_rfs_params, efs_sync,
	# efs_make_golden_copy among unknown op #s.  Finding which op # maps to
	# mcfg_ext_set_rfs_params is the most direct path to disabling
	# MCFG-Refresh-STM (manipulates rfs_params struct directly).
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "cmn-scan-v3: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "cmn-scan-v3: pre-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	# Probe 9-15 in order.  No per-op canary — burns too much budget.
	# 1200ms per op catches slow-not_supported (op=7 was ~900ms).  Wedgers
	# will time out and contaminate later ops via EAGAIN, but the post-canary
	# tells us SOMETHING wedged in the range; bisect in V4 if needed.
	for op in 9 10 11 12 13 14 15; do
		log "cmn-scan-v3: cmn op=$op uptime=$(cut -d' ' -f1 /proc/uptime)"
		"$PROBE" -t 1200 -C -o "op${op}" || true
	done

	log "cmn-scan-v3: post-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "cmn-scan-v3: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_CMN_HANDLER_SCAN_V2:-0}" = 1 ]; then
	# v2 follow-up to V1.  V1 (with corrected wire format: TLV 0x10 fixed
	# 512B, TLV 0x02=0x1ac per QCCI struct) found:
	#   op=1 with imei_protect magic: REAL response (positive control passes)
	#   op=1 empty: BUF[0x10]=04 (status=4, "missing param"-shaped error)
	#   op=0:  BUF[4..7]=03 f0 ff 0f (-4093, "not_supported"); cleanly rejected
	#   op=3:  same as op=0 — not_supported
	#   op=6:  BUF[4..7]=ff ff ff ff (-1) — different error path
	#   op=7:  TIMED OUT at 800ms
	#   op=8:  TIMED OUT at 800ms
	#   op=9+: send EAGAIN — port wedged
	#   post-canary: EAGAIN — port wedged
	#
	# Follow-up result (2026-04-30): op=7 returns a clean response and the
	# EFS canary still works.  op=8 times out, wedges port 3:8, and moves the
	# modem ERRFATAL from the natural MISSION+~15s baseline to MISSION+~10s.
	# That makes op=8 diagnostic noise for normal runs, so keep it behind an
	# explicit opt-in flag.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "cmn-scan-v2: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "cmn-scan-v2: pre-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	# Re-do positive control to establish that wire format still works
	# (different reboot, different modem state).
	log "cmn-scan-v2: positive-control cmn op=1 with imei_protect magic"
	"$PROBE" -t 1500 -C -o op1 -d "ca1a00009a9999999999b93f" || true

	log "cmn-scan-v2: post-pos-control canary EFS read on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	# Op=7: 5s timeout to let a long-running handler complete.
	log "cmn-scan-v2: cmn op=7 with 5000ms timeout uptime=$(cut -d' ' -f1 /proc/uptime)"
	"$PROBE" -t 5000 -C -o op7 || true

	log "cmn-scan-v2: post-op7 canary EFS read on $canary_path (EAGAIN = op=7 wedged)"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	if [ "${XIAOMI_CMN_HANDLER_TEST_OP8:-0}" = 1 ]; then
		log "cmn-scan-v2: DANGEROUS cmn op=8 with 5000ms timeout uptime=$(cut -d' ' -f1 /proc/uptime)"
		"$PROBE" -t 5000 -C -o op8 || true

		log "cmn-scan-v2: post-op8 canary EFS read on $canary_path"
		"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true
	else
		log "cmn-scan-v2: skipping cmn op=8; confirmed port wedger, set XIAOMI_CMN_HANDLER_TEST_OP8=1 to retry"
	fi

	log "cmn-scan-v2: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_CMN_HANDLER_SCAN_V1:-0}" = 1 ]; then
	# Enumerate xiaomi_extend_qmi_cmn_handler ops.  This is a SECOND firmware
	# dispatcher discovered by string analysis (qdsp6sw.mbn 0x1907113), distinct
	# from xiaomi_extend_qmi_efs_handler.  Found by disassembling mtb:
	#   xiaomi_extend_imei_protect      → req_id=1, op=1 (read), op=2 (write)
	#   xiaomi_extend_nv_backup          → req_id=1, op=4
	#   xiaomi_extend_nv_restore         → req_id=1, op=5
	# Buffer layout differs from efs_handler:
	#   BUF[0..3] = op (u32 LE), BUF[4..] = arg(s) — total inner buf 0x1ac (428B).
	#   QMI envelope: TLV 0x01=req_id 1, TLV 0x02=u32 0x1ac, TLV 0x10=428B.
	# cmn_handler exposes (per firmware strings near 0x1bf6c00):
	#   - imei_protect r/w
	#   - nv_backup / nv_restore
	#   - efs_sync
	#   - efs_make_golden_copy
	#   - mcfg_ext_set_rfs_params (sw/hw path info)
	#   - GPIO config / read (rf_ant_port_config, ant_tx_config)
	#   - card slot init gpio info
	#   - rf_tx_test_set_hdl
	#   - qmi_print_flag
	# Firmware error string `xiaomi_extend_qmi_cmn_handler, not supported cmd(%d)`
	# at 0x1907123 confirms enumeration is SAFE: unknown ops error cleanly.
	#
	# DESTRUCTIVE OP SKIPLIST: skip op=2 (imei_protect WRITE), op=4 (nv_backup;
	# 10s timeout in mtb means it may write), op=5 (nv_restore; 60s timeout, can
	# corrupt NV state).  Probe only ops {0, 1, 3, 6, 7, 8, 9, 10, 11, 12, 13,
	# 14, 15} = 13 ops.  Each with 800ms timeout; most should return -111
	# (not_supported_cmd) quickly.
	#
	# Canary: op=4 read on efs_handler (req_id=2, default mode) before and
	# after the scan to confirm port 3:8 stays alive.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "cmn-scan: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "cmn-scan: pre-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	# Positive control: cmn op=1 with imei_protect magic payload — mtb's
	# xiaomi_extend_imei_protect read calls send_sync with BUF[0..3]=01
	# BUF[4..7]=ca 1a 00 00 (=0x1aca) BUF[8..15]=double 0.1 (9a 99 99 99
	# 99 99 b9 3f).  If our wire format is right, this should produce a
	# REAL imei_protect response (not MALFORMED).  If it still rejects,
	# there's another wire-format invariant we are missing.
	log "cmn-scan: positive-control cmn op=1 with imei_protect magic payload"
	"$PROBE" -t 1500 -C -o op1 -d "ca1a00009a9999999999b93f" || true

	# Probe ops in order with empty payload.  op=1 first (imei_protect
	# read empty — should reject with a specific error if our format is
	# right, MALFORMED if not).
	for op in 1 0 3 6 7 8 9 10 11 12 13 14 15; do
		log "cmn-scan: trying cmn op=$op uptime=$(cut -d' ' -f1 /proc/uptime)"
		"$PROBE" -t 800 -C -o "op${op}" || true
	done

	log "cmn-scan: post-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "cmn-scan: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_EFS_OP10_FULLDUMP:-0}" = 1 ]; then
	# v4 confirmed only op=10 is "extra" (firmware-exposed beyond mtb's 1-9).
	# Ops 0/11/12/13/14/15 all return -111 = "not supported cmd".
	# op=10 returns success but the visible 128B window of the response is
	# all zeros except the op echo.  Probe with the rebuilt binary that
	# dumps 256B to check whether a size/result field lives in [0x80..0xff].
	#
	# Test both an existing path (size > 0 expected) and an absent path
	# (size 0 / errno expected) to confirm size-vs-stub semantics.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "op10dump: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	exist_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	absent_path=/nv/item_files/mcfg/ignore_modem_reset

	log "op10dump: pre-canary read sub=0 (op=4) on $exist_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$exist_path" || true

	log "op10dump: op=10 sub=0 on EXISTING $exist_path (expect size in 256B dump)"
	"$PROBE" -t 800 -m vendor -u 0 -o op10 "$exist_path" || true

	log "op10dump: op=10 sub=0 on ABSENT $absent_path (expect 0 or errno)"
	"$PROBE" -t 800 -m vendor -u 0 -o op10 "$absent_path" || true

	# Compare against op=9 (xiaomi_efs_stat) which returns a struct
	# containing size at known offset 0x18.  If op=10's response differs
	# in shape from op=9, op=10 is NOT just a duplicate stat path.
	log "op10dump: op=9 stat sub=0 on $exist_path (control)"
	"$PROBE" -t 800 -m vendor -u 0 -o op9 "$exist_path" || true

	log "op10dump: post-canary read sub=0 (op=4) on $exist_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$exist_path" || true

	log "op10dump: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_EFS_OPCODE_SCAN_V4:-0}" = 1 ]; then
	# v4: tighter scan after v3 hit ERRFATAL mid-run.  v3 confirmed
	# unknown ops do NOT wedge per-call (ops 10/11/12 all responded
	# cleanly), so mid-canaries are unnecessary.  Drop them and use
	# 800ms timeouts so all 7 ops fit in the ~7s window between the
	# service becoming reachable (MISSION+~3s) and ERRFATAL (MISSION+~10s).
	#
	# Findings to confirm or extend:
	#   op=10: status=0 success, all-zero data. Possibly xiaomi_efs_size
	#     but no size in 0..127 of inner buf.  Re-probe with sub=1.
	#   op=11: status=1, EFS errno -111 at inner[0x10].  Likely needs
	#     2-path layout (compare/copy).
	#   op=12: status=1, EFS errno -111 same shape.  Other of compare/copy.
	#   op=13/14/15/0: untested.
	#
	# Extend: probe op=10 with sub=0 AND sub=1 to see if size returns
	# differently per sub.  Probe ops 13/14/15/0 to enumerate.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "opscan4: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	scan_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "opscan4: pre-canary read sub=0 (op=4) on $scan_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$scan_path" || true

	# Tight loop: ops we have NOT yet tested (13/14/15/0), then re-test
	# op=10 with sub=1 to vary input.  No mid-canary.
	for op in 13 14 15 0; do
		log "opscan4: trying op=$op sub=0 uptime=$(cut -d' ' -f1 /proc/uptime)"
		"$PROBE" -t 800 -m vendor -u 0 -o "op${op}" "$scan_path" || true
	done

	log "opscan4: re-probe op=10 sub=1 uptime=$(cut -d' ' -f1 /proc/uptime)"
	"$PROBE" -t 800 -m vendor -u 1 -o op10 "$scan_path" || true

	log "opscan4: post-canary read sub=0 (op=4) on $scan_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$scan_path" || true

	log "opscan4: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_EFS_OPCODE_SCAN_V3:-0}" = 1 ]; then
	# Enumerate ops 10..20 (and op=0) — firmware string analysis
	# (qdsp6sw.mbn 0x1906bd3) shows xiaomi_extend_qmi_efs_handler logs
	# "not supported cmd(%d)" for unknown ops and returns cleanly, so
	# unknown-op probing is SAFE (does NOT wedge the port the way the
	# known-broken xiaomi_efs_write does).  Goal: find the op codes for
	# the firmware-only handlers xiaomi_efs_size and xiaomi_efs_copy
	# (mtb only exposes ops 1..9; ops above 9 must be enumerated).
	#
	# Layout sent: standard single-path read-shaped buffer
	# (BUF[0]=op, BUF[1]=sub, BUF[4]=plen, BUF[5..]=path).  An op that
	# needs a different layout (e.g. xiaomi_efs_copy with src+tgt) will
	# either log "req_c_struct_len should be %d" with an error result,
	# or process garbage and return a non-fatal error — either is
	# acceptable since we only need to identify which op # is alive.
	#
	# Mid-canary read after EACH op so we can detect a wedge early and
	# avoid contaminating the rest of the run.  Order tries op=0 last
	# because op=0 in some Qualcomm dispatch tables is "unused" and may
	# behave oddly.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "opscan3: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	scan_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "opscan3: pre-canary read sub=0 (op=4) on $scan_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$scan_path" || true

	for op in 10 11 12 13 14 15 0; do
		log "opscan3: trying op=$op against $scan_path sub=0 uptime=$(cut -d' ' -f1 /proc/uptime)"
		"$PROBE" -t 1500 -m vendor -u 0 -o "op${op}" "$scan_path" || true

		log "opscan3: post-op${op} canary read sub=0 (op=4) on $scan_path"
		if ! "$PROBE" -t 1500 -m vendor -u 0 -o read "$scan_path"; then
			log "opscan3: CANARY FAILED after op=$op — port wedged, aborting"
			break
		fi
	done

	log "opscan3: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_EFS_OPCODE_SCAN_V2:-0}" = 1 ]; then
	# Follow-up to OPCODE_SCAN: cover op=9 (untested — port wedged after op=8
	# in the v1 scan), confirm op=1 looks like xiaomi_efs_size by querying it
	# on a path we know is absent, and confirm op=6 (delete) is dispatched on
	# this firmware build.  Each step has its own canary to detect a wedge.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "opscan2: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	imr_path=/nv/item_files/mcfg/ignore_modem_reset
	scan_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim

	# Pre-canary: confirm port is responsive.
	log "opscan2: pre-canary read sub=0 (op=4) on $scan_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$scan_path" || true

	# op=1 on absent path: if op=1 == xiaomi_efs_size, it should report 0 or
	# an errno-shaped negative value for a missing path.  Compare against the
	# v1 scan where op=1 returned 0x19b (411) for an existing path.
	log "opscan2: op=1 on absent $imr_path (size of missing file)"
	"$PROBE" -t 1500 -m vendor -u 0 -o op1 "$imr_path" || true

	log "opscan2: mid-canary read sub=0 (op=4) on $scan_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$scan_path" || true

	# op=9 is the only xiaomi_extend op number we haven't seen the modem
	# respond to.  Try it against a known-good path; if it wedges, the rest
	# of the scan is contaminated.
	log "opscan2: op=9 on $scan_path (untested in v1)"
	"$PROBE" -t 1500 -m vendor -u 0 -o op9 "$scan_path" || true

	log "opscan2: post-op9-canary read sub=0 (op=4) on $scan_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$scan_path" || true

	# op=6 (delete) on the absent ignore_modem_reset path.  Delete-of-absent
	# should be a quick success or ENOENT — either way, no flash mutation.
	# Confirms whether the delete path on this firmware is responsive.
	log "opscan2: op=6 (delete) on absent $imr_path"
	"$PROBE" -t 4000 -m vendor -u 0 -o delete "$imr_path" || true

	log "opscan2: post-canary read sub=0 (op=4) on $scan_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$scan_path" || true

	log "opscan2: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_EFS_OPCODE_SCAN:-0}" = 1 ]; then
	# Enumerate xiaomi_extend op codes beyond 4/5/6 (read/write/delete).  The
	# firmware exposes additional handlers (xiaomi_efs_size, xiaomi_efs_copy,
	# xiaomi_efs_stat, xiaomi_efs_is_exsit, xiaomi_efs_compare) that mtb does
	# not surface; their op numbers live in the dispatch table near 0x1906000
	# in qdsp6sw.mbn but are not directly labelled.
	#
	# Strategy: send each candidate op with a path-only (read-shaped) inner
	# buffer and a known-good path.  A response with sensible TLVs means the
	# op exists; an error TLV means rejected; a timeout means the op exists
	# but wedges (same failure mode as op=5 write).  Use a short 1500ms
	# timeout per op so a wedge on one does not consume the whole MISSION
	# window before MCFG-Refresh-STM ERRFATAL fires.
	#
	# Stop apollo-pdc-load-mcfg.service first so any contention on the
	# modem's NV-manager is removed (matched the working write-test setup).
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "opscan: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	scan_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "opscan: pre-canary read sub=0 (op=4) on $scan_path"
	run_probe vendor 0 "$scan_path"

	for op in 1 2 3 7 8 9; do
		log "opscan: trying op=$op against $scan_path sub=0 uptime=$(cut -d' ' -f1 /proc/uptime)"
		"$PROBE" -t 1500 -m vendor -u 0 -o "op${op}" "$scan_path" || true
	done

	log "opscan: post-canary read sub=0 (op=4) on $scan_path (timeout = port wedged)"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$scan_path" || true

	log "opscan: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_EFS_IGNORE_MODEM_RESET_TEST:-0}" = 1 ]; then
	# Race the SSREQ→ERRFATAL chain. qdsp6sw.mbn strings show
	# mcfg_utils_reset_modem reads /nv/item_files/mcfg/ignore_modem_reset
	# right before sys_m_request_peripheral_restart_ssreq; when set it logs
	# "MCFG reset not enabled for activation" and returns. Setting this NV
	# to 1 short-circuits the entire crash path inside the firmware.
	#
	# The op=5 wedge documented in project_sdx55_efs_write_dead_end.md was
	# only tested with multi-hundred-byte payloads on existing paths. Try a
	# 1-byte write to a path that may not exist yet — different code path
	# (no efs_unlink, no large-buffer allocation), may avoid whatever the
	# modem hangs on.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "imr-test: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	imr_path=/nv/item_files/mcfg/ignore_modem_reset

	log "imr-test: pre-read $imr_path sub=0"
	run_probe vendor 0 "$imr_path"
	log "imr-test: pre-read $imr_path sub=1"
	run_probe vendor 1 "$imr_path"

	log "imr-test: write 0x01 (1 byte) to $imr_path sub=0"
	# 4s timeout — write either succeeds quickly or wedges; leaves margin
	# before MISSION+~15s ERRFATAL.
	"$PROBE" -t 4000 -m vendor -u 0 -o write -d 01 "$imr_path" || true

	log "imr-test: post-read $imr_path sub=0 (expect 0x01 if write took)"
	run_probe vendor 0 "$imr_path"

	log "imr-test: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_EFS_IGNORE_DATA_CLEANUP_TEST:-0}" = 1 ]; then
	# Write /nv/item_files/mcfg/ignore_data_cleanup so the modem's MCFG
	# framework skips "Wait for data ready rcevt signal" at boot.
	# The modem reads this NV item during MCFG init; if present (non-zero),
	# it bypasses the 15s data-ready wait that currently triggers ERRFATAL.
	#
	# Same strategy as ignore_modem_reset: stop PDC LOAD to release any
	# flash lock, write 0x01 as a 1-byte payload to a path that likely
	# does not exist yet (different kernel code path than overwriting a
	# multi-hundred-byte existing file), then read back to confirm.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "idc-test: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	idc_path=/nv/item_files/mcfg/ignore_data_cleanup

	log "idc-test: pre-read $idc_path sub=0 (expect error if absent)"
	run_probe vendor 0 "$idc_path"
	log "idc-test: pre-read $idc_path sub=1 (expect error if absent)"
	run_probe vendor 1 "$idc_path"

	log "idc-test: write 0x01 (1 byte) to $idc_path sub=0"
	"$PROBE" -t 4000 -m vendor -u 0 -o write -d 01 "$idc_path" || true

	log "idc-test: post-read $idc_path sub=0 (expect 0x01 if write took)"
	run_probe vendor 0 "$idc_path"

	log "idc-test: post-read $idc_path sub=1 (expect 0x01 if write took)"
	run_probe vendor 1 "$idc_path"

	log "idc-test: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	log "idc-test: if write succeeded, reboot and check if crash is gone"
	exit 0
fi

if [ "${XIAOMI_EFS_PROBE_WRITE_TEST:-0}" = 1 ]; then
	# Validate the EFS write-op wire format (op=5) by round-tripping a path the
	# modem already knows: mcfg_autoselect_by_uim.  Captured content from prior
	# reads is { 1, 1, 0, 0, 0x17 } as five u32 LE (the leading 0x04 in read
	# responses is op_echo, not data).  Writing the same bytes back is a no-op
	# if the wire format is correct, so a successful round-trip confirms the
	# layout without changing modem state.
	#
	# Disassembly of /vendor/bin/mtb confirms our wire format is byte-identical
	# to what mtb sends for op=5 (op@0, sub_id@1, path_len@4, path@5+,
	# total_buf_len@0x4C, chunk_size@0x50, sent_offset@0x54=0, data@0x58, plus
	# TLV 0x01=request_id 2 / TLV 0x02=0x200 / TLV 0x10=512B inner buffer).
	# Yet writes wedge port 3:8 (no RESP at all) while reads on the same port
	# work and PDC LOAD on port 3:17 keeps responding.  Hypothesis: the modem
	# EFS handler blocks on flash that PDC LOAD's NV-manager activity holds.
	# Stop PDC LOAD before the write to test whether contention is the cause.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "write-test: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	auto_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	auto_value=1,1,0,0,0x17

	log "write-test: pre-read $auto_path sub=0 (expect 04 00 00 00 01 00 00 00 01 00 00 00 00 00 00 00 17 00 00 00...)"
	run_probe vendor 0 "$auto_path"

	log "write-test: write $auto_value to $auto_path sub=0 (no-op round-trip)"
	run_probe_write_u32 vendor 0 "$auto_path" "$auto_value"

	log "write-test: post-read $auto_path sub=0 (expect identical to pre-read)"
	run_probe vendor 0 "$auto_path"

	log "write-test: baseline read mcfg_refresh_timer sub=0"
	run_probe vendor 0 /nv/item_files/mcfg/mcfg_refresh_timer
	log "write-test: baseline read mcfg_refresh_timer sub=1"
	run_probe vendor 1 /nv/item_files/mcfg/mcfg_refresh_timer

	log "write-test: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_THIRD_HANDLER_SCAN_V1:-0}" = 1 ]; then
	# Third handler (req_id=4) initial exploration.  Discovered via mtb
	# r2ghidra 2026-05-02.  Two wrappers share this dispatcher:
	#   xiaomi_send_common_qmi_msg (test ops: ssr, full_dump, nv_sync)
	#   xiaomi_cmn_cmd_send (card_status family)
	#
	# Wire format (req_id=4, buf_size=0x200):
	#   BUF[0..3]   = 2 (constant)
	#   BUF[4..7]   = 0 (zeroed)
	#   BUF[8..11]  = op (u32 LE)
	#   BUF[12..15] = payload len (u32 LE)
	#   BUF[16..]   = payload bytes
	#
	# Known ops (from mtb decompilation):
	#   op 0x1f=31 (SSR/full_dump) — destructive, skip
	#   op 0x23=35 (nv_sync) — flush pending NV writes
	#   op 0x27=39 (card_status_query, 0 args) — safest probe
	#   op 0x37=55 (card_status_get, 1 u32 arg)
	#   op 0x38=56 (card_type_query, 0 args) — safe diagnostic
	#   op 0x39=57 (card_status_set, 3 u32 args) — THE BYPASS
	#
	# V1 strategy:
	#   1. op=0x27 card_status_query — validates wire format end-to-end
	#   2. op=0x38 card_type_query — second safe diagnostic
	#   3. op=0x39 card_status_set (slot=0, state=READY=1, flags=0) —
	#      if SIM state machine is upstream of MCFG Refresh STM, forcing
	#      SIM-READY may deliver the REFRESHED input the STM waits for.
	#
	#   4. post-probe ERRFATAL check: if modem survives past MISSION+20s,
	#      the bypass worked.  If ERRFATAL still fires at MISSION+~10s,
	#      try different state values (state=2/3/4).
	#
	# Budget: 1 canary + 3 probes + 1 canary = 5 probes × ~1s = 5s.
	# MISSION+~10s ERRFATAL baseline gives comfortable margin.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "third-v1: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "third-v1: pre-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	# op=0x27 (39) card_status_query — 0 args, safe probe
	log "third-v1: op=0x27 card_status_query (no payload)"
	"$PROBE" -t 1200 -3 -o 39 || true

	# op=0x38 (56) card_type_query — 0 args, safe diagnostic
	log "third-v1: op=0x38 card_type_query (no payload)"
	"$PROBE" -t 1200 -3 -o 56 || true

	# op=0x39 (57) card_status_set — THE BYPASS
	# 3×u32 payload: slot=0, state=1 (READY), flags=0
	# Encoded: 0,0,0,0  1,0,0,0  0,0,0,0  in LE
	log "third-v1: op=0x39 card_status_set (slot=0, state=READY, flags=0)"
	"$PROBE" -t 1200 -3 -o 57 -D "0,1,0" || true

	log "third-v1: post-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "third-v1: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_CMN_HANDLER_SCAN_V11:-0}" = 1 ]; then
	# V11: extend op=6 multiplexer enumeration to sub-cmds 11-20.
	#
	# V10 probed sub-cmds 11-15 with 600ms timeouts but ERRFATAL had
	# already fired (all returned "service lookup failed" from MISSION+
	# 10.91s baseline).  V9/V8 found sub-cmds 4-10 are mostly stubs
	# (status=0, no return data with empty or 200-byte payloads).
	#
	# mcfg_ext_set_rfs_params is the prime target — it directly manipulates
	# the rfs_params struct.  Based on source-order in qdsp6sw.mbn strings
	# (after SIM detection at 26244310, before sw_version at 26245146), it
	# likely lives at sub-cmd 11..15.  V10 didn't actually reach it due to
	# the ERRFATAL race.
	#
	# Strategy: enumerate sub-cmds 11..20 with empty payload + 600ms
	# timeout per probe.  Sub-cmds 11..15 in hex are 0x0b..0x0f; 16..20
	# are 0x10..0x14.  Any that return non-zero data at BUF[0x10+] or a
	# non-zero/non-(-1) status is a real handler — and likely
	# mcfg_ext_set_rfs_params or sw_version reading.
	#
	# Budget: 1 canary + 10 probes + 1 canary = 12 probes × ~500ms = 6s.
	# This is tight but fits the MISSION+~10s heavy-probe window.
	systemctl stop apollo-pdc-load-mcfg.service 2>/dev/null || true
	log "cmn-scan-v11: stopped apollo-pdc-load-mcfg.service uptime=$(cut -d' ' -f1 /proc/uptime)"

	canary_path=/nv/item_files/mcfg/mcfg_autoselect_by_uim
	log "cmn-scan-v11: pre-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 800 -m vendor -u 0 -o read "$canary_path" || true

	# Enumerate sub-cmds 11..20 (0x0b..0x14) empty.
	# Note: sub-cmd values are u32 LE at BUF[4..7], so 11=0x0b000000 etc.
	for subcmd_hex in 0b 0c 0d 0e 0f 10 11 12 13 14; do
		log "cmn-scan-v11: sub-cmd=0x$subcmd_hex empty uptime=$(cut -d' ' -f1 /proc/uptime)"
		"$PROBE" -t 600 -C -o op6 -d "${subcmd_hex}000000" || true
	done

	log "cmn-scan-v11: post-canary EFS read sub=0 (op=4) on $canary_path"
	"$PROBE" -t 1500 -m vendor -u 0 -o read "$canary_path" || true

	log "cmn-scan-v11: finished uptime=$(cut -d' ' -f1 /proc/uptime)"
	exit 0
fi

if [ "${XIAOMI_EFS_PROBE_READS:-0}" != 1 ]; then
	log "lookup-only mode; set XIAOMI_EFS_PROBE_READS=1 to send EFS read probes"
	exit 0
fi

# First validate the direct QMI wire encoding against a Xiaomi path that mtb
# references.  Keep this short so we do not burn the modem's watchdog window.
run_probe vendor 0 /nv/item_files/modem/xiaomi/rf_ant_port_config

if [ "${XIAOMI_EFS_PROBE_FULL:-0}" != 1 ]; then
	log "single-read mode; set XIAOMI_EFS_PROBE_FULL=1 to sweep more paths"
	exit 0
fi

# Then collect the MCFG paths most likely to explain a PDC/MCFG watchdog.
for sub in 0 1; do
	for path in \
		/nv/item_files/mcfg/img_verify_err_fatal \
		/nv/item_files/mcfg/mcfg_sel_db.xml \
		/nv/item_files/mcfg/mcfg_autoselect_by_uim \
		/nv/item_files/mcfg/mcfg_version_info \
		/nv/item_files/modem/xiaomi/ant_tx_config \
		/nv/item_files/modem/xiaomi/rf_ant_port_config
	do
		run_probe vendor "$sub" "$path"
	done
done

log "finished uptime=$(cut -d' ' -f1 /proc/uptime)"
