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
#include "memory/general_memory_allocator.h"
#include "processing/engines/audio_engine.h"

#include <algorithm>
#include <atomic>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <cmath>
#include <cstring>
#include <iterator>
#include <new>

namespace {
// Deluge's audio rate (definitions_cxx.hpp) and Clouds' native rate
// (clouds/clouds.cc, and scattered through resonestor.h/reverb.h -- see
// PROVENANCE.md for why that isn't a single constant we could just read).
constexpr float kCloudsSampleRate = 32000.0f;
static_assert(kSampleRate * CloudsAdapter::kRateClouds == 32000 * CloudsAdapter::kRateIn,
              "the exact 441:320 phase arithmetic assumes 44.1 kHz in, 32 kHz in Clouds");

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

/// Gain that restores the engine's wet output to the module's balance.
///
/// Upstream ends Process() with stmlib::SoftConvert(), which is
/// SoftLimit(x * 0.5f) -- it halves the signal on the way to the 16-bit codec,
/// and the module's analog output stage makes that back. Its dry/wet crossfade
/// also peaks at 0.7071 at either end (lut_xfade_*). Measured natively, that
/// left the wet path 7.5 dB and the dry path 9 dB below the input. Scaling the
/// wet return by 2 / 0.7071 puts dry at exactly unity (we mix it ourselves)
/// and keeps upstream's own wet-to-dry ratio, including its 1.2 post-gain.
constexpr float kWetMakeup = 2.0f / 0.70710678f;
constexpr float kXfadeNormalise = 1.0f / 0.70710678f;

/// Upstream's equal-power crossfade curve, read from its own 17-point tables.
/// (stmlib's Interpolate() would read one past the end at exactly 1.0.)
inline float xfade(const float* lut, float x) {
	x = std::clamp(x, 0.0f, 1.0f) * 16.0f;
	int32_t i = std::min<int32_t>(static_cast<int32_t>(x), 15);
	float f = x - static_cast<float>(i);
	return (lut[i] + (lut[i + 1] - lut[i]) * f) * kXfadeNormalise;
}

// Kernel tables, laid out phase-major: row p (0..kKernelOversample) holds the
// coefficient for every tap at fractional position p/kKernelOversample, so one
// output reads two adjacent contiguous rows and blends them. Kaiser-windowed
// sinc, cutoff 15.5 kHz, beta 9; tap i sits at t = taps/2 - 1 - i + frac grid
// samples from the point computed.
//
// Precomputed (scripts/clouds/gen_clouds_kernels.cpp runs the
// generator these were first computed with at runtime) because building them
// on the Deluge was ~4M instructions -- a 10-20 ms freeze the first time Wisps
// was switched on.
#include "clouds_adapter_kernels.h"
static_assert(std::size(downKernel) == (CloudsAdapter::kKernelOversample + 1) * CloudsAdapter::kDownTaps);
static_assert(std::size(upKernel) == (CloudsAdapter::kKernelOversample + 1) * CloudsAdapter::kUpTaps);

/// One polyphase dot product, both channels. `histL`/`histR` point at the
/// oldest tap in mirrored (written twice) per-channel histories, so all kTaps
/// samples are contiguous. The fractional position is the exact fraction
/// num/den; coefficients are blended between two adjacent kernel rows.
///
/// NEON on the Deluge, four taps per instruction. Measured on ARM code the
/// scalar version of this cost ~25 M instructions per second per instance --
/// about as much as the Granular engine itself -- because GCC does not
/// vectorise it on its own.
template <int32_t kTaps>
[[gnu::always_inline]] inline void polyphase(const float* kernel, const float* __restrict histL,
                                             const float* __restrict histR, uint32_t num, uint32_t den, float& outL,
                                             float& outR) {
	static_assert(kTaps % 8 == 0);
	const uint32_t scaled = num * CloudsAdapter::kKernelOversample;
	const uint32_t row = scaled / den;
	const float frac = static_cast<float>(scaled - row * den) / static_cast<float>(den);
	const float* __restrict k0 = &kernel[row * kTaps];
	const float* __restrict k1 = k0 + kTaps;
#if defined(__ARM_NEON)
	const float32x4_t f = vdupq_n_f32(frac);
	float32x4_t l0 = vdupq_n_f32(0.0f);
	float32x4_t l1 = vdupq_n_f32(0.0f);
	float32x4_t r0 = vdupq_n_f32(0.0f);
	float32x4_t r1 = vdupq_n_f32(0.0f);
#pragma GCC unroll 8
	for (int32_t i = 0; i < kTaps; i += 8) {
		const float32x4_t a0 = vld1q_f32(k0 + i);
		const float32x4_t a1 = vld1q_f32(k0 + i + 4);
		const float32x4_t c0 = vmlaq_f32(a0, vsubq_f32(vld1q_f32(k1 + i), a0), f);
		const float32x4_t c1 = vmlaq_f32(a1, vsubq_f32(vld1q_f32(k1 + i + 4), a1), f);
		l0 = vmlaq_f32(l0, c0, vld1q_f32(histL + i));
		l1 = vmlaq_f32(l1, c1, vld1q_f32(histL + i + 4));
		r0 = vmlaq_f32(r0, c0, vld1q_f32(histR + i));
		r1 = vmlaq_f32(r1, c1, vld1q_f32(histR + i + 4));
	}
	const float32x4_t l = vaddq_f32(l0, l1);
	const float32x4_t r = vaddq_f32(r0, r1);
	const float32x2_t lr = vpadd_f32(vadd_f32(vget_low_f32(l), vget_high_f32(l)),
	                                 vadd_f32(vget_low_f32(r), vget_high_f32(r)));
	outL = vget_lane_f32(lr, 0);
	outR = vget_lane_f32(lr, 1);
#else
	// Same arithmetic, for the native test harness.
	float l[4] = {}, r[4] = {};
	for (int32_t i = 0; i < kTaps; i += 4) {
		for (int32_t j = 0; j < 4; ++j) {
			const float c = k0[i + j] + (k1[i + j] - k0[i + j]) * frac;
			l[j] += c * histL[i + j];
			r[j] += c * histR[i + j];
		}
	}
	outL = (l[0] + l[1]) + (l[2] + l[3]);
	outR = (r[0] + r[1]) + (r[2] + r[3]);
#endif
}

/// NaN/Inf test that survives -ffast-math. The firmware is built with
/// -ffast-math, which lets GCC assume no value is ever NaN or Inf and delete
/// std::isfinite() checks outright -- so the old poison guard never existed in
/// the binary. Looking at the exponent bits cannot be optimised away.
[[gnu::always_inline]] inline bool notFinite(float x) {
	uint32_t bits;
	std::memcpy(&bits, &x, sizeof bits);
	return (bits & 0x7f800000u) == 0x7f800000u;
}

/// Round a -1..1 float to int16 (scale 32768), saturating. Adding 1.5 * 2^23
/// leaves the rounded integer in the low mantissa bits; reading them back
/// through memcpy keeps -ffast-math from folding the add away.
[[gnu::always_inline]] inline int16_t toInt16Rounded(float x) {
	float y = std::clamp(x * 32768.0f, -32768.0f, 32767.0f) + 12582912.0f;
	int32_t bits;
	std::memcpy(&bits, &y, sizeof bits);
	return static_cast<int16_t>(bits - 0x4B400000);
}

/// Output protection. Below kKnee the signal passes untouched; above it the
/// curve bends smoothly (matching slope at the knee) towards full scale instead
/// of hard-clipping. The wet return is now at the module's level, and dense
/// Granular clouds on a hot input can sum past full scale.
constexpr float kKnee = 0.75f;
[[gnu::always_inline]] inline float softKnee(float x) {
	const float a = std::fabs(x);
	if (a <= kKnee) {
		return x;
	}
	const float u = (a - kKnee) * (1.0f / (1.0f - kKnee));
	const float y = kKnee + (1.0f - kKnee) * (u / (1.0f + u));
	return x < 0.0f ? -y : y;
}

/// Mix one chunk in place: out = dry * dryGain + wet * wetGain, gains ramping
/// per sample (gain for sample i is g + step * (i + 1)), through the soft knee,
/// back to q31. Returns true if the wet input held a NaN/Inf.
///
/// Poison guard: if the engine ever emits a non-finite sample -- feedback
/// paths, a resonator driven to feedback 20, float state we do not fully
/// control -- it must not reach the q31 conversion (undefined; in practice a
/// full-scale click). Those samples are zeroed and the caller re-Inits.
bool mixChunk(StereoSample* chunk, float* wetL, float* wetR, size_t n, float dryGain, float dryStep, float wetGain,
              float wetStep) {
	bool poisoned = false;
	for (size_t i = 0; i < n; ++i) {
		if (notFinite(wetL[i]) || notFinite(wetR[i])) {
			wetL[i] = 0.0f;
			wetR[i] = 0.0f;
			poisoned = true;
		}
	}
	size_t i = 0;
#if defined(__ARM_NEON)
	const float32x4_t ramp = {1.0f, 2.0f, 3.0f, 4.0f};
	const float32x4_t knee = vdupq_n_f32(kKnee);
	const float32x4_t range = vdupq_n_f32(1.0f - kKnee);
	const float32x4_t invRange = vdupq_n_f32(1.0f / (1.0f - kKnee));
	const float32x4_t one = vdupq_n_f32(1.0f);
	auto knee4 = [&](float32x4_t x) {
		const float32x4_t a = vabsq_f32(x);
		const float32x4_t u = vmulq_f32(vmaxq_f32(vsubq_f32(a, knee), vdupq_n_f32(0.0f)), invRange);
		const float32x4_t d = vaddq_f32(one, u);
		float32x4_t rcp = vrecpeq_f32(d);
		rcp = vmulq_f32(rcp, vrecpsq_f32(d, rcp));
		rcp = vmulq_f32(rcp, vrecpsq_f32(d, rcp));
		const float32x4_t y = vmlaq_f32(knee, range, vmulq_f32(u, rcp));
		const uint32x4_t over = vcgtq_f32(a, knee);
		const uint32x4_t sign = vandq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(0x80000000u));
		const float32x4_t ySigned = vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(y), sign));
		return vbslq_f32(over, ySigned, x);
	};
	for (; i + 4 <= n; i += 4) {
		int32x4x2_t in = vld2q_s32(reinterpret_cast<const int32_t*>(&chunk[i]));
		const float base = static_cast<float>(i);
		const float32x4_t gd = vmlaq_n_f32(vdupq_n_f32(dryGain + dryStep * base), ramp, dryStep);
		const float32x4_t gw = vmlaq_n_f32(vdupq_n_f32(wetGain + wetStep * base), ramp, wetStep);
		float32x4_t l = vmulq_f32(vcvtq_n_f32_s32(in.val[0], 31), gd);
		float32x4_t r = vmulq_f32(vcvtq_n_f32_s32(in.val[1], 31), gd);
		l = vmlaq_f32(l, vld1q_f32(wetL + i), gw);
		r = vmlaq_f32(r, vld1q_f32(wetR + i), gw);
		int32x4x2_t out;
		out.val[0] = vcvtq_n_s32_f32(knee4(l), 31); // saturating
		out.val[1] = vcvtq_n_s32_f32(knee4(r), 31);
		vst2q_s32(reinterpret_cast<int32_t*>(&chunk[i]), out);
	}
