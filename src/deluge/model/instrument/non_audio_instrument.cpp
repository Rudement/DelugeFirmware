/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
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

#include "model/instrument/non_audio_instrument.h"
#include "definitions_cxx.hpp"
#include "dsp/stereo_sample.h"
#include "model/clip/instrument_clip.h"
#include "model/model_stack.h"
#include "modulation/arpeggiator.h"
#include "modulation/midi_fx/midi_fx.h"
#include "modulation/params/param.h"
#include "processing/engines/cv_engine.h"
#include "storage/storage_manager.h"
#include "util/functions.h"
#include <algorithm>
#include <cstring>

void NonAudioInstrument::renderOutput(ModelStack* modelStack, std::span<StereoSample> output, int32_t* reverbBuffer,
                                      int32_t reverbAmountAdjust, int32_t sideChainHitPending,
                                      bool shouldLimitDelayFeedback, bool isClipActive) {
	// MIDI / CV arpeggiator, then MIDI FX
	if (activeClip) {
		InstrumentClip* activeInstrumentClip = (InstrumentClip*)activeClip;
		ArpeggiatorSettings* arpSettings = &activeInstrumentClip->arpSettings;

		ArpReturnInstruction instruction;

		if (arpSettings->mode != ArpMode::OFF) {
			uint32_t gateThreshold = (uint32_t)arpSettings->gate + 2147483648;
			uint32_t phaseIncrement = arpSettings->getPhaseIncrement(
			    getFinalParameterValueExp(paramNeutralValues[deluge::modulation::params::GLOBAL_ARP_RATE],
			                              cableToExpParamShortcut(arpSettings->rate)));

			arpeggiator.render(arpSettings, &instruction, output.size(), gateThreshold, phaseIncrement);
		}
		// Deliberately NO handlePendingNotes() in the arp-off branch, unlike Sound::render(). On a Sound a
		// note pends because no voice was free; MIDI and CV have no voice allocation, so the only note that
		// can be left PENDING here is a held one that was mid-arpeggiation when the arp was switched off.
		// Sound starts that note; MIDI/CV never has. Making them agree is a real behaviour change and Stage 1
		// ships none - see docs/MIDI-FX-DESIGN.md.

		// The FX seam. Runs whether or not the arp did, which is the entire point. With the arp off
		// `instruction` is default-constructed and inert, so with an empty chain both dispatch calls below
		// are no-ops - exactly as when this whole block was skipped.
		deluge::modulation::midi_fx::MidiFxContext midiFxContext{.settings = &activeInstrumentClip->midiFxSettings,
		                                                         .arpIsOn = !ArpeggiatorBase::arpIsOff(arpSettings)};
		midiFx.render(midiFxContext, output.size(), instruction);
		// Note-event axis after the time axis, so note-mapping effects also see anything a
		// time-based effect just generated. Return value ignored: this path carries note-offs.
		(void)midiFx.onNoteEvent(midiFxContext, instruction);

		// kDefaultLiftValue: is there some better option? The lift event wouldn't have occurred yet...
		dispatchArpNoteOffs(instruction, kDefaultLiftValue);
		dispatchArpNoteOns(instruction, ArpNoteStatusUpdate::AFTER_NOTE_ON);
	}
}

