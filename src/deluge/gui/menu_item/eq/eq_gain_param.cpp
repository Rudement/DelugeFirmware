/*
 * Copyright (c) 2026 Rudement
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
#include "modulation/params/param.h"
#include "modulation/params/param_set.h"

#include <cmath>

namespace deluge::gui::menu_item::eq {

namespace {

/// "+12.0dB", "-6.4dB", "0.0dB", "OFF". Seven characters at worst, matching the Hz readout, so
/// neither one truncates the param name in the notification popup any further than the other.
void appendGain(StringBuf& value, float db) {
	if (db <= dsp::eq::kMinDisplayDb) {
		value.append("OFF");
		return;
	}
	// Round before testing the sign, so the detent reads a clean "0.0dB" rather than picking up a
	// "-" from the half-LSB the centre menu position actually sits below unity.
	const float rounded = std::round(db * 10.f) / 10.f;
	if (rounded > 0.f) {
		value.append('+');
	}
	value.appendFloat(rounded == 0.f ? 0.f : rounded, 1, 1);
	value.append("dB");
}

} // namespace

int32_t EqGainParam::currentFreqParamValue() const {
	using namespace deluge::modulation;

	// Only the treble consults its frequency, so only the treble pays for the lookup.
	if (band_ != Band::TREBLE || soundEditor.currentParamManager == nullptr) {
		return 0;
	}
	return soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(params::UNPATCHED_TREBLE_FREQ);
}

void EqGainParam::getNotificationValue(StringBuf& value) {
	// getValue() is the 0-50 menu position, and computeFinalValueForStandardMenuItem() is the exact
	// conversion the write path uses. Reading this param back through the param manager instead
	// would lag by one encoder click, because the notification is raised from selectEncoderAction()
	// before the write has landed. The Freq param is a different story — nobody is turning it right
	// now, so what is already stored is current.
	const int32_t amount = computeFinalValueForStandardMenuItem(this->getValue());
	appendGain(value, dsp::eq::amountDb(band_, amount, currentFreqParamValue()));
}

} // namespace deluge::gui::menu_item::eq
