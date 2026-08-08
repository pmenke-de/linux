// SPDX-License-Identifier: GPL-2.0
/*
 * namwn_skel - NeuralAmpModeler WaveNet inference on Hexagon v66 HVX
 *
 * Runs a whole NAM WaveNet model on the CDSP in int16 fixed point. The entire
 * model has to live here rather than being driven layer-by-layer from the AP:
 * a FastRPC round trip costs ~130 us and a 128-sample block at 48 kHz only
 * has 2667 us, so 20 per-layer round trips would not fit.
 *
 * Layout is channel-major, x[channel][time], so one 128-byte vector holds 64
 * consecutive timesteps of a single channel. Every operation in a WaveNet
 * layer - the dilated conv, the 1x1s and the condition mixin - is then the
 * same shape: broadcast a weight, multiply-accumulate across time. Dilation
 * only shifts a load address, which is why the loads are unaligned.
 *
 * Fixed point: activations are int16 with a per-tensor power-of-two shift
 * fixed at model-build time; products accumulate in int32 and are requantised
 * with a single arithmetic shift plus saturation. Per-tensor (rather than one
 * global) shifts matter - measured activation peaks range from 0.5 to 17, so a
 * uniform format would cost about four bits on the tanh outputs, which carry
 * the signal.
 *
 * tanh is a 256-entry linearly interpolated table evaluated on the scalar
 * unit. HVX v66 could do it with vlutvwh, but scalar costs roughly 330 us per
 * block against a 2667 us budget and is far easier to get right; it is the
 * obvious thing to vectorise if the budget ever gets tight.
 *
 * Build: see tools/remoteproc/Makefile (clang, -mv66 -mhvx, -nostdlib).
 * Model blob is produced by tools/remoteproc/nam_quantize.py.
 */

typedef unsigned char u8;
typedef signed short s16;
typedef unsigned int u32;
typedef signed int s32;
typedef signed long long s64;

typedef long HVX_Vector     __attribute__((__vector_size__(128), aligned(128)));
typedef long HVX_UVector    __attribute__((__vector_size__(128), aligned(1)));
typedef long HVX_VectorPair __attribute__((__vector_size__(256), aligned(128)));

#define VLANES 64

#define AEE_SUCCESS       0
#define AEE_EBADPARM      14
#define AEE_EUNSUPPORTED  20
#define AEE_ENOMEMORY     2

#define NQ_MAGIC   0x3257514eu   /* "NQW2" */

#define ACT_TANH  0
#define ACT_LEAKY 1
#define LEAKY_Q15 328            /* 0.01 in Q15 */
#define MAX_GROUPS 4
#define MAX_LAYERS 32
#define MAX_CH     32
#define MAX_FRAMES 256
#define MAX_INSTANCES 4

/* Ring span per layer, in samples, beyond its history requirement. */
#define SPAN 512

/*
 * One arena per instance. The standard model needs ~344 KB of ring buffers and
 * the A2 captures ~300 KB, so 448 KB leaves room without making the module's
 * BSS enormous. Instances get their own arena rather than sharing one bump
 * allocator so that destroying one actually reclaims its memory.
 */
#define ARENA_BYTES (448 * 1024)

struct remote_arg {
	void *pv;
	u32 nLen;
};

/* ---- model blob layout, mirrored exactly by nam_quantize.py ---- */

struct nq_header {
	u32 magic;
	u32 n_groups;
	s32 cond_shift;     /* shift of the condition/input samples */
	s32 head_shift;     /* shift of the final head output */
	s32 head_scale_q;   /* head_scale in Q30 */
	u32 reserved;
};

struct nq_layer {
	u32 dilation;
	u32 ksize;          /* per-layer; A2 models mix 6 and 15 */
	s32 rshift_conv;    /* s_c - (s_w_conv + s_z) */
	s32 rshift_1x1;     /* s_z_out - (s_w_1x1 + s_a) */
	s32 s_c;            /* shift of the pre-activation */
	s32 rshift_z;       /* s_z_out - s_z_in, >= 0: rescale of the residual */
	s32 rshift_mix;     /* s_c - (s_w_mix + s_cond) */
	s32 lshift_a;       /* s_a - head_acc domain; tanh gives 0, LeakyReLU does not */
};

