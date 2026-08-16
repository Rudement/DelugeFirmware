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
#include <cstring>
#include <new>

PlaitsEngine* plaitsEngine = nullptr;

namespace {

/// Plaits shares one scratch pool between all of a Voice's engines. Upstream
/// sizes it at 16 KB because that is what its most demanding engine (Particle,
/// with its diffuser) asks for.
///
/// PHASE 1: one of these per simultaneous unison part, which is the expensive
/// part of the ~25 KB-per-voice figure in PLAITS-FEASIBILITY.md §4. Phase 2
/// sizes this per selected engine instead -- 16 of the 24 engines need under
/// 600 bytes, so the common case collapses to well under 1 KB. Do not "fix"
/// this number in isolation; it comes down when the allocation strategy
/// changes, not before.
constexpr size_t kPlaitsScratchBytes = 16384;

/// Deluge phaseIncrement -> Plaits note number.
///
/// The Deluge advances a 32-bit phase accumulator by phaseIncrement each
/// sample, so the normalised frequency (cycles per sample) is
/// phaseIncrement / 2^32.
///
/// Plaits asks for a MIDI note number and converts it back with
/// NoteToFrequency() in plaits/dsp/engine/engine.h:
///
///     f0 = a0 * 0.25 * 2^((note - 9) / 12),  a0 = (440/8) / kSampleRate
///
/// so f0 at note 9 is 13.75/44100 -- i.e. note 9 is A-1 at 13.75 Hz, which
/// checks out. Inverting:
///
///     note = 9 + 12 * log2( (phaseIncrement / 2^32) / (13.75 / 44100) )
///
/// VERIFY THIS BEFORE TRUSTING ANYTHING ELSE. A440 played on Plaits must beat
/// against A440 on a Deluge saw with no drift. Everything downstream of a wrong
/// constant here sounds like a broken engine rather than a tuning bug.
constexpr float kPhaseIncrementToNormalisedFreq = 1.0f / 4294967296.0f; // 2^-32
constexpr float kPlaitsFreqAtNote9 = (440.0f / 8.0f) * 0.25f / 44100.0f;

inline float phaseIncrementToNote(uint32_t phaseIncrement) {
	if (phaseIncrement == 0) {
		return 0.0f;
	}
	const float normalised = static_cast<float>(phaseIncrement) * kPhaseIncrementToNormalisedFreq;
	return 9.0f + 12.0f * log2f(normalised / kPlaitsFreqAtNote9);
}

/// Deluge hybrid patched param -> Plaits' 0.0 .. 1.0.
///
/// DERIVED, NOT GUESSED -- this is the number most likely to be quietly wrong,
/// so here is the whole chain:
///
///   * Harmonics/Timbre/Morph ride on LOCAL_OSC_A_PHASE_WIDTH,
///     LOCAL_OSC_A_WAVE_INDEX and LOCAL_OSC_B_PHASE_WIDTH. All three sit in
///     the FIRST_LOCAL__HYBRID block of the params enum, so patcher.cpp routes
///     them through getFinalParameterValueHybrid().
///   * getParamNeutralValue() has no case for any of them, so their neutral
///     value is the default: 0.
///   * getFinalParameterValueHybrid(0, patched) = signed_saturate<29>(patched >> 1) << 2,
///     i.e. simply patched / 2, clamped to +-2^30.
///   * The menus are half-precision items: knob 0..50 maps to patched
///     0..INT32_MAX (see computeFinalValueForHalfPrecisionMenuItem).
///
/// So the final value sweeps 0 .. 2^30 across full knob travel, and the divisor
/// is 2^30 -- NOT 2^31, which would strand the top half of every control.
///
/// Modulation can push these negative; Plaits CONSTRAINs its inputs, so a sign
/// error is SILENT (the engine just sits at 0.0) rather than loud. Hence the
/// explicit clamp and the named function rather than an inline multiply.
inline float hybridParamToUnit(int32_t value) {
	float f = static_cast<float>(value) * (1.0f / 1073741824.0f);
	if (f < 0.0f) {
		f = 0.0f;
	}
	if (f > 1.0f) {
		f = 1.0f;
	}
	return f;
}

} // namespace

