/*
 * Copyright © 2021-2023 Synthstrom Audible Limited
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

#include "gui/menu_item/submenu.h"

namespace deluge::gui::menu_item::runtime_feature {

/// Some runtime feature settings are squirrled away in submenus / used directly without a top-level menu item.
/// This MUST equal (number of RuntimeFeatureSettingType entries) minus (number of items in subMenuEntries below),
/// or the std::array ends up with trailing nullptr(s) and scrolling onto one freezes the device. Currently:
/// KeyboardNotePreview has a setting but no menu item, so 1.
constexpr size_t kNonTopLevelSettings = 1;
// RuntimeFeatureSettingType::MaxElement - kNonTopLevelSettings
class Settings final : public Submenu {
public:
	Settings(l10n::String name, l10n::String title);

private:
};

} // namespace deluge::gui::menu_item::runtime_feature