void NonAudioInstrument::sendNote(ModelStackWithThreeMainThings* modelStack, bool isOn, int32_t noteCodePreArp,
                                  int16_t const* mpeValues, int32_t fromMIDIChannel, uint8_t velocity,
                                  uint32_t sampleSyncLength, int32_t ticksLate, uint32_t samplesLate) {

	ArpeggiatorSettings* arpSettings = nullptr;
	if (activeClip) {
		arpSettings = &((InstrumentClip*)activeClip)->arpSettings;
	}

	ArpReturnInstruction instruction;
	// Sound::noteOn has always done this; the MIDI/CV path never did, because nothing here reads
	// it - MIDI and CV do no sample sync. Gate Time does read it: for a sequenced note this is
	// the note's length in ticks (NoteRow passes thisNote->getLength() through as
	// sampleSyncLength, note_row.cpp:2558, and Voice stores it as sequenceSyncLengthTicks,
	// voice.cpp:251 - the name says samples and means ticks). Write-only on this path before and
	// after, so no behaviour change.
	instruction.sampleSyncLengthOn = sampleSyncLength;

	// Note on
	if (isOn) {

		// Run everything by the Arp...
		arpeggiator.noteOn(arpSettings, noteCodePreArp, velocity, &instruction, fromMIDIChannel, mpeValues);

		// Note-event seam. After the arp, before the dispatch, arp on or off.
		if (!runMidiFxOnNoteEvent(instruction)) {
			return;
		}

		// Deliberately no note-offs here: the original code processed only note-ons on this path,
		// so any note-offs the arp returned alongside a note-on were dropped. Preserved as-is.
		dispatchArpNoteOns(instruction, ArpNoteStatusUpdate::AFTER_NOTE_ON);
	}

	// Note off
	else {

		// Run everything by the Arp...
		arpeggiator.noteOff(arpSettings, noteCodePreArp, &instruction);

		// Return value deliberately ignored: a swallowed note-off is a hanging note.
		(void)runMidiFxOnNoteEvent(instruction);

		dispatchArpNoteOffs(instruction, velocity);

		// CV instruments could switch on a note to do a glide
		if (type == OutputType::CV) {
			dispatchArpNoteOns(instruction, ArpNoteStatusUpdate::NONE);
		}
	}
}

// Inherit / overrides from both MelodicInstrument and ModControllable
void NonAudioInstrument::polyphonicExpressionEventOnChannelOrNote(int32_t newValue, int32_t expressionDimension,
                                                                  int32_t channelOrNoteNumber,
                                                                  MIDICharacteristic whichCharacteristic) {
	int32_t n;
	int32_t nEnd;

	// If for note, we can search right to it.
	if (whichCharacteristic == MIDICharacteristic::NOTE) {
		n = arpeggiator.notes.search(channelOrNoteNumber, GREATER_OR_EQUAL);
		if (n < arpeggiator.notes.getNumElements()) {
			nEnd = 0;
			goto lookAtArpNote;
		}
		return;
	}

	nEnd = arpeggiator.notes.getNumElements();

	for (n = 0; n < nEnd; n++) {
lookAtArpNote:
		ArpNote* arpNote = (ArpNote*)arpeggiator.notes.getElementAddress(n);
		if (arpNote->inputCharacteristics[util::to_underlying(whichCharacteristic)] == channelOrNoteNumber) {

			// Update the MPE value in the ArpNote. If arpeggiating, it'll get read from there the next time there's a
			// note-on-post-arp. I realise this is potentially frequent writing when it's only going to be read
			// occasionally, but since we're already this far (the Instrument being notified), it's hardly any extra
			// work.
			arpNote->mpeValues[expressionDimension] = newValue >> 16;

			// Send this even if arp is on and this note isn't currently sounding: its release might still be
			for (int32_t i = 0; i < ARP_MAX_INSTRUCTION_NOTES; i++) {
				if (arpNote->noteCodeOnPostArp[i] == ARP_NOTE_NONE
				    || arpNote->outputMemberChannel[i] == MIDI_CHANNEL_NONE) {
					break;
				}
				polyphonicExpressionEventPostArpeggiator(newValue, arpNote->noteCodeOnPostArp[i], expressionDimension,
				                                         arpNote, i);
			}
		}
	}
}

// Returns num ticks til next arp event
int32_t NonAudioInstrument::doTickForwardForArp(ModelStack* modelStack, int32_t currentPos) {
	if (!activeClip) {
		return 2147483647;
	}

	ArpReturnInstruction instruction;

	// Already unconditional: doTickForward() gates internally and returns kNoEvent when the arp is off, so
	// this call site is the arp-independent tick path the FX seam needs. Nothing to open up here.
	int32_t ticksTilNextArpEvent = arpeggiator.doTickForward(&((InstrumentClip*)activeClip)->arpSettings, &instruction,
	                                                         currentPos, activeClip->currentlyPlayingReversed);

	deluge::modulation::midi_fx::MidiFxContext midiFxContext{
	    .settings = &((InstrumentClip*)activeClip)->midiFxSettings,
	    .arpIsOn = !ArpeggiatorBase::arpIsOff(&((InstrumentClip*)activeClip)->arpSettings)};
	int32_t ticksTilNextFxEvent = midiFx.tick(midiFxContext, currentPos, instruction);
	(void)midiFx.onNoteEvent(midiFxContext, instruction);

	// kDefaultLiftValue: is there some better option? The lift event wouldn't have occurred yet...
	dispatchArpNoteOffs(instruction, kDefaultLiftValue);
	dispatchArpNoteOns(instruction, ArpNoteStatusUpdate::NONE);

	return std::min(ticksTilNextArpEvent, ticksTilNextFxEvent);
}