struct nq_group {
	u32 channels, in_ch, cond_ch, head_size, n_layers;
	u32 head_bias;
	u32 head_ksize;       /* 1 for a 1x1 head, 16 for A2's Conv1D head */
	u32 act_type;         /* ACT_TANH or ACT_LEAKY */
	s32 rshift_rechan;
	s32 rshift_head;
	s32 head_acc_rshift;  /* int32 head accumulator -> int16 for head rechannel */
	s32 head_seed_lshift; /* previous group's head -> this group's Q15 accumulator */
};

/*
 * Weights follow the headers in one block, in the same order the model is
 * evaluated, so the DSP can walk them linearly:
 *   per group: rechannel w
 *              per layer: conv w, conv b, mixin w, 1x1 w, 1x1 b
 *              head rechannel w, head rechannel b
 */

/* ---- state ---- */

struct layer_state {
	s16 *ring;      /* [channels][hist + SPAN] */
	u32 hist;
	u32 span;
	u32 wpos;       /* next write index within the per-channel row */
};

struct group_state {
	struct layer_state layers[MAX_LAYERS];
	s16 *z;         /* [channels][MAX_FRAMES] current residual */
	s32 *head_acc;  /* [channels][MAX_FRAMES] int32 skip accumulator */
	s16 *head_out;  /* [head_size][MAX_FRAMES] */
	/* A2's head is a Conv1D over the skip path, so it needs history too. */
	s16 *head_ring;
	u32 head_hist;
	u32 head_wpos;
};

/*
 * One instance per loaded model. A host can put more than one amp in a chain,
 * and a plugin graph may run them from different threads, so model state and
 * scratch are per instance - a single shared set would corrupt silently.
 *
 * Handles carry a generation so a stale handle is rejected rather than
 * quietly addressing whatever was loaded next.
 */
struct instance {
	u32 generation;                 /* 0 when the slot is free */
	struct nq_header hdr;
	struct nq_group groups[MAX_GROUPS];
	struct nq_layer layers[MAX_GROUPS][MAX_LAYERS];
	struct group_state gstate[MAX_GROUPS];
	const s16 *weights_base;        /* walked in evaluation order per block */
	s16 *scratch_a;
	s16 *scratch_c;
	s16 *ring_stage;
	u8 *arena;
	u32 arena_used;
};

static struct instance instances[MAX_INSTANCES];
static u8 arenas[MAX_INSTANCES][ARENA_BYTES] __attribute__((aligned(128)));
static u32 next_generation = 1;

/*
 * tanh table: linear segments over [-8, 8) in Q15, plus per-segment slopes.
 * Interpolation error falls as the square of the segment width, and the
 * activations sit near zero most of the time, where an absolute error is large
 * relative to the signal - hence 4096 segments rather than a token few hundred.
 * Computed on the host and shipped in the blob rather than built here: deriving
 * it on the DSP needs 64-bit division, and -nostdlib has no libgcc __divdi3.
 */
/* Shared: the table is just tanh, identical for every model. */
#define TANH_N 4096
#define TANH_FRAC 6      /* Q(TANH_FRAC+8): one segment is exactly 2^TANH_FRAC */
#define TANH_Q    14
static s16 tanh_base[TANH_N + 1];
static s16 tanh_slope[TANH_N];


__attribute__((visibility("hidden")))
void *memset(void *dst, int c, __SIZE_TYPE__ n)
{
	u8 *d = dst;

	while (n--)
		*d++ = (u8)c;
	return dst;
}

