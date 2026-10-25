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
 * tone control. Since 2026-10-05 the clipper is wrapped in a mid push (low band held back going
 * in, restored coming out) and fed a tracking bias, so a driven voice comes out as a pulse with
 * even harmonics rather than a plain square — see THE BITE STAGES below.
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
/// ONE INSTANCE COVERS BOTH CHANNELS, and that is deliberate. This holds a GAIN, and L and R
/// must get the same gain or the stereo image walks around as the drive changes. The filters
/// that DO hold signal history live in SearChannel below, one per channel.
///
/// WHERE THIS LIVES MATTERS AS MUCH AS WHAT IT HOLDS. A Voice owns the live copy, but a Voice
/// does not survive a note — Sound::acquireVoice() placement-constructs one out of a pool on
/// every note-on — so a Voice-only instance is cold on every note, which is the +8 dB case
/// above. Sound therefore keeps a seed (Sound::searLevelSeed) that each Voice copies on
/// construction and writes back after each block. Only the first note a Sound ever plays is
/// cold; measured warm-start error on every subsequent note is 0.000 dB.
struct SearLevel {
	q31_t envIn;     ///< mean |sample| entering the stage
	q31_t envOut;    ///< mean |sample| leaving it, before correction
	q31_t gain;      ///< correction currently applied, q31 meaning 0..4x (see kSearGainHeadroomShift)
	uint32_t warmup; ///< samples since the stage was last enabled, held at kSearWarmupSamples
	q31_t lastDrive; ///< drive at the end of the previous block; the next block glides from here
	q31_t wet;       ///< dry/wet crossfade position, 0..ONE_Q31; only ever between the ends while fading
	uint32_t snap;   ///< nonzero after silence: the next block jumps straight to its drive and wet
};

/*
 * THE BITE STAGES (2026-10-05). Reported from hardware: Sear "doesn't distort much". It was not a
 * level problem. Sear runs per voice, so it only ever clips ONE oscillator at a time, and a lone
 * saw through a symmetric clipper just turns squarer — the ear reads that as a different
 * waveform, not as dirt. Three changes, none of them a new param or a new file tag:
 *
 * 1. MID PUSH. Before the clipper, half the band below ~220 Hz is taken out (a 6 dB low cut);
 *    after it, an exact inverse puts it back. Small-signal the pair is flat, so with the drive
 *    down nothing changes. Driven, the lows reach the knee 6 dB later than everything above them,
 *    so the clipping happens in the mids where it is heard and the bottom stays tight instead of
 *    farting out. This is the pedal "pre/de-emphasis" idea, but the de-emphasis is the true
 *    inverse of the pre-emphasis rather than a separate tone stack, which is what keeps it flat
 *    at knob 0.
 *
 * 2. TRACKING BIAS. The signal is pushed off centre before the gain, by an amount proportional to
 *    its own measured level (envIn). A biased signal crosses zero off-centre, so once it is driven
 *    into the rails the result is a PULSE, not a square — and a pulse carries the even harmonics a
 *    square cannot. Because the bias tracks the input level, the duty shift is the same on a loud
 *    note and a quiet one, rather than a fixed offset that only bites on quiet material. At full
 *    bias a saw driven hard comes out roughly 65/35. None of Saturation (tanh, symmetric), Fold
 *    or the Gristleizer's Dirt do this, which is what keeps Sear its own thing. The DC this leaves
 *    is removed by a ~14 Hz blocker after the clipper.
 *
 * 3. MORE LOUDNESS ALLOWANCE. The residual lift now reaches its ceiling (1.6x, +4.1 dB of
 *    allowance) by about knob 20, rather than creeping up linearly to 1.5x at the very top. The
 *    old law held output within ±0.4 dB up to knob 15, which is exactly the range people play in.
 *
 * 4. PER SOUND, NOT PER VOICE (2026-10-05). After 1-3, still heard as "not very pronounced on a
 *    saw, it just sounds louder". That is the per-voice limit: one oscillator through any clipper
 *    is a timbre change. (An octave-fuzz fold was tried next and dropped — the character wasn't
 *    wanted.) Sear now runs once per Sound on the summed, panned voices, ahead of mod FX, delay and
 *    reverb — so chords, unison and the two oscillators intermodulate the way they would through
 *    a pedal. The drive is still the patched LOCAL_SEAR, taken as the highest of the voices'
 *    values each block; see Sound::searDriveThisBlock. Also: kSearPostMixBoostOctaves.
 *
 * HEADROOM. Every stateful step works at half scale so that no difference of two q31 values can
 * overflow, and the clipper's output is halved again before the DC blocker because a biased
 * clipper's output is offset. Net small-signal gain through the chain is therefore 0.375, and
 * the correction gain has to be able to reach above unity — hence the 0..4x gain range.
 */

