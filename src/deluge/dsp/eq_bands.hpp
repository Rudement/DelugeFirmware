/*
 * Copyright (c) 2026 Rudement
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

#include "definitions_cxx.hpp"
#include "util/functions.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

/// The tuning of the four-band EQ, in one place.
///
/// ModControllableAudio::processFX() used to spell these preset bases and octave multipliers out
/// inline. They live here now for ONE reason: the FX > EQ menu reports each band in Hz and dB, and
/// it has to derive those numbers from exactly the coefficients the audio path will use. A readout
/// that kept its own copy of the tuning would look right the day it was written and silently drift
/// the first time a base moved.
///
/// Both readouts were checked against a sample-accurate simulation of the fixed-point doEQ() before
/// they shipped: the bass and treble gains and all four frequencies agree to within 0.1 dB, and the
/// bells to within 0.05 dB anywhere they are boosting.
///
/// Every number below is load-bearing and was swept numerically before it was chosen. The reasoning
/// lives in the comments in processFX(), where the bands are actually run; read those before
/// touching anything here, and in particular do NOT raise either mid octave multiplier — at higher
/// values both of a bell's one-poles saturate at once, their difference cancels, and the band goes
/// silent over the top of its knob.
namespace deluge::dsp::eq {

/// Which of the four bands a control belongs to. Named here, next to the tuning, so the menu and
/// the filter cannot disagree about what "low mid" means.
enum class Band { BASS, LOW_MID, HIGH_MID, TREBLE };

// getExp() preset bases. Bass and treble are stock Deluge; the four mid bases came from the
// chopin (1.2.1) branch and are unchanged from it.
constexpr int32_t kBassBase = 120000000;
constexpr int32_t kTrebleBase = 700000000;
constexpr int32_t kLowMidLoBase = 145000000;
constexpr int32_t kLowMidHiBase = 580000000;
constexpr int32_t kHighMidLoBase = 285167594;
constexpr int32_t kHighMidHiBase = 932909729;

// How far each band's knob sweeps, in octaves either side of its base.
constexpr int32_t kShelfOctaves = 6;   // bass and treble shelves
constexpr int32_t kLowMidOctaves = 3;  // NOT 6 — see processFX()
constexpr int32_t kHighMidOctaves = 1; // NOT 3 — see processFX()

/// One-pole coefficient for a band, from that band's raw q31 param value.
[[nodiscard]] inline int32_t bandCoefficient(int32_t base, int32_t paramValue, int32_t octaves) {
	return getExp(base, (paramValue >> 5) * octaves);
}

[[nodiscard]] inline int32_t bassCoefficient(int32_t paramValue) {
	return bandCoefficient(kBassBase, paramValue, kShelfOctaves);
}
[[nodiscard]] inline int32_t trebleCoefficient(int32_t paramValue) {
	return bandCoefficient(kTrebleBase, paramValue, kShelfOctaves);
}
[[nodiscard]] inline int32_t lowMidLoCoefficient(int32_t paramValue) {
	return bandCoefficient(kLowMidLoBase, paramValue, kLowMidOctaves);
}
[[nodiscard]] inline int32_t lowMidHiCoefficient(int32_t paramValue) {
	return bandCoefficient(kLowMidHiBase, paramValue, kLowMidOctaves);
}
[[nodiscard]] inline int32_t highMidLoCoefficient(int32_t paramValue) {
	return bandCoefficient(kHighMidLoBase, paramValue, kHighMidOctaves);
}
[[nodiscard]] inline int32_t highMidHiCoefficient(int32_t paramValue) {
	return bandCoefficient(kHighMidHiBase, paramValue, kHighMidOctaves);
}

// ---------------------------------------------------------------------------------------------
// Hz readout. Menu-thread only — this is float maths and has no business in the sample loop.
// ---------------------------------------------------------------------------------------------

/// Ceiling for the displayed frequency.
///
/// getExp() saturates at q31 max once a band is swept far enough up, at which point the one-pole
/// coefficient reaches 1, the filter passes its input through instantly, and the exact corner runs
/// off towards infinity. The treble does this from knob 32 — the same ceiling EqMenu already works
/// around with its "treble boost has no effect above 32" clamp — and the low mid's upper pole pins
/// from around knob 41. The band really has flattened out up there, so parking the readout at the
/// top of hearing is honest. Printing the arithmetic answer (136 kHz for the treble) is not.
constexpr float kMaxDisplayHz = 20000.f;

/// Corner frequency in Hz of a one-pole lowpass run the way doEQ() runs them:
///     state += multiply_32x32_rshift32(input - state, coefficient) << shift
/// which is a per-sample coefficient of a = coefficient * 2^shift / 2^32. The shelves and bells
/// differ only in that shift — bass uses 0, everything else uses 1.
///
/// a = 1 - e^(-2*pi*fc/fs), so fc = -ln(1 - a) * fs / (2*pi).
[[nodiscard]] inline float onePoleCornerHz(int32_t coefficient, int32_t shift) {
	constexpr float kTwoPi = 6.283185307f;
	const float a =
	    std::clamp(static_cast<float>(coefficient) * static_cast<float>(1 << shift) / 4294967296.f, 1e-9f, 0.99999f);
	return std::min(-std::log(1.f - a) * static_cast<float>(kSampleRate) / kTwoPi, kMaxDisplayHz);
}

/// Centre of a bell built as the difference of two one-pole lowpasses: the geometric mean of the
/// two corners, which is where the difference peaks. Both corners are clamped before the mean is
/// taken, so a band whose upper pole has pinned still reads as a rising number rather than as a
/// runaway one.
[[nodiscard]] inline float bellCentreHz(int32_t loCoefficient, int32_t hiCoefficient) {
	return std::min(std::sqrt(onePoleCornerHz(loCoefficient, 1) * onePoleCornerHz(hiCoefficient, 1)), kMaxDisplayHz);
}

[[nodiscard]] inline float bassHz(int32_t paramValue) {
	return onePoleCornerHz(bassCoefficient(paramValue), 0);
}
[[nodiscard]] inline float trebleHz(int32_t paramValue) {
	return onePoleCornerHz(trebleCoefficient(paramValue), 1);
}
[[nodiscard]] inline float lowMidHz(int32_t paramValue) {
	return bellCentreHz(lowMidLoCoefficient(paramValue), lowMidHiCoefficient(paramValue));
}
[[nodiscard]] inline float highMidHz(int32_t paramValue) {
	return bellCentreHz(highMidLoCoefficient(paramValue), highMidHiCoefficient(paramValue));
}

// ---------------------------------------------------------------------------------------------
// dB readout. Menu-thread only, same as the Hz side above.
// ---------------------------------------------------------------------------------------------

/// Where a band's Amount knob sits: 0 at the bottom, 0.5 at the centre detent, 1 at the top.
///
/// processFX() spells this out in fixed point as `positive = (value >> 1) + 1073741824`, which is
/// exactly (value / 2^31 + 1) / 2 — and, because a menu step is 2^31/25, exactly knob/50. All four
/// bands then square it, so everything below is a pure function of the knob position.
[[nodiscard]] inline float amountPosition(int32_t paramValue) {
	return std::clamp((static_cast<float>(paramValue) / 2147483648.f + 1.f) * 0.5f, 0.f, 1.f);
}

/// The square law both shelves are built on, as a plain multiplier.
///
/// processFX() forms bassAmount as (p^2 - 0.25) * 2^31 and doEQ() adds bandOnly * that >> 32 << 3,
/// i.e. bandOnly * 4 * (p^2 - 0.25), giving 1 + 4(p^2 - 0.25) = 4p^2 wherever bandOnly is the whole
/// signal. Unity at the detent, x4 (+12 dB) at the top, a true zero at the bottom. trebleAmount
/// reaches the same law from the other direction, its no-change point being the 536870912 it starts
/// from rather than an offset subtracted from it.
[[nodiscard]] inline float shelfLaw(int32_t paramValue) {
	const float p = amountPosition(paramValue);
	return 4.f * p * p;
}

/// Bass shelf gain at DC.
///
/// Exact, and independent of Bass Freq: a one-pole lowpass is exactly 1 at DC no matter where its
/// corner is, so bassOnly there is the whole signal and the shelf lands on the bare square law.
[[nodiscard]] inline float bassGain(int32_t paramValue) {
	return shelfLaw(paramValue);
}

/// What a one-pole lowpass still passes at Nyquist: H(z) = a / (1 - (1 - a)z^-1) at z = -1, which
/// is a / (2 - a). Unlike its analog prototype a digital one-pole does NOT reach zero at the top of
/// the band, and for the treble shelf that leak is the whole story — see trebleGain().
[[nodiscard]] inline float onePoleNyquistLeak(int32_t coefficient, int32_t shift) {
	const float a =
	    std::clamp(static_cast<float>(coefficient) * static_cast<float>(1 << shift) / 4294967296.f, 0.f, 1.f);
	return a / (2.f - a);
}

/// Treble shelf gain at the top of the band. Needs the Treble Freq param as well as the amount.
///
/// doEQ() computes trebleOnly as input - withoutTreble and outputs withoutTreble + trebleOnly * G,
/// which rearranges to leak + G(1 - leak) where leak is what the lowpass still passes up there.
/// The bass shelf gets away with ignoring its own frequency because a lowpass is exactly 1 at DC;
/// the treble does not, because that same lowpass is emphatically not 0 at Nyquist.
///
/// This is why the treble readout tracks its Freq knob where the bells deliberately don't. It stays
/// monotonic in amount at any frequency, so none of the folding-back the bells suffer applies — and
/// it has to, because getExp() saturates from Freq knob 32 up, leak goes to 1, and the treble
/// amount stops doing ANYTHING. A fixed law would cheerfully report +12 dB across that whole dead
/// zone; this reports the 0 dB that is actually happening. Measured against the fixed-point path at
/// 20 kHz it agrees to within 0.1 dB at every setting tried.
[[nodiscard]] inline float trebleGain(int32_t amountParamValue, int32_t freqParamValue) {
	const float leak = onePoleNyquistLeak(trebleCoefficient(freqParamValue), 1);
	return leak + shelfLaw(amountParamValue) * (1.f - leak);
}

/// The fraction of the signal a bell passes at its centre.
///
/// A bell here is H(f_hi) - H(f_lo), the difference of two one-pole lowpasses. At the geometric
/// centre that difference is purely real and works out at (R - 1) / (R + 1), R being the ratio of
/// the two corners. That is why these bells peak at around 0.6 of their input rather than at 1.0,
/// and in turn why doEQ() applies the mid amounts at 6x where the shelves use 4x.
[[nodiscard]] inline float bellCapture(int32_t loCoefficient, int32_t hiCoefficient) {
	const float r = onePoleCornerHz(hiCoefficient, 1) / onePoleCornerHz(loCoefficient, 1);
	return (r - 1.f) / (r + 1.f);
}

/// What doEQ() multiplies a bell's extracted signal by: 4x from the << 3, and 1.5x more from the
/// + (boost >> 1) that follows it.
constexpr float kBellAmountScale = 6.f;

/// Bell gain at its centre, as a plain multiplier.
///
/// `capture` is the band's capture at its NEUTRAL centre — coefficient argument 0, i.e. the Freq
/// knob at its detent — and deliberately NOT at wherever Freq happens to be sitting. Tracking Freq
/// would be about 2.5 dB more exact on boost, but the amount law over-cuts on purpose (full cut
/// removes a little more than the whole band), so with Freq high the true gain passes through zero
/// and back out the other side as polarity inversion. A readout on that basis falls, bottoms out
/// and then RISES again as you wind Amount down. Holding capture fixed keeps the scale monotonic,
/// which is the entire job of a gain readout.
[[nodiscard]] inline float bellGain(int32_t paramValue, float capture) {
	const float p = amountPosition(paramValue);
	return 1.f + capture * kBellAmountScale * (p * p - 0.25f);
}

[[nodiscard]] inline float lowMidGain(int32_t paramValue) {
	return bellGain(paramValue, bellCapture(lowMidLoCoefficient(0), lowMidHiCoefficient(0)));
}
[[nodiscard]] inline float highMidGain(int32_t paramValue) {
	return bellGain(paramValue, bellCapture(highMidLoCoefficient(0), highMidHiCoefficient(0)));
}

/// Floor for the displayed gain. Below this the band has stopped being an EQ setting and become a
/// removal: the shelves reach a true zero at the bottom of their knob, and the bells land just
/// past one, cancelling slightly more than the band they extracted.
constexpr float kMinDisplayDb = -24.f;

[[nodiscard]] inline float gainToDb(float gain) {
	const float magnitude = std::fabs(gain);
	return magnitude < 1e-6f ? -120.f : 20.f * std::log10(magnitude);
}

/// Gain of a band in dB, from its Amount param's raw q31 value.
///
/// `freqParamValue` is that band's Freq param, and only the treble looks at it; see trebleGain()
/// for why it is the one band that has to.
[[nodiscard]] inline float amountDb(Band band, int32_t paramValue, int32_t freqParamValue) {
	switch (band) {
	case Band::LOW_MID:
		return gainToDb(lowMidGain(paramValue));
	case Band::HIGH_MID:
		return gainToDb(highMidGain(paramValue));
	case Band::TREBLE:
		return gainToDb(trebleGain(paramValue, freqParamValue));
	case Band::BASS:
	default:
		return gainToDb(bassGain(paramValue));
	}
}

/// Frequency of a band in Hz, from its Freq param's raw q31 value.
[[nodiscard]] inline float freqHz(Band band, int32_t paramValue) {
	switch (band) {
	case Band::BASS:
		return bassHz(paramValue);
	case Band::LOW_MID:
		return lowMidHz(paramValue);
	case Band::HIGH_MID:
		return highMidHz(paramValue);
	case Band::TREBLE:
	default:
		return trebleHz(paramValue);
	}
}

} // namespace deluge::dsp::eq
