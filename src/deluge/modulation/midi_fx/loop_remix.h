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

/// Loop Remix - RS7000-style real-time rearrangement of the clip's loop.
///
/// The loop is divided into slices and each slice is read from somewhere else in the loop. Nothing
/// is written into the clip: this is a transform on *where the phrase is read*, which is the same
/// contract Play FX's Clock Shift has, applied piecewise instead of as a constant. See
/// LOOP-REMIX-FEASIBILITY.md.
///
/// **Every decision here is a hash, not a draw.** Which slice sounds where, which run plays
/// backwards and which slice drops out are pure functions of (type, variation, slice), so the remix
/// repeats bar after bar, survives a clip relaunch and a stop/start, and comes out identical on
/// both cores. Type and Variation are indexes rather than seeds that get drawn, so a bar you liked
/// is one turn back rather than gone - the same rule Spice & Dice follows, and for the same reason.
///
/// **Reverse reverses the ORDER OF A RUN OF SLICES, not the material inside one.** The Deluge's
/// read head runs forwards through a note row: a slice mapped to a backwards-moving read position
/// would find its note-ons with a negative time-til-next and never sound them. Reversing a run of
/// four slices costs nothing, stays a pure permutation, and on a drum loop is the audible effect
/// people reach for. The other kind of reverse needs the sequencer to run backwards for part of a
/// bar, which is a different feature - LOOP-REMIX-FEASIBILITY.md §3.3.
namespace deluge::modulation::midi_fx::loop_remix {

// --- The RS7000's three settings ----------------------------------------------------------------
//
// Named and numbered as the RS7000 does, because that is the instrument people know:
//
//  - TYPE, 1-16: how complex the remix is. Higher Types move more, reach further, and also chop
//    finer (see sliceLength).
//  - VARIATION, 1-128: which remix at that Type. Sixteen each of Normal, Reverse, Break, Pitch and
//    Roll, then 48 Fills. An index, not a seed that is drawn, so a remix you liked is one turn back.
//  - INTERVAL, 1-8: how often it happens. 1 is every bar, 2 every other bar, up to every 8th.
//
// Stored zero-based, shown one-based.

constexpr uint8_t kTypeMin = 0;
constexpr uint8_t kTypeMax = 15;
constexpr uint8_t kVariationMin = 0;
constexpr uint8_t kVariationMax = 127;
constexpr uint8_t kIntervalMin = 0;
constexpr uint8_t kIntervalMax = 7;

// The families, in the order the RS7000 lists them. A Variation's family is its number / 16, except
// that everything from 81 up is a Fill.
constexpr uint8_t kFamilyNormal = 0;
constexpr uint8_t kFamilyReverse = 1;
constexpr uint8_t kFamilyBreak = 2;
constexpr uint8_t kFamilyPitch = 3;
constexpr uint8_t kFamilyRoll = 4;
constexpr uint8_t kFamilyFill = 5;
constexpr uint8_t kFirstFillVariation = 80;

[[nodiscard]] constexpr uint8_t familyOf(uint8_t variation) {
	return variation >= kFirstFillVariation ? kFamilyFill : static_cast<uint8_t>(variation / 16);
}

/// What is set. `active` is only ever true on the live copy: it is what Remix turns on and Default
/// turns off, since the RS7000's remix has no Off position - you either have pressed Remix or not.
struct Settings {
	uint8_t type{0};
	uint8_t variation{0};
	uint8_t interval{0};
	bool active{false};
	constexpr bool operator==(const Settings&) const = default;
};

/// One place a remix lives: what the page is set to, and what is actually playing. Nothing is heard
/// until launch() copies the first to the second, so a remix can be dialled in without it changing
/// under your hands. Lives on the Clip for a melodic clip and on each NoteRow of a Kit, because a
/// Kit row is a drum and each drum gets its own remix.
struct Slot {
	Settings page;
	Settings live;
	constexpr void launch() {
		live = page;
		live.active = true;
	}
	[[nodiscard]] constexpr bool isDefault() const { return page == Settings{} && !live.active; }
};

/// Ticks to a bar: 48 to a quarter note, the constant Strum's Amount is measured in.
constexpr int32_t kBarTicks = 192;

/// The slice length a Type chops at. The RS7000 folds the division into its Types and so does this.
/// Read off the hardware's own graphics: the division climbs gradually and tops out around
/// sixteenths, not thirty-seconds - the gentle Types chop at 1/4, the middle at 1/8, the top at 1/16.
[[nodiscard]] constexpr int32_t sliceLength(uint8_t type) {
	return type < 3 ? 48 : (type < 9 ? 24 : 12);
}

/// How many slices make up one bar at this Type's slice size: 8 at 1/8, 16 at 1/16, 32 at 1/32.
/// Loop Remix rearranges within the bar, so the beat grid is measured against this.
[[nodiscard]] constexpr int32_t slicesPerBar(uint8_t type) {
	return kBarTicks / sliceLength(type);
}

// --- The hash -----------------------------------------------------------------------------------

constexpr uint32_t kSaltOrder = 0x4F524452; // 'ORDR'
constexpr uint32_t kSaltRev = 0x52455653;   // 'REVS'
constexpr uint32_t kSaltDrop = 0x42524B53;  // 'BRKS'
constexpr uint32_t kSaltRoll = 0x524F4C4C;  // 'ROLL'
constexpr uint32_t kSaltFill = 0x46494C4C;  // 'FILL'
constexpr uint32_t kSaltPitch = 0x50544348; // 'PTCH'

[[nodiscard]] constexpr uint32_t mix32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

[[nodiscard]] constexpr uint32_t roll(uint8_t type, uint8_t variation, uint32_t salt, int32_t index) {
	const uint32_t seed = mix32((static_cast<uint32_t>(type) * 128U + variation) * 0x9E3779B9U ^ salt);
	return mix32(seed ^ (static_cast<uint32_t>(index) * 0x85EBCA6BU));
}

// --- What Type means ----------------------------------------------------------------------------

[[nodiscard]] constexpr int32_t moveChance(uint8_t type) {
	return 15 + type * 5; // 15% at Type 1, 90% at Type 16
}
[[nodiscard]] constexpr int32_t maxJump(uint8_t type) {
	return 1 + type / 2;
}
[[nodiscard]] constexpr int32_t reverseChance(uint8_t type) {
	return 10 + type * 3;
}
[[nodiscard]] constexpr int32_t dropChance(uint8_t type) {
	return 5 + type * 2;
}
[[nodiscard]] constexpr int32_t rollChance(uint8_t type) {
	return 15 + type * 3;
}
[[nodiscard]] constexpr int32_t runLength(uint8_t type) {
	return type < 8 ? 4 : 2;
}
/// The downbeat stays put below this Type: a remix that moves slice 0 stops landing.
constexpr uint8_t kTypeMovesDownbeat = 12;

// --- The mapping --------------------------------------------------------------------------------

[[nodiscard]] constexpr bool reversing(uint8_t family) {
	return family == kFamilyReverse || family == kFamilyBreak;
}
[[nodiscard]] constexpr bool breaking(uint8_t family) {
	return family == kFamilyBreak;
}

/// Reverses the order of the run this slice belongs to, when the run's own roll says so. A bijection
/// within the run. The last run is left alone when it is ragged.
[[nodiscard]] constexpr int32_t reverseRun(int32_t slice, int32_t numSlices, uint8_t family, uint8_t type,
                                           uint8_t variation) {
	if (!reversing(family)) {
		return slice;
	}
	const int32_t runLen = runLength(type);
	const int32_t runIdx = slice / runLen;
	const int32_t runStart = runIdx * runLen;
	if (runStart + runLen > numSlices) {
		return slice;
	}
	if (runIdx == 0 && type < kTypeMovesDownbeat) {
		return slice;
	}
	if (roll(type, variation, kSaltRev, runIdx) % 100U >= static_cast<uint32_t>(reverseChance(type))) {
		return slice;
	}
	return runStart + (runLen - 1 - (slice - runStart));
}

/// Which slice of the written loop should sound at this slice.
///
/// A musical, beat-aligned bijection. Within each bar (or the whole loop when it is shorter than a
/// bar) the slices are rearranged so every slice sounds exactly once - nothing doubles or vanishes -
/// and every move lands on the beat grid: whole beats are rotated and reversed, with motion inside a
/// beat added only as Type climbs, and beat one held in place until the top Types. That is what makes
/// it read as a groove variation rather than scattered hits; the old free hash, which was not a
/// permutation and jumped by arbitrary amounts, is what made the timing feel off.
[[nodiscard]] constexpr int32_t sourceSlice(int32_t slice, int32_t numSlices, uint8_t type, uint8_t variation) {
	if (numSlices <= 1) {
		return slice;
	}
	int32_t n = slicesPerBar(type);
	if (numSlices < n) {
		n = numSlices;
	}
	const int32_t base = (slice / n) * n;
	const int32_t blockLen = (numSlices - base < n) ? (numSlices - base) : n;
	if (blockLen < 2) {
		return slice;
	}
	const int32_t local = slice - base;
	int32_t spb = slicesPerBar(type) / 4; // slices per beat
	if (spb < 1) {
		spb = 1;
	}
	// A block a beat does not divide evenly (an odd loop length, a ragged tail): fall back to a plain
	// whole-block rotation, which is a bijection at any length.
	if (spb >= blockLen || (blockLen % spb) != 0) {
		const int32_t r = 1 + ((variation + type) % (blockLen - 1));
		return base + (local + r) % blockLen;
	}
	const int32_t beats = blockLen / spb;
	const int32_t beat = local / spb;
	const int32_t sub = local % spb;
	const int32_t level = type & 3; // where this Type sits inside its slice-size band - the intensity
	const bool anchor = type < kTypeMovesDownbeat;
	const int32_t rb = anchor ? beats - 1 : beats; // how many beats are free to move
	const int32_t off = anchor ? 1 : 0;            // first movable beat (beat one is held when anchored)
	int32_t br = (rb > 0) ? (variation + type) % rb : 0; // whole-beat rotation, Type folded in
	const bool rev = (level >= 2 && rb > 1);             // reverse the movable beats up the band
	int32_t sr = 0;
	const bool subMove = (spb > 1 && (type >= 4 || level >= 2));
	if (subMove) {
		sr = (variation + type) % spb; // rotate the slices inside a beat
	}
	if (br == 0 && !rev && sr == 0) {
		br = (rb > 1) ? 1 : 0; // never a no-op: an applied remix always moves something
	}
	int32_t beat2;
	if (anchor && beat == 0) {
		beat2 = 0;
	}
	else {
		const int32_t bi = beat - off;
		const int32_t bir = (bi + br) % rb;
		beat2 = off + (rev ? (rb - 1 - bir) : bir);
	}
	const int32_t sub2 = (sub + sr) % spb;
	return base + beat2 * spb + sub2;
}

[[nodiscard]] constexpr int32_t mappedSlice(int32_t slice, int32_t numSlices, uint8_t family, uint8_t type,
                                            uint8_t variation) {
	return sourceSlice(reverseRun(slice, numSlices, family, type, variation), numSlices, type, variation);
}

[[nodiscard]] constexpr bool sliceDropped(int32_t slice, uint8_t family, uint8_t type, uint8_t variation) {
	if (!breaking(family) || slice == 0) {
		return false;
	}
	return roll(type, variation, kSaltDrop, slice) % 100U < static_cast<uint32_t>(dropChance(type));
}

/// Whether the read position actually jumps at the start of this slice.
[[nodiscard]] constexpr bool cutsAt(int32_t slice, int32_t numSlices, uint8_t family, uint8_t type, uint8_t variation) {
	if (numSlices <= 1) {
		return false;
	}
	const int32_t previous = (slice == 0) ? numSlices - 1 : slice - 1;
	if (sliceDropped(slice, family, type, variation) || sliceDropped(previous, family, type, variation)) {
		return true;
	}
	const int32_t here = mappedSlice(slice, numSlices, family, type, variation);
	const int32_t before = mappedSlice(previous, numSlices, family, type, variation);
	return here != (before + 1) % numSlices;
}

/// Whether this slice is a roll: its read position repeats part of the slice instead of running
/// through it, so whatever comes next starts from a position that is not the end of its source.
[[nodiscard]] constexpr bool sliceRolled(int32_t slice, uint8_t family, uint8_t type, uint8_t variation) {
	return family == kFamilyRoll
	       && roll(type, variation, kSaltRoll, slice) % 100U < static_cast<uint32_t>(rollChance(type));
}

/// What the sequencer needs to know for one tick.
struct Mapping {
	/// Where in the phrase to read.
	int32_t pos;
	/// Ticks until this segment ends, so the row can be woken there. INT32_MAX when the remix is not
	/// applying, which leaves the caller's own scheduling untouched.
	int32_t ticksTilSliceEnd;
	/// Which segment this is - changes whenever the read position may jump - or -1 when the remix is
	/// not applying.
	int32_t slice;
	/// Removed: the row should sound nothing until the segment ends.
	bool silent;
	/// The read position jumped here, so a note sounding across the boundary must be stopped.
	bool cut;
	/// Semitones to transpose the note that sounds here (the Pitch family); 0 for every other family.
	int32_t transpose{0};
};

/// Segment ids for stretches that play as written. Far above any slice * 8 + roll id.
constexpr int32_t kPlainId = 1 << 20;

/// The Fill: the last bar of the loop (or the whole loop when it is a bar or shorter) rearranged into
/// a transition that leads back to the top. Everything before the fill plays as written. Forty-eight
/// Variations in three kinds of sixteen - scramble, reverse-and-drop, and an accelerating roll. (The
/// earlier version touched only the last beat, too little to hear.)
[[nodiscard]] constexpr Mapping mapFill(int32_t pos, int32_t effectiveLength, uint8_t type, uint8_t variation) {
	Mapping m{pos, 2147483647, -1, false, false};
	const int32_t window = (effectiveLength < kBarTicks) ? effectiveLength : kBarTicks;
	constexpr int32_t kStep = 12; // a sixteenth
	if (window < 2 * kStep) {
		return m;
	}
	const int32_t start = effectiveLength - window;
	if (pos < start) {
		m.slice = kPlainId;
		m.ticksTilSliceEnd = start - pos;
		return m;
	}
	const int32_t steps = window / kStep;
	const int32_t within = pos - start;
	if (within >= steps * kStep) {
		// Ragged tail of a loop a sixteenth does not divide: leave it as written.
		m.slice = kPlainId + 1;
		m.ticksTilSliceEnd = window - within;
		return m;
	}
	const int32_t step = within / kStep;
	const int32_t off = within % kStep;
	const int32_t kind = (variation - kFirstFillVariation) / 16; // 0, 1, 2 - sixteen fills each
	m.cut = true;
	if (kind == 0) {
		// Scramble: each sixteenth reads from a hashed sixteenth - varies with Type and Variation.
		const int32_t src = static_cast<int32_t>(roll(type, variation, kSaltFill, step) % static_cast<uint32_t>(steps));
		m.slice = step;
		m.ticksTilSliceEnd = kStep - off;
		m.pos = start + src * kStep + off;
	}
	else if (kind == 1) {
		// Reversed, rotated by a hashed amount so each Variation differs, with hashed drops.
		const int32_t rot = static_cast<int32_t>(roll(type, variation, kSaltFill, 777) % static_cast<uint32_t>(steps));
		const int32_t src = ((steps - 1 - step) + rot) % steps;
		m.slice = step;
		m.ticksTilSliceEnd = kStep - off;
		m.pos = start + src * kStep + off;
		m.silent = step > 0 && (roll(type, variation, kSaltDrop, step) % 100U) < 30U;
	}
	else {
		// Build: front is a hashed rearrangement, back an accelerating roll on a hashed anchor step,
		// so the whole thing moves with Type and Variation.
		const int32_t half = steps / 2;
		if (step < half) {
			const int32_t src =
			    static_cast<int32_t>(roll(type, variation, kSaltFill, step) % static_cast<uint32_t>(steps));
			m.slice = step;
			m.ticksTilSliceEnd = kStep - off;
			m.pos = start + src * kStep + off;
		}
		else {
			const int32_t anchor =
			    static_cast<int32_t>(roll(type, variation, kSaltRoll, 0) % static_cast<uint32_t>(half));
			const int32_t chunk = (step < (steps * 3) / 4) ? 12 : 6;
			const int32_t w = within % chunk;
			m.slice = 1000 + step;
			m.ticksTilSliceEnd = chunk - w;
			m.pos = start + anchor * kStep + w;
		}
	}
	return m;
}

/// The whole transform, in one call.
///
/// `pos` comes in already through Play FX's own transform, so the slice grid is measured in the
/// frame the row is actually being read in - which is what makes Loop Remix and Beat Stretch
/// compose rather than fight.
[[nodiscard]] constexpr int32_t pitchChance(uint8_t type) {
	return 15 + type * 4; // ~15% of slices at Type 1, ~75% at Type 16
}

/// Pitch: some slices are transposed by a musical interval. Which slices, and by how much, is hashed
/// from (type, variation) - so it repeats bar after bar, survives a relaunch, and comes back the same
/// when you turn the knob back. More slices move, and the pool of intervals widens, as Type climbs.
[[nodiscard]] constexpr int32_t pitchTranspose(int32_t slice, uint8_t type, uint8_t variation) {
	const uint32_t h = roll(type, variation, kSaltPitch, slice);
	if (h % 100U >= static_cast<uint32_t>(pitchChance(type))) {
		return 0;
	}
	constexpr int8_t kIntervals[] = {12, -12, 7, -5, 5, -7, 3, -3};
	const int32_t span = (type < 8) ? 4 : 8; // gentle Types keep to the octave / fifth / fourth
	return kIntervals[(h >> 8) % static_cast<uint32_t>(span)];
}

[[nodiscard]] constexpr Mapping map(int32_t pos, int32_t effectiveLength, const Settings& s) {
	Mapping m{pos, 2147483647, -1, false, false};
	if (!s.active || effectiveLength <= 0 || pos < 0) {
		return m;
	}
	const uint8_t family = familyOf(s.variation);
	if (family == kFamilyFill) {
		return mapFill(pos, effectiveLength, s.type, s.variation);
	}

	// Interval: only every Nth bar is remixed, counted from the start of the loop.
	const int32_t interval = s.interval + 1;
	const int32_t bar = pos / kBarTicks;
	if (interval > 1 && bar % interval != 0) {
		const int32_t numBars = (effectiveLength + kBarTicks - 1) / kBarTicks;
		const int32_t previousBar = bar == 0 ? numBars - 1 : bar - 1;
		m.slice = kPlainId + bar;
		m.ticksTilSliceEnd = kBarTicks - pos % kBarTicks;
		// The bar before was remixed, so the read position jumps back into the written loop here.
		m.cut = previousBar % interval == 0;
		return m;
	}

	const int32_t sliceLen = sliceLength(s.type);
	const int32_t numSlices = effectiveLength / sliceLen;
	if (numSlices < 2) {
		return m;
	}
	if (pos >= numSlices * sliceLen) {
		return m;
	}

	const int32_t k = pos / sliceLen;
	const int32_t offset = pos - k * sliceLen;
	m.slice = k * 8;
	m.ticksTilSliceEnd = sliceLen - offset;
	m.cut = cutsAt(k, numSlices, family, s.type, s.variation)
	        || sliceRolled(k == 0 ? numSlices - 1 : k - 1, family, s.type, s.variation);

	// The first slice of a remixed bar that follows a bar played as written: the read position
	// jumps there from the plain bar, which cutsAt cannot see.
	if (interval > 1 && (k * sliceLen) % kBarTicks == 0) {
		const int32_t numBars = (effectiveLength + kBarTicks - 1) / kBarTicks;
		const int32_t previousBar = bar == 0 ? numBars - 1 : bar - 1;
		if (previousBar % interval != 0) {
			m.cut = true;
		}
	}

	if (family == kFamilyPitch) {
		m.transpose = pitchTranspose(k, s.type, s.variation);
	}

	if (sliceDropped(k, family, s.type, s.variation)) {
		m.silent = true;
		return m;
	}
	int32_t within = offset;
	if (sliceRolled(k, family, s.type, s.variation)) {
		// A roll: the slice repeats its first half, or first quarter, until it ends.
		const uint32_t h = roll(s.type, s.variation, kSaltRoll, k + 4096);
		const int32_t div = (sliceLen >= 12 && (h % 2U) == 0U) ? 4 : 2;
		const int32_t sub = sliceLen / div;
		within = offset % sub;
		m.slice = k * 8 + offset / sub;
		m.ticksTilSliceEnd = sub - within;
		if (offset / sub > 0) {
			m.cut = true;
		}
	}
	m.pos = mappedSlice(k, numSlices, family, s.type, s.variation) * sliceLen + within;
	return m;
}

} // namespace deluge::modulation::midi_fx::loop_remix
