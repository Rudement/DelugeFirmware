/*
 * Copyright © 2026 Synthstrom Audible Limited
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

#include "engine_select.h"
#include "definitions_cxx.hpp"
#include "gui/menu_item/value_scaling.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "modulation/params/param_set.h"
#include "processing/source.h"

#include <algorithm>
#include "util/container/static_vector.hpp"

namespace deluge::gui::menu_item {

PlaitsEngineSelect plaitsEngineSelect{l10n::String::STRING_FOR_PLAITS_ENGINE};
PlaitsAux plaitsAuxToggle{l10n::String::STRING_FOR_PLAITS_AUX};
PlaitsLpg plaitsLpgToggle{l10n::String::STRING_FOR_PLAITS_LPG};
PlaitsDecay plaitsDecayMenu{l10n::String::STRING_FOR_PLAITS_DECAY};
PlaitsLpgColour plaitsLpgColourMenu{l10n::String::STRING_FOR_PLAITS_LPG_COLOUR};

namespace params = deluge::modulation::params;

PlaitsHalfPrecisionParam plaitsHarmonicsMenu{l10n::String::STRING_FOR_PLAITS_HARMONICS,
                                             params::LOCAL_OSC_A_PHASE_WIDTH};
PlaitsHalfPrecisionParam plaitsTimbreMenu{l10n::String::STRING_FOR_PLAITS_TIMBRE, params::LOCAL_OSC_A_WAVE_INDEX};
PlaitsHalfPrecisionParam plaitsMorphMenu{l10n::String::STRING_FOR_PLAITS_MORPH, params::LOCAL_OSC_B_PHASE_WIDTH};

int32_t PlaitsHalfPrecisionParam::getFinalValue() {
	return computeFinalValueForHalfPrecisionMenuItem(this->getValue());
}

void PlaitsHalfPrecisionParam::readCurrentValue() {
	// Clamped because Timbre's param is centred: a sound saved before Timbre
	// moved to half-precision scaling can hold a negative value, which reads
	// back as a negative knob position. Those sounds were already reaching the
	// engine as zero, so pinning them to 0 shows what is actually being heard.
	int32_t stored = soundEditor.currentParamManager->getPatchedParamSet()->getValue(getP());
	int32_t knob = computeCurrentValueForHalfPrecisionMenuItem(stored);
	this->setValue(std::clamp<int32_t>(knob, kMinMenuValue, kMaxMenuValue));
}

void PlaitsAux::readCurrentValue() {
	this->setValue(soundEditor.currentSource->plaitsAux);
}

void PlaitsLpg::readCurrentValue() {
	this->setValue(soundEditor.currentSource->plaitsLpg);
}

void PlaitsLpg::writeCurrentValue() {
	// Same no-killAllVoices reasoning as the Aux toggle: PlaitsVoice re-reads
	// this every block. A held note will not re-ping the gate -- the trigger is
	// already high, so there is no rising edge -- but the next note will. That
	// is the same seam the module has and is what you want while auditioning.
	soundEditor.currentSource->plaitsLpg = this->getValue();
}

void PlaitsDecay::readCurrentValue() {
	this->setValue(soundEditor.currentSource->plaitsDecay);
}

void PlaitsDecay::writeCurrentValue() {
	soundEditor.currentSource->plaitsDecay = static_cast<uint8_t>(this->getValue());
}

void PlaitsLpgColour::readCurrentValue() {
	this->setValue(soundEditor.currentSource->plaitsLpgColour);
}

void PlaitsLpgColour::writeCurrentValue() {
	soundEditor.currentSource->plaitsLpgColour = static_cast<uint8_t>(this->getValue());
}

void PlaitsAux::writeCurrentValue() {
	// No killAllVoices(): PlaitsVoice re-reads this every render block, so a
	// held note swaps output rather than being cut.
	soundEditor.currentSource->plaitsAux = this->getValue();
}

namespace {

/// Upstream registration order, from plaits/dsp/voice.cc. Indices 0-7 are the
/// models added in Plaits firmware 1.2; 8-23 are the original sixteen, which is
/// why the "classic" Virtual Analog sits at 8 and is the default.
///
/// DO NOT REORDER. The index is what Source::plaitsEngine stores and what the
/// preset file records, so shuffling this list silently repatches every saved
/// sound. If a friendlier order is wanted, sort the *display* and map back.
const char* const kEngineNames[kPlaitsNumEngines] = {
    "Virtual Analog VCF", // 0
    "Phase Distortion",   // 1
    "6-op FM A",          // 2
    "6-op FM B",          // 3
    "6-op FM C",          // 4
    "Wave Terrain",       // 5
    "String Machine",     // 6
    "Chiptune",           // 7
    "Virtual Analog",     // 8  <- default
    "Waveshaping",        // 9
    "2-op FM",            // 10
    "Granular Formant",   // 11
    "Harmonic",           // 12
    "Wavetable",          // 13
    "Chords",             // 14
    "Speech",             // 15
    "Granular Cloud",     // 16
    "Filtered Noise",     // 17
    "Particle Noise",     // 18
    "Inharmonic String",  // 19
    "Modal Resonator",    // 20
    "Bass Drum",          // 21
    "Snare Drum",         // 22
    "Hi-Hat",             // 23
};

/// Four characters, because that is all a 7-segment Deluge has.
const char* const kEngineNames7Seg[kPlaitsNumEngines] = {
    "VAVC", "PHDS", "FMA",  "FMB",  "FMC",  "TERR", "STRM", "CHIP", //
    "VANA", "WSHP", "FM2",  "FORM", "HARM", "WTBL", "CHRD", "SPCH", //
    "CLUD", "NOIS", "PART", "STRG", "MODL", "BASS", "SNAR", "HHAT",
};

} // namespace

void PlaitsEngineSelect::beginSession(MenuItem* navigatedBackwardFrom) {
	readValueAgain();
}

void PlaitsEngineSelect::readValueAgain() {
	currentValue = soundEditor.currentSource->plaitsEngine;
	if (currentValue < 0 || currentValue >= kPlaitsNumEngines) {
		currentValue = kPlaitsDefaultEngine;
	}
	// Open with the current model on screen. Without this, selecting a model
	// near the end of the list and coming back shows the top of the list with
	// nothing highlighted.
	scrollPos = std::clamp<int32_t>(currentValue - 1, 0, kPlaitsNumEngines - kOLEDMenuNumOptionsVisible);
	drawValue();
}

void PlaitsEngineSelect::drawPixelsForOled() {
	static_vector<std::string_view, kPlaitsNumEngines> itemNames;
	for (int32_t i = 0; i < kPlaitsNumEngines; i++) {
		itemNames.push_back(kEngineNames[i]);
	}
	drawItemsForOled(itemNames, currentValue - scrollPos, scrollPos);
}

void PlaitsEngineSelect::drawValue() {
	if (display->haveOLED()) {
		renderUIsForOled();
	}
	else {
		display->setScrollingText(kEngineNames7Seg[currentValue]);
	}
}

void PlaitsEngineSelect::selectEncoderAction(int32_t offset) {
	int32_t newValue = currentValue + offset;

	// OLED shows a scrolling list, so clamping at the ends reads naturally.
	// 7SEG shows one value at a time, so wrapping is the only sane behaviour --
	// same split DxEngineSelect uses.
	if (display->haveOLED()) {
		if (newValue >= kPlaitsNumEngines || newValue < 0) {
			return;
		}
	}
	else {
		newValue = ((newValue % kPlaitsNumEngines) + kPlaitsNumEngines) % kPlaitsNumEngines;
	}

	currentValue = newValue;
	if (display->haveOLED()) {
		// Keep the selection on screen, one row down where there is room, which
		// is the same feel as the DX7 cartridge and operator lists.
		scrollPos = std::clamp<int32_t>(newValue - 1, 0, kPlaitsNumEngines - kOLEDMenuNumOptionsVisible);
	}
	soundEditor.currentSource->plaitsEngine = static_cast<uint8_t>(newValue);

	// No killAllVoices() here: PlaitsVoice re-reads engineIndex every render
	// block, so a live note switches model rather than being cut. Changing
	// model on the module is not click-free either, so a seam is expected --
	// but a dropped note would not be.
	drawValue();
}

} // namespace deluge::gui::menu_item
