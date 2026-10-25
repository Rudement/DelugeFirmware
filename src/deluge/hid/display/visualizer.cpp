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

#include "hid/display/visualizer.h"
#include "gui/ui/ui.h"
#include "gui/views/view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/display/visualizer/visualizer_bar_spectrum.h"
#include "hid/display/visualizer/visualizer_cube.h"
#include "hid/display/visualizer/visualizer_line_spectrum.h"
#include "hid/display/visualizer/visualizer_midi_piano_roll.h"
#include "hid/display/visualizer/visualizer_pulsegrid.h"
#include "hid/display/visualizer/visualizer_skyline.h"
#include "hid/display/visualizer/visualizer_starfield.h"
#include "hid/display/visualizer/visualizer_stereo_bar_spectrum.h"
#include "hid/display/visualizer/visualizer_stereo_line_spectrum.h"
#include "hid/display/visualizer/visualizer_tunnel.h"
#include "hid/display/visualizer/visualizer_waveform.h"
#include "model/settings/runtime_feature_settings.h"
#include "playback/mode/session.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include <cstdlib>

namespace deluge::hid::display {

// ---------------------------------------------------------------------------- mode

uint32_t Visualizer::getMode() {
	if (session_visualizer_mode != RuntimeFeatureStateVisualizer::VisualizerOff) {
		return session_visualizer_mode;
	}
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::Visualizer);
}

void Visualizer::setSessionMode(uint32_t mode) {
	session_visualizer_mode = mode;
	renderUIsForOled();
}

void Visualizer::clearSessionMode() {
	session_visualizer_mode = RuntimeFeatureStateVisualizer::VisualizerOff;
}

void Visualizer::noteOverlayText() {
	last_overlay_text_time = AudioEngine::audioSampleTimer;
}

void Visualizer::resetSessionMode() {
	session_visualizer_mode = RuntimeFeatureStateVisualizer::VisualizerOff;
	cv_visualizer_mode = RuntimeFeatureStateVisualizer::VisualizerBarSpectrum;
	visualizer_toggle_enabled = false;
}

uint32_t Visualizer::getCVVisualizerMode() {
	return cv_visualizer_mode;
}

void Visualizer::setCVVisualizerMode(uint32_t mode) {
	cv_visualizer_mode = mode;
}

std::string_view Visualizer::getModeDisplayName(uint32_t mode) {
	switch (mode) {
	case RuntimeFeatureStateVisualizer::VisualizerWaveform:
		return "WAVEFORM";
	case RuntimeFeatureStateVisualizer::VisualizerLineSpectrum:
		return "LINE SPECTRUM";
	case RuntimeFeatureStateVisualizer::VisualizerBarSpectrum:
		return "GRAPHIC EQ";
	case RuntimeFeatureStateVisualizer::VisualizerCube:
		return "CUBE";
	case RuntimeFeatureStateVisualizer::VisualizerStereoLineSpectrum:
		return "STEREO SPECTRUM";
	case RuntimeFeatureStateVisualizer::VisualizerStereoBarSpectrum:
		return "STEREO EQ";
	case RuntimeFeatureStateVisualizer::VisualizerTunnel:
		return "TUNNEL";
	case RuntimeFeatureStateVisualizer::VisualizerStarfield:
		return "STARFIELD";
	case RuntimeFeatureStateVisualizer::VisualizerSkyline:
		return "SKYLINE";
	case RuntimeFeatureStateVisualizer::VisualizerPulseGrid:
		return "PULSE GRID";
	case RuntimeFeatureStateVisualizer::VisualizerMidiPianoRoll:
		return "PIANO ROLL";
	default:
		return "OFF";
	}
}

bool Visualizer::modeNeedsStereo(uint32_t mode) {
	return mode == RuntimeFeatureStateVisualizer::VisualizerStereoLineSpectrum
	       || mode == RuntimeFeatureStateVisualizer::VisualizerStereoBarSpectrum;
}

bool Visualizer::modeNeedsAudio(uint32_t mode) {
	// The piano roll is fed by MIDI, so with it selected the audio thread does nothing at all.
	return mode != RuntimeFeatureStateVisualizer::VisualizerOff
	       && mode != RuntimeFeatureStateVisualizer::VisualizerMidiPianoRoll;
}

