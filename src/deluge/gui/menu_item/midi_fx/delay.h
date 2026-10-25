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
#include "gui/menu_item/selection.h"
#include "gui/menu_item/sync_level.h"
#include "gui/ui/sound_editor.h"

/// The MIDI Delay page, laid out like the RS7000's: Level, Time, Repeats (its Delay page) then FB
/// Vel, FB Note, FB Rnd, FB Clock (its Feedback page). Paged across the HorizontalMenu.
///
/// NOTE: there is also a `deluge::modulation::midi_fx`. Any file with using-directives for both
/// `deluge::modulation` and `deluge::gui::menu_item` must qualify as `menu_item::midi_fx::`.
namespace deluge::gui::menu_item::midi_fx {

/// DELAY LEVEL. The velocity of the first repeat as a percentage of the source note's, 0..127 - the
/// RS7000's "DELAY LEVEL". 100 echoes at the level the note was played; the feedback controls take
/// the tail from there. 0 silences the echo, so the effect is inert.
class DelayLevel final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override {
		this->setValue(midiFxSettingsForRead() != nullptr ? midiFxSettingsForRead()->delayLevelPercent : 0);
	}
	void writeCurrentValue() override {
		midiFxApply([&](deluge::modulation::midi_fx::MidiFxSettings& s) {
			s.delayLevelPercent = static_cast<uint8_t>(this->getValue());
		});
	}
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// How many repeats follow the note. 0 is off, which is the default, so the effect is inert until
/// asked for.
class DelayRepeats final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override {
		this->setValue(midiFxSettingsForRead() != nullptr ? midiFxSettingsForRead()->delayRepeats : 3);
	}
	void writeCurrentValue() override {
		midiFxApply([&](deluge::modulation::midi_fx::MidiFxSettings& s) {
			s.delayRepeats = static_cast<uint8_t>(this->getValue());
		});
	}
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	/// 0..64, the RS7000's REPEAT TIMES range. One pool entry serves all of a note's repeats (the
	/// Echo record is re-used each repeat - see delay_fx.h), so the count is bounded by taste, not by
	/// NotePool::kSize; the pool only limits how many *notes* can echo at once (kMaxEchoes).
	[[nodiscard]] int32_t getMaxValue() const override { return 64; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// FB VELOCITY. Added to each repeat's velocity, −127..+127 - the RS7000's additive feedback, so a
/// negative value fades the tail and a positive one swells it. Below a floor of 1 the tail ends
/// rather than emitting velocity 0, which on MIDI is a note-off.
class DelayFbVelocity final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override {
		this->setValue(midiFxSettingsForRead() != nullptr ? midiFxSettingsForRead()->delayFbVelocity : 0);
	}
	void writeCurrentValue() override {
		midiFxApply([&](deluge::modulation::midi_fx::MidiFxSettings& s) {
			s.delayFbVelocity = static_cast<int8_t>(this->getValue());
		});
	}
	[[nodiscard]] int32_t getMinValue() const override { return -127; }
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// FB NOTE. Semitones added per repeat, −24..+24 - the RS7000's "FB NOTE". 0 is a plain echo;
/// anything else gives the classic rising or falling delay. A repeat transposed off the end of the
/// keyboard ends the tail there. The random variant is the separate FB Rnd toggle beside it.
class DelayPitch final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override {
		this->setValue(midiFxSettingsForRead() != nullptr ? midiFxSettingsForRead()->delayPitchShift : 0);
	}
	void writeCurrentValue() override {
		midiFxApply([&](deluge::modulation::midi_fx::MidiFxSettings& s) {
			s.delayPitchShift = static_cast<int8_t>(this->getValue());
		});
	}
	[[nodiscard]] int32_t getMinValue() const override { return -24; }
	[[nodiscard]] int32_t getMaxValue() const override { return 24; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// FB NOTE = RND. The RS7000's "RDN" setting on the FB NOTE knob, here its own toggle: when on,
/// each repeat is transposed by a fresh random interval instead of by the fixed FB Note step.
class DelayNoteRandom final : public Selection {
public:
	using Selection::Selection;
	void readCurrentValue() override {
		const auto* s = midiFxSettingsForRead();
		this->setValue(s != nullptr && s->delayNoteRandom);
	}
	void writeCurrentValue() override {
		// getValue<E>() is constrained to `util::enumeration`, so getValue<bool>() does not compile -
		// comparing Value<int32_t>::getValue() against 0 is what every other toggle here does.
		const bool on = this->getValue() != 0;
		midiFxApply([on](deluge::modulation::midi_fx::MidiFxSettings& s) { s.delayNoteRandom = on; });
	}
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {
		    l10n::getView(STRING_FOR_OFF), //<
		    l10n::getView(STRING_FOR_ON),  //<
		};
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }
	bool isToggle() override { return true; }
	bool shouldEnterSubmenu() override { return false; }
};

/// FB CLOCK. Scales the interval before each successive repeat, −100..+100 % - the RS7000's "FB
/// CLOCK". 0 keeps the echoes on the grid; negative accelerates them (the bouncing-ball delay),
/// positive slows them down. The interval is floored at one tick so a repeat can never land on top
/// of the one before it.
class DelayFbClock final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override {
		this->setValue(midiFxSettingsForRead() != nullptr ? midiFxSettingsForRead()->delayFbClockPercent : 0);
	}
	void writeCurrentValue() override {
		midiFxApply([&](deluge::modulation::midi_fx::MidiFxSettings& s) {
			s.delayFbClockPercent = static_cast<int8_t>(this->getValue());
		});
	}
	[[nodiscard]] int32_t getMinValue() const override { return -100; }
	[[nodiscard]] int32_t getMaxValue() const override { return 100; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// DELAY TIME. The interval between repeats, the shared SyncLevel item, so the divisions read and
/// behave exactly as the arpeggiator's do - which matters, because an arp into a delay that rounded
/// its divisions differently would drift against itself.
class DelaySync final : public SyncLevel {
public:
	using SyncLevel::SyncLevel;
	void readCurrentValue() override {
		if (midiFxSettingsForRead() == nullptr) {
			this->setValue(0);
			return;
		}
		this->setValue(syncTypeAndLevelToMenuOption(midiFxSettingsForRead()->delaySyncType,
		                                            midiFxSettingsForRead()->delaySyncLevel));
	}
	void writeCurrentValue() override {
		const int32_t v = this->getValue();
		midiFxApply([v](deluge::modulation::midi_fx::MidiFxSettings& s) {
			s.delaySyncType = syncValueToSyncType(v);
			s.delaySyncLevel = syncValueToSyncLevel(v);
		});
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }
};

} // namespace deluge::gui::menu_item::midi_fx
