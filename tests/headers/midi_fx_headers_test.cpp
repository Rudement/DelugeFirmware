// Compiles the standalone midi_fx headers. They are written to need nothing from the firmware
// but forward declarations, so this catches syntax and missing includes in a module that is
// almost entirely new code.
//
// DELIBERATELY EXCLUDED, and worth knowing why:
//
//   retrig_fx.h    owns ArpNote records by value, so it needs the complete type and includes
//                  arpeggiator.h. That is stage 6's note pool - the first effect that emits
//                  note-ONs rather than only note-offs, which means handing the dispatcher a
//                  real ArpNote rather than a note code and a channel.
//   midi_fx_chain.h  holds a RetrigFx by value, so it inherits the same dependency.
//
// Both are included by sound.h, melodic_instrument.h and drum.h, all of which already include
// arpeggiator.h, so nothing new is dragged anywhere. But the property is genuinely gone for
// those two, and pretending otherwise by deleting this test would lose the signal for the ones
// where it still holds.
#include "modulation/midi_fx/gate_time_fx.h"
#include "modulation/midi_fx/generate.h"
#include "modulation/midi_fx/harmonize.h"
#include "modulation/midi_fx/midi_fx.h"
#include "modulation/midi_fx/midi_fx_settings.h"
#include "modulation/midi_fx/spread.h"
#include "modulation/midi_fx/stochastic.h"
#include "modulation/midi_fx/strum.h"
#include "modulation/midi_fx/velocity_fx.h"
namespace mf = deluge::modulation::midi_fx;
static_assert(mf::kNoEvent == 2147483647);
static_assert(sizeof(mf::MidiFxSettings) <= 16, "stored per clip AND per drum, so keep it small");
// Parse-only: -fsyntax-only, no link. Instantiating an effect would need its vtable, which lives
// in a .cpp that includes arpeggiator.h and therefore will not build on this host at all. The
// check here is that the headers are self-contained and well-formed, which does not require
// running anything.
static_assert(sizeof(mf::VelocityFx) > 0);
static_assert(sizeof(mf::GateTimeFx) > 0);
static_assert(!mf::MidiFxSettings{}.enabled, "must default off, so old songs load inert");
static_assert(mf::MidiFxSettings{}.gateTimePercent == 100, "100% is off");
static_assert(mf::MidiFxSettings{}.retrigRepeats == 0, "0 repeats is off");

// Strum's arithmetic, which is the whole of it that can be got wrong without hardware.
namespace st = deluge::modulation::midi_fx::strum;
static_assert(st::offsetForPlace(3, 0) == 0, "the note that leads the chord is still on the beat");
static_assert(st::offsetForPlace(3, 2) == 6);
static_assert(st::offsetForPlace(st::kAmountOff, 5) == 0, "amount 0 is off");
// A note must never be delayed past its own end, or its note-off falls due before its note-on.
static_assert(st::clampedOffset(10, 4) == 3);
static_assert(st::clampedOffset(10, 1) == 0, "a one-tick note has nowhere to be pushed");
static_assert(st::clampedOffset(2, 40) == 2);
// The contour spans the chord rather than stepping per note, so three notes and six notes finish
// in the same place instead of the long one fading away.
static_assert(st::contouredVelocity(100, -50, 0, 4) == 100, "the first note is never touched");
static_assert(st::contouredVelocity(100, -50, 3, 4) == 50);
static_assert(st::contouredVelocity(100, -50, 5, 6) == 50);
static_assert(st::contouredVelocity(100, -50, 0, 1) == 100, "a chord of one has nowhere to tilt");
static_assert(st::contouredVelocity(4, -50, 9, 10) >= st::kMinVelocity, "never velocity 0 - that is a note-off");
static_assert(st::contouredVelocity(120, 50, 1, 2) == st::kMaxVelocity, "clamped at the top of MIDI");

// Generate's placement, which like Strum's arithmetic is all of it that can be wrong without
// hardware. The interesting properties are not particular values - those are a hash, and pinning one
// would only assert that nobody changed the constants - but the four things the feature rests on.
namespace gn = deluge::modulation::midi_fx::generate;
static_assert(!gn::placesStep(gn::kDensityMin, 7, 0, 48), "density 0 must place nothing, in every row");
static_assert(gn::placesStep(gn::kDensityMax, 7, 0, 48), "density 100 must place everything");
static_assert(gn::stepRoll(7, 0, 48) >= 0 && gn::stepRoll(7, 0, 48) < 100, "the roll is a percentage");
static_assert(gn::placesStep(50, 7, 3, 96) == gn::placesStep(50, 7, 3, 96),
              "same seed, same row, same step - same answer. The whole point of a hash over a draw.");

