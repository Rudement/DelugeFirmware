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
#include "deluge/dsp/sear.hpp" // softClipCubic
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
 * Resonance to the filter and Dirt to the VCA. This implements all nine, plus a master
 * switch the hardware did not have and does not need — a rack unit is bypassed by not
 * patching it.
 *
 *   On      UNPATCHED_GRISTLE_ON     master switch. Thresholded at centre; see isEnabled
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
 * FIXED-POINT RULE, same as sear.hpp: multiply_32x32_rshift32(a, b) yields a*b/2, not a*b,
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

	// The Slicer. Decoded by setupSlice(), not setup(): these are a second modulation source
	// rather than more of the original box's front panel. See THE SLICER, below.
	uint16_t sliceMask;      ///< lane A: the 16 steps that gate the amplitude, one bit each
	uint16_t sliceMaskB;     ///< lane B: the steps that drive the filter, or the dirt
	bool sliceDirtDest;      ///< lane B goes to the distortion rather than the filter
	q31_t sliceAttackB;      ///< lane B's rising edge. Floored slower than lane A: see kSliceMaxCoeffB
	q31_t sliceReleaseB;     ///< lane B's falling edge
	uint32_t sliceDutyPhase; ///< how far into each step the gate stays open, as a phase
	q31_t sliceAttack;       ///< one-pole coefficient for the step's rising edge, from Shape
	q31_t sliceRelease;      ///< the falling edge, floored slower than the attack. See kSliceMaxReleaseCoeff
	q31_t blend;             ///< 0 = LFO only, ONE_Q31 = pattern only, centre = the two multiplied
	int32_t sliceSyncIndex;  ///< which entry of kSliceSyncTicks the step length comes from
	bool doSlice;            ///< false when Blend is at zero, i.e. the effect as it was
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
 * the warp twice overshoots and kills the top half, exactly as it does for Sear.
 */
[[gnu::always_inline]] inline q31_t warpDepth(q31_t depth) {
	return add_saturate(depth, multiply_32x32_rshift32(depth, ONE_Q31 - depth) << 1);
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
	// inaudible, the same ceiling Sear runs into above ~100x.
	c.dirtMix = unipolar(dirtParam);
	c.dirtDrive = c.dirtMix;

	c.doDirt = (c.dirtMix != 0);
	c.doFilter = (c.mode != 0);
	c.doVca = (c.mode != ONE_Q31);

	return c;
}

/*
 * Whether the effect runs at all. Call once per buffer; when this is false the caller must skip
 * the sample loop entirely, which is what makes the default state a true bypass and costs
 * nothing on the overwhelming majority of sounds that never enable it.
 *
 * ONE PARAM DECIDES THIS, AND IT IS THE ONLY THING THAT DOES. UNPATCHED_GRISTLE_ON defaults to
 * minimum, so a fresh sound and a song that predates the effect both load bypassed.
 *
 * WHAT THIS REPLACED, AND WHY. Until the master switch existed this function inferred enablement
 * from the param values:
 *
 *     depth != min || mode != min || dirt != min || level != max
 *
 * which was correct in the narrow sense — those are exactly the four params that do audible work
 * — but it made the effect impossible to switch off without also throwing away the patch. Every
 * one of the nine knobs has a musically valid setting that happens not to be its default, so
 * "return everything to default" was the only bypass, and it was not reversible. Worse, the rule
 * was surprising in the other direction too: turning Depth to zero looks like an off switch and
 * is not one, because Mode up leaves a static lowpass in circuit at the Freq setting, exactly as
 * on the original, where the VCF does not leave the signal path just because the LFO is turned
 * down. Users hit that repeatedly. The switch removes both problems at once: the nine knobs now
 * only describe the sound, and one param decides whether you hear it.
 *
 * DO NOT REINTRODUCE A VALUE-DERIVED TERM HERE, in either direction. Not "...&& depth != min",
 * which would resurrect the old confusion inside the new switch, and not "|| dirt != min", which
 * would make the switch unable to switch anything off. If it is on, it runs; the knobs decide
 * what that sounds like, including the settings that sound like nothing.
 *
 * THRESHOLDED, NOT BLENDED. `> 0` is the whole test — the param is bipolar-ranged like every
 * other unpatched param, so centre is the switching point. Automating a ramp across it therefore
 * flips the effect once, cleanly, at the halfway point. Do not be tempted to fade the wet signal
 * with this value instead: that turns the bypass into a second, redundant depth control, and the
 * skip-the-loop optimisation above stops being possible. UnpatchedParamSwitch::readCurrentValue
 * duplicates this comparison so the menu agrees with what you hear; the two must stay in step.
 */
[[gnu::always_inline]] inline bool isEnabled(q31_t onParam) {
	return onParam > 0;
}

