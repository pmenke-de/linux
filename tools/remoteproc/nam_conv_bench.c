// SPDX-License-Identifier: GPL-2.0
/*
 * nam_conv_bench - Hexagon HVX vs CPU NEON on a NAM-shaped dilated conv1d
 *
 * Feasibility spike for NeuralAmpModelerCore on the Apollo's CDSP. NAM is
 * float32 on Eigen and v66 HVX is integer-only, so the question is not "can it
 * be ported" but "is a fixed-point rewrite worth it". This runs one WaveNet
 * layer (16 channels, kernel 3, Q15 int16) on both engines, checks they agree
 * bit for bit, and prices each against the 48 kHz block budget.
 *
 * Both sides implement identical semantics:
 *   y[co][t] = sat16(( sum_ci sum_k w[co][ci][k] * x[ci][t + k*dilation] ) >> 15)
 * HVX does it with vmpy(.h)->.w accumulate + vasr(...):sat; NEON with
 * vmlal_s16 + vqshrn_n_s32. Neither rounds, so results match exactly.
 *
 * Run inside a CDSP protection domain:
 *   sudo cdsp-run /usr/local/bin/nam_conv_bench
 *
 * Build on the device (see tools/remoteproc/Makefile).
 */

#define _GNU_SOURCE
#include <arm_neon.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <misc/fastrpc.h>

#include <libhexagonrpc/fastrpc.h>
#include <libhexagonrpc/session.h>

#define REMOTECTL_HANDLE 0

/* NAM "standard" WaveNet geometry. */
#define CHANNELS	16
#define KSIZE		3
#define NSAMPLES	128		/* 2667 us at 48 kHz */
#define SAMPLE_RATE	48000

#define BENCH_ITERS	400
/*
 * The CPU side swings by 3x run to run as DVFS moves the A77s around, while
 * the DSP side is stable to a few percent. Take the best of several trials on
 * both, which is the fairest read of what each engine can do.
 */
#define TRIALS		7

extern const struct fastrpc_function_def_interp2 remotectl_open_def;
extern const struct fastrpc_function_def_interp2 remotectl_close_def;

struct params {
	uint32_t channels;
	uint32_t ksize;
	uint32_t dilation;
	uint32_t nsamples;
	uint32_t iters;
};