__attribute__((visibility("hidden")))
void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n)
{
	u8 *d = dst;
	const u8 *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

static void *arena_alloc(struct instance *in, u32 bytes)
{
	void *p;

	bytes = (bytes + 127u) & ~127u;
	if (in->arena_used + bytes > ARENA_BYTES)
		return 0;
	p = &in->arena[in->arena_used];
	in->arena_used += bytes;
	return p;
}

static struct instance *handle_to_instance(u32 handle)
{
	u32 idx = handle & 0xf;
	u32 gen = handle >> 4;

	if (idx >= MAX_INSTANCES || gen == 0)
		return 0;
	if (instances[idx].generation != gen)
		return 0;
	return &instances[idx];
}

static s16 sat16(s32 v)
{
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (s16)v;
}

/*
 * a = tanh(c), with c at shift s_c and a in Q15.
 *
 * The table has 256 segments over [-8, 8), so one segment is 1/16 of a unit.
 * Indexing wants value*16 and interpolation wants four more fractional bits,
 * i.e. the input has to arrive as value*256 - Q8, not Q4. Since
 * value = c * 2^s_c, that is a shift of (s_c + 8), which for the shifts this
 * model produces is always a right shift.
 */
static void apply_tanh(const s16 *c, s16 *a, u32 n, s32 s_c)
{
	const s32 to_q = s_c + TANH_Q;
	const s32 lim = (TANH_N / 2) << TANH_FRAC;
	u32 i;

	for (i = 0; i < n; i++) {
		s32 v = c[i];
		s32 idx, frac;

		if (to_q >= 0)
			v <<= to_q;
		else
			v >>= -to_q;

		if (v >= lim - 1) {
			a[i] = 32767;
			continue;
		}
		if (v <= -lim) {
			a[i] = -32767;
			continue;
		}

		idx = (v >> TANH_FRAC) + TANH_N / 2;
		frac = v & ((1 << TANH_FRAC) - 1);
		a[i] = sat16(tanh_base[idx]
			     + ((tanh_slope[idx] * frac) >> TANH_FRAC));
	}
}

/*
 * LeakyReLU keeps the input's shift: it never increases magnitude, so s_a = s_c
 * and the negative branch is a Q15 multiply. Unlike tanh the output is
 * unbounded, which is exactly why it cannot share tanh's fixed Q15 output.
 */
static void apply_leaky(const s16 *c, s16 *a, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++) {
		s32 v = c[i];

		a[i] = v >= 0 ? (s16)v
			      : sat16((v * LEAKY_Q15 + (1 << 14)) >> 15);
	}
}

/*
 * out[o][t] = sat(( bias[o] + sum_i sum_k w[o][i][k] * x[i][t + k*dil] ) >> rsh)
 *
 * x rows are xstride apart and already positioned so that t=0 lines up with
 * the oldest tap. n must be a multiple of VLANES.
 */
static void conv_hvx(const s16 *w, const s32 *bias, const s16 *x, u32 xstride,
		     s16 *out, u32 out_stride, u32 out_ch, u32 in_ch,
		     u32 ksize, u32 dil, u32 n, s32 rsh, int accumulate)
{
	u32 o, i, k, t;

	for (o = 0; o < out_ch; o++) {
		for (t = 0; t < n; t += VLANES) {
			HVX_VectorPair acc;
			HVX_Vector lo, hi, res;

			acc = __builtin_HEXAGON_V6_vcombine_128B(
				__builtin_HEXAGON_V6_vd0_128B(),
				__builtin_HEXAGON_V6_vd0_128B());

			for (i = 0; i < in_ch; i++) {
				const s16 *xrow = x + (u32)i * xstride + t;
				const s16 *wrow = w + ((u32)o * in_ch + i) * ksize;

				for (k = 0; k < ksize; k++) {
					HVX_Vector xv, wv;

					xv = *(const HVX_UVector *)(xrow + k * dil);
					wv = __builtin_HEXAGON_V6_lvsplath_128B(wrow[k]);
					acc = __builtin_HEXAGON_V6_vmpyhv_acc_128B(acc, xv, wv);
				}
			}

			lo = __builtin_HEXAGON_V6_lo_128B(acc);
			hi = __builtin_HEXAGON_V6_hi_128B(acc);

			if (bias) {
				HVX_Vector b = __builtin_HEXAGON_V6_lvsplatw_128B(bias[o]);

				lo = __builtin_HEXAGON_V6_vaddw_128B(lo, b);
				hi = __builtin_HEXAGON_V6_vaddw_128B(hi, b);
			}

			/*
			 * vasr(...):sat takes its shift from a 4-bit register
			 * field, so it can only shift by 0..15. The requantise
			 * shifts this model needs reach 20, so anything over 15
			 * is pre-applied to the 32-bit accumulators with vasrw
			 * (which accepts up to 31) and the rest is done in the
			 * narrowing step. The bits dropped early would have been
			 * shifted out regardless.
			 */
			if (rsh > 15) {
				lo = __builtin_HEXAGON_V6_vasrw_128B(lo, rsh - 15);
				hi = __builtin_HEXAGON_V6_vasrw_128B(hi, rsh - 15);
				res = __builtin_HEXAGON_V6_vasrwhsat_128B(hi, lo, 15);
			} else {
				res = __builtin_HEXAGON_V6_vasrwhsat_128B(hi, lo, rsh);
			}

			if (accumulate) {
				HVX_Vector prev =
					*(const HVX_UVector *)(out + (u32)o * out_stride + t);

				res = __builtin_HEXAGON_V6_vaddhsat_128B(prev, res);
			}

			*(HVX_UVector *)(out + (u32)o * out_stride + t) = res;
		}
	}
}

