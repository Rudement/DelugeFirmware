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
// Mutable Instruments Plaits <-> Deluge adapter.
//
// THIS IS THE ONLY FILE THAT KNOWS ABOUT BOTH WORLDS. Everything under
// dsp/plaits/ is upstream and knows nothing about the Deluge; everything in
// model/, processing/ and gui/ talks to Plaits only through here.
//
// That split is deliberate and load-bearing: it is what makes the 1.2.1
// backport a copy of dsp/plaits/ plus a re-apply of this header, rather than a
// merge. Do not let Deluge types leak into dsp/plaits/, and do not let Plaits
// types leak past this header into the engine.
//
// Modelled throughout on dsp/dx/engine.h and the OscType::DX7 paths, which are
// the same shape of change and are already proven on hardware.
// ============================================================================

#pragma once

#include "definitions_cxx.hpp"
#include <cstdint>

namespace plaits {
class Voice;
}

/// One Plaits voice. Owned by a VoiceUnisonPartSource, exactly as DxVoice is.
///
/// PHASE 1 NOTE: this wraps the whole upstream plaits::Voice, which carries all
/// 24 engines plus a 16 KB shared scratch pool -- about 25 KB per simultaneous
/// unison part. That is faithful to the module and fine for measuring, but it
/// is NOT the shipping shape. Phase 2 replaces it with per-engine allocation:
/// 16 of the 24 engines need under 600 bytes of scratch, so the common case
/// drops from ~25 KB to under 1 KB. See PLAITS-PLAN.md §Phase 2.
class PlaitsVoice {
public:
	PlaitsVoice();
	~PlaitsVoice();

	/// Note-on. Resets engine state and latches the note.
	void init(int32_t noteCode, uint8_t velocity);

	/// Note-off. Drops Plaits' gate. Matters for the eight self-enveloped
	/// engines, and for anything using the LPG; for a bypassed-LPG,
	/// non-self-enveloped engine it is a no-op and the Deluge's envelopes do all
	/// the amplitude work.
	void keyup();

	/// True for the eight engines whose post_processing_settings declare
	/// already_enveloped: the three six-op FM banks, Inharmonic String, Modal
	/// Resonator, and the three drums. Read straight off upstream's
	/// RegisterInstance() calls in plaits/dsp/voice.cc -- if that list ever
	/// changes, this must change with it.
	static bool engineIsSelfEnveloped(uint8_t engine);

	/// Render `numSamples` into `buffer` as q31.
	///
	/// `phaseIncrement` is the Deluge's own pitch representation, converted
	/// here rather than at the call site so that the conversion lives with the
	/// rest of the Plaits-facing arithmetic.
	///
	/// `harmonics`, `timbre` and `morph` are q31 (0 .. 2^31-1) and map to
	/// Plaits' 0.0 .. 1.0. Returns false when the voice has finished and can be
	/// unassigned, matching DxVoice::compute's contract.
	bool compute(int32_t* buffer, int32_t numSamples, uint32_t phaseIncrement, int32_t harmonics, int32_t timbre,
	             int32_t morph);

	/// Take the engine's AUX output rather than its main one. Both are rendered
	/// either way -- Plaits computes them together -- so this costs nothing but
	/// the choice of which buffer to read.
	bool useAux = false;

	/// Which of the 24 engines this voice renders. Read off the Source at
	/// note-on and refreshed each render block, so changing model mid-note
	/// takes effect immediately. kPlaitsNumEngines / kPlaitsDefaultEngine live
	/// in definitions_cxx.hpp so the menu, the Source and the clamp all agree.
	uint8_t engineIndex = kPlaitsDefaultEngine;

	/// Engage Plaits' low-pass gate. Mirrors Source::plaitsLpg and, like
	/// engineIndex, is refreshed every render block so a held note responds.
	///
	/// This is the one flag that decides whether the port sounds like the
	/// module or like a bare oscillator. See compute() for the whole argument.
	bool lpg = false;

	/// patch.decay, 0.0 .. 1.0. Drives the LPG's tail when `lpg` is set -- and
	/// nothing else that can be heard here.
	///
	/// It does NOT reach the eight self-enveloped engines, despite what an
	/// earlier version of this comment claimed. patch.decay is not a member of
	/// EngineParameters, so no engine is ever handed it. Voice::Render uses it
	/// for the LPG envelope (bypassed for those eight) and for decay_envelope_,
	/// which only reaches pitch/timbre/morph via the three
	/// *_modulation_amount attenuverters -- all held at 0 in compute(). Their
	/// decay comes from MORPH (drums, Inharmonic String, Modal Resonator) or
	/// from the loaded patch's own envelopes (the six-op FM banks). Verified by
	/// ear on hardware, 2026-08.
	float decay = 0.5f;

	/// patch.lpg_colour, 0.0 .. 1.0. VCA at one end, VCF at the other. Only read
	/// when `lpg` is set.
	float lpgColour = 0.5f;

	/// False when construction ran out of RAM. Callers must check; running out
	/// is a normal outcome here, not a bug -- see solicitPlaitsVoice().
	[[nodiscard]] bool isValid() const { return voice_ != nullptr; }

private:
	plaits::Voice* voice_ = nullptr;

	/// Plaits' shared per-voice scratch pool. plaits::Voice holds raw pointers
	/// into this, so it must outlive the voice and must not be reused.
	void* scratch_ = nullptr;

	/// Plaits renders in blocks of 12 (plaits::kBlockSize) and the Deluge asks
	/// for up to 128 (SSI_TX_BUFFER_NUM_SAMPLES), which 12 does not divide.
	/// Same wall the DX7 port hit; same answer -- render whole engine-blocks
	/// into an oversized scratch and copy out what was asked for.
	/// 132 = 11 * 12 >= 128, which is exactly why DX_MAX_N is 132.
	static constexpr int32_t kScratchSamples = 132;

	float note_ = 48.0f;
	float velocity_ = 1.0f;
	bool active_ = false;
	/// Plaits' gate. Held high while the note is on; Voice::Render finds the
	/// rising edge itself via its own trigger delay line, so there is nothing to
	/// edge-detect here.
	bool gate_ = false;

	/// Set by init() when the gate was ALREADY high -- i.e. this PlaitsVoice is
	/// being reused for a new note without an intervening keyup(), which
	/// voice_unison_part_source.cpp explicitly allows ("we might actually
	/// already have one, and just be restarting this voice").
	///
	/// Upstream only re-pings the LPG on a RISING edge of the trigger. Without
	/// this, a restarted note would find trigger_state_ already true, produce no
	/// edge, and play with no attack at all -- silent in the worst case. One
	/// render block of forced-low trigger manufactures the edge; kTriggerDelay
	/// is 5 samples and a block is 12, so one block is comfortably enough.
	bool retriggerPending_ = false;
};

/// Global Plaits state holder: the LUTs that every voice shares, plus the voice
/// pool. Allocated lazily on first use via allocMaxSpeed, exactly like
/// DxEngine, so a firmware that never selects Plaits never pays for it.
class PlaitsEngine {
public:
	PlaitsEngine();

	PlaitsVoice* solicitPlaitsVoice();
	void plaitsVoiceUnassigned(PlaitsVoice* plaitsVoice);
};

extern PlaitsEngine* plaitsEngine;

/// Allocates the engine on first call. May return nullptr if RAM is exhausted;
/// every caller must handle that, as the DX7 paths already do.
PlaitsEngine* getPlaitsEngine();
