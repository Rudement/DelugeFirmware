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

/// Clock division: the rate a phrase is read at, as an exact musical ratio.
///
/// Beat Stretch already moves that rate, and this is the same read-time transform. What it cannot
/// do is say x3. Beat Stretch stores an integer percentage, so x3 has to be written 33%, and
/// 100/33 is 3.0303 - about five ticks of drift by the end of a 192-tick bar, which arrives at the
/// ear as the firmware being sloppy rather than as a number having been rounded. Under a name like
/// "/3" that is not acceptable, so divisions are stored as a ratio and never as a percentage.
///
/// Stored as a small signed number rather than as an index into the table below, so the meaning of
/// a saved value does not depend on the table's order: positive divides (2 is /2), negative
/// multiplies (-2 is x2), and 0, 1 and -1 all mean off. A division added later just works, in
/// songs written before it existed as well as after.
namespace deluge::model::clock_division {

/// The largest ratio either way. Bounds the load clamp and the position multiply.
constexpr int32_t kMaxDivision = 8;

/// Position is read at pos * num / den, so num > den plays the phrase faster.
struct Ratio {
	int32_t num;
	int32_t den;
};

[[nodiscard]] constexpr Ratio ratioFor(int32_t division) {
	if (division >= 2) {
		return {1, division};
	}
	if (division <= -2) {
		return {-division, 1};
	}
	return {1, 1};
}

[[nodiscard]] constexpr bool isOff(int32_t division) {
	return division > -2 && division < 2;
}

/// What the menu sweeps through, fastest first so the encoder turns the way the phrase speeds up.
/// Fives and sevens are left out deliberately: against a clip length that is a power of two they
/// never come back into phase, and a knob position that is always wrong is worse than one that is
/// missing.
constexpr int8_t kSteps[] = {-8, -6, -4, -3, -2, 0, 2, 3, 4, 6, 8};
constexpr int32_t kNumSteps = 11;
constexpr int32_t kOffStep = 5;

[[nodiscard]] constexpr int32_t stepForDivision(int32_t division) {
	for (int32_t i = 0; i < kNumSteps; i++) {
		if (kSteps[i] == division) {
			return i;
		}
	}
	return kOffStep;
}

[[nodiscard]] constexpr int8_t divisionForStep(int32_t step) {
	if (step < 0 || step >= kNumSteps) {
		return 0;
	}
	return kSteps[step];
}

} // namespace deluge::model::clock_division
