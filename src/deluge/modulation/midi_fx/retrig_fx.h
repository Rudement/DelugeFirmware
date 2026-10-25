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

#pragma once

#include "modulation/arpeggiator.h"
#include "modulation/midi_fx/midi_fx.h"
#include <array>
#include <cstddef>
#include <cstdint>

/// Retrigger — a drum hit repeats itself N times at a musical division, each hit quieter.
///
/// Elektron retrig / RS7000 territory, and the reason MIDI FX exists on Kits at all.
///
/// **Free-running, not synced.** It advances on the audio block clock in `render()`, using a
/// phase increment derived from tempo the same way `ArpeggiatorSettings::getPhaseIncrement`
/// does — so hitting a pad with the transport stopped still rolls. That was the explicit
/// requirement, and it is why this does not use the tick axis that Gate Time uses.
///
/// **The first effect that emits note-ONs**, and that changes what it needs. A note-off only
/// wants a note code and a channel, which Gate Time stores by value. A note-on has to hand the
/// dispatcher an `ArpNote*` — velocity, note code, per-slot status — and the arpeggiator's own
/// records are deleted the moment the key is released. So this owns its records outright. That
/// is `docs/MIDI-FX-DESIGN.md` §3 option B, scoped down to what a retrigger actually needs: no
/// MPE member-channel survey, because drums do not send MPE.
namespace deluge::modulation::midi_fx {

class RetrigFx final : public MidiFx {
public:
	/// Arms a retrigger when a note starts. Never swallows.
	bool onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction) override;

	/// Advances the free-running clock and emits repeats as they come due.
	void render(const MidiFxContext& context, int32_t numSamples, ArpReturnInstruction& instruction) override;

	/// Drops everything armed, emitting a note-off for each. Unlike Gate Time's shortened notes,
	/// these are notes only this effect knows about: the transport's own stop path walks the
	/// arpeggiator's records and the note rows, and a hit this effect generated is in neither.
	bool flush(const MidiFxContext& context, ArpReturnInstruction& instruction) override;

private:
	/// One note repeating. `note` is owned here, not a pointer into `arpeggiator.notes` — that
	/// array deletes its elements on release, which is exactly when a repeat is still going.
	struct Voice {
		ArpNote note;
		uint8_t repeatsLeft;
		uint8_t velocity;
		bool active;
	};

	/// A drum roll is one note repeating; a few at once covers a flam across pads. Deliberately
	/// small — each entry carries a whole ArpNote.
	static constexpr size_t kMaxVoices = 4;

	std::array<Voice, kMaxVoices> voices_{};

	/// Free-running phase. Wrapping past 2^32 is one division elapsed — the same trick the
	/// arpeggiator's gate uses.
	uint32_t phase_{0};

	[[nodiscard]] bool anyActive() const;

	/// Whether that pointer is one of this effect's own records.
	///
	/// The note-event axis runs immediately after the time axis on the same instruction, so a
	/// repeat this effect just emitted arrives at its own onNoteEvent one call later. Arming from
	/// it would start a roll off a roll, filling every voice from one pad hit.
	[[nodiscard]] bool ownsRecord(const ArpNote* note) const;
};

} // namespace deluge::modulation::midi_fx