static int skel_open(int fd, const char *name, uint32_t *handle)
{
	char err[256] = "";
	int32_t dlret = 0;
	int ret;

	ret = fastrpc2(&remotectl_open_def, fd, REMOTECTL_HANDLE,
		       (uint32_t)strlen(name) + 1, name,
		       handle, &dlret,
		       (uint32_t)sizeof(err), err);
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

static int dsp_conv1d(int fd, uint32_t handle, const struct params *p,
		      const int16_t *w, const int16_t *x, int16_t *y,
		      size_t wlen, size_t xlen, size_t ylen)
{
	struct fastrpc_invoke_args args[4];
	struct fastrpc_invoke inv;

	args[0].ptr = (uint64_t)(uintptr_t)p;
	args[0].length = sizeof(*p);
	args[0].fd = -1;

	args[1].ptr = (uint64_t)(uintptr_t)w;
	args[1].length = wlen;
	args[1].fd = -1;

	args[2].ptr = (uint64_t)(uintptr_t)x;
	args[2].length = xlen;
	args[2].fd = -1;

	args[3].ptr = (uint64_t)(uintptr_t)y;
	args[3].length = ylen;
	args[3].fd = -1;

	inv.handle = handle;
	inv.sc = REMOTE_SCALARS_MAKE(0, 3, 1);
	inv.args = (uint64_t)(uintptr_t)args;

	return ioctl(fd, FASTRPC_IOCTL_INVOKE, &inv);
}

/*
 * NEON reference, same layout and same fixed-point semantics as the skel.
 * vmlal_s16 widens int16 products into int32 lanes; vqshrn_n_s32 shifts back
 * to Q15 and saturates to int16, matching HVX's vasr(...):sat.
 */
static void neon_conv1d(const int16_t *w, const int16_t *x, int16_t *y,
			uint32_t chan, uint32_t ksize, uint32_t dil,
			uint32_t nsamp)
{
	const uint32_t xstride = (ksize - 1) * dil + nsamp;
	uint32_t co, ci, k, t;

	for (co = 0; co < chan; co++) {
		for (t = 0; t < nsamp; t += 8) {
			int32x4_t lo = vdupq_n_s32(0);
			int32x4_t hi = vdupq_n_s32(0);

			for (ci = 0; ci < chan; ci++) {
				const int16_t *xrow = x + (size_t)ci * xstride + t;
				const int16_t *wrow = w + ((size_t)co * chan + ci) * ksize;

				for (k = 0; k < ksize; k++) {
					int16x8_t xv = vld1q_s16(xrow + k * dil);
					int16x4_t wv = vdup_n_s16(wrow[k]);

					lo = vmlal_s16(lo, vget_low_s16(xv), wv);
					hi = vmlal_s16(hi, vget_high_s16(xv), wv);
				}
			}

			vst1q_s16(y + (size_t)co * nsamp + t,
				  vcombine_s16(vqshrn_n_s32(lo, 15),
					       vqshrn_n_s32(hi, 15)));
		}
	}
}

#define PSY "/sys/class/power_supply/qcom-battery/"

static double read_psy(const char *name)
{
	char path[160];
	double v = 0;
	FILE *f;

	snprintf(path, sizeof(path), PSY "%s", name);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%lf", &v) != 1)
		v = 0;
	fclose(f);
	return v;
}

/* current_now is uA and voltage_now uV, so this comes out in watts. */
static double read_power_w(void)
{
	return (read_psy("current_now") / 1e6) * (read_psy("voltage_now") / 1e6);
}

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* Deterministic, and bounded so 48 taps cannot overflow the int32 accumulator. */
static uint32_t rng_state = 12345;

static int16_t rng_next(int16_t range)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return (int16_t)((int32_t)((rng_state >> 16) % (2u * range)) - range);
}

enum phase { PHASE_IDLE, PHASE_CPU, PHASE_DSP };

/*
 * Hold one engine busy for `secs` while sampling battery draw, and report the
 * marginal energy each conv1d layer costs. The first SETTLE_S of every phase is
 * discarded: the fuel gauge needs a moment to catch up with a load change.
 *
 * During the DSP phase the host thread is asleep inside ioctl(), so what shows
 * up above idle is the DSP's own draw rather than the caller's.
 */
#define SETTLE_S 4.0

static double run_power_phase(enum phase ph, double secs, int fd,
			      uint32_t handle, struct params *p,
			      const int16_t *w, const int16_t *x, int16_t *y,
			      size_t wlen, size_t xlen, size_t ylen,
			      unsigned long long *layers_out)
{
	unsigned long long layers = 0;
	double t_end, t_settled, sum = 0;
	unsigned int nsamp = 0;
	double start;

	start = now_us();
	t_end = start + secs * 1e6;
	t_settled = start + SETTLE_S * 1e6;

	while (now_us() < t_end) {
		double before = now_us();

		switch (ph) {
		case PHASE_IDLE:
			usleep(200000);
			break;
		case PHASE_CPU:
			for (unsigned int k = 0; k < 2000; k++)
				neon_conv1d(w, x, y, p->channels, p->ksize,
					    p->dilation, p->nsamples);
			layers += 2000;
			break;
		case PHASE_DSP:
			p->iters = 10000;
			if (dsp_conv1d(fd, handle, p, w, x, y, wlen, xlen, ylen))
				return -1;
			layers += 10000;
			break;
		}

		if (before >= t_settled) {
			sum += read_power_w();
			nsamp++;
		}
	}

	if (layers_out)
		*layers_out = layers;

	return nsamp ? sum / nsamp : 0;
}

