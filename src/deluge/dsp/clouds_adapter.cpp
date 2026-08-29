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

#include "clouds_adapter.h"

#include "clouds/dsp/frame.h"
#include "clouds/dsp/granular_processor.h"
#include "clouds/resources.h"
#include "stmlib/dsp/dsp.h"
#include "memory/general_memory_allocator.h"
#include "processing/engines/audio_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <new>

namespace {
// Deluge's audio rate (definitions_cxx.hpp) and Clouds' native rate
// (clouds/clouds.cc, and scattered through resonestor.h/reverb.h -- see
// PROVENANCE.md for why that isn't a single constant we could just read).
constexpr float kCloudsSampleRate = 32000.0f;
// The one ratio both resampling stages need. Per Deluge-side sample, the
// Clouds-side (32 kHz) timeline advances by this much -- used directly as the
// phase-accumulator step in both directions, just against an input-driven
// loop in stage 1 and an output-driven loop in stage 3.
constexpr float kStep = kCloudsSampleRate / static_cast<float>(kSampleRate);

/// q31 (-2^31 .. 2^31-1) to upstream's unipolar 0..1.
inline float q31ToUnipolar(q31_t v) {
	return static_cast<float>(v) * (0.5f / 2147483648.0f) + 0.5f;
}

/// q31 to upstream's bipolar -1..1.
inline float q31ToBipolar(q31_t v) {
	return static_cast<float>(v) * (1.0f / 2147483648.0f);
}

/// Upstream reads pitch in semitones and clamps to +/-48 internally; we only
/// offer +/-24, which is the range Clouds' own front panel covers and keeps
/// the granular player's resampling ratio sane.
constexpr float kPitchSemitoneRange = 24.0f;

// Per-mode output trim, wet only.
//
// Upstream's six modes are nowhere near level with each other, which does not
// matter on the module -- Clouds is the whole signal path there and you set the
// output by ear -- but does as an insert you switch between mid-set. Measured on
// this exact vendored engine with the committed defaults, 20 s of chord-plus-noise
// at -20 dBFS, RMS over the last 0.4 s, full wet:
//
//     Granular    0.71x dry   -3.0 dB
//     Stretch     0.37x       -8.5
//     Delay       0.38x       -8.5
//     Spectral    0.97x       -0.2
//     Oliverb     2.22x       +6.9
//     Resonestor  1.06x       +0.5
//
// Granular is the one people reach for first, so it is the reference. Resonestor
// sits 3.5 dB above it, which is what reads as "the resonator is much louder than
// everything else" -- and it is not the distortion, which measures identically at
// 0 and 1. Trim it back to Granular. Nothing is boosted: pushing the quiet modes up
// would cost headroom and noise for no reason, and their softness is upstream's
// voice, not a fault.
//
// Oliverb measures a further 6.4 dB above Resonestor and wants the same treatment,
// left at 1.0 here pending a listen rather than assumed.
// Everything leaving the engine goes through stmlib's SoftConvert():
//
//     SoftConvert(x) = Clip16(SoftLimit(x * 0.5f) * 32768.0f)
//
// -- a 0.5 gain before the limiter, which in the linear region is simply half.
// Upstream's dry is crossfaded INSIDE the engine, so it takes that halving too.
// Ours is read straight off the Deluge's buffer and never sees it, so without this
// the dry we mix back sits 6 dB above the dry every other mode produces. Measured
// at Blend 0: the five engine-crossfaded modes return 0.3531x dry, this path
// returned 0.7071x -- which is what "Resonestor is loud even at zero Blend" was.
constexpr float kEngineDryScale = 0.5f;

constexpr float kModeOutputTrim[] = {
    1.0f,   // OFF -- never rendered
    1.0f,   // Granular, the reference
    1.0f,   // Stretch
    1.0f,   // Delay
    1.0f,   // Spectral
    1.0f,   // Oliverb -- measures +6.4 dB on Granular; candidate, not yet applied
    0.668f, // Resonestor: -3.5 dB, levelling it with Granular
};
static_assert(static_cast<int32_t>(std::size(kModeOutputTrim)) == kNumCloudsModes,
              "CloudsMode and the output-trim table have drifted apart");

constexpr clouds::PlaybackMode kPlaybackModeFor[] = {
    clouds::PLAYBACK_MODE_GRANULAR, // CloudsMode::OFF -- never used, see setMode()
    clouds::PLAYBACK_MODE_GRANULAR, clouds::PLAYBACK_MODE_STRETCH,    clouds::PLAYBACK_MODE_LOOPING_DELAY,
    clouds::PLAYBACK_MODE_SPECTRAL, clouds::PLAYBACK_MODE_OLIVERB,    clouds::PLAYBACK_MODE_RESONESTOR,
};
static_assert(static_cast<int32_t>(std::size(kPlaybackModeFor)) == kNumCloudsModes,
              "CloudsMode and the upstream PlaybackMode mapping have drifted apart");
} // namespace

