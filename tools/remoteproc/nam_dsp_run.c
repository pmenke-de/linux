// SPDX-License-Identifier: GPL-2.0
/*
 * nam_dsp_run - drive the NAM WaveNet running on the CDSP
 *
 * Uploads a quantised model (built by nam_quantize.py) to libnamwn_skel.so,
 * then streams a WAV through it a block at a time - one FastRPC round trip per
 * block, which is the only arrangement that fits the audio budget.
 *
 *   sudo cdsp-run "/usr/local/bin/nam_dsp_run model.nqw in.wav out.wav"
 *
 * A second model may be appended as "model2.nqw out2.wav"; both then run as
 * separate DSP instances, interleaved block by block. Each output must come
 * out identical to its solo run - that is the test that instances really are
 * independent rather than sharing state.
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
#define BLOCK 128               /* must be a multiple of 64 and <= MAX_FRAMES */

extern const struct fastrpc_function_def_interp2 remotectl_open_def;
extern const struct fastrpc_function_def_interp2 remotectl_close_def;

static int skel_open(int fd, const char *name, uint32_t *handle)
{
	char err[256] = "";
	int32_t dlret = 0;
	int ret;

	ret = fastrpc2(&remotectl_open_def, fd, REMOTECTL_HANDLE,
		       (uint32_t)strlen(name) + 1, name,
		       handle, &dlret, (uint32_t)sizeof(err), err);
	if (ret == -1) {
		fprintf(stderr, "remotectl_open(%s): %s\n", name, strerror(errno));
		return -1;
	}
	if (dlret) {
		fprintf(stderr, "remotectl_open(%s): dsp error %d%s%s\n",
			name, dlret, err[0] ? ": " : "", err);
		return -1;
	}
	return 0;
}

static void skel_close(int fd, uint32_t handle)
{
	char err[256] = "";
	uint32_t dlret = 0;

	fastrpc2(&remotectl_close_def, fd, REMOTECTL_HANDLE,
		 handle, &dlret, (uint32_t)sizeof(err), err);
}

/*
 * The skel keeps one model per instance and hands back a handle, so a host can
 * run more than one amp without them trampling each other.
 */
static int dsp_create(int fd, uint32_t handle, const void *blob, size_t len,
		      uint32_t *model)
{
	struct fastrpc_invoke_args args[2];
	struct fastrpc_invoke inv;

	args[0].ptr = (uint64_t)(uintptr_t)blob;
	args[0].length = len;
	args[0].fd = -1;

	args[1].ptr = (uint64_t)(uintptr_t)model;
	args[1].length = sizeof(*model);
	args[1].fd = -1;

	inv.handle = handle;
	inv.sc = REMOTE_SCALARS_MAKE(0, 1, 1);
	inv.args = (uint64_t)(uintptr_t)args;

	return ioctl(fd, FASTRPC_IOCTL_INVOKE, &inv);
}

static int dsp_process(int fd, uint32_t handle, uint32_t model,
		       const int16_t *in, int16_t *out, unsigned int n)
{
	struct fastrpc_invoke_args args[3];
	struct fastrpc_invoke inv;

	args[0].ptr = (uint64_t)(uintptr_t)&model;
	args[0].length = sizeof(model);
	args[0].fd = -1;

	args[1].ptr = (uint64_t)(uintptr_t)in;
	args[1].length = n * sizeof(int16_t);
	args[1].fd = -1;

	args[2].ptr = (uint64_t)(uintptr_t)out;
	args[2].length = n * sizeof(int16_t);
	args[2].fd = -1;

	inv.handle = handle;
	inv.sc = REMOTE_SCALARS_MAKE(1, 2, 1);
	inv.args = (uint64_t)(uintptr_t)args;

	return ioctl(fd, FASTRPC_IOCTL_INVOKE, &inv);
}

