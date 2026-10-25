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
#include "gui/menu_item/integer.h"
#include "gui/menu_item/midi_fx/target.h"
#include "gui/ui/sound_editor.h"
#include "modulation/midi_fx/spice.h"

/// NOTE: there is also a `deluge::modulation::midi_fx` (the effects themselves). Any file with
/// using-directives for both `deluge::modulation` and `deluge::gui::menu_item` - `menus.cpp` is
/// one - must qualify as `menu_item::midi_fx::` or the name is ambiguous.
namespace deluge::gui::menu_item::midi_fx {

/// Spice: how far the gates wander from what is written.
///
/// 0 is off and the sequencer takes exactly the path it took before this existed. Turned up, steps
/// start dropping out and surviving gates get shorter - both decided by a hash of the Dice value
/// below, so the variation repeats bar after bar rather than churning.
class SpiceAmount final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		const auto* s = midiFxSettingsForRead();
		this->setValue(s != nullptr ? s->spiceAmount : deluge::modulation::midi_fx::spice::kAmountOff);
	}

	void writeCurrentValue() override {
		const auto amount = static_cast<uint8_t>(this->getValue());
		midiFxApply([amount](deluge::modulation::midi_fx::MidiFxSettings& s) { s.spiceAmount = amount; });
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::spice::kAmountOff; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::spice::kAmountMax; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Dice: which variation is playing.
///
/// **Turning this knob is the roll.** Arturia's Dice is a momentary button that draws a new
/// variation; this is the same gesture with the outcome left on screen, which is strictly better
/// here - the number names the variation, so the bar you liked two turns ago is two turns back
/// rather than gone. It is a plain index, not a seed that gets drawn: the same number always gives
/// the same pattern.
///
/// It stays useful live because a horizontal menu page lets the encoder change the column value in
/// place - park on the Spice page and one knob rolls. A learnable MIDI command that rolls without
/// looking is the obvious next step and is not in v1.
class Dice final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		const auto* s = midiFxSettingsForRead();
		this->setValue(s != nullptr ? s->spiceDice : deluge::modulation::midi_fx::spice::kDiceMin);
	}

	void writeCurrentValue() override {
		const auto dice = static_cast<uint8_t>(this->getValue());
		midiFxApply([dice](deluge::modulation::midi_fx::MidiFxSettings& s) { s.spiceDice = dice; });
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::spice::kDiceMin; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::spice::kDiceMax; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

} // namespace deluge::gui::menu_item::midi_fx
