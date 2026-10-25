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

// The Visualizer: eleven ways of drawing what is coming out of the machine, on the OLED.
// Ported from Bruce Zawadzki's Visualizer (SynthstromAudible/DelugeFirmware PR #4221). The ten
// mode renderers are his, essentially unchanged; this layer and the FFT are re-homed on the
// Rudement 1.3 line and differ from upstream in three deliberate ways, noted below and in
// GRAPHIC-EQ-FEASIBILITY.md.
//
// It is a meter. No audio is changed, nothing is written into a song, and nothing is saved beyond
// the Community Features choice of mode.
//
// Upstream's clip-view visualizer and its popup rerouting are NOT ported.

#pragma once

#include "definitions_cxx.hpp"
#include "dsp/stereo_sample.h"
#include "hid/display/oled_canvas/canvas.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

class View;

namespace deluge::hid::display {

class Visualizer {
public:
	// ---------------------------------------------------------------- audio thread
	/// Once per render block, from AudioEngine::renderAudio. Early-outs when no mode is selected or
	/// nothing is on screen, so switched off this costs one compare per block.
	static void sampleAudioForDisplay(std::span<StereoSample> renderingBuffer, size_t numSamples);

	// ---------------------------------------------------------------- idle thread
	/// From SessionView::renderOLED (ArrangerView and PerformanceView come through there too).
	/// Draws the current mode and returns true when it has taken the screen.
	static bool potentiallyRenderVisualizer(oled_canvas::Canvas& canvas);

	/// SHIFT + LEVEL: show the visualizer without arming the VU meter. True when it took the press.
	static bool handleModButtonToggle(View& view);

	/// Called when the UI changes, so a mode does not survive into a screen that should not have it.
	static void reset();
	/// Called when a song is loaded: drops any session-only mode override.
	static void resetSessionMode();

	/// Called by OLED::drawPermanentPopupLookingText. Anything popup-shaped owns the screen for a
	/// moment afterwards: see isDisplaying().
	static void noteOverlayText();

	// ---------------------------------------------------------------- state
	/// Whether a mode is selected at all (the Community Features setting is not Off).
	static bool isEnabled();
	/// Visualizer Priority: the display keeps the screen and the interruptions give way, rather
	/// than the other way round. Off by default.
	static bool hasPriority();

	/// Enabled, and either the VU meter is up or SHIFT + LEVEL has been used.
	static bool isActive(bool displayVUMeter);
	static bool isActive(View& view);
	/// Active, on a screen that can show it, and not timed out on silence.
	static bool isDisplaying();

	/// The mode in force: the session override if one is set, otherwise the saved setting.
	static uint32_t getMode();
	/// A session-only override, set by the SYNTH / KIT / MIDI / CV hotkeys. Not saved.
	static void setSessionMode(uint32_t mode);
	static void clearSessionMode();
	/// Which of the eight CV modes to come back to.
	static uint32_t getCVVisualizerMode();
	static void setCVVisualizerMode(uint32_t mode);
	static std::string_view getModeDisplayName(uint32_t mode);

	static bool isToggleEnabled();
	static void setToggleEnabled(bool enabled);

	// ---------------------------------------------------------------- the ring
	/// Recent samples, written by the audio thread and read by the renderers.
	///
	/// RELAXED on purpose. One producer, one consumer, the consumer only reads, and the worst a torn
	/// read can do is put one bar at the wrong height for one frame at 100 fps. An acquire/release
	/// pair here would be two memory barriers per captured sample on the audio deadline, which is
	/// two thirds of this feature's whole audio-path cost.
	static constexpr size_t kVisualizerBufferSize = 512;
	inline static std::array<int32_t, kVisualizerBufferSize> visualizer_sample_buffer{};
	/// Only written while one of the two stereo modes is on screen.
	inline static std::array<int32_t, kVisualizerBufferSize> visualizer_sample_buffer_left{};
	inline static std::array<int32_t, kVisualizerBufferSize> visualizer_sample_buffer_right{};
	inline static std::atomic<uint32_t> visualizer_write_pos{0};
	inline static std::atomic<uint32_t> visualizer_sample_count{0};

	/// Every sample is captured, unlike upstream which took every second one. Decimating by two
	/// halves the buffer's effective rate while the band maths still assumes the full rate, which
	/// puts every band an octave out and makes the top bands aliasing.
	static constexpr uint32_t kVisualizerSampleInterval = 1;
	static constexpr int32_t kQ31ToQ15Shift = 16;

	/// The MIDI piano roll hides on no notes rather than on no audio.
	inline static uint32_t midi_piano_roll_last_note_time = 0;

	/// How long a piece of popup-looking text keeps the screen after it is drawn. Long enough to
	/// bridge the gap between two redraws of the same message, short enough that the display comes
	/// back promptly once the text stops.
	static constexpr uint32_t kOverlayHoldSamples = kSampleRate / 2;

	/// Hide after a second of nothing.
	static constexpr uint32_t kSilenceTimeoutSamples = kSampleRate;
	/// ~0.0005 full scale: above the noise floor, below anything audible.
	// The mix here is 8 doublings below the outputs, so 1 << 20 was about -18 dBFS at the jacks and a quiet
	// passage hid the display. 1 << 13 is about -60 dBFS: only real silence counts.
	static constexpr int32_t kSilenceThreshold = 1 << 13;

private:
	static void requestVisualizerUpdateIfNeeded();
	static void updateSilenceTimer(std::span<StereoSample> renderingBuffer, size_t numSamples);
	static bool isSilent();
	/// True for the two modes that need separate left and right rings.
	static bool modeNeedsStereo(uint32_t mode);
	/// True for the one mode that does not read audio at all.
	static bool modeNeedsAudio(uint32_t mode);

	inline static bool visualizer_toggle_enabled = false;
	inline static uint32_t session_visualizer_mode = 0; // RuntimeFeatureStateVisualizer::VisualizerOff
	inline static uint32_t cv_visualizer_mode = 3;      // VisualizerBarSpectrum, the default special one
	inline static uint32_t visualizer_frame_counter = 0;
	static constexpr uint32_t kFrameSkip = 2;
	inline static uint32_t last_audio_time = 0;
	inline static uint32_t last_overlay_text_time = 0;
};

} // namespace deluge::hid::display