/// ~220 Hz at 44.1 kHz: lp += (x - lp) >> 5. Corner of the low band the mid push holds back.
constexpr int32_t kSearEmphShift = 5;

/// ~14 Hz. DC blocker after the biased clipper.
constexpr int32_t kSearDcShift = 9;

/// Ceiling of the tracking bias, as a fraction of the input's mean |sample| (in the half-scale
/// domain). 0.6 here puts a hard-driven saw at about 65/35.
constexpr q31_t kSearMaxBias = 0.6 * ONE_Q31;

/// Ceiling of the residual loudness lift: the corrected output is allowed to sit this much above
/// the input, i.e. 1.6x of mean |sample|, +4.1 dB of allowance.
constexpr q31_t kSearMaxLift = 0.6 * ONE_Q31;

/// Extra drive, in octaves, on top of the knob's 1x..256x. Sear runs on the SUMMED voices of a Sound
/// since 2026-10-05, after each voice's amplitude envelope and volume have been applied, so it sees
/// a much quieter signal than the raw oscillator buffer it was tuned on. 2 octaves (12 dB) puts
/// first audible grit back near the bottom of the knob. Only the distortion amount changes — the
/// auto-level holds the output loudness either way. An estimate from the render path, not a
/// measurement on hardware: if the knob bites too early or too late, this is the number to move.
constexpr int32_t kSearPostMixBoostOctaves = 2;

/// The correction gain is stored as gain/4, so ONE_Q31 means 4x. Applying it therefore needs a
/// shift of 3: 2 for the /4 and 1 to undo multiply_32x32_rshift32's halving.
constexpr int32_t kSearGainHeadroomShift = 2;

/*
 * WHY EVERY FUNCTION THAT TOUCHES SEAR STATE IS [[gnu::noinline]] (2026-10-05).
 *
 * Since Sear moved onto Sound, its state lives inside the Sound object. Sound's alignof is 8, so when
 * these helpers were inlined into Sound's constructor GCC knew `this` was 8-aligned and zeroed the
 * SearChannel pair with `vst1.64 {d16,d17}, [r3:64]` — a NEON store with a 64-bit alignment hint.
 * The Deluge's allocator only guarantees 4-byte alignment, so on hardware that store took an
 * alignment fault and the unit hard-faulted at boot, the moment the first Sound was constructed
 * (build 4ef4f6fb9, crash screen pointer 0x20113A3E → LR-8 = that vst1).
 *
 * Out of line, these functions only see a SearChannel* / SearLevel* / q31_t*, whose alignof is 4,
 * so GCC cannot assume more and emits plain word accesses. Keep them out of line, and keep any new
 * code that touches Sear state on Sound behind one of them, rather than writing members directly
 * from inline Sound code.
 */

/// Per-channel filter memory for the bite stages. Holds signal history, so — like the tone stage —
/// it MUST be one per channel; sharing it across an interleaved buffer combs.
struct SearChannel {
	q31_t preLp;  ///< low band of the input, half scale
	q31_t dc;     ///< DC follower after the clipper
	q31_t postLp; ///< low band inside the de-emphasis loop, half scale
};

[[gnu::noinline]] inline void searChannelReset(SearChannel& ch) {
	ch.preLp = 0;
	ch.dc = 0;
	ch.postLp = 0;
}

/// Cold-start initialiser. Called once per Sound, NOT per note — see SearLevel above.
[[gnu::noinline]] inline void searLevelReset(SearLevel& state) {
	state.envIn = 0;
	state.envOut = 0;
	// The chain's small-signal gain is 0.375, so start at its inverse (8/3x, stored /4).
	state.gain = 0.6666666 * ONE_Q31;
	state.warmup = 0;
	state.lastDrive = 0;
	state.wet = 0;
	state.snap = 1;
}

