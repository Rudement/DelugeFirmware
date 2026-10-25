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

// Shared helpers for the Visualizer modes. Trimmed from upstream: the clip-view visualizer, its
// popup rerouting and the per-clip silence timers are not in this fork's port, so the helpers that
// served them are not here either.

#pragma once

#include "NE10.h"
#include "definitions_cxx.hpp"
#include "hid/button.h"
#include "hid/display/oled_canvas/canvas.h"
#include "model/settings/runtime_feature_settings.h"

class View;

namespace deluge::hid::display {

/// One transform's worth of output. `output` is the mono result; the stereo modes use the other two.
struct FFTResult {
	ne10_fft_cpx_int32_t* output;
	ne10_fft_cpx_int32_t* output_left;
	ne10_fft_cpx_int32_t* output_right;
	bool isValid;
	bool isSilent;
	bool isStereo;

	FFTResult(ne10_fft_cpx_int32_t* output = nullptr, ne10_fft_cpx_int32_t* output_left = nullptr,
	          ne10_fft_cpx_int32_t* output_right = nullptr, bool isValid = false, bool isSilent = false,
	          bool isStereo = false)
	    : output(output), output_left(output_left), output_right(output_right), isValid(isValid), isSilent(isSilent),
	      isStereo(isStereo) {}
};

/// Standard inset for every mode's drawing area.
constexpr int32_t kDisplayMargin = 2;
/// Bin 0 is DC plus window leakage; pulling it down keeps a tiny offset from pegging the bottom band.
constexpr int32_t kDcBinReductionFactor = 4;

/// Q15 reference for a moderate level, used by the amplitude-driven modes.
constexpr float kReferenceAmplitude = 10000.0f;
/// How many of the newest samples the peak detector looks at.
constexpr uint32_t kAmplitudeSampleCount = 256;

/// Where in the ring the newest `sample_count` samples begin.
uint32_t getVisualizerReadStartPos(uint32_t sample_count);

/// Soft-knee compression plus a gentle treble tilt, so quiet material still moves the display.
float applyVisualizerCompression(float amplitude, float frequency);

/// Peak level of the newest samples, normalised to 0..1.
float computeCurrentAmplitude();

/// SYNTH / KIT / MIDI / CV switch modes while a visualizer is on screen. CV cycles the eight
/// "special" ones. These are session-only: they do not change the saved Community Features setting.
ActionResult handleVisualizerModeButton(deluge::hid::Button button, View& view);

/// Whether those buttons should be doing that right now.
bool shouldHandleVisualizerModeButtons(View& view);

} // namespace deluge::hid::display
