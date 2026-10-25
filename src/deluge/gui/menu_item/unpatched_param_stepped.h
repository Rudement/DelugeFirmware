/*
 * Copyright © 2026
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

#include "definitions_cxx.hpp"
#include "dsp/gristle.hpp"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/unpatched_param.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "modulation/params/param_set.h"
#include "util/containers.h"
#include "util/d_string.h"
#include <algorithm>
#include <span>
#include <string_view>

namespace deluge::gui::menu_item {

/*
 * An unpatched param presented as a NAMED LIST instead of a 0..50 knob.
 *
 * WHY A PARAM AND NOT A Selection. Selection is the usual way to show a list, and it is much
 * less machinery — but it reads and writes a plain variable, and a plain variable cannot be
 * automated, thrown by a gold knob or driven by a CC. Slice Pattern and Variation are exactly
 * the things a performer wants to throw: stepping the variation mid-bar is the pedal's own
 * gesture. So the storage stays an ordinary full-range param and only the presentation changes.
 * This is the same bargain UnpatchedParamSwitch struck for the master switch, one step further
 * along: two positions there, N here.
 *
 * THE BAND MATHS LIVES IN dsp/gristle.hpp, NOT HERE. sliceIndex() decides which option a stored
 * value means and sliceParamForIndex() decides what to store for an option. The DSP reads the
 * param with the first of those every buffer, so if this file grew its own copy the menu would
 * eventually disagree with what you hear — the same failure UnpatchedParamSwitch warns about,
 * where the menu says OFF and the effect is audible.
 *
 * ENCODER RANGE. getMaxValue() is the option count minus one, and Integer::selectEncoderAction
 * clamps to it, so the encoder cannot land between options. A CC or an automation ramp still
 * sweeps the whole range continuously; it just lands in one band or the next, which is what
 * makes a swept CC step through the patterns in order.
 */
class UnpatchedParamStepped : public UnpatchedParam {
public:
	/// Named options, e.g. the slice patterns.
	UnpatchedParamStepped(l10n::String newName, l10n::String title, int32_t newP, std::span<const l10n::String> options)
	    : UnpatchedParam(newName, title, newP), options_(options), count_(static_cast<int32_t>(options.size())) {}

	/// Numbered options, for a list whose members have no names worth giving — Variation is
	/// eleven rhythms derived from one pattern, and the number IS what the performer steps
	/// through. Cheaper than eleven translation strings that all read back as themselves.
	UnpatchedParamStepped(l10n::String newName, l10n::String title, int32_t newP, int32_t numOptions)
	    : UnpatchedParam(newName, title, newP), count_(numOptions) {}

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return count_ - 1; }

	void readCurrentValue() override {
		setValue(deluge::dsp::gristle::sliceIndex(
		    soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(getP()), numOptions()));
	}

	void getNotificationValue(StringBuf& valueBuf) override { valueBuf.append(nameFor(getValue())); }

protected:
	/// The CENTRE of the option's band, never an edge. An edge is one LSB from meaning the
	/// neighbouring option, so a value written here could read back as the next one along.
	int32_t getFinalValue() override { return deluge::dsp::gristle::sliceParamForIndex(getValue(), numOptions()); }

	/// 7SEG. Scrolling because the names are up to four characters and some displays are three.
	void drawValue() override {
		if (display->haveOLED()) {
			renderUIsForOled();
		}
		if (display->have7SEG()) {
			display->setScrollingText(nameFor(getValue()));
		}
	}

	/// OLED, full screen: the same windowed list Selection draws, so a list backed by a param and
	/// a list backed by a variable are indistinguishable to the user.
	void drawPixelsForOled() override {
		deluge::vector<std::string_view> names;
		deluge::vector<NumberName> numbers;
		if (options_.empty()) {
			numbers.resize(count_);
			for (int32_t i = 0; i < count_; i++) {
				intToString(i, numbers[i].text);
				names.push_back(std::string_view{numbers[i].text});
			}
		}
		else {
			for (size_t i = 0; i < options_.size(); i++) {
				names.push_back(l10n::getView(options_[i]));
			}
		}

		int32_t current = getValue();
		int32_t before = std::max<int32_t>(0, current - kOLEDMenuNumOptionsVisible);
		int32_t nBefore = current - before;
		int32_t after = std::min<int32_t>(current + kOLEDMenuNumOptionsVisible, numOptions());
		int32_t nAfter = after - current;

		int32_t pos = (kOLEDMenuNumOptionsVisible - 1) / 2;
		int32_t tail = kOLEDMenuNumOptionsVisible - pos;
		if (nBefore < pos) {
			pos = nBefore;
		}
		else if (nAfter < tail) {
			tail = nAfter;
			pos = std::min<int32_t>(kOLEDMenuNumOptionsVisible - tail, nBefore);
		}

		MenuItem::drawItemsForOled(std::span{names.data(), names.size()}, pos, current - pos);
	}

	/// One slot of a horizontal menu: the option's name, centred, where a knob would show a bar.
	void renderInHorizontalMenu(const SlotPosition& slot) override {
		deluge::hid::display::oled_canvas::Canvas& image = deluge::hid::display::OLED::main;
		image.drawStringCentered(nameFor(getValue()), slot.start_x, slot.start_y + kHorizontalMenuSlotYOffset,
		                         kTextSpacingX, kTextSpacingY, slot.width);
	}

private:
	[[nodiscard]] int32_t numOptions() const { return count_; }

	/// A number rendered as text. Its own type so that the OLED list can hold a whole column of
	/// them alive while drawItemsForOled() reads the views pointing into them.
	struct NumberName {
		char text[8];
	};

	const char* nameFor(int32_t index) {
		int32_t clamped = std::clamp<int32_t>(index, 0, numOptions() - 1);
		if (!options_.empty()) {
			return l10n::get(options_[clamped]);
		}
		intToString(clamped, numberBuffer_.text);
		return numberBuffer_.text;
	}

	std::span<const l10n::String> options_;
	int32_t count_;
	/// Scratch for nameFor() in the numbered case. One UI thread, one value on screen at a time.
	NumberName numberBuffer_{};
};

} // namespace deluge::gui::menu_item
