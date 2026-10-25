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

#include "modulation/midi_fx/delay_fx.h"
#include "modulation/arpeggiator.h"
#include "util/functions.h"
#include <algorithm>

namespace deluge::modulation::midi_fx {

namespace {

/// Velocity below which a repeat is dropped instead of emitted. 1, not 0: velocity 0 is note-off
/// on MIDI, so a "silent" repeat would be a note-off for a note that never started.
constexpr int32_t kMinVelocity = 1;

/// Lowest and highest note a repeat may be transposed to. A repeat that walks off the end of the
/// keyboard ends the tail rather than being clamped: clamping turns a rising delay into a note
/// that repeats on the same pitch forever, which sounds like a stuck effect.
constexpr int32_t kLowestNote = 0;
constexpr int32_t kHighestNote = 127;

/// The division, in internal ticks.
///
/// Identical arithmetic to `ArpeggiatorBase::doTickForward` (arpeggiator.cpp:1563), on purpose:
/// the delay's divisions must line up exactly with the arpeggiator's, or an arp into a delay
/// drifts against itself.
int32_t divisionInTicks(const MidiFxSettings& settings) {
	if (settings.delaySyncLevel == 0) {
		return 0;
	}
	uint32_t ticksPerPeriod = 3u << (9 - settings.delaySyncLevel);
	if (settings.delaySyncType == SYNC_TYPE_TRIPLET) {
		ticksPerPeriod = ticksPerPeriod * 2 / 3;
	}
	else if (settings.delaySyncType == SYNC_TYPE_DOTTED) {
		ticksPerPeriod = ticksPerPeriod * 3 / 2;
	}
	return static_cast<int32_t>(ticksPerPeriod);
}

} // namespace

bool DelayFx::emitNoteOff(Echo& echo, ArpReturnInstruction& instruction) {
	uint32_t slot = 0;
	while (slot < ARP_MAX_INSTRUCTION_NOTES && instruction.noteCodeOffPostArp[slot] != ARP_NOTE_NONE) {
		slot++;
	}
	if (slot >= ARP_MAX_INSTRUCTION_NOTES) {
		return false;
	}
	instruction.noteCodeOffPostArp[slot] = echo.note->noteCodeOnPostArp[0];
	instruction.outputMIDIChannelOff[slot] = echo.note->outputMemberChannel[0];
	echo.sounding = false;
	return true;
}

void DelayFx::releaseEcho(const MidiFxContext& context, Echo& echo) {
	if (context.pool != nullptr) {
		context.pool->release(echo.note);
	}
	echo = Echo{};
}

bool DelayFx::onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction) {
	if (context.settings == nullptr || !context.settings->enabled || context.settings->delayRepeats == 0) {
		return true;
	}
	if (context.pool == nullptr) {
		return true;
	}
	ArpNote* source = instruction.arpNoteOn;
	if (source == nullptr || source->noteCodeOnPostArp[0] == ARP_NOTE_NONE) {
		return true;
	}

	// Our own repeat, coming back round. The note-event axis runs immediately after the time axis
	// on the same instruction, so every repeat this effect emits arrives here one call later.
	// Arming from it would echo the echo, and that is a runaway rather than a feedback control.
	if (context.pool->owns(source)) {
		return true;
	}

	const int32_t division = divisionInTicks(*context.settings);
	if (division <= 0) {
		return true;
	}

	auto slot = std::ranges::find_if(echoes_, [](const Echo& e) { return !e.active; });
	if (slot == echoes_.end()) {
		// Every echo slot busy. The new note plays undelayed rather than stealing a slot: stealing
		// would strand a sounding repeat with no note-off scheduled for it.
		return true;
	}

	ArpNote* record = context.pool->acquire(*source);
	if (record == nullptr) {
		return true;
	}

	// One note per record. The source may carry several post-arp notes (a chord from the arp), and
	// the tail only ever sounds slot 0 - so blank the rest rather than leave stale note codes that
	// the dispatch loop would emit.
	for (uint32_t n = 1; n < ARP_MAX_INSTRUCTION_NOTES; n++) {
		record->noteCodeOnPostArp[n] = ARP_NOTE_NONE;
	}

	// Gate: a sequenced note keeps its own length so a staccato phrase echoes staccato; a live note
	// has no length until it is released (`sampleSyncLengthOn` is 0), so half the division stands
	// in. Never longer than the division, or repeat N would still be sounding when N+1 starts and
	// the pool would need a record per repeat rather than per note.
	const int32_t sourceLength = static_cast<int32_t>(instruction.sampleSyncLengthOn);
	const int32_t gate =
	    (sourceLength > 0 && sourceLength < division) ? sourceLength : std::max<int32_t>(1, division / 2);

	*slot = Echo{.note = record,
	             .ticksToNextOn = division,
	             .ticksToOff = 0,
	             .divisionTicks = division,
	             .gateTicks = std::max<int32_t>(1, gate),
	             .sourceNoteCode = source->noteCodeOnPostArp[0],
	             .repeatsLeft = context.settings->delayRepeats,
	             .repeatIndex = 0,
	             .velocity = source->velocity,
	             .sounding = false,
	             .active = true};

	// Never swallows: the note the player actually played goes out as normal, and the repeats
	// follow it.
	return true;
}

