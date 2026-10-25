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

#include <cstdint>

/// Turning a ratio control into a short sweep with a neutral middle.
///
/// Gate Time and Beat Stretch are both "×0.25 to ×4" controls. Exposed as a raw percentage they
/// are 375 knob positions of which the useful ones are bunched at both ends, and the readout lies
/// about its own resolution: 101% of a sixteenth note is the same number of ticks as 100%, so the
/// first several positions do nothing at all and the effect only opens up somewhere past 200.
///
/// So the control is steps, not percent. One step is a semitone's worth of ratio - 2^(1/12) - so
/// twelve steps double the value and twenty-four quadruple it, and zero is neutral. The readout
/// still shows the percentage it lands on, because that is the number that means something
/// musically; only the encoder travel changed.
namespace deluge::gui::menu_item::midi_fx {

constexpr int32_t kRatioStepMin = -24;
constexpr int32_t kRatioStepMax = 24;
constexpr int32_t kRatioNeutralPercent = 100;

/// 2^(n/12), times 1000. Integer arithmetic all the way: this runs from menu code, but there is no
/// reason to drag float into a header the audio side includes.
constexpr int32_t kSemitoneRatioX1000[12] = {1000, 1059, 1122, 1189, 1260, 1335, 1414, 1498, 1587, 1682, 1782, 1888};

[[nodiscard]] constexpr int32_t ratioStepToPercent(int32_t step) {
	if (step == 0) {
		return kRatioNeutralPercent;
	}
	const bool below = step < 0;
	const int32_t magnitude = below ? -step : step;
	const int32_t octaves = magnitude / 12;
	const int32_t semitones = magnitude % 12;
	const int32_t ratioX1000 = kSemitoneRatioX1000[semitones] << octaves;

	// Rounded, not truncated. Truncation would make the two halves of the sweep asymmetric - +12
	// would land on 200% while -12 landed on 49%.
	if (below) {
		return (kRatioNeutralPercent * 1000 + ratioX1000 / 2) / ratioX1000;
	}
	return (kRatioNeutralPercent * ratioX1000 + 500) / 1000;
}

/// The nearest step to a stored percentage, for reading a value back out of a settings object.
///
/// A scan rather than a logarithm: fifty candidates in menu code costs nothing, and it cannot
/// disagree with ratioStepToPercent the way a second formula could.
[[nodiscard]] constexpr int32_t percentToRatioStep(int32_t percent) {
	int32_t best = 0;
	int32_t bestDistance = INT32_MAX;
	for (int32_t step = kRatioStepMin; step <= kRatioStepMax; step++) {
		const int32_t candidate = ratioStepToPercent(step);
		const int32_t distance = candidate > percent ? candidate - percent : percent - candidate;
		if (distance < bestDistance) {
			bestDistance = distance;
			best = step;
		}
	}
	return best;
}

/// What the Stretch readout says at each step, fastest first.
///
/// The stored value is a duration percentage - 200% means every step takes twice as long - but
/// the readout is a *speed*, because that is the direction Division reads in, and two controls on
/// one page counting opposite ways is a trap rather than a preference. So a stored 200% shows
/// "/2", which is what Division calls the same thing.
///
/// A fraction appears only where the stored percentage is exactly that fraction's reciprocal.
/// Ten of the 49 positions are: x4, 10/3, 5/2, x2, 4/3, 1, 2/3, /2, /3, /4. Everything else
/// reads as a rounded speed percentage, and that is the point - "133%" is honest about being a
/// rounded number in a way "~3/4" is not, because a fraction invites you to trust it exactly.
/// 3/4 and 3/2 are absent from the landmarks for exactly that reason: no step lands on them.
///
/// Baked rather than computed: finding the nearest rational is a search, it would run on every
/// redraw of the menu, and a table can be read and checked by eye. Four characters at most, so
/// one table serves OLED and 7SEG. Index is step + kRatioStepMax.
constexpr const char* kRatioStepLabels[] = {
    "x4", "385%", "357%", "10/3", "313%", "303%", "286%",
    "270%", "5/2", "238%", "222%", "213%", "x2", "189%",
    "179%", "169%", "159%", "149%", "141%", "4/3", "127%",
    "119%", "112%", "106%", "1", "94%", "89%", "84%",
    "79%", "75%", "71%", "2/3", "63%", "60%", "56%",
    "53%", "/2", "47%", "45%", "42%", "40%", "37%",
    "35%", "/3", "32%", "30%", "28%", "26%", "/4",
};
constexpr int32_t kNumRatioStepLabels = 49;
static_assert(kNumRatioStepLabels == kRatioStepMax - kRatioStepMin + 1);

/// The steps run -24..+24; a Selection menu counts 0..48. These two are that offset, named so
/// neither direction is written out by hand at a call site.
[[nodiscard]] constexpr int32_t ratioStepToSelection(int32_t step) {
	return step + kRatioStepMax;
}
[[nodiscard]] constexpr int32_t selectionToRatioStep(int32_t selection) {
	return selection - kRatioStepMax;
}

} // namespace deluge::gui::menu_item::midi_fx
