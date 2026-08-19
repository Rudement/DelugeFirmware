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

// ============================================================================
// Mutable Instruments Clouds (Parasites) <-> Deluge adapter.
//
// THIS IS THE ONLY FILE THAT KNOWS ABOUT BOTH WORLDS, same rule as
// plaits_adapter.h: everything under dsp/clouds/ is upstream and knows
// nothing about the Deluge; anything that wants Clouds talks to it only
// through here. Do not let Deluge types leak into dsp/clouds/, and do not
// let clouds:: types leak past this header -- note that clouds::Parameters
// is NOT exposed here any more, only plain q31/float setters, so callers
// cannot accidentally depend on upstream's layout.
//
// Clouds is an effect, not an oscillator, so unlike PlaitsVoice this hangs
// off GlobalEffectable's FX chain rather than Source/Voice. See
// GlobalEffectable::processClouds() for the call site and
// dsp/clouds/PROVENANCE.md for provenance and licensing.
//
// Memory: the two working buffers upstream's clouds.cc declares (~180 KB
// combined) come from the stealable SDRAM pool, NOT from the tight internal
// heap and no longer from plain new[]. They follow exactly the pattern
// dsp/granular/GrainBuffer already uses: allocStealable, a Stealable
// subclass that can hand the memory back under pressure, and a re-Init on
// the next render after a steal. Losing the buffers is audible (the
// recording is gone) but never fatal.
// ============================================================================

#pragma once

#include "definitions_cxx.hpp"
#include "dsp/stereo_sample.h"
#include "memory/stealable.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace clouds {
class GranularProcessor;
} // namespace clouds

class CloudsAdapter;

/// The stealable SDRAM block backing one CloudsAdapter.
///
/// Upstream asks for two separate buffers; we take them as one allocation
/// and hand out two spans of it, so there is a single Stealable to track and
/// the two can never be half-stolen (which would leave the processor Init'd
/// against a dangling pointer).
class CloudsBuffer : public Stealable {
public:
	CloudsBuffer() = delete;
	CloudsBuffer(CloudsBuffer& other) = delete;
	CloudsBuffer(const CloudsBuffer& other) = delete;
	explicit CloudsBuffer(CloudsAdapter* owner) : owner_(owner) {}

	// Matches clouds/clouds.cc's own block_mem/block_ccm sizes exactly, so
	// this is the same working set the module itself runs with. On the module
	// the small one lives in fast core-coupled RAM; here both are SDRAM,
	// which is the plentiful pool on this hardware. See PROVENANCE.md.
	static constexpr size_t kLargeBufferBytes = 118784;
	static constexpr size_t kSmallBufferBytes = 65536 - 128;

	bool mayBeStolen(void* thingNotToStealFrom) override {
		if (thingNotToStealFrom != this) {
			return !inUse;
		}
		return false;
	}
	void steal(char const* errorCode) override;

	/// Same queue GrainBuffer uses: these are large and slow to reallocate,
	/// so they should be among the last things given up.
	StealableQueue getAppropriateQueue() override {
		return StealableQueue::CURRENT_SONG_SAMPLE_DATA_REPITCHED_CACHE;
	}

	uint8_t* large() { return storage_; }
	uint8_t* small() { return storage_ + kLargeBufferBytes; }

	bool inUse{true};

private:
	CloudsAdapter* owner_;
	uint8_t storage_[kLargeBufferBytes + kSmallBufferBytes];
};

/// One Clouds granular processor instance, wrapped for the Deluge FX chain.
class CloudsAdapter {
public:
	CloudsAdapter();
	~CloudsAdapter();

	CloudsAdapter(const CloudsAdapter&) = delete;
	CloudsAdapter& operator=(const CloudsAdapter&) = delete;

	/// False only if the upstream processor object failed to allocate, i.e.
	/// this adapter can never work and should be torn down.
	///
	/// Deliberately does NOT test buffer_. The working buffer is acquired at
	/// the top of each render and released at the end of it, so it is null
	/// for most of an adapter's life -- including immediately after
	/// construction, which is exactly when setCloudsMode() checks this. An
	/// earlier version tested it here and, because the constructor no longer
	/// allocates the buffer, made every single mode change fail.
	[[nodiscard]] bool isValid() const { return processor_ != nullptr; }

	/// Reclaim the buffer if it is still ours, or take a fresh one if it was
	/// stolen. Called at the top of every render while Clouds is switched on.
	/// Returns false if there was no memory to be had, in which case the
	/// caller must skip processing this block.
	bool acquireBuffer();

	/// Unpin the buffer so the allocator may steal it. Called only when Clouds
	/// is being switched off -- NOT per block. A steal costs the entire
	/// recording, so the buffer stays pinned while the effect is live, the
	/// same bargain GrainBuffer makes.
	void releaseBuffer();

	/// Called by CloudsBuffer::steal(). Drops our pointer without freeing:
	/// the allocator owns the memory again by then.
	void bufferStolen();

	// --- Parameters ---------------------------------------------------------
	// All Deluge-side units. q31 params arrive as the firmware's usual signed
	// 32-bit -2^31..2^31-1 and are mapped to upstream's 0..1 floats here, in
	// the one file allowed to know both conventions.

	/// `mode` is a CloudsMode; OFF is handled by the caller not calling us.
	void setMode(CloudsMode mode);
	[[nodiscard]] CloudsMode mode() const { return mode_; }

	void setPosition(q31_t v);
	void setSize(q31_t v);
	/// Bipolar: full negative is -2 octaves, full positive +2 octaves.
	void setPitch(q31_t v);
	void setDensity(q31_t v);
	void setTexture(q31_t v);
	void setBlend(q31_t v);
	void setSpread(q31_t v);
	void setFeedback(q31_t v);
	void setReverb(q31_t v);
	void setFreeze(bool frozen);

