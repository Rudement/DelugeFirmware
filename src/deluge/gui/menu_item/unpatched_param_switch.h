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

#include "gui/l10n/l10n.h"
#include "gui/menu_item/unpatched_param.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/oled.h"
#include "modulation/params/param_set.h"

namespace deluge::gui::menu_item {

/*
 * An unpatched param presented as an on/off switch instead of a continuous knob.
 *
 * WHY A PARAM AND NOT A BOOL. Every other FX switch on the Deluge — delay ping-pong, stutter
 * reverse — is a plain bool on the ModControllable, read through a small Selection subclass.
 * That is much less machinery than a param, which additionally costs an enum slot and entries
 * in the automation lists. The one thing it buys, and the reason to pay: a bool cannot be
 * recorded into a clip or thrown by a gold knob. For a bypass that is the difference between a
 * setup option and a performance gesture.
 *
 * On this branch it costs NO CC map or shortcut grid entry, because there is no free grid
 * position left to give it one — see the note on UNPATCHED_GRISTLE_ON in param.h. Automation
 * and the gold knobs still reach it; MIDI Follow does not.
 *
 * THE VALUE IS THRESHOLDED, NEVER BLENDED. What is stored is an ordinary full-range q31, so
 * automation, CCs and gold knobs need no special case anywhere. The DSP compares it against
 * centre and nothing else. A ramp therefore flips the effect exactly once, at the halfway
 * point, rather than fading it in — which is what a bypass should do, and why this is not
 * simply a Depth knob by another name.
 *
 * readCurrentValue() below MUST use the same comparison as the DSP. If the two ever disagree
 * the menu will confidently show OFF while the effect is audible, which is precisely the class
 * of bug the master switch was added to remove.
 */
class UnpatchedParamSwitch final : public UnpatchedParam {
public:
	using UnpatchedParam::UnpatchedParam;

	// Two positions rather than the usual 0..50. Integer::selectEncoderAction clamps to this
	// range, so the encoder can only land on 0 or 1 and there is no intermediate state to
	// render.
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 1; }

	void readCurrentValue() override {
		setValue(soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(getP()) > 0 ? 1 : 0);
	}

	uint8_t shouldDrawDotOnName() override {
		readCurrentValue();
		return getValue() != 0 ? 3 : 255;
	}

protected:
	// Rail, rather than scale 0..50 through the standard taper. Writing the extremes keeps the
	// stored value unambiguous either side of the threshold, so a param that has been touched
	// by the menu reads back identically whether it is later moved by a knob, a CC or nothing
	// at all.
	int32_t getFinalValue() override { return getValue() != 0 ? 2147483647 : -2147483648; }

	// 7SEG.
	void drawValue() override { display->setText(getNameFor(getValue() != 0)); }

	// OLED, full screen. Deliberately the same two-line list Toggle draws, so that a switch
	// backed by a param and a switch backed by a bool are indistinguishable to the user.
	void drawPixelsForOled() override {
		deluge::hid::display::oled_canvas::Canvas& canvas = deluge::hid::display::OLED::main;

		int32_t yPixel = (OLED_MAIN_HEIGHT_PIXELS == 64) ? 15 : 14;
		yPixel += OLED_MAIN_TOPMOST_PIXEL;

		const bool selectedOption = getValue() != 0;
		const bool order[2] = {false, true};
		for (bool o : order) {
			const char* name = getNameFor(o);
			canvas.drawString(name, kTextSpacingX, yPixel, kTextSpacingX, kTextSpacingY);

			if (o == selectedOption) {
				canvas.invertArea(0, OLED_MAIN_WIDTH_PIXELS, yPixel, yPixel + 8);
				deluge::hid::display::OLED::setupSideScroller(0, name, kTextSpacingX, OLED_MAIN_WIDTH_PIXELS, yPixel,
				                                              yPixel + 8, kTextSpacingX, kTextSpacingY, true);
			}

			yPixel += kTextSpacingY;
		}
	}

private:
	static const char* getNameFor(bool on) {
		return l10n::get(on ? l10n::String::STRING_FOR_ENABLED : l10n::String::STRING_FOR_DISABLED);
	}
};

} // namespace deluge::gui::menu_item