static int dsp_destroy(int fd, uint32_t handle, uint32_t model)
{
	struct fastrpc_invoke_args args[1];
	struct fastrpc_invoke inv;

	args[0].ptr = (uint64_t)(uintptr_t)&model;
	args[0].length = sizeof(model);
	args[0].fd = -1;

	inv.handle = handle;
	inv.sc = REMOTE_SCALARS_MAKE(2, 1, 0);
	inv.args = (uint64_t)(uintptr_t)args;

	return ioctl(fd, FASTRPC_IOCTL_INVOKE, &inv);
}

/* Minimal RIFF reader/writer: mono, 16 or 24 bit PCM in, 16 bit out. */
static int16_t *read_wav(const char *path, unsigned int *count, unsigned int *rate)
{
	unsigned char *d;
	long size;
	FILE *f = fopen(path, "rb");
	unsigned int pos = 12, bits = 0, ch = 1;
	unsigned char *data = NULL;
	unsigned int dlen = 0;
	int16_t *out;
	unsigned int i;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = malloc(size);
	if (!d || fread(d, 1, size, f) != (size_t)size) {
		fclose(f);
		free(d);
		return NULL;
	}
	fclose(f);

	while (pos + 8 <= (unsigned long)size) {
		unsigned int sz;

		memcpy(&sz, d + pos + 4, 4);
		if (!memcmp(d + pos, "fmt ", 4)) {
			memcpy(&ch, d + pos + 10, 2);
			ch &= 0xffff;
			memcpy(rate, d + pos + 12, 4);
			memcpy(&bits, d + pos + 22, 2);
			bits &= 0xffff;
		} else if (!memcmp(d + pos, "data", 4)) {
			data = d + pos + 8;
			dlen = sz;
		}
		pos += 8 + sz + (sz & 1);
	}
	if (!data || (bits != 16 && bits != 24)) {
		free(d);
		return NULL;
	}

	*count = dlen / (bits / 8) / ch;
	out = malloc(*count * sizeof(int16_t));
	for (i = 0; i < *count; i++) {
		if (bits == 16) {
			int16_t v;

			memcpy(&v, data + (size_t)i * 2 * ch, 2);
			out[i] = v;
		} else {
			const unsigned char *p = data + (size_t)i * 3 * ch;
			int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);

			if (v & 0x800000)
				v -= 1 << 24;
			/*
			 * Round rather than truncate. Half an LSB sounds like
			 * nothing, but it is a real half-LSB of input error and
			 * it is also what nam_emul.py does - matching here is
			 * what lets the emulator reproduce the DSP bit-exactly,
			 * which is the whole point of having it.
			 */
			v = (v + 128) >> 8;
			out[i] = v > 32767 ? 32767 : (int16_t)v;
		}
	}
	free(d);
	return out;
}