static int power_mode(int fd, uint32_t handle, const int16_t *w,
		      const int16_t *x, int16_t *y,
		      size_t wlen, size_t xlen, size_t ylen, double secs)
{
	struct params p = {
		.channels = CHANNELS, .ksize = KSIZE, .dilation = 64,
		.nsamples = NSAMPLES, .iters = 1,
	};
	unsigned long long cpu_layers = 0, dsp_layers = 0;
	double p_idle, p_cpu, p_dsp;

	printf("Power draw, %.0fs per phase (screen off, discarding first %.0fs)\n\n",
	       secs, SETTLE_S);

	p_idle = run_power_phase(PHASE_IDLE, secs, fd, handle, &p,
				 w, x, y, wlen, xlen, ylen, NULL);
	printf("  idle             %6.3f W\n", p_idle);

	p_cpu = run_power_phase(PHASE_CPU, secs, fd, handle, &p,
				w, x, y, wlen, xlen, ylen, &cpu_layers);
	printf("  CPU (NEON)       %6.3f W   (+%.3f W over idle)\n",
	       p_cpu, p_cpu - p_idle);

	p_dsp = run_power_phase(PHASE_DSP, secs, fd, handle, &p,
				w, x, y, wlen, xlen, ylen, &dsp_layers);
	if (p_dsp < 0) {
		fprintf(stderr, "dsp phase failed: %s\n", strerror(errno));
		return 1;
	}
	printf("  DSP (HVX)        %6.3f W   (+%.3f W over idle)\n",
	       p_dsp, p_dsp - p_idle);

	printf("\n  engine   layers done   marginal W   energy per layer\n");
	printf("  ------   -----------   ----------   ----------------\n");
	printf("  CPU      %11llu   %10.3f   %10.2f uJ\n", cpu_layers,
	       p_cpu - p_idle,
	       cpu_layers ? (p_cpu - p_idle) * secs / cpu_layers * 1e6 : 0);
	printf("  DSP      %11llu   %10.3f   %10.2f uJ\n", dsp_layers,
	       p_dsp - p_idle,
	       dsp_layers ? (p_dsp - p_idle) * secs / dsp_layers * 1e6 : 0);

	return 0;
}

