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

namespace deluge::dsp {

/*
 * Heat: a cubic soft-clipper followed by a one-pole tilt tone control.
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
 * puts Heat AFTER the ladder filter, so rolling the LPF down also tames the aliasing.
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

/// Makeup gain. Chosen so that at level 0 the small-signal gain of the whole stage is
/// exactly 1.0 (turning Heat up off its stop must not step the level), while a clipped
/// signal falls only ~1.7 dB across the entire sweep — so Heat reads as distortion
/// rather than as a volume control. Sweeps 0.667 down to 0.542.
inline q31_t heatMakeup(q31_t level) {
	constexpr q31_t TWO_THIRDS_Q31 = 0.66667 * ONE_Q31;
	return TWO_THIRDS_Q31 - (level >> 3);
}

/*
 * Heat (drive stage).
 *
 * Pre-gain is linear in the param: 1x at level 0 rising to 33x at full. Linear rather
 * than exponential because the clipper already compresses the top of the range, so the
 * audible result is roughly logarithmic anyway — and a linear law is one add and one
 * shift rather than a lookup.
 *
 * Measured small-signal gain through the whole stage: 1.0x, 2.6x, 8.6x, 15.3x, 21.0x,
 * 25.8x at level 0.00, 0.05, 0.25, 0.50, 0.75, 1.00.
 *
 * Memoryless, so an interleaved stereo buffer can be passed straight through as one
 * long range — exactly the shortcut foldBufferPolyApproximation() takes.
 */
inline void heatBuffer(q31_t* startSample, q31_t* endSample, q31_t level) {
	if (level <= 0) {
		return; // bypass — mirrors how LOCAL_FOLD is gated at the call site
	}

	const q31_t makeup = heatMakeup(level);
	q31_t* currentSample = startSample;

	do {
		q31_t c = *currentSample;

		// c * (1 + 32*level), saturating. mul halves, <<6 multiplies by 64.
		q31_t x = add_saturate(c, lshiftAndSaturateUnknown(multiply_32x32_rshift32(level, c), 6));
		q31_t y = softClipCubic(x);

		*currentSample = lshiftAndSaturateUnknown(multiply_32x32_rshift32(y, makeup), 1);

		currentSample += 1;
	} while (currentSample < endSample);
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
constexpr int32_t kHeatToneCoefficientShift = 4; // lp += (x - lp) >> 4

/// tone: q31. 0 = fully dark, ONE_Q31/2 = flat, ONE_Q31 = fully bright.
/// `state` is the lowpass memory and must persist across calls, one per channel.
inline void heatToneBuffer(q31_t* startSample, q31_t* endSample, q31_t tone, q31_t* state) {
	const q31_t gHigh = tone;
	const q31_t gLow = ONE_Q31 - tone;
	q31_t lp = *state;
	q31_t* currentSample = startSample;

	do {
		q31_t c = *currentSample;
		lp += (c - lp) >> kHeatToneCoefficientShift;
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
inline void heatToneBufferStereo(q31_t* startSample, q31_t* endSample, q31_t tone, q31_t* stateL, q31_t* stateR) {
	const q31_t gHigh = tone;
	const q31_t gLow = ONE_Q31 - tone;
	q31_t lpL = *stateL;
	q31_t lpR = *stateR;
	q31_t* currentSample = startSample;

	while (currentSample < endSample) {
		q31_t l = currentSample[0];
		q31_t r = currentSample[1];

		lpL += (l - lpL) >> kHeatToneCoefficientShift;
		lpR += (r - lpR) >> kHeatToneCoefficientShift;
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
/// that heatToneBuffer expects, with the param's centre landing on tone centre.
/// Matches the codebase idiom — see mod_controllable_audio.cpp, where UNPATCHED_BITCRUSHING
/// and UNPATCHED_SAMPLE_RATE_REDUCTION are both converted by adding 2147483648.
inline q31_t heatToneFromUnpatched(int32_t unpatchedValue) {
	return static_cast<q31_t>((static_cast<int64_t>(unpatchedValue) + 2147483648LL) >> 1);
}

} // namespace deluge::dsp
