/*
 * Copyright (c) 2014-2023 Synthstrom Audible Limited
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
#include "definitions_cxx.hpp"
#include "gui/menu_item/formatted_title.h"
#include "gui/menu_item/source/patched_param.h"
#include "gui/ui/sound_editor.h"
#include "modulation/params/param_set.h"
#include "processing/sound/sound.h"

namespace deluge::gui::menu_item::osc {
class PulseWidth final : public menu_item::source::PatchedParam, public FormattedTitle {
public:
	PulseWidth(l10n::String name, l10n::String title_format_str, int32_t newP)
	    : source::PatchedParam(name, newP), FormattedTitle(title_format_str) {}

	/// When source 0 is Plaits this param is not a pulse width: it is HARMONICS
	/// on source 0 and MORPH on source 1 (Morph has to reach across to source B
	/// -- see the render block in voice.cpp). Relabel rather than leave the user
	/// reading "Pulse width" for something else entirely.
	[[nodiscard]] bool isPlaitsControl() const {
		return soundEditor.currentSound != nullptr && soundEditor.currentSound->sources[0].oscType == OscType::PLAITS;
	}

	[[nodiscard]] std::string_view getName() const override {
		if (isPlaitsControl()) {
			return l10n::getView(soundEditor.currentSourceIndex == 0 ? l10n::String::STRING_FOR_PLAITS_HARMONICS
			                                                         : l10n::String::STRING_FOR_PLAITS_MORPH);
		}
		return PatchedParam::getName();
	}

	[[nodiscard]] std::string_view getTitle() const override {
		if (isPlaitsControl()) {
			return getName();
		}
		return FormattedTitle::title();
	}

	int32_t getFinalValue() override { return computeFinalValueForHalfPrecisionMenuItem(this->getValue()); }

	void readCurrentValue() override {
		this->setValue(computeCurrentValueForHalfPrecisionMenuItem(
		    soundEditor.currentParamManager->getPatchedParamSet()->getValue(getP())));
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		Sound* sound = static_cast<Sound*>(modControllable);
		if (sound->getSynthMode() == SynthMode::FM) {
			return false;
		}
		OscType oscType = sound->sources[whichThing].oscType;
		return (oscType != OscType::SAMPLE && oscType != OscType::INPUT_L && oscType != OscType::INPUT_R
		        && oscType != OscType::INPUT_STEREO);
	}
};

} // namespace deluge::gui::menu_item::osc
