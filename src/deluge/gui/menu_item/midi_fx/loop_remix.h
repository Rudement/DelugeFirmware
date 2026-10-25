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
#include "gui/menu_item/midi_fx/play_fx.h"
#include "gui/menu_item/midi_fx/switched_on.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "model/clip/instrument_clip.h"
#include "model/settings/runtime_feature_settings.h"
#include "modulation/midi_fx/loop_remix.h"
#include "hid/display/oled.h"
#include <cstdio>

/// NOTE: there is also a `deluge::modulation::midi_fx` (the effects themselves). Any file with
/// using-directives for both `deluge::modulation` and `deluge::gui::menu_item` - `menus.cpp` is
/// one - must qualify as `menu_item::midi_fx::` or the name is ambiguous.
namespace deluge::gui::menu_item::midi_fx {

/// The Loop Remix page, laid out as on the RS7000: Type, Variation, Interval, Remix, Default.
///
/// Where a value lives follows Play FX, and for the same reason: on a melodic clip it is the Clip's,
/// on a Kit with a drum selected it is that drum's own, and with AFFECT ENTIRE held it is every
/// drum's at once. A Kit row is a drum, and remixing the hats without the kick is the thing people
/// reach for; the Kit-wide gesture is the one the Deluge already uses everywhere. With a Kit, no
/// drum selected and no AFFECT ENTIRE there is nothing to edit, and the page hides.
namespace lrx = deluge::modulation::midi_fx::loop_remix;

/// Builds the family + index label shown for a Variation value. Long form ("REVERSE 5") for the
/// full-screen value and the 7-seg scroll; short form ("REV5") for the horizontal-menu column, where
/// four items share the width. Families follow lrx::familyOf: sixteen each of Normal, Reverse, Break,
/// Pitch and Roll, then forty-eight Fills. The stored 0-127 value and the hash are unchanged - this
/// only changes what the display reads off the value. Modeled on arpeggiator::Rhythm.
inline void loopRemixVariationLabel(char* buf, size_t bufLen, uint8_t variation, bool shortForm) {
	static constexpr const char* kLong[6] = {"NORMAL", "REVERSE", "BREAK", "PITCH", "ROLL", "FILL"};
	static constexpr const char* kShort[6] = {"NOR", "REV", "BRK", "PIT", "ROL", "FIL"};
	const uint8_t family = lrx::familyOf(variation);
	const int32_t index =
	    (family == lrx::kFamilyFill ? (variation - lrx::kFirstFillVariation) : (variation % 16)) + 1;
	snprintf(buf, bufLen, "%s %d", shortForm ? kShort[family] : kLong[family], static_cast<int>(index));
}

[[nodiscard]] inline bool loopRemixEditable() {
	return playFxEditable()
	       && runtimeFeatureSettings.get(RuntimeFeatureSettingType::LoopRemix) == RuntimeFeatureStateToggle::On;
}

/// The slot to show: the Clip's, the selected drum's, or under AFFECT ENTIRE with no drum selected,
/// the first row's (they are written together).
[[nodiscard]] inline const lrx::Slot* loopRemixSlotForRead() {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr) {
		return nullptr;
	}
	if (!playFxIsKit()) {
		return &clip->loopRemix;
	}
	if (NoteRow* row = playFxSelectedRow(); row != nullptr) {
		return &row->loopRemix;
	}
	if (clip->affectEntire && clip->noteRows.getNumElements() > 0) {
		return &clip->noteRows.getElement(0)->loopRemix;
	}
	return nullptr;
}

/// Runs fn on every slot an edit reaches.
template <typename Fn>
inline void loopRemixWrite(Fn&& fn) {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr) {
		return;
	}
	playFxTempHoldNoteClip(clip);
	if (!playFxIsKit()) {
		fn(clip->loopRemix);
		return;
	}
	if (clip->affectEntire) {
		for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
			playFxTempHoldNoteRow(clip->noteRows.getElement(i));
			fn(clip->noteRows.getElement(i)->loopRemix);
		}
		return;
	}
	if (NoteRow* row = playFxSelectedRow(); row != nullptr) {
		playFxTempHoldNoteRow(row);
		fn(row->loopRemix);
	}
}

