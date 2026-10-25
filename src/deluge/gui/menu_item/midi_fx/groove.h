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
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "model/clip/instrument_clip.h"
#include "model/settings/runtime_feature_settings.h"
#include "modulation/midi_fx/groove.h"
#include "modulation/midi_fx/groove_file.h"
#include "util/cfunctions.h"
#include <cstring>

/// NOTE: there is also a `deluge::modulation::midi_fx` (the effects themselves). Any file with
/// using-directives for both `deluge::modulation` and `deluge::gui::menu_item` - `menus.cpp` is
/// one - must qualify as `menu_item::midi_fx::` or the name is ambiguous.
namespace deluge::gui::menu_item::midi_fx {

/// The Groove page: Template, Accent, Time, Random, Offset, Reset.
///
/// Where a value lives follows Loop Remix exactly: on a melodic clip it is the Clip's, on a Kit with a
/// drum selected it is that drum's own, and with AFFECT ENTIRE held it is every drum's at once. Grooving
/// the hats while the kick stays straight is a thing people do; the whole kit is one gesture away.
namespace grv = deluge::modulation::midi_fx::groove;

[[nodiscard]] inline bool grooveEditable() {
	return playFxEditable()
	       && runtimeFeatureSettings.get(RuntimeFeatureSettingType::Groove) == RuntimeFeatureStateToggle::On;
}

/// The settings to show: the Clip's, the selected drum's, or under AFFECT ENTIRE with no drum selected, the
/// first row's (they are written together).
[[nodiscard]] inline const grv::Settings* grooveForRead() {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr) {
		return nullptr;
	}
	if (!playFxIsKit()) {
		return &clip->groove;
	}
	if (NoteRow* row = playFxSelectedRow(); row != nullptr) {
		return &row->groove;
	}
	if (clip->affectEntire && clip->noteRows.getNumElements() > 0) {
		return &clip->noteRows.getElement(0)->groove;
	}
	return nullptr;
}

/// Runs fn on every Settings an edit reaches, recording each with Temp Hold first.
template <typename Fn>
inline void grooveWrite(Fn&& fn) {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr) {
		return;
	}
	playFxTempHoldNoteClip(clip);
	if (!playFxIsKit()) {
		fn(clip->groove);
		return;
	}
	if (clip->affectEntire) {
		for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
			playFxTempHoldNoteRow(clip->noteRows.getElement(i));
			fn(clip->noteRows.getElement(i)->groove);
		}
		return;
	}
	if (NoteRow* row = playFxSelectedRow(); row != nullptr) {
		playFxTempHoldNoteRow(row);
		fn(row->groove);
	}
}

// --- The Template list ----------------------------------------------------------------------------
//
// One list, in this order: Off, the built-ins, the .agr files in /GROOVES, then any groove a loaded song
// brought with it whose file is not on this card. The last group is what lets a song made on another card
// still show - and keep - its groove.

/// Library entries with no matching file, in library order.
[[nodiscard]] inline int32_t grooveOrphanCount() {
	int32_t n = 0;
	for (int32_t i = 0; i < grv::numUserLoaded(); i++) {
		const grv::Template* t = grv::templateFor(static_cast<uint8_t>(grv::kUserBase + i));
		if (t != nullptr && grv::file::indexOfName(t->name) < 0) {
			n++;
		}
	}
	return n;
}

[[nodiscard]] inline uint8_t grooveOrphanId(int32_t which) {
	for (int32_t i = 0; i < grv::numUserLoaded(); i++) {
		const auto id = static_cast<uint8_t>(grv::kUserBase + i);
		const grv::Template* t = grv::templateFor(id);
		if (t != nullptr && grv::file::indexOfName(t->name) < 0 && which-- == 0) {
			return id;
		}
	}
	return grv::kOff;
}

[[nodiscard]] inline int32_t grooveChoiceCount(bool rescan) {
	const int32_t files = rescan ? grv::file::scan() : grv::file::cachedCount();
	return 1 + grv::numBuiltIn() + files + grooveOrphanCount();
}

