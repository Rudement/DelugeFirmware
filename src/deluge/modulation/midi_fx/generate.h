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

#include "modulation/midi_fx/spice.h"
#include <cstdint>

/// Generate - the sequencer writes a part for you, inside constraints you set.
///
/// **The one thing here that is not like the rest of this folder: it writes.** Velocity, Gate,
/// Delay, Retrigger, Spice, Strum and Harmonize are all read-time transforms that leave the
/// NoteVector alone, and turning them off gives you back exactly what you wrote. This does not.
/// It replaces the row's notes, it goes through the action log like any other edit, and undo is
/// how you get back. That is the whole point of a generator: you want to keep editing what it
/// wrote.
///
/// **Rhythm and line are two separate things here, because on this machine they have to be.** A
/// melodic NoteRow *is* one pitch - `NoteRow::getNoteCode()` returns the row's own `y` - so notes
/// written into one row can only ever be a rhythm at one pitch. A melody means writing across rows,
/// creating the ones that do not exist yet, and logging the lot as a single action so that one undo
/// takes the whole line back out. See GENERATIVE-FEASIBILITY.md §2.2.
///
/// So Density, Seed and Gate below decide *when* notes happen, and Range and Leap decide *what pitch*
/// each one takes. They are independent on purpose: turn Range up and the tune appears over the
/// groove you already had; turn it back to 0 and the groove is still there, unchanged.
///
/// **The seed is an index, not a drawn seed**, exactly as Spice's Dice is, and for the same
/// reason: a generator whose knob draws fresh randomness is a slot machine - you find a pattern
/// you like, turn the knob, and it is gone. Seed 7 is always the same pattern on any build, so
/// turning to 8 and back to 7 is a round trip. Nothing is stored per note; the pattern is
/// recomputed from the hash whenever it is asked for.
namespace deluge::modulation::midi_fx::generate {

/// How likely each step is to carry a note, in percent. 0 generates nothing at all, and the
/// command refuses rather than silently emptying the row - clearing a row is what CLEAR is for.
constexpr uint8_t kDensityMin = 0;
constexpr uint8_t kDensityMax = 100;
constexpr uint8_t kDensityDefault = 30;

/// Which pattern. A plain index, as above.
constexpr uint8_t kSeedMin = 0;
constexpr uint8_t kSeedMax = 99;

/// Note length as a percentage of one step. Over 100 the note runs into the next step, so it is
/// clamped at write time to stop where the next note starts - two overlapping notes in one row is
/// not a state the NoteVector is allowed to be in.
constexpr uint16_t kGateMin = 5;
constexpr uint16_t kGateMax = 200;
constexpr uint16_t kGateDefault = 100;

/// Distinct from Spice's salts so that a row with both never correlates - a generated pattern
/// whose notes all survived spicing, or all dropped, would look like one feature failing rather
/// than two features stacking.
constexpr uint32_t kSaltPlace = 0x47454E52; // 'GENR'

/// The roll for one candidate step, 0..99.
///
/// **Keyed on tick position, not step index.** That way the decision for a given point in the bar
/// is the same whatever the zoom level was when you generated: zooming in offers new candidate
/// steps *between* the ones already there rather than reshuffling the lot, so the pattern refines
/// instead of changing. Keying on step index would make the same seed mean a different pattern at
/// every zoom, which is the kind of thing that reads as the feature being broken.
[[nodiscard]] constexpr int32_t stepRoll(uint8_t seed, int32_t noteRowId, int32_t notePos) {
	return static_cast<int32_t>(spice::roll(seed, noteRowId, notePos, kSaltPlace) % 100U);
}

/// Whether a note lands on this step.
[[nodiscard]] constexpr bool placesStep(uint8_t density, uint8_t seed, int32_t noteRowId, int32_t notePos) {
	if (density == kDensityMin) {
		return false;
	}
	return stepRoll(seed, noteRowId, notePos) < static_cast<int32_t>(density);
}

// --- The line: which pitch each of those notes takes ---
//
// Everything above decides *when*. Everything below decides *what*, as a walk in scale degrees
// above the row you auditioned. The degree is turned into a note code by the caller, because that
// needs the song's key and this header is deliberately free of the firmware.
//
// **Range 0 is the whole of the old behaviour**, not a special case bolted on: the walk returns
// degree 0 every time, every note lands in the one row, and a Kit - where a row is a drum and not a
// pitch - is simply held at 0. So a drum pattern generates exactly as it did before this existed.

/// How far above the auditioned row the line may climb, in scale degrees. 24 is about three and a
/// half octaves of a seven-note scale, which is more than a melody wants and less than the note
/// range can refuse.
constexpr uint8_t kRangeMin = 0;
constexpr uint8_t kRangeMax = 24;
/// 0, so that a build carrying this change generates what the build before it generated until the
/// knob is moved - and so Kits and drum rows need no special handling in the menu.
constexpr uint8_t kRangeDefault = 0;

/// How often the line jumps rather than steps to a neighbouring degree, in percent. Of everything
/// here this is the control that most decides whether the result sounds like a melody or like
/// noise: near 0 it is a scale run, near 100 it is scattered arpeggio.
constexpr uint8_t kLeapMin = 0;
constexpr uint8_t kLeapMax = 100;
constexpr uint8_t kLeapDefault = 25;

constexpr uint32_t kSaltLeap = 0x4C454150; // 'LEAP'
constexpr uint32_t kSaltStep = 0x53544550; // 'STEP'

/// Folds a degree back inside the range by reflecting off each end rather than clamping to it.
///
/// Clamping is the obvious thing and it is audibly wrong: a line that clamps piles up on the top
/// note, repeating it for as long as the walk keeps pushing upward, and that reads as the generator
/// having got stuck. Reflection turns the same push into a descent, which is what a melody does when
/// it reaches the top of its range.
[[nodiscard]] constexpr int32_t reflectDegree(int32_t degree, uint8_t range) {
	const int32_t top = static_cast<int32_t>(range);
	if (top <= 0) {
		return 0;
	}
	// A single leap can be wider than the range and overshoot both ends, so this bounces until it
	// lands. The range caps the number of turns, so it always terminates.
	while (degree < 0 || degree > top) {
		if (degree < 0) {
			degree = -degree;
		}
		if (degree > top) {
			degree = 2 * top - degree;
		}
	}
	return degree;
}

/// How far the line moves at note number `noteIndex`, in degrees. Signed; 0 is never returned, so
/// the line always goes somewhere.
///
/// Keyed on the note's index in the line rather than on its position in the bar, unlike the
/// placement roll above. The walk is a sequence - each degree depends on the one before it - so the
/// nth note has to make the same decision wherever the density happened to put it, or thinning the
/// rhythm would rewrite the tune.
[[nodiscard]] constexpr int32_t degreeDelta(uint8_t range, uint8_t leap, uint8_t seed, int32_t noteIndex) {
	if (range == kRangeMin) {
		return 0;
	}
	const bool leaps =
	    static_cast<int32_t>(spice::roll(seed, noteIndex, 0, kSaltLeap) % 100U) < static_cast<int32_t>(leap);
	const uint32_t m = spice::roll(seed, noteIndex, 1, kSaltStep);
	const int32_t sign = ((m & 1U) != 0U) ? 1 : -1;
	if (!leaps) {
		return sign;
	}
	// Leaps run from 2 up to the range. Drawn flat rather than weighted towards small intervals:
	// the Leap control is already the weighting, and weighting twice makes its top half do nothing.
	const int32_t span = (range < 2) ? 1 : (static_cast<int32_t>(range) - 1);
	return sign * (2 + static_cast<int32_t>((m >> 1) % static_cast<uint32_t>(span)));
}

/// The next degree in the line, given the one before it.
///
/// The line starts at degree 0 - the row you auditioned - so the tune begins where you put it, and
/// the caller carries the degree in a running variable as it walks the grid. That matters for cost:
/// the caller walks the whole grid once per target row, and a `degreeAt(n)` that recomputed the walk
/// from note 0 each time would make the command quadratic in note count on the UI thread. Carrying
/// it is still exactly reproducible, because every walk starts at 0 and visits the same firing steps
/// in the same order.
[[nodiscard]] constexpr int32_t nextDegree(int32_t degree, uint8_t range, uint8_t leap, uint8_t seed,
                                           int32_t noteIndex) {
	return reflectDegree(degree + degreeDelta(range, leap, seed, noteIndex), range);
}

} // namespace deluge::modulation::midi_fx::generate