// ---------------------------------------------------------------------------- state

bool Visualizer::hasPriority() {
	return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::VisualizerPriority);
}

bool Visualizer::isEnabled() {
	return getMode() != RuntimeFeatureStateVisualizer::VisualizerOff;
}

bool Visualizer::isToggleEnabled() {
	return visualizer_toggle_enabled;
}

void Visualizer::setToggleEnabled(bool enabled) {
	visualizer_toggle_enabled = enabled;
}

bool Visualizer::isActive(bool displayVUMeter) {
	return isEnabled() && (displayVUMeter || visualizer_toggle_enabled);
}

bool Visualizer::isActive(View& view_in) {
	return isActive(view_in.displayVUMeter);
}

bool Visualizer::isSilent() {
	uint32_t since = (getMode() == RuntimeFeatureStateVisualizer::VisualizerMidiPianoRoll)
	                     ? (AudioEngine::audioSampleTimer - midi_piano_roll_last_note_time)
	                     : (AudioEngine::audioSampleTimer - last_audio_time);
	return since > kSilenceTimeoutSamples;
}

bool Visualizer::isDisplaying() {
	if (!isActive(view.displayVUMeter)) {
		return false;
	}
	// Song-level screens only, the same ones that can show a VU meter.
	if (rootUIIsClipMinderScreen()) {
		return false;
	}
	// With Visualizer Priority on, none of the yielding below applies: the display holds the screen
	// and the interruptions are the ones that give way. They are not drawn at all rather than drawn
	// underneath - see OLED::popupText and OLED::drawPermanentPopupLookingText. Everything still
	// happens, you just do not read about it, which is the point when you are watching a sweep
	// rather than reading a number off it.
	if (!hasPriority()) {
		// Anything popup-shaped takes the screen. Without this the display and the message each
		// redraw over the other and the whole thing flashes - which is what arming a clip used to
		// look like, because the launch countdown is written straight into the main image rather
		// than into the popup layer.
		if (OLED::isPopupPresent()) {
			return false;
		}
		// The launch countdown, for as long as a launch is actually pending. A timer is not enough
		// here: the countdown only redraws when the number changes, so the display would reclaim
		// the screen in between and flash once a bar instead of once a frame.
		if (playbackHandler.isEitherClockActive() && currentPlaybackMode == &session
		    && session.launchEventAtSwungTickCount != 0u) {
			return false;
		}
		// Everything else that draws popup-looking text straight into the main image - the
		// resampling notice, stem export progress, the automation overview - gets a short hold.
		if (static_cast<uint32_t>(AudioEngine::audioSampleTimer - last_overlay_text_time) < kOverlayHoldSamples) {
			return false;
		}
	}
	return !isSilent();
}

void Visualizer::reset() {
	visualizer_frame_counter = 0;
	resetBarSpectrumState();
	// Start the clocks now, so arriving on a screen does not read as a second of silence.
	last_audio_time = AudioEngine::audioSampleTimer;
	midi_piano_roll_last_note_time = AudioEngine::audioSampleTimer;
}

// ---------------------------------------------------------------------------- buttons

bool Visualizer::handleModButtonToggle(View& view_in) {
	if (!Buttons::isShiftButtonPressed()) {
		return false;
	}
	if (!isEnabled()) {
		return false;
	}
	if (rootUIIsClipMinderScreen()) {
		return false;
	}

	visualizer_toggle_enabled = !visualizer_toggle_enabled;
	if (visualizer_toggle_enabled) {
		last_audio_time = AudioEngine::audioSampleTimer;
		midi_piano_roll_last_note_time = AudioEngine::audioSampleTimer;
	}
	else {
		// Put the normal display back now rather than at the next event.
		renderUIsForOled();
	}
	// Global scope: plain `display` would resolve to this namespace, not the display pointer.
	::display->displayPopup(visualizer_toggle_enabled ? "VISUALIZER ON" : "VISUALIZER OFF");
	return true;
}

// ---------------------------------------------------------------------------- capture

void Visualizer::updateSilenceTimer(std::span<StereoSample> renderingBuffer, size_t numSamples) {
	// Breaks on the first sample above the threshold, so on anything audible this is one iteration.
	for (size_t i = 0; i < numSamples; i += kVisualizerSampleInterval) {
		if (std::abs(renderingBuffer[i].l) > kSilenceThreshold || std::abs(renderingBuffer[i].r) > kSilenceThreshold) {
			last_audio_time = AudioEngine::audioSampleTimer;
			return;
		}
	}
}

