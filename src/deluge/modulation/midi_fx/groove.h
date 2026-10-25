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

#include <algorithm>
#include <cstddef>
#include <cstdint>

/// Groove - a mashup of the OXI One's groove engine and Ableton Live's Groove Pool.
///
/// From the OXI: a template of per-step timing and velocity offsets, scaled by Accent and Time
/// (-120..+120%, negative inverts), Offset to rotate it, and a groove of its own per track.
/// From Ableton: .agr groove files load straight off the card, bringing their own Timing / Velocity /
/// Random amounts with them, and Random is a control in its own right - it humanises timing and
/// velocity whatever Accent and Time are set to, and with the template Off it is a plain humaniser.
/// See GROOVE-FEASIBILITY.md.
///
/// **How the timing is done.** Not by delaying notes. A groove is a time warp: the clip's clock is
/// bent so that grid step k is *reached* at k*step + offset(k), and everything between two steps is
/// stretched or squeezed to fit. The row then reads its notes through the bent clock, exactly the
/// way Play FX Beat Stretch and Loop Remix read through theirs. That buys three things a note delay
/// could not: notes can land early as well as late (the sequence is known in advance, so there is
/// nothing to look ahead for), note lengths move with their notes so nothing overlaps or hangs, and
/// it is non-destructive - the notes on the grid never move, turn Groove off and they are where you
/// wrote them.
///
/// The loop's first step is always on the grid. The warp is pinned at both ends of the loop so the
/// bent clock and the real one wrap at the same instant; without that a note pushed late across the
/// loop point would belong to two loops at once.
namespace deluge::modulation::midi_fx::groove {

/// Longest template, in steps. Two bars of 16ths.
constexpr int32_t kMaxSteps = 64;
/// Template timing is stored in 1/250ths of a step, so +/-125 is half a step.
constexpr int32_t kTimingUnit = 250;
constexpr int32_t kTimingMax = 125;
/// Template velocity offsets, as on the OXI's groove builder.
constexpr int32_t kVelocityMax = 64;
/// Name length including the terminator. Room for a typical Ableton groove file name.
constexpr int32_t kNameMax = 24;

/// Accent and Time: -120% to +120%, as on the OXI. Negative inverts the template.
constexpr int32_t kAmountMin = -120;
constexpr int32_t kAmountMax = 120;
constexpr int32_t kRandomMax = 100;

/// Template ids. 0 is off; built-ins follow; user grooves (loaded from .agr files or from a song) live
/// in a session library and are numbered from kUserBase.
constexpr uint8_t kOff = 0;
constexpr uint8_t kUserBase = 64;
constexpr int32_t kMaxUser = 32;

struct Template {
	char name[kNameMax];
	uint8_t length;          ///< Steps, 1..kMaxSteps.
	uint8_t stepsPerQuarter; ///< 4 = 16ths, 2 = 8ths, 6 = 16th triplets, 3 = 8th triplets, 8 = 32nds, 1 = quarters.
	int8_t timing[kMaxSteps];
	int8_t velocity[kMaxSteps];
};

/// What a clip, or a kit row, stores. Small on purpose: a kit carries one per row.
struct Settings {
	uint8_t templateId{kOff};
	int8_t accent{0};
	int8_t time{0};
	uint8_t random{0};
	uint8_t offset{0};

	[[nodiscard]] constexpr bool operator==(const Settings& o) const {
		return templateId == o.templateId && accent == o.accent && time == o.time && random == o.random
		       && offset == o.offset;
	}
	[[nodiscard]] constexpr bool isDefault() const { return *this == Settings{}; }
};

// --- Templates ----------------------------------------------------------------------------------

[[nodiscard]] int32_t numBuiltIn();
/// The template an id refers to, or nullptr for off / an id that is not loaded.
[[nodiscard]] const Template* templateFor(uint8_t id);
/// Adds a user template to the session library, or finds an identical one already there. Returns its
/// id, or kOff if the library is full.
uint8_t internUser(const Template& t);
/// How many user templates the session library holds (ids kUserBase .. kUserBase + n - 1).
[[nodiscard]] int32_t numUserLoaded();
[[nodiscard]] bool isValidStepsPerQuarter(int32_t spq);

/// Turns Ableton groove (.agr) XML into a template. Pure, so it can be tested off the hardware.
/// `name` is the groove's name (normally the file name without .agr). Returns false if the data
/// holds no usable groove.
bool parseAgr(const char* data, size_t length, const char* name, Template& out);

/// The Groove Pool amounts an .agr file was saved with (Ableton's Timing, Velocity and Random), each
/// clamped to our ranges. A file that does not say leaves the defaults: Timing 100, Velocity 0, Random 0.
struct FileAmounts {
	int32_t timing{100};
	int32_t velocity{0};
	int32_t random{0};
};
void parseAgrAmounts(const char* data, size_t length, FileAmounts& out);

/// The template Random humanises against when no template is chosen: flat, so only the roll moves notes.
[[nodiscard]] const Template* flatTemplate();

/// Song-file encoding of a Settings plus, for a user groove, the template itself, as one string.
/// Built-in: "b|<id>|<accent>|<time>|<random>|<offset>". User: "u|<accent>|<time>|<random>|<offset>|
/// <spq>|<length>|<timing hex>|<velocity hex>|<name>". The name goes last so it may contain '|'.
/// Returns the number of characters written (excluding the terminator), 0 if nothing to write.
size_t encode(const Settings& s, char* buf, size_t bufLen);
/// The reverse. Unknown or damaged strings leave `s` at its default and return false.
bool decode(const char* str, Settings& s);

// --- The warp -----------------------------------------------------------------------------------

/// Murmur-style finaliser, as Spice uses.
[[nodiscard]] constexpr uint32_t mix32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

constexpr uint32_t kSaltTime = 0x54494D45; // 'TIME'
constexpr uint32_t kSaltVel = 0x56454C4F;  // 'VELO'

/// A roll in -1000..1000 for one step of one pass.
[[nodiscard]] constexpr int32_t bipolarRoll(uint32_t salt, int32_t rowSalt, int32_t globalStep) {
	uint32_t h = salt;
	h ^= static_cast<uint32_t>(rowSalt) * 0x85EBCA6BU;
	h ^= static_cast<uint32_t>(globalStep) * 0xC2B2AE35U;
	return static_cast<int32_t>(mix32(h) % 2001U) - 1000;
}

/// Floor division for possibly-negative numerators.
[[nodiscard]] constexpr int64_t floorDiv(int64_t a, int64_t b) {
	int64_t q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0))) {
		q--;
	}
	return q;
}