#endif
	for (; i < n; ++i) {
		const float k = static_cast<float>(i + 1);
		const float gd = dryGain + dryStep * k;
		const float gw = wetGain + wetStep * k;
		const float l = static_cast<float>(chunk[i].l) * (1.0f / 2147483648.0f) * gd + wetL[i] * gw;
		const float r = static_cast<float>(chunk[i].r) * (1.0f / 2147483648.0f) * gd + wetR[i] * gw;
		chunk[i].l = static_cast<q31_t>(std::clamp(softKnee(l) * 2147483648.0f, -2147483648.0f, 2147483520.0f));
		chunk[i].r = static_cast<q31_t>(std::clamp(softKnee(r) * 2147483648.0f, -2147483648.0f, 2147483520.0f));
	}
	return poisoned;
}

} // namespace

void CloudsBuffer::steal(char const* errorCode) {
	owner_->bufferStolen();
}

int32_t CloudsAdapter::liveCount_ = 0;
CloudsAdapter* CloudsAdapter::tracked_[kMaxTracked] = {};
uint32_t CloudsAdapter::nextStartSeq_ = 0;
int32_t CloudsAdapter::overloadScore_ = 0;

uint32_t CloudsAdapter::renderTick_ = 0;

int32_t CloudsAdapter::activeCount() {
	int32_t n = 0;
	for (CloudsAdapter* a : tracked_) {
		if (a != nullptr && renderTick_ - a->lastRenderTick_ < kActiveRenders) {
			++n;
		}
	}
	return n;
}

