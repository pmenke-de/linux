// SPDX-License-Identifier: GPL-2.0
/*
 * hexagon_cdsp_test - load and call code on the Hexagon 698 CDSP
 *
 * The SM8250 CDSP is reachable from mainline through the in-kernel FastRPC
 * driver plus hexagonrpcd, which creates a dynamic user protection domain on
 * the DSP and serves it files over the HexagonFS reverse tunnel. Once that PD
 * exists, loading DSP code is a two-step dance:
 *
 *   remotectl_open("calculator")  -> DSP dlopen()s libcalculator_skel.so
 *                                    (fetched from the AP via HexagonFS)
 *   fastrpc2(&sum_def, fd, handle, ...) -> the method actually runs on Hexagon
 *
 * Note remotectl takes the *interface* name, not a filename: the DSP-side
 * remotectl prepends "lib" and appends "_skel.so" itself.
 *
 * FastRPC argument marshalling, as implemented by libhexagonrpc, is:
 *   in_nums  scalars passed by value, then
 *   in_bufs  (uint32 len, void *ptr) pairs, then
 *   out_nums pointers to 32-bit words, then
 *   out_bufs (uint32 len, void *ptr) pairs.
 * Scalars are packed by the library into an implicit primitive buffer, which
 * is why e.g. a "rout int64" shows up as two out_nums rather than a buffer.
 *
 * This must run under hexagonrpcd so that it inherits the FastRPC fd of an
 * already-created PD:
 *
 *   hexagonrpcd -f /dev/fastrpc-cdsp -d cdsp \
 *       -c /usr/share/qcom/sm8250/Xiaomi/apollo/dsp/cdsp/fastrpc_shell_3 \
 *       -R /usr/share/qcom/sm8250/Xiaomi/apollo \
 *       -p /usr/local/bin/hexagon_cdsp_test
 *
 * Build on the device (see tools/remoteproc/Makefile).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

#include <misc/fastrpc.h>

#include <libhexagonrpc/fastrpc.h>
#include <libhexagonrpc/session.h>

#define REMOTECTL_HANDLE 0

extern const struct fastrpc_function_def_interp2 remotectl_open_def;
extern const struct fastrpc_function_def_interp2 remotectl_close_def;

static int skel_open(int fd, const char *name, uint32_t *handle)
{
	char err[256] = "";
	int32_t dlret = 0;
	int ret;

	ret = fastrpc2(&remotectl_open_def, fd, REMOTECTL_HANDLE,
		       (uint32_t)strlen(name) + 1, name,
		       handle,
		       &dlret,
		       (uint32_t)sizeof(err), err);
	if (ret == -1) {
		printf("  %-24s FAIL   ioctl: %s\n", name, strerror(errno));
		return -1;
	}
	if (dlret) {
		printf("  %-24s FAIL   dsp error %d%s%s\n", name, dlret,
		       err[0] ? ": " : "", err);
		return -1;
	}

	printf("  %-24s LOADED handle 0x%08x\n", name, *handle);
	return 0;
}

static void skel_close(int fd, uint32_t handle)
{
	char err[256] = "";
	uint32_t dlret = 0;

	fastrpc2(&remotectl_close_def, fd, REMOTECTL_HANDLE,
		 handle,
		 &dlret,
		 (uint32_t)sizeof(err), err);
}

/*
 * calculator.idl ships with the Hexagon SDK as the canonical offload example:
 *   AEEResult sum(in sequence<int32> vec, rout int64 res);
 *
 * This one is issued as a raw ioctl rather than through fastrpc2(). Qualcomm's
 * ABI puts a sequence's *element count* in the primitive input buffer, while
 * libhexagonrpc writes the buffer's *byte* length there. The two agree for the
 * char sequences its own interfaces use - which is why remotectl_open works -
 * but differ by a factor of four for sequence<int32>, and the skel validates
 * that length and answers AEE_EBADPARM (14) when it disagrees.
 *
 * The wire layout Qualcomm's skel expects is:
 *   in  buffer 0: uint32 vecLen        (elements, not bytes)
 *   in  buffer 1: the vector itself
 *   out buffer 0: int64 res
 */
static int calculator_sum_raw(int fd, uint32_t handle, uint32_t msg_id,
			      const int32_t *vec, uint32_t nelem, int64_t *res)
{
	struct fastrpc_invoke_args args[3];
	struct fastrpc_invoke inv;
	uint32_t in_prim[1];

	in_prim[0] = nelem;

	args[0].ptr = (uint64_t)(uintptr_t)in_prim;
	args[0].length = sizeof(in_prim);
	args[0].fd = -1;

	args[1].ptr = (uint64_t)(uintptr_t)vec;
	args[1].length = nelem * sizeof(*vec);
	args[1].fd = -1;

	args[2].ptr = (uint64_t)(uintptr_t)res;
	args[2].length = sizeof(*res);
	args[2].fd = -1;

	inv.handle = handle;
	inv.sc = REMOTE_SCALARS_MAKE(msg_id, 2, 1);
	inv.args = (uint64_t)(uintptr_t)args;

	return ioctl(fd, FASTRPC_IOCTL_INVOKE, &inv);
}

