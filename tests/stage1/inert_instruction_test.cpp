// Stage 1 safety check, extracted verbatim from the firmware.
//
// The whole "zero functional change" claim for NonAudioInstrument::renderOutput rests on one
// thing: when the arp is OFF, the ArpReturnInstruction is default-constructed and both dispatch
// seams are no-ops. Previously the block was skipped entirely; now it runs with an inert
// instruction. If that claim is wrong, the arp-off path starts emitting notes.
//
// ArpReturnInstruction's ctor and both dispatch bodies are copied verbatim from
// modulation/arpeggiator.h, model/instrument/non_audio_instrument.cpp and
// processing/sound/sound.cpp at e3402639.

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

constexpr int32_t ARP_MAX_INSTRUCTION_NOTES = 4;
constexpr int16_t ARP_NOTE_NONE = 32767;
constexpr uint8_t MIDI_CHANNEL_NONE = 255;

enum class ArpNoteStatus : uint8_t { OFF, PENDING, PLAYING };
enum class ArpNoteStatusUpdate { NONE, BEFORE_NOTE_ON, AFTER_NOTE_ON };

struct ArpNote {
	std::array<int16_t, ARP_MAX_INSTRUCTION_NOTES> noteCodeOnPostArp{};
	std::array<ArpNoteStatus, ARP_MAX_INSTRUCTION_NOTES> noteStatus{};
};

// --- verbatim from arpeggiator.h:190 ---
class ArpReturnInstruction {
public:
	ArpReturnInstruction() {
		sampleSyncLengthOn = 0;
		invertReversed = false;
		arpNoteOn = nullptr;
		outputMIDIChannelOff.fill(MIDI_CHANNEL_NONE);
		noteCodeOffPostArp.fill(ARP_NOTE_NONE);
		glideOutputMIDIChannelOff.fill(MIDI_CHANNEL_NONE);
		glideNoteCodeOffPostArp.fill(ARP_NOTE_NONE);
	}
	uint32_t sampleSyncLengthOn;
	bool invertReversed;
	ArpNote* arpNoteOn;
	std::array<uint8_t, ARP_MAX_INSTRUCTION_NOTES> outputMIDIChannelOff;
	std::array<int16_t, ARP_MAX_INSTRUCTION_NOTES> noteCodeOffPostArp;
	std::array<uint8_t, ARP_MAX_INSTRUCTION_NOTES> glideOutputMIDIChannelOff;
	std::array<int16_t, ARP_MAX_INSTRUCTION_NOTES> glideNoteCodeOffPostArp;
};

static int noteOffCalls = 0;
static int noteOnCalls = 0;
static void noteOffPostArp(int32_t, int32_t, int32_t, int32_t) {
	noteOffCalls++;
}
static void noteOnPostArp(int32_t, ArpNote*, int32_t) {
	noteOnCalls++;
}

// --- verbatim from non_audio_instrument.cpp:153 ---
static void dispatchArpNoteOffs(ArpReturnInstruction& instruction, int32_t velocity) {
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

// --- verbatim from non_audio_instrument.cpp:168 ---
static void dispatchArpNoteOns(ArpReturnInstruction& instruction, ArpNoteStatusUpdate statusUpdate) {
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

int main() {
	// 1. The arp-off case renderOutput now reaches: default-constructed, untouched.
	{
		ArpReturnInstruction instruction;
		dispatchArpNoteOffs(instruction, 64);
		dispatchArpNoteOns(instruction, ArpNoteStatusUpdate::BEFORE_NOTE_ON);
		assert(noteOffCalls == 0 && "inert instruction must emit no note-offs");
		assert(noteOnCalls == 0 && "inert instruction must emit no note-ons");
	}
	// 2. And it must not have been mutated into a non-inert state by the dispatch itself,
	//    since doTickForwardForArp reuses one instruction across the arp and FX calls.
	{
		ArpReturnInstruction instruction;
		dispatchArpNoteOffs(instruction, 64);
		dispatchArpNoteOns(instruction, ArpNoteStatusUpdate::NONE);
		assert(instruction.arpNoteOn == nullptr);
		for (int32_t n = 0; n < ARP_MAX_INSTRUCTION_NOTES; n++) {
			assert(instruction.noteCodeOffPostArp[n] == ARP_NOTE_NONE);
			assert(instruction.glideNoteCodeOffPostArp[n] == ARP_NOTE_NONE);
		}
	}
	// 3. Sanity: a populated instruction still dispatches, so the assertions above are not
	//    vacuously passing because the harness is wired wrong.
	{
		ArpNote note;
		note.noteCodeOnPostArp = {60, ARP_NOTE_NONE, ARP_NOTE_NONE, ARP_NOTE_NONE};
		ArpReturnInstruction instruction;
		instruction.arpNoteOn = &note;
		instruction.noteCodeOffPostArp[0] = 48;
		dispatchArpNoteOffs(instruction, 64);
		dispatchArpNoteOns(instruction, ArpNoteStatusUpdate::BEFORE_NOTE_ON);
		assert(noteOffCalls == 1);
		assert(noteOnCalls == 1);
		assert(note.noteStatus[0] == ArpNoteStatus::PLAYING);
	}
	// 4. min() with kNoEvent must be identity - the tick sites now return
	//    std::min(arpTicks, fxTicks) instead of arpTicks.
	{
		constexpr int32_t kNoEvent = 2147483647;
		const int32_t samples[] = {0, 1, 3, 96, 2147483646, kNoEvent};
		for (int32_t arpTicks : samples) {
			assert((arpTicks < kNoEvent ? arpTicks : kNoEvent) == arpTicks);
		}
	}
	printf("all Stage 1 equivalence checks passed\n");
	return 0;
}