	/// Do everything expensive that a mode change implies -- acquire the
	/// buffer, Init the processor if needed, set the playback mode and run the
	/// first Prepare() -- from a non-audio thread.
	///
	/// Upstream drives Prepare() from its main loop for exactly this reason:
	/// on a mode change it reallocates the grain players, the diffuser, the
	/// reverb, the correlator and the pitch shifter, fourteen Init/Allocate
	/// calls in all. Doing that inside an audio block overruns the render
	/// deadline. Once this has run, the Prepare() in process() finds
	/// reset_buffers_ clear and previous_playback_mode_ already matching, so
	/// it takes its cheap path.
	///
	/// The CALLER must hold a critical section: this reallocates buffers that
	/// process() reads.
	void prepareOffAudioThread();

	/// Render in place over `buffer`, at the Deluge's kSampleRate. Internally
	/// resamples down to Clouds' native 32 kHz, runs whole
	/// clouds::kMaxBlockSize blocks through the untouched upstream processor,
	/// and resamples back up. No-op if !isValid().
	void process(std::span<StereoSample> buffer);

private:
	void resetResamplerState();

	clouds::GranularProcessor* processor_ = nullptr;
	CloudsBuffer* buffer_ = nullptr;

	CloudsMode mode_ = CloudsMode::GRANULAR;

	/// Samples of fade-in remaining after a mode change or a re-Init.
	///
	/// Changing playback mode makes Prepare() take its reset path: it
	/// reallocates the grain players and re-Inits the audio buffers, so the
	/// engine's output jumps discontinuously. Straight into the mix that is an
	/// audible click. Ramping the return in over a few milliseconds costs
	/// nothing and removes it.
	int32_t fadeInRemaining_ = 0;

	/// Samples of audio since Prepare() was last called, so its rate can be
	/// bounded independently of the render block size.
	int32_t samplesSincePrepare_ = 0;
	/// Call Prepare() at most this often. The Deluge's render block varies from
	/// 4 to 128 samples, so calling Prepare() once per block means anywhere
	/// from 344 Hz to 11 kHz. Upstream calls it from its main loop, on the
	/// order of 1 kHz.
	///
	/// 32 samples is 1378 Hz at 44.1 kHz, i.e. still upstream's rate -- which is
	/// fine on upstream's main loop and not fine here, because we have to run
	/// Prepare() inside the audio render where a spike threatens the deadline.
	/// Measured natively, per 32-frame block:
	///
	///     mode        Process()   Prepare()
	///     Granular      5.63 us     0.03 us
	///     Stretch       6.90 us     5.59 us
	///     Delay         8.18 us     0.03 us
	///     Spectral      3.51 us    12.48 us
	///     Oliverb      11.21 us     3.68 us
	///     Resonestor   12.09 us     0.04 us
	///
	/// Spectral is the cheapest mode to run and the most expensive to prepare, by
	/// a factor of three and a half over its own Process(). That is the whole
	/// reason it fell over on hardware while Granular was fine.
	///
	/// The work Prepare() does is incremental by design, so calling it less often
	/// costs convergence speed rather than correctness -- and measurement says
	/// there is a great deal of headroom. Output RMS against a 220 Hz tone,
	/// varying only the Prepare rate:
	///
	///     rate      1000 Hz   250 Hz   125 Hz    31 Hz
	///     Spectral   0.3274   0.3282   0.3279   0.3273
	///     Stretch    0.1400   0.1401   0.1398   0.0898
	///
	/// Spectral is indifferent all the way down. Stretch is flat to 125 Hz and
	/// only starts losing overlap material below that. 256 samples is 172 Hz at
	/// 44.1 kHz: an eightfold cut in prepare cost, inside the range where both
	/// measured identical to running it every block.
	static constexpr int32_t kSamplesBetweenPrepares = 256;

	/// Set when the next Prepare() will take its expensive reallocation path,
	/// so process() can tell the audio engine not to cull voices over it.
	bool heavyPreparePending_ = true;
	static constexpr int32_t kFadeInSamples = kSampleRate / 50; // 20 ms

	/// Set when the buffer is (re)acquired, so the next process() re-Inits
	/// upstream against the new memory before touching it.
	bool needsInit_ = true;

	/// Resampler state. Linear interpolation only -- functionally correct,
	/// not the quality dsp/interpolation/ provides elsewhere. See
	/// PROVENANCE.md point 3 under "Not done in this pass".
	///
	/// These were file-scope statics while this was a single-instance
	/// scaffold. They are per-instance now, and have to be: every
	/// GlobalEffectable that switches Clouds on gets its own adapter, and
	/// shared resampler phase between two of them would cross-feed one
	/// track's audio into another's ring.
	float downsamplePhase_ = 0.0f;
	float upsamplePhase_ = 0.0f;

	/// Interleaved stereo frames, kRingFrames pairs each. Held as plain
	/// floats rather than clouds::FloatFrame so this header stays free of
	/// upstream types; the .cpp casts them back. Sized well clear of one
	/// native Clouds block (32 frames) so any Deluge block size the firmware
	/// actually uses fits without the ring wrapping mid-block.
	static constexpr size_t kRingFrames = 256;
	float downRing_[kRingFrames * 2]{};
	float upRing_[kRingFrames * 2]{};
	uint16_t downWrite_ = 0, downRead_ = 0, downCount_ = 0;
	uint16_t upWrite_ = 0, upRead_ = 0, upCount_ = 0;

	float prevInL_ = 0.0f, prevInR_ = 0.0f;
	float prevOutL_ = 0.0f, prevOutR_ = 0.0f;
	float curOutL_ = 0.0f, curOutR_ = 0.0f;
};
