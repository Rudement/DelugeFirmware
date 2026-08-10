/*
 * Copyright © 2026
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
#include "deluge/util/fixedpoint.h"
#include "deluge/util/functions.h"
#include <algorithm>

namespace deluge::dsp {

/*
 * Sear: a cubic soft-clipper with automatic level matching, followed by a one-pole tilt
 * tone control.
 *
 * Was called Heat up to 2026-08-08. The rename went all the way through, XML attributes
 * included ("sear", "searTone"), which was safe only because the effect had never shipped
 * and no saved song referenced the old tags. That window is now closed: from here on,
 * changing those strings orphans the param in every song that uses it, and a rename would
 * need a read-side alias for the old spelling instead.
 *
 * Deliberately shaped like foldBufferPolyApproximation() in util.hpp — same q31
 * conventions, same (startSample, endSample) pointer-range walk, same do/while. Read
 * fold() there first if the fixed-point idiom is unfamiliar.
 *
 * FIXED-POINT RULE used throughout: multiply_32x32_rshift32(a, b) yields a*b/2, not
 * a*b, because it drops 32 bits of a 64-bit product held in q31. Every multiply below
 * is therefore followed by a shift that puts the result back on scale. Getting this
 * wrong is silent — the stage just ends up 6 dB down — so each transfer curve is stated
 * in a comment and was checked numerically against its ideal before committing.
 *
 * WHY CUBIC AND NOT TANH. Sound::saturate() already gives a tanh curve via lookup, so a
 * second tanh would duplicate an existing character. The cubic has a harder knee and a
 * true ceiling at ±1, which reads as "pedal" beside the softer saturate(). It is also
 * three multiplies with no table lookup, which matters when this runs per voice.
 *
 * ALIASING — a genuine limitation, not an oversight. There is no oversampling, and the
 * cubic generates harmonics to 3f, so bright material driven hard WILL alias.
 * Oversampling per-voice on a 400 MHz Cortex-A9 was judged too expensive. The call site
 * puts Sear AFTER the ladder filter, so rolling the LPF down also tames the aliasing.
 */

// ---------------------------------------------------------------------------
// Soft clipper
// ---------------------------------------------------------------------------

/// Transfer curve: y = 1.5x - 0.5x^3, evaluated on [-1, 1].
///
/// Slope at the origin is 1.5, and y(±1) = ±1 exactly. Monotonic over the whole domain —
/// which matters, because the very similar 1.5x - x^3 folds back above x = 0.707 and
/// would turn this into a second wavefolder rather than a clipper.
///
/// Callers must saturate x into [-1, 1] first; the curve is only monotonic there.
///
/// Also used by gristle.hpp for its Dirt stage, so the name stays generic.
inline q31_t softClipCubic(q31_t x) {
	constexpr q31_t THREE_QUARTERS_Q31 = 0.75 * ONE_Q31;
	constexpr q31_t ONE_QUARTER_Q31 = 0.25 * ONE_Q31;

	// Each multiply_32x32_rshift32 halves the scale, so double after each to keep
	// x2 and x3 as true q31 powers of x.
	q31_t x2 = 2 * multiply_32x32_rshift32(x, x);
	q31_t x3 = 2 * multiply_32x32_rshift32(x2, x);

	// 4 * (0.75x/2 - 0.25x^3/2) == 1.5x - 0.5x^3. The <<2 is the put-it-back-on-scale
	// step; it saturates because y(±1) lands exactly on the rail.
	return lshiftAndSaturateUnknown(
	    multiply_32x32_rshift32(THREE_QUARTERS_Q31, x) - multiply_32x32_rshift32(ONE_QUARTER_Q31, x3), 2);
}

// ---------------------------------------------------------------------------
// Automatic level matching
// ---------------------------------------------------------------------------

