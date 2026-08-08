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

#include "gui/menu_item/eq/eq_unpatched_param.h"
#include "util/d_stringbuf.h"

namespace deluge::gui::menu_item::eq {

/// The four EQ amount controls, reporting the gain the band lands on rather than a bare 0-50.
///
/// Same deal as EqFreqParam: notification popup only, no change to the param itself.
///
/// The three readouts are derived differently, and each for a reason spelled out in eq_bands.hpp:
///   Bass     exact, and needs nothing but the amount — a lowpass is exactly 1 at DC.
///   Treble   reads its own Freq param, because a digital lowpass is nowhere near 0 at Nyquist,
///            and because above Freq knob 32 the treble amount does nothing at all.
///   Bells    fixed capture at their neutral centre rather than tracking Freq, to stay monotonic
///            through the region where the amount law over-cuts into polarity inversion.
class EqGainParam final : public EqUnpatchedParam {
public:
	EqGainParam(l10n::String name, int32_t newP, Band band) : EqUnpatchedParam(name, newP), band_{band} {}

	void getNotificationValue(StringBuf& value) override;

private:
	Band band_;

	/// The band's Freq param as it currently stands, or 0 for the bands that don't consult it.
	[[nodiscard]] int32_t currentFreqParamValue() const;
};

} // namespace deluge::gui::menu_item::eq
