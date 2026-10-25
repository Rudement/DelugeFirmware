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

/// Harmonize - OXI One style "harmonize to track".
///
/// A clip with Harmonize on stops playing its written notes as absolute pitches and plays them as
/// *degrees of whatever chord the song's chord track is sounding right now*. The phrase keeps its
/// shape, its rhythm and its contour; only the pitches move, and they move with the progression.
///
/// **Degree mapping, not nearest-note.** The written note's degree in the song key indexes into the
/// chord's tones, wrapping upward by octave when it runs off the top. A phrase written as degrees
/// 0-2-1-0 is a *shape*: over Am it plays A-E-C-A, and when the chord track moves to F it plays
/// F-C-A-F. Snapping each note to whichever chord tone is nearest would keep the register closer but
/// collapse two written notes a tone apart onto one pitch, which loses the shape - and the shape is
/// the thing being harmonised.
///
/// Everything here is a pure function of (written note, key, chord). No state, no allocation,
/// nothing that has to agree across the two cores - the state lives one file over in
/// `chord_source`, which is also where the chord is read and cached.
namespace deluge::modulation::midi_fx::harmonize {

/// Chord tones held at once. A chord source sounding more than this is not a chord, so the extras
/// are dropped rather than grown into - a fixed array keeps the read off the heap and out of the
/// audio thread's way.
constexpr int32_t kMaxChordTones = 8;

constexpr int32_t kLowestNote = 0;
constexpr int32_t kHighestNote = 127;

/// Floor division and modulo, so a note below the key root lands in the octave below rather than
/// truncating toward zero. C's `/` and `%` round toward zero, which for a bassline written under
/// the root puts two different notes on the same degree and is audible immediately.
[[nodiscard]] constexpr int32_t floorDiv(int32_t a, int32_t b) {
	const int32_t q = a / b;
	return ((a % b != 0) && ((a < 0) != (b < 0))) ? q - 1 : q;
}

[[nodiscard]] constexpr int32_t floorMod(int32_t a, int32_t b) {
	const int32_t r = a % b;
	return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

/// Folds a note back into MIDI range by octaves rather than clamping it.
///
/// Clamping would put every out-of-range degree on the same pitch, and a tail of notes all landing
/// on 127 sounds like a fault rather than a voicing. Folding keeps the pitch class - so it is still
/// a chord tone - and only moves the register.
[[nodiscard]] constexpr int32_t foldIntoRange(int32_t note) {
	while (note > kHighestNote) {
		note -= 12;
	}
	while (note < kLowestNote) {
		note += 12;
	}
	return note;
}

/// How far either side of the written note a harmonised note may sit, in semitones. A tritone: far
/// enough that the chord tone is genuinely the one the degree chose, near enough that the phrase
/// never leaves the register it was written in.
constexpr int32_t kRegisterWindow = 6;

/// Moves a chord tone by whole octaves until it sits within a tritone of the written note.
///
/// **This is what keeps the phrase in the register you wrote it in**, and it is the whole of the
/// second register fix. Moving by whole octaves can never change which chord tone it is - only where
/// that tone sits - so the degree mapping above decides the note and this decides only the octave.
[[nodiscard]] constexpr int32_t placeNear(int32_t tone, int32_t writtenNote) {
	while (tone - writtenNote > kRegisterWindow) {
		tone -= 12;
	}
	while (writtenNote - tone > kRegisterWindow) {
		tone += 12;
	}
	return tone;
}

/// The chord currently sounding on the chord track, as absolute MIDI notes, ascending, no
/// duplicates.
struct Chord {
	int8_t count{0};
	int8_t pitches[kMaxChordTones]{};

	constexpr void clear() { count = 0; }
	[[nodiscard]] constexpr bool empty() const { return count == 0; }

	/// Insertion sort into the fixed array. Duplicate pitches are ignored: two rows written on the
	/// same note are one chord tone, and letting them in twice would shift every degree above them.
	constexpr void add(int32_t pitch) {
		if (pitch < kLowestNote || pitch > kHighestNote) {
			return;
		}
		const auto value = static_cast<int8_t>(pitch);
		int32_t i = 0;
		while (i < count && pitches[i] < value) {
			i++;
		}
		if (i < count && pitches[i] == value) {
			return;
		}
		if (count >= kMaxChordTones) {
			return;
		}
		for (int32_t j = count; j > i; j--) {
			pitches[j] = pitches[j - 1];
		}
		pitches[i] = value;
		count++;
	}

	[[nodiscard]] constexpr bool contains(int32_t pitch) const {
		for (int32_t i = 0; i < count; i++) {
			if (pitches[i] == pitch) {
				return true;
			}
		}
		return false;
	}
};

/// The degree mapping.
///
/// **The degree decides which chord tone; the written note decides which octave.** Degree 0 is the
/// chord's lowest sounding tone, so a phrase written on the key root plays the chord's bass note
/// whatever the chord is, and degrees above the chord's tone count wrap round it: with a triad,
/// scale degrees 0-1-2-3 give tone 0, tone 1, tone 2, tone 0 again. Then `placeNear` puts whichever
/// tone was chosen in the octave nearest the note you actually wrote.
///
/// **This took three passes on hardware to get right, and both of the wrong answers are worth
/// knowing.** The first version added the written note's octave above `key.rootNote` to
/// `chord.pitches[index]` - pitches that are already absolute MIDI notes off the chord track - so
/// the register was counted twice. `MusicalKey::rootNote` defaults to 0, so a line written at 60-71
/// over a C3 triad came out at 108-127: four octaves up, jammed against the ceiling, with
/// `foldIntoRange` folding it back on itself so a written 69 sounded *higher* than a written 71.
///
/// The second version measured the octave from the chord's own lowest tone instead, which fixed
/// that - but kept an octave *carry* of `degree / chord.count`, so that a rising scale came out as a
/// rising arpeggio. That is a real musical idea and it sounds right for a narrow phrase, which is
/// why it survived a hardware pass. It cannot survive a wide one: seven scale degrees mapped
/// monotonically onto three chord tones must stretch the register by seven thirds of an octave per
/// octave, so a line walking a full octave climbed away over two.
///
/// You cannot have both the shape and the register, and the register is the one a player notices.
/// So the carry is gone and the result is simply placed beside the written note. A narrow phrase is
/// completely unaffected - it never crossed a carry boundary in the first place - and a wide one
/// now wraps round the chord instead of climbing off the top of it. The header test pins both.
[[nodiscard]] constexpr int32_t fromDegree(int32_t writtenNote, int32_t degreeInScale, Chord const& chord) {
	if (chord.empty()) {
		return -1;
	}
	const int32_t index = floorMod(degreeInScale, chord.count);
	return foldIntoRange(placeNear(static_cast<int32_t>(chord.pitches[index]), writtenNote));
}

/// Fallback for a written note that is not in the song's key at all, where it has no degree.
///
/// Snaps to whichever chord tone is closest in pitch, counting the chord's tones in every octave -
/// so a chromatic passing note lands on the nearest thing that fits rather than dropping out. A
/// dropped note reads as a fault; a slightly re-pitched one reads as harmonisation, which is what
/// this is.
[[nodiscard]] constexpr int32_t nearestTone(int32_t writtenNote, Chord const& chord) {
	if (chord.empty()) {
		return -1;
	}
	int32_t best = -1;
	int32_t bestDistance = 1000;
	for (int32_t i = 0; i < chord.count; i++) {
		// Move this tone by whole octaves to sit as near the written note as it can - the same
		// placement fromDegree uses, so an out-of-key note lands in the same register as its
		// neighbours rather than somewhere of its own.
		const int32_t candidate = foldIntoRange(placeNear(static_cast<int32_t>(chord.pitches[i]), writtenNote));
		const int32_t distance = candidate > writtenNote ? candidate - writtenNote : writtenNote - candidate;
		if (distance < bestDistance) {
			bestDistance = distance;
			best = candidate;
		}
	}
	return best;
}

} // namespace deluge::modulation::midi_fx::harmonize