/*
 * The value UNPATCHED_GRISTLE_ON holds until something sets it explicitly.
 *
 * It is centre, so isEnabled() reads it as OFF and it behaves as an off switch in every way the
 * user can observe. Its second job is invisible: it tells the song loader that this param has
 * never been written, which is what lets inferLegacyGristleOn() reconstruct the switch for songs
 * saved before it existed WITHOUT keeping any read-time state. ModControllableAudio's file
 * readers are static, so there is nowhere to keep such state anyway.
 *
 * The menu writes only the two rails, and the tag reader stamps a definite value over this
 * sentinel the moment a gristleOn tag is seen, so a file written by this build never loads with
 * it still set. Normalising centre to the OFF rail there is behaviourally invisible - both read
 * as off - which is precisely why the sentinel can be an ordinary in-range value rather than
 * something that would need special-casing everywhere the param is touched.
 */
constexpr q31_t kOnUnset = 0;

/*
 * Shape the triangle. Returns a q31 waveform spanning the full range whatever the shape.
 *
 * !!! DO NOT REMOVE THE octaves > 0 GUARD !!!
 * functions.h carries the warning "lshift must be greater than 0! Not 0" above
 * lshiftAndSaturateUnknown. With a shift of zero it calls
 * signed_saturate_operand_unknown(val, 32), whose switch only covers 31 down to 13 and whose
 * default: is signed_saturate<12> — so the sample is clamped to TWELVE BITS and shifted by
 * nothing, roughly 120 dB down, silently. That exact call cost days on Sear (handoff bug 4)
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
	q31_t t = add_saturate(triangle, c.bias);

	q31_t a = (c.octaves > 0) ? lshiftAndSaturateUnknown(t, c.octaves) : t;
	q31_t b = lshiftAndSaturateUnknown(t, c.octaves + 1); // shift >= 1 always, safe

	// Lerp a -> b by frac. Plain subtraction is safe here and the reason is worth stating:
	// a and b are the same wave saturated at two adjacent shifts, so they always share its
	// sign, and |b - a| <= max(|a|, |b|). The int32 overflow case needs opposite signs, which
	// cannot arise. The result lies between a and b, so the add cannot overflow either.
	return a + (multiply_32x32_rshift32(c.frac, b - a) << 1);
}

/// One channel's filter memory. MUST be per-channel: running a single state over an
/// interleaved stereo buffer combs instead of filtering, the same trap noted for Sear's tone
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
	/// The slice cycle's own phase. Free-runs like lfoPhase and is pulled back toward the song's
	/// position once per buffer by alignSlicePhase(), rather than being recomputed per sample.
	uint32_t slicePhase;
	/// The step envelope, carried across buffers so an edge in progress is not restarted.
	q31_t sliceEnv;
	/// How much of the effect is currently in circuit, 0..ONE_Q31. Not the same thing as the
	/// master switch: this is how the switch is CROSSFADED. See kEnableRampStep.
	q31_t enableRamp;
	/// The rhythm actually being played, latched at the last step boundary. NOT read straight
	/// from Config: see sliceGates01() for why a pattern change waits for the next step.
	uint16_t sliceActiveMask;
	/// Lane B's latched rhythm. Latched at the same instant as lane A, so the two never describe
	/// different patterns even for one step.
	uint16_t sliceActiveMaskB;
	/// Lane B's envelope.
	q31_t sliceEnvB;
	/// Which step that latch happened on. 0xFF means "nothing latched yet", so the first sample
	/// after the effect comes on adopts the pattern immediately rather than playing silence.
	uint8_t sliceLastStep;
	/// The step length actually in use, which lags the knob. See latchSliceSync().
	uint8_t sliceActiveSync;
	/// The beat the last sync handover was measured against.
	int32_t sliceLastBeat;
};

/*
 * One sample, one channel.
 *
 * `shaped` is the output of shapeLFO() and is shared by both channels.
 */
/*
 * One sample, one channel.
 *
 * TWO MODULATORS, NOT ONE, since lane B arrived. `shapedVca` drives the amplitude and
 * `shapedFilter` the cutoff; pass the same value twice and this behaves exactly as it did when
 * there was one gate, which is what every single-lane pattern and the bare LFO do.
 *
 * `dirtScale` attenuates the distortion for this sample, and is ONE_Q31 unless the pattern sends
 * lane B to the dirt. It scales the MIX and the DRIVE together, so a step that is not accented
 * is clean rather than quietly distorted.
 */
[[gnu::always_inline]] inline q31_t processSample(q31_t sample, q31_t shapedVca, q31_t shapedFilter, q31_t dirtScale,
                                                  const Config& c, FilterState& state) {
	// LFO to 0..ONE_Q31, then invert: the modulator is how far DOWN we pull, so that zero
	// depth is exactly zero pull and therefore an exact bypass on both paths.
	q31_t lfo01 = unipolar(shapedVca);
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
			// shift, and add_saturate keeps the sum in range for softClipCubic, which requires
			// its input already inside [-1, 1].
			q31_t drive = multiply_32x32_rshift32(c.dirtDrive, dirtScale) << 1;
			q31_t mix = multiply_32x32_rshift32(c.dirtMix, dirtScale) << 1;
			q31_t boost = lshiftAndSaturate<1>(multiply_32x32_rshift32(vca, drive));
			q31_t driven = softClipCubic(add_saturate(vca, add_saturate(boost, boost)));
			// Lerp clean -> driven by dirtMix. Both operands share a sign here for the same
			// reason as in shapeLFO, so the subtraction cannot overflow.
			vca = vca + (multiply_32x32_rshift32(mix, driven - vca) << 1);
		}
	}

	q31_t filtered = 0;
	if (c.doFilter) {
		// The filter has its own pull, from its own modulator. On a single-lane pattern this is
		// the same number as above and the compiler folds it away; on a dual one it is the
		// second rhythm, which is the whole point of lane B.
		q31_t filterPull = multiply_32x32_rshift32(c.depth, ONE_Q31 - unipolar(shapedFilter)) << 1; // 0..ONE_Q31

		// Cutoff falls from fcBase toward kMinCoefficient as the modulator pulls down.
		q31_t span = c.fcBase - kMinCoefficient;
		q31_t fc = c.fcBase - (multiply_32x32_rshift32(filterPull, span) << 1);

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
		mixed = lshiftAndSaturate<1>(
		    add_saturate(multiply_32x32_rshift32(vca, ONE_Q31 - c.mode), multiply_32x32_rshift32(filtered, c.mode)));
	}
	else {
		mixed = c.doVca ? vca : filtered;
	}

	// Output trim.
	return multiply_32x32_rshift32(mixed, c.level) << 1;
}

