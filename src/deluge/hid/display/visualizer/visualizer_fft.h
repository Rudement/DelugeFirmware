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

#pragma once

#include "NE10.h"
#include "visualizer_common.h"

namespace deluge::hid::display {

/// 512 points at 44.1 kHz is an 86.13 Hz bin and an 11.6 ms window. Bigger separates the low bands
/// better and costs more; it is not worth it, because telling 31 Hz from 50 Hz needs about 4096
/// points. The bottom few bands of the bar modes therefore move together — a property of the
/// window, not a bug.
constexpr int32_t kVisualizerFFTSize = 512;
constexpr int32_t kSpectrumFFTMagnitude = 9; // 2^9
constexpr int32_t kSpectrumFFTOutputSize = (kVisualizerFFTSize >> 1) + 1;

/// Mono transform of the newest window. Cached: it will not recompute until the ring has advanced a
/// quarter of a window, so it cannot run more than once every 2.9 ms however often it is asked for.
FFTResult computeVisualizerFFT();

/// The same for the left and right channels, cached separately. Only the stereo modes call it, so
/// the second and third transforms are only paid for while one of those is on screen.
FFTResult computeVisualizerStereoFFT();

/// Builds the Hanning window on first use.
void initSpectrumHanningWindow();

/// Whether a transform's output is all below `threshold`.
bool isFFTSilent(const ne10_fft_cpx_int32_t* fft_output, int32_t threshold);

/// Weighted average magnitude over the bins overlapping [lower_freq, upper_freq], each bin counted
/// by how much of it falls inside the band.
float calculateWeightedMagnitude(const FFTResult& fft_result, float lower_freq, float upper_freq,
                                 float freq_resolution);

} // namespace deluge::hid::display
