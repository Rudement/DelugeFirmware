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
#include "util/cfunctions.h"

#include <cmath>
#include <cstring>

/// Shared string formatting for the FX > EQ readouts.
///
/// The 1.3 line does this with StringBuf and a getNotificationValue() hook, neither of which exists
/// on 1.2.1. What 1.2.1 has instead is drawInteger() and drawValue(), both virtual, so the readout
/// is built here as a plain char buffer and handed to those. Same numbers, same rounding, same
/// shapes ("466Hz", "4.87kHz", "+12.0dB", "OFF") - only the plumbing differs.
///
/// Everything is formatted with integer arithmetic. Float printf pulls in a large chunk of newlib
/// and is not worth it for four menu items, so the fractional digits are split out by hand.
namespace deluge::gui::menu_item::eq {

/// "6Hz", "466Hz", "4.87kHz", "20.0kHz". Seven characters at worst, which matters: the popup
/// truncates the param name to make room for the value, and the longest name here is
/// "High mid freq".
inline void formatFrequency(char* buffer, float hz, bool withUnit = true) {
	if (hz < 1000.f) {
		intToString(static_cast<int32_t>(std::lround(hz)), buffer, 1);
		if (withUnit) {
			strcat(buffer, "Hz");
		}
		return;
	}

	// Two decimals below 10 kHz, one above, matching the 1.3 readout. Held in hundredths/tenths of
	// a kHz as integers so no float formatting is needed.
	const bool twoDecimals = hz < 10000.f;
	const int32_t scaled = static_cast<int32_t>(std::lround(hz / (twoDecimals ? 10.f : 100.f)));
	const int32_t decimals = twoDecimals ? 2 : 1;
	const int32_t divisor = twoDecimals ? 100 : 10;

	char* write = buffer;
	intToString(scaled / divisor, write, 1);
	write += strlen(write);
	*write++ = '.';
	intToString(scaled % divisor, write, decimals);
	write += strlen(write);
	if (withUnit) {
		strcpy(write, "kHz");
	}
}

/// "+12.0dB", "-6.4dB", "0.0dB", "OFF". Seven characters at worst, matching the Hz readout so
/// neither truncates the param name any further than the other.
///
/// `withSign` is off for the 7SEG, which has four characters and no useful '+' glyph.
inline void formatGain(char* buffer, float db, bool withUnit = true, bool withSign = true) {
	if (db <= dsp::eq::kMinDisplayDb) {
		strcpy(buffer, "OFF");
		return;
	}

	// Round before testing the sign, so the detent reads a clean "0.0dB" rather than picking up a
	// '-' from the half-LSB the centre menu position actually sits below unity.
	int32_t tenths = static_cast<int32_t>(std::lround(db * 10.f));

	char* write = buffer;
	if (tenths < 0) {
		*write++ = '-';
		tenths = -tenths;
	}
	else if (withSign && tenths > 0) {
		*write++ = '+';
	}

	intToString(tenths / 10, write, 1);
	write += strlen(write);
	*write++ = '.';
	intToString(tenths % 10, write, 1);
	write += strlen(write);
	if (withUnit) {
		strcpy(write, "dB");
	}
}

} // namespace deluge::gui::menu_item::eq
