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

#include "dsp/plaits_adapter.h"

#include "memory/memory_allocator_interface.h"
#include "plaits/dsp/dsp.h"
#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"

#include <cmath>
#include <new>

static_assert(plaits::kBlockSize == 12, "PlaitsVoice::kBlock must equal plaits::kBlockSize");

namespace {
PlaitsEngine plaitsEngineInstance; // stateless; nothing to allocate
} // namespace
PlaitsEngine* plaitsEngine = &plaitsEngineInstance;

namespace {

/// Plaits' shared engine RAM. Upstream's size; measured: Particle Noise's
/// diffuser uses exactly all 16384 bytes, so it cannot shrink.
constexpr size_t kScratchBytes = 16384;

/// Deluge phaseIncrement -> MIDI-style note number for Plaits.
///
/// A Deluge oscillator advances a 32-bit phase by phaseIncrement per sample, so
/// f = phaseIncrement / 2^32 * 44100. Plaits converts notes back with
/// NoteToFrequency(): f/SR = 13.75/SR * 2^((note - 9) / 12) (note 9 = A-1 =
/// 13.75 Hz; kSampleRate is set to 44100 in dsp.h). Solving for note:
///     note = 9 + 12 * log2(phaseIncrement * 44100 / (2^32 * 13.75))
/// Verified natively: 440 Hz in -> 440.0 Hz out on every pitched engine.
inline float phaseIncrementToNote(uint32_t phaseIncrement) {
	constexpr float kScale = 44100.0f / (4294967296.0f * 13.75f);
	return 9.0f + 12.0f * log2f(static_cast<float>(phaseIncrement) * kScale);
}

inline float clampUnit(float f) {
	return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

} // namespace

float PlaitsVoice::unitFromHalfPrecisionParam(int32_t finalValue) {
	return clampUnit(static_cast<float>(finalValue) * (1.0f / 1073741824.0f));
}

float PlaitsVoice::unitFromBipolarParam(int32_t finalValue) {
	return clampUnit((static_cast<float>(finalValue) + 1073741824.0f) * (1.0f / 2147483648.0f));
}

bool PlaitsVoice::engineIsSelfEnveloped(uint8_t engine) {
	// Must match the `true` flags in upstream's RegisterInstance() calls in
	// plaits/dsp/voice.cc.
	switch (engine) {
	case 2:  // six-op FM A
	case 3:  // six-op FM B
	case 4:  // six-op FM C
	case 19: // Inharmonic String
	case 20: // Modal Resonator
	case 21: // Bass Drum
	case 22: // Snare Drum
	case 23: // Hi-Hat
		return true;
	default:
		return false;
	}
}

PlaitsVoice::PlaitsVoice() {
	void* voiceMem = allocMaxSpeed(sizeof(plaits::Voice));
	if (voiceMem == nullptr) {
		return;
	}
	void* scratchMem = allocMaxSpeed(kScratchBytes);
	if (scratchMem == nullptr) {
		delugeDealloc(voiceMem);
		return;
	}
	voice_ = new (voiceMem) plaits::Voice();
	scratch_ = scratchMem;
	stmlib::BufferAllocator allocator(scratch_, kScratchBytes);
	voice_->Init(&allocator);
}

PlaitsVoice::~PlaitsVoice() {
	if (voice_ != nullptr) {
		voice_->~Voice();
		delugeDealloc(voice_);
		voice_ = nullptr;
	}
	if (scratch_ != nullptr) {
		delugeDealloc(scratch_);
		scratch_ = nullptr;
	}
}

void PlaitsVoice::init(int32_t noteCode, uint8_t velocity) {
	(void)noteCode; // pitch arrives every block via phaseIncrement, which includes transpose and cents
	velocity_ = static_cast<float>(velocity) * (1.0f / 127.0f);
	gate_ = true;
	primePending_ = true;
	// A restarted voice drops whatever was left of the old note's last block.
	pendingPos_ = kBlock;
}

void PlaitsVoice::keyup() {
	gate_ = false;
}

void PlaitsVoice::renderBlock(const PlaitsControls& c, float note, bool triggerHigh) {
	const uint8_t engine = c.engine < kPlaitsNumEngines ? c.engine : kPlaitsDefaultEngine;
	const bool selfEnveloped = engineIsSelfEnveloped(engine);

	plaits::Patch patch{};
	patch.engine = engine;
	patch.note = 0.0f; // the whole pitch travels in modulations.note
	patch.harmonics = c.harmonics;
	patch.timbre = c.timbre;
	patch.morph = c.morph;
	// Attenuverters at noon, as a freshly patched module. These are what keep
	// Plaits' internal decay envelope off pitch/timbre/morph when TRIG is patched.
	patch.frequency_modulation_amount = 0.0f;
	patch.timbre_modulation_amount = 0.0f;
	patch.morph_modulation_amount = 0.0f;
	patch.decay = c.decay;
	patch.lpg_colour = c.lpgColour;

	plaits::Modulations m{};
	m.note = note;
	m.engine = 0.0f;

	// TRIG is "patched" for the self-enveloped engines (their envelopes and the
	// drums' strike need it -- unpatched, the drums are silent) and for any
	// engine when the LPG is on (Voice::Render then pings the LPG on each
	// rising edge). Otherwise Plaits runs as a free oscillator and the Deluge's
	// envelopes do all the amplitude work.
	m.trigger_patched = selfEnveloped || c.lpg;
	m.trigger = (m.trigger_patched && triggerHigh) ? 1.0f : 0.0f;

	// LEVEL = velocity, but only where upstream bypasses the LPG anyway (the
	// self-enveloped engines), so it acts purely as ACCENT -- the drums' punch,
	// the strings' strike, DX7 velocity. 0.6 scaling puts velocity 100 at
	// accent ~0.8, which is what an unpatched module uses.
	m.level_patched = selfEnveloped;
	m.level = velocity_ * 0.6f;

	m.frequency_patched = false;
	m.timbre_patched = false;
	m.morph_patched = false;

	plaits::Voice::Frame frames[kBlock];
	voice_->Render(patch, m, frames, kBlock);

	// Plaits' post-processor writes int16 with a deliberately negative gain (the
	// module's DAC stage inverts it back), so negate. x512 puts it at about
	// +-2^24, the DX7 engine's scale.
	for (int32_t i = 0; i < kBlock; i++) {
		const int16_t s = c.aux ? frames[i].aux : frames[i].out;
		pending_[i] = -static_cast<int32_t>(s) * 512;
	}
}

void PlaitsVoice::compute(int32_t* buffer, int32_t numSamples, uint32_t phaseIncrement, const PlaitsControls& c) {
	if (voice_ == nullptr || numSamples <= 0) {
		return;
	}
	const float note = phaseIncrement ? phaseIncrementToNote(phaseIncrement) : 0.0f;

	if (primePending_) {
		// Silent block: settles pitch smoothing on the new note and, on a
		// restart, writes a low trigger so the next block is a rising edge.
		primePending_ = false;
		renderBlock(c, note, false);
		pendingPos_ = kBlock; // discard it
	}

	int32_t done = 0;
	while (done < numSamples) {
		if (pendingPos_ >= kBlock) {
			renderBlock(c, note, gate_);
			pendingPos_ = 0;
		}
		int32_t take = kBlock - pendingPos_;
		if (take > numSamples - done) {
			take = numSamples - done;
		}
		for (int32_t i = 0; i < take; i++) {
			buffer[done + i] = pending_[pendingPos_ + i];
		}
		done += take;
		pendingPos_ += take;
	}
}

PlaitsVoice* PlaitsEngine::solicitPlaitsVoice() {
	void* memory = allocMaxSpeed(sizeof(PlaitsVoice));
	if (memory == nullptr) {
		return nullptr;
	}
	auto* v = new (memory) PlaitsVoice();
	if (!v->isValid()) {
		v->~PlaitsVoice();
		delugeDealloc(memory);
		return nullptr;
	}
	return v;
}

void PlaitsEngine::plaitsVoiceUnassigned(PlaitsVoice* plaitsVoice) {
	if (plaitsVoice == nullptr) {
		return;
	}
	plaitsVoice->~PlaitsVoice();
	delugeDealloc(plaitsVoice);
}

PlaitsEngine* getPlaitsEngine() {
	return plaitsEngine;
}
