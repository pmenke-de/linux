#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# cdsp-run - run a program inside a Hexagon 698 CDSP protection domain
#
# hexagonrpcd creates the PD, serves it files over the HexagonFS reverse
# tunnel, and passes the FastRPC fd to the child through $HEXAGONRPC_FD. A PD
# belongs to the process that created it, so compute clients have to be started
# this way rather than talking to a long-running daemon.
#
#   cdsp-run /usr/local/bin/hexagon_cdsp_test
#
# The DSP shell probes for oemconfig.so and testsig[-<socid>].so before it will
# load anything. Those probes must *succeed*, but nothing reads their contents:
# empty files are enough, and are what setup() installs. They are not real
# Qualcomm blobs and must not be faked with unrelated libraries - a signed
# production shell plus Qualcomm-signed skels needs no test certificate.

set -e

ROOT=/usr/share/qcom/sm8250/Xiaomi/apollo
CDSP_DIR=$ROOT/dsp/cdsp
SHELL_BIN=$CDSP_DIR/fastrpc_shell_3
NODE=/dev/fastrpc-cdsp

if [ $# -lt 1 ]; then
	echo "usage: $0 PROGRAM [args...]" >&2
	exit 1
fi

if [ ! -e "$SHELL_BIN" ]; then
	echo "$0: $SHELL_BIN missing - extract it from the dsp partition:" >&2
	echo "  mount -o ro /dev/disk/by-partlabel/dsp /mnt/dsp" >&2
	echo "  cp /mnt/dsp/cdsp/* $CDSP_DIR/" >&2
	exit 1
fi

# The shell blocks forever if a probe cannot be served, so make sure they exist.
# The testsig name is keyed to this SoC's ID; if hexagonrpcd ever logs
# "Could not open testsig-0x<something>.so", add that name here.
for probe in oemconfig.so testsig.so testsig-0xa1edb7c5.so; do
	[ -e "$CDSP_DIR/$probe" ] || : > "$CDSP_DIR/$probe"
done

PROG=$1
shift

# hexagonrpcd execs its client via /usr/bin/env with no arguments, so wrap the
# real command line in a throwaway script.
WRAPPER=$(mktemp /tmp/cdsp-run.XXXXXX)
trap 'rm -f "$WRAPPER"' EXIT
printf '#!/bin/sh\nexec %s %s\n' "$PROG" "$*" > "$WRAPPER"
chmod +x "$WRAPPER"

exec hexagonrpcd -f "$NODE" -d cdsp -c "$SHELL_BIN" -R "$ROOT" -p "$WRAPPER"