[[nodiscard]] constexpr int32_t positiveMod(int64_t a, int32_t m) {
	int32_t r = static_cast<int32_t>(a % m);
	return r < 0 ? r + m : r;
}

/// One row's warp for one pass. Built fresh on each call - it is a handful of integers - so there is no
/// state to go stale when a setting, the loop length or the template changes under it.
struct Warp {
	const Template* tpl{nullptr};
	int32_t step{0};   ///< Ticks per template step.
	int32_t length{0}; ///< Loop length, in ticks.
	int32_t knots{0};  ///< Grid steps in the loop: ceil(length / step).
	int32_t time{0};
	int32_t random{0};
	int32_t offset{0};
	int32_t repeat{0};
	int32_t rowSalt{0};

	/// Grooved (grid) position of knot k.
	[[nodiscard]] constexpr int32_t gridPos(int32_t k) const { return k >= knots ? length : k * step; }

	/// How far knot k is moved, in ticks. 0 at both ends of the loop.
	[[nodiscard]] constexpr int32_t knotOffset(int32_t k) const {
		if (k <= 0 || k >= knots) {
			return 0;
		}
		const int64_t globalStep = static_cast<int64_t>(repeat) * knots + k;
		const int32_t idx = positiveMod(globalStep + offset, tpl->length);
		// Rounded rather than truncated: at 48 ticks a quarter a 57% swing is 1.68 ticks, and
		// truncating it to 1 would play it as 54%.
		const int64_t num = static_cast<int64_t>(tpl->timing[idx]) * step * time;
		constexpr int64_t den = kTimingUnit * 100;
		int64_t d = (num >= 0 ? num + den / 2 : num - den / 2) / den;
		if (random != 0) {
			// Ableton's Random rather than the OXI's: on its own, not scaled by Time, so it humanises
			// even a clip with no template. At 100 a step may wander a quarter of a step either way.
			const int32_t roll = bipolarRoll(kSaltTime, rowSalt, static_cast<int32_t>(globalStep));
			d += static_cast<int64_t>(roll) * step * random / (1000LL * 100 * 4);
		}
		// Each knot stays within half the gap to either neighbour, less a tick, so knots can never
		// cross and the warp stays strictly increasing - which is what makes it invertible.
		const int32_t before = gridPos(k) - gridPos(k - 1);
		const int32_t after = gridPos(k + 1) - gridPos(k);
		int32_t limit = std::min(before, after) / 2 - 1;
		if (limit < 0) {
			limit = 0;
		}
		return static_cast<int32_t>(std::clamp<int64_t>(d, -limit, limit));
	}

	/// Real position of knot k.
	[[nodiscard]] constexpr int32_t realPos(int32_t k) const { return gridPos(k) + knotOffset(k); }

	/// Real clock -> grooved clock. Identity outside the loop.
	[[nodiscard]] constexpr int32_t forward(int32_t r) const {
		if (r <= 0 || r >= length) {
			return r;
		}
		int32_t k = std::min(r / step, knots - 1);
		int32_t rk = realPos(k);
		while (k > 0 && rk > r) {
			k--;
			rk = realPos(k);
		}
		int32_t rk1 = realPos(k + 1);
		while (k < knots - 1 && rk1 <= r) {
			k++;
			rk = rk1;
			rk1 = realPos(k + 1);
		}
		const int32_t gk = gridPos(k);
		const int32_t gk1 = gridPos(k + 1);
		const int32_t span = rk1 - rk;
		if (span <= 0) {
			return gk;
		}
		return gk + static_cast<int32_t>(static_cast<int64_t>(r - rk) * (gk1 - gk) / span);
	}

