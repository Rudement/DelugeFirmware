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
// let clouds:: types leak past this header.
//
// UNLIKE plaits_adapter.h, this is a standalone scaffold, not wired into the
// synth engine yet. Clouds is an effect (Mutable's own module is "Texture
// synthesizer", closer in shape to a granular/looper effect than an
// oscillator), so it does not fit the Source/Voice pattern PlaitsVoice uses.
// See dsp/granular/GranularProcessor for the existing precedent this should
// eventually follow instead, and src/deluge/dsp/clouds/PROVENANCE.md
// ("Not done in this pass") for the full list of what's left: effect-chain
// wiring, menu items/params/preset save-load, and swapping the resampler
// below for the project's polyphase one.
//
// What this file does today: owns one clouds::GranularProcessor, allocates
// its two working buffers, and resamples between the Deluge's 44.1 kHz and
// Clouds' native 32 kHz so the vendored DSP can run completely unmodified.
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>

namespace clouds {
class GranularProcessor;
} // namespace clouds

/// One Clouds granular processor instance.
///
/// Owns its own copy of the two buffers `clouds::GranularProcessor::Init()`
/// asks for (~180 KB total, sized to match upstream's clouds.cc exactly --
/// see PROVENANCE.md). Heap-allocated with plain `new[]` for now; routing
/// this through GeneralMemoryAllocator/Stealable so it comes out of the
/// stealable SDRAM pool instead of the tight internal-RAM heap is follow-up
/// work, not done here.
class CloudsAdapter {
public:
	CloudsAdapter();
	~CloudsAdapter();

	/// False if either working buffer, or the upstream processor itself,
	/// failed to allocate. Callers must check before calling process(),
	/// exactly as PlaitsVoice::isValid() must be checked.
	[[nodiscard]] bool isValid() const { return processor_ != nullptr && largeBuffer_ != nullptr && smallBuffer_ != nullptr; }

	/// Render `numSamples` of stereo audio at the Deluge's kSampleRate
	/// (44100), reading from `input` and writing to `output`. Internally:
	/// resample down to Clouds' native 32 kHz, run whole
	/// clouds::kMaxBlockSize (32-sample) blocks through the upstream
	/// processor, resample back up.
	///
	/// `input`/`output` are interleaved stereo float pairs, `numSamples`
	/// frames long (i.e. `2 * numSamples` floats). No-op (silence passed
	/// through unprocessed... actually copied through, not silenced) if
	/// !isValid().
	void process(const float* input, float* output, int32_t numSamples);

	/// Direct access to the upstream parameter block (position, size, pitch,
	/// density, texture, dry_wet, ...). Deliberately not wrapped: there is no
	/// Deluge param mapping yet (see file header), so there is nothing to
	/// adapt. Returns nullptr if !isValid().
	clouds::GranularProcessor* processor() { return processor_; }

private:
	clouds::GranularProcessor* processor_ = nullptr;
	uint8_t* largeBuffer_ = nullptr;
	uint8_t* smallBuffer_ = nullptr;

	// Matches clouds/clouds.cc's own block_mem/block_ccm sizes exactly, so
	// this is the same working set the module itself runs with. See
	// PROVENANCE.md's "Build notes" for the SDRAM-vs-internal-RAM context.
	static constexpr size_t kLargeBufferBytes = 118784;
	static constexpr size_t kSmallBufferBytes = 65536 - 128;

	/// Resampler state. Linear interpolation only -- functionally correct,
	/// not the quality the rest of this codebase's dsp/interpolation/
	/// provides elsewhere. See PROVENANCE.md point 3 under "Not done in this
	/// pass".
	float downsamplePhase_ = 0.0f;
	float upsamplePhase_ = 0.0f;
};
