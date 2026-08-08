#!/bin/sh
set -eu

STATE_DIR=/var/lib/mdm-helper-native
BOOTDONE_ONCE="$STATE_DIR/bootdone-once"

mkdir -p "$STATE_DIR"

if [ -e "$BOOTDONE_ONCE" ]; then
	rm -f "$BOOTDONE_ONCE"
	exec /usr/local/bin/mdm_helper_native --startup-timeout-ms 0 --boot-done --poll-ms 1000
fi

exec /usr/local/bin/mdm_helper_native --startup-timeout-ms 0