// ---------------------------------------------------------------------------
// THE SLICER (phase 1) — a tempo-locked step gate standing beside the LFO
// ---------------------------------------------------------------------------

/*
 * WHAT THIS IS. The Gristleizer's LFO is free-running: it chops at whatever Rate says and has
 * no idea where the song is. A slicer pedal does the opposite — it gates in a fixed rhythmic
 * pattern locked to the tempo. This adds that second modulation source and one knob, Blend,
 * that mixes the two. Blend at minimum is the effect exactly as it was, and that is also the
 * default, so no song that predates this changes at all.
 *
 * THE OTHER KNOBS DO DOUBLE DUTY, DELIBERATELY. Shape becomes the slice's attack — how hard
 * each step's edge is — and Bias becomes its duty, how much of the step sounds. Those are
 * precisely what they already do to the LFO (Shape morphs triangle to square, Bias sets the
 * duty cycle of the shaped wave), so one hand position means the same thing on both sources.
 * Adding separate Attack and Duty params would have meant four more enum slots to say what
 * these two already say.
 *
 * THE PATTERN IS A BITMASK, NOT A TABLE OF ENVELOPES. Sixteen steps, one bit each, and the
 * envelope is generated from Shape and Bias at run time. That is what keeps the whole bank
 * inside 16 bytes and makes a variation a transform of a number rather than another table.
 *
 * VARIATIONS ARE DERIVED, NOT STORED. variation 0 is the base pattern; 1..10 rotate it and
 * add or drop one step, chosen by a hash of (pattern, variation). The hash is pure, so a
 * variation number always gives back the same rhythm — on this unit, on another unit, and
 * after a reload. Eleven per pattern matches what the pedal offers; eight patterns times
 * eleven is 88 rhythms out of eight literals.
 *
 * NOTHING HERE READS THE CLOCK. The call site passes the song's tick position in and this
 * header does arithmetic on it, the same division of labour as the rest of the file: the DSP
 * owns the law, the caller owns the plumbing.
 */

/// Steps per pattern cycle. Sixteen, so that at the 1/16 sync level one cycle is exactly one bar.
constexpr int32_t kNumSliceSteps = 16;
constexpr int32_t kNumSlicePatterns = 12;
/// Variations per pattern, matching the pedal. 0 is the base pattern itself.
constexpr int32_t kNumSliceVariations = 11;
constexpr int32_t kNumSliceSyncLevels = 8;

/*
 * A pattern is TWO rhythms, not one.
 *
 * Lane A gates the amplitude, lane B the filter — or, when `dirt` is set, lane B drives the
 * distortion instead and the filter follows lane A. That is the whole of the Dual and SFX idea:
 * one slicer where there used to be one gate.
 *
 * WHEN THE TWO LANES ARE EQUAL, NOTHING IS ADDED. The first eight patterns set b == a and behave
 * exactly as they did before lane B existed — same sound, same CPU once the compiler folds the
 * second envelope onto the first's result. Only the four at the end are genuinely two-handed.
 *
 * Written as hex because the literals are read here and nowhere else; the comment beside each is
 * the rhythm it spells.
 */
struct SlicePattern {
	uint16_t a; ///< drives the amplitude
	uint16_t b; ///< drives the filter, or the dirt when `dirt` is set
	bool dirt;  ///< lane B goes to the distortion rather than the filter
};