/*
 * WHY THERE IS NO STATIC MAKEUP CONSTANT ANY MORE.
 *
 * Two earlier versions both tried to solve this with a single number multiplied in after
 * the clipper, and both failed, in opposite directions:
 *
 *   makeup = 2/3        anchored SMALL-SIGNAL gain, cancelling the cubic's slope of 1.5 at
 *                       the origin. But anything loud enough to reach the knee leaves the
 *                       curve pinned at ±1 and was then still multiplied by 0.667 — a flat
 *                       3.5 dB drop. The knob audibly ducked instead of dirtying.
 *
 *   makeup = 1 - level  anchored PEAK response at unity, sweeping 1.0 down to 0.75. Peaks
 *                       stayed put, but peaks are not loudness. Measured on a saw, RMS rose
 *                       +4 dB by knob 1 and +7.8 dB by knob 15 while this pulled back only
 *                       2.5 dB at the very top. Reported from hardware 2026-08-08: "super
 *                       hot, I have to turn the volume down past 15."
 *
 * A saturating curve cannot be transparent in peak AND in RMS at once, and no constant can
 * be, because the right correction depends on how hard the input is already hitting the
 * knee. Measured compensation needed to hold RMS flat, by input peak:
 *
 *     input peak    0.125    0.25     0.5     0.9
 *     knob 15      -16.7   -13.2    -7.8    -3.0   dB
 *     knob 50      -18.5   -12.5    -6.5    -1.4   dB
 *
 * Nearly 14 dB of spread at one knob position. That is the whole argument for measuring
 * rather than tabulating.
 *
 * WHAT THIS DOES INSTEAD. Two one-pole followers track mean |sample| either side of the
 * clipper, and the corrective gain is their ratio. Mean-abs, not RMS: it needs no squares
 * and no square root, and it was checked against a true RMS detector across sine and saw at
 * four input levels and eleven knob positions — the two agree to within 0.05 dB everywhere,
 * so the cheap one wins.
 *
 * THIS IS NOT A COMPRESSOR AND MUST NOT BECOME ONE. It tracks the RATIO of two levels, not
 * a level. When the input gets quieter the output gets quieter with it and the ratio barely
 * moves, so note dynamics pass through untouched; only the amount the clipper is ADDING is
 * corrected. That is also why the detector can afford to be slow.
 */

/// ~1024 samples, about 23 ms at 44.1 kHz.
///
/// THIS IS A LOWER BOUND SET BY THE LOWEST NOTE, not a taste setting. The first working
/// version measured each render block instead, and a render block is at most 80 samples —
/// a fraction of one cycle at 55 Hz. The detector then followed the waveform within the
/// cycle and the gain wobbled at the note frequency, which measured as a 0.4 dB spread
/// between 55 Hz and 1 kHz on otherwise identical material. At 1024 samples the same test
/// agrees to 0.03 dB. Shortening this brings the wobble back, starting at the bottom of
/// the keyboard where it is hardest to notice on a bench and easiest to notice in a mix.
constexpr int32_t kSearDetectorShift = 10;

/// ~128 samples, about 2.9 ms. How fast the applied gain glides toward the target. Applied
/// per sample rather than per block so that a change in the target cannot step, which would
/// click. Slow enough not to chase the detector's own ripple, fast enough to follow a filter
/// sweep into the clipper.
constexpr int32_t kSearGainShift = 7;

/// Length of the warm-up ramp, in samples. Equal to the detector's own time constant.
constexpr uint32_t kSearWarmupSamples = 1u << kSearDetectorShift;

/*
 * COLD STARTS, and why the obvious fixes are wrong.
 *
 * A measuring stage has nothing to measure at the instant it starts, so the question is what
 * it does for the first cycle or so of a note. Three things conspire here, and each one was
 * arrived at by breaking it:
 *
 * 1. A one-pole follower started from zero reads far too low for its first time constant, so
 *    a 23 ms detector would leave the whole attack of a note uncorrected — measured at +8 dB
 *    over the first millisecond, which is exactly the part of the note that is loudest.
 *
 *    Fixed by ramping the detector's time constant: it starts at one sample and doubles as
 *    the sample count doubles until it reaches kSearDetectorShift. While ramping, the
 *    follower is approximately a running mean over every sample seen so far, which is the
 *    unbiased estimate — no window has to be guessed in advance. Costs one predictable, and
 *    after 23 ms permanently untaken, branch per sample.
 *
 * 2. SEEDING the followers from the first render block's mean was tried before the ramp and
 *    is worse than it looks. A block is at most 80 samples and one cycle of a 220 Hz note is
 *    200, so the seed measures whichever fifth of the waveform the note happened to start on.
 *    That put the first millisecond +4.05 dB above steady state, decaying over ~12 ms, with
 *    the size of the thump depending on note phase. Do not reinstate it.
 *
 * 3. The gain is GLIDED and never snapped — see the note at the bottom of searBuffer(). An
 *    early estimate can be not merely inaccurate but absurd, and gliding is what stops an
 *    absurd one reaching the output.
 *
 * None of that removes the cold start, it only makes it smooth: the cycle-average of a
 * waveform is not knowable until a cycle has been seen, which is 4.5 ms at 220 Hz and 18 ms
 * at 55 Hz. What removes it is not starting cold — see SearLevel below.
 */