/*
 * Dilated conv and condition mixin summed in one 32-bit accumulator.
 *
 * Requantising the two separately to int16 and adding costs about 14 dB: the
 * mixin partially cancels the conv, so the conv alone can swing wider than
 * their sum and clips against a range chosen for the sum. Measured at layer 1
 * of the standard model, the conv came out at 31 dB on its own while its input
 * was still 76 dB.
 *
 * The two accumulators live in different domains, so both are brought to the
 * coarser of the two - the one with the smaller requantise shift - before being
 * added, and the remaining shift finishes the job.
 */
static void conv_mixin_hvx(const s16 *w, const s32 *bias, const s16 *x,
			   u32 xstride, u32 ksize, u32 dil, s32 rsh_conv,
			   const s16 *mw, const s16 *cond, u32 cond_ch,
			   s32 rsh_mix, s16 *out, u32 out_stride, u32 ch,
			   u32 n)
{
	const s32 rs_out = rsh_conv < rsh_mix ? rsh_conv : rsh_mix;
	const s32 sh_c = rsh_conv - rs_out;
	const s32 sh_m = rsh_mix - rs_out;
	u32 o, i, k, t;

	for (o = 0; o < ch; o++) {
		for (t = 0; t < n; t += VLANES) {
			HVX_VectorPair ac, am;
			HVX_Vector lo, hi, mlo, mhi, res;

			ac = __builtin_HEXAGON_V6_vcombine_128B(
				__builtin_HEXAGON_V6_vd0_128B(),
				__builtin_HEXAGON_V6_vd0_128B());
			am = ac;

			for (i = 0; i < ch; i++) {
				const s16 *xrow = x + (u32)i * xstride + t;
				const s16 *wrow = w + ((u32)o * ch + i) * ksize;

				for (k = 0; k < ksize; k++)
					ac = __builtin_HEXAGON_V6_vmpyhv_acc_128B(
						ac,
						*(const HVX_UVector *)(xrow + k * dil),
						__builtin_HEXAGON_V6_lvsplath_128B(wrow[k]));
			}

			for (i = 0; i < cond_ch; i++)
				am = __builtin_HEXAGON_V6_vmpyhv_acc_128B(
					am,
					*(const HVX_UVector *)(cond + (u32)i * n + t),
					__builtin_HEXAGON_V6_lvsplath_128B(mw[o * cond_ch + i]));

			lo = __builtin_HEXAGON_V6_lo_128B(ac);
			hi = __builtin_HEXAGON_V6_hi_128B(ac);
			if (bias) {
				HVX_Vector b = __builtin_HEXAGON_V6_lvsplatw_128B(bias[o]);

				lo = __builtin_HEXAGON_V6_vaddw_128B(lo, b);
				hi = __builtin_HEXAGON_V6_vaddw_128B(hi, b);
			}
			if (sh_c) {
				lo = __builtin_HEXAGON_V6_vasrw_128B(lo, sh_c);
				hi = __builtin_HEXAGON_V6_vasrw_128B(hi, sh_c);
			}

			mlo = __builtin_HEXAGON_V6_lo_128B(am);
			mhi = __builtin_HEXAGON_V6_hi_128B(am);
			if (sh_m) {
				mlo = __builtin_HEXAGON_V6_vasrw_128B(mlo, sh_m);
				mhi = __builtin_HEXAGON_V6_vasrw_128B(mhi, sh_m);
			}

			lo = __builtin_HEXAGON_V6_vaddw_128B(lo, mlo);
			hi = __builtin_HEXAGON_V6_vaddw_128B(hi, mhi);

			if (rs_out > 15) {
				lo = __builtin_HEXAGON_V6_vasrw_128B(lo, rs_out - 15);
				hi = __builtin_HEXAGON_V6_vasrw_128B(hi, rs_out - 15);
				res = __builtin_HEXAGON_V6_vasrwhsat_128B(hi, lo, 15);
			} else {
				res = __builtin_HEXAGON_V6_vasrwhsat_128B(hi, lo, rs_out);
			}

			*(HVX_UVector *)(out + (u32)o * out_stride + t) = res;
		}
	}
}