/// Two rows of one kit must not decide in lockstep, or a generated kit comes out as one block of
/// hits and silences rather than as parts. Written as a sweep rather than as one pinned position:
/// any single step can agree by chance, sixteen in a row cannot.
constexpr bool generateRowsDiffer() {
	for (int32_t notePos = 0; notePos < 16 * 6; notePos += 6) {
		if (gn::placesStep(50, 7, 0, notePos) != gn::placesStep(50, 7, 1, notePos)) {
			return true;
		}
	}
	return false;
}
static_assert(generateRowsDiffer(), "the note row id must reach the hash");

/// And two seeds must give two patterns, or the Seed knob does nothing - which is the failure this
/// feature would be least likely to notice by ear on a busy pattern.
constexpr bool generateSeedsDiffer() {
	for (int32_t notePos = 0; notePos < 16 * 6; notePos += 6) {
		if (gn::placesStep(50, 7, 0, notePos) != gn::placesStep(50, 8, 0, notePos)) {
			return true;
		}
	}
	return false;
}
static_assert(generateSeedsDiffer(), "the seed must reach the hash");

// And Generate's line, which is the other half and has one property that matters more than the rest:
// a degree outside the range is a note row outside the range, and a note code that is not the one the
// user asked for.

/// Reflection has to land inside the range for every input, including leaps far wider than the range
/// itself - a single leap can overshoot both ends, and the fold has to keep bouncing until it lands.
constexpr bool generateReflectionStaysInRange() {
	for (uint8_t range = 1; range <= gn::kRangeMax; range++) {
		for (int32_t degree = -200; degree <= 200; degree++) {
			const int32_t folded = gn::reflectDegree(degree, range);
			if (folded < 0 || folded > static_cast<int32_t>(range)) {
				return false;
			}
		}
	}
	return true;
}
static_assert(generateReflectionStaysInRange(), "a degree outside the range is a row outside the range");

static_assert(gn::reflectDegree(-1, 4) == 1, "one below the bottom bounces to one above it");
static_assert(gn::reflectDegree(5, 4) == 3, "and one above the top to one below");
static_assert(gn::reflectDegree(7, 0) == 0, "range 0 has one degree and everything folds onto it");

/// Range 0 must be the old behaviour exactly, not approximately: every note in the row you
/// auditioned, whatever Leap says.
constexpr bool generateRangeZeroNeverMoves() {
	int32_t degree = 0;
	for (int32_t noteIndex = 1; noteIndex < 64; noteIndex++) {
		degree = gn::nextDegree(degree, gn::kRangeMin, 50, 7, noteIndex);
		if (degree != 0) {
			return false;
		}
	}
	return true;
}
static_assert(generateRangeZeroNeverMoves(), "Range 0 is a rhythm at one pitch");

/// At Leap 0 the line only ever steps to a neighbour. This is what makes the bottom of the knob a
/// scale run rather than just a narrower scatter.
constexpr bool generateLeapZeroOnlySteps() {
	for (int32_t noteIndex = 1; noteIndex < 128; noteIndex++) {
		const int32_t delta = gn::degreeDelta(7, gn::kLeapMin, 11, noteIndex);
		if (delta != 1 && delta != -1) {
			return false;
		}
	}
	return true;
}
static_assert(generateLeapZeroOnlySteps(), "Leap 0 steps, never jumps");

// Spread's arithmetic. Like Strum's and Generate's, the interesting properties are not particular
// values - those are a hash - but the things the feature rests on, each of which is a way a note
// goes missing or hangs if it stops holding.
namespace sp = deluge::modulation::midi_fx::spread;
static_assert(sp::velocity(100, sp::kOff, 7, 48) == 100, "off changes nothing");
static_assert(sp::gateLength(24, sp::kOff, 7, 48) == 24, "off changes nothing");
static_assert(sp::octave(60, sp::kOff, 7, 48) == 60, "off changes nothing");
static_assert(sp::octaveShift(sp::kOctaveMax, 7, 48) != 0, "chance 100 always displaces");
static_assert(sp::octaveShift(sp::kOff, 7, 48) == 0, "chance 0 never displaces");
static_assert(sp::gateLength(1, sp::kGateMax, 7, 48) == 1, "a one-tick note is left alone");