constexpr SlicePattern kSlicePatterns[kNumSlicePatterns] = {
    // Single. One rhythm, both lanes.
    {0x1111, 0x1111, false}, // 1000 1000 1000 1000  four on the floor
    {0x5555, 0x5555, false}, // 1010 1010 1010 1010  eighths
    {0x4444, 0x4444, false}, // 0010 0010 0010 0010  offbeats only
    {0x4D4D, 0x4D4D, false}, // 1011 0010 1011 0010  gallop
    {0x0F0F, 0x0F0F, false}, // 1111 0000 1111 0000  bursts of four
    {0x9249, 0x9249, false}, // 1001 0010 0100 1001  triplet feel across the bar
    {0x2929, 0x2929, false}, // 1001 0100 1001 0100  syncopated
    {0xFFFF, 0xFFFF, false}, // 1111 1111 1111 1111  every step — tremolo

    // Dual. Two rhythms at once, which is the thing a single lane cannot do at all.
    {0x1111, 0x4444, false}, // volume on the beat, filter on the offbeat — they interlock
    {0x5555, 0x9249, false}, // eighths against a triplet feel — three over two

    // Filter and drive. The amplitude holds steady or nearly so, and the SECOND lane is what you
    // hear moving: a filter opening in bursts, or the distortion arriving on the beat.
    {0xFFFF, 0x0F0F, false}, // volume constant, filter sweeps in bursts of four
    {0x5555, 0x1111, true},  // eighths, with the dirt landing on the beat
};

/*
 * Ticks per step at input-tick magnitude 0, i.e. as a fraction of the 96-tick bar that
 * Song::getBarLength() returns. Every entry divides 96 exactly, so no sync level ever lands
 * the cycle off the bar line.
 */
constexpr uint32_t kSliceSyncTicks[kNumSliceSyncLevels] = {
    3,  // 1/32
    4,  // 1/16 triplet
    6,  // 1/16
    8,  // 1/8 triplet
    12, // 1/8
    24, // 1/4
    48, // 1/2
    96, // one bar
};

/// The shortest slice, as a fraction of a step. Zero duty would be silence at every setting of
/// every other knob, which is a dead zone at the bottom of Bias rather than a usable sound.
constexpr q31_t kSliceMinDuty = 171798691; // 0.08

/*
 * Edge speed. A one-pole coefficient; the comments give the time constant at 44.1 kHz.
 *
 * MEASURED BY EAR, and the first version had it wrong. Shape used to reach ~0.1 ms at the square
 * end, and on hardware every step edge ticked from about knob 36 of 50 upward — an edge of around
 * 0.6 ms was already too fast for the speaker to follow. The ceiling now stops at 0.5 ms and the
 * centre moved down with it so the knob still sweeps smoothly instead of running into the clamp
 * a third of the way up.
 */
constexpr q31_t kSliceCoeffCentre = 7030000; // ~7 ms
constexpr int32_t kSliceCoeffOctaves = 4;
constexpr q31_t kSliceMinCoeff = 507500;   // ~90 ms, the soft flutter end
constexpr q31_t kSliceMaxCoeff = 97384000; // ~0.5 ms, the hard chop end

/*
 * Lane B never moves faster than this, whatever Shape says: ~3 ms.
 *
 * THE EAR IS NOT AS FORGIVING OF A CUTOFF JUMP AS OF A GAIN JUMP. A fast amplitude edge reads as
 * a transient, which is the point of a chop. A fast filter edge has no such cover — it is heard
 * as the zipper that stepping a cutoff always gives, and the resonance makes it worse because the
 * peak moves with it. Six times the amplitude floor, found by the same method: too fast is
 * audible, and there is no musical reason to go faster.
 */
constexpr q31_t kSliceMaxCoeffB = 16230500;

/*
 * The release never goes faster than this, whatever Shape says.
 *
 * THE CLICK IS ON THE WAY DOWN, NOT THE WAY UP. Gain rising fast is a transient the ear reads as
 * attack — it is what makes a chop percussive. Gain falling fast is a step toward silence with no
 * musical event to hide it, and that is what ticks. So the two edges get different floors: the
 * attack stays as fast as Shape asks, and the release stops at ~1.5 ms. Slicer and tremolo pedals
 * behave the same way, which is why their hardest settings sound tight rather than broken.
 */
constexpr q31_t kSliceMaxReleaseCoeff = 32461000; // ~1.5 ms

/// Half of q31, i.e. where Blend switches from mixing the LFO with the product to mixing the
/// product with the pattern. Named because it appears twice and means the same thing both times.
constexpr q31_t kSliceBlendMid = 1073741824;

/*
 * Fold a bipolar param onto one of `numOptions` equal bands.
 *
 * Pattern, Variation and Sync are lists, but they are stored as ordinary full-range params so
 * that automation, CCs and gold knobs need no special case — the same bargain the master switch
 * struck. Equal bands are what make a knob sweep land on every option in turn.
 *
 * THE MENU MUST WRITE sliceParamForIndex(), NOT THE BAND EDGE. Writing an edge leaves the
 * stored value one LSB away from the next band, so a value the menu wrote could read back as
 * its neighbour. The centre cannot.
 */
[[gnu::always_inline]] inline int32_t sliceIndex(q31_t param, int32_t numOptions) {
	int64_t offset = (int64_t)param + 2147483648LL; // 0 .. 2^32-1
	int32_t index = (int32_t)((offset * numOptions) >> 32);
	return std::clamp<int32_t>(index, 0, numOptions - 1);
}

/// The stored value for an option: the centre of its band. See sliceIndex().
inline q31_t sliceParamForIndex(int32_t index, int32_t numOptions) {
	int64_t value = -2147483648LL + ((int64_t)(2 * index + 1) * 4294967296LL) / (2 * numOptions);
	return (q31_t)std::clamp<int64_t>(value, -2147483648LL, 2147483647LL);
}

