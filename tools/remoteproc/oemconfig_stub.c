// SPDX-License-Identifier: GPL-2.0
/*
 * oemconfig_stub - a legitimate empty oemconfig.so for the FastRPC shell
 *
 * The DSP shell probes for oemconfig.so before loading user code. If HexagonFS
 * cannot serve the file the shell blocks forever; if it serves something that
 * is not a Hexagon ELF the loader fails with "bad elf magic in oemconfig.so".
 * An empty file is enough to get the stock signed skels loaded, but loading a
 * *new* module makes the shell actually parse it.
 *
 * Xiaomi ships no oemconfig.so on this device, so rather than renaming an
 * unrelated library into place (which is what the sdsp directory ended up
 * containing), build a real, valid, deliberately empty Hexagon shared object:
 *
 *   clang -target hexagon-unknown-linux-musl -mv66 -O2 -fPIC -shared \
 *         -nostdlib -fuse-ld=lld -o oemconfig.so oemconfig_stub.c
 */

/* Present only so the object has a symbol table and a non-empty .text. */
int oemconfig_stub_version(void);

int oemconfig_stub_version(void)
{
	return 0;
}