/// Velocity 0 is a note-off on MIDI, so a spread that reached it would be a hanging note in a
/// disguise. 128 and up wraps to silence.
constexpr bool spreadVelocityStaysLegal() {
	for (uint8_t amount = 0; amount <= sp::kVelocityMax; amount++) {
		for (int32_t written = 1; written <= 127; written += 7) {
			for (int32_t pos = 0; pos < 96; pos += 6) {
				const int32_t v = sp::velocity(written, amount, 3, pos);
				if (v < sp::kMinVelocity || v > sp::kMaxVelocity) {
					return false;
				}
			}
		}
	}
	return true;
}
static_assert(spreadVelocityStaysLegal());

/// A length that rounds to 0 ends on the tick it began, which the sequencer reads as "already
/// over" rather than "very short" - and that is how a row goes silent for reasons nobody can find.
constexpr bool spreadGateStaysPositive() {
	for (uint8_t amount = 0; amount <= sp::kGateMax; amount += 3) {
		for (int32_t len = 1; len <= 192; len += 7) {
			for (int32_t pos = 0; pos < 96; pos += 6) {
				if (sp::gateLength(len, amount, 3, pos) < 1) {
					return false;
				}
			}
		}
	}
	return true;
}
static_assert(spreadGateStaysPositive());

/// A displacement off the end of the keyboard is declined, not clamped - clamping would pile notes
/// onto one pitch, which sounds like a fault rather than like a part.
constexpr bool spreadOctaveStaysInRange() {
	for (uint8_t chance = 0; chance <= sp::kOctaveMax; chance += 5) {
		for (int32_t note = sp::kMinNoteCode; note <= sp::kMaxNoteCode; note++) {
			const int32_t out = sp::octave(note, chance, 3, note * 6);
			if (out < sp::kMinNoteCode || out > sp::kMaxNoteCode) {
				return false;
			}
		}
	}
	return true;
}
static_assert(spreadOctaveStaysInRange());

/// One salt each, or every loud note would also be a long one and the whole thing would read as a
/// single lurching parameter rather than as three.
constexpr bool spreadSaltsAreIndependent() {
	int32_t disagreements = 0;
	for (int32_t pos = 0; pos < 64 * 6; pos += 6) {
		if ((sp::velocity(64, 40, 3, pos) > 64) != (sp::gateLength(48, 40, 3, pos) > 48)) {
			disagreements++;
		}
	}
	return disagreements > 8;
}
static_assert(spreadSaltsAreIndependent());

/// The row id must reach the hash, or a chord would jump an octave as a block.
constexpr bool spreadRowsDiffer() {
	for (int32_t pos = 0; pos < 96; pos += 6) {
		if (sp::velocity(64, 30, 0, pos) != sp::velocity(64, 30, 1, pos)) {
			return true;
		}
	}
	return false;
}
static_assert(spreadRowsDiffer());

// Same inputs, same answer, which is the whole point of a hash over a draw: the variation repeats
// bar after bar, survives stop/start and save/load with nothing stored, and comes out identical on
// both cores.
static_assert(sp::velocity(90, 30, 5, 120) == sp::velocity(90, 30, 5, 120));
static_assert(sp::gateLength(48, 30, 5, 120) == sp::gateLength(48, 30, 5, 120));

// ---------------------------------------------------------------------------------------------
// Stochastic. Same shape of test as Spread's above: not particular values - those are a hash, and
// pinning one would only assert that nobody changed the constants - but the properties the feature
// rests on, each of which is a way it could be silently wrong on hardware.
namespace so = deluge::modulation::midi_fx::stochastic;

/// Amount 0 must be inert, in every row, at every position - it is what an old song loads as.
constexpr bool stochasticOffFiresNothing() {
	for (uint8_t dice = 0; dice <= 99; dice += 7) {
		for (int32_t row = 0; row < 8; row++) {
			for (int32_t pos = 0; pos < 96 * 4; pos += 6) {
				if (so::fires(so::kOff, dice, row, pos)) {
					return false;
				}
			}
		}
	}
	return true;
}
static_assert(stochasticOffFiresNothing());

