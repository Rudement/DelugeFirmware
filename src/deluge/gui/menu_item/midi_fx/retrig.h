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
#include "gui/menu_item/sync_level.h"
#include "gui/ui/sound_editor.h"

/// NOTE: there is also a `deluge::modulation::midi_fx`. Any file with using-directives for both
/// `deluge::modulation` and `deluge::gui::menu_item` must qualify as `menu_item::midi_fx::`.
namespace deluge::gui::menu_item::midi_fx {

/// How many extra hits follow the original. 0 is off.
class RetrigRepeats final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override {
		this->setValue(midiFxSettingsForRead() != nullptr ? midiFxSettingsForRead()->retrigRepeats : 0);
	}
	void writeCurrentValue() override {
		midiFxApply([&](deluge::modulation::midi_fx::MidiFxSettings& s) {
			s.retrigRepeats = static_cast<uint8_t>(this->getValue());
		});
	}
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 16; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Velocity multiplier per repeat. 100 = no fade.
class RetrigDecay final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override {
		this->setValue(midiFxSettingsForRead() != nullptr ? midiFxSettingsForRead()->retrigDecayPercent : 100);
	}
	void writeCurrentValue() override {
		midiFxApply([&](deluge::modulation::midi_fx::MidiFxSettings& s) {
			s.retrigDecayPercent = static_cast<uint8_t>(this->getValue());
		});
	}
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 100; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// The interval between hits. Reuses the shared SyncLevel item, so triplet and dotted divisions
/// come free and behave exactly as they do for the arpeggiator.
class RetrigSync final : public SyncLevel {
public:
	using SyncLevel::SyncLevel;
	void readCurrentValue() override {
		if (midiFxSettingsForRead() == nullptr) {
			this->setValue(0);
			return;
		}
		this->setValue(syncTypeAndLevelToMenuOption(midiFxSettingsForRead()->retrigSyncType,
		                                            midiFxSettingsForRead()->retrigSyncLevel));
	}
	void writeCurrentValue() override {
		const int32_t v = this->getValue();
		midiFxApply([v](deluge::modulation::midi_fx::MidiFxSettings& s) {
			s.retrigSyncType = syncValueToSyncType(v);
			s.retrigSyncLevel = syncValueToSyncLevel(v);
		});
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }
};

} // namespace deluge::gui::menu_item::midi_fx
