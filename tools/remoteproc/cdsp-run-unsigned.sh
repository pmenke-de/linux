#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# cdsp-run-unsigned - run a program in an UNSIGNED CDSP protection domain
#
# Unsigned is what self-built skels need: the signed shell refuses to dlopen
# anything that is not Qualcomm-signed. That needs FASTRPC_MODE_UNSIGNED_MODULE
# at PD creation, which stock hexagonrpcd does not set - hence the patched build
# with -u.
#
# No sudo required once /dev/fastrpc-* is mode 660 fastrpc:fastrpc and the user
# is in the fastrpc group, which matters because the audio stack runs as the
# user and a root-owned PD would be unreachable from it.
#
#   cdsp-run-unsigned "lv2_alsa_host ~/.lv2/neural_amp_modeler.lv2 m.nqw hw:1,0 128 4 4"
#
# hexagonrpcd's listener never returns and it only signals its -p client on its
# own exit, so the wrapper kills its parent once the client finishes.

set -u

ROOT=${CDSP_FS_ROOT:-/usr/share/qcom/sm8250/Xiaomi/apollo}
RPCD=${CDSP_RPCD:-$HOME/hexagonrpc-src/build/hexagonrpcd/hexagonrpcd}
LIB=${CDSP_RPCD_LIB:-$HOME/hexagonrpc-src/build/libhexagonrpc}
SHELL_BIN=$ROOT/dsp/cdsp/fastrpc_shell_unsigned_3

if [ $# -lt 1 ]; then
	echo "usage: $0 \"PROGRAM [args...]\"" >&2
	exit 1
fi

if [ ! -x "$RPCD" ]; then
	echo "$0: patched hexagonrpcd not found at $RPCD" >&2
	echo "  build it with the -u (unsigned PD) patch; see the project notes" >&2
	exit 1
fi

# The DSP shell blocks forever if these probes cannot be served. Contents are
# never read, so empty files are enough - do not substitute unrelated libraries.
for probe in oemconfig.so testsig.so testsig-0xa1edb7c5.so; do
	[ -e "$ROOT/dsp/cdsp/$probe" ] || : > "$ROOT/dsp/cdsp/$probe" 2>/dev/null
done

WRAPPER=$(mktemp /tmp/cdsp-run.XXXXXX)
trap 'rm -f "$WRAPPER"' EXIT
printf '#!/bin/sh\n%s\nkill $PPID 2>/dev/null\n' "$*" > "$WRAPPER"
chmod +x "$WRAPPER"

LD_LIBRARY_PATH=$LIB exec "$RPCD" \
	-f /dev/fastrpc-cdsp -d cdsp -u \
	-c "$SHELL_BIN" -R "$ROOT" -p "$WRAPPER"