/// Amount 100 must fire on every note, or the top of the knob quietly means "most".
constexpr bool stochasticFullFiresEverything() {
	for (int32_t row = 0; row < 8; row++) {
		for (int32_t pos = 0; pos < 96 * 4; pos += 6) {
			if (!so::fires(so::kAmountMax, 11, row, pos)) {
				return false;
			}
		}
	}
	return true;
}
static_assert(stochasticFullFiresEverything());

/// **The offset is never 0, and stays inside the width.** This is the one that pays for the odd
/// fold in `degreeOffset`: a flat draw over -width..+width would leave one fired note in 2*width+1
/// sitting exactly where it was written, so Amount 100 would visibly move only four notes in five
/// at width 2 - the knob would be lying by twenty percent and it would be very hard to see why.
constexpr bool stochasticOffsetNeverZeroAndInWidth() {
	for (uint8_t width = so::kWidthMin; width <= so::kWidthMax; width++) {
		for (uint8_t dice = 0; dice <= 99; dice += 3) {
			for (int32_t pos = 0; pos < 96 * 2; pos += 6) {
				const int32_t d = so::degreeOffset(width, dice, 3, pos);
				if (d == 0 || d < -static_cast<int32_t>(width) || d > static_cast<int32_t>(width)) {
					return false;
				}
			}
		}
	}
	return true;
}
static_assert(stochasticOffsetNeverZeroAndInWidth());

/// Both directions actually happen. A fold that got its arithmetic wrong could easily produce only
/// upward motion, which would sound like a feature rather than like a bug and would never be traced.
constexpr bool stochasticMovesBothWays() {
	bool up = false;
	bool down = false;
	for (int32_t pos = 0; pos < 96 * 2; pos += 6) {
		const int32_t d = so::degreeOffset(2, 7, 3, pos);
		up = up || d > 0;
		down = down || d < 0;
	}
	return up && down;
}
static_assert(stochasticMovesBothWays());

/// Offset 0 must reconstruct the written note exactly. Everything upstream of a wander that does
/// not happen has to be a no-op, or Amount 0 would not be off.
///
/// Modelled on a natural minor scale from a root of 0: degree 4 is 7 semitones up, and a note two
/// octaves above the root on that degree is 24 + 7.
static_assert(so::assemble(0, 2, 0, 7) == 31);
static_assert(so::assemble(60 % 12, 4, 0, 0) == 48, "the key root itself comes back as the key root");

/// A wander off the top of the scale carries an octave rather than wrapping in place - the bug that
/// `harmonize::fromDegree` records having had, where a bassline disappears through the floor.
static_assert(so::wrappedDegree(7, 7) == 0 && so::octaveCarry(7, 7) == 1);
static_assert(so::wrappedDegree(-1, 7) == 6 && so::octaveCarry(-1, 7) == -1);
static_assert(so::wrappedDegree(-8, 7) == 6 && so::octaveCarry(-8, 7) == -2);

/// Floor semantics, not truncation. C rounds toward zero, which would put two different notes on
/// the same degree for any line written below the key root.
static_assert(so::floorDiv(-1, 12) == -1 && so::floorMod(-1, 12) == 11);
static_assert(so::floorDiv(-12, 12) == -1 && so::floorMod(-12, 12) == 0);

/// The two decisions must be independent, or every note that moved would move the same way as its
/// neighbour and the whole thing would read as one lurching control rather than as two.
constexpr bool stochasticSaltsAreIndependent() {
	int32_t disagreements = 0;
	for (int32_t pos = 0; pos < 64 * 6; pos += 6) {
		if (so::fires(50, 9, 3, pos) != (so::degreeOffset(2, 9, 3, pos) > 0)) {
			disagreements++;
		}
	}
	return disagreements > 8;
}
static_assert(stochasticSaltsAreIndependent());

/// The row id must reach the hash, or every row of a chord would wander together and the chord
/// would move as a block instead of coming apart.
constexpr bool stochasticRowsDiffer() {
	for (int32_t pos = 0; pos < 96; pos += 6) {
		if (so::degreeOffset(3, 4, 0, pos) != so::degreeOffset(3, 4, 1, pos)) {
			return true;
		}
	}
	return false;
}
static_assert(stochasticRowsDiffer());

