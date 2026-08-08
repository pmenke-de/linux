// SPDX-License-Identifier: GPL-3.0
/* See hexagon_backend.h. */

#include "hexagon_backend.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/ioctl.h>
#include <vector>

extern "C" {
#include <misc/fastrpc.h>
#include <libhexagonrpc/fastrpc.h>
#include <libhexagonrpc/session.h>
}

extern "C" const struct fastrpc_function_def_interp2 remotectl_open_def;
extern "C" const struct fastrpc_function_def_interp2 remotectl_close_def;

namespace HexagonNam {

namespace {

constexpr uint32_t kRemotectlHandle = 0;

/* Mirrors namwn_skel.c: 0 create, 1 process, 2 destroy. */
int dsp_create(int fd, uint32_t skel, const void *blob, size_t len, uint32_t *model)
{
	struct fastrpc_invoke_args args[2];
	struct fastrpc_invoke inv;

	args[0].ptr = (uint64_t)(uintptr_t)blob;
	args[0].length = len;
	args[0].fd = -1;
	args[1].ptr = (uint64_t)(uintptr_t)model;
	args[1].length = sizeof(*model);
	args[1].fd = -1;

	inv.handle = skel;
	inv.sc = REMOTE_SCALARS_MAKE(0, 1, 1);
	inv.args = (uint64_t)(uintptr_t)args;
	return ioctl(fd, FASTRPC_IOCTL_INVOKE, &inv);
}

int dsp_process(int fd, uint32_t skel, uint32_t model, const int16_t *in,
		int16_t *out, uint32_t n)
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

	inv.handle = skel;
	inv.sc = REMOTE_SCALARS_MAKE(1, 2, 1);
	inv.args = (uint64_t)(uintptr_t)args;
	return ioctl(fd, FASTRPC_IOCTL_INVOKE, &inv);
}

int dsp_destroy(int fd, uint32_t skel, uint32_t model)
{
	struct fastrpc_invoke_args args[1];
	struct fastrpc_invoke inv;

	args[0].ptr = (uint64_t)(uintptr_t)&model;
	args[0].length = sizeof(model);
	args[0].fd = -1;

	inv.handle = skel;
	inv.sc = REMOTE_SCALARS_MAKE(2, 1, 0);
	inv.args = (uint64_t)(uintptr_t)args;
	return ioctl(fd, FASTRPC_IOCTL_INVOKE, &inv);
}

inline int16_t to_q15(float v)
{
	float s = v * 32768.0f;

	if (s >= 32767.0f)
		return 32767;
	if (s <= -32768.0f)
		return -32768;
	return (int16_t)(s >= 0.0f ? s + 0.5f : s - 0.5f);
}

} // namespace

Session::Session()
{
	fd_ = hexagonrpc_fd_from_env();
	if (fd_ < 0) {
		error_ = "no FastRPC session in the environment; "
			 "start the host under cdsp-run";
		return;
	}

	char err[256] = "";
	int32_t dlret = 0;
	const char *name = "namwn";

	int ret = fastrpc2(&remotectl_open_def, fd_, kRemotectlHandle,
			   (uint32_t)strlen(name) + 1, name,
			   &skel_, &dlret, (uint32_t)sizeof(err), err);
	if (ret == -1) {
		error_ = std::string("remotectl_open(namwn): ") + strerror(errno);
		skel_ = 0;
	} else if (dlret) {
		error_ = std::string("remotectl_open(namwn) failed on the DSP: ")
			 + (err[0] ? err : "unknown error");
		skel_ = 0;
	}
}

Session &Session::instance()
{
	static Session s;
	return s;
}

Model *Model::CreateFromFile(const char *path, std::string *error)
{
	Session &s = Session::instance();

	if (!s.ok()) {
		if (error)
			*error = s.error();
		return nullptr;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		if (error)
			*error = std::string("cannot open ") + path + ": " + strerror(errno);
		return nullptr;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> blob((size_t)(len > 0 ? len : 0));
	bool read_ok = len > 0 && fread(blob.data(), 1, (size_t)len, f) == (size_t)len;
	fclose(f);
	if (!read_ok) {
		if (error)
			*error = std::string("cannot read ") + path;
		return nullptr;
	}

	uint32_t model = 0;
	if (dsp_create(s.fd(), s.skel(), blob.data(), blob.size(), &model) || !model) {
		if (error)
			*error = std::string("DSP rejected ") + path +
				 " (is it a .nqw blob from nam_quantize.py?)";
		return nullptr;
	}

	Model *m = new Model();
	m->handle_ = model;
	return m;
}

Model::~Model()
{
	Session &s = Session::instance();

	if (handle_ && s.ok())
		dsp_destroy(s.fd(), s.skel(), handle_);
}

void Model::Reset() noexcept
{
	in_count_ = out_count_ = 0;
}

bool Model::RunBlock(uint32_t n) noexcept
{
	Session &s = Session::instance();

	return dsp_process(s.fd(), s.skel(), handle_, block_in_, block_out_, n) == 0;
}

void Model::Process(float *out, const float *in, uint32_t n) noexcept
{
	if (!handle_ || n == 0)
		return;

	/* Oversized callbacks are split; the FIFOs only need to cover one pass. */
	while (n > 0) {
		uint32_t take = n;

		if (take > kFifoSize - in_count_)
			take = kFifoSize - in_count_;
		if (take == 0)
			break;

		for (uint32_t i = 0; i < take; i++)
			in_fifo_[in_count_ + i] = to_q15(in[i]);
		in_count_ += take;

		/* Drain in the largest legal chunks the DSP will accept. */
		while (in_count_ >= kGranule) {
			uint32_t chunk = in_count_ - (in_count_ % kGranule);

			if (chunk > kMaxBlock)
				chunk = kMaxBlock;
			if (out_count_ + chunk > kFifoSize)
				break;

			memcpy(block_in_, in_fifo_, chunk * sizeof(int16_t));
			if (!RunBlock(chunk)) {
				/* Stay silent rather than emit garbage on a DSP fault. */
				memset(block_out_, 0, chunk * sizeof(int16_t));
			}
			memcpy(out_fifo_ + out_count_, block_out_,
			       chunk * sizeof(int16_t));
			out_count_ += chunk;

			in_count_ -= chunk;
			memmove(in_fifo_, in_fifo_ + chunk, in_count_ * sizeof(int16_t));
		}

		uint32_t give = take < out_count_ ? take : out_count_;

		for (uint32_t i = 0; i < give; i++)
			out[i] = (float)out_fifo_[i] * (1.0f / 32768.0f);
		/* Only short while priming, and only if the host block is unaligned. */
		for (uint32_t i = give; i < take; i++)
			out[i] = 0.0f;

		out_count_ -= give;
		memmove(out_fifo_, out_fifo_ + give, out_count_ * sizeof(int16_t));

		in += take;
		out += take;
		n -= take;
	}
}

} // namespace HexagonNam
