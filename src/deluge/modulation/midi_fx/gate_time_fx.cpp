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

#include "modulation/midi_fx/gate_time_fx.h"
#include "modulation/arpeggiator.h"
#include <algorithm>

namespace deluge::modulation::midi_fx {

namespace {
/// 100% means "leave notes alone", so the effect is inert at its own default.
constexpr uint16_t kGateTimeOff = 100;

/// How much longer than its written length a note must come out before this effect takes over
/// responsibility for releasing it.
///
/// Found on hardware: at 101% a sixteenth note computes to the same number of ticks as at 100%,
/// because ticks are integers. The gate had not changed at all, but the note was now being held by
/// this effect instead of by the sequencer, and the release landed on the same tick as the next
/// note - which on the wrong side of that tick cuts the next note's head off. Sixteenths went
/// quieter and nothing "opened up" until the multiplier was large enough to matter.
///
/// So a sub-tick extension is no extension: leave the note alone and let the sequencer release it
/// exactly as it always did.
constexpr int32_t kMinExtensionTicks = 2;
} // namespace

void GateTimeFx::swallowNoteOffsForHeldNotes(ArpReturnInstruction& instruction) {
	// Compact rather than blank: the dispatch loops stop at the first ARP_NOTE_NONE, so a hole in
	// the middle would silently drop every note-off after it.
	uint32_t write = 0;
	for (uint32_t read = 0; read < ARP_MAX_INSTRUCTION_NOTES; read++) {
		const int16_t noteCode = instruction.noteCodeOffPostArp[read];
		if (noteCode == ARP_NOTE_NONE) {
			break;
		}
		const bool held = std::ranges::any_of(pending_, [noteCode](const PendingOff& p) {
			return p.active && p.holdingPastWrittenEnd && p.noteCode == noteCode;
		});
		if (held) {
			continue; // swallowed: this effect owes the note a note-off of its own
		}
		instruction.noteCodeOffPostArp[write] = noteCode;
		instruction.outputMIDIChannelOff[write] = instruction.outputMIDIChannelOff[read];
		write++;
	}
	for (uint32_t n = write; n < ARP_MAX_INSTRUCTION_NOTES; n++) {
		instruction.noteCodeOffPostArp[n] = ARP_NOTE_NONE;
	}
}

bool GateTimeFx::onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction) {
	// Deliberately first, and deliberately not gated on `enabled`: a note already being held past
	// its written end still has to have the sequencer's note-off suppressed, even if the effect
	// was switched off or set back to 100% while that note was sounding. Otherwise switching the
	// control mid-note releases it early, and worse, leaves this effect's own note-off to fire
	// against a note that has since been retriggered.
	swallowNoteOffsForHeldNotes(instruction);

	if (context.settings == nullptr || !context.settings->enabled) {
		return true;
	}
	if (context.settings->gateTimePercent == kGateTimeOff || context.arpIsOn) {
		return true;
	}
	ArpNote* arpNote = instruction.arpNoteOn;
	if (arpNote == nullptr) {
		return true;
	}

	// No length means a live note - a held key has no length until it is released. Pass through.
	const uint32_t lengthTicks = instruction.sampleSyncLengthOn;
	if (lengthTicks == 0) {
		return true;
	}

	// At least one tick. A gate that rounds to zero would emit the note-off in the same pass as
	// the note-on, which on MIDI is a note that never sounds rather than a very short one.
	const int32_t gateTicks =
	    std::max<int32_t>(1, static_cast<int32_t>((lengthTicks * context.settings->gateTimePercent) / kGateTimeOff));
	const bool holding = gateTicks > static_cast<int32_t>(lengthTicks) + kMinExtensionTicks;

	// Over 100% but not by a whole tick or two. Nothing to do, and taking the note over anyway is
	// what made short notes quieter - see kMinExtensionTicks.
	if (context.settings->gateTimePercent > kGateTimeOff && !holding) {
		return true;
	}

	// uint32_t, not int32_t: ARP_MAX_INSTRUCTION_NOTES is unsigned (arpeggiator.h:37) and the
	// surrounding code mixes the two, which is a -Wsign-compare warning under MAINTAINER_MODE.
	for (uint32_t n = 0; n < ARP_MAX_INSTRUCTION_NOTES; n++) {
		const int16_t noteCode = arpNote->noteCodeOnPostArp[n];
		if (noteCode == ARP_NOTE_NONE) {
			break;
		}

		// The same note starting again while an extension of it is still pending. Cancel the old
		// one rather than emit it: emitting here would not work anyway, because the note-on path
		// through sendNote dispatches note-ons only. The new note schedules its own release, so
		// the note still gets one.
		for (PendingOff& p : pending_) {
			if (p.active && p.noteCode == noteCode) {
				p.active = false;
			}
		}

		auto slot = std::ranges::find_if(pending_, [](const PendingOff& p) { return !p.active; });
		if (slot == pending_.end()) {
			// Full. Shortening: the note keeps its written length, which sounds wrong but sounds.
			// Lengthening: fall back to the written length too, by leaving the sequencer's own
			// note-off to do its job - which is why nothing is swallowed for a note with no entry.
			break;
		}
		*slot = PendingOff{.ticksRemaining = gateTicks,
		                   .noteCode = noteCode,
		                   .outputMemberChannel = arpNote->outputMemberChannel[n],
		                   .holdingPastWrittenEnd = holding,
		                   .active = true};
	}

	return true;
}