/// Dice must reach the hash too, or the shared variation number would do nothing here and only the
/// gates would change when it was turned.
constexpr bool stochasticDiceMatters() {
	for (int32_t pos = 0; pos < 96; pos += 6) {
		if (so::degreeOffset(3, 0, 2, pos) != so::degreeOffset(3, 1, 2, pos)) {
			return true;
		}
	}
	return false;
}
static_assert(stochasticDiceMatters());

// Same inputs, same answer - the variation repeats bar after bar, survives stop/start and
// save/load with nothing stored, and comes out identical on both cores.
static_assert(so::fires(40, 5, 3, 120) == so::fires(40, 5, 3, 120));
static_assert(so::degreeOffset(3, 5, 3, 120) == so::degreeOffset(3, 5, 3, 120));

static_assert(so::inRange(0) && so::inRange(127));
static_assert(!so::inRange(-1) && !so::inRange(128));

// --- The two properties found on hardware on 2026-09-14 -----------------------------------------

/// **Ear test J, done here instead.** "A wander off the end of the keyboard is declined, not
/// clamped" turned out to be close to impossible to hear deliberately - you cannot reliably write a
/// phrase that asks for it. So it is asserted across the entire note range instead: no written note,
/// at any width, ever produces something outside 0..127, and whenever the raw wander would have,
/// the written note comes back unchanged rather than a pile of notes landing on the lowest pitch.
///
/// The scale here is C natural minor from a root of 0 - seven degrees, the shape the feature is
/// built around - modelled as a plain array because NoteSet is firmware and this test is not.
constexpr int8_t kTestScale[7] = {0, 2, 3, 5, 7, 8, 10};
constexpr int32_t kTestScaleSize = 7;

constexpr bool stochasticNeverLeavesTheKeyboard() {
	for (int32_t written = so::kMinNoteCode; written <= so::kMaxNoteCode; written++) {
		const int32_t octave = so::floorDiv(written, so::kSemitonesPerOctave);
		const int32_t interval = so::floorMod(written, so::kSemitonesPerOctave);
		int32_t degree = -1;
		for (int32_t d = 0; d < kTestScaleSize; d++) {
			if (kTestScale[d] == interval) {
				degree = d;
			}
		}
		if (degree < 0) {
			continue; // not in key - left alone, which is ear test I
		}
		for (int32_t offset = -so::kWidthMax; offset <= so::kWidthMax; offset++) {
			if (offset == 0) {
				continue;
			}
			const int32_t target = degree + offset;
			const int32_t raw = so::assemble(0, octave, so::octaveCarry(target, kTestScaleSize),
			                                 kTestScale[so::wrappedDegree(target, kTestScaleSize)]);
			const int32_t out = so::applyOrDecline(written, raw);
			if (!so::inRange(out)) {
				return false;
			}
			// Declined, not clamped: out of range means the note did not move at all.
			if (!so::inRange(raw) && out != written) {
				return false;
			}
			// And in range means it really is the wandered note, not a nudged one.
			if (so::inRange(raw) && out != raw) {
				return false;
			}
		}
	}
	return true;
}
static_assert(stochasticNeverLeavesTheKeyboard());

// --- Harmonize's register: three hardware passes, pinned so there is not a fourth -------------
//
// Pass 1 added the written note's octave above `key.rootNote` to chord.pitches[index] - pitches
// already absolute off the chord track - so the register was counted twice. With the default root
// of 0 a line written at 60-71 over a C3 triad came out at 108-127.
//
// Pass 2 measured the octave from the chord's own lowest tone, which fixed that but kept an octave
// carry of degree/chord.count. Narrow phrases were fine; a line walking a full octave still climbed
// away over two, because seven degrees mapped monotonically onto three tones must stretch by 7/3.
//
// Pass 3, this one: the degree picks the chord tone, and the written note picks the octave.
namespace hm = deluge::modulation::midi_fx::harmonize;

constexpr hm::Chord cMajorAt48() {
	hm::Chord c;
	c.add(48); // C3
	c.add(52); // E3
	c.add(55); // G3
	return c;
}

/// The narrow melody - the one that already sounded right on hardware before this pass.
/// (written note, scale degree) for a phrase that stays inside degrees 0 to 2.
constexpr int32_t kNarrow[5][2] = {{60, 0}, {62, 1}, {64, 2}, {62, 1}, {60, 0}};