/*
 * The rhythm for a (pattern, variation) pair.
 *
 * Variation 0 returns the base untouched, so the eight literals above are always reachable.
 * Above that: rotate, then optionally add one step and optionally drop one. Rotation alone
 * would only ever give displacements of the same rhythm; the add and the drop are what make
 * the eleven feel like eleven.
 *
 * THE EMPTY-MASK GUARD IS NOT OPTIONAL. Dropping the only remaining step of a sparse pattern
 * gives a mask of zero, which is not a rhythm but a mute — and a mute that appears at one
 * variation number out of 88 is the kind of fault that is found on stage rather than here.
 */
/// One lane's mask after the variation transform. `salt` keeps the two lanes from moving in
/// lockstep on a dual pattern without needing a second hash function.
inline uint16_t sliceVaryLane(uint16_t base, int32_t pattern, int32_t variation, uint32_t salt) {
	if (variation <= 0) {
		return base;
	}

	// Two odd multipliers and two shifts. Enough avalanche that neighbouring variation numbers
	// give unrelated rhythms, and cheap enough to run once per buffer rather than caching.
	uint32_t h = ((uint32_t)pattern * 0x9E3779B1u) ^ ((uint32_t)variation * 0x85EBCA6Bu) ^ salt;
	h ^= h >> 15;
	h *= 0x2545F491u;
	h ^= h >> 13;

	int32_t rotation = (int32_t)(h & 15);
	uint16_t mask = (uint16_t)(((uint32_t)base << rotation) | ((uint32_t)base >> (16 - rotation)));

	// One step always added and one always dropped, with a second of each on half the seeds.
	// ROTATION ALONE IS NOT ENOUGH, and the four-on-the-floor pattern is the proof: it is
	// symmetric every four steps, so its sixteen rotations are only four rhythms, and eleven
	// variations of it would have been four. Measured over all eight single patterns, adding and
	// dropping takes the bank from 62 distinct rhythms out of 88 to 85.
	mask |= (uint16_t)(1u << ((h >> 8) & 15));
	if (h & 0x10u) {
		mask |= (uint16_t)(1u << ((h >> 12) & 15));
	}

	// A drop is skipped rather than allowed to empty the mask. An empty mask is not a rhythm but
	// a mute, and a mute at one variation number out of 88 is the kind of fault that is found on
	// stage rather than here.
	uint16_t drop = (uint16_t)(1u << ((h >> 16) & 15));
	if (mask & (uint16_t)~drop) {
		mask &= (uint16_t)~drop;
	}
	if (h & 0x20u) {
		uint16_t drop2 = (uint16_t)(1u << ((h >> 20) & 15));
		if (mask & (uint16_t)~drop2) {
			mask &= (uint16_t)~drop2;
		}
	}

	return mask ? mask : base;
}

/*
 * Both lanes for a (pattern, variation) pair.
 *
 * A SINGLE PATTERN STAYS SINGLE AT EVERY VARIATION. When the two base lanes are equal the same
 * transformed mask is used for both, rather than being varied twice with different salt — vary
 * them separately and pattern 0 variation 4 silently becomes a dual pattern, which is not what
 * the list on screen says it is.
 */
inline SlicePattern sliceMasksFor(int32_t pattern, int32_t variation) {
	SlicePattern base = kSlicePatterns[std::clamp<int32_t>(pattern, 0, kNumSlicePatterns - 1)];
	SlicePattern out = base;

	out.a = sliceVaryLane(base.a, pattern, variation, 0u);
	out.b = (base.b == base.a) ? out.a : sliceVaryLane(base.b, pattern, variation, 0x5BD1E995u);
	return out;
}

/// Ticks per step at the song's current tick magnitude. Mirrors Song::getBarLength(), which is
/// increaseMagnitude(96, magnitude) — so a pattern at the 1/16 level is a bar wide at any
/// magnitude, and a negative magnitude shifts the other way rather than overflowing.
inline uint32_t sliceTicksPerStep(int32_t syncIndex, int32_t tickMagnitude) {
	int32_t ticks = increaseMagnitude(
	    (int32_t)kSliceSyncTicks[std::clamp<int32_t>(syncIndex, 0, kNumSliceSyncLevels - 1)], tickMagnitude);
	return (uint32_t)std::max<int32_t>(ticks, 1);
}

/*
 * Decode the four slice knobs. Called once per buffer, beside setup().
 *
 * Kept separate from setup() rather than folded into it: setup()'s nine arguments are the
 * original box's front panel, and the two lines of code saved by merging are not worth losing
 * that. Shape and Bias arrive here a second time because this stage reads them as attack and
 * duty — the same numbers, a different law.
 */