PlaitsVoice::PlaitsVoice() {
	void* voiceMem = allocMaxSpeed(sizeof(plaits::Voice));
	if (voiceMem == nullptr) {
		return;
	}
	void* scratchMem = allocMaxSpeed(kPlaitsScratchBytes);
	if (scratchMem == nullptr) {
		delugeDealloc(voiceMem);
		return;
	}

	voice_ = new (voiceMem) plaits::Voice();
	stmlib::BufferAllocator allocator(scratchMem, kPlaitsScratchBytes);
	voice_->Init(&allocator);
	// The scratch block is owned for the lifetime of the voice; plaits::Voice
	// keeps raw pointers into it. Deliberately not freed until the destructor.
	scratch_ = scratchMem;
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

bool PlaitsVoice::engineIsSelfEnveloped(uint8_t engine) {
	switch (engine) {
	case 2:  // six-op FM, bank A
	case 3:  // six-op FM, bank B
	case 4:  // six-op FM, bank C
	case 19: // Inharmonic String
	case 20: // Modal Resonator
	case 21: // Analog Bass Drum
	case 22: // Analog Snare Drum
	case 23: // Analog Hi-Hat
		return true;
	default:
		return false;
	}
}

void PlaitsVoice::init(int32_t noteCode, uint8_t velocity) {
	note_ = static_cast<float>(noteCode);
	velocity_ = static_cast<float>(velocity) * (1.0f / 127.0f);
	active_ = (voice_ != nullptr);
	gate_ = true;
}

void PlaitsVoice::keyup() {
	// Only meaningful for the self-enveloped engines; harmless otherwise, since
	// compute() ignores the gate unless trigger_patched is set.
	gate_ = false;
}

bool PlaitsVoice::compute(int32_t* buffer, int32_t numSamples, uint32_t phaseIncrement, int32_t harmonics,
                          int32_t timbre, int32_t morph) {
	if (voice_ == nullptr || !active_) {
		return false;
	}

	plaits::Patch patch{};
	patch.engine = engineIndex < kPlaitsNumEngines ? engineIndex : kPlaitsDefaultEngine;
	patch.note = 0.0f; // note travels in modulations.note; see below
	patch.harmonics = hybridParamToUnit(harmonics);
	patch.timbre = hybridParamToUnit(timbre);
	patch.morph = hybridParamToUnit(morph);
	patch.frequency_modulation_amount = 0.0f;
	patch.timbre_modulation_amount = 0.0f;
	patch.morph_modulation_amount = 0.0f;
	patch.decay = 0.5f;
	patch.lpg_colour = 0.5f;

	plaits::Modulations modulations{};
	modulations.note = phaseIncrementToNote(phaseIncrement);
	modulations.engine = 0.0f;
	modulations.level = 1.0f;

	// ------------------------------------------------------------------
	// THE FLAG COMBINATION THAT MATTERS. Read plaits/dsp/voice.cc::Render
	// before changing either of these.
	//
	//   use_internal_envelope = modulations.trigger_patched
	//   lpg_bypass            = already_enveloped
	//                           || (!level_patched && !trigger_patched)
	//
	// The Deluge already has envelopes, a filter and its own amplitude
	// handling. We want Plaits' raw oscillator, not its decay envelope and
	// not its low-pass gate. BOTH flags false is what turns both off.
	//
	// Get this wrong and every single patch sounds plucked, on every engine,
	// which reads as "the port is broken" rather than "one bool is wrong".
	// ------------------------------------------------------------------
	//
	// THE EXCEPTION, and it is upstream's own rule rather than a guess. Eight
	// engines declare already_enveloped in their post_processing_settings: the
	// three six-op FM banks, Inharmonic String, Modal Resonator and the three
	// drums. For those, Voice::Render forces lpg_bypass true REGARDLESS of the
	// flags -- so patching the trigger there enables their internal envelopes
	// without reintroducing the low-pass gate this bypass exists to avoid.
	//
	// It is not optional for the drums. AnalogBassDrum reads an unpatched
	// trigger as `sustain` and takes its level from accent * decay, where decay
	// is MORPH -- which defaults to 0 on the Deluge because the param it
	// borrows does. Result: a bass drum with nothing to strike it and zero
	// sustain gain, i.e. silence. Snare and hi-hat are the same shape.
	const bool selfEnveloped = engineIsSelfEnveloped(patch.engine);
	modulations.trigger_patched = selfEnveloped;
	modulations.trigger = (selfEnveloped && gate_) ? 1.0f : 0.0f;
	modulations.level_patched = false;
	modulations.frequency_patched = false;
	modulations.timbre_patched = false;
	modulations.morph_patched = false;

	// Plaits renders in blocks of 12 and its internal buffers are sized
	// kMaxBlockSize (24), so it must be driven in whole engine-blocks. 128 is
	// not a multiple of 12, hence the oversized scratch and the copy-out --
	// the same shape as DX_MAX_N = 132 = 11 * 12 in the DX7 port.
	// static, not stack: mirrors the DX7 block's `static int32_t uniBuf[DX_MAX_N]`.
	// The audio render path is single-threaded and non-reentrant, and 528 bytes
	// of stack in the render loop is not free.
	static plaits::Voice::Frame frames[kScratchSamples];
	int32_t rendered = 0;
	while (rendered < numSamples) {
		const size_t block = plaits::kBlockSize;
		voice_->Render(patch, modulations, &frames[rendered], block);
		rendered += static_cast<int32_t>(block);
	}

	// Scaling and polarity.
	//
	// Plaits' ChannelPostProcessor writes int16 with post_gain deliberately
	// NEGATIVE (its DAC inverts, and inverts back in hardware). We are not
	// that DAC, so negate to get the polarity the rest of the Deluge expects.
	//
	// << 9 puts the int16 at roughly +-2^24, which is the same scale the DX7
	// engine's uniBuf uses -- so the caller in voice.cpp can reuse the DX7
	// block's `multiply_32x32_rshift32(x, amp) << 6` amplitude convention
	// verbatim instead of inventing a second one. If Plaits comes out
	// noticeably louder or quieter than DX7 on first listen, this shift is the
	// place to look, and it should move in whole bits.
	for (int32_t i = 0; i < numSamples; i++) {
		const int16_t sample = useAux ? frames[i].aux : frames[i].out;
		// Multiply rather than shift: left-shifting a negative value is not
		// something to rely on, and the compiler emits the same instruction.
		buffer[i] = -static_cast<int32_t>(sample) * 512;
	}

	return true;
}

PlaitsEngine::PlaitsEngine() = default;

PlaitsVoice* PlaitsEngine::solicitPlaitsVoice() {
	void* memory = allocMaxSpeed(sizeof(PlaitsVoice));
	if (memory == nullptr) {
		return nullptr;
	}
	PlaitsVoice* v = new (memory) PlaitsVoice();
	if (!v->isValid()) {
		// Ran out of RAM part-way through construction. Returning nullptr is a
		// supported outcome -- noteOn() already handles it, exactly as it does
		// for DxEngine::solicitDxVoice().
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
	if (plaitsEngine == nullptr) {
		void* engineMem = allocMaxSpeed(sizeof(PlaitsEngine) + alignof(PlaitsEngine) - 1);
		if (engineMem == nullptr) {
			return nullptr;
		}
		engineMem = (void*)((((intptr_t)engineMem) + alignof(PlaitsEngine) - 1) & -alignof(PlaitsEngine));
		plaitsEngine = new (engineMem) PlaitsEngine();
	}
	return plaitsEngine;
}