/// Everything Sound holds for Sear, zeroed in one out-of-line call. See the note on [[gnu::noinline]].
[[gnu::noinline]] inline void searResetAll(SearLevel& level, SearChannel* channels, int32_t numChannels,
                                            q31_t* toneState) {
	searLevelReset(level);
	for (int32_t i = 0; i < numChannels; i++) {
		searChannelReset(channels[i]);
		toneState[i] = 0;
	}
}

/*
 * Sear (drive stage).
 *
 * Pre-gain sweeps 1x to 256x on an EXPONENTIAL taper — gain = 2^(8 * level).
 *
 * The first hardware test used a linear 1x..33x law and was, in George's words, "not very
 * audible". It was ~4x weaker than the wavefolder sitting next to it, and a linear taper wastes
 * the knob. The exponential taper fixes both. `s` is the integer part and `f` the fraction;
 * 2^s * (1 + f) is the standard linear-interpolation-in-the-exponent approximation, continuous at
 * every octave boundary because 2^s * 2 == 2^(s+1).
 *
 * Measured against the REAL param pipeline, knob position 0..50:
 *
 *     knob   0    3    5   10   15   20   25   30   35    40    45    50
 *     gain  1.0  1.5  1.8  3.2  5.6  9.6   16   29   51    90   154   256
 *
 * Getting here needed BOTH the >>26 below and moving LOCAL_SEAR out of the patcher's volume block
 * (see param.h).
 *
 * TWO PASSES, NOT ONE. The gain for a block cannot be known until the block has been clipped, so
 * pass 1 clips and measures and pass 2 applies.
 *
 * `channels` is 1 for the mono path, 2 for an interleaved stereo buffer, and `ch` must point at
 * that many SearChannel. The drive itself is still memoryless; it is only the bite filters that
 * need the stride.
 */
