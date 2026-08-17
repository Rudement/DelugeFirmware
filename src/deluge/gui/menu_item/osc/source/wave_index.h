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
#include "gui/menu_item/formatted_title.h"
#include "gui/menu_item/source/patched_param.h"
#include "gui/ui/sound_editor.h"
#include "processing/sound/sound.h"

namespace deluge::gui::menu_item::osc::source {
class WaveIndex final : public menu_item::source::PatchedParam, public FormattedTitle {
public:
	WaveIndex(l10n::String name, l10n::String title_format_str, int32_t newP, uint8_t source_id)
	    : PatchedParam(name, newP, source_id), FormattedTitle(title_format_str, source_id + 1) {}

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

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		const auto sound = static_cast<Sound*>(modControllable);
		auto& source = sound->sources[source_id_];
		if (sound->getSynthMode() == SynthMode::FM) {
			return false;
		}
		// Plaits borrows this param for TIMBRE, so it must be reachable even
		// with no wavetable loaded. (Pulse width's gate already lets any
		// non-sample osc type through, so this is the only one that needed
		// opening up.)
		if (source.oscType == OscType::PLAITS) {
			return true;
		}
		return source.oscType == OscType::WAVETABLE && source.hasAtLeastOneAudioFileLoaded();
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return SLIDER; }
};
} // namespace deluge::gui::menu_item::osc::source