int32_t DelayFx::tick(const MidiFxContext& context, uint32_t clipCurrentPos, ArpReturnInstruction& instruction) {
	// Not gated on `enabled`: a tail already ringing when the effect is switched off still has to
	// finish and, above all, still has to send its note-offs.
	const uint32_t previousPos = lastClipPos_;
	const bool hadPos = havePos_;
	lastClipPos_ = clipCurrentPos;
	havePos_ = true;

	if (std::ranges::none_of(echoes_, [](const Echo& e) { return e.active; })) {
		return kNoEvent;
	}
	if (context.settings == nullptr) {
		return kNoEvent;
	}

	// A rewind is the clip looping, and a delay tail is supposed to ring across a loop point - that
	// is most of what a delay is for. So re-establish the baseline and charge no elapsed time,
	// rather than firing everything the way Gate Time does. Gate Time is right to fire: its pending
	// note-offs belong to notes the loop is about to retrigger. These do not.
	//
	// The cost is the handful of ticks between the last visit and the wrap, dropped from one
	// repeat's timing, once per loop.
	const bool rewound = hadPos && clipCurrentPos < previousPos;
	const int32_t elapsed = (hadPos && !rewound) ? static_cast<int32_t>(clipCurrentPos - previousPos) : 0;

	// An instruction carries one note-on. If the arp or the sequencer already put a note in it,
	// this pass emits none - the repeat waits for the next tick rather than overwriting a note
	// somebody is actually playing.
	bool noteOnTaken = instruction.arpNoteOn != nullptr;
	int32_t soonest = kNoEvent;

	for (Echo& echo : echoes_) {
		if (!echo.active) {
			continue;
		}

		if (echo.sounding) {
			echo.ticksToOff -= elapsed;
			if (echo.ticksToOff <= 0) {
				if (!emitNoteOff(echo, instruction)) {
					// Four note-off slots, all spoken for. Ask to be actioned again on this same
					// tick rather than letting the note ring on.
					soonest = 0;
				}
			}
			else {
				soonest = std::min(soonest, echo.ticksToOff);
			}
		}

		echo.ticksToNextOn -= elapsed;

		if (echo.repeatsLeft == 0) {
			if (!echo.sounding) {
				releaseEcho(context, echo);
			}
			continue;
		}
		if (echo.ticksToNextOn > 0) {
			soonest = std::min(soonest, echo.ticksToNextOn);
			continue;
		}
		if (noteOnTaken) {
			soonest = 0;
			continue;
		}
		// A gate at or over the division can leave the previous repeat still sounding. Release it
		// first: an external synth given two note-ons and one note-off holds the note forever.
		if (echo.sounding && !emitNoteOff(echo, instruction)) {
			soonest = 0;
			continue;
		}

		const int32_t nextIndex = echo.repeatIndex + 1;

		// Velocity. The first repeat is set by DELAY LEVEL, a percentage of the source note it was
		// armed from; every repeat after that adds FB VELOCITY to the one before - the RS7000's
		// additive feedback, which can fade the tail or swell it. `echo.velocity` holds the source
		// velocity until the first repeat, then each repeat's own level.
		const int32_t nextVelocity =
		    (echo.repeatIndex == 0)
		        ? (static_cast<int32_t>(echo.velocity) * context.settings->delayLevelPercent) / 100
		        : static_cast<int32_t>(echo.velocity) + context.settings->delayFbVelocity;

		// Pitch. A fixed step per repeat (FB NOTE), or - with FB NOTE = RND - a fresh random interval
		// per repeat, one octave either side of the source so it stays musical rather than scattering
		// across the whole keyboard.
		const int32_t shifted = context.settings->delayNoteRandom
		                            ? static_cast<int32_t>(echo.sourceNoteCode) + (random(24) - 12)
		                            : static_cast<int32_t>(echo.sourceNoteCode)
		                                  + static_cast<int32_t>(context.settings->delayPitchShift) * nextIndex;

		if (nextVelocity < kMinVelocity || shifted < kLowestNote || shifted > kHighestNote) {
			// Faded out, or transposed off the end of the keyboard. Either way the tail is over.
			releaseEcho(context, echo);
			continue;
		}

		echo.repeatIndex = static_cast<uint8_t>(nextIndex);
		echo.velocity = static_cast<uint8_t>(std::clamp<int32_t>(nextVelocity, kMinVelocity, 127));
		echo.repeatsLeft--;

		echo.note->noteCodeOnPostArp[0] = static_cast<int16_t>(shifted);
		echo.note->velocity = echo.velocity;
		// Anchored the same way VelocityFx anchors, so a velocity offset applied to a repeat is
		// computed from the repeat's own level rather than compounding on every pass.
		echo.note->baseVelocity = echo.velocity;
		echo.note->noteStatus[0] = ArpNoteStatus::PENDING;

		instruction.arpNoteOn = echo.note;
		noteOnTaken = true;
		echo.sounding = true;
		echo.ticksToOff = echo.gateTicks;
		// FB CLOCK scales the interval before each successive repeat, so the echoes speed up (negative)
		// or slow down (positive) instead of sitting on the grid. Applied after the first repeat has
		// been scheduled, so that repeat lands on the base division and the scaling compounds from
		// there - exactly the RS7000's "delay time of the 2nd and subsequent repeats = the preceding
		// repeat's × FB CLOCK". Floored at one tick so a repeat can never land on top of its predecessor.
		if (context.settings->delayFbClockPercent != 0) {
			echo.divisionTicks = std::max<int32_t>(
			    1, (echo.divisionTicks * (100 + context.settings->delayFbClockPercent)) / 100);
		}
		echo.ticksToNextOn = echo.divisionTicks;
		soonest = std::min(soonest, std::min(echo.gateTicks, echo.divisionTicks));
	}

	return soonest;
}

bool DelayFx::flush(const MidiFxContext& context, ArpReturnInstruction& instruction) {
	// Unlike Gate Time, this one does emit. Gate Time's pending note-offs belong to notes the
	// transport has already cut by other means; these are notes only this effect knows about, and
	// nothing else will ever release them. A repeat left sounding on an external synth when the
	// transport stops is the bug report this whole feature has to avoid.
	bool moreToDo = false;
	for (Echo& echo : echoes_) {
		if (!echo.active) {
			continue;
		}
		if (echo.sounding && !emitNoteOff(echo, instruction)) {
			// Out of note-off slots. Keep the record; the caller comes back with a fresh
			// instruction.
			moreToDo = true;
			continue;
		}
		releaseEcho(context, echo);
	}
	havePos_ = false;
	return moreToDo;
}

} // namespace deluge::modulation::midi_fx
