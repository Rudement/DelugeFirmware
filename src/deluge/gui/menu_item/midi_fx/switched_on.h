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
#pragma once

#include "model/settings/runtime_feature_settings.h"

namespace deluge::gui::menu_item::midi_fx {

/// Community Features > Rudement upgrades > MIDI FX.
///
/// Every "is this editable here" helper in the MIDI FX menus asks this first, so with the feature off
/// every item is irrelevant: the MIDI FX submenu drops out of the sound menu (Submenu::isRelevant is
/// "any child relevant"), and HorizontalMenu::switchHorizontalMenu skips the page group in the strip.
/// The pages are FIXED layout, which shows an irrelevant item as a dash rather than dropping it, so
/// the strip needs that explicit skip; the vertical menus need nothing more.
///
/// Presentation only, like rf::Gated: the effects keep running on a song that uses them.
[[nodiscard]] inline bool midiFxSwitchedOn() {
	return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::MidiFx);
}

} // namespace deluge::gui::menu_item::midi_fx