/// Persistent auto-level state.
///
/// ONE INSTANCE COVERS BOTH CHANNELS, and that is deliberate — the exact opposite of the
/// tone stage below, which needs one per channel. The tone filter holds a signal history,
/// so sharing it across an interleaved buffer combs. This holds a GAIN, and L and R must
/// get the same gain or the stereo image walks around as the drive changes.
///
/// WHERE THIS LIVES MATTERS AS MUCH AS WHAT IT HOLDS. A Voice owns the live copy, but a Voice
/// does not survive a note — Sound::acquireVoice() placement-constructs one out of a pool on
/// every note-on — so a Voice-only instance is cold on every note, which is the +8 dB case
/// above. Sound therefore keeps a seed (Sound::searLevelSeed) that each Voice copies on
/// construction and writes back after each block. Only the first note a Sound ever plays is
/// cold; measured warm-start error on every subsequent note is 0.000 dB.
struct SearLevel {
	q31_t envIn;     ///< mean |sample| entering the clipper
	q31_t envOut;    ///< mean |sample| leaving it, before correction
	q31_t gain;      ///< correction currently applied
	uint32_t warmup; ///< samples since the stage was last enabled, held at kSearWarmupSamples
};

/// Cold-start initialiser. Called once per Sound, NOT per note — see SearLevel above.
inline void searLevelReset(SearLevel& state) {
	state.envIn = 0;
	state.envOut = 0;
	state.gain = ONE_Q31;
	state.warmup = 0;
}

/*
 * Sear (drive stage).
 *
 * Pre-gain sweeps 1x to 256x on an EXPONENTIAL taper — gain = 2^(8 * level).
 *
 * The first hardware test used a linear 1x..33x law and was, in George's words, "not very
 * audible". Two things were wrong with it. It was ~4x weaker than the wavefolder sitting
 * next to it — foldBufferPolyApproximation() shifts its product left by 8, this shifted by
 * 6, which is 12 dB less drive:
 *
 *     level 0.25   fold x32    old sear x9
 *     level 0.50   fold x64    old sear x17
 *     level 1.00   fold x128   old sear x33
 *
 * And a linear taper wastes the knob: by 25% you are already most of the way to the
 * maximum in perceptual terms, so the top three quarters of the travel all sound alike.
 *
 * The exponential taper fixes both. `s` is the integer part and `f` the fraction; 2^s * (1 + f)
 * is the standard linear-interpolation-in-the-exponent approximation, and it is continuous at
 * every octave boundary because 2^s * 2 == 2^(s+1).
 *
 * Measured against the REAL param pipeline (not the DSP in isolation — that was the earlier
 * mistake), knob position 0..50:
 *
 *     knob   0    3    5   10   15   20   25   30   35    40    45    50
 *     gain  1.0  1.5  1.8  3.2  5.6  9.6   16   29   51    90   154   256
 *
 * Those figures are the ORIGINAL taper. Reported too tame on hardware 2026-08-08, so the curve
 * is now scaled by 1.5 in exponent space (see kSearDriveBoost in searBuffer). Recomputed through
 * the same fixed-point path, same knob positions:
 *
 *     knob   0    3    5   10   15   20   25   30   35    40    45    50
 *     gain  1.6  2.1  2.8  4.7  7.9   14   25   44   76   127   228   406
 *
 * The realised ratio wobbles between 1.42x and 1.59x rather than sitting exactly on 1.5x, and
 * the ceiling lands on ~406x rather than the nominal 384x. That is the pre-existing linear-
 * interpolation-in-the-exponent approximation, not a new error: 2^s * (1 + f) overshoots 2^(s+f)
 * by up to 6% mid-octave, and the offset changes where in each octave a given knob position
 * falls. Correcting it would mean replacing the interpolation, which is not worth a cycle here.
 *
 * NOTE ON THE BOOST AND AUTO-LEVEL. This offset raises PRE-gain into the clipper, and the level
 * matcher below then removes the loudness it would otherwise add. That is the intent: the extra
 * drive lands as saturation rather than volume, which is what "too tame" actually meant — the
 * old curve was quieter AND cleaner, and only the cleanliness is a fault worth fixing.
 *
 * Roughly a constant ratio per step, audible by knob 3-5, and no dead zone. Getting here needed
 * BOTH the >>26 below and moving LOCAL_SEAR out of the patcher's volume block (see param.h) —
 * the volume block's parabola alone held the entire bottom half between 1.0x and 2.0x.
 *
 * TWO PASSES, NOT ONE. The gain for a block cannot be known until the block has been clipped,
 * so pass 1 clips and measures and pass 2 applies. The alternative — correcting with the
 * PREVIOUS block's gain — was tried and is not worth it: it saves one walk of a buffer that
 * the tone stage is about to walk again anyway, and it puts a block of latency into the one
 * part of the signal path that has to track a filter sweep.
 *
 * The drive stage itself is memoryless, so an interleaved stereo buffer can be passed
 * straight through as one long range — exactly the shortcut foldBufferPolyApproximation()
 * takes, and safe here for the correction too, per the note on SearLevel.
 */
