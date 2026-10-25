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
#include <array>
#include <cstddef>

/// The note pool: records for notes that outlive the key that produced them.
///
/// `docs/MIDI-FX-DESIGN.md` §3 option B. The problem it solves is stated there and is worth
/// restating here, because every future effect that generates notes runs into it:
///
/// `noteOnPostArp` takes an `ArpNote*` and writes back into it — `midi_instrument.cpp:940` and
/// `cv_instrument.cpp:36` both store the member channel they picked. So a generated note needs a
/// real `ArpNote` record, not a value copy made on the stack.
///
/// It cannot be a pointer into `arpeggiator.notes`. That array is a `ResizeableArray` which moves
/// its elements when one is deleted, and `Arpeggiator::noteOff` deletes the source element the
/// moment the key comes up (`arpeggiator.cpp:432`) — which for an echo is exactly when it is still
/// running. The pointer would dangle, and silently, because the memory stays mapped and holds a
/// different note.
///
/// So the pool owns its records outright. Fixed size, no allocation, and `owns()` is what lets an
/// effect tell its own generated notes apart from the ones it should be reacting to.
namespace deluge::modulation::midi_fx {

class NotePool {
public:
	/// One entry per echoing voice, not per repeat: an echo re-uses its record for each repeat,
	/// because with a gate shorter than the division only one repeat of a given note sounds at a
	/// time. Four echoing voices plus headroom for a second effect that generates notes.
	///
	/// Sized deliberately small. This lives on every MelodicInstrument and every Drum, whether or
	/// not MIDI FX is ever switched on, so each entry is paid for by every song. `ArpNote` is
	/// around forty bytes, which puts the pool in the hundreds of bytes rather than the kilobytes.
	static constexpr size_t kSize = 8;

	/// Copies `source` into a free record and hands it back, or returns nullptr when the pool is
	/// full. Full means the tail of an echo is dropped, which is quieter than expected; the
	/// alternative is stealing a record from a note that is currently sounding, which leaves that
	/// note on forever. Wrong-but-quiet beats wrong-and-stuck.
	ArpNote* acquire(const ArpNote& source) {
		for (Entry& e : entries_) {
			if (e.active) {
				continue;
			}
			e.note = source;
			// The dispatcher decides the member channel and writes it back here. Starting from
			// "none" keeps this record out of the MPE survey's count until it actually sounds.
			e.note.outputMemberChannel.fill(MIDI_CHANNEL_NONE);
			e.active = true;
			return &e.note;
		}
		return nullptr;
	}

	/// Returns a record to the pool. Safe with nullptr and with a pointer this pool never issued.
	void release(const ArpNote* note) {
		for (Entry& e : entries_) {
			if (e.active && &e.note == note) {
				e.active = false;
				return;
			}
		}
	}

	void releaseAll() {
		for (Entry& e : entries_) {
			e.active = false;
		}
	}

	/// Whether this pool issued that pointer.
	///
	/// This is how an effect avoids reacting to its own output. The note-event axis runs
	/// immediately after the time axis on the same instruction, so a note an effect just generated
	/// comes straight back to its own `onNoteEvent` — and an echo that echoes its own echoes is a
	/// runaway, not a feature.
	[[nodiscard]] bool owns(const ArpNote* note) const {
		if (note == nullptr) {
			return false;
		}
		for (const Entry& e : entries_) {
			if (&e.note == note) {
				return true;
			}
		}
		return false;
	}

	/// Every record currently issued, for the MPE member-channel survey in
	/// `MIDIInstrument::noteOnPostArp`. That survey walks `arpeggiator.notes` to count notes per
	/// member channel; a ringing echo is not in that array, so without this it is invisible and a
	/// live note gets assigned a channel an echo is still using — wrong bend on the wrong note.
	template <typename Fn>
	void forEachActive(Fn&& fn) const {
		for (const Entry& e : entries_) {
			if (e.active) {
				fn(e.note);
			}
		}
	}

private:
	struct Entry {
		ArpNote note;
		bool active{false};
	};

	std::array<Entry, kSize> entries_{};
};

} // namespace deluge::modulation::midi_fx
