// SPDX-License-Identifier: GPL-3.0
/*
 * hexagon_backend - run a NAM model on the Hexagon 698 CDSP from an LV2 plugin
 *
 * Drop-in replacement for the NeuralAudio model in mikeoliphant's
 * neural-amp-modeler-lv2: same two-call shape (load off the audio thread,
 * Process() on it), but inference happens on the DSP.
 *
 * Models are pre-quantised .nqw blobs built by tools/remoteproc/nam_quantize.py.
 * Picking the fixed-point shifts needs a float pass over real audio, which is
 * not something to do inside a plugin load, so that stays an offline step.
 *
 * A FastRPC protection domain belongs to the process that created it, and this
 * code is a shared library inside somebody else's host. For now the host has to
 * be started under cdsp-run so the fd arrives in the environment:
 *
 *   sudo cdsp-run "jalv http://github.com/mikeoliphant/neural-amp-modeler-lv2"
 *
 * Making the plugin stand up its own domain means lifting hexagonrpcd's
 * listener into this library; Session is the seam where that would go.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace HexagonNam {

/*
 * The DSP wants blocks that are a multiple of 64 and at most 256. Hosts hand us
 * whatever they like, so audio is queued and drained in legal chunks. When the
 * host block is already a multiple of 64 - which every common one is - nothing
 * ever accumulates and the plugin adds no latency at all.
 */
static constexpr uint32_t kGranule = 64;
static constexpr uint32_t kMaxBlock = 256;
static constexpr uint32_t kFifoSize = 2048;

/* Process-wide FastRPC session and skel handle, opened once. */
class Session {
public:
	static Session &instance();

	bool ok() const { return fd_ >= 0 && skel_ != 0; }
	const std::string &error() const { return error_; }
	int fd() const { return fd_; }
	uint32_t skel() const { return skel_; }

private:
	Session();
	int fd_ = -1;
	uint32_t skel_ = 0;
	std::string error_;
};

class Model {
public:
	/* Worker thread: loads a .nqw blob and creates a DSP instance. */
	static Model *CreateFromFile(const char *path, std::string *error);
	~Model();

	/* Audio thread. in and out may alias. */
	void Process(float *out, const float *in, uint32_t n) noexcept;

	void Reset() noexcept;

	/* Kept so the plugin's existing call sites need no special-casing. */
	float GetRecommendedInputDBAdjustment() const { return 0.0f; }
	int GetReceptiveFieldSize() const { return -1; }
	void SetQualityScaleFactor(float) {}
	float GetQualityScaleFactor() const { return 1.0f; }

private:
	Model() = default;
	bool RunBlock(uint32_t n) noexcept;

	uint32_t handle_ = 0;

	int16_t in_fifo_[kFifoSize] = {};
	int16_t out_fifo_[kFifoSize] = {};
	uint32_t in_count_ = 0;
	uint32_t out_count_ = 0;

	int16_t block_in_[kMaxBlock] = {};
	int16_t block_out_[kMaxBlock] = {};
};

} // namespace HexagonNam