/*
 * Accumulate int16 activations into the int32 skip path.
 *
 * tanh always lands in Q15 so every layer shares a domain, but LeakyReLU keeps
 * its input's shift, which differs per layer. The accumulator therefore runs at
 * the finest of them and each contribution is shifted up into it - int32 has
 * ample room, so this costs nothing rather than forcing a lossy common shift.
 */
static void head_accumulate(s32 *acc, const s16 *a, u32 ch, u32 stride, u32 n,
			    s32 lshift)
{
	u32 c, t;

	for (c = 0; c < ch; c++)
		for (t = 0; t < n; t++)
			acc[c * MAX_FRAMES + t] +=
				(s32)a[c * stride + t] << lshift;
}

static int setup_state(struct instance *in)
{
	u32 g, l;

	for (g = 0; g < in->hdr.n_groups; g++) {
		struct nq_group *G = &in->groups[g];
		struct group_state *S = &in->gstate[g];

		S->z = arena_alloc(in, G->channels * MAX_FRAMES * sizeof(s16));
		S->head_acc = arena_alloc(in, G->channels * MAX_FRAMES * sizeof(s32));
		S->head_out = arena_alloc(in, G->head_size * MAX_FRAMES * sizeof(s16));
		if (!S->z || !S->head_acc || !S->head_out)
			return AEE_ENOMEMORY;

		S->head_hist = G->head_ksize - 1;
		S->head_wpos = S->head_hist;
		if (S->head_hist) {
			u32 hrow = S->head_hist + SPAN;

			S->head_ring = arena_alloc(in, G->channels * hrow * sizeof(s16));
			if (!S->head_ring)
				return AEE_ENOMEMORY;
			memset(S->head_ring, 0, G->channels * hrow * sizeof(s16));
		} else {
			S->head_ring = 0;
		}

		for (l = 0; l < G->n_layers; l++) {
			struct layer_state *L = &S->layers[l];
			u32 hist = (in->layers[g][l].ksize - 1) * in->layers[g][l].dilation;
			u32 row = hist + SPAN;

			L->hist = hist;
			L->span = SPAN;
			L->wpos = hist;
			L->ring = arena_alloc(in, G->channels * row * sizeof(s16));
			if (!L->ring)
				return AEE_ENOMEMORY;
			memset(L->ring, 0, G->channels * row * sizeof(s16));
		}
	}

	in->scratch_a = arena_alloc(in, MAX_CH * MAX_FRAMES * sizeof(s16));
	in->scratch_c = arena_alloc(in, MAX_CH * MAX_FRAMES * sizeof(s16));
	in->ring_stage = arena_alloc(in, MAX_CH * MAX_FRAMES * sizeof(s16));
	if (!in->scratch_a || !in->scratch_c || !in->ring_stage)
		return AEE_ENOMEMORY;

	return AEE_SUCCESS;
}

