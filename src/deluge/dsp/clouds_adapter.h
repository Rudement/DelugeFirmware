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
// subclass allocated from that pool. It stays pinned while Clouds is active,
// because losing it would discard the recording and require expensive recovery
// on the audio path.
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
	/// Deliberately does NOT test buffer_. The private adapter acquires and
	/// prepares its working buffer before publication; this check runs earlier,
	/// immediately after construction.
	[[nodiscard]] bool isValid() const { return processor_ != nullptr; }

	/// How many adapters exist right now (one per synth, kit row or audio clip with Wisps on).
	/// Includes silent ones; the mode menu goes by activeCount().
	[[nodiscard]] static int32_t liveCount() { return liveCount_; }

	/// Adapters that have been rendered in the last second or so. Unlike liveCount(), this
	/// leaves out ones kept alive but silent -- an instrument held only by undo history, or a
	/// Sound that has stopped rendering -- which must not block switching Wisps on elsewhere.
	[[nodiscard]] static int32_t activeCount();

	/// True once Blend is above minimum: the effect is producing sound, including tails and a
	/// frozen buffer after the input has stopped. A Sound keeps rendering while this holds.
	/// False once the overload guard has shed this instance.
	[[nodiscard]] bool isAudible() const { return blend_ > 0.0f && !shed_; }

	// ---- Overload guard ------------------------------------------------------------------
	// Two Spectrals measured DSP peak 286 and ~4800 hard culls: voices get culled, but Wisps
	// itself cannot be, so the render never catches up and the unit locks up. So while two or
	// more instances are audible and renders keep running past the cull limit, the most
	// recently switched-on one is shed: it ramps to dry within one block, and then
	// takeShedOwner() hands its owner to the UI side to switch it OFF and say why.

	/// The ModControllableAudio this adapter belongs to (opaque here: dsp/ does not depend on
	/// model/). Set when the owner publishes it.
	void setOwner(void* owner) { owner_ = owner; }

	/// Once per audio render, from AudioEngine's direness check: how far that render ran
	/// past the cull limit, in samples (negative when inside it).
	static void noteRenderLoad(int32_t samplesOverLimit);

	/// The owner of a shed adapter that has finished ramping to dry, at most once per
	/// adapter; nullptr if none. Called from outside any render.
	static void* takeShedOwner();

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

	/// Mode change on a running adapter, click-free: the next render ramps the wet return to
	/// silence, the one after switches mode (setMode() plus its structural Prepare, in the render)
	/// and fades the new mode in. setMode() directly cuts the old mode's output mid-waveform, which
	/// popped on every step of the mode menu. Several requests before the switch: the last wins.
	void requestMode(CloudsMode mode) { pendingMode_ = mode == mode_ ? CloudsMode::OFF : mode; }
	[[nodiscard]] CloudsMode mode() const { return mode_; }

	void setPosition(q31_t v);
	void setSize(q31_t v);
	/// Bipolar: full negative is -2 octaves, full positive +2 octaves.
	void setPitch(q31_t v);
	void setDensity(q31_t v);
	void setTexture(q31_t v);
	/// Deluge-level dry/wet blend, mixed here at the Deluge rate in every mode.
	///
	/// The engine itself always runs fully wet. Upstream's own dry/wet stage would
	/// send the dry signal through the 32 kHz round trip and through the output
	/// converter that halves the level, so any Blend above zero used to cost the
	/// dry signal about 9 dB and its top octave. Here the dry path is the caller's
	/// exact q31 input, crossfaded with upstream's own equal-power curve
	/// (lut_xfade_in/out, normalised so each end is unity).
	///
	/// Minimum is a true computational bypass and preserves the exact input.
	/// Resonestor repurposes upstream's dry_wet field as distortion; that hidden
	/// control is left neutral.
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
	/// The adapter must be private (not reachable by the audio path) while this runs. Returns
	/// false if its working buffer could not be acquired.
	bool prepareOffAudioThread();

	/// Render in place over `buffer`, at the Deluge's kSampleRate. Internally
	/// resamples down to Clouds' native 32 kHz with a band-limited polyphase
	/// filter, runs whole clouds::kMaxBlockSize blocks through the untouched
	/// upstream processor as soon as each one is complete, and resamples back up
	/// with a matching filter. The wet path has a fixed latency of kLatency
	/// samples whatever block sizes the engine hands us. No-op if !isValid().
	void process(std::span<StereoSample> buffer);

	// --- Resampler design. Public so the native harness can check them. -----
	/// Kaiser-windowed sinc, cutoff 15.5 kHz for both directions: flat to ~14 kHz,
	/// 1 dB down at 14.5 kHz, and anything that could fold back below 13.5 kHz is
	/// more than 100 dB down. Clouds itself stops at 16 kHz (it runs at 32 kHz).
	static constexpr int32_t kDownTaps = 48; ///< at 44.1 kHz
	static constexpr int32_t kUpTaps = 32;   ///< at 32 kHz
	static constexpr int32_t kKernelOversample = 128;
	/// 44.1 kHz : 32 kHz is exactly 441 : 320, so phases are tracked exactly as
	/// integers -- no drift, no float phase accumulator.
	static constexpr int32_t kRateIn = 441;
	static constexpr int32_t kRateClouds = 320;
	/// Wet-path delay in Deluge samples. Enough that the output filter never asks
	/// for an engine frame that a whole 32-frame block has not yet produced, for
	/// ANY sequence of render block sizes (worst case: half the down filter, plus
	/// half the up filter and one engine block measured in Deluge samples).
	static constexpr int32_t kLatency =
	    kDownTaps / 2 + ((kUpTaps / 2 + 32 + 2) * kRateIn + kRateClouds - 1) / kRateClouds + 1;