void CloudsBuffer::steal(char const* errorCode) {
	owner_->bufferStolen();
}

CloudsAdapter* CloudsAdapter::firstAdapter_ = nullptr;

void CloudsAdapter::tickAllPrepares() {
	for (CloudsAdapter* a = firstAdapter_; a != nullptr; a = a->nextAdapter_) {
		// needsInit_ means the next render will re-Init and prepare for itself, and
		// heavyPreparePending_ means a mode change already has one queued. Preparing
		// under either would be preparing an engine that is about to be rebuilt.
		if (a->processor_ == nullptr || a->buffer_ == nullptr || a->needsInit_ || a->heavyPreparePending_) {
			continue;
		}
		// Only three modes have anything periodic to do. Prepare()'s tail is:
		//
		//     if (SPECTRAL)                 phase_vocoder_.Buffer();
		//     else if (STRETCH || OLIVERB)  LoadCorrelator(); EvaluateSomeCandidates();
		//
		// For Granular, Delay and Resonestor there is no else branch -- calling this
		// is a measured 0.03 us of doing nothing. Skip them outright rather than
		// waking the engine hundreds of times a second to find that out, which is
		// load the Deluge did not carry before this task existed.
		if (a->mode_ != CloudsMode::SPECTRAL && a->mode_ != CloudsMode::STRETCH
		    && a->mode_ != CloudsMode::OLIVERB) {
			continue;
		}
		a->processor_->Prepare();
	}
}

CloudsAdapter::CloudsAdapter() {
	nextAdapter_ = firstAdapter_;
	firstAdapter_ = this;

	// The processor object itself is small (it holds pointers into the two
	// working buffers, not the buffers); the 180 KB lives in CloudsBuffer and
	// is acquired lazily by acquireBuffer().
	void* memory = GeneralMemoryAllocator::get().allocLowSpeed(sizeof(clouds::GranularProcessor));
	if (memory != nullptr) {
		// ZERO IT FIRST. Upstream declares its GranularProcessor as a global
		// in clouds.cc, so on the module it lives in .bss and every member
		// starts at zero. It quietly depends on that: `silence_` is assigned
		// by neither the (empty) constructor nor Init(), and Process() opens
		// with
		//
		//     if (silence_ || reset_buffers_ || previous_playback_mode_ != ...)
		//         fill(output, 0); return;
		//
		// so a garbage-nonzero silence_ means the engine outputs digital
		// silence forever, with no other symptom. We placement-new into
		// allocator memory that is not cleared, so .bss has to be reproduced
		// by hand. parameters_.trigger/.gate/.granular.reverse are in the same
		// position -- never assigned by the DSP, supplied by upstream's
		// ui.cc, which is not vendored -- and this zeroing is what defines
		// them too.
		std::memset(memory, 0, sizeof(clouds::GranularProcessor));
		processor_ = new (memory) clouds::GranularProcessor();
	}
}

CloudsAdapter::~CloudsAdapter() {
	for (CloudsAdapter** link = &firstAdapter_; *link != nullptr; link = &(*link)->nextAdapter_) {
		if (*link == this) {
			*link = nextAdapter_;
			break;
		}
	}
	if (buffer_ != nullptr) {
		buffer_->~CloudsBuffer();
		delugeDealloc(buffer_);
		buffer_ = nullptr;
	}
	if (processor_ != nullptr) {
		processor_->~GranularProcessor();
		delugeDealloc(processor_);
		processor_ = nullptr;
	}
}

