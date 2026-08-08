// SPDX-License-Identifier: GPL-2.0
/*
 * namconv_skel - int16 dilated causal conv1d on Hexagon v66 HVX
 *
 * A feasibility spike for running NeuralAmpModelerCore-style inference on the
 * Apollo's CDSP. NAM is float32 on Eigen, but v66 HVX is integer-only - vector
 * floating point only arrives in v68 - so anything that wants the vector units
 * has to be fixed point. This implements one WaveNet-shaped layer in Q15 int16
 * to find out what that costs.
 *
 * Built with plain upstream clang, no Hexagon SDK:
 *
 *   clang -target hexagon-unknown-linux-musl -mv66 -mhvx -mhvx-length=128b \
 *         -O3 -fPIC -shared -nostdlib -o libnamconv_skel.so namconv_skel.c
 *
 * -nostdlib keeps the result free of DT_NEEDED entries, matching
 * libbenchmark_skel.so; the calculator skel by contrast imports libc.so and
 * libgcc.so, which the FastRPC shell - not the filesystem - provides.
 *
 * The skel entry point ABI was recovered by disassembling
 * calculator_skel_invoke rather than taken from the SDK:
 *
 *   int <iface>_skel_invoke(uint32 sc, remote_arg *pra);
 *   struct remote_arg { void *pv; uint32 nLen; };   // 8 bytes, DSP is 32-bit
 *   sc: method @24-28, nIn @16-23, nOut @8-15, noIn @4-7, noOut @0-3
 *   pra order: [in buffers][out buffers][handles]
 *   returns 0, or 14 (AEE_EBADPARM) / 20 (AEE_EUNSUPPORTED)
 *
 * Method 0 - conv1d:
 *   in  0: struct params  { u32 channels, ksize, dilation, nsamples, iters }
 *   in  1: int16 weights  [chan_out][chan_in][ksize], Q15
 *   in  2: int16 input    [channels][(ksize-1)*dilation + nsamples]
 *   out 0: int16 output   [channels][nsamples]
 *
 * "iters" reruns the layer entirely on the DSP so the ~130 us FastRPC
 * round-trip can be amortised out and the compute measured on its own.
 */

typedef unsigned int u32;
typedef signed short s16;

typedef long HVX_Vector      __attribute__((__vector_size__(128), aligned(128)));
typedef long HVX_UVector     __attribute__((__vector_size__(128), aligned(1)));
typedef long HVX_VectorPair  __attribute__((__vector_size__(256), aligned(128)));

#define VLANES 64		/* int16 lanes per 128-byte HVX vector */

#define AEE_SUCCESS	0
#define AEE_EBADPARM	14
#define AEE_EUNSUPPORTED 20

struct remote_arg {
	void *pv;
	u32 nLen;
};

struct params {
	u32 channels;
	u32 ksize;
	u32 dilation;
	u32 nsamples;
	u32 iters;
};

/*
 * -nostdlib means anything the compiler open-codes into a libcall has to be
 * satisfied here. Only ever called on small aligned regions.
 *
 * These must stay hidden: the module is dlopen()ed into a process that already
 * has the shell's libc, and exporting a second memcpy/memset from it breaks the
 * load. Everything except the entry point is hidden via -fvisibility=hidden.
 */