#ifdef CLOUDS_ADAPTER_STATS
	/// Native-harness instrumentation only: times the output filter needed a frame
	/// the engine had not produced yet. Must stay 0.
	uint32_t statUnderruns = 0;
#endif

private:
	/// Acquire the full working set while this adapter is still private and unreachable by audio.
	/// Render code cannot call this allocator.
	bool acquireBuffer();
	void resetResamplerState();
	/// Upstream playback mode and channel count for mode_ (Spectral is mono).
	void applyPlaybackMode();
	/// Push one 32 kHz frame towards the engine; runs the engine when a block is complete.
	void pushEngineFrame(float l, float r);
	/// Does this mode have periodic Prepare() work (FFT hops, WSOLA correlator)?
	[[nodiscard]] bool modeNeedsPeriodicPrepare() const {
		return mode_ == CloudsMode::SPECTRAL || mode_ == CloudsMode::STRETCH || mode_ == CloudsMode::OLIVERB;
	}

	static int32_t liveCount_;
	static constexpr int32_t kMaxTracked = 32;
	static CloudsAdapter* tracked_[kMaxTracked];
	static uint32_t nextStartSeq_;
	static int32_t overloadScore_;
	/// Overloaded renders add 4, others take 1 away; shed at this. About 50 net overloaded
	/// renders: a lasting overload, not the one-off spike of a mode change or a song load.
	static constexpr int32_t kShedScore = 200;
	uint32_t startSeq_ = 0;
	/// Counts audio renders (noteRenderLoad runs once per render).
	static uint32_t renderTick_;
	uint32_t lastRenderTick_ = 0;
	static constexpr uint32_t kActiveRenders = 400; // ~1 s of 128-sample renders
	bool shed_ = false;
	bool shedReported_ = false;
	void* owner_ = nullptr;

	clouds::GranularProcessor* processor_ = nullptr;
	CloudsBuffer* buffer_ = nullptr;

	CloudsMode mode_ = CloudsMode::GRANULAR;
	/// Samples left before periodic slice work may start after a structural Prepare.
	int32_t settleSamplesLeft_ = 0;
	static constexpr int32_t kSettleSamples = 512; // ~12 ms
	/// Per-mode level trim on the wet return (see applyPlaybackMode()).
	float modeWetTrim_ = 1.0f;
	/// From requestMode(); OFF when none is waiting.
	CloudsMode pendingMode_ = CloudsMode::OFF;
	/// Samples of the pre-switch fade-out left; -1 when not started.
	int32_t switchRampLeft_ = -1;
	static constexpr int32_t kSwitchRampSamples = kSampleRate / 100; // 10 ms
	float blend_ = 0.0f;
	/// Freeze as last requested. Upstream's Prepare() clears its own copy whenever
	/// it takes the reset path, so it is re-applied after every in-render reset.
	bool freeze_ = false;

	/// Samples of fade-in remaining after a mode change, a re-Init or a return
	/// from bypass. Changing playback mode makes Prepare() take its reset path,
	/// so the engine's output jumps discontinuously; ramping the wet return in
	/// over a few milliseconds hides that click.
	int32_t fadeInRemaining_ = 0;


	/// Set when the next Prepare() will take its expensive reallocation path,
	/// so process() can tell the audio engine not to cull voices over it.
	bool heavyPreparePending_ = true;
	static constexpr int32_t kFadeInSamples = kSampleRate / 50; // 20 ms

	/// Set until private preparation has initialized upstream against the working buffer, and again
	/// if defensive non-finite recovery requires a rebuild.
	bool needsInit_ = true;

	/// True while the last call was bypassed (Blend at minimum). The resampler
	/// history is stale then, so it is cleared before the wet path resumes.
	bool bypassed_ = true;
	/// Samples of engine-input fade-in remaining after a return from bypass.
	int32_t inputFadeRemaining_ = 0;
	static constexpr int32_t kInputFadeSamples = kSampleRate / 200; // 5 ms

	/// Work allowance, in ARM instructions, for upstream's periodic Prepare()
	/// work, which process() runs a slice per render (see PrepareSliced()).
	/// Spectral (mono, NEON FFT) charges ~1.16M per hop against 1000 x 1411
	/// samples = 1.41M of allowance per 32 ms hop. Lower is better here: the rate
	/// is also the most one render may spend, so it sets the CPU peak. The
	/// harness output is bit-identical to unlimited allowance at 1000 (with
	/// 128-sample and random 8..128-sample renders); at 800 it starts to differ. 1000 cut the
	/// worst render from 318k to 223k instructions (347k to 252k with Reverb up).
	/// The WSOLA search rate matches what upstream's four-call search managed
	/// from a 5 ms task. A step that overruns what was left is carried as debt.
	int32_t sliceCredit_ = 0;
	static constexpr int32_t kSpectralCreditPerSample = 1000;
	static constexpr int32_t kSearchCreditPerSample = 1600;
	/// Slice work one render may do: the larger of this and its own accrual.
	static constexpr int32_t kMaxSliceWorkPerRender = 30000;
	/// Unspent allowance kept for catching up after long renders.
	static constexpr int32_t kMaxBankedCredit = 400000;

	/// Current (smoothed) mix gains; ramped to the Blend target across each block.
	float dryGain_ = 1.0f;
	float wetGain_ = 0.0f;

	// --- Resampler state (per instance: every GlobalEffectable has its own). --
	/// Input history at the Deluge rate.
	static constexpr uint32_t kInHistFrames = 256; // > kChunk + kDownTaps, power of two
	/// process() works in chunks of at most this many samples.
	static constexpr int32_t kChunk = 64;
	/// One per channel, mirrored: every sample is written twice, kInHistFrames
	/// apart, so any kDownTaps-long window is contiguous without wrap checks
	/// and loads straight into NEON registers.
	float inHistL_[kInHistFrames * 2]{};
	float inHistR_[kInHistFrames * 2]{};
	uint32_t inCount_ = 0; ///< Deluge samples written; wraps harmlessly
	/// Position of the next 32 kHz frame to emit, in Deluge samples: int + rem/320.
	uint32_t downPosInt_ = 0;
	uint32_t downPosRem_ = 0;

	/// Frames per call into the engine. Upstream's codec block is 32; the engine
	/// takes any size up to that. 16 spreads its work over more of the Deluge's
	/// (~19-sample) renders instead of loading every second or third: on ARM it
	/// halved the worst render in the steady modes for 2-4% more total work.
	static constexpr int32_t kEngineBlockFrames = 16;
	/// Engine block being filled.
	int16_t engineIn_[32 * 2]{};
	uint32_t engineFill_ = 0;

	/// Engine output history at 32 kHz.
	static constexpr uint32_t kWetHistFrames = 256; // power of two
	/// Per channel, mirrored the same way.
	float wetHistL_[kWetHistFrames * 2]{};
	float wetHistR_[kWetHistFrames * 2]{};
	uint32_t wetCount_ = 0; ///< engine frames produced; wraps harmlessly
	/// Position of the next output sample in 32 kHz frames: int + rem/441.
	uint32_t upPosInt_ = 0;
	uint32_t upPosRem_ = 0;
};