static int write_wav(const char *path, const int16_t *x, unsigned int n,
		     unsigned int rate)
{
	unsigned char hdr[44];
	unsigned int bytes = n * 2;
	FILE *f = fopen(path, "wb");

	if (!f)
		return -1;
	memcpy(hdr, "RIFF", 4);
	*(uint32_t *)(hdr + 4) = 36 + bytes;
	memcpy(hdr + 8, "WAVEfmt ", 8);
	*(uint32_t *)(hdr + 16) = 16;
	*(uint16_t *)(hdr + 20) = 1;
	*(uint16_t *)(hdr + 22) = 1;
	*(uint32_t *)(hdr + 24) = rate;
	*(uint32_t *)(hdr + 28) = rate * 2;
	*(uint16_t *)(hdr + 32) = 2;
	*(uint16_t *)(hdr + 34) = 16;
	memcpy(hdr + 36, "data", 4);
	*(uint32_t *)(hdr + 40) = bytes;
	fwrite(hdr, 1, 44, f);
	fwrite(x, 2, n, f);
	fclose(f);
	return 0;
}

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int main(int argc, char **argv)
{
	uint32_t handle = 0, model = 0, model2 = 0;
	void *blob2 = NULL;
	long blob2_len = 0;
	int16_t *out2 = NULL;
	int dual;
	unsigned int n = 0, rate = 48000, i, blocks = 0;
	int16_t *in, *out;
	void *blob;
	long blob_len;
	FILE *bf;
	double t0, total;
	int fd, rc = 1;

	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc != 4 && argc != 6) {
		fprintf(stderr, "usage: %s model.nqw in.wav out.wav "
				"[model2.nqw out2.wav]\n", argv[0]);
		return 1;
	}

	fd = hexagonrpc_fd_from_env();
	if (fd == -1) {
		fprintf(stderr, "No FastRPC session; run under cdsp-run\n");
		return 1;
	}

	bf = fopen(argv[1], "rb");
	if (!bf) {
		fprintf(stderr, "cannot open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	fseek(bf, 0, SEEK_END);
	blob_len = ftell(bf);
	fseek(bf, 0, SEEK_SET);
	blob = malloc(blob_len);
	if (fread(blob, 1, blob_len, bf) != (size_t)blob_len) {
		fclose(bf);
		return 1;
	}
	fclose(bf);

	dual = (argc == 6);
	if (dual) {
		FILE *b2 = fopen(argv[4], "rb");

		if (!b2) {
			fprintf(stderr, "cannot open %s: %s\n", argv[4],
				strerror(errno));
			return 1;
		}
		fseek(b2, 0, SEEK_END);
		blob2_len = ftell(b2);
		fseek(b2, 0, SEEK_SET);
		blob2 = malloc(blob2_len);
		if (fread(blob2, 1, blob2_len, b2) != (size_t)blob2_len) {
			fclose(b2);
			return 1;
		}
		fclose(b2);
	}

	in = read_wav(argv[2], &n, &rate);
	if (!in) {
		fprintf(stderr, "cannot read %s\n", argv[2]);
		return 1;
	}
	out = calloc(n + BLOCK, sizeof(int16_t));
	if (dual)
		out2 = calloc(n + BLOCK, sizeof(int16_t));

	if (skel_open(fd, "namwn", &handle))
		return 1;

	if (dsp_create(fd, handle, blob, blob_len, &model)) {
		fprintf(stderr, "model create failed: %s\n", strerror(errno));
		goto out;
	}
	printf("model loaded (%ld bytes) as instance %#x, %u samples at %u Hz, "
	       "block %d\n", blob_len, model, n, rate, BLOCK);

	if (dual) {
		if (dsp_create(fd, handle, blob2, blob2_len, &model2)) {
			fprintf(stderr, "second model create failed: %s\n",
				strerror(errno));
			goto out;
		}
		printf("second model loaded as instance %#x\n", model2);
	}

	t0 = now_us();
	for (i = 0; i + BLOCK <= n; i += BLOCK) {
		int ret = dsp_process(fd, handle, model, in + i, out + i, BLOCK);

		if (!ret && dual)
			ret = dsp_process(fd, handle, model2, in + i,
					  out2 + i, BLOCK);
		if (ret) {
			fprintf(stderr, "process failed at sample %u (ret %d, %s)\n",
				i, ret, strerror(errno));
			goto out;
		}
		blocks++;
	}
	total = now_us() - t0;

	write_wav(argv[3], out, i, rate);
	if (dual)
		write_wav(argv[5], out2, i, rate);

	printf("processed %u blocks in %.1f ms  (%.1f us/block, budget %.1f us)\n",
	       blocks, total / 1000.0, total / blocks,
	       BLOCK * 1e6 / (double)rate);
	printf("real-time factor: %.2fx\n",
	       (BLOCK * 1e6 / (double)rate) / (total / blocks));
	rc = 0;
out:
	if (model2)
		dsp_destroy(fd, handle, model2);
	if (model)
		dsp_destroy(fd, handle, model);
	skel_close(fd, handle);
	return rc;
}
