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

#include <algorithm>
#include <cstring>
#include <new>

namespace {
// Deluge's audio rate (definitions_cxx.hpp) and Clouds' native rate
// (clouds/clouds.cc, and scattered through resonestor.h/reverb.h -- see
// PROVENANCE.md for why that isn't a single constant we could just read).
constexpr float kDelugeSampleRate = 44100.0f;
constexpr float kCloudsSampleRate = 32000.0f;
// The one ratio both resampling stages need. Per Deluge-side (44.1 kHz)
// sample, the Clouds-side (32 kHz) timeline advances by this much -- used
// directly as the phase-accumulator step in both directions, just against
// an input-driven loop in stage 1 and an output-driven loop in stage 3.
constexpr float kStep = kCloudsSampleRate / kDelugeSampleRate;

// Sized generously relative to one native Clouds block (32 frames) so a
// process() call of any Deluge-side block size the firmware actually uses
// (SSI_TX_BUFFER_NUM_SAMPLES = 128, or smaller) can always find enough
// buffered frames without the ring ever needing to wrap mid-block.
constexpr size_t kRingCapacity = 256;
} // namespace

/// Tiny single-producer/single-consumer float-frame ring, used on both sides
/// of the resampler so process() can be called with any block size without
/// losing fractional frames between calls. Not upstream Clouds code -- this
/// is Deluge-adapter-only plumbing, same spirit as the rest of this file.
namespace {
struct FrameRing {
	clouds::FloatFrame buf[kRingCapacity];
	size_t writeIdx = 0;
	size_t readIdx = 0;
	size_t count = 0;

	void push(clouds::FloatFrame f) {
		if (count >= kRingCapacity) {
			return; // Drop rather than overrun; should not happen at kRingCapacity.
		}
		buf[writeIdx] = f;
		writeIdx = (writeIdx + 1) % kRingCapacity;
		count++;
	}

	clouds::FloatFrame pop() {
		clouds::FloatFrame f = buf[readIdx];
		readIdx = (readIdx + 1) % kRingCapacity;
		count--;
		return f;
	}
};
} // namespace

// Held as static rings rather than CloudsAdapter members so the header
// doesn't need to know FrameRing's layout (it's an implementation detail,
// same reasoning as PlaitsVoice hiding plaits::Voice behind a pointer). One
// CloudsAdapter instance only for now, so file-scope statics are fine; if a
// second instance is ever needed these move onto the class.
namespace {
FrameRing g_downsampled; // 32 kHz domain, waiting to be fed to the processor
FrameRing g_upsampled;   // 44.1 kHz domain, waiting to be drained by callers
} // namespace

CloudsAdapter::CloudsAdapter() {
	largeBuffer_ = new (std::nothrow) uint8_t[kLargeBufferBytes];
	smallBuffer_ = new (std::nothrow) uint8_t[kSmallBufferBytes];
	processor_ = new (std::nothrow) clouds::GranularProcessor();

	if (largeBuffer_ == nullptr || smallBuffer_ == nullptr || processor_ == nullptr) {
		delete[] largeBuffer_;
		delete[] smallBuffer_;
		delete processor_;
		largeBuffer_ = nullptr;
		smallBuffer_ = nullptr;
		processor_ = nullptr;
		return;
	}

	processor_->Init(largeBuffer_, kLargeBufferBytes, smallBuffer_, kSmallBufferBytes);
	processor_->set_playback_mode(clouds::PLAYBACK_MODE_GRANULAR);
}

CloudsAdapter::~CloudsAdapter() {
	delete processor_;
	delete[] largeBuffer_;
	delete[] smallBuffer_;
}

