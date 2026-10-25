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

#include "visualizer_common.h"
#include "gui/ui/ui.h"
#include "gui/views/arranger_view.h"
#include "gui/views/session_view.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "hid/display/visualizer.h"
#include <algorithm>
#include <cmath>

namespace deluge::hid::display {

namespace {
/// A square root: 2:1 visual compression.
constexpr float kCompressionExponent = 0.5f;
/// Gentle treble tilt, the usual meter convention.
constexpr float kFrequencyBoostExponent = 0.1f;
constexpr float kFrequencyNormalizationHz = 1000.0f;
constexpr float kAmplitudeBoost = 1.0f;
} // namespace

uint32_t getVisualizerReadStartPos(uint32_t sample_count) {
	// Relaxed: see the note on the ring in visualizer.h.
	uint32_t write_pos = Visualizer::visualizer_write_pos.load(std::memory_order_relaxed);
	// Walk back from the write head to the start of the newest `sample_count` samples.
	return (write_pos - sample_count) & (Visualizer::kVisualizerBufferSize - 1);
}

float applyVisualizerCompression(float amplitude, float frequency) {
	amplitude = std::clamp(amplitude, 0.0f, 1.0f);
	return (std::pow(amplitude, kCompressionExponent)
	        * std::pow(frequency / kFrequencyNormalizationHz, kFrequencyBoostExponent))
	       * kAmplitudeBoost;
}

float computeCurrentAmplitude() {
	uint32_t sample_count = Visualizer::visualizer_sample_count.load(std::memory_order_relaxed);
	if (sample_count < 2) {
		return 0.0f;
	}
	uint32_t read_start_pos = getVisualizerReadStartPos(sample_count);
	float peak_amplitude = 0.0f;

	for (uint32_t i = 0; i < kAmplitudeSampleCount && i < sample_count; i++) {
		uint32_t buffer_index = (read_start_pos + i) & (Visualizer::kVisualizerBufferSize - 1);
		int32_t sample = Visualizer::visualizer_sample_buffer[buffer_index];
		peak_amplitude = std::max(peak_amplitude, static_cast<float>(std::abs(sample)));
	}

	return std::clamp(peak_amplitude / kReferenceAmplitude, 0.0f, 1.0f);
}

bool shouldHandleVisualizerModeButtons(View& view) {
	UI* current_ui = getCurrentUI();
	return (current_ui == &sessionView || current_ui == &arrangerView) && Visualizer::isActive(view.displayVUMeter)
	       && currentUIMode != UI_MODE_CLIP_PRESSED_IN_SONG_VIEW
	       && currentUIMode != UI_MODE_HOLDING_ARRANGEMENT_ROW_AUDITION;
}

namespace {
/// CV walks this ring of the eight modes that have no button of their own.
constexpr uint32_t kSpecialModes[] = {
    RuntimeFeatureStateVisualizer::VisualizerBarSpectrum,
    RuntimeFeatureStateVisualizer::VisualizerStereoLineSpectrum,
    RuntimeFeatureStateVisualizer::VisualizerStereoBarSpectrum,
    RuntimeFeatureStateVisualizer::VisualizerCube,
    RuntimeFeatureStateVisualizer::VisualizerSkyline,
    RuntimeFeatureStateVisualizer::VisualizerStarfield,
    RuntimeFeatureStateVisualizer::VisualizerTunnel,
    RuntimeFeatureStateVisualizer::VisualizerPulseGrid,
};
constexpr size_t kNumSpecialModes = sizeof(kSpecialModes) / sizeof(kSpecialModes[0]);

void switchTo(uint32_t mode) {
	Visualizer::setSessionMode(mode);
	::display->displayPopup(Visualizer::getModeDisplayName(mode).data());
}
} // namespace

ActionResult handleVisualizerModeButton(deluge::hid::Button button, View& view) {
	if (!shouldHandleVisualizerModeButtons(view)) {
		return ActionResult::NOT_DEALT_WITH;
	}

	switch (button) {
	case deluge::hid::button::SYNTH:
		switchTo(RuntimeFeatureStateVisualizer::VisualizerWaveform);
		return ActionResult::DEALT_WITH;

	case deluge::hid::button::KIT:
		switchTo(RuntimeFeatureStateVisualizer::VisualizerLineSpectrum);
		return ActionResult::DEALT_WITH;

	case deluge::hid::button::MIDI:
		switchTo(RuntimeFeatureStateVisualizer::VisualizerMidiPianoRoll);
		return ActionResult::DEALT_WITH;

	case deluge::hid::button::CV: {
		// On one of the eight, step to the next; on anything else, come back to the last one used.
		uint32_t current = Visualizer::getMode();
		uint32_t next = Visualizer::getCVVisualizerMode();
		for (size_t i = 0; i < kNumSpecialModes; i++) {
			if (kSpecialModes[i] == current) {
				next = kSpecialModes[(i + 1) % kNumSpecialModes];
				break;
			}
		}
		Visualizer::setCVVisualizerMode(next);
		switchTo(next);
		return ActionResult::DEALT_WITH;
	}

	default:
		return ActionResult::NOT_DEALT_WITH;
	}
}

} // namespace deluge::hid::display