static int method_create(struct remote_arg *pra)
{
	const u8 *blob = pra[0].pv;
	u32 len = pra[0].nLen;
	struct instance *in = 0;
	u32 off, g, idx;

	if (pra[1].nLen < sizeof(u32))
		return AEE_EBADPARM;

	for (idx = 0; idx < MAX_INSTANCES; idx++) {
		if (!instances[idx].generation) {
			in = &instances[idx];
			break;
		}
	}
	if (!in)
		return AEE_ENOMEMORY;

	memset(in, 0, sizeof(*in));
	in->arena = arenas[idx];

	if (len < sizeof(struct nq_header))
		return AEE_EBADPARM;

	memcpy(&in->hdr, blob, sizeof(in->hdr));
	if (in->hdr.magic != NQ_MAGIC || in->hdr.n_groups == 0
	    || in->hdr.n_groups > MAX_GROUPS)
		return AEE_EBADPARM;

	off = sizeof(in->hdr);
	if (len < off + in->hdr.n_groups * sizeof(struct nq_group))
		return AEE_EBADPARM;

	for (g = 0; g < in->hdr.n_groups; g++) {
		memcpy(&in->groups[g], blob + off, sizeof(struct nq_group));
		off += sizeof(struct nq_group);

		if (in->groups[g].channels > MAX_CH
		    || in->groups[g].n_layers > MAX_LAYERS
		    || in->groups[g].channels == 0
		    || in->groups[g].n_layers == 0)
			return AEE_EBADPARM;
	}

	for (g = 0; g < in->hdr.n_groups; g++) {
		u32 l;

		if (off + in->groups[g].n_layers * sizeof(struct nq_layer) > len)
			return AEE_EBADPARM;
		for (l = 0; l < in->groups[g].n_layers; l++) {
			memcpy(&in->layers[g][l], blob + off,
			       sizeof(struct nq_layer));
			off += sizeof(struct nq_layer);
		}
	}

	if (off & 1)
		off++;

	/* tanh table comes next: TANH_N+1 bases then TANH_N slopes. */
	if (off + (TANH_N * 2 + 1) * sizeof(s16) > len)
		return AEE_EBADPARM;
	memcpy(tanh_base, blob + off, sizeof(tanh_base));
	off += sizeof(tanh_base);
	memcpy(tanh_slope, blob + off, sizeof(tanh_slope));
	off += sizeof(tanh_slope);

	/* Biases are int32 embedded in the int16 weight stream, so the base of
	 * that stream has to be 4-aligned or those loads fault. */
	off = (off + 3u) & ~3u;
	if (off > len)
		return AEE_EBADPARM;

	if (setup_state(in) != AEE_SUCCESS)
		return AEE_ENOMEMORY;

	/*
	 * Keep the weights: FastRPC input buffers are only valid for the
	 * duration of the call, so copy them into the arena rather than
	 * pointing at caller memory.
	 */
	{
		u32 wbytes = len - off;
		s16 *dst = arena_alloc(in, wbytes);

		if (!dst)
			return AEE_ENOMEMORY;
		memcpy(dst, blob + off, wbytes);
		in->weights_base = dst;
	}

	in->generation = next_generation++;
	if (next_generation == 0)               /* never hand out generation 0 */
		next_generation = 1;

	*(u32 *)pra[1].pv = (in->generation << 4) | idx;
	return AEE_SUCCESS;
}

static int method_destroy(struct remote_arg *pra)
{
	struct instance *in;

	if (pra[0].nLen < sizeof(u32))
		return AEE_EBADPARM;

	in = handle_to_instance(*(const u32 *)pra[0].pv);
	if (!in)
		return AEE_EBADPARM;

	in->generation = 0;      /* invalidates any outstanding handle */
	in->arena_used = 0;
	return AEE_SUCCESS;
}

