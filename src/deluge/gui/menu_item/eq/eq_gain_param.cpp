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
#include "gui/menu_item/eq/eq_gain_param.h"
#include "dsp/eq_bands.hpp"
#include "gui/menu_item/value_scaling.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "modulation/params/param.h"
#include "modulation/params/param_set.h"

namespace deluge::gui::menu_item::eq {

int32_t EqGainParam::currentFreqParamValue() const {
	using namespace deluge::modulation;

	// Only the treble consults its frequency, so only the treble pays for the lookup.
	if (band_ != Band::TREBLE || soundEditor.currentParamManager == nullptr) {
		return 0;
	}
	return soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(params::UNPATCHED_TREBLE_FREQ);
}

float EqGainParam::db() {
	// getValue() is the 0-50 menu position, and computeFinalValueForStandardMenuItem() is the exact
	// conversion the write path uses. Reading this param back through the param manager instead
	// would lag by one encoder click, because the redraw happens before the write has landed. The
	// Freq param is a different story - nobody is turning it right now, so what is already stored
	// is current.
	const int32_t amount = computeFinalValueForStandardMenuItem(this->getValue());
	return dsp::eq::amountDb(band_, amount, currentFreqParamValue());
}

void EqGainParam::drawInteger(int32_t textWidth, int32_t textHeight, int32_t yPixel) {
	char buffer[16];
	formatGain(buffer, db());
	deluge::hid::display::OLED::main.drawStringCentred(buffer, yPixel + OLED_MAIN_TOPMOST_PIXEL, textWidth, textHeight);
}

void EqGainParam::drawValue() {
	// No unit and no '+': four characters, and "-6.4" already uses all of them.
	char buffer[16];
	formatGain(buffer, db(), false, false);
	display->setText(buffer, true);
}

} // namespace deluge::gui::menu_item::eq