inline void searBuffer(q31_t* startSample, q31_t* endSample, q31_t level, SearLevel& state) {
	if (level <= 0) {
		// Bypass — mirrors how LOCAL_FOLD is gated at the call site. Rewind the warm-up so that
		// turning the knob back up re-measures rather than resuming a stale gain.
		state.warmup = 0;
		return;
	}

	// +50% drive, applied as a constant offset in exponent space so the taper SHAPE and the
	// knob feel are untouched — every position gets ~1.5x the pre-gain it had before, and the
	// top of the sweep goes 256x -> ~406x (see the table above for why not exactly 384x).
	//
	// The offset is log2(1.5) expressed in `level`'s units, where one octave is 2^26 (see the
	// shift note below). Offsetting the exponent rather than multiplying the sample keeps the
	// octave/fraction split exact and costs one add per buffer, not per sample.
	//
	// KNOWN STEP AT THE BOTTOM OF THE KNOB: gain now starts at ~1.5x the instant Sear comes off
	// its stop, where it used to start at 1.0x, because the offset applies at level 1 as much as
	// at full scale. That is inherent to a uniform boost. Note the auto-leveller LARGELY MASKS
	// this one: it is a pre-gain step, so the correction tracks it out within a few ms, leaving
	// a brief timbral jump rather than a loudness jump. If it ever needs removing outright,
	// scale `level` by 1.073 instead of adding this constant: same 384x ceiling, no step, but
	// mid-knob positions then get less than the full 1.5x.
	//
	// Headroom: `level` tops out just under 2^29, so `boosted` stays well inside int32 and
	// `octaves` reaches 8 rather than 7 at full drive. lshiftAndSaturateUnknown() handles 8
	// fine; softClipCubic() absorbs the rest, which is the intent — more drive, not more level.
	constexpr q31_t kSearDriveBoost = 39258415; // round(log2(1.5) * (1 << 26))
	const q31_t boosted = level + kSearDriveBoost;

	// Split `boosted` into integer octaves and a fraction. The cast matters: shifting a signed
	// q31_t left would overflow into the sign bit.
	//
	// THE SHIFT IS TIED TO THE PARAM PIPELINE, not to q31. `level` is
	// paramFinalValues[LOCAL_SEAR], which does NOT span the full q31 range: with neutral value
	// 25*10737418 and getFinalParameterValueLinear's `<<3`, it tops out just under 2^29. So >>26
	// is what yields 0..7 octaves before the boost above, 0..8 after it. An earlier version used
	// >>28 on the assumption that level was full-scale q31 — that capped the whole control at
	// 16x instead of 256x. If the neutral value or the param's patcher block ever changes,
	// recompute this.
	const int32_t octaves = boosted >> 26;
	const q31_t frac = static_cast<q31_t>((static_cast<uint32_t>(boosted) << 5) & 0x7FFFFFFF);

	// Residual loudness lift, so the knob is not dead flat.
	//
	// Correcting all the way to 0.00 dB makes drive read as "nothing is happening" — the ear
	// takes the loudness rise as part of the effect. So the target is not unity but unity plus
	// a little, rising linearly with knob position: at 4 * level the multiplier reaches 2.0,
	// i.e. +6.0 dB of allowance at full knob, scaling linearly at every position below.
	//
	// WHY 2.0 AND NOT 1.5. This was 2 * level -> 1.5x (+3.5 dB) and it measured as about
	// +2.3 dB of real output rise at knob 50, staying inside ±0.4 dB up to knob 15. It was
	// reported as "too tame" on hardware. The diagnostic bypass build — corrective gain forced
	// to unity — came back as GOOD DRIVE CHARACTER BUT TOO LOUD, which localised the fault to
	// the leveller rather than the waveshaper: softClipCubic is exonerated, and the answer is
	// partial correction, just more generous than 1.5x. 2.0x was auditioned and accepted.
	// See HANDOFF.md and issue #1.
	//
	// The allowance is not the measured rise — mean-abs matching is slightly stricter than RMS
	// on a clipped wave, so real output rise lands below the figure above. The 1.5x version
	// measured at roughly two thirds of its allowance; the 2.0x version has been auditioned but
	// NOT re-measured, so do not quote a measured number for it without taking one.
	//
	// STILL LINEAR IN `level`, which is a known limitation rather than a design choice. Doubling
	// the slope doubles the allowance everywhere, but the bottom of the knob remains small in
	// absolute terms (~+2.3 dB at knob 15). If tameness is ever reported low on the knob rather
	// than across the range, the fix is to SHAPE this curve, not to enlarge it further.
	//
	// The guard is defensive, and tracks the shift. `level` is documented as topping out just
	// under 2^29, so << 2 is safe, but a future change to the param's neutral value would
	// silently turn a left shift into a sign flip and hand the clipper a negative makeup. If
	// the shift ever changes again, move this bound with it.
	const q31_t lift = (level < (1 << 29)) ? (level << 2) : ONE_Q31;

	q31_t envIn = state.envIn;
	q31_t envOut = state.envOut;
	uint32_t warmup = state.warmup;
	// getMagnitude(w) is floor(log2(w)), so the time constant doubles as the count doubles.
	int32_t detShift = (warmup >= kSearWarmupSamples) ? kSearDetectorShift
	                                                  : std::min<int32_t>(getMagnitude(warmup + 1), kSearDetectorShift);

	// --- Pass 1: drive, clip, measure. -------------------------------------------------
	q31_t* currentSample = startSample;
	do {
		const q31_t c = *currentSample;

		// c * (1 + frac), already saturated into q31 by add_saturate.
		// The 2* undoes the halving in multiply_32x32_rshift32.
		const q31_t driven = add_saturation(c, 2 * multiply_32x32_rshift32(c, frac));

		// THE octaves == 0 GUARD IS LOAD-BEARING. DO NOT REMOVE IT.
		//
		// lshiftAndSaturateUnknown(val, 0) is documented in functions.h as forbidden —
		// "lshift must be greater than 0! Not 0" — and it fails silently rather than loudly.
		// It calls signed_saturate_operand_unknown(val, 32 - 0), whose switch only covers 31
		// down to 13 and whose default is signed_saturate<12>. So a shift of zero clamps the
		// sample to TWELVE BITS and shifts by nothing: about 120 dB of attenuation, i.e.
		// silence, not merely a level drop.
		//
		// `octaves` is zero across the bottom of the knob — with kSearDriveBoost applied that
		// means boosted < 2^26, i.e. level < 27.8M, i.e. roughly k < 2.6 of 50 (it was k < 6.25
		// before the boost narrowed the window) —
		// which is precisely the range that was reported as muting on hardware — twice, at two
		// different widths, because earlier param laws put the octaves==0 boundary in a
		// different place. It was misdiagnosed as insufficient drive and then as makeup gain
		// before the real cause was found. See HANDOFF.md.
		//
		// At octaves == 0 no shift is wanted anyway, and `driven` is already saturated, so the
		// correct behaviour is simply to pass it through.
		const q31_t x = (octaves > 0) ? lshiftAndSaturateUnknown(driven, octaves) : driven;
		const q31_t y = softClipCubic(x);
		*currentSample = y;

		// subtract_saturate rather than negation or std::abs: |NEGATIVE_ONE_Q31| is not
		// representable, so plain -c overflows on exactly the samples a clipper produces most
		// of. qsub pins it to ONE_Q31, which is the answer we want anyway.
		const q31_t a = (c < 0) ? subtract_saturation(0, c) : c;
		const q31_t b = (y < 0) ? subtract_saturation(0, y) : y;

		// Same one-pole idiom as the tone stage. The arithmetic shift of a negative difference
		// biases very slightly downward, which is what lets these settle exactly on zero in
		// silence instead of creeping negative.
		envIn += (a - envIn) >> detShift;
		envOut += (b - envOut) >> detShift;

		// Warm-up. Predictable, and once the stage has run for 23 ms it is never taken again —
		// note that this counts from when Sear was last ENABLED, not from note-on, because the
		// state is seeded from the Sound rather than reset per note.
		if (warmup < kSearWarmupSamples) {
			warmup += 1;
			detShift = std::min<int32_t>(getMagnitude(warmup + 1), kSearDetectorShift);
		}

		currentSample += 1;
	} while (currentSample < endSample);

	state.envIn = envIn;
	state.envOut = envOut;
	state.warmup = warmup;

	// --- Correction. -------------------------------------------------------------------
	//
	// raw = envIn / envOut, in (0, 1]. The clipper never reduces mean-abs, so envIn > envOut
	// only happens transiently while the two followers are settling; treating that as unity
	// is both correct and the reason there is no separate "never boost" clamp — add_saturate
	// pins the lifted result at INT32_MAX, which IS ONE_Q31.
	q31_t raw = ONE_Q31;
	if (envOut > 0 && envIn < envOut) {
		raw = static_cast<q31_t>((static_cast<int64_t>(envIn) << 31) / envOut);
	}
	const q31_t target = add_saturation(raw, 2 * multiply_32x32_rshift32(raw, lift));

	// NOT YET PROFILED ON HARDWARE. That is one 64-bit divide per block per voice, and only when
	// the stage is enabled — at a 40-sample block, ~1.1 kHz per voice. The Cortex-A9 has no
	// integer divide instruction, so this compiles to a libgcc call rather than an instruction.
	// Arithmetic says it lands well under 1% of the budget even at full polyphony, but that is
	// an estimate and not a measurement; if Sear ever shows up in a profile, start here. The
	// per-sample work in both passes is shifts and multiplies only, so this is the one place in
	// the file where the cost is not obvious by inspection.

	// THE GAIN IS ONLY EVER GLIDED, NEVER SNAPPED, and that is what makes the warm-up ramp safe
	// rather than dangerous. An early estimate can be wildly wrong — one sample into a note that
	// began on a zero crossing, a heavily driven stage sees a near-silent input against a fully
	// railed output and asks for something like -60 dB. Snapping to the first block's target
	// obeyed that and cost 19 dB out of the first millisecond. Gliding means a target only moves
	// the gain by a 128th per sample, so a bad estimate that lasts a few samples moves it
	// almost not at all, and by the time the detector has integrated enough to be meaningful the
	// gain is still where it started. Do not "optimise" this into a snap for faster convergence.
	q31_t gain = state.gain;

	// --- Pass 2: apply, gliding. -------------------------------------------------------
	currentSample = startSample;
	do {
		gain += (target - gain) >> kSearGainShift;
		// Shift of 1 exactly undoes the multiply's halving, and is >= 1, so the forbidden
		// zero-shift case above cannot arise here.
		*currentSample = lshiftAndSaturateUnknown(multiply_32x32_rshift32(*currentSample, gain), 1);
		currentSample += 1;
	} while (currentSample < endSample);

	state.gain = gain;
}

