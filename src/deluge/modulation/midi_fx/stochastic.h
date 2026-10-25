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

/// Stochastic - OXI One style pitch variation on a written phrase.
///
/// **The pitch half of Spice.** Spice locks the pitch and varies the gate; this locks the rhythm
/// and varies the pitch. The clip plays the steps, lengths and accents you wrote, but some of the
/// notes land a scale degree or two away from where they are written - and which notes, and how
/// far, is a variation you can browse and keep. Nothing is written into the clip; Amount 0 is the
/// path the sequencer took before this existed, the same contract Spice, Strum, Harmonize and
/// Spread all keep.
///
/// **Degrees, not semitones.** The wander is an offset in degrees of the song's key, so the result
/// is in key by construction rather than by correction, and it follows the scale when the song
/// changes it. A version that drew semitones and then snapped would be in key only by accident,
/// and would bunch onto whichever pitches the snap happened to favour.
///
/// **Two controls, because "how often" and "how far" are different questions.** Amount is the
/// chance that a given note is redrawn at all; Width is how far it may go when it is. One combined
/// knob can only ever give a constant shimmer at low settings - it can never give "my part, mostly
/// as written, with the occasional departure", which is the setting people actually want.
///
/// **The variation index is Spice's Dice**, shared rather than duplicated. One number meaning
/// "which variation am I hearing" across the group is the better user model, and it costs a read
/// rather than a field - which matters, because MidiFxSettings is exactly at the 16-byte ceiling
/// its own test asserts. Amount and Width therefore live on the InstrumentClip, as Strum's,
/// Generate's and Spread's all do.
///
/// **Everything here is a hash, not a draw**, for the reasons spice.h sets out at length: the
/// variation repeats bar after bar, survives a stop/start and a save/load with nothing stored, and
/// comes out identical on both cores.
///
/// This header is deliberately free of everything in the firmware except spice.h, so that the
/// header test can assert its arithmetic without a linker. The one part that is not arithmetic -
/// looking a degree up in the song's scale - is left to the caller, which is why `wrappedDegree`,
/// `octaveCarry` and `assemble` are separate rather than one function. `pitch_source.cpp` is the
/// caller, and is where the key is read.
namespace deluge::modulation::midi_fx::stochastic {

/// Amount 0 is off, so a song saved before this existed plays back untouched.
constexpr uint8_t kOff = 0;

/// Amount is a percentage: how many of the written notes are redrawn. 100 means every note moves -
/// see `degreeOffset`, which never returns 0, so the knob means what it says at the top as well as
/// at the bottom.
constexpr uint8_t kAmountMax = 100;

/// Width in scale degrees, either side of the written note. Never 0: a width of nothing is what
/// Amount 0 already says, and two ways to spell "off" is one too many.
constexpr uint8_t kWidthMin = 1;

/// Seven degrees is an octave of a seven-note scale, which is as far as a wander can go before the
/// contour of the written phrase stops being audible underneath it.
constexpr uint8_t kWidthMax = 7;

/// A third either way: far enough to hear, near enough that the line is still recognisably yours.
constexpr uint8_t kWidthDefault = 2;

/// The bottom and top of the note range. A wander that would leave it is declined rather than
/// clamped - clamping piles notes onto one pitch, which is heard as a fault rather than as a part.
constexpr int32_t kMinNoteCode = 0;
constexpr int32_t kMaxNoteCode = 127;

constexpr int32_t kSemitonesPerOctave = 12;

/// One salt each, so "does this note move" and "how far" are independent. Sharing a roll would tie
/// them together, and every note that moved would move by the same amount as its neighbour.
constexpr uint32_t kSaltFire = 0x53544F43;   // 'STOC'
constexpr uint32_t kSaltDegree = 0x53444547; // 'SDEG'

/// Floor division and modulo. `harmonize.h` carries the same pair, and they are repeated here
/// rather than shared so that this header depends on nothing but spice.h - which is what lets the
/// header test compile it on the host. C's `/` and `%` round toward zero, which for a line written
/// below the key root would put two different notes on the same degree, audibly.
[[nodiscard]] constexpr int32_t floorDiv(int32_t a, int32_t b) {
	const int32_t q = a / b;
	return ((a % b != 0) && ((a < 0) != (b < 0))) ? q - 1 : q;
}

[[nodiscard]] constexpr int32_t floorMod(int32_t a, int32_t b) {
	const int32_t r = a % b;
	return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

/// Whether this note is redrawn this time round.
[[nodiscard]] constexpr bool fires(uint8_t amount, uint8_t dice, int32_t noteRowId, int32_t notePos) {
	if (amount == kOff) {
		return false;
	}
	return static_cast<int32_t>(spice::roll(dice, noteRowId, notePos, kSaltFire) % 100U) < static_cast<int32_t>(amount);
}

/// How far this note moves, in scale degrees. **Never 0.**
///
/// The obvious spelling - a flat draw over -width..+width - includes zero, so one fired note in
/// `2*width+1` would not actually move and Amount 100 would visibly shift only four notes in five.
/// Drawing over `2*width` values and folding the upper half up by one gives {-width..-1, +1..+width}
/// with every value equally likely, so a fired note always moves and Amount means what it says.
[[nodiscard]] constexpr int32_t degreeOffset(uint8_t width, uint8_t dice, int32_t noteRowId, int32_t notePos) {
	if (width < kWidthMin) {
		return 0;
	}
	const int32_t w = static_cast<int32_t>(width);
	const uint32_t drawn = spice::roll(dice, noteRowId, notePos, kSaltDegree) % (2U * static_cast<uint32_t>(width));
	const int32_t d = static_cast<int32_t>(drawn);
	return d < w ? d - w : d - w + 1;
}

/// Which degree of the scale the wandered note lands on, having wrapped into 0..scaleSize-1.
[[nodiscard]] constexpr int32_t wrappedDegree(int32_t degree, int32_t scaleSize) {
	return scaleSize > 0 ? floorMod(degree, scaleSize) : 0;
}

/// How many whole octaves the wander carried, when it ran off either end of the scale.
///
/// Kept apart from the degree for the reason `harmonize::fromDegree` spells out: a single running
/// degree counted across octaves and then divided does not come back out at the octave it went in
/// at, and a bassline disappears through the floor.
[[nodiscard]] constexpr int32_t octaveCarry(int32_t degree, int32_t scaleSize) {
	return scaleSize > 0 ? floorDiv(degree, scaleSize) : 0;
}

/// Puts the note back together. `semitoneInOctave` is the scale's own value for the target degree,
/// which the caller looks up because it is the one part of this that is not arithmetic.
///
/// At offset 0 this reconstructs the written note exactly, which is the property the whole feature
/// rests on: everything upstream of a wander that does not happen must be a no-op.
[[nodiscard]] constexpr int32_t assemble(int32_t rootNote, int32_t octave, int32_t carry, int32_t semitoneInOctave) {
	return rootNote + kSemitonesPerOctave * (octave + carry) + semitoneInOctave;
}

[[nodiscard]] constexpr bool inRange(int32_t note) {
	return note >= kMinNoteCode && note <= kMaxNoteCode;
}

/// The wandered note, or the written one when the wander would leave the keyboard.
///
/// **Declined, not clamped.** Clamping piles every out-of-range wander onto the lowest or highest
/// note, which is heard as a fault rather than as a part; declining means a bassline sitting on the
/// floor simply stops making its downward jumps. Split out as its own function so the header test
/// can assert the property across the whole note range, because it is close to impossible to hear
/// deliberately - ear test J, which nobody could run.
[[nodiscard]] constexpr int32_t applyOrDecline(int32_t written, int32_t wandered) {
	return inRange(wandered) ? wandered : written;
}

} // namespace deluge::modulation::midi_fx::stochastic