/// Type: how complex the remix is, 1-16, as on the RS7000.
///
/// Shown one-based, stored zero-based. The RS7000 numbers its types from 1 and so does every
/// conversation about it; the zero-based value is what indexes the hash, and there is no reason for
/// the person at the instrument to meet it.
class LoopRemixType final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		const auto* slot = loopRemixSlotForRead();
		this->setValue(slot != nullptr ? slot->page.type : 0);
	}

	void writeCurrentValue() override {
		const auto value = static_cast<uint8_t>(this->getValue());
		loopRemixWrite([value](lrx::Slot& slot) { slot.page.type = value; });
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::loop_remix::kTypeMin; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::loop_remix::kTypeMax; }
	[[nodiscard]] int32_t getDisplayValue() override { return this->getValue() + 1; }
	void getNotificationValue(StringBuf& value) override { value.appendInt(getDisplayValue()); }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return loopRemixEditable();
	}

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	// Show the one-based number in the page column too, not Number's raw zero-based value.
	void renderInHorizontalMenu(const SlotPosition& slot) override {
		char buf[8];
		snprintf(buf, sizeof(buf), "%d", static_cast<int>(getDisplayValue()));
		deluge::hid::display::OLED::main.drawStringCentered(buf, slot.start_x, slot.start_y + kHorizontalMenuSlotYOffset,
		                                                    kTextTitleSpacingX, kTextTitleSizeY, slot.width);
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Variation: which arrangement at that intensity. The RS7000's knob 2.
///
/// An index, not a seed that gets drawn - so turning it back brings the earlier remix back at the
/// same number. That is the whole reason this is a knob rather than a button, and it is why the
/// footswitch command steps it rather than randomising the lot.
class LoopRemixVariation final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		const auto* slot = loopRemixSlotForRead();
		this->setValue(slot != nullptr ? slot->page.variation : 0);
	}

	void writeCurrentValue() override {
		const auto value = static_cast<uint8_t>(this->getValue());
		loopRemixWrite([value](lrx::Slot& slot) { slot.page.variation = value; });
	}

	[[nodiscard]] int32_t getMinValue() const override {
		return deluge::modulation::midi_fx::loop_remix::kVariationMin;
	}
	[[nodiscard]] int32_t getMaxValue() const override {
		return deluge::modulation::midi_fx::loop_remix::kVariationMax;
	}

	// Shown as family + index rather than a bare 1-128, so Normal/Reverse/Break/Pitch/Roll/Fill read
	// off the page the way they do on the RS7000. Value and hash unchanged; this is display only.
	void drawValue() override { // 7-seg: scroll the full name, e.g. "REVERSE 5"
		char buf[12];
		loopRemixVariationLabel(buf, sizeof(buf), static_cast<uint8_t>(this->getValue()), false);
		display->setScrollingText(buf);
	}

	void drawInteger(int32_t textWidth, int32_t textHeight, int32_t yPixel) override { // OLED full value
		char buf[12];
		loopRemixVariationLabel(buf, sizeof(buf), static_cast<uint8_t>(this->getValue()), false);
		deluge::hid::display::OLED::main.drawStringCentred(buf, yPixel + OLED_MAIN_TOPMOST_PIXEL, textWidth,
		                                                   textHeight);
	}

	void renderInHorizontalMenu(const SlotPosition& slot) override { // OLED page column: short form
		char buf[8];
		loopRemixVariationLabel(buf, sizeof(buf), static_cast<uint8_t>(this->getValue()), true);
		deluge::hid::display::OLED::main.drawStringCentered(buf, slot.start_x, slot.start_y + kHorizontalMenuSlotYOffset,
		                                                    kTextSpacingX, kTextSpacingY, slot.width);
	}

	// The popup that flashes when you turn the knob (HorizontalMenu::displayNotification) - show the
	// family label here too, not the raw 0-127 the Number base would print.
	void getNotificationValue(StringBuf& value) override {
		char buf[12];
		loopRemixVariationLabel(buf, sizeof(buf), static_cast<uint8_t>(this->getValue()), false);
		value.append(buf);
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return loopRemixEditable();
	}

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	// Two columns wide: the family + index label does not fit the ~5 characters a single horizontal-menu
	// column gives, so a two-digit index had its last digit trimmed off. Same reason arpeggiator::Rhythm
	// takes two slots.
	[[nodiscard]] int32_t getOccupiedSlots() const override { return 2; }
};

/// Interval: how often the remix happens. 1 is every bar, 2 every other bar, up to every 8th.
class LoopRemixInterval final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		const auto* slot = loopRemixSlotForRead();
		this->setValue(slot != nullptr ? slot->page.interval : 0);
	}

	void writeCurrentValue() override {
		const auto value = static_cast<uint8_t>(this->getValue());
		loopRemixWrite([value](lrx::Slot& slot) { slot.page.interval = value; });
	}

	[[nodiscard]] int32_t getMinValue() const override { return lrx::kIntervalMin; }
	[[nodiscard]] int32_t getMaxValue() const override { return lrx::kIntervalMax; }
	[[nodiscard]] int32_t getDisplayValue() override { return this->getValue() + 1; }
	void getNotificationValue(StringBuf& value) override { value.appendInt(getDisplayValue()); }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return loopRemixEditable();
	}

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	// Show the one-based number in the page column too, not Number's raw zero-based value.
	void renderInHorizontalMenu(const SlotPosition& slot) override {
		char buf[8];
		snprintf(buf, sizeof(buf), "%d", static_cast<int>(getDisplayValue()));
		deluge::hid::display::OLED::main.drawStringCentered(buf, slot.start_x, slot.start_y + kHorizontalMenuSlotYOffset,
		                                                    kTextTitleSpacingX, kTextTitleSizeY, slot.width);
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Remix: the button. Press SELECT on it and the page's settings are what you hear.
///
/// Type, Variation and Interval only set up the next remix; the clip keeps playing the last one
/// pressed (or the plain loop) until this is pressed. Change a setting and press it again and the
/// new setting takes over. Reset, beside it, is the way back to the plain loop.
///
/// One entry with nothing to turn - the encoder does nothing here. The same shape as the RS7000's
/// REMIX key: you set up, you press, you hear it.
class LoopRemixLaunch final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override { this->setValue(0); }

	void writeCurrentValue() override { this->setValue(0); }

	MenuItem* selectButtonPress() override {
		loopRemixWrite([](lrx::Slot& slot) { slot.launch(); });
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_MIDI_FX_LOOP_REMIX_LAUNCH));
		return NO_NAVIGATION;
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		return {l10n::getView(l10n::String::STRING_FOR_MIDI_FX_LOOP_REMIX_LAUNCH)};
	}

	/// The popup above is the confirmation; the horizontal menu's own notification would only say
	/// "Remix" a second time.
	[[nodiscard]] bool showNotification() const override { return false; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return loopRemixEditable();
	}

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	bool shouldEnterSubmenu() override { return false; }
};

} // namespace deluge::gui::menu_item::midi_fx