// ---------------------------------------------------------------------------
// Tone (tilt)
// ---------------------------------------------------------------------------

/*
 * A one-pole lowpass splits the signal into lp and hp = x - lp, and the output is a
 * weighted sum of the two. The weights are chosen so that at centre both are unity and
 * the halves sum back to the input EXACTLY — measured flat to 0.00 dB. That is the whole
 * point of the tilt topology here: centre is a true bypass, so a user who never touches
 * Tone hears the drive character unaltered.
 *
 * Measured response relative to input:
 *            100 Hz    1 kHz    8 kHz
 *   dark     +5.8 dB  -1.7 dB  -18.5 dB
 *   centre    0.0 dB   0.0 dB    0.0 dB
 *   bright   -7.6 dB  +4.9 dB   +5.7 dB
 *
 * The pivot sits near 1 kHz, where a guitar-pedal tone stack usually puts it.
 */
constexpr int32_t kSearToneCoefficientShift = 4; // lp += (x - lp) >> 4

/// tone: q31. 0 = fully dark, ONE_Q31/2 = flat, ONE_Q31 = fully bright.
/// `state` is the lowpass memory and must persist across calls, one per channel.
inline void searToneBuffer(q31_t* startSample, q31_t* endSample, q31_t tone, q31_t* state) {
	const q31_t gHigh = tone;
	const q31_t gLow = ONE_Q31 - tone;
	q31_t lp = *state;
	q31_t* currentSample = startSample;

	do {
		q31_t c = *currentSample;
		lp += (c - lp) >> kSearToneCoefficientShift;
		q31_t hp = c - lp;

		*currentSample =
		    lshiftAndSaturateUnknown(multiply_32x32_rshift32(lp, gLow) + multiply_32x32_rshift32(hp, gHigh), 2);

		currentSample += 1;
	} while (currentSample < endSample);

	*state = lp;
}