inline void setupSlice(Config& c, q31_t patternParam, q31_t variationParam, q31_t syncParam, q31_t blendParam,
                       q31_t shapeParam, q31_t biasParam) {
	c.blend = unipolar(blendParam);
	c.doSlice = (c.blend != 0);
	if (!c.doSlice) {
		// Nothing below is read when the pattern is not mixed in. Left definite anyway, because
		// a Config with junk in it is a debugging trap the moment Blend is automated upward.
		c.sliceMask = 0;
		c.sliceMaskB = 0;
		c.sliceDirtDest = false;
		c.sliceDutyPhase = 0;
		c.sliceAttack = kSliceCoeffCentre;
		c.sliceRelease = kSliceCoeffCentre;
		c.sliceAttackB = kSliceCoeffCentre;
		c.sliceReleaseB = kSliceCoeffCentre;
		c.sliceSyncIndex = 0;
		return;
	}

	c.sliceSyncIndex = sliceIndex(syncParam, kNumSliceSyncLevels);
	SlicePattern masks =
	    sliceMasksFor(sliceIndex(patternParam, kNumSlicePatterns), sliceIndex(variationParam, kNumSliceVariations));
	c.sliceMask = masks.a;
	c.sliceMaskB = masks.b;
	c.sliceDirtDest = masks.dirt;

	// Duty: the fraction of each step the gate stays open, floored so the shortest slice is
	// still a slice. Held as a full 32-bit phase because that is what it is compared against.
	q31_t duty =
	    add_saturate(kSliceMinDuty, multiply_32x32_rshift32(ONE_Q31 - kSliceMinDuty, unipolar(biasParam)) << 1);
	c.sliceDutyPhase = (uint32_t)duty << 1;

	// Attack: Shape's own exponential law, four octaves around 7 ms. Clamped at both ends — the
	// top end is what keeps an edge from becoming a click, so it is not merely a backstop.
	c.sliceAttack = std::clamp<q31_t>(getExp(kSliceCoeffCentre, (shapeParam >> 5) * kSliceCoeffOctaves), kSliceMinCoeff,
	                                  kSliceMaxCoeff);
	// The release follows Shape until Shape gets faster than the release floor, then stops there.
	// Taking the slower of the two rather than a fixed value keeps the soft end of the knob
	// symmetrical, where a long fade in and a long fade out are the whole point.
	c.sliceRelease = std::min<q31_t>(c.sliceAttack, kSliceMaxReleaseCoeff);

	// Lane B follows Shape too, under its own slower ceiling both ways round. Both edges are
	// capped here, not just the release: a cutoff that snaps open zippers exactly as one that
	// snaps shut does, which is the difference between this lane and the amplitude one.
	c.sliceAttackB = std::min<q31_t>(c.sliceAttack, kSliceMaxCoeffB);
	c.sliceReleaseB = std::min<q31_t>(c.sliceRelease, kSliceMaxCoeffB);
}

/// Both lanes' openings for one sample.
struct SliceGates {
	q31_t a; ///< amplitude
	q31_t b; ///< filter, or dirt
};

/*
 * One sample of the pattern gates, each as a 0..ONE_Q31 opening.
 *
 * The envelopes are one-poles toward each step's target rather than the targets themselves. That
 * is the whole of Attack, and it is also what makes this safe: a bare 0-to-1 step on a VCA is a
 * click at every step edge, at every setting, and on a cutoff it is a zipper.
 */
[[gnu::always_inline]] inline SliceGates sliceGates01(uint32_t phase, const Config& c, Memory& m) {
	int32_t step = (int32_t)(phase >> 28); // 0..15
	uint32_t stepPhase = phase << 4;       // position within this step

	/*
	 * A NEW PATTERN WAITS FOR THE NEXT STEP. Heard on hardware: turning Variation while the
	 * effect was running clicked, and not because of the envelope — the rhythm changed under a
	 * step that was already sounding, so a gate that was open became closed mid-slice and the
	 * envelope had to chase it from wherever it was.
	 *
	 * Latching at the step boundary removes the discontinuity rather than smoothing it: every
	 * step is played out as whatever it was when it started. The new rhythm lands within one
	 * step, which at 1/16 is 125 ms at 120 BPM, so it still feels immediate under the hand.
	 * This is also the musically right answer — a rhythm should change on a beat division, not
	 * partway through one.
	 *
	 * BOTH LANES LATCH TOGETHER, in the same branch, so the two can never be describing
	 * different patterns even for a single step.
	 */
	if ((uint8_t)step != m.sliceLastStep) {
		m.sliceActiveMask = c.sliceMask;
		m.sliceActiveMaskB = c.sliceMaskB;
		m.sliceLastStep = (uint8_t)step;
	}

	bool openA = ((m.sliceActiveMask >> step) & 1) && (stepPhase < c.sliceDutyPhase);
	bool openB = ((m.sliceActiveMaskB >> step) & 1) && (stepPhase < c.sliceDutyPhase);

	// Both operands are 0..ONE_Q31, so the difference cannot overflow int32 and the result
	// stays inside the same range: a one-pole can never overshoot its target.
	q31_t targetA = openA ? ONE_Q31 : 0;
	q31_t coefficientA = (targetA > m.sliceEnv) ? c.sliceAttack : c.sliceRelease;
	m.sliceEnv = m.sliceEnv + (multiply_32x32_rshift32(coefficientA, targetA - m.sliceEnv) << 1);

	q31_t targetB = openB ? ONE_Q31 : 0;
	q31_t coefficientB = (targetB > m.sliceEnvB) ? c.sliceAttackB : c.sliceReleaseB;
	m.sliceEnvB = m.sliceEnvB + (multiply_32x32_rshift32(coefficientB, targetB - m.sliceEnvB) << 1);

	return {m.sliceEnv, m.sliceEnvB};
}