__attribute__((visibility("hidden")))
void *memset(void *dst, int c, __SIZE_TYPE__ n)
{
	unsigned char *d = dst;

	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

__attribute__((visibility("hidden")))
void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

/*
 * y[co][t] = sat16(( sum_ci sum_k w[co][ci][k] * x[ci][t + k*dilation] ) >> 15)
 *
 * Channel-major layout lets one vector hold 64 consecutive timesteps of a
 * single channel, so each (out_chan, in_chan, tap) is a broadcast weight and a
 * single multiply-accumulate across 64 samples at once. Dilation only shifts
 * the load address, which is why the loads are unaligned (vmemu).
 *
 * V6_vmpyhv widens int16 x int16 into 32-bit accumulators, and deinterleaves
 * even/odd lanes across the vector pair. V6_vasrwhsat(hi, lo, 15) shifts back
 * down to Q15 with saturation and reinterleaves, exactly undoing that split.
 */
static void conv1d_hvx(const s16 *w, const s16 *x, s16 *y,
		       u32 chan, u32 ksize, u32 dil, u32 nsamp)
{
	const u32 xstride = (ksize - 1) * dil + nsamp;
	u32 co, ci, k, t;

	for (co = 0; co < chan; co++) {
		for (t = 0; t < nsamp; t += VLANES) {
			HVX_VectorPair acc = __builtin_HEXAGON_V6_vcombine_128B(
					__builtin_HEXAGON_V6_vd0_128B(),
					__builtin_HEXAGON_V6_vd0_128B());

			for (ci = 0; ci < chan; ci++) {
				const s16 *xrow = x + (u32)ci * xstride + t;
				const s16 *wrow = w + ((u32)co * chan + ci) * ksize;

				for (k = 0; k < ksize; k++) {
					HVX_Vector xv, wv;

					xv = *(const HVX_UVector *)(xrow + k * dil);
					wv = __builtin_HEXAGON_V6_lvsplath_128B(wrow[k]);

					acc = __builtin_HEXAGON_V6_vmpyhv_acc_128B(acc, xv, wv);
				}
			}

			*(HVX_UVector *)(y + (u32)co * nsamp + t) =
				__builtin_HEXAGON_V6_vasrwhsat_128B(
					__builtin_HEXAGON_V6_hi_128B(acc),
					__builtin_HEXAGON_V6_lo_128B(acc),
					15);
		}
	}
}

int namconv_skel_invoke(u32 sc, struct remote_arg *pra);

/*
 * Force a GOT and a couple of relative relocations into the object.
 *
 * With everything inlined and nothing external referenced, lld produces a
 * module with no relocations at all, so it emits neither DT_PLTGOT nor
 * DT_RELA. Every stock skel has both, and the DSP loader answers
 * AEE_EUNABLETOLOAD (0x80000406) without them - Hexagon PIC expects the loader
 * to establish a GOT base. The self-reference keeps lld from folding this away.
 */
__attribute__((visibility("hidden"), used))
static void *const got_anchor[] = {
	(void *)(unsigned long)&namconv_skel_invoke,
	(void *)&got_anchor,
};

__attribute__((visibility("default")))
int namconv_skel_invoke(u32 sc, struct remote_arg *pra)
{
	const u32 method = (sc >> 24) & 0x1f;
	const u32 nin    = (sc >> 16) & 0xff;
	const u32 nout   = (sc >> 8)  & 0xff;
	const struct params *p;
	const s16 *w, *x;
	u32 need_x, need_w, need_y, i;
	s16 *y;

	if (method != 0)
		return AEE_EUNSUPPORTED;

	if (nin != 3 || nout != 1)
		return AEE_EBADPARM;

	if (pra[0].nLen < sizeof(*p))
		return AEE_EBADPARM;

	p = pra[0].pv;

	if (!p->channels || !p->ksize || !p->dilation || !p->nsamples)
		return AEE_EBADPARM;

	/* The kernel writes a full vector at a time and never masks the tail. */
	if (p->nsamples % VLANES)
		return AEE_EBADPARM;

	need_w = p->channels * p->channels * p->ksize * sizeof(s16);
	need_x = p->channels * ((p->ksize - 1) * p->dilation + p->nsamples) * sizeof(s16);
	need_y = p->channels * p->nsamples * sizeof(s16);

	if (pra[1].nLen < need_w || pra[2].nLen < need_x || pra[3].nLen < need_y)
		return AEE_EBADPARM;

	w = pra[1].pv;
	x = pra[2].pv;
	y = pra[3].pv;

	for (i = 0; i < (p->iters ? p->iters : 1); i++)
		conv1d_hvx(w, x, y, p->channels, p->ksize, p->dilation,
			   p->nsamples);

	return AEE_SUCCESS;
}