bool CloudsAdapter::acquireBuffer() {
	if (processor_ == nullptr) {
		return false;
	}
	if (buffer_ == nullptr) {
		void* memory = GeneralMemoryAllocator::get().allocStealable(sizeof(CloudsBuffer));
		if (memory == nullptr) {
			return false;
		}
		buffer_ = new (memory) CloudsBuffer(this);
		// Fresh memory is not zeroed by the allocator and upstream reads its
		// audio buffer before it has written a full pass over it. Without
		// this the first second or so after switching Clouds on is whatever
		// the previous owner of that SDRAM left behind, at full volume.
		std::memset(buffer_->large(), 0, CloudsBuffer::kLargeBufferBytes);
		std::memset(buffer_->small(), 0, CloudsBuffer::kSmallBufferBytes);
		needsInit_ = true;
	}
	else {
		buffer_->inUse = true;
	}
	return true;
}

void CloudsAdapter::releaseBuffer() {
	if (buffer_ != nullptr) {
		buffer_->inUse = false;
	}
}

void CloudsAdapter::bufferStolen() {
	buffer_ = nullptr;
	needsInit_ = true;
}

void CloudsAdapter::resetResamplerState() {
	downsamplePhase_ = 0.0f;
	upsamplePhase_ = 0.0f;
	downWrite_ = downRead_ = downCount_ = 0;
	upWrite_ = upRead_ = upCount_ = 0;
	prevInL_ = prevInR_ = 0.0f;
	prevOutL_ = prevOutR_ = 0.0f;
	curOutL_ = curOutR_ = 0.0f;
}

void CloudsAdapter::setMode(CloudsMode mode) {
	if (mode == mode_ || mode == CloudsMode::OFF) {
		return;
	}
	mode_ = mode;
	fadeInRemaining_ = kFadeInSamples; // hide the reallocation discontinuity
	heavyPreparePending_ = true;
	if (processor_ != nullptr) {
		// The meaning of dry_wet changes with the mode, so it has to be rewritten
		// here and not only when Blend next moves.
		processor_->mutable_parameters()->dry_wet = (mode_ == CloudsMode::RESONESTOR) ? 0.0f : blend_;
		// Upstream reallocates its internal players out of the working buffer
		// when the playback mode changes, so this is only safe once Init has
		// run against a buffer we actually hold.
		if (!needsInit_ && buffer_ != nullptr) {
			processor_->set_playback_mode(kPlaybackModeFor[util::to_underlying(mode_)]);
		}
	}
}

void CloudsAdapter::setPosition(q31_t v) {
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->position = q31ToUnipolar(v);
	}
}

void CloudsAdapter::setSize(q31_t v) {
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->size = q31ToUnipolar(v);
	}
}

void CloudsAdapter::setPitch(q31_t v) {
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->pitch = q31ToBipolar(v) * kPitchSemitoneRange;
	}
}

void CloudsAdapter::setDensity(q31_t v) {
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->density = q31ToUnipolar(v);
	}
}

void CloudsAdapter::setTexture(q31_t v) {
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->texture = q31ToUnipolar(v);
	}
}

void CloudsAdapter::setBlend(q31_t v) {
	blend_ = q31ToUnipolar(v);
	if (processor_ != nullptr) {
		// Resonestor reads this as distortion, not dry/wet -- see process(). It gets a
		// clean 0 there and Blend does its crossfade downstream instead, so the knob
		// means the same thing in all six modes.
		processor_->mutable_parameters()->dry_wet = (mode_ == CloudsMode::RESONESTOR) ? 0.0f : blend_;
	}
}

void CloudsAdapter::setSpread(q31_t v) {
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->stereo_spread = q31ToUnipolar(v);
	}
}

void CloudsAdapter::setFeedback(q31_t v) {
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->feedback = q31ToUnipolar(v);
	}
}

void CloudsAdapter::setReverb(q31_t v) {
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->reverb = q31ToUnipolar(v);
	}
}

void CloudsAdapter::setFreeze(bool frozen) {
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->freeze = frozen;
	}
}

