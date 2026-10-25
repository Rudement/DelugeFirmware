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

/// Strum - a chord stops landing as a block and lands as a sweep.
///
/// Every pad-drawn chord on this machine is perfectly simultaneous, which is the one thing no
/// played instrument ever is. Amount is the gap between successive notes in ticks; Direction is
/// which end of the chord goes first; Contour tilts the velocities across the sweep, which is what
/// separates "spread out in time" from "strummed".
///
/// **Nothing in here knows about the sequencer.** The arithmetic is pure so that it can be checked
/// on a host compiler without hardware - the same property `spice.h` has, and the reason
/// Harmonize's octave bug was caught before it ever compiled. The deferral itself, which is the
/// part with a lifetime, lives in `InstrumentClip::processCurrentPos`; see
/// STRUM-FEASIBILITY.md §3a.
namespace deluge::modulation::midi_fx::strum {

/// Strum off. The sequencer takes exactly the path it took before this feature existed, so every
/// song written before this plays back identically.
constexpr uint8_t kAmountOff = 0;

/// Ticks between successive notes. 24 is half a quarter note at the Deluge's 48 ticks per quarter,
/// which at 120 bpm is a ~240 ms sweep across a four-note chord - a slow harp roll, and further
/// than anything musical needs. The bottom of the range matters more: 1 tick is ~10 ms at 120 bpm,
/// a credible flam.
constexpr uint8_t kAmountMax = 24;

/// How many notes of one chord can be spread. Beyond this the overflow plays on the beat rather
/// than being dropped - a note that arrives unstrummed is a far smaller fault than a missing one.
constexpr int32_t kMaxStrumNotes = 16;

/// Which end of the chord goes first.
///
/// Alternate (up, then down on the next chord) and Inward/Outward are phase 2: each needs a bool
/// or two of state that persists between chords, which is a different thing from this file, where
/// every answer is a function of the chord in hand.
enum class Direction : uint8_t {
	UP = 0,     ///< Lowest note first, the way a hand crosses the strings on a downstroke.
	DOWN = 1,   ///< Highest first.
	RANDOM = 2, ///< A fresh shuffle per chord. See `buildOrder` on why this one takes a seed.
};

constexpr uint8_t kDirectionMin = 0;
constexpr uint8_t kDirectionMax = 2;

/// Velocity tilt across the sweep, in percent, applied to the LAST note of the chord.
///
/// The first note always keeps its written velocity and the rest are interpolated to this, so a
/// three-note chord and a six-note chord end up in the same place rather than the long one fading
/// to nothing. Negative is a hand losing force through the stroke, positive is a hand gaining it.
constexpr int8_t kContourOff = 0;
constexpr int8_t kContourMin = -50;
constexpr int8_t kContourMax = 50;

/// MIDI velocity floor. 0 is a note-off on the wire, so a contour steep enough to reach it would
/// silence the end of the chord in a way that reads as a fault rather than as a fade.
constexpr int32_t kMinVelocity = 1;
constexpr int32_t kMaxVelocity = 127;

/// Small LCG, used only to shuffle. Numerical Recipes' constants - a full-period generator on
/// 2^32, which is all a Fisher-Yates over at most 16 elements asks of it.
[[nodiscard]] constexpr uint32_t nextRandom(uint32_t& state) {
	state = state * 1664525U + 1013904223U;
	return state;
}

/// Fills `order[0..count)` with each note's place in the sweep, indexed by the note's rank in
/// ascending order. `order[i] == 0` means the note with ascending rank `i` plays first.
///
/// Up and Down are the identity and its reverse. Random is a genuine shuffle rather than a hash of
/// the chord, and that is deliberate: Spice's variations must repeat bar after bar because they are
/// a *pattern*, while a random strum is a performance gesture and a hand does not cross the strings
/// the same way twice. The seed is passed in rather than drawn here so this function stays pure and
/// testable; the sequencer hands it `getRandom255()`.
constexpr void buildOrder(Direction direction, int32_t count, uint32_t seed, uint8_t* order) {
	if (count <= 0) {
		return;
	}
	for (int32_t i = 0; i < count; i++) {
		order[i] = static_cast<uint8_t>(direction == Direction::DOWN ? (count - 1 - i) : i);
	}
	if (direction != Direction::RANDOM) {
		return;
	}
	// Fisher-Yates, downwards, so every permutation is equally likely. A shuffle of the *places*
	// rather than of the notes, which is why it can run over `order` in place.
	uint32_t state = seed | 1U;
	for (int32_t i = count - 1; i > 0; i--) {
		const int32_t j = static_cast<int32_t>(nextRandom(state) % static_cast<uint32_t>(i + 1));
		const uint8_t swap = order[i];
		order[i] = order[j];
		order[j] = swap;
	}
}

/// Ticks this note waits before it sounds. The note that plays first waits nothing, so a strummed
/// chord still starts exactly on the beat - the chord spreads *after* the downbeat rather than
/// around it, which is what keeps a strummed clip in time with everything else.
[[nodiscard]] constexpr int32_t offsetForPlace(uint8_t amount, int32_t place) {
	if (amount == kAmountOff || place <= 0) {
		return 0;
	}
	return static_cast<int32_t>(amount) * place;
}

/// The offset actually used, once the note's own length has had its say.
///
/// **A note must never be delayed past its own end.** Its note-off is computed from the written
/// length, and an offset longer than that would fall due before the note-on fired - a note-off for
/// a note that is not sounding, followed by a note-on with nothing left to stop it. Clamped to one
/// tick short of the length, so the shortest a strummed note can get is a single tick.
[[nodiscard]] constexpr int32_t clampedOffset(int32_t offset, int32_t noteLength) {
	if (offset <= 0) {
		return 0;
	}
	if (noteLength <= 1) {
		return 0;
	}
	return offset < (noteLength - 1) ? offset : (noteLength - 1);
}

/// This note's velocity once the contour has tilted it.
///
/// `place` is where the note falls in the sweep, `count` how many notes are in the chord. A chord
/// of one is exempt: a contour across a single note has nowhere to go, and dividing by count-1
/// would be a division by zero.
[[nodiscard]] constexpr uint8_t contouredVelocity(uint8_t velocity, int8_t contour, int32_t place, int32_t count) {
	if (contour == kContourOff || count <= 1 || place <= 0) {
		return velocity;
	}
	const int32_t percent = 100 + (static_cast<int32_t>(contour) * place) / (count - 1);
	int32_t scaled = (static_cast<int32_t>(velocity) * percent) / 100;
	if (scaled < kMinVelocity) {
		scaled = kMinVelocity;
	}
	else if (scaled > kMaxVelocity) {
		scaled = kMaxVelocity;
	}
	return static_cast<uint8_t>(scaled);
}

} // namespace deluge::modulation::midi_fx::strum