/*
 * Stereo tone MUST NOT reuse the mono path.
 *
 * The tone stage holds state, so walking an interleaved buffer with a single filter
 * would feed L into R's history and back — a comb filter, not a tone control. Hence the
 * strided loop and two independent states. This is the single easiest thing to get wrong
 * in this file, and the drive stage above is safe only because it is memoryless.
 */
inline void searToneBufferStereo(q31_t* startSample, q31_t* endSample, q31_t tone, q31_t* stateL, q31_t* stateR) {
	const q31_t gHigh = tone;
	const q31_t gLow = ONE_Q31 - tone;
	q31_t lpL = *stateL;
	q31_t lpR = *stateR;
	q31_t* currentSample = startSample;

	while (currentSample < endSample) {
		q31_t l = currentSample[0];
		q31_t r = currentSample[1];

		lpL += (l - lpL) >> kSearToneCoefficientShift;
		lpR += (r - lpR) >> kSearToneCoefficientShift;
		q31_t hpL = l - lpL;
		q31_t hpR = r - lpR;

		currentSample[0] =
		    lshiftAndSaturateUnknown(multiply_32x32_rshift32(lpL, gLow) + multiply_32x32_rshift32(hpL, gHigh), 2);
		currentSample[1] =
		    lshiftAndSaturateUnknown(multiply_32x32_rshift32(lpR, gLow) + multiply_32x32_rshift32(hpR, gHigh), 2);

		currentSample += 2;
	}

	*stateL = lpL;
	*stateR = lpR;
}

/// Convert an unpatched param's signed q31 (-2^31 .. 2^31-1) to the unsigned 0..ONE_Q31
/// that searToneBuffer expects, with the param's centre landing on tone centre.
/// Matches the codebase idiom — see mod_controllable_audio.cpp, where UNPATCHED_BITCRUSHING
/// and UNPATCHED_SAMPLE_RATE_REDUCTION are both converted by adding 2147483648.
inline q31_t searToneFromUnpatched(int32_t unpatchedValue) {
	return static_cast<q31_t>((static_cast<int64_t>(unpatchedValue) + 2147483648LL) >> 1);
}

} // namespace deluge::dsp