[[gnu::noinline]] inline void searBuffer(q31_t* startSample, q31_t* endSample, q31_t levelFrom, q31_t level,
                                         SearLevel& state, SearChannel* ch, int32_t channels) {
	if (level <= 0 && levelFrom <= 0) {
		// Bypass — mirrors how LOCAL_FOLD is gated at the call site. Rewind the warm-up so that
		// turning the knob back up re-measures rather than resuming a stale gain.
		state.warmup = 0;
		return;
	}

	// THE SHIFT IS TIED TO THE PARAM PIPELINE, not to q31. `level` is
	// paramFinalValues[LOCAL_SEAR], which tops out just under 2^29 (neutral value 25*10737418 and
	// getFinalParameterValueLinear's `<<3`). So >>26 is what yields 0..7 octaves. If the neutral
	// value or the param's patcher block ever changes, recompute this.
	//
	// THE DRIVE GLIDES ACROSS THE BLOCK, from `levelFrom` (where the last block ended) to `level`
	// (2026-10-05). It used to be one value per block. The knob sends steps, and on a fast turn several
	// arrive per block; through a gain that spans 1x..1024x a step lands as a jump in the waveform at
	// the block boundary - a pop - and the tracking bias, which moves the signal's DC, stepped with
	// it. Gliding per frame (both channels of a frame share one value, so L and R stay bit-identical)
	// turns each step into a ramp a block long. Cost: a few shifts and one multiply per frame.
	const int32_t numFrames = std::max<int32_t>(1, static_cast<int32_t>(endSample - startSample) / channels);
	const q31_t levelStep = (level - levelFrom) / numFrames;
	q31_t levelNow = levelFrom;

	// Residual loudness lift, so the knob is not dead flat. Correcting all the way to 0.00 dB
	// makes drive read as "nothing is happening" — the ear takes the loudness rise as part of the
	// effect. Rises to kSearMaxLift by knob ~20 and holds there. The comparison is done BEFORE the
	// shift: level << 3 overflows from level 2^28 (knob 25) up.
	const q31_t lift = (level < (kSearMaxLift >> 3)) ? (level << 3) : kSearMaxLift;


	q31_t envIn = state.envIn;
	q31_t envOut = state.envOut;
	uint32_t warmup = state.warmup;
	// getMagnitude(w) is floor(log2(w)), so the time constant doubles as the count doubles.
	int32_t detShift = (warmup >= kSearWarmupSamples) ? kSearDetectorShift
	                                                  : std::min<int32_t>(getMagnitude(warmup + 1), kSearDetectorShift);

	// Tracking bias. envIn is mean |c|, so this is a fixed fraction of the signal's own size, in the
	// same half-scale domain as `pushed`. Taken ONCE PER BLOCK from the level at the block's start,
	// not per sample: on an interleaved buffer a per-sample value differs between L and R by one
	// detector step, and at high drive that moves the clipped edges by a fraction of a sample —
	// measured as 3% L/R difference on a mono source. Per block, L and R are bit-identical, and the
	// detector moves far too slowly (23 ms) for the block steps to matter. (The drive, and the bias
	// AMOUNT that follows it, now glide per frame - see the top of this function. envIn stays per
	// block for the reason above.)
	const q31_t envInBlock = envIn;
	int32_t octaves = 0;
	q31_t frac = 0;
	q31_t bias = 0;

	// Local copies, so the compiler is not forced to reload them through a pointer that could
	// alias the sample buffer.
	SearChannel f[2];
	f[0] = ch[0];
	f[1] = (channels > 1) ? ch[1] : ch[0];
	int32_t c_i = 0;
	q31_t blockPeak = 0;

	// --- Pass 1: push, bias, drive, clip, restore, measure. ------------------------------------
	q31_t* currentSample = startSample;
	do {
		SearChannel& s = f[c_i];
		const q31_t c = *currentSample;

		if (c_i == 0) {
			// New frame: step the drive. The last frame lands exactly on `level` give or take the
			// division's remainder, which the next block's levelFrom picks up.
			levelNow += levelStep;
			// The guard is defensive: the level is documented as topping out just under 2^29, but a
			// future change to the param's neutral value would turn these left shifts into sign flips.
			const bool inRange = levelNow < (1 << 29);
			octaves = std::max<int32_t>(levelNow, 0) >> 26;
			frac = static_cast<q31_t>((static_cast<uint32_t>(std::max<int32_t>(levelNow, 0)) << 5) & 0x7FFFFFFF);
			// Tracking bias, reaching kSearMaxBias at knob ~30.
			const q31_t biasAmount =
			    inRange ? std::min<q31_t>(std::max<int32_t>(levelNow, 0) << 2, kSearMaxBias) : kSearMaxBias;
			bias = multiply_32x32_rshift32(envInBlock, biasAmount);
		}

		// Mid push, half scale. preLp follows c/2, so `pushed` is (c/2) * (1 - LP/2): lows at
		// -6 dB, everything above the corner untouched. |pushed| <= 0.75, so nothing overflows.
		s.preLp += ((c >> 1) - s.preLp) >> kSearEmphShift;
		const q31_t pushed = (c >> 1) - (s.preLp >> 1);

		const q31_t biased = add_saturate(pushed, bias);

		// biased * (1 + frac), then 2^(octaves + 1). The +1 undoes the half scale, and it also
		// means the shift is never zero — lshiftAndSaturateUnknown(val, 0) is FORBIDDEN (it clamps
		// to twelve bits, i.e. silence; that was the muting bug at the bottom of the knob). Keep
		// the +1 or reinstate an explicit octaves == 0 guard.
		const q31_t driven = add_saturate(biased, 2 * multiply_32x32_rshift32(biased, frac));
		const q31_t x = lshiftAndSaturateUnknown(driven, octaves + 1 + kSearPostMixBoostOctaves);

		// Clip, then halve: a biased clipper's output is offset, so it needs the headroom before
		// the DC blocker subtracts. |y| < 1 by construction.
		const q31_t y = softClipCubic(x) >> 1;

		// DC blocker. y and s.dc are both within [-0.5, 0.5], so the difference cannot overflow.
		s.dc += (y - s.dc) >> kSearDcShift;
		const q31_t v = y - s.dc;

		// De-emphasis: the exact inverse of the mid push, (1/2) / (1 - LP/2), with postLp following
		// z. DC gain 1, HF gain 1/2, so |z| <= max|v| < 1. The update is done at half scale for the
		// same overflow reason as the push.
		const q31_t z = (v >> 1) + (s.postLp >> 1);
		s.postLp += ((z >> 1) - (s.postLp >> 1)) >> (kSearEmphShift - 1);

		*currentSample = z;

		// subtract_saturate rather than negation: |NEGATIVE_ONE_Q31| is not representable.
		const q31_t a = (c < 0) ? subtract_saturate(0, c) : c;
		blockPeak = std::max(blockPeak, a);
		const q31_t b = (z < 0) ? subtract_saturate(0, z) : z;
		envIn += (a - envIn) >> detShift;
		envOut += (b - envOut) >> detShift;

		if (warmup < kSearWarmupSamples) {
			warmup += 1;
			detShift = std::min<int32_t>(getMagnitude(warmup + 1), kSearDetectorShift);
		}

		c_i = (c_i + 1 < channels) ? c_i + 1 : 0;
		currentSample += 1;
	} while (currentSample < endSample);

	ch[0] = f[0];
	if (channels > 1) {
		ch[1] = f[1];
	}
	state.envIn = envIn;
	state.envOut = envOut;
	state.warmup = warmup;

	// --- Correction. -------------------------------------------------------------------------
	//
	// raw = envIn / envOut, stored /4 so the 0..4x range fits q31. With the chain's 0.375
	// small-signal gain, raw sits near 2.67x at low drive and falls as the clipper adds loudness.
	// When the input has dropped away, HOLD the current gain rather than inventing a target. This
	// matters more than it did before the bite stages: when the input stops, the bias it was carrying
	// is still decaying through the clipper and the DC blocker, so the output rings on while the input
	// is already zero. The ratio then walked the gain all the way to 0x, leaving the next note to fade
	// in from nothing. "Dropped away" = this block's loudest input sample is more than 18 dB under the
	// running mean |sample|, which a continuous signal — release tails included — never does.
	q31_t gain = state.gain;
	q31_t raw = gain;
	if (envOut > 0 && blockPeak > (envIn >> 3)) {
		const int64_t r = (static_cast<int64_t>(envIn) << (31 - kSearGainHeadroomShift)) / envOut;
		raw = static_cast<q31_t>(std::min<int64_t>(r, ONE_Q31));
	}
	// raw * (1 + lift), pinned at ONE_Q31 (4x) by add_saturate. When holding, there is no lift to add.
	const q31_t target = (raw == gain) ? gain : add_saturate(raw, 2 * multiply_32x32_rshift32(raw, lift));

	// NOT YET PROFILED ON HARDWARE: one 64-bit divide per block per voice (a libgcc call — the
	// Cortex-A9 has no integer divide). If Sear ever shows up in a profile, start here.

	// THE GAIN IS ONLY EVER GLIDED, NEVER SNAPPED. An early estimate can be wildly wrong, and
	// gliding by a 128th per sample is what keeps a bad one from reaching the output. Do not
	// "optimise" this into a snap for faster convergence.

	// --- Pass 2: apply, gliding. -------------------------------------------------------------
	currentSample = startSample;
	do {
		gain += (target - gain) >> kSearGainShift;
		*currentSample =
		    lshiftAndSaturateUnknown(multiply_32x32_rshift32(*currentSample, gain), kSearGainHeadroomShift + 1);
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
[[gnu::noinline]] inline void searToneBufferStereo(q31_t* startSample, q31_t* endSample, q31_t tone, q31_t* stateL, q31_t* stateR) {
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

/// Crossfade length when Sear switches on or off: ~5.8 ms at 44.1 kHz (256 frames).
constexpr q31_t kSearFadeStep = ONE_Q31 / 256;

/*
 * Sear for one Sound block: drive, tone and the on/off crossfade. (2026-10-05)
 *
 * WHY THE CROSSFADE. Sear used to be skipped outright whenever the drive was 0. Knob 0 is a true
 * bypass, but knob 1 is already 4x into a clipper (kSearPostMixBoostOctaves) through the mid push
 * and the tone tilt, so sweeping across the bottom of the knob switched between two different
 * waveforms from one sample to the next - a pop, and the reported "popping when moving Sear too
 * quickly" was mostly this. Now the processed signal is faded in and out against the dry one over
 * ~6 ms. Fading out holds the last non-zero drive, so the wet side does not change character while
 * it disappears. Nothing changes at any fixed knob setting: once a fade completes the output is
 * exactly what it was before.
 *
 * AFTER SILENCE there is nothing to click against, so the first block snaps straight to its drive
 * and wet - no fade-in that would soften the attack of a driven note. Sound calls searNoteSilence()
 * from the branch that runs when no voice is sounding.
 *
 * `dry` is scratch space for the crossfade, at least (endSample - startSample) q31.
 */
[[gnu::noinline]] inline void searProcessStereo(q31_t* startSample, q31_t* endSample, q31_t drive, q31_t tone,
                                                SearLevel& state, SearChannel* ch, q31_t* toneL, q31_t* toneR,
                                                q31_t* dry) {
	if (state.snap != 0) {
		state.snap = 0;
		state.wet = (drive > 0) ? ONE_Q31 : 0;
		state.lastDrive = drive;
	}

	const q31_t wetTarget = (drive > 0) ? ONE_Q31 : 0;
	if (wetTarget == 0 && state.wet == 0) {
		// Fully off: the old bypass, unchanged. Rewind the warm-up so turning the knob back up
		// re-measures rather than resuming a stale gain.
		state.warmup = 0;
		state.lastDrive = 0;
		return;
	}

	// Coming on from fully off: start the glide AT the new drive. Gliding up from 0 would sweep through
	// the bottom of the knob on every switch-on, which is a fade the crossfade already provides.
	if (state.wet == 0) {
		state.lastDrive = drive;
	}
	// Going off: keep the last real drive while the wet side fades.
	const q31_t driveTo = (drive > 0) ? drive : state.lastDrive;

	const bool fading = state.wet != wetTarget;
	if (fading) {
		for (q31_t* p = startSample; p < endSample; ++p) {
			dry[p - startSample] = *p;
		}
	}

	searBuffer(startSample, endSample, state.lastDrive, driveTo, state, ch, 2);
	searToneBufferStereo(startSample, endSample, tone, toneL, toneR);
	state.lastDrive = driveTo;

	if (!fading) {
		return;
	}

	q31_t wet = state.wet;
	const q31_t* d = dry;
	for (q31_t* p = startSample; p < endSample; p += 2, d += 2) {
		if (wet < wetTarget) {
			wet = (wet > wetTarget - kSearFadeStep) ? wetTarget : wet + kSearFadeStep;
		}
		else if (wet > wetTarget) {
			wet = (wet < kSearFadeStep) ? 0 : wet - kSearFadeStep;
		}
		// out = dry + (wet - dry) * w, at half scale so the difference cannot overflow.
		for (int32_t c = 0; c < 2; c++) {
			const q31_t diff = (p[c] >> 1) - (d[c] >> 1);
			p[c] = add_saturate(d[c], multiply_32x32_rshift32(diff, wet) << 2);
		}
	}
	state.wet = wet;
	if (wet == 0) {
		// Finished fading out. Next time the knob comes up, measure afresh.
		state.warmup = 0;
		state.lastDrive = 0;
	}
}

/// Called by Sound on any block with no voice sounding. See searProcessStereo.
[[gnu::noinline]] inline void searNoteSilence(SearLevel& state) {
	state.snap = 1;
}

/// Convert an unpatched param's signed q31 (-2^31 .. 2^31-1) to the unsigned 0..ONE_Q31
/// that searToneBuffer expects, with the param's centre landing on tone centre.
/// Matches the codebase idiom — see mod_controllable_audio.cpp, where UNPATCHED_BITCRUSHING
/// and UNPATCHED_SAMPLE_RATE_REDUCTION are both converted by adding 2147483648.
inline q31_t searToneFromUnpatched(int32_t unpatchedValue) {
	return static_cast<q31_t>((static_cast<int64_t>(unpatchedValue) + 2147483648LL) >> 1);
}

} // namespace deluge::dsp
