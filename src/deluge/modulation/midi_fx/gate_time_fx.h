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
#include <array>
#include <cstddef>
#include <cstdint>

/// Gate Time — a percentage of a sequenced note's written length, 5% to 400%.
///
/// The RS7000's control, and like that one it is a ratio in both directions. Two things about
/// this seam make it less simple than the number suggests:
///
/// - **Sequenced notes carry their length.** `NoteRow` passes `thisNote->getLength()` through as
///   `sampleSyncLength` (`note_row.cpp:2558`), which reaches the seam as
///   `ArpReturnInstruction::sampleSyncLengthOn`. In ticks, despite the name — `Voice` stores it
///   as `sequenceSyncLengthTicks` (`voice.cpp:251`). A percentage is well defined.
/// - **A held key does not.** It has no length until it is released. A percentage of an unknown
///   number is not a thing, so live notes pass through untouched in both directions. Anyone who
///   wants a fixed gate on live notes wants a different control with a different name.
///
/// **Shortening and lengthening are different mechanisms**, which is why lengthening arrived a
/// stage later:
///
/// - Under 100% the early note-off always lands while the note's own record is still alive, so it
///   needs identity but nothing else. The sequencer's real note-off still arrives at the written
///   end and is deliberately let through — a second note-off for an already-released note is
///   harmless on MIDI, CV and a Sound voice alike, while suppressing it would mean matching
///   note-offs to notes across retriggers, which is a good way to swallow the wrong one.
/// - Over 100% the note has to outlive its written end, so the sequencer's note-off is exactly
///   what must not get through. That one **is** swallowed, and this effect then owes the note a
///   note-off of its own. Every path out of that debt is covered: the timer, a rewind, a
///   retrigger of the same note, and flush.
///
/// It still needs no note pool. A pending note-off is a note code and a channel, kept by value;
/// only a generated note-**on** needs a record that outlives the arpeggiator's array.
///
/// **Off when the arpeggiator is on.** The arp has its own Gate control; two gates fighting over
/// one note is not a feature. Checked via `MidiFxContext::arpIsOn`.
namespace deluge::modulation::midi_fx {

class GateTimeFx final : public MidiFx {
public:
	/// Schedules the note-off for a sequenced note-on, and swallows the sequencer's own note-off
	/// for any note currently being held past its written end. Never swallows a note-on.
	bool onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction) override;

	/// Emits any note-offs that have come due and returns ticks until the next one.
	[[nodiscard]] int32_t tick(const MidiFxContext& context, uint32_t clipCurrentPos,
	                           ArpReturnInstruction& instruction) override;

	/// Releases anything held past its written end. Notes merely shortened are dropped rather than
	/// released — see the implementation.
	bool flush(const MidiFxContext& context, ArpReturnInstruction& instruction) override;

private:
	/// A note-off waiting for its tick.
	///
	/// Stored **by value**, not as a pointer into `arpeggiator.notes`. That array is a
	/// `ResizeableArray` which moves its elements when one is deleted, so a pointer into it
	/// dangles silently the moment any other note is released. Two `int16_t` and an `int32_t` is
	/// cheaper than being right about lifetimes.
	struct PendingOff {
		int32_t ticksRemaining;
		int16_t noteCode;
		uint8_t outputMemberChannel;
		/// True when this note is being held past its written end, and the sequencer's own
		/// note-off must therefore be suppressed. Also means this effect is now the only thing
		/// that will ever release the note — see flush().
		bool holdingPastWrittenEnd;
		bool active;
	};

	/// Deliberately generous: a chord of held notes plus a fast sequence. Overflow drops the
	/// *new* note's gate rather than an existing one's note-off — a note that stays full length
	/// is a wrong-sounding note, a note-off that never fires is a stuck note.
	static constexpr size_t kMaxPendingOffs = 16;

	/// Removes from `instruction` any note-off for a note this effect is holding past its written
	/// end. The dispatch loops stop at the first ARP_NOTE_NONE, so the array is compacted rather
	/// than holed.
	void swallowNoteOffsForHeldNotes(ArpReturnInstruction& instruction);

	std::array<PendingOff, kMaxPendingOffs> pending_{};

	uint32_t lastClipPos_{0};
	bool havePos_{false};
};

} // namespace deluge::modulation::midi_fx