void CloudsAdapter::noteRenderLoad(int32_t samplesOverLimit) {
	++renderTick_;
	int32_t audible = 0;
	CloudsAdapter* newest = nullptr;
	for (CloudsAdapter* a : tracked_) {
		if (a != nullptr && a->isAudible()) {
			++audible;
			if (newest == nullptr || a->startSeq_ > newest->startSeq_) {
				newest = a;
			}
		}
	}
	if (audible < 2) {
		// One instance is the user's call (and the voices can still be culled around it).
		overloadScore_ = 0;
		return;
	}
	if (samplesOverLimit >= 0) {
		overloadScore_ += 4;
	}
	else if (overloadScore_ > 0) {
		--overloadScore_;
	}
	if (overloadScore_ >= kShedScore) {
		newest->shed_ = true;
		overloadScore_ = 0;
	}
}

void* CloudsAdapter::takeShedOwner() {
	for (CloudsAdapter* a : tracked_) {
		// Wait for the ramp to dry to finish (bypassed_), so switching it off cuts nothing.
		if (a != nullptr && a->shed_ && !a->shedReported_ && a->bypassed_) {
			a->shedReported_ = true;
			return a->owner_;
		}
	}
	return nullptr;
}

CloudsAdapter::CloudsAdapter() {
	++liveCount_;
	startSeq_ = ++nextStartSeq_;
	lastRenderTick_ = renderTick_;
	for (CloudsAdapter*& slot : tracked_) {
		if (slot == nullptr) {
			slot = this;
			break;
		}
	}

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
	--liveCount_;
	for (CloudsAdapter*& slot : tracked_) {
		if (slot == this) {
			slot = nullptr;
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
		void* memory = GeneralMemoryAllocator::get().allocStealable<CloudsBuffer>();
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
	std::fill(std::begin(inHistL_), std::end(inHistL_), 0.0f);
	std::fill(std::begin(inHistR_), std::end(inHistR_), 0.0f);
	std::fill(std::begin(wetHistL_), std::end(wetHistL_), 0.0f);
	std::fill(std::begin(wetHistR_), std::end(wetHistR_), 0.0f);
	inCount_ = 0;
	downPosInt_ = 0;
	downPosRem_ = 0;
	engineFill_ = 0;
	wetCount_ = 0;
	// Output sample j sits at Deluge time j - kLatency, i.e. at
	// (j - kLatency) * 320/441 in engine frames. Start at j = 0: a negative
	// position, held as two's complement and floored, with a non-negative
	// remainder. Frames before 0 read the zeroed history.
	const int32_t num = -kLatency * kRateClouds;
	int32_t q = num / kRateIn;
	int32_t rem = num - q * kRateIn;
	if (rem < 0) {
		rem += kRateIn;
		--q;
	}
	upPosInt_ = static_cast<uint32_t>(q);
	upPosRem_ = static_cast<uint32_t>(rem);
}

void CloudsAdapter::applyPlaybackMode() {
	// Spectral runs Clouds' own mono quality setting: one STFT channel instead of
	// two. Its hop work (two 4096-point FFT/IFFT pairs plus the frame transform,
	// sweeping ~200 KB of SDRAM every 32 ms) was more than the Deluge could do in
	// real time in stereo -- measured on the unit: DSP 58 average, thousands of
	// hard culls. Mono halves it. Upstream mixes the input to mono and copies the
	// result to both outputs; the dry path stays stereo. Every other mode keeps
	// stereo. A channel-count change makes upstream rebuild its buffers, which a
	// change into or out of Spectral does anyway.
	processor_->set_num_channels(mode_ == CloudsMode::SPECTRAL ? 1 : 2);
	processor_->set_playback_mode(kPlaybackModeFor[util::to_underlying(mode_)]);
	// Upstream's dry/wet: the adapter does its own Blend, so the engine runs fully wet -- except
	// Resonestor, where Parasites reuses this parameter as the distortion amount. Set here, with
	// the mode, so every way into or out of Resonestor gets it (a mode change in the render used to
	// miss it: Resonestor came in fully distorted and loud, and the next mode came out dry).
	processor_->mutable_parameters()->dry_wet = mode_ == CloudsMode::RESONESTOR ? 0.0f : 1.0f;
	// Resonestor's resonators ring far above the input on sustained material: +9.6 dB wet over
	// dry at full Blend on a held chord (Delay and Stretch sit at 0 dB). On the unit it stood out
	// as louder than dry and than every other mode, so its wet return is taken down 6 dB: about
	// +3.6 dB re dry on that chord, near Granular. (-12 dB was tried while a Sound render bug fed
	// Resonestor stale audio; with that fixed it was too quiet.)
	modeWetTrim_ = mode_ == CloudsMode::RESONESTOR ? 0.5f : 1.0f;
}

void CloudsAdapter::setMode(CloudsMode mode) {
	if (mode == mode_ || mode == CloudsMode::OFF) {
		return;
	}
	mode_ = mode;
	startSeq_ = ++nextStartSeq_; // the overload guard sheds the most recently started first
	fadeInRemaining_ = kFadeInSamples; // hide the reallocation discontinuity
	heavyPreparePending_ = true;
	if (processor_ != nullptr) {
		// Upstream reallocates its internal players out of the working buffer
		// when the playback mode changes, so this is only safe once Init has
		// run against a buffer we actually hold.
		if (!needsInit_ && buffer_ != nullptr) {
			applyPlaybackMode();
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
		// The engine always runs fully wet; the blend happens in process(), at
		// the Deluge rate, against the untouched dry signal. In Resonestor,
		// Parasites reads this field as distortion instead, which stays neutral.
	}
}

void CloudsAdapter::setSpread(q31_t v) {
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->stereo_spread = q31ToUnipolar(v);
	}
}

void CloudsAdapter::setFeedback(q31_t v) {
	if (processor_ != nullptr) {
		float feedback = q31ToUnipolar(v);
		if (mode_ == CloudsMode::GRANULAR) {
			// Device testing puts the regenerative threshold near upstream 0.238.
			// Keep maximum just below it: strong and slow-decaying without entering
			// the self-running region. The margin covers source-dependent gain.
			feedback *= 0.225f;
		}
		processor_->mutable_parameters()->feedback = feedback;
	}
}

void CloudsAdapter::setReverb(q31_t v) {
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->reverb = q31ToUnipolar(v);
	}
}

void CloudsAdapter::setFreeze(bool frozen) {
	freeze_ = frozen;
	if (processor_ != nullptr) {
		processor_->mutable_parameters()->freeze = frozen;
	}
}

bool CloudsAdapter::prepareOffAudioThread() {
	if (processor_ == nullptr) {
		return false;
	}
	if (!acquireBuffer()) {
		return false;
	}
	if (needsInit_) {
		processor_->Init(buffer_->large(), CloudsBuffer::kLargeBufferBytes, buffer_->small(),
		                 CloudsBuffer::kSmallBufferBytes);
		processor_->set_silence(false);
		processor_->set_bypass(false);
		resetResamplerState();
		needsInit_ = false;
	}
	applyPlaybackMode();

	// The expensive one. Runs here so the audio thread never sees it. Zero
	// credit: only the structural part -- the periodic work belongs to the render.
	int32_t noCredit = 0;
	processor_->PrepareSliced(&noCredit);

	heavyPreparePending_ = false;
	sliceCredit_ = 0;
	settleSamplesLeft_ = kSettleSamples;
	fadeInRemaining_ = kFadeInSamples;
	return true;
}

void CloudsAdapter::pushEngineFrame(float l, float r) {
	// Round to nearest rather than truncate toward zero: truncation is a
	// level-dependent dead band around zero (crossover distortion on quiet
	// tails). The engine records 16-bit audio by design, so this is the one
	// place its input precision is decided. (Rounded with the float-add trick,
	// not lrintf, which is a library call on ARM -- 2 M calls a second.)
	engineIn_[engineFill_ * 2] = toInt16Rounded(l);
	engineIn_[engineFill_ * 2 + 1] = toInt16Rounded(r);
	if (++engineFill_ < static_cast<uint32_t>(kEngineBlockFrames)) {
		return;
	}
	engineFill_ = 0;

	clouds::ShortFrame out[kEngineBlockFrames];
	processor_->Process(reinterpret_cast<clouds::ShortFrame*>(engineIn_), out, kEngineBlockFrames);
	const float wetScale = (kWetMakeup / 32768.0f) * modeWetTrim_;
	for (size_t i = 0; i < static_cast<size_t>(kEngineBlockFrames); ++i) {
		const uint32_t idx = wetCount_ & (kWetHistFrames - 1);
		wetHistL_[idx] = wetHistL_[idx + kWetHistFrames] = static_cast<float>(out[i].l) * wetScale;
		wetHistR_[idx] = wetHistR_[idx + kWetHistFrames] = static_cast<float>(out[i].r) * wetScale;
		++wetCount_;
	}
}

void CloudsAdapter::process(std::span<StereoSample> buffer) {
	lastRenderTick_ = renderTick_;
	// A requested mode change, once the old mode has ramped out (or was silent anyway).
	if (pendingMode_ != CloudsMode::OFF && (bypassed_ || (switchRampLeft_ == 0 && wetGain_ <= 0.0f))) {
		setMode(pendingMode_); // sets heavyPreparePending_ and the fade-in
		pendingMode_ = CloudsMode::OFF;
		switchRampLeft_ = -1;
		// The wet history holds the old mode's last output; start the new one from silence.
		resetResamplerState();
		inputFadeRemaining_ = kInputFadeSamples;
	}

	// Minimum Blend is a true bypass. Besides preserving the exact input words,
	// this keeps Spectral's FFT maintenance from consuming audio time for an
	// inaudible effect. Raising Blend resumes the retained engine state.
	//
	// Getting there takes one block: the first block at minimum still runs, so
	// the wet return and the engine's input ramp to zero instead of being cut
	// mid-waveform. Every block after that is the exact bypass.
	// Ramping out ahead of a mode change: the wet return fades over kSwitchRampSamples (several
	// blocks, so even a loud wet signal bends rather than kinks), then the switch happens above.
	float switchScale = 1.0f;
	if (pendingMode_ != CloudsMode::OFF) {
		if (switchRampLeft_ < 0) {
			switchRampLeft_ = kSwitchRampSamples;
		}
		switchRampLeft_ = std::max<int32_t>(0, switchRampLeft_ - static_cast<int32_t>(buffer.size()));
		switchScale = static_cast<float>(switchRampLeft_) / static_cast<float>(kSwitchRampSamples);
	}
	// Shed by the overload guard: treated exactly like Blend at minimum.
	const float blend = shed_ ? 0.0f : blend_;
	const bool rampingOut = blend <= 0.0f || switchScale <= 0.0f;
	if (rampingOut && (bypassed_ || wetGain_ <= 0.0f)) {
		bypassed_ = true;
		return;
	}

	// A published adapter was fully prepared with both objects. Keep the check defensive: an
	// unexpected ownership loss passes audio through dry, and never allocates from this render path.
	if (processor_ == nullptr || buffer_ == nullptr) {
		return; // Dry passthrough: caller's buffer is left exactly as it came in.
	}

	if (needsInit_) {
		processor_->Init(buffer_->large(), CloudsBuffer::kLargeBufferBytes, buffer_->small(),
		                 CloudsBuffer::kSmallBufferBytes);
		// Init() sets bypass_ but NOT silence_ -- see the constructor. Saying
		// it explicitly here means a future re-vendor that reorders Init()
		// cannot silently reintroduce the silent-engine bug.
		processor_->set_silence(false);
		processor_->set_bypass(false);
		applyPlaybackMode();
		resetResamplerState();
		fadeInRemaining_ = kFadeInSamples;
		heavyPreparePending_ = true;
		needsInit_ = false;
	}

	if (bypassed_) {
		// Back from bypass: the resampler history predates the gap, so start
		// it clean and fade the wet return in. The engine's own recording keeps
		// whatever it held, exactly as a frozen module would.
		resetResamplerState();
		fadeInRemaining_ = kFadeInSamples;
		inputFadeRemaining_ = kInputFadeSamples;
		dryGain_ = 1.0f;
		wetGain_ = 0.0f;
		bypassed_ = false;
	}

	// Prepare() on a mode change or a re-Init takes its reset path: fourteen
	// Init/Allocate calls. Upstream runs it from its main loop; when it is due
	// here we have no choice, so tell the engine not to cull voices over the
	// spike. (Without this, changing mode blew the deadline, every voice was
	// culled, and Resonestor looked broken for want of input.)
	if (heavyPreparePending_) {
		settleSamplesLeft_ = kSettleSamples;
		AudioEngine::bypassCulling = true;
		heavyPreparePending_ = false;
		int32_t noCredit = 0;
		processor_->PrepareSliced(&noCredit);
		sliceCredit_ = 0;
		// The reset path clears upstream's freeze; put back what the user set.
		processor_->mutable_parameters()->freeze = freeze_;
	}

	// Blend, smoothed across the block so automation and knob turns do not zipper.
	// switchScale < 1 only while ramping out for a mode change: towards all dry, like Blend 0.
	const float wetTarget = xfade(clouds::lut_xfade_in, blend) * switchScale;
	const float dryTarget = 1.0f - (1.0f - xfade(clouds::lut_xfade_out, blend)) * switchScale;
	const float inv = 1.0f / static_cast<float>(buffer.size());
	const float dryStep = (dryTarget - dryGain_) * inv;
	const float wetStep = (wetTarget - wetGain_) * inv;
	float dryGain = dryGain_;
	float wetGain = wetGain_;
	bool poisoned = false;
	// Engine input gain: ramps down across a ramp-out block, up after a resume,
	// so the engine's recording holds a short fade at each edge rather than a
	// step that grains and delay taps would later replay as a click.
	float inGain = 1.0f;
	const float inGainStep = rampingOut ? -inv : 0.0f;

	// Worked in chunks: all of a chunk's input goes down to 32 kHz (running the
	// engine whenever a 32-frame block completes), then the chunk's wet output
	// comes back up into a scratch buffer, then dry and wet are mixed four
	// samples at a time. Each output sample is still computed only after every
	// input up to it has been consumed, so kLatency holds exactly as before.
	float wetL[kChunk];
	float wetR[kChunk];
	size_t done = 0;
	while (done < buffer.size()) {
		const size_t n = std::min(buffer.size() - done, static_cast<size_t>(kChunk));
		StereoSample* chunk = buffer.data() + done;

		// --- Down: 44.1 kHz -> 32 kHz.
		for (size_t i = 0; i < n; ++i) {
			float engineGain = inGain;
			inGain += inGainStep;
			if (inputFadeRemaining_ > 0) {
				engineGain *= 1.0f - static_cast<float>(inputFadeRemaining_) / static_cast<float>(kInputFadeSamples);
				--inputFadeRemaining_;
			}
			const float g = engineGain * (1.0f / 2147483648.0f);
			const uint32_t idx = inCount_ & (kInHistFrames - 1);
			inHistL_[idx] = inHistL_[idx + kInHistFrames] = static_cast<float>(chunk[i].l) * g;
			inHistR_[idx] = inHistR_[idx + kInHistFrames] = static_cast<float>(chunk[i].r) * g;
			++inCount_;
			// Emit every 32 kHz frame whose filter window this sample completes (0 or 1).
			while (static_cast<int32_t>(inCount_ - 1 - downPosInt_ - kDownTaps / 2) >= 0) {
				float l;
				float r;
				const uint32_t first = (downPosInt_ - kDownTaps / 2 + 1) & (kInHistFrames - 1);
				polyphase<kDownTaps>(downKernel, &inHistL_[first], &inHistR_[first], downPosRem_, kRateClouds, l, r);
				pushEngineFrame(l, r);
				downPosRem_ += kRateIn;
				while (downPosRem_ >= static_cast<uint32_t>(kRateClouds)) {
					downPosRem_ -= kRateClouds;
					++downPosInt_;
				}
			}
		}

		// --- Up: 32 kHz -> 44.1 kHz, kLatency samples behind the input.
		for (size_t i = 0; i < n; ++i) {
#ifdef CLOUDS_ADAPTER_STATS
			if (static_cast<int32_t>(wetCount_ - 1 - (upPosInt_ + kUpTaps / 2)) < 0
			    && static_cast<int32_t>(upPosInt_ + kUpTaps / 2) >= 0) {
				++statUnderruns;
			}
#endif
			const uint32_t first = (upPosInt_ - kUpTaps / 2 + 1) & (kWetHistFrames - 1);
			polyphase<kUpTaps>(upKernel, &wetHistL_[first], &wetHistR_[first], upPosRem_, kRateIn, wetL[i], wetR[i]);
			upPosRem_ += kRateClouds;
			if (upPosRem_ >= static_cast<uint32_t>(kRateIn)) {
				upPosRem_ -= kRateIn;
				++upPosInt_;
			}
		}

		// Fade the wet return in after a mode change, re-Init or resume. Rare.
		for (size_t i = 0; i < n && fadeInRemaining_ > 0; ++i) {
			const float gain = 1.0f - (static_cast<float>(fadeInRemaining_) / static_cast<float>(kFadeInSamples));
			wetL[i] *= gain;
			wetR[i] *= gain;
			--fadeInRemaining_;
		}

		poisoned |= mixChunk(chunk, wetL, wetR, n, dryGain, dryStep, wetGain, wetStep);

		dryGain += dryStep * static_cast<float>(n);
		wetGain += wetStep * static_cast<float>(n);
		done += n;
	}
	dryGain_ = dryTarget;
	wetGain_ = wetTarget;

	// Upstream's periodic Prepare() work -- Spectral's STFT hop, Stretch and
	// Oliverb's WSOLA load and search. On the module it runs in the main loop
	// while audio runs in an interrupt; on the Deluge the main loop renders the
	// audio, and doing it in one go stalled the 128-sample output buffer for
	// several ms. So: allowance accrues per sample, and each render spends at
	// most kMaxSliceWorkPerRender of it. A capped spend matters: the allowance
	// grows with the render's size, so a render that was long for some other
	// reason would otherwise also pay off any backlog at once, making the next
	// window longer still. Each render may spend its own accrual, or
	// kMaxSliceWorkPerRender if that is more; anything unspent carries. (It
	// used to be accrual plus a quarter; with the work spread evenly instead,
	// Spectral's worst render is about a third lower, with identical output.)
	// Not for kSettleSamples after a structural Prepare (a mode change or a switch-on): that render
	// carries the rebuild -- clearing ~180 KB of SDRAM, which costs far more on the unit than its
	// instruction count says -- and the next few run on a cold cache. Oliverb's WSOLA search keeps
	// every render near its allowance, and starting it straight away made switching into Oliverb
	// overrun (CPU light) and pop. Nothing accrues meanwhile, so no burst follows; the search simply
	// starts ~12 ms later, under the new mode's 20 ms fade-in.
	if (settleSamplesLeft_ > 0) {
		settleSamplesLeft_ -= static_cast<int32_t>(buffer.size());
	}
	else if (modeNeedsPeriodicPrepare()) {
		const int32_t rate = mode_ == CloudsMode::SPECTRAL ? kSpectralCreditPerSample : kSearchCreditPerSample;
		sliceCredit_ = std::min(sliceCredit_ + static_cast<int32_t>(buffer.size()) * rate, kMaxBankedCredit);
		// Never below this render's own accrual (a run of long renders must still
		// keep up); the cap only stops a backlog being paid off in one burst.
		const int32_t cap = std::max(kMaxSliceWorkPerRender, static_cast<int32_t>(buffer.size()) * rate);
		int32_t budget = std::min(sliceCredit_, cap);
		if (budget > 0) {
			const int32_t granted = budget;
			processor_->PrepareSliced(&budget);
			sliceCredit_ -= granted - budget; // a step that overran leaves budget negative: debt
		}
	}

	if (poisoned) {
		// Resetting our own state is not enough: the engine feeds its previous
		// output back (fb_), so NaN stays there. Only a re-Init clears it, which
		// the next process() does because needsInit_ is set.
		needsInit_ = true;
		heavyPreparePending_ = true;
		fadeInRemaining_ = kFadeInSamples;
	}

	// NOTE: the buffer is deliberately NOT released here. Losing it forces a
	// re-Init, which clears the recording Clouds exists to granulate (measured:
	// re-Init every block drops wet output 4x). It stays pinned while the
	// effect is on, like GrainBuffer, and the adapter is destroyed when the
	// mode goes OFF, which is what actually returns the 180 KB.
}
