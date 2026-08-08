#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# diag-efs-write-runner — write ignore_data_cleanup NV item before crash.
# Uses the EFS MHI channel (/dev/wwan0qcdm1 from channels 10/11), which
# bypasses both DIAG and QMI paths entirely.
#
# Protocol v2: matches xiaomi_efs_probe inner format (u8 path length).
#
# Enable: XIAOMI_DIAG_EFS_WRITE=1 diag-efs-write-runner.sh
# Override: EFS_PROBE=/path/to/efs_mhi_write
#            EFS_DEV=/dev/wwan0qcdm1
#            EFS_TARGET=/nv/item_files/mcfg/ignore_data_cleanup

set -eu

PROBE=${EFS_PROBE:-/usr/local/bin/efs_mhi_write}
DEV=${EFS_DEV:-/dev/wwan0qcdm1}
TARGET=${EFS_TARGET:-/nv/item_files/mcfg/ignore_data_cleanup}
CANARY=${EFS_CANARY:-/nv/item_files/mcfg/mcfg_autoselect_by_uim}
WAIT_TIMEOUT=40

log()
{
	printf '%s %s\n' "diag-efs-write-runner:" "$*"
}

uptime_int()
{
	cut -d. -f1 /proc/uptime
}

dev_exists()
{
	[ -c "$DEV" ]
}

log "starting uptime=$(uptime_int) target=$TARGET"

# 1. Wait for EFS MHI device
log "waiting for $DEV (max ${WAIT_TIMEOUT}s)..."
elapsed=0
while ! dev_exists; do
	sleep 0.25
	elapsed=$((elapsed + 1))
	if [ "$elapsed" -ge "$WAIT_TIMEOUT" ]; then
		log "timeout waiting for $DEV after ${WAIT_TIMEOUT}s"
		exit 1
	fi
done
log "$DEV appeared at uptime=$(uptime_int)"

# 2. Canary read: probe a known-existing path to confirm channel works.
#    Run sub=0 first (the primary modem), then sub=1 (secondary if dual-SIM).
log "canary read: $CANARY sub=0"
if "$PROBE" -d "$DEV" --read -p "$CANARY" -u 0 2>&1; then
	log "canary sub=0 OK"
else
	log "canary sub=0 failed (rc=$?)"
fi

log "canary read: $CANARY sub=1"
if "$PROBE" -d "$DEV" --read -p "$CANARY" -u 1 2>&1; then
	log "canary sub=1 OK"
else
	log "canary sub=1 failed (rc=$?)"
fi

# 3. Pre-read target to see current value
log "pre-read target: $TARGET sub=0"
"$PROBE" -d "$DEV" --read -p "$TARGET" -u 0 2>&1 || true

# 4. Write — this is the critical step
log "write 0x01 to $TARGET sub=0"
if "$PROBE" -d "$DEV" --write -p "$TARGET" -u 0 -x 01 2>&1; then
	log "write OK"
else
	log "write failed (rc=$?)"
fi

# 5. Post-read to verify (may fail if write triggered modem reset)
log "post-read target: $TARGET sub=0"
"$PROBE" -d "$DEV" --read -p "$TARGET" -u 0 2>&1 || true

log "finished uptime=$(uptime_int)"
log "check dmesg for ERRFATAL and MCFG timeout"