/*
 * Mix the two modulators and hand back something shapeLFO() could have produced, so that
 * processSample() needs no knowledge of any of this.
 *
 * THE MIDDLE OF BLEND IS THE PRODUCT, NOT A CROSSFADE. Fading one source into the other would
 * make the centre of the knob the least interesting place on it — half of each, with the
 * rhythm half-erased. Multiplying instead gives the thing neither source can do alone: the
 * pattern's rhythm with the LFO's slow throb riding over it. So the knob runs LFO, product,
 * pattern, in that order, and both ends are still exactly one source.
 */
[[gnu::always_inline]] inline q31_t blendSlice(q31_t lfoShaped, q31_t slice01, const Config& c) {
	q31_t lfo01 = unipolar(lfoShaped);
	q31_t product01 = multiply_32x32_rshift32(lfo01, slice01) << 1;

	q31_t out01;
	if (c.blend <= kSliceBlendMid) {
		// Saturating shifts: at exactly the midpoint this doubles 2^30 onto the rail, and a
		// plain shift would wrap it to negative — the one place this arithmetic can bite.
		q31_t t = lshiftAndSaturate<1>(c.blend);
		out01 = lfo01 + (multiply_32x32_rshift32(t, product01 - lfo01) << 1);
	}
	else {
		q31_t t = lshiftAndSaturate<1>(c.blend - kSliceBlendMid);
		out01 = product01 + (multiply_32x32_rshift32(t, slice01 - product01) << 1);
	}

	// Back into the bipolar domain processSample() expects: the exact inverse of unipolar().
	return lshiftAndSaturate<1>(out01 - kSliceBlendMid);
}

/*
 * How fast the master switch crosses over: ~5 ms at 44.1 kHz.
 *
 * WHY THE SWITCH NEEDS A FADE AT ALL. Heard on hardware as a loud pop on switching OFF, and the
 * arithmetic says exactly why: at the instant of the switch the output goes from whatever the
 * effect was doing — a slice that happens to be closed, so silence, or a filtered and
 * level-trimmed signal — straight to the full dry signal in one sample. That is a step of up to
 * full scale. The Slicer makes it worse rather than causing it: a chop spends much of its time
 * near zero, so the step is usually near maximum.
 *
 * Five milliseconds is long enough that the crossover cannot be heard as a click and short
 * enough that the switch still feels instant under the hand — the same order as the release
 * floor on a slice edge, for the same reason.
 *
 * THE ZERO-COST BYPASS SURVIVES THIS. The effect keeps running only while the ramp is still
 * moving; once it reaches zero the sample loop is skipped exactly as before, so a song that
 * never switches the effect on still pays nothing.
 */
constexpr q31_t kEnableRampStep = 9761289;

/// Walk the crossfade one sample toward its target, without overshooting it.
[[gnu::always_inline]] inline q31_t advanceEnableRamp(q31_t ramp, q31_t target) {
	if (ramp < target) {
		return std::min<q31_t>(add_saturate(ramp, kEnableRampStep), target);
	}
	if (ramp > target) {
		return std::max<q31_t>(ramp - kEnableRampStep, target);
	}
	return ramp;
}

/*
 * Blend the effect against the untouched signal by the crossfade.
 *
 * Built as two halved multiplies and one shared shift rather than dry + ramp * (wet - dry): the
 * difference of two full-scale samples of opposite sign overflows int32, and this is the one
 * place in the file where both operands are audio rather than one being a control value.
 */
[[gnu::always_inline]] inline q31_t crossfadeBypass(q31_t dry, q31_t wet, q31_t ramp) {
	return lshiftAndSaturate<1>(
	    add_saturate(multiply_32x32_rshift32(dry, ONE_Q31 - ramp), multiply_32x32_rshift32(wet, ramp)));
}

/// Reset the step latch, so that the next sample adopts the current pattern rather than one
/// left over from before a bypass. Paired with parking the envelope at the call site.
[[gnu::always_inline]] inline void resetSliceLatch(Memory& m) {
	m.sliceLastStep = 0xFF;
	m.sliceActiveMask = 0;
	m.sliceActiveMaskB = 0;
	m.sliceActiveSync = 0xFF;
}

/*
 * Adopt a new step length at a moment where the jump cannot be heard.
 *
 * WHY SYNC NEEDS MORE THAN THE PATTERN LATCH. Changing the pattern swaps which steps sound.
 * Changing Sync changes how long a step IS, which changes the length of the whole cycle — so the
 * phase the song implies moves too, and alignSlicePhase() sees an error big enough to snap.
 * Landing that snap in the middle of a sounding slice is the bump heard on hardware.
 *
 * Three moments are safe, in the order they are tried:
 *   - the gate is closed. The output is already silence, so nothing can step. This is the common
 *     case and it usually arrives within a step or two.
 *   - the transport is stopped. Nothing is playing to disturb.
 *   - the beat turns over. The worst case, for a pattern with no gaps at all (Tremolo at full
 *     duty), and still under half a second at 120 BPM.
 *
 * WAITING FOR THE CYCLE INSTEAD WOULD BE WORSE, not better: at the one-bar setting a cycle is
 * sixteen bars, and a knob that takes half a minute to answer reads as broken.
 */