bool NonAudioInstrument::runMidiFxOnNoteEvent(ArpReturnInstruction& instruction) {
	if (midiFx.isEmpty() || activeClip == nullptr) {
		return true;
	}
	deluge::modulation::midi_fx::MidiFxContext context{
	    .settings = &((InstrumentClip*)activeClip)->midiFxSettings,
	    .arpIsOn = !ArpeggiatorBase::arpIsOff(&((InstrumentClip*)activeClip)->arpSettings)};
	return midiFx.onNoteEvent(context, instruction);
}

void NonAudioInstrument::dispatchMidiFxFlushNoteOffs(ArpReturnInstruction& instruction) {
	// kDefaultLiftValue for the same reason the render path uses it: there was no lift event, and
	// there is not going to be one.
	dispatchArpNoteOffs(instruction, kDefaultLiftValue);
}

void NonAudioInstrument::dispatchArpNoteOffs(ArpReturnInstruction& instruction, int32_t velocity) {
	for (int32_t n = 0; n < ARP_MAX_INSTRUCTION_NOTES; n++) {
		if (instruction.glideNoteCodeOffPostArp[n] == ARP_NOTE_NONE) {
			break;
		}
		noteOffPostArp(instruction.glideNoteCodeOffPostArp[n], instruction.glideOutputMIDIChannelOff[n], velocity, n);
	}
	for (int32_t n = 0; n < ARP_MAX_INSTRUCTION_NOTES; n++) {
		if (instruction.noteCodeOffPostArp[n] == ARP_NOTE_NONE) {
			break;
		}
		noteOffPostArp(instruction.noteCodeOffPostArp[n], instruction.outputMIDIChannelOff[n], velocity, n);
	}
}

void NonAudioInstrument::dispatchArpNoteOns(ArpReturnInstruction& instruction, ArpNoteStatusUpdate statusUpdate) {
	if (instruction.arpNoteOn == nullptr) {
		return;
	}
	for (int32_t n = 0; n < ARP_MAX_INSTRUCTION_NOTES; n++) {
		if (instruction.arpNoteOn->noteCodeOnPostArp[n] == ARP_NOTE_NONE) {
			break;
		}
		if (statusUpdate == ArpNoteStatusUpdate::BEFORE_NOTE_ON) {
			instruction.arpNoteOn->noteStatus[n] = ArpNoteStatus::PLAYING;
		}
		noteOnPostArp(instruction.arpNoteOn->noteCodeOnPostArp[n], instruction.arpNoteOn, n);
		if (statusUpdate == ArpNoteStatusUpdate::AFTER_NOTE_ON) {
			instruction.arpNoteOn->noteStatus[n] = ArpNoteStatus::PLAYING;
		}
	}
}

// Unlike other Outputs, these don't have ParamManagers backed up at the Song level
ParamManager* NonAudioInstrument::getParamManager(Song* song) {

	if (activeClip) {
		return &activeClip->paramManager;
	}
	else {
		return nullptr;
	}
}

bool NonAudioInstrument::readTagFromFile(Deserializer& reader, char const* tagName) {

	char const* slotXMLTag = getSlotXMLTag();

	if (!strcmp(tagName, slotXMLTag)) {
		setChannel(reader.readTagOrAttributeValueInt());
	}

	else {
		return MelodicInstrument::readTagFromFile(reader, tagName);
	}

	reader.exitTag();
	return true;
}

bool NonAudioInstrument::needsEarlyPlayback() const {
	return (type == OutputType::MIDI_OUT && channel == MIDI_CHANNEL_TRANSPOSE);
}