void Visualizer::sampleAudioForDisplay(std::span<StereoSample> renderingBuffer, size_t numSamples) {
	uint32_t mode = getMode();
	if (!modeNeedsAudio(mode)) {
		// Off, or the piano roll, which reads MIDI instead.
		if (mode != RuntimeFeatureStateVisualizer::VisualizerOff) {
			requestVisualizerUpdateIfNeeded();
		}
		return;
	}
	if (!view.displayVUMeter && !visualizer_toggle_enabled) {
		return;
	}

	updateSilenceTimer(renderingBuffer, numSamples);

	uint32_t pos = visualizer_write_pos.load(std::memory_order_relaxed);
	uint32_t count = visualizer_sample_count.load(std::memory_order_relaxed);
	const bool stereo = modeNeedsStereo(mode);

	for (size_t i = 0; i < numSamples; i += kVisualizerSampleInterval) {
		int32_t left = renderingBuffer[i].l >> kQ31ToQ15Shift;
		int32_t right = renderingBuffer[i].r >> kQ31ToQ15Shift;
		visualizer_sample_buffer[pos] = (left + right) >> 1;
		if (stereo) {
			// The two stereo modes are the only ones that read these, so the other nine do not pay
			// for them.
			visualizer_sample_buffer_left[pos] = left;
			visualizer_sample_buffer_right[pos] = right;
		}
		pos = (pos + 1) & (kVisualizerBufferSize - 1);
		if (count < kVisualizerBufferSize) {
			count++;
		}
	}

	// One store each per block rather than per sample.
	visualizer_write_pos.store(pos, std::memory_order_relaxed);
	visualizer_sample_count.store(count, std::memory_order_relaxed);

	requestVisualizerUpdateIfNeeded();
}

void Visualizer::requestVisualizerUpdateIfNeeded() {
	if (!isDisplaying()) {
		return;
	}
	if (++visualizer_frame_counter < kFrameSkip) {
		return;
	}
	visualizer_frame_counter = 0;
	// Sets a dirty flag only. The oledRoutine task does the work, at idle priority, no faster than
	// 100 fps, and it cannot preempt the audio routine.
	renderUIsForOled();
}

// ---------------------------------------------------------------------------- render

bool Visualizer::potentiallyRenderVisualizer(oled_canvas::Canvas& canvas) {
	if (!isDisplaying()) {
		return false;
	}

	switch (getMode()) {
	case RuntimeFeatureStateVisualizer::VisualizerWaveform:
		renderVisualizerWaveform(canvas);
		break;
	case RuntimeFeatureStateVisualizer::VisualizerLineSpectrum:
		renderVisualizerLineSpectrum(canvas);
		break;
	case RuntimeFeatureStateVisualizer::VisualizerBarSpectrum:
		renderVisualizerBarSpectrum(canvas);
		break;
	case RuntimeFeatureStateVisualizer::VisualizerCube:
		renderVisualizerCube(canvas);
		break;
	case RuntimeFeatureStateVisualizer::VisualizerStereoLineSpectrum:
		renderVisualizerStereoLineSpectrum(canvas);
		break;
	case RuntimeFeatureStateVisualizer::VisualizerStereoBarSpectrum:
		renderVisualizerStereoBarSpectrum(canvas);
		break;
	case RuntimeFeatureStateVisualizer::VisualizerTunnel:
		renderVisualizerTunnel(canvas);
		break;
	case RuntimeFeatureStateVisualizer::VisualizerStarfield:
		renderVisualizerStarfield(canvas);
		break;
	case RuntimeFeatureStateVisualizer::VisualizerSkyline:
		renderVisualizerSkyline(canvas);
		break;
	case RuntimeFeatureStateVisualizer::VisualizerPulseGrid:
		renderVisualizerPulseGrid(canvas);
		break;
	case RuntimeFeatureStateVisualizer::VisualizerMidiPianoRoll:
		renderVisualizerMidiPianoRoll(canvas);
		break;
	default:
		return false;
	}

	return true;
}

} // namespace deluge::hid::display