[[gnu::always_inline]] inline void latchSliceSync(Config& c, Memory& m, bool clockActive, int64_t swungTick,
                                                  int32_t tickMagnitude) {
	/// Below this the gate is closed for all practical purposes: about -48 dB.
	constexpr q31_t kSliceEnvClosed = 8388608;

	int32_t quarterTicks = std::max<int32_t>(increaseMagnitude(24, tickMagnitude), 1);
	int32_t beat = clockActive ? (int32_t)((swungTick / quarterTicks) & 0x7FFFFFFF) : 0;
	bool beatTurnedOver = clockActive && (beat != m.sliceLastBeat);

	if (m.sliceActiveSync == 0xFF || !clockActive || m.sliceEnv < kSliceEnvClosed || beatTurnedOver) {
		m.sliceActiveSync = (uint8_t)std::clamp<int32_t>(c.sliceSyncIndex, 0, kNumSliceSyncLevels - 1);
	}
	if (clockActive) {
		m.sliceLastBeat = beat;
	}

	// Everything downstream reads the latched value, never the knob.
	c.sliceSyncIndex = (int32_t)m.sliceActiveSync;
}

/*
 * Lane B against unity, for the patterns that send it to the dirt.
 *
 * Not blendSlice(): that one mixes against the LFO and hands back a bipolar wave for the gates.
 * This one starts at "no attenuation" and walks toward the gate as Blend comes up, so at Blend
 * zero the distortion is exactly what the Dirt knob says and the pattern is not in circuit at
 * all — the same promise the rest of the Slicer makes.
 */
[[gnu::always_inline]] inline q31_t blendToUnity(q31_t gate01, const Config& c) {
	// gate01 - ONE_Q31 is at most one full scale in magnitude and never positive, so neither the
	// multiply nor the sum can leave q31.
	return ONE_Q31 + (multiply_32x32_rshift32(c.blend, gate01 - ONE_Q31) << 1);
}

/// Where the song says the cycle is, and how fast to walk it. Filled once per buffer.
struct SliceClock {
	uint32_t increment; ///< phase per sample
	uint32_t target;    ///< the phase the song's position implies, when it has one
	bool haveTarget;    ///< false when the transport is stopped, i.e. free-run
};

/*
 * Work out the cycle's speed and position from the transport.
 *
 * `timePerTick` is audio-clock units per internal tick and `tickRemainder` how far past the
 * last swung tick we are, both straight from PlaybackHandler. Under an external clock the
 * remainder is always zero — PlaybackHandler says so itself — which is why the caller must
 * ease onto the target rather than snap to it every buffer. See alignSlicePhase().
 */
inline SliceClock sliceClock(const Config& c, uint32_t timePerTick, int32_t tickMagnitude, bool clockActive,
                             int64_t swungTick, uint32_t tickRemainder) {
	SliceClock clock{0, 0, false};

	uint32_t ticksPerStep = sliceTicksPerStep(c.sliceSyncIndex, tickMagnitude);
	uint64_t cycleTicks = (uint64_t)ticksPerStep * kNumSliceSteps;
	uint64_t samplesPerCycle = cycleTicks * (uint64_t)timePerTick;

	// A cycle shorter than a buffer is not a rhythm, and dividing by it would give an increment
	// that wraps several times a sample. Tempo can only get here through a param, so clamp
	// rather than assume.
	if (samplesPerCycle < 64) {
		samplesPerCycle = 64;
	}
	clock.increment = (uint32_t)(0x100000000ULL / samplesPerCycle);

	if (clockActive && timePerTick > 0) {
		uint64_t posSamples = (uint64_t)(uint32_t)(swungTick % (int64_t)cycleTicks) * timePerTick + tickRemainder;
		clock.target = (uint32_t)((posSamples << 32) / samplesPerCycle);
		clock.haveTarget = true;
	}

	return clock;
}

/*
 * Pull the running phase toward where the song says it should be.
 *
 * SNAP FOR A JUMP, EASE FOR DRIFT. A transport start, a loop, a position pointer — those are
 * real jumps and the pattern should land on the beat immediately. Everything smaller is either
 * rounding or the one-tick granularity of an external clock, and snapping to that every buffer
 * would make the slicer stutter in time with the incoming clock instead of the music. An eighth
 * of the error per buffer converges in a few milliseconds and is inaudible.
 */
[[gnu::always_inline]] inline void alignSlicePhase(Memory& m, const SliceClock& clock) {
	if (!clock.haveTarget) {
		return;
	}
	// Deliberately signed wraparound arithmetic: the shorter way round a circle.
	int32_t error = (int32_t)(clock.target - m.slicePhase);
	constexpr int32_t kSliceSnapThreshold = 67108864; // 1/64 of a cycle
	if (error > kSliceSnapThreshold || error < -kSliceSnapThreshold) {
		m.slicePhase = clock.target;
	}
	else {
		m.slicePhase += error >> 3;
	}
}

} // namespace deluge::dsp::gristle