int32_t GateTimeFx::tick(const MidiFxContext& /*context*/, uint32_t clipCurrentPos, ArpReturnInstruction& instruction) {
	// Not gated on `enabled` or on `arpIsOn`: notes scheduled before either changed still have to
	// be released. Switching an effect off must not strand its outstanding note-offs.
	const uint32_t previousPos = lastClipPos_;
	const bool hadPos = havePos_;
	lastClipPos_ = clipCurrentPos;
	havePos_ = true;

	bool anyActive = false;
	for (const PendingOff& p : pending_) {
		anyActive |= p.active;
	}
	if (!anyActive) {
		return kNoEvent;
	}

	// Elapsed ticks since the last visit.
	//
	// A rewind - clip loop, or the transport jumping backwards - means the note this was
	// scheduled against is about to be retriggered or has already been cut, so release everything
	// rather than try to be clever about it. An early note-off is a short note; a note-off that
	// never comes is a stuck note, and only one of those two generates a bug report. This matters
	// more now than it did when the effect could only shorten: a note held past the loop point has
	// nothing else in the system that will ever release it.
	//
	// No previous position is NOT a rewind. It means this is the first visit and no time has
	// passed yet, so establish the baseline and leave the queue alone. Firing here would release
	// every note scheduled since the last flush the instant the clock next ran.
	const bool fireEverything = hadPos && clipCurrentPos < previousPos;
	const int32_t elapsed = (hadPos && !fireEverything) ? static_cast<int32_t>(clipCurrentPos - previousPos) : 0;

	// Start at the first free slot, not at zero. With the arp on this instruction may already
	// carry the arp's own note-offs, and overwriting them would strand arpeggiated notes. The
	// dispatch loop stops at the first ARP_NOTE_NONE, so the free slots are the tail.
	uint32_t emitted = 0;
	while (emitted < ARP_MAX_INSTRUCTION_NOTES && instruction.noteCodeOffPostArp[emitted] != ARP_NOTE_NONE) {
		emitted++;
	}

	int32_t soonest = kNoEvent;
	for (PendingOff& p : pending_) {
		if (!p.active) {
			continue;
		}

		// The same note starting again in this very instruction. Releasing it now would release the
		// new note, not the old one - the two are the same note code on the same channel, and the
		// output has no way to tell them apart. Drop the release instead: the note that just
		// started brings its own.
		//
		// onNoteEvent cancels this too, but only when the note-on arrives on an earlier pass than
		// this tick. When both land together the time axis runs first, and this is the guard for
		// that ordering.
		if (p.holdingPastWrittenEnd && instruction.arpNoteOn != nullptr) {
			bool retriggered = false;
			for (uint32_t n = 0; n < ARP_MAX_INSTRUCTION_NOTES; n++) {
				if (instruction.arpNoteOn->noteCodeOnPostArp[n] == ARP_NOTE_NONE) {
					break;
				}
				retriggered |= instruction.arpNoteOn->noteCodeOnPostArp[n] == p.noteCode;
			}
			if (retriggered) {
				p.active = false;
				continue;
			}
		}

		p.ticksRemaining -= elapsed;
		if (!fireEverything && p.ticksRemaining > 0) {
			soonest = std::min(soonest, p.ticksRemaining);
			continue;
		}
		if (emitted >= ARP_MAX_INSTRUCTION_NOTES) {
			// The instruction only carries four note-offs. Come straight back for the rest rather
			// than dropping them - returning 0 asks to be actioned again on this same tick.
			soonest = 0;
			continue;
		}
		instruction.noteCodeOffPostArp[emitted] = p.noteCode;
		instruction.outputMIDIChannelOff[emitted] = p.outputMemberChannel;
		emitted++;
		p.active = false;
	}
	return soonest;
}

bool GateTimeFx::flush(const MidiFxContext& /*context*/, ArpReturnInstruction& instruction) {
	// Two kinds of entry, two answers.
	//
	// A shortened note's pending note-off is dropped without emitting. Flush happens when the
	// transport or the clip stops, and those paths already cut every sounding note by other means;
	// emitting here would send note-offs for notes that are already gone.
	//
	// A note held past its written end is the opposite case. Its real note-off was swallowed, so
	// nothing else in the system is going to release it, and the stop path cannot know about it.
	// That one is emitted.
	bool moreToDo = false;
	uint32_t emitted = 0;
	while (emitted < ARP_MAX_INSTRUCTION_NOTES && instruction.noteCodeOffPostArp[emitted] != ARP_NOTE_NONE) {
		emitted++;
	}

	for (PendingOff& p : pending_) {
		if (!p.active) {
			continue;
		}
		if (!p.holdingPastWrittenEnd) {
			p.active = false;
			continue;
		}
		if (emitted >= ARP_MAX_INSTRUCTION_NOTES) {
			moreToDo = true;
			continue;
		}
		instruction.noteCodeOffPostArp[emitted] = p.noteCode;
		instruction.outputMIDIChannelOff[emitted] = p.outputMemberChannel;
		emitted++;
		p.active = false;
	}

	havePos_ = false;
	return moreToDo;
}

} // namespace deluge::modulation::midi_fx
