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

#include "modulation/midi_fx/retrig_fx.h"
#include "playback/playback_handler.h"
#include <algorithm>

namespace deluge::modulation::midi_fx {

namespace {

/// Velocity below which a repeat is dropped rather than emitted. 1, not 0: velocity 0 is
/// note-off on MIDI, so a "silent" repeat would be a note-off for a note that never started.
constexpr int32_t kMinVelocity = 1;

/// Phase increment per sample for one division, derived from tempo rather than from transport
/// position - which is what lets this run with playback stopped.
///
/// Same arithmetic as ArpeggiatorSettings::getPhaseIncrement (arpeggiator.cpp:1592), minus its
/// syncLevel == 0 branch: a retrigger is always a musical division, never a free rate.
uint32_t retrigPhaseIncrement(const MidiFxSettings& settings) {
	if (settings.retrigSyncLevel == 0) {
		return 0;
	}
	const int32_t rightShiftAmount = 9 - settings.retrigSyncLevel;
	uint32_t phaseIncrement = playbackHandler.getTimePerInternalTickInverse();
	phaseIncrement >>= rightShiftAmount;
	if (settings.retrigSyncType == SYNC_TYPE_TRIPLET) {
		phaseIncrement = phaseIncrement * 3 / 2;
	}
	else if (settings.retrigSyncType == SYNC_TYPE_DOTTED) {
		phaseIncrement = phaseIncrement * 2 / 3;
	}
	return phaseIncrement;
}

} // namespace

bool RetrigFx::anyActive() const {
	return std::ranges::any_of(voices_, [](const Voice& v) { return v.active; });
}

bool RetrigFx::ownsRecord(const ArpNote* note) const {
	if (note == nullptr) {
		return false;
	}
	return std::ranges::any_of(voices_, [note](const Voice& v) { return &v.note == note; });
}

bool RetrigFx::onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction) {
	if (context.settings == nullptr || !context.settings->enabled || context.settings->retrigRepeats == 0) {
		return true;
	}
	ArpNote* source = instruction.arpNoteOn;
	if (source == nullptr || source->noteCodeOnPostArp[0] == ARP_NOTE_NONE) {
		return true;
	}

	// One of this effect's own repeats, coming back round the same instruction. Arming from it
	// would roll the roll.
	if (ownsRecord(source)) {
		return true;
	}

	auto slot = std::ranges::find_if(voices_, [](const Voice& v) { return !v.active; });
	if (slot == voices_.end()) {
		// All voices busy. Drop the new roll rather than steal one - stealing would leave the
		// stolen note's repeats half-emitted, and a partial roll sounds like a fault.
		return true;
	}

	// Copy the record. From here the repeats do not depend on the arpeggiator's array, which is
	// the whole point: releasing the pad deletes that element while the roll is still running.
	*slot = Voice{
	    .note = *source, .repeatsLeft = context.settings->retrigRepeats, .velocity = source->velocity, .active = true};

	// Restart the phase so the first repeat lands a full division after the hit rather than
	// wherever the free-running counter happened to be. A roll that starts mid-division is the
	// difference between a retrigger and a stutter.
	phase_ = 0;
	return true;
}

void RetrigFx::render(const MidiFxContext& context, int32_t numSamples, ArpReturnInstruction& instruction) {
	if (!anyActive()) {
		return;
	}
	// Deliberately not gated on `enabled` or on repeats being non-zero: a roll already running
	// when the effect is switched off still has to finish or be flushed, not be abandoned mid-way
	// with notes left on.
	if (context.settings == nullptr) {
		return;
	}

	const uint32_t phaseIncrement = retrigPhaseIncrement(*context.settings);
	if (phaseIncrement == 0) {
		return;
	}

	const uint32_t advanced = phaseIncrement * static_cast<uint32_t>(numSamples);
	const uint32_t newPhase = phase_ + advanced;
	const bool divisionElapsed = newPhase < phase_; // wrapped past 2^32
	phase_ = newPhase;
	if (!divisionElapsed) {
		return;
	}

	// One note-on per instruction - ArpReturnInstruction carries a single arpNoteOn. With more
	// than one voice due on the same block, the rest wait for the next one, a few milliseconds
	// later. Audible only if you are looking for it, and the alternative is dropping them.
	bool emittedNoteOn = false;
	uint32_t offSlot = 0;
	while (offSlot < ARP_MAX_INSTRUCTION_NOTES && instruction.noteCodeOffPostArp[offSlot] != ARP_NOTE_NONE) {
		offSlot++;
	}

	for (Voice& v : voices_) {
		if (!v.active) {
			continue;
		}

		// Release the previous hit before retriggering, so an internal voice restarts cleanly and
		// an external synth does not accumulate held notes.
		if (offSlot < ARP_MAX_INSTRUCTION_NOTES) {
			instruction.noteCodeOffPostArp[offSlot] = v.note.noteCodeOnPostArp[0];
			instruction.outputMIDIChannelOff[offSlot] = v.note.outputMemberChannel[0];
			offSlot++;
		}

		const int32_t decayed = (static_cast<int32_t>(v.velocity) * context.settings->retrigDecayPercent) / 100;
		if (v.repeatsLeft == 0 || decayed < kMinVelocity) {
			v.active = false;
			continue;
		}

		if (emittedNoteOn) {
			continue; // catches the next block
		}

		v.velocity = static_cast<uint8_t>(std::clamp<int32_t>(decayed, kMinVelocity, 127));
		v.repeatsLeft--;
		v.note.velocity = v.velocity;
		v.note.noteStatus[0] = ArpNoteStatus::PENDING;
		instruction.arpNoteOn = &v.note;
		emittedNoteOn = true;
	}
}

bool RetrigFx::flush(const MidiFxContext& /*context*/, ArpReturnInstruction& instruction) {
	uint32_t emitted = 0;
	while (emitted < ARP_MAX_INSTRUCTION_NOTES && instruction.noteCodeOffPostArp[emitted] != ARP_NOTE_NONE) {
		emitted++;
	}

	bool moreToDo = false;
	for (Voice& v : voices_) {
		if (!v.active) {
			continue;
		}
		if (emitted >= ARP_MAX_INSTRUCTION_NOTES) {
			// Four note-off slots per instruction. Keep the voice and let the caller come back with
			// a fresh one rather than dropping the release.
			moreToDo = true;
			continue;
		}
		// Sent for every armed voice, sounding or not. A note-off for a note that is already off
		// is harmless on MIDI, CV and a Sound voice alike; tracking which hits are mid-gate to
		// avoid it would cost more than it saves.
		instruction.noteCodeOffPostArp[emitted] = v.note.noteCodeOnPostArp[0];
		instruction.outputMIDIChannelOff[emitted] = v.note.outputMemberChannel[0];
		emitted++;
		v.active = false;
	}
	if (!moreToDo) {
		phase_ = 0;
	}
	return moreToDo;
}

} // namespace deluge::modulation::midi_fx
