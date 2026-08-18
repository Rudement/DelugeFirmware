/*
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

#pragma once

#include "model/settings/runtime_feature_settings.h"

namespace deluge::gui::menu_item::runtime_feature {

/// Wraps any MenuItem so that it disappears from its parent when a community feature is switched off.
///
/// This hides the door, not the room. The DSP behind the menu is untouched and still runs, so a song
/// saved using the feature plays back exactly the same with the menus hidden - which is the whole
/// reason to gate presentation rather than processing. EnableDX7Engine works the same way.
///
/// Gating a menu's *children* is usually enough to make its container vanish too:
/// HorizontalMenu::switchHorizontalMenu() already skips any menu whose paging reports no relevant
/// items. Containers are gated as well here so they also drop out of ordinary submenu listings.
template <typename Base, RuntimeFeatureSettingType kSetting>
class Gated : public Base {
public:
	using Base::Base;

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		if (!runtimeFeatureSettings.isOn(kSetting)) {
			return false;
		}
		return Base::isRelevant(modControllable, whichThing);
	}
};

} // namespace deluge::gui::menu_item::runtime_feature