/// The list position of a template id.
[[nodiscard]] inline int32_t grooveChoiceFor(uint8_t id) {
	if (id == grv::kOff) {
		return 0;
	}
	if (id <= grv::numBuiltIn()) {
		return id;
	}
	const grv::Template* t = grv::templateFor(id);
	if (t == nullptr) {
		return 0;
	}
	const int32_t files = grv::file::cachedCount();
	const int32_t fileIndex = grv::file::indexOfName(t->name);
	if (fileIndex >= 0) {
		return 1 + grv::numBuiltIn() + fileIndex;
	}
	int32_t orphan = 0;
	for (int32_t i = 0; i < grv::numUserLoaded(); i++) {
		const auto other = static_cast<uint8_t>(grv::kUserBase + i);
		const grv::Template* o = grv::templateFor(other);
		if (o == nullptr || grv::file::indexOfName(o->name) >= 0) {
			continue;
		}
		if (other == id) {
			return 1 + grv::numBuiltIn() + files + orphan;
		}
		orphan++;
	}
	return 0;
}

/// Copies a name into buf, cut to fit. (Not snprintf: a header can't pull in the firmware's printf.)
inline void grooveCopyName(char* buf, size_t bufLen, const char* src) {
	size_t i = 0;
	for (; i + 1 < bufLen && src[i] != 0; i++) {
		buf[i] = src[i];
	}
	buf[i] = 0;
}

/// The name shown for a list position.
inline void grooveChoiceName(int32_t choice, char* buf, size_t bufLen) {
	if (choice <= 0) {
		grooveCopyName(buf, bufLen, "OFF");
		return;
	}
	if (choice <= grv::numBuiltIn()) {
		grooveCopyName(buf, bufLen, grv::templateFor(static_cast<uint8_t>(choice))->name);
		return;
	}
	const int32_t files = grv::file::cachedCount();
	const int32_t fileIndex = choice - 1 - grv::numBuiltIn();
	if (fileIndex < files) {
		grooveCopyName(buf, bufLen, grv::file::name(fileIndex));
		return;
	}
	const grv::Template* t = grv::templateFor(grooveOrphanId(fileIndex - files));
	grooveCopyName(buf, bufLen, t != nullptr ? t->name : "?");
}

/// Template: Off, a built-in, or an Ableton groove file from /GROOVES.
///
/// Choosing one while Accent and Time are both at 0 also sets them, so a groove is heard the moment it is
/// picked: an .agr file brings the Timing, Velocity and Random it was saved with in Live's Groove Pool, a
/// built-in comes in at Time 100 and Accent 50. Once either has been dialled in, changing template leaves
/// your amounts alone - that is the point of having them.
class GrooveTemplate final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		const auto* s = grooveForRead();
		this->setValue(s != nullptr ? grooveChoiceFor(s->templateId) : 0);
	}

	void writeCurrentValue() override {
		const int32_t choice = this->getValue();
		uint8_t id = grv::kOff;
		bool fromFile = false;
		grv::FileAmounts amounts;
		const int32_t files = grv::file::cachedCount();
		if (choice > 0 && choice <= grv::numBuiltIn()) {
			id = static_cast<uint8_t>(choice);
		}
		else if (choice > grv::numBuiltIn()) {
			const int32_t fileIndex = choice - 1 - grv::numBuiltIn();
			if (fileIndex < files) {
				id = grv::file::load(fileIndex, &amounts);
				fromFile = true;
				if (id == grv::kOff) {
					display->displayPopup(
					    deluge::l10n::get(deluge::l10n::String::STRING_FOR_MIDI_FX_GROOVE_CANT_LOAD));
				}
			}
			else {
				id = grooveOrphanId(fileIndex - files);
			}
		}
		const grv::Template* t = grv::templateFor(id);
		grooveWrite([&](grv::Settings& s) {
			const bool fresh = s.accent == 0 && s.time == 0;
			s.templateId = id;
			if (t != nullptr) {
				if (fresh) {
					if (fromFile) {
						s.time = static_cast<int8_t>(amounts.timing);
						s.accent = static_cast<int8_t>(amounts.velocity);
						s.random = static_cast<uint8_t>(amounts.random);
					}
					else {
						s.time = 100;
						s.accent = 50;
					}
				}
			}
		});
	}

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return grooveChoiceCount(true) - 1; }

	void drawValue() override { // 7-seg: scroll the name
		char buf[grv::kNameMax + 4];
		grooveChoiceName(this->getValue(), buf, sizeof(buf));
		display->setScrollingText(buf);
	}

	void drawInteger(int32_t textWidth, int32_t textHeight, int32_t yPixel) override { // OLED full value
		char buf[grv::kNameMax + 4];
		grooveChoiceName(this->getValue(), buf, sizeof(buf));
		deluge::hid::display::OLED::main.drawStringCentred(buf, yPixel + OLED_MAIN_TOPMOST_PIXEL, textWidth,
		                                                   textHeight);
	}

	void renderInHorizontalMenu(const SlotPosition& slot) override { // OLED page column
		// Read live, not from the page's cache: the page only re-reads a column when it changes page.
		readCurrentValue();
		char buf[grv::kNameMax + 4];
		grooveChoiceName(this->getValue(), buf, sizeof(buf));
		// Two columns hold about ten characters at this size; a longer name is cut rather than run on.
		buf[10] = 0;
		deluge::hid::display::OLED::main.drawStringCentered(buf, slot.start_x, slot.start_y + kHorizontalMenuSlotYOffset,
		                                                    kTextSpacingX, kTextSpacingY, slot.width);
	}

	void getNotificationValue(StringBuf& value) override {
		char buf[grv::kNameMax + 4];
		grooveChoiceName(this->getValue(), buf, sizeof(buf));
		value.append(buf);
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return grooveEditable();
	}

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] int32_t getOccupiedSlots() const override { return 2; }
};