void CloudsAdapter::process(const float* input, float* output, int32_t numSamples) {
	if (!isValid()) {
		// Pass through unprocessed rather than silence -- easier to notice a
		// missing allocation while testing than silent output that could be
		// mistaken for a legitimate freeze/dry_wet=0 state.
		std::memcpy(output, input, sizeof(float) * 2 * numSamples);
		return;
	}

	// --- Stage 1: resample input from 44.1 kHz down into the 32 kHz ring. --
	// Input-driven: one accumulator step per incoming (44.1 kHz) sample.
	// downsamplePhase_ crosses 1.0 on ~kStep of every input sample (kStep is
	// < 1, since the output side runs slower), which is exactly the emission
	// rate a 44.1 -> 32 kHz conversion needs. `t` places the emitted frame at
	// its true fractional position between prevIn and the current input
	// sample, not at the input sample itself.
	//
	// See PROVENANCE.md -- this is the one piece of this port that is NOT
	// upstream-pristine, because it isn't upstream code at all; it exists
	// only to feed upstream's untouched DSP the sample rate it was written
	// for, and it is linear interpolation, not the polyphase resampler
	// dsp/interpolation/ provides elsewhere in this codebase.
	static clouds::FloatFrame prevIn{0.0f, 0.0f};
	for (int32_t i = 0; i < numSamples; ++i) {
		clouds::FloatFrame in{input[2 * i], input[2 * i + 1]};
		downsamplePhase_ += kStep;
		if (downsamplePhase_ >= 1.0f) {
			downsamplePhase_ -= 1.0f;
			// downsamplePhase_ is now how far *past* the crossing we are, in
			// units of kStep; 1 - that is how far *into* [prevIn, in] the
			// crossing landed.
			float t = 1.0f - downsamplePhase_;
			g_downsampled.push({prevIn.l + (in.l - prevIn.l) * t, prevIn.r + (in.r - prevIn.r) * t});
		}
		prevIn = in;
	}

	// --- Stage 2: run whole native blocks through the vendored processor. --
	processor_->Prepare();
	while (g_downsampled.count >= clouds::kMaxBlockSize) {
		clouds::ShortFrame shortIn[clouds::kMaxBlockSize];
		clouds::ShortFrame shortOut[clouds::kMaxBlockSize];
		for (size_t i = 0; i < clouds::kMaxBlockSize; ++i) {
			clouds::FloatFrame f = g_downsampled.pop();
			shortIn[i].l = static_cast<int16_t>(std::clamp(f.l, -1.0f, 1.0f) * 32767.0f);
			shortIn[i].r = static_cast<int16_t>(std::clamp(f.r, -1.0f, 1.0f) * 32767.0f);
		}

		processor_->Process(shortIn, shortOut, clouds::kMaxBlockSize);

		for (size_t i = 0; i < clouds::kMaxBlockSize; ++i) {
			g_upsampled.push(
			    {static_cast<float>(shortOut[i].l) / 32768.0f, static_cast<float>(shortOut[i].r) / 32768.0f});
		}
	}

	// --- Stage 3: resample the 32 kHz result back up to 44.1 kHz. ----------
	// Output-driven, mirror image of stage 1: one accumulator step per
	// outgoing (44.1 kHz) sample, consuming a new 32 kHz-domain frame from
	// the ring each time accumulated phase crosses 1.0 (which happens on
	// ~kStep of every output sample -- fewer than one consume per output on
	// average, correct for going from the slower to the faster rate).
	static clouds::FloatFrame prevOut{0.0f, 0.0f};
	static clouds::FloatFrame curOut{0.0f, 0.0f};
	for (int32_t i = 0; i < numSamples; ++i) {
		upsamplePhase_ += kStep;
		while (upsamplePhase_ >= 1.0f) {
			upsamplePhase_ -= 1.0f;
			prevOut = curOut;
			if (g_upsampled.count > 0) {
				curOut = g_upsampled.pop();
			}
			// Else: ring ran dry (only expected while the first block is
			// still filling) -- hold curOut rather than read garbage.
		}
		output[2 * i] = prevOut.l + (curOut.l - prevOut.l) * upsamplePhase_;
		output[2 * i + 1] = prevOut.r + (curOut.r - prevOut.r) * upsamplePhase_;
	}
}
