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
#include "gui/menu_item/midi_fx/ratio_step.h"
#include "gui/menu_item/midi_fx/target.h"
#include "gui/ui/sound_editor.h"

/// NOTE: there is also a `deluge::modulation::midi_fx` (the effects themselves). Any file with
/// using-directives for both `deluge::modulation` and `deluge::gui::menu_item` - `menus.cpp` is
/// one - must qualify as `menu_item::midi_fx::` or the name is ambiguous.
namespace deluge::gui::menu_item::midi_fx {

/// Gate Time as a percentage of a sequenced note's written length, 5% to 400% - the shared
/// ratio-step control (ratio_step.h) with 100% in the middle.
///
/// Bottom of the range is 5, not 0: a 0% gate is a note that never sounds, which reads as a
/// broken effect rather than a very short one. 100% is neutral - the effect returns early
/// there and touches nothing. Above 100% the note is held past its written end, so Gate Time
/// lengthens as well as shortens.
///
/// Only affects **sequenced** notes, and only with the arpeggiator off. A held key has no length
/// to take a percentage of, and the arp has its own Gate. Both are stated in the manual entry
/// rather than left for someone to discover by twisting the knob and hearing nothing.
class GateTime final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		const int32_t percent =
		    midiFxSettingsForRead() != nullptr ? midiFxSettingsForRead()->gateTimePercent : kRatioNeutralPercent;
		this->setValue(percentToRatioStep(percent));
	}

	void writeCurrentValue() override {
		// uint16_t since the range went past 255. Casting to uint8_t here would have wrapped 400%
		// round to 144% and looked like an arithmetic bug three files away.
		const auto percent = static_cast<uint16_t>(ratioStepToPercent(this->getValue()));
		midiFxApply([percent](deluge::modulation::midi_fx::MidiFxSettings& s) { s.gateTimePercent = percent; });
	}

	[[nodiscard]] int32_t getMinValue() const override { return kRatioStepMin; }
	/// Steps, not percent - a short sweep with 0 in the middle, ratio_step.h says why. The value
	/// on screen is still the percentage it lands on, because that is the number that means
	/// something musically.
	[[nodiscard]] int32_t getMaxValue() const override { return kRatioStepMax; }
	[[nodiscard]] int32_t getDisplayValue() override { return ratioStepToPercent(this->getValue()); }
	const char* getUnit() override { return "%"; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

} // namespace deluge::gui::menu_item::midi_fx
