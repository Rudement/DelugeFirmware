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

/// Spread - velocity, gate and octave variation on notes the sequencer already holds.
///
/// Three knobs that stop a written part being mechanically identical every time it plays. Nothing
/// is written into the clip: turn them back to zero and the phrase is exactly what you wrote, the
/// same contract Spice, Strum and Harmonize keep.
///
/// **The arpeggiator's Randomizer page carries these three names already, but with the arp off it
/// covers only part of them.** Its parameters live in `ArpeggiatorSettings`; with the arp off a note
/// still passes through the arp's off-branch, which applies Note probability and Velocity spread but
/// not Gate or Octave - those run only in the arp's on-path (`switchNoteOn`). So on a written part
/// with no arp this page is the only source of gate and octave variation, and its Velocity overlaps
/// the Randomizer's (a different curve: absolute steps here, a scaled spread there). Reusing the
/// names is deliberate - a user who knows that page knows this one - with one difference the manual
/// states: there, Spread Octave is a range; here Octave is a *chance*.
///
/// **Two of the three are spreads and one is a probability, and that is not sloppiness.** Velocity
/// and gate are continuous, so "a bit more, a bit less" means something and every note should
/// differ slightly. An octave is quantised and coarse: at +/-1 there is no small amount, a note is
/// displaced or it is not. See SPREAD-FEASIBILITY.md section 11.
///
/// **No Dice, unlike Spice.** Spice makes patterns, and browsing different rolls of a pattern is
/// the point of it. This is an amount - nobody auditions variations of humanisation - so the roll
/// is a plain hash of (row, position, salt) with no variation index. It repeats bar after bar,
/// survives stop/start and save/load with nothing stored, and comes out identical on both cores.
namespace deluge::modulation::midi_fx::spread {

/// Every control is off at 0, so a song saved before this existed plays back untouched.
constexpr uint8_t kOff = 0;

/// Velocity deviation in absolute velocity steps, either side of what is written. Absolute rather
/// than proportional: predictable to dial, and on a part with written accents the loud and quiet
/// notes wander by the same amount rather than in proportion to themselves.
constexpr uint8_t kVelocityMax = 64;

/// Gate deviation as a percentage of the written length, either side of it. 90 rather than 100
/// because a note that can reach 0% of its length is a note that can vanish, and the floor below
/// would be doing all the work at the bottom of the knob.
constexpr uint8_t kGateMax = 90;

/// How often a note is thrown an octave, in percent. A probability, not a range - see the header
/// note above.
constexpr uint8_t kOctaveMax = 100;

/// Velocity 0 is a note-off on MIDI, so a spread that reached it would be a hanging note wearing a
/// disguise. VelocityFx floors at 1 for the same reason.
constexpr int32_t kMinVelocity = 1;
constexpr int32_t kMaxVelocity = 127;

/// The bottom and top of the note range. A displacement that would leave it is not applied at all -
/// clamping would pile notes onto one pitch, which sounds like a fault, and dropping would lose a
/// note the phrase expects.
constexpr int32_t kMinNoteCode = 0;
constexpr int32_t kMaxNoteCode = 127;

constexpr int32_t kSemitonesPerOctave = 12;

/// One salt each, so the three decisions for one note are independent. Sharing a roll would tie
/// them together and every loud note would also be a long one.
constexpr uint32_t kSaltVelocity = 0x5356454C;  // 'SVEL'
constexpr uint32_t kSaltGate = 0x53474154;      // 'SGAT'
constexpr uint32_t kSaltOctave = 0x534F4354;    // 'SOCT'
constexpr uint32_t kSaltOctaveDir = 0x534F4452; // 'SODR'

/// A signed deviation in -amount..+amount, flat across that span.
///
/// `spice::roll` is reused rather than a second hash being written: it is already a Murmur-style
/// finaliser over (dice, row, position, salt), already known good on both cores, and passing 0 for
/// the dice is what says out loud that this feature has no variation index.
[[nodiscard]] constexpr int32_t signedDeviation(uint8_t amount, int32_t noteRowId, int32_t notePos, uint32_t salt) {
	if (amount == kOff) {
		return 0;
	}
	const uint32_t span = 2U * static_cast<uint32_t>(amount) + 1U;
	return static_cast<int32_t>(spice::roll(0, noteRowId, notePos, salt) % span) - static_cast<int32_t>(amount);
}

/// What this note's velocity becomes.
[[nodiscard]] constexpr uint8_t velocity(int32_t written, uint8_t amount, int32_t noteRowId, int32_t notePos) {
	if (amount == kOff) {
		return static_cast<uint8_t>(written);
	}
	const int32_t v = written + signedDeviation(amount, noteRowId, notePos, kSaltVelocity);
	if (v < kMinVelocity) {
		return static_cast<uint8_t>(kMinVelocity);
	}
	if (v > kMaxVelocity) {
		return static_cast<uint8_t>(kMaxVelocity);
	}
	return static_cast<uint8_t>(v);
}

/// What this note's length becomes, in ticks. Varies both ways: some notes ring longer, some
/// shorter. The caller clamps a lengthened note so it cannot overlap the next note in its row -
/// two overlapping notes in one row is not a state a NoteVector is allowed to be in.
///
/// Floored at 1, and a one-tick note is left alone, for the reason spice.h records: a length that
/// rounds to 0 ends on the tick it began, which the sequencer reads as "already over" rather than
/// "very short", and that is how a row goes silent for reasons nobody can find.
[[nodiscard]] constexpr int32_t gateLength(int32_t writtenLength, uint8_t amount, int32_t noteRowId, int32_t notePos) {
	if (amount == kOff || writtenLength <= 1) {
		return writtenLength;
	}
	const int32_t percent = 100 + signedDeviation(amount, noteRowId, notePos, kSaltGate);
	const int32_t scaled = writtenLength * percent / 100;
	return scaled < 1 ? 1 : scaled;
}

/// The octave displacement for this note, in semitones: 0, +12 or -12.
///
/// Up and down both. Near the bottom of a bassline the down-jumps simply will not happen, because
/// `octave()` below declines a displacement that would leave the note range - so the direction
/// needs no special handling at the edges.
[[nodiscard]] constexpr int32_t octaveShift(uint8_t chance, int32_t noteRowId, int32_t notePos) {
	if (chance == kOff) {
		return 0;
	}
	if (static_cast<int32_t>(spice::roll(0, noteRowId, notePos, kSaltOctave) % 100U) >= static_cast<int32_t>(chance)) {
		return 0;
	}
	return ((spice::roll(0, noteRowId, notePos, kSaltOctaveDir) & 1U) != 0U) ? kSemitonesPerOctave
	                                                                         : -kSemitonesPerOctave;
}

/// The note code this note should actually sound, after any octave displacement.
///
/// **Called after Harmonize, never before.** Harmonize maps a written note's scale degree onto a
/// chord tone; displacing first would have that mapping throw the octave away, because it derives
/// the pitch from the degree. Displacing after keeps the note a chord tone, since an octave of a
/// chord tone is still a chord tone.
[[nodiscard]] constexpr int32_t octave(int32_t noteCode, uint8_t chance, int32_t noteRowId, int32_t notePos) {
	const int32_t shifted = noteCode + octaveShift(chance, noteRowId, notePos);
	if (shifted < kMinNoteCode || shifted > kMaxNoteCode) {
		return noteCode;
	}
	return shifted;
}

} // namespace deluge::modulation::midi_fx::spread
