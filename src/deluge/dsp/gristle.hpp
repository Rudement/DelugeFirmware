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
#include "deluge/dsp/heat.hpp" // softClipCubic
#include "deluge/util/fixedpoint.h"
#include "deluge/util/functions.h"
#include <algorithm>

namespace deluge::dsp::gristle {

/*
 * THE GRISTLEIZER — a standalone effect, NOT a ModFX type.
 *
 * One LFO driving an amplitude stage and a filter stage. After the Roy Gwinn design
 * published in Practical Electronics in 1975 and sold in kit form by Phonosonics in 1977,
 * which Chris Carter modified for Throbbing Gristle — the modification being that he brought
 * the Bias trimpot out to the front panel, which is why Bias is a first-class knob here.
 *
 * The original front panel was Speed, Depth, Waveform, Bias and Level, over a VCA/VCF
 * switch. The Eurorack redesign (FSS TG3/TG4, Gwinn + Carter, 2017) added Register and
 * Resonance to the filter and Dirt to the VCA. This implements all nine.
 *
 *   Rate    UNPATCHED_GRISTLE_RATE   LFO speed, LFO rates through to audio rate
 *   Depth   UNPATCHED_GRISTLE_DEPTH  how far the LFO pulls the stage down. 0 = bypass
 *   Shape   UNPATCHED_GRISTLE_SHAPE  triangle -> trapezoid -> square, continuous
 *   Bias    UNPATCHED_GRISTLE_BIAS   offsets the LFO before shaping. Bipolar, 0 = centred
 *   Mode    UNPATCHED_GRISTLE_MODE   0 = pure VCA (chop), full = pure VCF (throb)
 *   Level   UNPATCHED_GRISTLE_LEVEL  output trim. Defaults to unity
 *   Freq    UNPATCHED_GRISTLE_FREQ   filter centre ("Register"). Bipolar, 0 = ~800 Hz
 *   Reso    UNPATCHED_GRISTLE_RES    filter resonance
 *   Dirt    UNPATCHED_GRISTLE_DIRT   blends the clean VCA output against a driven one
 *
 * WHY THIS IS NO LONGER A ModFX TYPE. As a ModFX type it borrowed
 * UNPATCHED_MOD_FX_FEEDBACK for Shape and UNPATCHED_MOD_FX_OFFSET for Mode. But
 * GlobalEffectable::getActiveModFXType() forces the type to NONE whenever the currently
 * selected gold-knob param sits at its minimum, and that param defaults to FEEDBACK
 * (global_effectable.cpp:44). Shape at minimum is a legitimate setting — it is a plain
 * triangle, and it is where an untouched knob sits — so on song, kit and audio-clip FX the
 * whole effect silently vanished before processFX ever saw it. For flanger and chorus that
 * gate means "nothing worth computing"; for this it was fatal. Owning our own params removes
 * the gate entirely. Do not reintroduce a "param at minimum means off" rule here: on this
 * effect every knob has a musically valid minimum.
 *
 * FIXED-POINT RULE, same as heat.hpp: multiply_32x32_rshift32(a, b) yields a*b/2, not a*b,
 * because it drops 32 bits of a 64-bit q31 product. Every multiply below is followed by the
 * shift that puts it back on scale. Getting this wrong is silent — the stage just lands 6 dB
 * down — so each step states its intended transfer in a comment.
 *
 * NO DELAY LINE. Six words of state per instance (LFO phase, plus low/band per channel).
 */

// ---------------------------------------------------------------------------
// Filter coefficient limits
// ---------------------------------------------------------------------------

/*
 * The filter is a Chamberlin state-variable, the same topology and the same fixed-point
 * idiom as dsp/filter/svf.cpp, but with the cutoff recomputed every sample because the LFO
 * sweeps it. `fc` is 2*sin(pi*f/fs) expressed as a q31 fraction of 1.0.
 *
 * A Chamberlin SVF goes unstable as fc approaches 1.0. Everything below is sized so that the
 * ceiling is never reached in the first place rather than clamped at it — a clamp would give
 * a dead zone at the top of the Freq knob, which is exactly the failure Mid EQ hit when its
 * sweep was +/-6 octaves (see the handoff). kMaxCoefficient is still applied as a backstop so
 * that a future range change cannot destabilise the filter, but with the shipped range it is
 * unreachable.
 */

/// ~80 Hz at 44.1 kHz. The floor the LFO pulls the cutoff down to. Chosen so the filter
/// closes to a throb rather than to silence.
constexpr q31_t kMinCoefficient = 24480000;

/// ~3.6 kHz at 44.1 kHz, i.e. fc = 0.5. Backstop only — the shipped Freq range tops out at
/// ~3.2 kHz. Do not raise this without re-checking stability at maximum resonance.
constexpr q31_t kMaxCoefficient = 1073741824;

/// Centre of the Freq sweep, ~800 Hz at 44.1 kHz, reached when the bipolar Freq param is 0.
constexpr q31_t kCentreCoefficient = 244750000;

/// Octaves either side of centre that Freq travels. 2 gives ~200 Hz to ~3.2 kHz, which keeps
/// fc under kMaxCoefficient across the whole knob. Raising this reintroduces the clamp.
constexpr int32_t kFreqOctaves = 2;

/// Decoded once per buffer, then held constant across the sample loop.
struct Config {
	int32_t octaves;   ///< integer part of the LFO shaping gain, 0..4
	q31_t frac;        ///< fractional part between octaves and octaves+1, 0..ONE_Q31
	q31_t bias;        ///< signed LFO offset applied before shaping
	q31_t depth;       ///< warped modulation depth, 0..ONE_Q31
	q31_t mode;        ///< 0 = pure VCA, ONE_Q31 = pure VCF
	q31_t level;       ///< output trim, 0..ONE_Q31
	q31_t fcBase;      ///< filter coefficient at full LFO opening
	q31_t q;           ///< SVF damping. ONE_Q31 = no resonance, small = very resonant
	q31_t filterInput; ///< input trim compensating for resonance gain
	q31_t dirtDrive;   ///< pre-gain into the soft clipper
	q31_t dirtMix;     ///< 0 = clean VCA, ONE_Q31 = fully driven
	bool doVca;        ///< false when Mode is hard over to the filter
	bool doFilter;     ///< false when Mode is hard over to the VCA
	bool doDirt;       ///< false when Dirt is at zero
};

/*
 * Depth taper.
 *
 * As a ModFX type this param arrived pre-warped by getFinalParameterValueVolume()'s
 * parabola and needed correcting. It no longer does — UNPATCHED_GRISTLE_DEPTH is read raw —
 * so the warp here is applied to a plain linear knob and the taper table in the handoff for
 * the ModFX version no longer describes it.
 *
 *   w = 2d - d^2      moves -6 dB of chop from knob ~35 of 50 to knob ~22
 *
 * Written as d + d(1 - d) rather than 2d - d*d because the latter overflows int32 at the
 * intermediate 2d for any d above half scale. In this form every intermediate stays inside
 * q31, the result is exactly ONE_Q31 at d = ONE_Q31, and — critically — zero is still exactly
 * zero, i.e. a true bypass.
 *
 * If it still feels slow, warp again rather than changing the range; but note that applying
 * the warp twice overshoots and kills the top half, exactly as it does for Heat.
 */
[[gnu::always_inline]] inline q31_t warpDepth(q31_t depth) {
	return add_saturation(depth, multiply_32x32_rshift32(depth, ONE_Q31 - depth) << 1);
}

/// Fold a signed unpatched param onto 0..ONE_Q31, the same way bitcrush and SRR do.
[[gnu::always_inline]] inline q31_t unipolar(q31_t param) {
	return (param >> 1) + 1073741824;
}

/*
 * Decode the knobs. Call once per buffer, not per sample.
 *
 * Every argument is the raw signed q31 value straight out of the unpatched param set. No
 * caller-side scaling — keeping all of the law in one place is what stops the two call sites
 * (Sound and GlobalEffectable) drifting apart.
 */
inline Config setup(q31_t shapeParam, q31_t biasParam, q31_t depthParam, q31_t modeParam, q31_t levelParam,
                    q31_t freqParam, q31_t resParam, q31_t dirtParam) {
	Config c;

	q31_t shape01 = unipolar(shapeParam);
	c.mode = unipolar(modeParam);
	c.level = unipolar(levelParam);
	c.depth = warpDepth(unipolar(depthParam));

	// Bias is bipolar and passes through untouched: it is added to the triangle before
	// shaping, so at the extremes it rails the LFO and the stage sticks fully open or fully
	// shut. That is the original control's behaviour, not a bug — Carter brought this trimpot
	// to the front panel precisely so it could be pushed that far.
	c.bias = biasParam;

	// Shape drives a gain applied to the triangle before it is clipped, so the waveform morphs
	// triangle -> trapezoid -> square continuously. Splitting that gain into an integer octave
	// count plus a fraction lets the whole morph be two shifts and a lerp instead of an exp
	// lookup. shape01 >> 29 spans 0..3, i.e. 1x to 8x, and the frac interpolates toward the
	// next octave, so the top of the knob reaches an effective 16x — enough to reach a
	// hard-railed square.
	c.octaves = std::min<int32_t>(shape01 >> 29, 4);
	c.frac = (q31_t)((uint32_t)(shape01 & ((1 << 29) - 1)) << 2);

	// Filter centre. Same getExp idiom as the bass/treble shelves, at +/-kFreqOctaves.
	c.fcBase = std::clamp<q31_t>(getExp(kCentreCoefficient, (freqParam >> 5) * kFreqOctaves), kMinCoefficient,
	                             kMaxCoefficient);

	// Resonance. q is damping, so it runs the other way to the knob: ONE_Q31 is critically
	// damped and small values ring. Stopping at ~0.03 rather than 0 keeps the filter short of
	// self-oscillation; the band-feedback tanh below is what actually guarantees stability, but
	// relying on the tanh alone would mean the top of the knob is a squeal rather than a
	// resonant sweep.
	q31_t res = unipolar(resParam);
	c.q = ONE_Q31 - (res - (res >> 5));

	// A resonant SVF has gain at the peak, so trim the input as resonance rises. Mirrors
	// svf.cpp's `in`, which exists for the same reason.
	c.filterInput = (c.q >> 1) + (ONE_Q31 >> 1);

	// Dirt. The mix is the knob; the drive rises alongside it so that turning Dirt up both
	// adds more clipped signal and clips it harder, which is what the JFET does as it is
	// pushed. Drive tops out at 3x — beyond that the cubic is fully railed and further gain is
	// inaudible, the same ceiling Heat runs into above ~100x.
	c.dirtMix = unipolar(dirtParam);
	c.dirtDrive = c.dirtMix;

	c.doDirt = (c.dirtMix != 0);
	c.doFilter = (c.mode != 0);
	c.doVca = (c.mode != ONE_Q31);

	return c;
}

/*
 * Whether the effect needs to run at all. Call once per buffer; when this is false the caller
 * must skip the sample loop entirely, which is what makes the default state a true bypass and
 * costs nothing on the overwhelming majority of sounds that never enable it.
 *
 * NOTE THE MODE TERM. It is tempting to gate on Depth alone, but the filter is in circuit
 * whenever Mode is up even with Depth at zero — that is a static lowpass at the Freq setting,
 * exactly as on the original, where the VCF does not leave the signal path just because the
 * LFO is turned down. Gating on Depth alone would silently bypass a filter the user can hear
 * themselves setting. Likewise Dirt and Level do audible work with no modulation at all.
 *
 * Every default is the inactive value (Depth, Dirt, Mode at minimum; Level at maximum), so
 * existing songs load bypassed and unchanged.
 */
[[gnu::always_inline]] inline bool isEnabled(q31_t depthParam, q31_t modeParam, q31_t levelParam, q31_t dirtParam) {
	return depthParam != NEGATIVE_ONE_Q31 || modeParam != NEGATIVE_ONE_Q31 || dirtParam != NEGATIVE_ONE_Q31
	       || levelParam != ONE_Q31;
}

/*
 * Shape the triangle. Returns a q31 waveform spanning the full range whatever the shape.
 *
 * !!! DO NOT REMOVE THE octaves > 0 GUARD !!!
 * functions.h carries the warning "lshift must be greater than 0! Not 0" above
 * lshiftAndSaturateUnknown. With a shift of zero it calls
 * signed_saturate_operand_unknown(val, 32), whose switch only covers 31 down to 13 and whose
 * default: is signed_saturate<12> — so the sample is clamped to TWELVE BITS and shifted by
 * nothing, roughly 120 dB down, silently. That exact call cost days on Heat (handoff bug 4)
 * and then reappeared here. octaves is zero across the entire bottom quarter of the Shape
 * knob, so without this guard the whole triangle end of the morph collapses.
 *
 * MEASURED, by deleting the guard and rerunning the numerical test: peak output over Shape
 * 0..12 falls to 0.0000 of full scale — silence, not merely quiet. That is WORSE than the
 * 0.500 the ModFX version measured, because Bias is now added before the shift, so the whole
 * biased waveform lands inside the 12-bit clamp instead of only part of it. At zero octaves no
 * shift is wanted and the triangle is already full scale, so passing it straight through is
 * correct.
 */
[[gnu::always_inline]] inline q31_t shapeLFO(q31_t triangle, const Config& c) {
	// Bias first, so that it sets the duty cycle of the shaped wave rather than merely
	// offsetting the result. Saturating: a large bias is meant to rail the wave.
	q31_t t = add_saturation(triangle, c.bias);

	q31_t a = (c.octaves > 0) ? lshiftAndSaturateUnknown(t, c.octaves) : t;
	q31_t b = lshiftAndSaturateUnknown(t, c.octaves + 1); // shift >= 1 always, safe

	// Lerp a -> b by frac. Plain subtraction is safe here and the reason is worth stating:
	// a and b are the same wave saturated at two adjacent shifts, so they always share its
	// sign, and |b - a| <= max(|a|, |b|). The int32 overflow case needs opposite signs, which
	// cannot arise. The result lies between a and b, so the add cannot overflow either.
	return a + (multiply_32x32_rshift32(c.frac, b - a) << 1);
}

/// One channel's filter memory. MUST be per-channel: running a single state over an
/// interleaved stereo buffer combs instead of filtering, the same trap noted for Heat's tone
/// stage in the handoff.
struct FilterState {
	q31_t low;
	q31_t band;
};

/// Per-instance state. `lfoPhase` is shared by both channels so the two sides chop and sweep
/// together rather than drifting apart.
struct Memory {
	uint32_t lfoPhase;
	FilterState l;
	FilterState r;
};

/*
 * One sample, one channel.
 *
 * `shaped` is the output of shapeLFO() and is shared by both channels.
 */
[[gnu::always_inline]] inline q31_t processSample(q31_t sample, q31_t shaped, const Config& c, FilterState& state) {
	// LFO to 0..ONE_Q31, then invert: the modulator is how far DOWN we pull, so that zero
	// depth is exactly zero pull and therefore an exact bypass on both paths.
	q31_t lfo01 = unipolar(shaped);
	q31_t pull = multiply_32x32_rshift32(c.depth, ONE_Q31 - lfo01) << 1; // 0..ONE_Q31

	q31_t vca = 0;
	if (c.doVca) {
		// Gain falls from unity to silence.
		q31_t gain = ONE_Q31 - pull;
		vca = multiply_32x32_rshift32(sample, gain) << 1;

		if (c.doDirt) {
			// Dirt sits AFTER the VCA, not before it. On the original the nonlinearity is the
			// JFET inside the amplitude stage, so how hard it distorts depends on how far the
			// VCA is currently open — the chop modulates the distortion. Driving the input
			// instead would give a static overdrive with a tremolo after it, which is a
			// different and much duller effect.
			//
			// Drive is 1 + 2*dirt, i.e. up to 3x, built by saturating adds rather than a shift.
			// A plain `multiply_32x32_rshift32(vca, drive) << 1` OVERFLOWS: the product peaks at
			// 2^30 when both operands are full scale, and shifting that left by one lands on
			// 2^31, one past INT32_MAX. lshiftAndSaturate<1> is the saturating form of the same
			// shift, and add_saturation keeps the sum in range for softClipCubic, which requires
			// its input already inside [-1, 1].
			q31_t boost = lshiftAndSaturate<1>(multiply_32x32_rshift32(vca, c.dirtDrive));
			q31_t driven = softClipCubic(add_saturation(vca, add_saturation(boost, boost)));
			// Lerp clean -> driven by dirtMix. Both operands share a sign here for the same
			// reason as in shapeLFO, so the subtraction cannot overflow.
			vca = vca + (multiply_32x32_rshift32(c.dirtMix, driven - vca) << 1);
		}
	}

	q31_t filtered = 0;
	if (c.doFilter) {
		// Cutoff falls from fcBase toward kMinCoefficient as the LFO pulls down.
		q31_t span = c.fcBase - kMinCoefficient;
		q31_t fc = c.fcBase - (multiply_32x32_rshift32(pull, span) << 1);

		// The filter is fed the DRY sample, not the VCA output. Cascading them would multiply
		// the two depths and Mode would stop being a blend.
		q31_t in = multiply_32x32_rshift32(c.filterInput, sample) << 1;

		q31_t low = state.low;
		q31_t band = state.band;

		// Chamberlin SVF, one iteration. Each `2 *` restores the scale the multiply halved.
		low = low + 2 * multiply_32x32_rshift32(band, fc);
		q31_t high = in - low - 2 * multiply_32x32_rshift32(band, c.q);
		band = band + 2 * multiply_32x32_rshift32(high, fc);

		// Saturating the band feedback is what bounds the filter at high resonance. svf.cpp
		// does the same, for the same reason. Removing it turns maximum Reso into a blow-up.
		band = getTanHUnknown(band, 3);

		state.low = low;
		state.band = band;
		filtered = low;
	}

	// Mode blend. Each half is halved by its multiply, so one shared shift restores scale;
	// saturating because a full-scale sample at either extreme lands exactly on the rail.
	q31_t mixed;
	if (c.doVca && c.doFilter) {
		mixed = lshiftAndSaturate<1>(add_saturation(multiply_32x32_rshift32(vca, ONE_Q31 - c.mode),
		                                            multiply_32x32_rshift32(filtered, c.mode)));
	}
	else {
		mixed = c.doVca ? vca : filtered;
	}

	// Output trim.
	return multiply_32x32_rshift32(mixed, c.level) << 1;
}

} // namespace deluge::dsp::gristle