int main(int argc, char **argv)
{
	static const uint32_t dilations[] = { 1, 16, 64, 512 };
	const double budget_us = NSAMPLES * 1e6 / SAMPLE_RATE;
	uint32_t handle = 0;
	int16_t *w, *x, *y_dsp, *y_cpu;
	size_t wlen, xlen, ylen, xmax;
	unsigned int d, i;
	int fd, rc = 1;

	setvbuf(stdout, NULL, _IONBF, 0);

	/*
	 * Keep the NEON reference off the little cores: on this SoC CPUs 0-3
	 * are 1.8 GHz A55s and 4-7 the A77s, and landing on the wrong cluster
	 * alone changes the answer by 3x.
	 */
	{
		cpu_set_t set;

		CPU_ZERO(&set);
		for (i = 4; i < 8; i++)
			CPU_SET(i, &set);
		if (sched_setaffinity(0, sizeof(set), &set))
			fprintf(stderr, "warning: could not pin to big cores: %s\n",
				strerror(errno));
	}

	fd = hexagonrpc_fd_from_env();
	if (fd == -1) {
		fprintf(stderr, "No FastRPC session; run under: cdsp-run %s\n",
			"nam_conv_bench");
		return 1;
	}

	if (skel_open(fd, "namconv", &handle))
		return 1;

	wlen = (size_t)CHANNELS * CHANNELS * KSIZE * sizeof(int16_t);
	ylen = (size_t)CHANNELS * NSAMPLES * sizeof(int16_t);
	xmax = (size_t)CHANNELS * ((KSIZE - 1) * 512 + NSAMPLES) * sizeof(int16_t);

	w = aligned_alloc(128, (wlen + 127) & ~(size_t)127);
	x = aligned_alloc(128, (xmax + 127) & ~(size_t)127);
	y_dsp = aligned_alloc(128, (ylen + 127) & ~(size_t)127);
	y_cpu = aligned_alloc(128, (ylen + 127) & ~(size_t)127);
	if (!w || !x || !y_dsp || !y_cpu)
		goto out;

	for (i = 0; i < wlen / sizeof(int16_t); i++)
		w[i] = rng_next(2048);		/* Q15 |w| <= 0.0625 */
	for (i = 0; i < xmax / sizeof(int16_t); i++)
		x[i] = rng_next(8192);

	if (argc > 1 && !strcmp(argv[1], "--power")) {
		double secs = argc > 2 ? atof(argv[2]) : 20.0;

		xlen = (size_t)CHANNELS *
		       ((KSIZE - 1) * 64 + NSAMPLES) * sizeof(int16_t);
		rc = power_mode(fd, handle, w, x, y_dsp, wlen, xlen, ylen, secs);
		goto out;
	}

	printf("NAM-shaped dilated conv1d: %d channels, kernel %d, %d samples/block\n",
	       CHANNELS, KSIZE, NSAMPLES);
	printf("Block budget at %d Hz: %.1f us   (%d MAC/block/layer)\n\n",
	       SAMPLE_RATE, budget_us, CHANNELS * CHANNELS * KSIZE * NSAMPLES);

	printf("  dil    HVX (DSP)     NEON (CPU)   speedup   HVX%%budget  match\n");
	printf("  ---  -----------   ------------   -------   ----------  -----\n");

	for (d = 0; d < sizeof(dilations) / sizeof(dilations[0]); d++) {
		struct params p = {
			.channels = CHANNELS,
			.ksize = KSIZE,
			.dilation = dilations[d],
			.nsamples = NSAMPLES,
			.iters = 1,
		};
		double t0, t1, t_one, t_many;
		double dsp_us = 1e30, cpu_us = 1e30;
		unsigned int trial;
		int ok;

		xlen = (size_t)CHANNELS *
		       ((KSIZE - 1) * dilations[d] + NSAMPLES) * sizeof(int16_t);

		/* Warm, then time 1 and N invocations to cancel RPC overhead. */
		if (dsp_conv1d(fd, handle, &p, w, x, y_dsp, wlen, xlen, ylen)) {
			printf("  %3u   dsp invoke failed: %s\n",
			       dilations[d], strerror(errno));
			continue;
		}

		for (trial = 0; trial < TRIALS; trial++) {
			double one;

			p.iters = 1;
			t0 = now_us();
			dsp_conv1d(fd, handle, &p, w, x, y_dsp, wlen, xlen, ylen);
			t_one = now_us() - t0;

			p.iters = BENCH_ITERS;
			t0 = now_us();
			dsp_conv1d(fd, handle, &p, w, x, y_dsp, wlen, xlen, ylen);
			t_many = now_us() - t0;

			one = (t_many - t_one) / (BENCH_ITERS - 1);
			if (one > 0 && one < dsp_us)
				dsp_us = one;

			t0 = now_us();
			for (i = 0; i < BENCH_ITERS; i++)
				neon_conv1d(w, x, y_cpu, CHANNELS, KSIZE,
					    dilations[d], NSAMPLES);
			t1 = now_us();

			one = (t1 - t0) / BENCH_ITERS;
			if (one < cpu_us)
				cpu_us = one;
		}

		ok = memcmp(y_dsp, y_cpu, ylen) == 0;

		printf("  %3u   %8.2f us   %8.2f us   %6.2fx   %8.2f%%   %s\n",
		       dilations[d], dsp_us, cpu_us, cpu_us / dsp_us,
		       100.0 * dsp_us / budget_us, ok ? "yes" : "NO");
	}

	printf("\nNote: HVX time excludes the ~130 us FastRPC round trip, which a real\n"
	       "implementation pays once per block (about %.1f%% of the budget).\n",
	       100.0 * 130.0 / budget_us);

	rc = 0;
out:
	skel_close(fd, handle);
	return rc;
}