/// The four amounts share everything but which field they touch and their range.
enum class GrooveField : uint8_t { ACCENT, TIME, RANDOM, OFFSET };

template <GrooveField F>
class GrooveAmount final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		const auto* s = grooveForRead();
		this->setValue(s != nullptr ? get(*s) : 0);
	}

	void writeCurrentValue() override {
		const int32_t value = this->getValue();
		grooveWrite([value](grv::Settings& s) { set(s, value); });
	}

	/// Start from the live value. The page caches each column's value and re-reads it only on a page
	/// change, so after Template filled Accent and Time in, turning one would have started from the
	/// stale 0 the column still showed.
	void selectEncoderAction(int32_t offset) override {
		readCurrentValue();
		Integer::selectEncoderAction(offset);
	}

	/// Live, and in the small font: the page's default large digits fit three characters, which cut
	/// "100" to "10" and would have cut "-120" worse.
	void renderInHorizontalMenu(const SlotPosition& slot) override {
		readCurrentValue();
		char buf[12];
		intToString(this->getValue(), buf, 1);
		deluge::hid::display::OLED::main.drawStringCentered(buf, slot.start_x, slot.start_y + kHorizontalMenuSlotYOffset,
		                                                    kTextSpacingX, kTextSpacingY, slot.width);
	}

	[[nodiscard]] int32_t getMinValue() const override {
		return (F == GrooveField::ACCENT || F == GrooveField::TIME) ? grv::kAmountMin : 0;
	}

	[[nodiscard]] int32_t getMaxValue() const override {
		if constexpr (F == GrooveField::RANDOM) {
			return grv::kRandomMax;
		}
		else if constexpr (F == GrooveField::OFFSET) {
			// A fixed range, not the template's length: the engine wraps it to whatever template is in
			// use. Tying the range to the template pinned it at 0 whenever the settings being shown had
			// no template - a kit's first row under AFFECT ENTIRE, say.
			return grv::kMaxSteps - 1;
		}
		else {
			return grv::kAmountMax;
		}
	}

	const char* getUnit() override { return F == GrooveField::OFFSET ? "" : "%"; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return grooveEditable();
	}

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }

private:
	static int32_t get(const grv::Settings& s) {
		switch (F) {
		case GrooveField::ACCENT:
			return s.accent;
		case GrooveField::TIME:
			return s.time;
		case GrooveField::RANDOM:
			return s.random;
		case GrooveField::OFFSET:
		default:
			return s.offset;
		}
	}

	static void set(grv::Settings& s, int32_t v) {
		switch (F) {
		case GrooveField::ACCENT:
			s.accent = static_cast<int8_t>(std::clamp<int32_t>(v, grv::kAmountMin, grv::kAmountMax));
			break;
		case GrooveField::TIME:
			s.time = static_cast<int8_t>(std::clamp<int32_t>(v, grv::kAmountMin, grv::kAmountMax));
			break;
		case GrooveField::RANDOM:
			s.random = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, grv::kRandomMax));
			break;
		case GrooveField::OFFSET:
		default:
			s.offset = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, grv::kMaxSteps - 1));
			break;
		}
	}
};

using GrooveAccent = GrooveAmount<GrooveField::ACCENT>;
using GrooveTime = GrooveAmount<GrooveField::TIME>;
using GrooveRandom = GrooveAmount<GrooveField::RANDOM>;
using GrooveOffset = GrooveAmount<GrooveField::OFFSET>;

} // namespace deluge::gui::menu_item::midi_fx