/* Push n new samples of `src` (channel-major, stride n) into a layer's ring. */
static void ring_push(struct layer_state *L, u32 ch, const s16 *src, u32 n)
{
	u32 row = L->hist + L->span;
	u32 c;

	if (L->wpos + n > row) {
		/* Rewind: keep only the history the taps still need. */
		for (c = 0; c < ch; c++)
			memcpy(L->ring + c * row,
			       L->ring + c * row + L->wpos - L->hist,
			       L->hist * sizeof(s16));
		L->wpos = L->hist;
	}

	for (c = 0; c < ch; c++)
		memcpy(L->ring + c * row + L->wpos, src + c * n, n * sizeof(s16));

	L->wpos += n;
}

static int method_process(struct remote_arg *pra)
{
	struct instance *inst;
	const s16 *in_samples = pra[1].pv;
	s16 *out = pra[2].pv;
	u32 n = pra[1].nLen / sizeof(s16);
	const s16 *w;
	u32 g, l, c, t;

	if (pra[0].nLen < sizeof(u32))
		return AEE_EBADPARM;
	inst = handle_to_instance(*(const u32 *)pra[0].pv);
	if (!inst)
		return AEE_EBADPARM;

	if (n == 0 || n > MAX_FRAMES || (n % VLANES) != 0)
		return AEE_EBADPARM;
	if (pra[2].nLen < n * sizeof(s16))
		return AEE_EBADPARM;

	w = inst->weights_base;

	for (g = 0; g < inst->hdr.n_groups; g++) {
		struct nq_group *G = &inst->groups[g];
		struct group_state *S = &inst->gstate[g];
		const s16 *layer_in = (g == 0) ? in_samples : inst->gstate[g - 1].z;
		u32 in_stride = (g == 0) ? n : MAX_FRAMES;

		/* rechannel: 1x1, no bias, in_ch -> channels */
		conv_hvx(w, 0, layer_in, in_stride, S->z, MAX_FRAMES,
			 G->channels, G->in_ch, 1, 1, n, G->rshift_rechan, 0);
		w += G->channels * G->in_ch;

		/* head accumulator: zero for group 0, else previous group's head */
		if (g == 0) {
			for (c = 0; c < G->channels; c++)
				for (t = 0; t < n; t++)
					S->head_acc[c * MAX_FRAMES + t] = 0;
		} else {
			struct nq_group *P = &inst->groups[g - 1];

			/*
			 * The skip path is carried in Q15 int32 here, but the
			 * previous group's head sits at its own shift, so seed
			 * it up into this accumulator's domain.
			 */
			for (c = 0; c < G->channels; c++)
				for (t = 0; t < n; t++)
					S->head_acc[c * MAX_FRAMES + t] =
						(c < P->head_size)
						? ((s32)inst->gstate[g - 1].head_out[c * MAX_FRAMES + t]
						   << G->head_seed_lshift)
						: 0;
		}

		for (l = 0; l < G->n_layers; l++) {
			struct nq_layer *P = &inst->layers[g][l];
			struct layer_state *L = &S->layers[l];
			u32 row = L->hist + L->span;
			const s16 *conv_w = w;
			const s32 *conv_b;
			const s16 *mix_w, *one_w;
			const s32 *one_b;

			w += G->channels * G->channels * P->ksize;
			conv_b = (const s32 *)w;
			w += G->channels * 2;
			mix_w = w;
			w += G->channels * G->cond_ch;
			one_w = w;
			w += G->channels * G->channels;
			one_b = (const s32 *)w;
			w += G->channels * 2;

			/* feed this layer's ring from the current residual */
			for (c = 0; c < G->channels; c++)
				memcpy(inst->ring_stage + c * n, S->z + c * MAX_FRAMES,
				       n * sizeof(s16));
			ring_push(L, G->channels, inst->ring_stage, n);

			/* dilated conv and mixin, summed before requantising */
			conv_mixin_hvx(conv_w, conv_b,
				       L->ring + (L->wpos - n - L->hist), row,
				       P->ksize, P->dilation, P->rshift_conv,
				       mix_w, in_samples, G->cond_ch, P->rshift_mix,
				       inst->scratch_c, MAX_FRAMES, G->channels, n);

			/* activation */
			for (c = 0; c < G->channels; c++) {
				if (G->act_type == ACT_LEAKY)
					apply_leaky(inst->scratch_c + c * MAX_FRAMES,
						    inst->scratch_a + c * MAX_FRAMES, n);
				else
					apply_tanh(inst->scratch_c + c * MAX_FRAMES,
						   inst->scratch_a + c * MAX_FRAMES,
						   n, P->s_c);
			}

			head_accumulate(S->head_acc, inst->scratch_a, G->channels,
					MAX_FRAMES, n, P->lshift_a);

			/* residual: z = (z >> rshift_z) + 1x1(a) */
			if (P->rshift_z > 0) {
				for (c = 0; c < G->channels; c++)
					for (t = 0; t < n; t++)
						S->z[c * MAX_FRAMES + t] >>= P->rshift_z;
			}
			conv_hvx(one_w, one_b, inst->scratch_a, MAX_FRAMES,
				 S->z, MAX_FRAMES, G->channels, G->channels,
				 1, 1, n, P->rshift_1x1, 1);
		}

		/* head rechannel: requantise the int32 skip path, then 1x1 */
		for (c = 0; c < G->channels; c++)
			for (t = 0; t < n; t++)
				inst->scratch_a[c * MAX_FRAMES + t] =
					sat16(S->head_acc[c * MAX_FRAMES + t]
					      >> G->head_acc_rshift);

		{
			const s16 *hw = w;
			const s32 *hb;

			w += G->head_size * G->channels * G->head_ksize;
			hb = (const s32 *)w;
			if (G->head_bias)
				w += G->head_size * 2;

			if (G->head_ksize > 1) {
				/*
				 * A2's head is a Conv1D over the skip path, so
				 * it carries history across blocks like any
				 * other dilated layer.
				 */
				struct layer_state hl;
				u32 hrow = S->head_hist + SPAN;

				hl.ring = S->head_ring;
				hl.hist = S->head_hist;
				hl.span = SPAN;
				hl.wpos = S->head_wpos;

				for (c = 0; c < G->channels; c++)
					memcpy(inst->ring_stage + c * n,
					       inst->scratch_a + c * MAX_FRAMES,
					       n * sizeof(s16));
				ring_push(&hl, G->channels, inst->ring_stage, n);
				S->head_wpos = hl.wpos;

				conv_hvx(hw, G->head_bias ? hb : 0,
					 hl.ring + (hl.wpos - n - hl.hist), hrow,
					 S->head_out, MAX_FRAMES, G->head_size,
					 G->channels, G->head_ksize, 1, n,
					 G->rshift_head, 0);
			} else {
				conv_hvx(hw, G->head_bias ? hb : 0, inst->scratch_a,
					 MAX_FRAMES, S->head_out, MAX_FRAMES,
					 G->head_size, G->channels, 1, 1, n,
					 G->rshift_head, 0);
			}
		}
	}

	/* final output = head_scale * last group's head, channel 0 */
	{
		struct group_state *S = &inst->gstate[inst->hdr.n_groups - 1];

		for (t = 0; t < n; t++) {
			s64 v = (s64)S->head_out[t] * inst->hdr.head_scale_q;

			out[t] = sat16((s32)(v >> 30));
		}
	}

	return AEE_SUCCESS;
}

__attribute__((visibility("default")))
int namwn_skel_invoke(u32 sc, struct remote_arg *pra)
{
	const u32 method = (sc >> 24) & 0x1f;
	const u32 nin  = (sc >> 16) & 0xff;
	const u32 nout = (sc >> 8) & 0xff;

	switch (method) {
	case 0:                         /* create: blob in, handle out */
		if (nin != 1 || nout != 1)
			return AEE_EBADPARM;
		return method_create(pra);
	case 1:                         /* process: handle + samples in, samples out */
		if (nin != 2 || nout != 1)
			return AEE_EBADPARM;
		return method_process(pra);
	case 2:                         /* destroy: handle in */
		if (nin != 1 || nout != 0)
			return AEE_EBADPARM;
		return method_destroy(pra);
	default:
		return AEE_EUNSUPPORTED;
	}
}
