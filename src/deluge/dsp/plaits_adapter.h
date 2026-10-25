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
// Mutable Instruments Plaits <-> Deluge adapter (rewrite, 2026-10-20).
//
// The only file that knows about both worlds. dsp/plaits/ is upstream Mutable
// code and stays byte-for-byte untouched; the Deluge talks to it only through
// this header.
//
// What the rewrite fixes, in order of how much you could hear it:
//
//  1. Pitch and continuity. Plaits renders in fixed 12-sample blocks; the
//     Deluge asks for any count from 1 to 128. The first port rendered whole
//     blocks and threw away the overhang, so every call silently skipped up to
//     11 samples. Measured: +53 cents sharp at 128-sample calls, +2..+3
//     semitones at the small calls the audio routine makes most of the time,
//     plus a glitch at every call boundary, and every Plaits envelope ran fast.
//     Now the overhang is kept and played out first on the next call, so
//     Plaits sees an unbroken stream of 12-sample blocks, exactly as on the
//     module.
//
//  2. Timbre's lower half. Timbre rides on the wave-index param, which is a
//     full-range (bipolar) param. The old mapping clamped the negative half to
//     zero, so knob 0..25 all meant Timbre = 0. Now knob 0..50 is 0..1 and the
//     default (25) is 0.5 -- the module's noon.
//
//  3. Velocity. It was latched and never used. It now drives Plaits' ACCENT for
//     the engines that have one -- the three drums, Inharmonic String, Modal
//     Resonator and the six-op FM banks (where it is DX7 velocity) -- the same
//     job the LEVEL input does on the module.
//
//  4. The note-on chirp. Plaits smooths pitch by averaging this block's note
//     with the last one, and a fresh voice's "last note" is 0, so the first 12
//     samples of every note played about half the pitch. One silent priming
//     block at note-on settles it before anything is heard.
// ============================================================================

#pragma once

#include "definitions_cxx.hpp"
#include <cstdint>

namespace plaits {
class Voice;
} // namespace plaits

/// Everything that shapes a Plaits voice, read fresh from the Source and the
/// patched params every render call, so menu changes reach a held note.
struct PlaitsControls {
	uint8_t engine = kPlaitsDefaultEngine;
	bool aux = false;       ///< Take Plaits' AUX output instead of OUT.
	bool lpg = false;       ///< Strike Plaits' low-pass gate on each note.
	float decay = 0.5f;     ///< 0..1. LPG decay; envelope time of the self-enveloped engines.
	float lpgColour = 0.5f; ///< 0..1. VCA .. VCF. Only heard with the LPG on.
	float harmonics = 0.5f; ///< 0..1
	float timbre = 0.5f;    ///< 0..1
	float morph = 0.5f;     ///< 0..1
};

/// One Plaits voice. Owned by a VoiceUnisonPartSource, as DxVoice is.
class PlaitsVoice {
public:
	PlaitsVoice();
	~PlaitsVoice();
	PlaitsVoice(const PlaitsVoice&) = delete;
	PlaitsVoice& operator=(const PlaitsVoice&) = delete;

	/// Note-on. Also used when an existing voice is restarted for a new note
	/// without a note-off in between; that case still produces a fresh strike.
	void init(int32_t noteCode, uint8_t velocity);

	/// Note-off. Drops Plaits' gate (heard on the six-op FM release, and as the
	/// end of the gate for the LPG). Amplitude release is the Deluge's envelope.
	void keyup();

	/// Render `numSamples` (1..128) into `buffer`, q31 at about +-2^24 -- the
	/// same scale as the DX7 engine, so voice.cpp mixes it identically.
	/// `phaseIncrement` is the Deluge's pitch for this unison part.
	void compute(int32_t* buffer, int32_t numSamples, uint32_t phaseIncrement, const PlaitsControls& controls);

	/// The eight engines whose amplitude envelope is built in (upstream marks
	/// them already_enveloped): six-op FM A/B/C, Inharmonic String, Modal
	/// Resonator, Bass Drum, Snare Drum, Hi-Hat.
	static bool engineIsSelfEnveloped(uint8_t engine);

	/// Deluge final param value -> Plaits 0..1. Harmonics and Morph sit on the
	/// two pulse-width params (half-precision menus: final 0..2^30). Timbre sits
	/// on wave index (full-range menu: final -2^30..+2^30, centre = 0.5).
	static float unitFromHalfPrecisionParam(int32_t finalValue);
	static float unitFromBipolarParam(int32_t finalValue);

	/// False when construction ran out of RAM. Running out is a normal outcome
	/// here; the caller drops this unison part, as it does for the DX7.
	[[nodiscard]] bool isValid() const { return voice_ != nullptr; }

private:
	void renderBlock(const PlaitsControls& controls, float note, bool triggerHigh);

	plaits::Voice* voice_ = nullptr;
	void* scratch_ = nullptr; ///< Plaits' shared 16 KB engine RAM; voice_ points into it.

	/// One rendered 12-sample Plaits block, already converted to Deluge scale.
	/// Played out across as many compute() calls as it takes -- this is the fix
	/// for item 1 above.
	static constexpr int32_t kBlock = 12; // plaits::kBlockSize, checked in the .cpp
	int32_t pending_[kBlock] = {};
	int32_t pendingPos_ = kBlock; ///< == kBlock means empty.

	float velocity_ = 0.8f;
	bool gate_ = false;
	/// Set by init(): render one silent block before the next audible one. It
	/// settles Plaits' pitch smoothing and, for a restarted voice, holds the
	/// trigger low for a block so the new note is a real rising edge.
	bool primePending_ = false;
};

/// Voice factory. Exists so voice code reads the same as the DX7 paths.
class PlaitsEngine {
public:
	PlaitsVoice* solicitPlaitsVoice();
	void plaitsVoiceUnassigned(PlaitsVoice* plaitsVoice);
};

extern PlaitsEngine* plaitsEngine;

/// Returns the engine, creating it on first use. Never nullptr in practice, but
/// callers keep the DX7-style check.
PlaitsEngine* getPlaitsEngine();
