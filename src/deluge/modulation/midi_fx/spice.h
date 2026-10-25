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

/// Spice & Dice - MicroFreak-style gate variation on a written phrase.
///
/// Spice is an amount: at 0 the clip plays exactly as written, and the further up it goes the more
/// the gates wander off the grid. Dice is which variation you are hearing: change it and you get a
/// different one, change it back and the first one returns.
///
/// **Every decision here is a hash, not a draw.** Whether a step drops and how long its gate runs
/// are pure functions of (dice, note row, step position), so the variation repeats bar after bar,
/// survives a clip relaunch and a stop/start, and comes out identical on both cores. Nothing is
/// stored per note, nothing is written into the NoteVector, and there is no RNG state to keep in
/// step. Using `getRandom255()` here would have given a different answer every pass - that is what
/// the Deluge's own note probability does, and it is explicitly not this. Arturia's Dice rolls a
/// variation; the variation then *repeats* until it is re-rolled.
///
/// **v1 only ever subtracts.** Steps drop out and gates get shorter. It never revives a silent step
/// and never holds a note past its written end. Both of those need the sequencer to wake on a grid
/// rather than on note events, and to own note-offs for notes the clip does not contain - see
/// SPICE-DICE-FEASIBILITY.md §2, which is also where the CPU risk lives. Shortening needs neither:
/// the note-off tick is recomputed from the stored length on every pass, so moving it is one
/// expression and costs nothing.
namespace deluge::modulation::midi_fx::spice {

/// Spice off. The sequencer takes exactly the path it took before this feature existed.
constexpr uint8_t kAmountOff = 0;
constexpr uint8_t kAmountMax = 50;

/// Variations. An index rather than a seed that gets drawn: the same number is always the same
/// variation, so a bar you liked is one turn back rather than gone.
constexpr uint8_t kDiceMin = 0;
constexpr uint8_t kDiceMax = 99;

/// How often a step drops at full Spice, in percent. Not 100, and not close to it: a phrase with
/// every step missing is silence rather than a variation, and by ear the useful range runs out
/// well before half the notes are gone.
constexpr int32_t kMaxDropPercent = 55;

/// The shortest a spiced gate gets, as a percentage of the written length. Floored rather than
/// allowed to reach zero, because a zero-length gate is a note that never sounds - which is the
/// drop, and the drop is already its own separate decision.
constexpr int32_t kMinGatePercent = 30;

/// Salts, so the drop decision and the gate decision for one step are independent of each other.
/// Sharing a roll between them would tie the two together and every surviving note would come out
/// the same length.
constexpr uint32_t kSaltDrop = 0x44524F50; // 'DROP'
constexpr uint32_t kSaltGate = 0x47415445; // 'GATE'

/// Murmur-style finaliser. Single-digit cycles, and good enough that neighbouring steps do not
/// correlate - which is what stops a spiced row from thinning out in visible stripes.
[[nodiscard]] constexpr uint32_t mix32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

/// The roll for one step of one row.
///
/// `noteRowId` is in the hash so that the rows of a kit do not drop in lockstep. Without it every
/// row makes the same decision at the same step, the whole kit stutters as one, and it reads as a
/// dropout rather than as a variation.
[[nodiscard]] constexpr uint32_t roll(uint8_t dice, int32_t noteRowId, int32_t notePos, uint32_t salt) {
	uint32_t h = static_cast<uint32_t>(dice) * 0x9E3779B1U;
	h ^= static_cast<uint32_t>(noteRowId) * 0x85EBCA6BU;
	h ^= static_cast<uint32_t>(notePos) * 0xC2B2AE35U;
	h ^= salt;
	return mix32(h);
}

/// Whether this step is silenced this time round.
[[nodiscard]] constexpr bool dropsStep(uint8_t amount, uint8_t dice, int32_t noteRowId, int32_t notePos) {
	if (amount == kAmountOff) {
		return false;
	}
	const int32_t chance = static_cast<int32_t>(amount) * kMaxDropPercent / kAmountMax;
	return static_cast<int32_t>(roll(dice, noteRowId, notePos, kSaltDrop) % 100U) < chance;
}

/// What this step's gate becomes, as a percentage of what is written. Never above 100 - see the
/// header note on why v1 shortens only.
[[nodiscard]] constexpr int32_t gatePercent(uint8_t amount, uint8_t dice, int32_t noteRowId, int32_t notePos) {
	if (amount == kAmountOff) {
		return 100;
	}
	// How far below 100 this amount of Spice is allowed to pull a gate, then where inside that span
	// this particular step lands. Two steps so that low Spice is a nudge rather than a coin toss
	// between "written" and "as short as it gets".
	const int32_t span = (100 - kMinGatePercent) * static_cast<int32_t>(amount) / kAmountMax;
	const int32_t cut =
	    static_cast<int32_t>(roll(dice, noteRowId, notePos, kSaltGate) % static_cast<uint32_t>(span + 1));
	return 100 - cut;
}

/// The spiced length of a note, in ticks.
///
/// Floored at 1, and a one-tick note is left alone: a length that rounds to 0 ends on the tick it
/// began, which the sequencer reads as "already over" rather than "very short", and that is how a
/// row ends up silent for reasons nobody can find.
[[nodiscard]] constexpr int32_t gateLength(int32_t writtenLength, uint8_t amount, uint8_t dice, int32_t noteRowId,
                                           int32_t notePos) {
	if (amount == kAmountOff || writtenLength <= 1) {
		return writtenLength;
	}
	const int32_t scaled = writtenLength * gatePercent(amount, dice, noteRowId, notePos) / 100;
	return scaled < 1 ? 1 : scaled;
}

} // namespace deluge::modulation::midi_fx::spice
