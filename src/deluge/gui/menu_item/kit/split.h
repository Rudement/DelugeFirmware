/*
 * Copyright © 2026 Rudement
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
#include "gui/l10n/l10n.h"
#include "gui/menu_item/menu_item.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit_split.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "util/d_string.h"
#include <cstring>

namespace deluge::gui::menu_item::kit {

/// Performs the split. Lives one level down inside the "Split Kit" submenu, so that stepping into the submenu is
/// itself the confirmation - this is destructive and cannot be undone.
class SplitPerform final : public MenuItem {
public:
	using MenuItem::MenuItem;

	/// Says what it is about to do, e.g. "Split into 12", so the confirmation is not a bare yes/no.
	[[nodiscard]] std::string_view getName() const override {
		static char buffer[32];
		int32_t count = deluge::model::kit_split::countSplittableRows(getCurrentInstrumentClip());

		char number[12];
		intToString(count, number);

		// 7SEG draws this through setText(), which keeps only the first four characters - "Split into 12"
		// arrives as "SPLI" and the count is lost, which is the whole reason this label states a number
		// instead of being a bare yes/no. kMaxSplitKits is 64, so two digits always fit alongside "SP".
		if (display->have7SEG()) {
			strcpy(buffer, "SP");
		}
		else {
			strcpy(buffer, "Split into ");
		}
		strncat(buffer, number, sizeof(buffer) - strlen(buffer) - 1);
		return {buffer};
	}

	[[nodiscard]] std::string_view getTitle() const override { return getName(); }

	MenuItem* selectButtonPress() override {
		InstrumentClip* clip = getCurrentInstrumentClip();

		soundEditor.exitCompletely();

		int32_t created = deluge::model::kit_split::performSplit(clip);
		if (created > 0) {
			display->consoleText(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_DONE));
		}

		return NO_NAVIGATION;
	}

	bool shouldEnterSubmenu() override { return false; }
};

/// The "Split Kit" entry itself. Only shown on a Kit clip that actually has something to split.
class Split final : public Submenu {
public:
	using Submenu::Submenu;

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		if (!runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::KitSplit)) {
			return false;
		}
		if (getCurrentOutputType() != OutputType::KIT) {
			return false;
		}
		return deluge::model::kit_split::canSplit(getCurrentInstrumentClip());
	}
};

} // namespace deluge::gui::menu_item::kit
