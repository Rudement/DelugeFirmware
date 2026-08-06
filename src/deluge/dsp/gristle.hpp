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

namespace deluge::dsp::gristle {

/*
 * Gristle: a Gristleizer-style modulated VCA / VCF, as a ModFX type.
 *
 * The original Gristleizer (Roy Gwinn, ETI 1977; the box Throbbing Gristle built their
 * name on) is one LFO driving either an amplitude stage or a filter stage, with a
 * front-panel switch between them and a waveform that ranges from a lazy triangle to a
 * hard on/off chop. This reproduces that behaviour on the three knobs ModFX gives us:
 *
 *   Rate     GLOBAL_MOD_FX_RATE       LFO speed (shared with the stock ModFX types)
 *   Depth    GLOBAL_MOD_FX_DEPTH      how far the LFO pulls the stage down
 *   Shape    UNPATCHED_MOD_FX_FEEDBACK  triangle -> square morph, continuous
 *   Mode     UNPATCHED_MOD_FX_OFFSET    0 = pure VCA (chop), full = pure VCF (throb)
 *
 * Mode is a continuous blend rather than the original's switch, because a knob that
 * only had two positions would be a waste of the one spare param ModFX has, and the
 * intermediate positions (amplitude and filter moving together) are the useful part.
 *
 * FIXED-POINT RULE, same as heat.hpp: multiply_32x32_rshift32(a, b) yields a*b/2, not
 * a*b, because it drops 32 bits of a 64-bit q31 product. Every multiply below is
 * followed by the shift that puts it back on scale. Getting this wrong is silent — the
 * stage just lands 6 dB down — so each step states its intended transfer in a comment
 * and was checked numerically against the real param pipeline before committing.
 *
 * NO DELAY LINE. Unlike flanger/chorus/grain this needs no buffer at all, only four
 * words of filter state. setModFXType() must therefore free both modFXBuffer and
 * modFXGrainBuffer for this type — it falls into the existing `else` branch that
 * already does exactly that, so nothing was added there.
 */

/// Lowest one-pole coefficient the VCF path reaches, ~100 Hz at 44.1 kHz. Chosen so the
/// filter closes to a throb rather than to silence; below about this the VCF mode stops
/// sounding like a filter and starts sounding like a broken VCA.
constexpr q31_t kMinCoefficient = 30400000;

/// Decoded once per buffer from the knobs, then held constant across the sample loop.
struct Config {
	int32_t octaves; ///< integer part of the LFO shaping gain, 0..3
	q31_t frac;      ///< fractional part between octaves and octaves+1, 0..ONE_Q31
	q31_t mode;      ///< 0 = pure VCA, ONE_Q31 = pure VCF
	q31_t depth;     ///< warped modulation depth, 0..ONE_Q31
};

/*
 * Depth taper. GLOBAL_MOD_FX_DEPTH arrives having already been through
 * getFinalParameterValueVolume()'s parabola, which is bottom-heavy: measured against the
 * real pipeline, the stock law puts -6 dB of chop at about knob 35 of 50 and leaves the
 * whole bottom half doing almost nothing. That is the same complaint Heat drew (see the
 * handoff, bug 2) and the same fix applies — warp the value rather than change the range.
 *
 *   w = 2d - d^2      moves -6 dB from knob ~35 to knob ~22
 *
 * Written as d + d(1 - d) rather than 2d - d*d because the latter overflows int32 at the
 * intermediate 2d for any d above half scale. In this form every intermediate stays
 * inside q31 and the result is exactly ONE_Q31 at d = ONE_Q31, so full depth is still
 * full depth and — critically — zero is still exactly zero, i.e. a true bypass.
 */
[[gnu::always_inline]] inline q31_t warpDepth(q31_t depth) {
	return add_saturation(depth, multiply_32x32_rshift32(depth, ONE_Q31 - depth) << 1);
}

/// Decode the knobs. Call once per buffer, not per sample.
[[gnu::always_inline]] inline Config setup(q31_t shapeParam, q31_t modeParam, q31_t depthParam) {
	Config c;

	// Unpatched params are signed q31; fold to 0..ONE_Q31 the same way bitcrush/SRR do.
	q31_t shape01 = (shapeParam >> 1) + 1073741824;
	c.mode = (modeParam >> 1) + 1073741824;

	// Shape drives a gain applied to the triangle before it is clipped, so the waveform
	// morphs triangle -> trapezoid -> square continuously. Splitting that gain into an
	// integer octave count plus a fraction lets the whole morph be two shifts and a lerp
	// instead of an exp lookup. shape01 >> 29 spans 0..3, i.e. 1x to 8x, and the frac
	// interpolates toward the next octave, so the top of the knob reaches an effective
	// 16x — verified sufficient to reach a hard-railed square.
	c.octaves = std::min<int32_t>(shape01 >> 29, 4);
	c.frac = (q31_t)((uint32_t)(shape01 & ((1 << 29) - 1)) << 2);

	// GLOBAL_MOD_FX_DEPTH's neutral value is documented as "2% lower than 536870912", so
	// the knob's top stops just short of unity. Stretch it back before warping, or full
	// depth never quite reaches a full chop.
	c.depth = warpDepth(add_saturation(depthParam, depthParam >> 4));

	return c;
}

/*
 * Shape the triangle. Returns a q31 waveform spanning the full range whatever the shape.
 *
 * !!! DO NOT REMOVE THE octaves > 0 GUARD !!!
 * functions.h carries the warning "lshift must be greater than 0! Not 0" above
 * lshiftAndSaturateUnknown. With a shift of zero it calls
 * signed_saturate_operand_unknown(val, 32), whose switch only covers 31 down to 13 and
 * whose default: is signed_saturate<12> — so the sample is clamped to TWELVE BITS and
 * shifted by nothing, roughly 120 dB down, silently. That exact call cost days on Heat
 * (handoff bug 4). octaves is zero across the entire bottom quarter of the Shape knob,
 * so without this guard the whole triangle end of the morph would be near-silence.
 * At zero octaves no shift is wanted and the triangle is already full scale, so passing
 * it straight through is correct.
 */
[[gnu::always_inline]] inline q31_t shapeLFO(q31_t triangle, const Config& c) {
	q31_t a = (c.octaves > 0) ? lshiftAndSaturateUnknown(triangle, c.octaves) : triangle;
	q31_t b = lshiftAndSaturateUnknown(triangle, c.octaves + 1); // shift >= 1 always, safe

	// Lerp a -> b by frac. Plain subtraction is safe here and the reason is worth stating:
	// a and b are the same triangle saturated at two adjacent shifts, so they always share
	// its sign, and |b - a| <= max(|a|, |b|). The int32 overflow case needs opposite signs,
	// which cannot arise. The result lies between a and b, so the add cannot overflow either.
	return a + (multiply_32x32_rshift32(c.frac, b - a) << 1);
}

/*
 * One sample, one channel.
 *
 * `state` is the one-pole filter memory and MUST be per-channel. Running a single state
 * over an interleaved stereo buffer combs instead of filtering — the same trap noted for
 * Heat's tone stage in the handoff.
 *
 * `shaped` is the output of shapeLFO() and is shared by both channels, so the two sides
 * chop and sweep together rather than drifting apart.
 */
[[gnu::always_inline]] inline q31_t processSample(q31_t sample, q31_t shaped, const Config& c, q31_t& state) {
	// LFO to 0..ONE_Q31, then invert: the modulator is how far DOWN we pull, so that zero
	// depth is exactly zero pull and therefore an exact bypass on both paths.
	q31_t lfo01 = (shaped >> 1) + 1073741824;
	q31_t pull = multiply_32x32_rshift32(c.depth, ONE_Q31 - lfo01) << 1; // 0..ONE_Q31

	// VCA path: gain falls from unity to silence.
	q31_t gain = ONE_Q31 - pull;
	q31_t vca = multiply_32x32_rshift32(sample, gain) << 1;

	// VCF path: one-pole lowpass whose coefficient falls from ONE_Q31 (transparent, a
	// literal pass-through) to kMinCoefficient. Note the filter is fed the DRY sample, not
	// the VCA output — cascading them would multiply the two depths and the blend would
	// stop being a blend.
	// The distanceToGo shape and the plain (non-saturating) accumulate are deliberately the
	// same idiom doEQ() uses for its shelves; a one-pole lowpass is contractive, so state
	// stays bounded by the input range and cannot run away.
	q31_t coefficient = ONE_Q31 - (multiply_32x32_rshift32(pull, ONE_Q31 - kMinCoefficient) << 1);
	q31_t distanceToGo = sample - state;
	state += multiply_32x32_rshift32(distanceToGo, coefficient) << 1;

	// Blend. Each half is already halved by the multiply, so one shared shift restores
	// scale; saturating because a full-scale sample at either extreme lands exactly on
	// the rail.
	return lshiftAndSaturate<1>(
	    add_saturation(multiply_32x32_rshift32(vca, ONE_Q31 - c.mode), multiply_32x32_rshift32(state, c.mode)));
}

} // namespace deluge::dsp::gristle
