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

#include "modulation/midi_fx/midi_fx.h"
#include "modulation/midi_fx/note_pool.h"
#include <array>
#include <cstddef>
#include <cstdint>

/// MIDI Delay — a note repeats at a musical division, each repeat quieter and optionally
/// transposed. The RS7000's MIDI Delay, and the effect this whole feature was designed around.
///
/// **Synced, not free-running.** It runs on the tick axis, so the repeats sit on the clock rather
/// than drifting against it, and the division reuses `SyncLevel`/`SyncType` so triplet and dotted
/// come free. The consequence is stated plainly because a user will otherwise find it by
/// surprise: **with the transport stopped, nothing echoes.** Free-running delay is a second
/// timing implementation and a separate piece of work; Retrigger is the free-running effect.
///
/// **This is the first effect that needs the note pool**, and the reason the pool exists. An echo
/// outlives the key that started it — that is the entire point — so it cannot hold the
/// arpeggiator's `ArpNote`, which is deleted on release. See `note_pool.h`.
///
/// **It must not hear itself.** The note-event axis runs immediately after the time axis on the
/// same instruction, so a repeat this effect just emitted arrives at its own `onNoteEvent` in the
/// same pass. Arming a new echo from it would be a runaway. `NotePool::owns` is the guard.
namespace deluge::modulation::midi_fx {

class DelayFx final : public MidiFx {
public:
	/// Arms an echo when a note starts. Never swallows.
	bool onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction) override;

	/// Emits repeats and their note-offs as they come due, and reports when to be woken next.
	[[nodiscard]] int32_t tick(const MidiFxContext& context, uint32_t clipCurrentPos,
	                           ArpReturnInstruction& instruction) override;

	/// Releases everything, emitting note-offs for whatever is still sounding.
	bool flush(const MidiFxContext& context, ArpReturnInstruction& instruction) override;

private:
	/// One note and its tail. The record is re-used for every repeat of that note, which is why
	/// one pool entry per echo is enough: with the gate shorter than the division, only one repeat
	/// of a given note is sounding at any moment.
	struct Echo {
		/// Owned by the pool, not by the arpeggiator. Null when the slot is free.
		ArpNote* note{nullptr};
		int32_t ticksToNextOn{0};
		int32_t ticksToOff{0};
		int32_t divisionTicks{0};
		int32_t gateTicks{0};
		int16_t sourceNoteCode{ARP_NOTE_NONE};
		uint8_t repeatsLeft{0};
		uint8_t repeatIndex{0};
		uint8_t velocity{0};
		bool sounding{false};
		bool active{false};
	};

	/// A chord's worth of echoes. Each held note passes through `onNoteEvent` as its own `ArpNote`
	/// — the arpeggiator keeps one record per key — so four covers a four-note chord echoing at
	/// once. Beyond that the newest note goes undelayed rather than cutting an existing tail.
	static constexpr size_t kMaxEchoes = 4;

	/// Emits a note-off for `echo` into the first free slot. False when the instruction's four
	/// note-off slots are already spoken for, in which case the caller asks to be woken again.
	static bool emitNoteOff(Echo& echo, ArpReturnInstruction& instruction);

	void releaseEcho(const MidiFxContext& context, Echo& echo);

	std::array<Echo, kMaxEchoes> echoes_{};

	/// Clip position at the previous tick, for working out how much time passed. Same bookkeeping
	/// as GateTimeFx, and for the same reason: the tick axis reports where the clip is, not how
	/// long it has been since it last said so.
	uint32_t lastClipPos_{0};
	bool havePos_{false};
};

} // namespace deluge::modulation::midi_fx
