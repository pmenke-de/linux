# SPDX-License-Identifier: GPL-2.0

CROSS_COMPILE ?= aarch64-linux-gnu-
CC ?= $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -O2 -g -static

all: mdm_helper_native pm_service_native sysmon_probe_native dms_nas_probe diag_reader ipa_qmi_proxy xiaomi_efs_probe diag_efs_write efs_mhi_write mhi_efs_sync qrtr_tftp_server

mdm_helper_native: mdm_helper_native.c
	$(CC) $(CFLAGS) -o $@ $<

pm_service_native: pm_service_native.c
	$(CC) $(CFLAGS) -o $@ $<

sysmon_probe_native: sysmon_probe_native.c
	$(CC) $(CFLAGS) -o $@ $<

dms_nas_probe: dms_nas_probe.c
	$(CC) $(CFLAGS) -o $@ $<

diag_reader: diag_reader.c
	$(CC) $(CFLAGS) -o $@ $<

ipa_qmi_proxy: ipa_qmi_proxy.c
	$(CC) $(CFLAGS) -o $@ $<

xiaomi_efs_probe: xiaomi_efs_probe.c
	$(CC) $(CFLAGS) -o $@ $<

diag_efs_write: diag_efs_write.c
	$(CC) $(CFLAGS) -o $@ $<

efs_mhi_write: efs_mhi_write.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	$(RM) mdm_helper_native pm_service_native sysmon_probe_native dms_nas_probe diag_reader ipa_qmi_proxy xiaomi_efs_probe diag_efs_write efs_mhi_write
	$(RM) hexagon_cdsp_test nam_conv_bench libnamconv_skel.so oemconfig.so

.PHONY: all clean

mhi_efs_sync: mhi_efs_sync.c
	$(CC) $(CFLAGS) -o $@ $<

qrtr_tftp_server: qrtr_tftp_server.c
	$(CC) $(CFLAGS) -o $@ $<

# Links against libhexagonrpc, so it cannot be static like the rest.
hexagon_cdsp_test: hexagon_cdsp_test.c
	$(CC) $(filter-out -static,$(CFLAGS)) -o $@ $< -lhexagonrpc

nam_conv_bench: nam_conv_bench.c
	$(CC) $(filter-out -static,$(CFLAGS)) -o $@ $< -lhexagonrpc

nam_dsp_run: nam_dsp_run.c
	$(CC) $(filter-out -static,$(CFLAGS)) -o $@ $< -lhexagonrpc

# DSP-side kernel. Needs clang with the Hexagon target plus ld.lld; -nostdlib
# keeps it free of DT_NEEDED so the FastRPC shell can load it standalone.
#
# Matching the stock skels matters: they are -Bsymbolic, sysv-hashed, 4K
# aligned, have a single RX load segment, and carry the Hexagon-only dynamic
# tag DT_HEXAGON_VER=3. lld emits none of that last one, and the DSP loader
# rejects modules without it reporting only a generic "unknown error" through
# remotectl - hence the spare DT_NULL slots and the post-link patch.
HEXCC ?= clang-20
# separate-loadable-segments is the one that actually matters: the DSP loader
# maps each PT_LOAD at page granularity, so segments packed into a shared page
# (lld's default) make dlopen fail with a bare "unknown error". The stock skels
# all start every LOAD on a page boundary in both file offset and vaddr.
#
# The rest keeps the object shaped like those skels: no PT_NOTE (a note is
# optional, but if present the loader demands a "uimg.dl.ver.*" one and rejects
# a GNU build-id), no PT_GNU_RELRO/PT_GNU_STACK, sysv hash, -Bsymbolic.
HEXLDFLAGS = -fuse-ld=lld -Wl,--hash-style=sysv -Wl,--no-rosegment \
	     -Wl,-z,max-page-size=4096 -Wl,-Bsymbolic \
	     -Wl,--build-id=none -Wl,-z,norelro -Wl,-z,nognustack \
	     -Wl,-z,separate-loadable-segments

libnamconv_skel.so: namconv_skel.c hexagon_skel_fixup.py
	$(HEXCC) -target hexagon-unknown-linux-musl -mv66 -mhvx -mhvx-length=128b \
		-O3 -fPIC -shared -nostdlib -fvisibility=hidden \
		$(HEXLDFLAGS) -Wl,-soname,$@ -o $@ $<
	./hexagon_skel_fixup.py $@

libnamwn_skel.so: namwn_skel.c hexagon_skel_fixup.py
	$(HEXCC) -target hexagon-unknown-linux-musl -mv66 -mhvx -mhvx-length=128b \
		-O3 -fPIC -shared -nostdlib -fvisibility=hidden \
		$(HEXLDFLAGS) -Wl,-soname,$@ -o $@ $<
	./hexagon_skel_fixup.py $@