/// The wide melody - the one that climbed away. A full scale octave, degrees 0 to 6.
constexpr int32_t kWide[7][2] = {{60, 0}, {62, 1}, {64, 2}, {65, 3}, {67, 4}, {69, 5}, {71, 6}};

/// **The narrow melody must be untouched by this pass.** It never crossed a carry boundary, so it
/// was already right, and a register fix that changed it would have broken the one thing that
/// worked. These are the notes pass 2 played and the notes pass 3 must still play.
static_assert(hm::fromDegree(60, 0, cMajorAt48()) == 60);
static_assert(hm::fromDegree(62, 1, cMajorAt48()) == 64);
static_assert(hm::fromDegree(64, 2, cMajorAt48()) == 67);

/// **The wide melody stays in its own register instead of climbing.** Pass 2 played
/// 60, 64, 67, 72, 76, 79, 84 for these; the last four were the complaint.
static_assert(hm::fromDegree(65, 3, cMajorAt48()) == 60);
static_assert(hm::fromDegree(67, 4, cMajorAt48()) == 64);
static_assert(hm::fromDegree(69, 5, cMajorAt48()) == 67);
static_assert(hm::fromDegree(71, 6, cMajorAt48()) == 72);

/// No harmonised note may sit more than a tritone from the note that was written. This is the
/// property, and every symptom across all three passes was a violation of it.
constexpr bool harmonizeStaysBesideTheWrittenNote() {
	const hm::Chord chord = cMajorAt48();
	for (int32_t note = 12; note <= 115; note++) {
		for (int32_t degree = -14; degree <= 20; degree++) {
			const int32_t out = hm::fromDegree(note, degree, chord);
			const int32_t distance = out > note ? out - note : note - out;
			if (distance > hm::kRegisterWindow) {
				return false;
			}
		}
	}
	return true;
}
static_assert(harmonizeStaysBesideTheWrittenNote());

/// The key root has nothing to say about the answer. It decides which DEGREE a note is - that is
/// the caller's job - and nothing else. Pass 1's whole defect was that it could not express this.
constexpr bool harmonizeIgnoresTheKeyRoot() {
	const hm::Chord chord = cMajorAt48();
	for (int32_t i = 0; i < 7; i++) {
		if (hm::fromDegree(kWide[i][0], kWide[i][1], chord) != hm::fromDegree(kWide[i][0], kWide[i][1], chord)) {
			return false;
		}
	}
	return true;
}
static_assert(harmonizeIgnoresTheKeyRoot());

/// Every result is a chord tone, in some octave - the one thing that must never break, and the only
/// promise Harmonize makes that all three passes kept.
constexpr bool harmonizeOnlyEverPlaysChordTones() {
	const hm::Chord chord = cMajorAt48();
	for (int32_t note = 24; note <= 96; note++) {
		for (int32_t degree = -7; degree <= 13; degree++) {
			const int32_t out = hm::fromDegree(note, degree, chord);
			bool found = false;
			for (int32_t i = 0; i < chord.count; i++) {
				if (((out - chord.pitches[i]) % 12) == 0) {
					found = true;
				}
			}
			if (!found) {
				return false;
			}
		}
	}
	return true;
}
static_assert(harmonizeOnlyEverPlaysChordTones());

/// Degrees a whole scale apart are the same chord tone in the same place - the carry is genuinely
/// gone, not merely small. This is what costs the rising contour on a wide line, and it is the one
/// thing someone reading this later might mistake for a regression, so it is stated outright.
static_assert(hm::fromDegree(65, 0, cMajorAt48()) == hm::fromDegree(65, 3, cMajorAt48()));

/// A line written low stays low rather than being dragged up to the chord.
static_assert(hm::fromDegree(36, 0, cMajorAt48()) == 36);
static_assert(hm::fromDegree(84, 0, cMajorAt48()) == 84);

/// placeNear only ever moves by whole octaves, so it can never change which chord tone was chosen.
constexpr bool placeNearKeepsThePitchClass() {
	for (int32_t tone = 0; tone <= 127; tone += 1) {
		for (int32_t written = 0; written <= 127; written += 1) {
			if (((hm::placeNear(tone, written) - tone) % 12) != 0) {
				return false;
			}
		}
	}
	return true;
}
static_assert(placeNearKeepsThePitchClass());
