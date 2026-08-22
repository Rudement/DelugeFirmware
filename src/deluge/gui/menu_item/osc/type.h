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
#include "gui/menu_item/selection.h"
#include "gui/menu_item/submenu.h"
#include "gui/ui/sound_editor.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound.h"
#include "model/settings/runtime_feature_settings.h"
#include "processing/source.h"
#include "util/comparison.h"

#include <algorithm>

extern deluge::gui::menu_item::Submenu dxMenu;
extern deluge::gui::menu_item::Submenu plaitsMenu;

namespace deluge::gui::menu_item::osc {
class Type final : public Selection, public FormattedTitle {
public:
	Type(l10n::String name, l10n::String title_format_str) : Selection(name), FormattedTitle(title_format_str) {};
	void beginSession(MenuItem* navigatedBackwardFrom) override { Selection::beginSession(navigatedBackwardFrom); }

	/// DX7 and Plaits are both source-0-only and unavailable in kits.
	bool mayUseSourceZeroOnlyTypes() { return !soundEditor.editingKit() && soundEditor.currentSourceIndex == 0; }
	bool mayUseDx() { return mayUseSourceZeroOnlyTypes(); }
	bool mayUsePlaits() {
		return mayUseSourceZeroOnlyTypes()
		       && runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::EnablePlaitsEngine);
	}

	/// The OscTypes currently on offer, in the order getOptions() lists them.
	///
	/// This used to be a fixed +/-2 index shift that assumed DX7 and PLAITS were hidden together, with
	/// a comment warning you not to add a third source-0-only type beside them. Plaits can now be
	/// switched off on its own from Community Features, so the mapping is looked up instead of
	/// computed. Add a type in both this list and getOptions(), in the same position, and nothing
	/// else needs touching.
	deluge::vector<OscType> offeredTypes() {
		deluge::vector<OscType> types = {OscType::SINE,         OscType::TRIANGLE, OscType::SQUARE,
		                                 OscType::ANALOG_SQUARE, OscType::SAW,     OscType::ANALOG_SAW_2,
		                                 OscType::WAVETABLE};
		if (soundEditor.currentSound->getSynthMode() == SynthMode::RINGMOD) {
			return types;
		}
		types.emplace_back(OscType::SAMPLE);
		if (mayUseDx()) {
			types.emplace_back(OscType::DX7);
		}
		if (mayUsePlaits()) {
			types.emplace_back(OscType::PLAITS);
		}
		if (AudioEngine::micPluggedIn || AudioEngine::lineInPluggedIn) {
			types.emplace_back(OscType::INPUT_L);
			types.emplace_back(OscType::INPUT_R);
			types.emplace_back(OscType::INPUT_STEREO);
		}
		else {
			// One collapsed "INPUT" entry, and it selects INPUT_L. That is what the index arithmetic
			// this replaced did -- the entry sat at INPUT_L's position in the enum -- so the behaviour
			// is preserved rather than quietly changed to stereo.
			types.emplace_back(OscType::INPUT_L);
		}
		return types;
	}

	void readCurrentValue() override {
		const OscType current = soundEditor.currentSource->oscType;
		const auto types = offeredTypes();
		const auto it = std::find(types.begin(), types.end(), current);
		// A type that is no longer offered -- Plaits switched off under a song already using it -- has
		// no row to sit on. Show the first entry rather than an out-of-range index; the sound itself is
		// untouched unless the user actually picks something.
		this->setValue(it == types.end() ? 0 : (int32_t)std::distance(types.begin(), it));
	}
	void writeCurrentValue() override {

		OscType oldValue = soundEditor.currentSource->oscType;
		const auto types = offeredTypes();
		const int32_t index = this->getValue();
		if (index < 0 || index >= (int32_t)types.size()) {
			return;
		}
		OscType newValue = types[index];

		auto needs_unassignment = {
		    OscType::INPUT_L,
		    OscType::INPUT_R,
		    OscType::INPUT_STEREO,
		    OscType::SAMPLE,
		    OscType::DX7,
		    OscType::PLAITS,

		    // Haven't actually really determined if this needs to be here - maybe not?
		    OscType::WAVETABLE,
		};

		if (util::one_of(oldValue, needs_unassignment) || util::one_of(newValue, needs_unassignment)) {
			soundEditor.currentSound->unassignAllVoices();
		}

		soundEditor.currentSource->setOscType(newValue);

		if (oldValue == OscType::SQUARE || newValue == OscType::SQUARE) {
			soundEditor.currentSound->setupPatchingForAllParamManagers(currentSong);
		}
	}

	[[nodiscard]] std::string_view getTitle() const override { return FormattedTitle::title(); }

	deluge::vector<std::string_view> getOptions() override {
		using enum l10n::String;
		deluge::vector<std::string_view> options = {
		    l10n::getView(STRING_FOR_SINE),          //<
		    l10n::getView(STRING_FOR_TRIANGLE),      //<
		    l10n::getView(STRING_FOR_SQUARE),        //<
		    l10n::getView(STRING_FOR_ANALOG_SQUARE), //<
		    l10n::getView(STRING_FOR_SAW),           //<
		    l10n::getView(STRING_FOR_ANALOG_SAW),    //<
		    l10n::getView(STRING_FOR_WAVETABLE),     //<
		};

		if (soundEditor.currentSound->getSynthMode() == SynthMode::RINGMOD) {
			return options;
		}

		options.emplace_back(l10n::getView(STRING_FOR_SAMPLE));

		if (mayUseDx()) {
			options.emplace_back(l10n::getView(STRING_FOR_DX7));
		}

		if (mayUsePlaits()) {
			options.emplace_back(l10n::getView(STRING_FOR_PLAITS));
		}

		if (AudioEngine::micPluggedIn || AudioEngine::lineInPluggedIn) {
			options.emplace_back(l10n::getView(STRING_FOR_INPUT_LEFT));
			options.emplace_back(l10n::getView(STRING_FOR_INPUT_RIGHT));
			options.emplace_back(l10n::getView(STRING_FOR_INPUT_STEREO));
		}
		else {
			options.emplace_back(l10n::getView(STRING_FOR_INPUT));
		}

		return options;
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		Sound* sound = static_cast<Sound*>(modControllable);
		return (sound->getSynthMode() != SynthMode::FM);
	}

	MenuItem* selectButtonPress() final {
		const OscType oscType = soundEditor.currentSource->oscType;
		if (oscType == OscType::DX7) {
			return (MenuItem*)&dxMenu;
		}
		if (oscType == OscType::PLAITS) {
			return (MenuItem*)&plaitsMenu;
		}
		return NULL;
	}
};

} // namespace deluge::gui::menu_item::osc
