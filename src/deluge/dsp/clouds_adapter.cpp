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
#include "memory/general_memory_allocator.h"

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

CloudsAdapter::CloudsAdapter() {
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
	if (processor_ != nullptr) {
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
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->dry_wet = q31ToUnipolar(v);
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

void CloudsAdapter::process(std::span<StereoSample> buffer) {
	// Both are required *here* specifically: isValid() only says the adapter
	// is usable at all, and the working buffer is per-render state that the
	// caller acquires for us. Either being absent means pass the audio
	// through dry rather than render.
	if (processor_ == nullptr || buffer_ == nullptr) {
		return; // Dry passthrough: caller's buffer is left exactly as it came in.
	}

#if CLOUDS_DIAGNOSTIC_BUILD
	// ======================= TEMPORARY - REVERT ME =======================
	// Four questions, one flash. The mode selector is repurposed as a probe
	// because reasoning from source has now been wrong four times running and
	// each wrong guess costs a build-and-flash cycle.
	//
	//   Granular  -> untouched, the real signal path
	//   Stretch   -> DIAG A: emit a 440 Hz tone, engine and input ignored.
	//                Hearing it proves processClouds() runs and that whatever
	//                this function writes actually reaches the output.
	//   Delay     -> DIAG B: input through the resampler ONLY, engine skipped.
	//                Hearing clean audio clears the resampler; hearing silence
	//                or mush convicts it.
	//   Spectral  -> DIAG C: the granular engine with dry_wet and density
	//                forced in code, ignoring the menu entirely. Hearing
	//                grains convicts the parameter path.
	if (mode_ == CloudsMode::STRETCH) {
		constexpr float kTwoPi = 6.28318530718f;
		for (StereoSample& sample : buffer) {
			float v = 0.25f * std::sin(diagPhase_);
			diagPhase_ += kTwoPi * 440.0f / static_cast<float>(kSampleRate);
			if (diagPhase_ > kTwoPi) {
				diagPhase_ -= kTwoPi;
			}
			sample.l = sample.r = static_cast<q31_t>(v * 2147483647.0f);
		}
		return;
	}
	if (CLOUDS_DIAG_FORCED_PARAMS(mode_)) {
		clouds::Parameters* q = processor_->mutable_parameters();
		q->position = 0.5f;
		q->size = 0.5f;
		q->pitch = 0.0f;
		q->density = 0.9f;
		q->texture = 0.5f;
		q->dry_wet = 1.0f;
		q->stereo_spread = 0.0f;
		q->feedback = 0.0f;
		q->reverb = 0.0f;
		q->freeze = false;
	}
	const bool diagBypassEngine = (mode_ == CloudsMode::DELAY);
	// =====================================================================
#else
	constexpr bool diagBypassEngine = false;
#endif

	if (needsInit_) {
		processor_->Init(buffer_->large(), CloudsBuffer::kLargeBufferBytes, buffer_->small(),
		                 CloudsBuffer::kSmallBufferBytes);
		// Init() sets bypass_ but NOT silence_ -- see the constructor. Saying
		// it explicitly here means a future re-vendor that reorders Init()
		// cannot silently reintroduce the silent-engine bug, and it documents
		// at the call site that we know the difference between the two.
		processor_->set_silence(false);
		processor_->set_bypass(false);
		processor_->set_playback_mode(CLOUDS_DIAG_FORCED_PARAMS(mode_)
		                                  ? clouds::PLAYBACK_MODE_GRANULAR
		                                  : kPlaybackModeFor[util::to_underlying(mode_)]);
		resetResamplerState();
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
	processor_->Prepare();
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

		if (!diagBypassEngine) {
			processor_->Process(shortIn, shortOut, clouds::kMaxBlockSize);
		}
		else {
			std::memcpy(shortOut, shortIn, sizeof(shortIn));
		}

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
	for (StereoSample& sample : buffer) {
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
