/*
 * Copyright (c) 2025 Bruce Zawadzki (Tone Coder)
 * Copyright (c) 2026 Rudement
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "visualizer_fft.h"
#include "dsp/fft/fft_config_manager.h"
#include "hid/display/visualizer.h"
#include "util/fixedpoint.h"
#include "util/functions.h"
#include "visualizer_common.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace deluge::hid::display {

namespace {

static_assert((Visualizer::kVisualizerBufferSize & (Visualizer::kVisualizerBufferSize - 1)) == 0,
              "the ring is masked, not divided, so its size has to be a power of two");
static_assert(Visualizer::kVisualizerBufferSize >= kVisualizerFFTSize, "the window has to fit in the ring");

/// Q15 sample times Q31 window, shifted back by 16. Kept exactly as upstream, because every mode's
/// reference magnitude is calibrated against this scaling and that calibration was done on hardware.
constexpr int32_t kQ31ToQ15Shift = 16;

constexpr int32_t kFFTSilenceThreshold = 100;
constexpr int32_t kSilenceCheckStride = 16;

/// Recompute once the ring has moved a quarter of a window on — 2.9 ms, so at most ~344 transforms
/// a second even if the renderer asked on every block.
constexpr uint32_t kCacheThreshold = kVisualizerFFTSize / 4;

std::array<int32_t, kVisualizerFFTSize> fft_input{};
std::array<int32_t, kVisualizerFFTSize> fft_input_left{};
std::array<int32_t, kVisualizerFFTSize> fft_input_right{};

std::array<ne10_fft_cpx_int32_t, kSpectrumFFTOutputSize> fft_output{};
std::array<ne10_fft_cpx_int32_t, kSpectrumFFTOutputSize> fft_output_left{};
std::array<ne10_fft_cpx_int32_t, kSpectrumFFTOutputSize> fft_output_right{};

std::array<int32_t, kVisualizerFFTSize> hanning_window{};
bool window_ready = false;

uint32_t mono_cached_at = 0;
bool mono_cache_valid = false;
uint32_t stereo_cached_at = 0;
bool stereo_cache_valid = false;

/// True when the ring has not moved far enough since `cached_at` to be worth transforming again.
bool cacheStillGoodFor(uint32_t cached_at, uint32_t write_pos) {
	uint32_t advanced = (write_pos - cached_at) & (Visualizer::kVisualizerBufferSize - 1);
	return advanced < kCacheThreshold;
}

/// Copies the newest window out of one ring and applies the window function.
void windowInto(std::array<int32_t, kVisualizerFFTSize>& dest,
                const std::array<int32_t, Visualizer::kVisualizerBufferSize>& ring, uint32_t write_pos) {
	uint32_t read_start =
	    (write_pos - static_cast<uint32_t>(kVisualizerFFTSize)) & (Visualizer::kVisualizerBufferSize - 1);
	for (int32_t i = 0; i < kVisualizerFFTSize; i++) {
		int32_t sample = ring[(read_start + i) & (Visualizer::kVisualizerBufferSize - 1)];
		int64_t windowed = (static_cast<int64_t>(sample) * static_cast<int64_t>(hanning_window[i])) >> kQ31ToQ15Shift;
		dest[i] = static_cast<int32_t>(windowed);
	}
}

} // namespace

void initSpectrumHanningWindow() {
	if (window_ready) {
		return;
	}
	window_ready = true;
	for (int32_t i = 0; i < kVisualizerFFTSize; i++) {
		float w = 0.5f
		          * (1.0f
		             - std::cos(2.0f * std::numbers::pi_v<float>
		                        * static_cast<float>(i) / static_cast<float>(kVisualizerFFTSize - 1)));
		hanning_window[i] = static_cast<int32_t>(w * ONE_Q31f);
	}
}

bool isFFTSilent(const ne10_fft_cpx_int32_t* fft_output_to_check, int32_t threshold) {
	// The middle bin first: on anything with content up there this returns immediately.
	if (fastPythag(fft_output_to_check[kSpectrumFFTOutputSize / 2].r, fft_output_to_check[kSpectrumFFTOutputSize / 2].i)
	    >= threshold) {
		return false;
	}
	for (int32_t i = 0; i < kSpectrumFFTOutputSize; i += kSilenceCheckStride) {
		if (fastPythag(fft_output_to_check[i].r, fft_output_to_check[i].i) >= threshold) {
			return false;
		}
	}
	return true;
}

FFTResult computeVisualizerFFT() {
	FFTResult result{};

	if (Visualizer::visualizer_sample_count.load(std::memory_order_relaxed) < kVisualizerFFTSize) {
		return result; // not enough audio captured yet
	}

	ne10_fft_r2c_cfg_int32_t config = FFTConfigManager::getConfig(kSpectrumFFTMagnitude);
	if (config == nullptr) {
		// Allocation failed, or we are too early in boot. Drawing nothing is the right answer.
		return result;
	}

	uint32_t write_pos = Visualizer::visualizer_write_pos.load(std::memory_order_relaxed);

	if (!mono_cache_valid || !cacheStillGoodFor(mono_cached_at, write_pos)) {
		initSpectrumHanningWindow();
		windowInto(fft_input, Visualizer::visualizer_sample_buffer, write_pos);
		ne10_fft_r2c_1d_int32_neon(fft_output.data(), fft_input.data(), config, 0);
		mono_cached_at = write_pos;
		mono_cache_valid = true;
	}

	result.output = fft_output.data();
	result.isValid = true;
	result.isSilent = isFFTSilent(fft_output.data(), kFFTSilenceThreshold);
	return result;
}

FFTResult computeVisualizerStereoFFT() {
	FFTResult result{};

	if (Visualizer::visualizer_sample_count.load(std::memory_order_relaxed) < kVisualizerFFTSize) {
		return result;
	}

	ne10_fft_r2c_cfg_int32_t config = FFTConfigManager::getConfig(kSpectrumFFTMagnitude);
	if (config == nullptr) {
		return result;
	}

	uint32_t write_pos = Visualizer::visualizer_write_pos.load(std::memory_order_relaxed);

	if (!stereo_cache_valid || !cacheStillGoodFor(stereo_cached_at, write_pos)) {
		initSpectrumHanningWindow();
		windowInto(fft_input_left, Visualizer::visualizer_sample_buffer_left, write_pos);
		windowInto(fft_input_right, Visualizer::visualizer_sample_buffer_right, write_pos);
		ne10_fft_r2c_1d_int32_neon(fft_output_left.data(), fft_input_left.data(), config, 0);
		ne10_fft_r2c_1d_int32_neon(fft_output_right.data(), fft_input_right.data(), config, 0);
		stereo_cached_at = write_pos;
		stereo_cache_valid = true;
	}

	result.output_left = fft_output_left.data();
	result.output_right = fft_output_right.data();
	result.isValid = true;
	result.isStereo = true;
	result.isSilent = isFFTSilent(fft_output_left.data(), kFFTSilenceThreshold)
	                  && isFFTSilent(fft_output_right.data(), kFFTSilenceThreshold);
	return result;
}

float calculateWeightedMagnitude(const FFTResult& fft_result, float lower_freq, float upper_freq,
                                 float freq_resolution) {
	if (fft_result.output == nullptr) {
		return 0.0f;
	}

	constexpr float kMaxBin = static_cast<float>(kSpectrumFFTOutputSize - 1);

	float start_bin_f = std::clamp(lower_freq / freq_resolution, 0.0f, kMaxBin);
	float end_bin_f = std::clamp(upper_freq / freq_resolution, 0.0f, kMaxBin);
	if (end_bin_f <= start_bin_f) {
		// A band narrower than one bin still has to read something.
		end_bin_f = std::min(start_bin_f + 1.0f, kMaxBin);
	}

	auto start_bin = std::clamp<int32_t>(static_cast<int32_t>(start_bin_f), 0, kSpectrumFFTOutputSize - 1);
	auto end_bin = std::clamp<int32_t>(static_cast<int32_t>(end_bin_f), 0, kSpectrumFFTOutputSize - 1);

	float weighted_sum = 0.0f;
	float total_weight = 0.0f;

	for (int32_t bin = start_bin; bin <= end_bin; bin++) {
		// How much of this bin falls inside the band, counted in bins.
		float bin_low = static_cast<float>(bin);
		float overlap = std::min(bin_low + 1.0f, end_bin_f) - std::max(bin_low, start_bin_f);
		if (overlap <= 0.0f) {
			continue;
		}

		int32_t magnitude = fastPythag(fft_result.output[bin].r, fft_result.output[bin].i);
		if (bin == 0) {
			magnitude /= kDcBinReductionFactor;
		}

		weighted_sum += static_cast<float>(magnitude) * overlap;
		total_weight += overlap;
	}

	return (total_weight > 0.0f) ? (weighted_sum / total_weight) : 0.0f;
}

} // namespace deluge::hid::display
