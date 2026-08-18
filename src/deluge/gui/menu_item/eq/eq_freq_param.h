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

#pragma once

#include "dsp/eq_bands.hpp"
#include "gui/menu_item/eq/eq_unpatched_param.h"
#include "gui/menu_item/value_scaling.h"
#include "util/d_stringbuf.h"

#include <cmath>
#include "model/settings/runtime_feature_settings.h"

namespace deluge::gui::menu_item::eq {

/// The four EQ frequency controls, reporting where the band actually sits rather than a bare 0-50.
///
/// Only the notification popup changes — the one raised while you turn the select encoder in
/// FX > EQ, which is where the number gets read in practice. The param's range, its automation and
/// its stored value are all untouched; this is a relabelling and nothing more.
class EqFreqParam final : public EqUnpatchedParam {
public:
	EqFreqParam(l10n::String name, l10n::String columnLabel, int32_t newP, Band band)
	    : EqUnpatchedParam(name, columnLabel, newP), band_{band} {}

	/// The mid bands are the Rudement addition; bass and treble are stock and always stay. Hiding the
	/// menu does not touch the DSP, so a song using the mids still sounds the same with them hidden.
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		if ((band_ == Band::LOW_MID || band_ == Band::HIGH_MID)
		    && !runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::FourBandEq)) {
			return false;
		}
		return EqUnpatchedParam::isRelevant(modControllable, whichThing);
	}


	void getNotificationValue(StringBuf& value) override {
		appendFrequency(value, dsp::eq::freqHz(band_, currentParamValue()));
	}

private:
	Band band_;

	/// The q31 the param will hold once writeCurrentValue() runs, which is what the DSP reads.
	///
	/// getValue() is the 0-50 menu position and computeFinalValueForStandardMenuItem() is the exact
	/// conversion the write path uses. Reading back through the param manager instead would lag by
	/// one encoder click, because the notification is raised from selectEncoderAction() before the
	/// write has landed.
	[[nodiscard]] int32_t currentParamValue() { return computeFinalValueForStandardMenuItem(this->getValue()); }

	/// "6Hz", "466Hz", "4.87kHz", "20.0kHz". Seven characters at worst, which matters: the
	/// notification popup truncates the param name to make room for the value, and the longest
	/// name in this menu is "High mid freq".
	static void appendFrequency(StringBuf& value, float hz) {
		if (hz < 1000.f) {
			value.appendInt(static_cast<int32_t>(std::lround(hz)));
			value.append("Hz");
			return;
		}
		const float kHz = hz / 1000.f;
		value.appendFloat(kHz, 1, kHz < 10.f ? 2 : 1);
		value.append("kHz");
	}
};

} // namespace deluge::gui::menu_item::eq
