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
#include "gui/menu_item/value_scaling.h"
#include "gui/ui/sound_editor.h"
#include "modulation/params/param_set.h"
#include "processing/sound/sound.h"

#include <algorithm>

namespace deluge::gui::menu_item::osc::source {
class WaveIndex final : public menu_item::source::PatchedParam, public FormattedTitle {
public:
	WaveIndex(l10n::String name, l10n::String title_format_str, int32_t newP)
	    : PatchedParam(name, newP), FormattedTitle(title_format_str) {}

	/// Plaits borrows this param for TIMBRE.
	[[nodiscard]] bool isPlaitsControl() const {
		return soundEditor.currentSound != nullptr
		       && soundEditor.currentSound->sources[soundEditor.currentSourceIndex].oscType == OscType::PLAITS;
	}

	[[nodiscard]] std::string_view getName() const override {
		if (isPlaitsControl()) {
			return l10n::getView(l10n::String::STRING_FOR_PLAITS_TIMBRE);
		}
		return PatchedParam::getName();
	}

	[[nodiscard]] std::string_view getTitle() const override {
		if (isPlaitsControl()) {
			return getName();
		}
		return FormattedTitle::title();
	}

	/// Plaits' TIMBRE runs 0.0 .. 1.0 and the adapter clamps negatives, but this
	/// param's standard menu scaling is CENTRED -- knob 25 is patched zero. Left
	/// alone, the bottom half of the knob all arrived at the engine as zero.
	/// Under PLAITS we therefore scale like pulse width, 0..50 -> 0..INT32_MAX,
	/// matching the Timbre entry in the PLTS menu so both doors agree. The
	/// stored value and the engine mapping are unchanged, so saved sounds still
	/// play back identically -- only the displayed number moves.
	int32_t getFinalValue() override {
		if (isPlaitsControl()) {
			return computeFinalValueForHalfPrecisionMenuItem(this->getValue());
		}
		return PatchedParam::getFinalValue();
	}

	void readCurrentValue() override {
		if (isPlaitsControl()) {
			// Clamped: a sound saved before this change can hold a negative
			// value, which was already reaching the engine as zero.
			int32_t stored = soundEditor.currentParamManager->getPatchedParamSet()->getValue(getP());
			int32_t knob = computeCurrentValueForHalfPrecisionMenuItem(stored);
			this->setValue(std::clamp<int32_t>(knob, kMinMenuValue, kMaxMenuValue));
			return;
		}
		PatchedParam::readCurrentValue();
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		Sound* sound = static_cast<Sound*>(modControllable);
		Source* source = &sound->sources[whichThing];
		if (sound->getSynthMode() == SynthMode::FM) {
			return false;
		}
		// Plaits borrows this param for TIMBRE, so it must be reachable even
		// with no wavetable loaded. (Pulse width's gate already lets any
		// non-sample osc type through, so this is the only one that needed
		// opening up.)
		if (source->oscType == OscType::PLAITS) {
			return true;
		}
		return source->oscType == OscType::WAVETABLE;
	}
};
} // namespace deluge::gui::menu_item::osc::source