void CloudsAdapter::prepareOffAudioThread() {
	if (processor_ == nullptr) {
		return;
	}

	// Raise BEFORE anything is touched, and keep it up until the engine is
	// coherent again. Everything below either reallocates the engine's internals
	// or depends on that having finished; a render landing in the middle reads
	// freed pointers. Single core, called from the main loop: a render can only
	// preempt us, so a plain flag closes the window that disabling interrupts used
	// to close by force -- and without stalling the audio thread for the length of
	// a Prepare().
	rebuilding_ = true;
	__asm volatile("" ::: "memory");

	if (!acquireBuffer()) {
		rebuilding_ = false;
		return; // No memory; process() will pass audio through dry.
	}
	if (needsInit_) {
		processor_->Init(buffer_->large(), CloudsBuffer::kLargeBufferBytes, buffer_->small(),
		                 CloudsBuffer::kSmallBufferBytes);
		processor_->set_silence(false);
		processor_->set_bypass(false);
		processor_->mutable_parameters()->dry_wet = (mode_ == CloudsMode::RESONESTOR) ? 0.0f : 1.0f;
		resetResamplerState();
		needsInit_ = false;
	}
	processor_->set_playback_mode(kPlaybackModeFor[util::to_underlying(mode_)]);

	// The expensive one. Runs here so the audio thread never sees it.
	processor_->Prepare();

	// Moving the work off the audio thread is only half of it. The caller holds a
	// critical section across this whole sequence, so the audio thread is *stalled*
	// for its duration rather than overrunning inside it -- and a stall of this
	// length reads to the engine as a missed deadline just the same. It then culls
	// every voice, and the synth stays silent until the transport is stopped and
	// restarted to retrigger it, which is exactly the symptom this was supposed to
	// cure. It also makes Resonestor look broken a second time over: a culled synth
	// feeds it nothing, and a resonator with no input has nothing to resonate.
	//
	// Suppress culling for the routine that resumes after the stall. AudioEngine
	// clears the flag itself at the end of that routine, so this does not leak.
	//
	// Note that clearing heavyPreparePending_ below is what removes the in-render
	// Prepare() -- and with it the only other place bypassCulling was ever set. The
	// two have to move together; setting one without the other is what left this
	// broken after the fixes were restored.
	AudioEngine::bypassCulling = true;

	heavyPreparePending_ = false;
	fadeInRemaining_ = kFadeInSamples;

	__asm volatile("" ::: "memory");
	rebuilding_ = false;
}