	/// The first real tick at which the grooved clock has reached g.
	[[nodiscard]] constexpr int32_t realAtOrAfter(int32_t g) const {
		if (g <= 0 || g >= length) {
			return g;
		}
		const int32_t k = std::min(g / step, knots - 1);
		const int32_t gk = gridPos(k);
		const int32_t gk1 = gridPos(k + 1);
		const int32_t rk = realPos(k);
		const int32_t rk1 = realPos(k + 1);
		int32_t r = rk;
		if (gk1 > gk) {
			r = rk + static_cast<int32_t>(-floorDiv(-static_cast<int64_t>(g - gk) * (rk1 - rk), gk1 - gk));
		}
		for (int32_t i = 0; i < 4 && forward(r - 1) >= g; i++) {
			r--;
		}
		for (int32_t i = 0; i < 4 && forward(r) < g; i++) {
			r++;
		}
		return r;
	}

	/// The last real tick at which the grooved clock has not passed g. For reverse playback.
	[[nodiscard]] constexpr int32_t realAtOrBefore(int32_t g) const {
		if (g <= 0 || g >= length) {
			return g;
		}
		const int32_t k = std::min(g / step, knots - 1);
		const int32_t gk = gridPos(k);
		const int32_t gk1 = gridPos(k + 1);
		const int32_t rk = realPos(k);
		const int32_t rk1 = realPos(k + 1);
		int32_t r = rk;
		if (gk1 > gk) {
			r = rk + static_cast<int32_t>(floorDiv(static_cast<int64_t>(g - gk) * (rk1 - rk), gk1 - gk));
		}
		for (int32_t i = 0; i < 4 && forward(r + 1) <= g; i++) {
			r++;
		}
		for (int32_t i = 0; i < 4 && forward(r) > g; i++) {
			r--;
		}
		return r;
	}
};

/// Builds the warp for one row, or returns false when Groove has nothing to do: off, no template, Time
/// at 0, or a loop too short to hold two steps. `quarterTicks` is the song's quarter-note length.
[[nodiscard]] inline bool makeWarp(const Settings& s, int32_t quarterTicks, int32_t loopLength, int32_t repeat,
                                   int32_t rowSalt, Warp& w) {
	const Template* t = templateFor(s.templateId);
	if (t == nullptr) {
		t = flatTemplate();
	}
	// Nothing to bend: no template timing in play and no Random.
	if ((s.time == 0 || s.templateId == kOff) && s.random == 0) {
		return false;
	}
	if (t->length == 0 || t->stepsPerQuarter == 0) {
		return false;
	}
	const int32_t step = quarterTicks / t->stepsPerQuarter;
	if (step < 4 || loopLength < 2 * step) {
		return false;
	}
	w.tpl = t;
	w.step = step;
	w.length = loopLength;
	w.knots = (loopLength + step - 1) / step;
	w.time = s.templateId == kOff ? 0 : s.time;
	w.random = s.random;
	w.offset = s.offset;
	w.repeat = repeat < 0 ? 0 : repeat;
	w.rowSalt = rowSalt;
	return true;
}

/// Accent: the note's velocity with the template's offset for the step it falls on, scaled by Accent,
/// plus Random's share. `realPos` is where the clock really is as the note starts - grooved notes move
/// less than half a step, so rounding it to the nearest step finds the step the note was written on.
[[nodiscard]] inline int32_t accentVelocity(const Settings& s, int32_t velocity, int32_t quarterTicks,
                                            int32_t realPos, int32_t loopLength, int32_t repeat, int32_t rowSalt) {
	const Template* t = templateFor(s.templateId);
	if (t == nullptr) {
		t = flatTemplate();
	}
	if ((s.accent == 0 || s.templateId == kOff) && s.random == 0) {
		return velocity;
	}
	if (t->length == 0 || t->stepsPerQuarter == 0) {
		return velocity;
	}
	const int32_t step = quarterTicks / t->stepsPerQuarter;
	if (step <= 0 || loopLength <= 0) {
		return velocity;
	}
	const int32_t knots = (loopLength + step - 1) / step;
	const int32_t k = (realPos + step / 2) / step; // may equal knots: the next loop's first step
	const int64_t globalStep = static_cast<int64_t>(repeat < 0 ? 0 : repeat) * knots + k;
	const int32_t idx = positiveMod(globalStep + s.offset, t->length);
	int32_t v = velocity + (s.templateId == kOff ? 0 : t->velocity[idx] * s.accent / 100);
	if (s.random != 0) {
		// On its own, like the timing roll. At Random 100 a note may wander 20 either way.
		const int32_t roll = bipolarRoll(kSaltVel, rowSalt, static_cast<int32_t>(globalStep));
		v += static_cast<int32_t>(static_cast<int64_t>(roll) * 20 * s.random / (1000LL * 100));
	}
	return std::clamp<int32_t>(v, 1, 127);
}

} // namespace deluge::modulation::midi_fx::groove
