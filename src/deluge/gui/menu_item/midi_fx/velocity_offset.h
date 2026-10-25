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
#include "gui/menu_item/integer.h"
#include "gui/menu_item/midi_fx/target.h"
#include "gui/ui/sound_editor.h"

/// NOTE: there is also a `deluge::modulation::midi_fx` (the effects themselves). Any file with
/// using-directives for both `deluge::modulation` and `deluge::gui::menu_item` - `menus.cpp` is
/// one - must qualify as `menu_item::midi_fx::` or the name is ambiguous.
namespace deluge::gui::menu_item::midi_fx {

/// Velocity offset, −64..+63, added to the note's original velocity.
///
/// Signed on purpose: an offset that can only add is half a control, and the RS7000's velocity
/// knob cut as well as boosted. The effect clamps the result to 1..127 — see VelocityFx, which
/// explains why the floor is 1 rather than 0.
class VelocityOffset final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		this->setValue(midiFxSettingsForRead() != nullptr ? midiFxSettingsForRead()->velocityOffset : 0);
	}

	void writeCurrentValue() override {
		midiFxApply([&](deluge::modulation::midi_fx::MidiFxSettings& s) {
			s.velocityOffset = static_cast<int8_t>(this->getValue());
		});
	}

	[[nodiscard]] int32_t getMinValue() const override { return -64; }
	[[nodiscard]] int32_t getMaxValue() const override { return 63; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

} // namespace deluge::gui::menu_item::midi_fx