void CloudsAdapter::process(std::span<StereoSample> buffer) {
	// Both are required *here* specifically: isValid() only says the adapter
	// is usable at all, and the working buffer is per-render state that the
	// caller acquires for us. Either being absent means pass the audio
	// through dry rather than render.
	if (processor_ == nullptr || buffer_ == nullptr) {
		return; // Dry passthrough: caller's buffer is left exactly as it came in.
	}

	// A mode change is rebuilding the engine on the main loop. Dry until it is done
	// -- 20 ms or so, and the fade-in on the far side hides the seam.
	if (rebuilding_) {
		return;
	}

	if (needsInit_) {
		processor_->Init(buffer_->large(), CloudsBuffer::kLargeBufferBytes, buffer_->small(),
		                 CloudsBuffer::kSmallBufferBytes);
		// Init() sets bypass_ but NOT silence_ -- see the constructor. Saying
		// it explicitly here means a future re-vendor that reorders Init()
		// cannot silently reintroduce the silent-engine bug, and it documents
		// at the call site that we know the difference between the two.
		processor_->set_silence(false);
		processor_->set_bypass(false);
		processor_->set_playback_mode(kPlaybackModeFor[util::to_underlying(mode_)]);
		resetResamplerState();
		fadeInRemaining_ = kFadeInSamples;
		heavyPreparePending_ = true;
		needsInit_ = false;
	}

	auto* downRing = reinterpret_cast<clouds::FloatFrame*>(downRing_);
	auto* upRing = reinterpret_cast<clouds::FloatFrame*>(upRing_);

	// --- Stage 1: resample input from the Deluge rate down into the 32 kHz
	// ring. Input-driven: one accumulator step per incoming sample.
	// downsamplePhase_ crosses 1.0 on ~kStep of every input sample (kStep is
	// < 1, since the output side runs slower), which is exactly the emission
	// rate the conversion needs. `t` places the emitted frame at its true
	// fractional position between the previous and current input sample.
	//
	// See PROVENANCE.md -- this is the one piece of this port that is NOT
	// upstream-pristine, because it isn't upstream code at all; it exists
	// only to feed upstream's untouched DSP the sample rate it was written
	// for, and it is linear interpolation, not the polyphase resampler
	// dsp/interpolation/ provides elsewhere in this codebase.
	for (StereoSample& sample : buffer) {
		float inL = static_cast<float>(sample.l) * (1.0f / 2147483648.0f);
		float inR = static_cast<float>(sample.r) * (1.0f / 2147483648.0f);
		// q31 in, so these are finite by construction -- but the engine's own
		// feedback path reads its previous output, so clamp anyway rather than
		// let one bad block become a permanent one.
		inL = std::clamp(inL, -1.0f, 1.0f);
		inR = std::clamp(inR, -1.0f, 1.0f);
		downsamplePhase_ += kStep;
		if (downsamplePhase_ >= 1.0f) {
			downsamplePhase_ -= 1.0f;
			// downsamplePhase_ is now how far *past* the crossing we are, in
			// units of kStep; 1 - that is how far *into* [prevIn, in] the
			// crossing landed.
			float t = 1.0f - downsamplePhase_;
			if (downCount_ < kRingFrames) {
				downRing[downWrite_].l = prevInL_ + (inL - prevInL_) * t;
				downRing[downWrite_].r = prevInR_ + (inR - prevInR_) * t;
				downWrite_ = (downWrite_ + 1) % kRingFrames;
				downCount_++;
			}
		}
		prevInL_ = inL;
		prevInR_ = inR;
	}

	// --- Stage 2: run whole native blocks through the vendored processor. ---
	//
	// Prepare() is cheap on most blocks, but on a mode change or a re-Init it
	// takes its reset path: fourteen Init/Allocate calls -- diffuser, reverb,
	// correlator, pitch shifter, and then the players or the phase vocoder or
	// the resonator. Upstream runs Prepare() from its main loop precisely
	// because of this; we have no choice but to run it here, inside the audio
	// render, so tell the engine not to cull voices over the resulting spike.
	//
	// Without this, changing mode blew the audio deadline, the engine culled
	// every voice, and the synth stayed silent until playback was stopped and
	// restarted to re-trigger it. Which also made Resonestor look broken: a
	// culled synth puts nothing into the send bus, and a resonator with no
	// input has nothing to resonate.
	if (heavyPreparePending_) {
		AudioEngine::bypassCulling = true;
		heavyPreparePending_ = false;
		processor_->Prepare();
	}
	else {
		// Nothing. The periodic Prepare() now runs from tickAllPrepares() on a
		// non-audio task, because for Spectral, Stretch and Oliverb it is a
		// several-hundred-microsecond spike that no render block can absorb. See
		// the measurements on tickAllPrepares().
	}
	while (downCount_ >= clouds::kMaxBlockSize) {
		clouds::ShortFrame shortIn[clouds::kMaxBlockSize];
		clouds::ShortFrame shortOut[clouds::kMaxBlockSize];
		for (size_t i = 0; i < clouds::kMaxBlockSize; ++i) {
			clouds::FloatFrame f = downRing[downRead_];
			downRead_ = (downRead_ + 1) % kRingFrames;
			downCount_--;
			shortIn[i].l = static_cast<int16_t>(std::clamp(f.l, -1.0f, 1.0f) * 32767.0f);
			shortIn[i].r = static_cast<int16_t>(std::clamp(f.r, -1.0f, 1.0f) * 32767.0f);
		}

		processor_->Process(shortIn, shortOut, clouds::kMaxBlockSize);

		for (size_t i = 0; i < clouds::kMaxBlockSize; ++i) {
			if (upCount_ < kRingFrames) {
				upRing[upWrite_].l = static_cast<float>(shortOut[i].l) / 32768.0f;
				upRing[upWrite_].r = static_cast<float>(shortOut[i].r) / 32768.0f;
				upWrite_ = (upWrite_ + 1) % kRingFrames;
				upCount_++;
			}
		}
	}

	// --- Stage 3: resample the 32 kHz result back up, in place. -------------
	// Output-driven, mirror image of stage 1: one accumulator step per
	// outgoing sample, consuming a new 32 kHz-domain frame from the ring each
	// time accumulated phase crosses 1.0 (which happens on ~kStep of every
	// output sample -- fewer than one consume per output on average, correct
	// for going from the slower rate to the faster one).
	// Resonestor is the one mode upstream never crossfades. Do it here, with
	// upstream's own equal-power curve so the taper matches the other five. No
	// post gain: the 1.2f upstream applies lives inside the block it skips for this
	// mode, and adding it would make the loudest mode louder still.
	const float outputTrim = kModeOutputTrim[util::to_underlying(mode_)];
	const bool crossfadeHere = (mode_ == CloudsMode::RESONESTOR);
	const float wetGain = crossfadeHere ? stmlib::Interpolate(clouds::lut_xfade_in, blend_, 16.0f) : 1.0f;
	const float dryGain = crossfadeHere ? stmlib::Interpolate(clouds::lut_xfade_out, blend_, 16.0f) : 0.0f;

	for (StereoSample& sample : buffer) {
		// Read before stage 3 overwrites it: stages 1 and 2 took their copy into the
		// rings, so this still holds the block's dry input.
		const float dryL = crossfadeHere ? static_cast<float>(sample.l) / 2147483648.0f * kEngineDryScale : 0.0f;
		const float dryR = crossfadeHere ? static_cast<float>(sample.r) / 2147483648.0f * kEngineDryScale : 0.0f;

		upsamplePhase_ += kStep;
		while (upsamplePhase_ >= 1.0f) {
			upsamplePhase_ -= 1.0f;
			prevOutL_ = curOutL_;
			prevOutR_ = curOutR_;
			if (upCount_ > 0) {
				curOutL_ = upRing[upRead_].l;
				curOutR_ = upRing[upRead_].r;
				upRead_ = (upRead_ + 1) % kRingFrames;
				upCount_--;
			}
			// Else: ring ran dry (only expected while the first block is
			// still filling) -- hold curOut rather than read garbage.
		}
		float outL = prevOutL_ + (curOutL_ - prevOutL_) * upsamplePhase_;
		float outR = prevOutR_ + (curOutR_ - prevOutR_) * upsamplePhase_;
		// Poison guard. If the engine ever emits a non-finite sample -- and
		// with feedback paths, a resonator that can be driven to feedback 86,
		// and float state we do not fully control, it can -- then NaN would
		// otherwise be latched into prevOut/curOut and every sample from here
		// on would be NaN. std::clamp does not help: all of NaN's comparisons
		// are false, so it returns NaN unchanged, and casting that to q31_t is
		// undefined behaviour, in practice INT_MIN. That is a full-scale click
		// followed by permanent silence, which is exactly the reported
		// "drops out and doesn't come back".
		//
		// So: check, and if it happens, mute this block and reset the
		// resampler so the next one starts clean.
		if (!std::isfinite(outL) || !std::isfinite(outR)) {
			// Resetting our own state is not enough. The engine keeps its
			// previous output in fb_ and feeds it back, so once NaN is in
			// there it stays, and every subsequent block is NaN too. The only
			// way back is to re-Init the engine, which the next process()
			// will do because needsInit_ is set here. Expensive, but this
			// should never happen, and permanent silence is worse.
			resetResamplerState();
			needsInit_ = true;
			heavyPreparePending_ = true; // don't let the re-Init cull voices
			fadeInRemaining_ = kFadeInSamples;
			outL = 0.0f;
			outR = 0.0f;
		}

		if (fadeInRemaining_ > 0) {
			// Linear ramp 0 -> 1 across kFadeInSamples. Only ever runs for 20 ms
			// after a mode change or a re-Init, so the cost is irrelevant.
			float gain = 1.0f - (static_cast<float>(fadeInRemaining_) / static_cast<float>(kFadeInSamples));
			outL *= gain;
			outR *= gain;
			--fadeInRemaining_;
		}
		// Wet only, and before the dry is mixed back in: the trim exists to level the
		// modes against each other, not to turn the whole insert down.
		outL *= outputTrim;
		outR *= outputTrim;

		if (crossfadeHere) {
			outL = dryL * dryGain + outL * wetGain;
			outR = dryR * dryGain + outR * wetGain;
		}

		sample.l = static_cast<q31_t>(std::clamp(outL, -1.0f, 1.0f) * 2147483647.0f);
		sample.r = static_cast<q31_t>(std::clamp(outR, -1.0f, 1.0f) * 2147483647.0f);
	}

	// NOTE: the buffer is deliberately NOT released here.
	//
	// It used to be, on the theory that unpinning between blocks made the
	// memory "genuinely" stealable rather than merely allocated from the
	// stealable pool. That was wrong, and measurably so. Losing the buffer
	// forces a re-Init, Prepare() then takes its reset_buffers_ path,
	// reallocates the grain players and clears the recording buffer -- so
	// every steal throws away the audio Clouds exists to granulate. Measured
	// natively against the real engine: re-Init on every block drops wet
	// output from 0.30 RMS to 0.069, a 4x loss, while the dry path is
	// untouched and hides it.
	//
	// GrainBuffer stays pinned for as long as grain FX is on for exactly this
	// reason. Clouds now does the same: pinned for the life of the adapter,
	// and the adapter is destroyed the moment the mode goes OFF, which is
	// what actually returns the 180 KB.
}
