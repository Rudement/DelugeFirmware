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

#include "dsp/eq_bands.hpp"
#include "gui/menu_item/eq/eq_readout.h"
#include "gui/menu_item/unpatched_param.h"
#include "gui/menu_item/value_scaling.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"

namespace deluge::gui::menu_item::eq {

/// Which of the four EQ bands a menu item belongs to. Borrowed from the DSP rather than redeclared
/// so the readouts and the filter can never drift apart on what "low mid" means.
using deluge::dsp::eq::Band;

/// The four EQ frequency controls, reporting where the band actually sits rather than a bare 0-50.
///
/// Only the displayed value changes. The param's range, its automation and its stored value are all
/// untouched; this is a relabelling and nothing more.
/// NOT `final`: the two mid bands are presented through
/// rf::Gated<EqFreqParam, FourBandEq>, which derives from this to hide them when the
/// Four-Band EQ community feature is off. Marking it final breaks that gate at compile time.
class EqFreqParam : public UnpatchedParam {
public:
	EqFreqParam(l10n::String newName, int32_t newP, Band band) : UnpatchedParam(newName, newP), band_{band} {}
	EqFreqParam(l10n::String newName, l10n::String title, int32_t newP, Band band)
	    : UnpatchedParam(newName, title, newP), band_{band} {}

protected:
	/// OLED. Replaces the plain number drawn by Integer::drawInteger(); IntegerContinuous still
	/// draws its bar underneath, since that comes from drawPixelsForOled() one level up.
	void drawInteger(int32_t textWidth, int32_t textHeight, int32_t yPixel) override {
		char buffer[16];
		formatFrequency(buffer, hz());
		deluge::hid::display::OLED::main.drawStringCentred(buffer, yPixel + OLED_MAIN_TOPMOST_PIXEL, textWidth,
		                                                   textHeight);
	}

	/// 7SEG. Four characters, and the '.' is merged into the preceding digit by encodeText(), so
	/// "4.87" and "20.0" both fit. The unit is dropped — there is no room for it and the menu name
	/// has just been on screen.
	void drawValue() override {
		char buffer[16];
		formatFrequency(buffer, hz(), false);
		display->setText(buffer, true);
	}

private:
	Band band_;

	/// The q31 the param will hold once writeCurrentValue() runs, which is what the DSP reads.
	///
	/// getValue() is the 0-50 menu position and computeFinalValueForStandardMenuItem() is the exact
	/// conversion the write path uses. Reading back through the param manager instead would lag by
	/// one encoder click, because the redraw happens before the write has landed.
	[[nodiscard]] float hz() {
		return dsp::eq::freqHz(band_, computeFinalValueForStandardMenuItem(this->getValue()));
	}
};

} // namespace deluge::gui::menu_item::eq