static int calculator_sum(int fd, uint32_t handle, uint32_t msg_id)
{
	static const int32_t vec[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	const uint32_t nelem = sizeof(vec) / sizeof(vec[0]);
	int64_t expect = 0;
	int64_t res = 0;
	uint32_t i;
	int ret;

	for (i = 0; i < nelem; i++)
		expect += vec[i];

	ret = calculator_sum_raw(fd, handle, msg_id, vec, nelem, &res);
	if (ret == -1) {
		printf("  sum(method %u)           FAIL   ioctl: %s\n",
		       msg_id, strerror(errno));
		return -1;
	}
	if (ret) {
		printf("  sum(method %u)           FAIL   dsp returned %d\n",
		       msg_id, ret);
		return -1;
	}

	printf("  sum(method %u)           %s dsp=%lld cpu=%lld\n",
	       msg_id, res == expect ? "OK    " : "BAD   ",
	       (long long)res, (long long)expect);

	return res == expect ? 0 : -1;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

/*
 * Round-trip cost of one FastRPC call, which is the floor on any block-based
 * offload: a live audio graph has to finish inside one buffer period, so at
 * 48 kHz a 128-sample block leaves 2667 us and a 64-sample block 1333 us.
 * The payload is sized like an audio block so the measurement includes the
 * copy, not just the doorbell.
 */
static void bench_roundtrip(int fd, uint32_t handle, uint32_t msg_id,
			    uint32_t nelem, unsigned int iters)
{
	static int32_t vec[4096];
	uint64_t *samples;
	struct timespec t0, t1;
	uint64_t total = 0;
	unsigned int i;
	int64_t res;

	if (nelem > sizeof(vec) / sizeof(vec[0]))
		return;

	samples = calloc(iters, sizeof(*samples));
	if (!samples)
		return;

	for (i = 0; i < nelem; i++)
		vec[i] = (int32_t)i;

	/* Warm the path so first-call page faults do not skew the minimum. */
	for (i = 0; i < 16; i++)
		calculator_sum_raw(fd, handle, msg_id, vec, nelem, &res);

	for (i = 0; i < iters; i++) {
		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (calculator_sum_raw(fd, handle, msg_id, vec, nelem, &res)) {
			free(samples);
			return;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);

		samples[i] = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull
			   + (t1.tv_nsec - t0.tv_nsec);
		total += samples[i];
	}

	qsort(samples, iters, sizeof(*samples), cmp_u64);

	printf("  %4u samples  min %6.1f us  median %6.1f us  "
	       "mean %6.1f us  p99 %6.1f us\n",
	       nelem,
	       samples[0] / 1000.0,
	       samples[iters / 2] / 1000.0,
	       (double)total / iters / 1000.0,
	       samples[(iters * 99) / 100] / 1000.0);

	free(samples);
}

int main(int argc, char **argv)
{
	/*
	 * Only names that really are FastRPC interfaces belong here: remotectl
	 * turns "foo" into libfoo_skel.so, so plain DSP libraries such as
	 * libworker_pool.so cannot be opened this way.
	 */
	static const char *const defaults[] = { "calculator" };
	const char *const *names;
	uint32_t calc_handle = 0;
	int have_calc = 0;
	unsigned int count;
	uint32_t msg_id = 0;
	const char *env;
	unsigned int i;
	int fd;

	/*
	 * hexagonrpcd SIGTERMs its clients on shutdown, so anything still
	 * sitting in stdio would be lost. Loading a skel also takes seconds of
	 * chatter over the reverse tunnel, and unbuffered output lets that
	 * progress be watched live against hexagonrpcd's own log.
	 */
	setvbuf(stdout, NULL, _IONBF, 0);

	fd = hexagonrpc_fd_from_env();
	if (fd == -1) {
		fprintf(stderr,
			"No FastRPC session in the environment.\n"
			"Run this under: hexagonrpcd ... -p %s\n", argv[0]);
		return 1;
	}

	if (argc > 1) {
		names = (const char *const *)&argv[1];
		count = argc - 1;
	} else {
		names = defaults;
		count = sizeof(defaults) / sizeof(defaults[0]);
	}

	env = getenv("HEXAGON_CALC_METHOD");
	if (env)
		msg_id = (uint32_t)strtoul(env, NULL, 0);

	printf("Loading DSP interfaces on the CDSP:\n");

	for (i = 0; i < count; i++) {
		uint32_t handle = 0;

		if (skel_open(fd, names[i], &handle))
			continue;

		if (!strcmp(names[i], "calculator")) {
			calc_handle = handle;
			have_calc = 1;
			continue;
		}

		skel_close(fd, handle);
	}

	if (have_calc) {
		printf("\nRunning a real computation on Hexagon:\n");
		calculator_sum(fd, calc_handle, msg_id);

		printf("\nFastRPC round-trip cost (audio block budget:"
		       " 64 smp = 1333 us, 128 smp = 2667 us @ 48 kHz):\n");
		bench_roundtrip(fd, calc_handle, msg_id, 64, 2000);
		bench_roundtrip(fd, calc_handle, msg_id, 128, 2000);
		bench_roundtrip(fd, calc_handle, msg_id, 512, 2000);

		skel_close(fd, calc_handle);
	}

	return 0;
}
