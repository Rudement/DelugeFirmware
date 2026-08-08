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

namespace deluge::gui::menu_item::eq {

using deluge::dsp::eq::Band;

/// The four EQ amount controls, reporting gain in dB rather than a bare 0-50.
class EqGainParam final : public UnpatchedParam {
public:
	EqGainParam(l10n::String newName, int32_t newP, Band band) : UnpatchedParam(newName, newP), band_{band} {}
	EqGainParam(l10n::String newName, l10n::String title, int32_t newP, Band band)
	    : UnpatchedParam(newName, title, newP), band_{band} {}

protected:
	void drawInteger(int32_t textWidth, int32_t textHeight, int32_t yPixel) override;

	// 7SEG
	void drawValue() override;

private:
	Band band_;

	/// Only the treble consults its frequency, so only the treble pays for the lookup. See
	/// trebleGain() in eq_bands.hpp for why it is the one band that has to.
	[[nodiscard]] int32_t currentFreqParamValue() const;

	[[nodiscard]] float db();
};

} // namespace deluge::gui::menu_item::eq
