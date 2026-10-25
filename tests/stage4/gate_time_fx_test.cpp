// Stage 4: what GateTimeFx schedules.
//
// Standalone, same reasoning as the other stage tests. The arpeggiator types are stubbed - the
// real arpeggiator.h drags in most of the firmware - and the three method bodies below are
// copied VERBATIM from modulation/midi_fx/gate_time_fx.cpp with the GateTimeFx:: qualifier
// stripped. If the original changes this copy goes stale; same hazard as tests/stage1.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ranges>

constexpr uint32_t ARP_MAX_INSTRUCTION_NOTES = 4; // uint32_t, matching arpeggiator.h:37 -
                                                  // the copied loops mix it with int32_t and the
                                                  // stub must reproduce that, not tidy it away
constexpr int16_t ARP_NOTE_NONE = 32767;
constexpr uint8_t MIDI_CHANNEL_NONE = 255;
constexpr int32_t kNoEvent = 2147483647;

struct ArpNote {
	std::array<int16_t, ARP_MAX_INSTRUCTION_NOTES> noteCodeOnPostArp{
	    {ARP_NOTE_NONE, ARP_NOTE_NONE, ARP_NOTE_NONE, ARP_NOTE_NONE}};
	std::array<uint8_t, ARP_MAX_INSTRUCTION_NOTES> outputMemberChannel{
	    {MIDI_CHANNEL_NONE, MIDI_CHANNEL_NONE, MIDI_CHANNEL_NONE, MIDI_CHANNEL_NONE}};
};

struct ArpReturnInstruction {
	ArpNote* arpNoteOn{nullptr};
	uint32_t sampleSyncLengthOn{0};
	std::array<int16_t, ARP_MAX_INSTRUCTION_NOTES> noteCodeOffPostArp{
	    {ARP_NOTE_NONE, ARP_NOTE_NONE, ARP_NOTE_NONE, ARP_NOTE_NONE}};
	std::array<uint8_t, ARP_MAX_INSTRUCTION_NOTES> outputMIDIChannelOff{
	    {MIDI_CHANNEL_NONE, MIDI_CHANNEL_NONE, MIDI_CHANNEL_NONE, MIDI_CHANNEL_NONE}};
};

struct MidiFxSettings {
	bool enabled{false};
	uint8_t gateTimePercent{100};
};
struct MidiFxContext {
	MidiFxSettings* settings;
	bool arpIsOn{false};
};

// ---- state, matching the private members of GateTimeFx ----
struct PendingOff {
	int32_t ticksRemaining;
	int16_t noteCode;
	uint8_t outputMemberChannel;
	bool active;
};
static constexpr size_t kMaxPendingOffs = 16;
static std::array<PendingOff, kMaxPendingOffs> pending_{};
static uint32_t lastClipPos_{0};
static bool havePos_{false};

static void resetState() {
	pending_ = {};
	lastClipPos_ = 0;
	havePos_ = false;
}

// ---- VERBATIM FROM gate_time_fx.cpp ----
namespace {
/// 100% means "leave notes alone", so the effect is inert at its own default.
constexpr uint8_t kGateTimeOff = 100;
} // namespace

bool onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction) {
	if (context.settings == nullptr || !context.settings->enabled) {
		return true;
	}
	if (context.settings->gateTimePercent >= kGateTimeOff || context.arpIsOn) {
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
	    std::max<int32_t>(1, static_cast<int32_t>((lengthTicks * context.settings->gateTimePercent) / 100));

	// uint32_t, not int32_t: ARP_MAX_INSTRUCTION_NOTES is unsigned (arpeggiator.h:37) and the
	// surrounding code mixes the two, which is a -Wsign-compare warning under MAINTAINER_MODE.
	for (uint32_t n = 0; n < ARP_MAX_INSTRUCTION_NOTES; n++) {
		const int16_t noteCode = arpNote->noteCodeOnPostArp[n];
		if (noteCode == ARP_NOTE_NONE) {
			break;
		}
		auto slot = std::ranges::find_if(pending_, [](const PendingOff& p) { return !p.active; });
		if (slot == pending_.end()) {
			// Full. The note keeps its written length, which sounds wrong but sounds - dropping an
			// existing entry instead would strand a note-off and leave a note on forever.
			break;
		}
		*slot = PendingOff{.ticksRemaining = gateTicks,
		                   .noteCode = noteCode,
		                   .outputMemberChannel = arpNote->outputMemberChannel[n],
		                   .active = true};
	}

	// Never swallows. The sequencer's own note-off still arrives at the note's written end, and
	// it is deliberately allowed through: a second note-off for an already-released note is
	// harmless on all three outputs, and suppressing it would mean matching note-offs to notes
	// across re-triggers - a good way to swallow the wrong one and hang a note. Worth re-checking
	// under MPE, where a stale note-off could touch a member channel that has been reassigned.
	return true;
}

int32_t tick(const MidiFxContext& /*context*/, uint32_t clipCurrentPos, ArpReturnInstruction& instruction) {
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
	// never comes is a stuck note, and only one of those two generates a bug report.
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

void flush(const MidiFxContext& /*context*/) {
	for (PendingOff& p : pending_) {
		p.active = false;
	}
	havePos_ = false;
}

// ---- END VERBATIM ----

static int countActive() {
	int n = 0;
	for (auto& p : pending_) {
		n += p.active;
	}
	return n;
}
static int countOffs(const ArpReturnInstruction& in) {
	int n = 0;
	for (auto c : in.noteCodeOffPostArp) {
		n += (c != ARP_NOTE_NONE);
	}
	return n;
}

int main() {
	MidiFxSettings settings;
	MidiFxContext ctx{.settings = &settings};

	// --- Inert cases must schedule nothing at all. ---
	{
		ArpNote note;
		note.noteCodeOnPostArp[0] = 60;
		ArpReturnInstruction in{.arpNoteOn = &note, .sampleSyncLengthOn = 96};

		resetState();
		onNoteEvent(ctx, in); // disabled
		assert(countActive() == 0);

		settings.enabled = true;
		settings.gateTimePercent = 100; // 100% is off
		onNoteEvent(ctx, in);
		assert(countActive() == 0);

		settings.gateTimePercent = 50;
		ctx.arpIsOn = true; // the arp has its own Gate
		onNoteEvent(ctx, in);
		assert(countActive() == 0);
		ctx.arpIsOn = false;

		// A live note has no length to take a percentage of.
		ArpReturnInstruction live{.arpNoteOn = &note, .sampleSyncLengthOn = 0};
		onNoteEvent(ctx, live);
		assert(countActive() == 0);
	}

	settings.enabled = true;
	settings.gateTimePercent = 50;

	// --- The basic case: note of length 96 at 50% releases 48 ticks later. ---
	{
		resetState();
		ArpNote note;
		note.noteCodeOnPostArp[0] = 60;
		note.outputMemberChannel[0] = 3;
		ArpReturnInstruction on{.arpNoteOn = &note, .sampleSyncLengthOn = 96};
		onNoteEvent(ctx, on);
		assert(countActive() == 1);

		ArpReturnInstruction t0;
		assert(tick(ctx, 0, t0) == 48); // first visit establishes position, asks for 48
		assert(countOffs(t0) == 0);

		ArpReturnInstruction t1;
		assert(tick(ctx, 24, t1) == 24); // halfway
		assert(countOffs(t1) == 0);

		ArpReturnInstruction t2;
		assert(tick(ctx, 48, t2) == kNoEvent); // due
		assert(countOffs(t2) == 1);
		assert(t2.noteCodeOffPostArp[0] == 60);
		assert(t2.outputMIDIChannelOff[0] == 3);
		assert(countActive() == 0);
	}

	// --- A whole chord schedules a note-off each. ---
	{
		resetState();
		ArpNote chord;
		chord.noteCodeOnPostArp = {60, 64, 67, ARP_NOTE_NONE};
		ArpReturnInstruction on{.arpNoteOn = &chord, .sampleSyncLengthOn = 96};
		onNoteEvent(ctx, on);
		assert(countActive() == 3);
	}

	// --- Never emits into a slot the arp is already using. ---
	{
		resetState();
		ArpNote note;
		note.noteCodeOnPostArp[0] = 60;
		ArpReturnInstruction on{.arpNoteOn = &note, .sampleSyncLengthOn = 96};
		onNoteEvent(ctx, on);

		ArpReturnInstruction t0;
		tick(ctx, 0, t0);

		ArpReturnInstruction due;
		due.noteCodeOffPostArp[0] = 48; // pretend the arp released something this tick
		tick(ctx, 48, due);
		assert(due.noteCodeOffPostArp[0] == 48 && "must not clobber the arp's note-off");
		assert(due.noteCodeOffPostArp[1] == 60);
	}

	// --- More due at once than the instruction can carry: returns 0 to come straight back. ---
	{
		resetState();
		ArpNote a, b;
		a.noteCodeOnPostArp = {60, 61, 62, 63};
		b.noteCodeOnPostArp = {70, 71, ARP_NOTE_NONE, ARP_NOTE_NONE};
		ArpReturnInstruction onA{.arpNoteOn = &a, .sampleSyncLengthOn = 96};
		ArpReturnInstruction onB{.arpNoteOn = &b, .sampleSyncLengthOn = 96};
		onNoteEvent(ctx, onA);
		onNoteEvent(ctx, onB);
		assert(countActive() == 6);

		ArpReturnInstruction t0;
		tick(ctx, 0, t0);
		ArpReturnInstruction due;
		assert(tick(ctx, 48, due) == 0 && "must ask to be actioned again, not drop the rest");
		assert(countOffs(due) == 4);
		assert(countActive() == 2);

		ArpReturnInstruction due2;
		assert(tick(ctx, 48, due2) == kNoEvent);
		assert(countOffs(due2) == 2);
		assert(countActive() == 0 && "nothing may be left stuck");
	}

	// --- A clip loop rewinds the position. Fire everything rather than strand a note-off. ---
	{
		resetState();
		ArpNote note;
		note.noteCodeOnPostArp[0] = 60;
		ArpReturnInstruction on{.arpNoteOn = &note, .sampleSyncLengthOn = 96};
		onNoteEvent(ctx, on);

		ArpReturnInstruction t0;
		tick(ctx, 90, t0);
		assert(countOffs(t0) == 0);

		ArpReturnInstruction wrapped;
		tick(ctx, 2, wrapped); // looped back to the top with the note still pending
		assert(countOffs(wrapped) == 1 && "a rewind must release, not strand");
		assert(countActive() == 0);
	}

	// --- Overflow drops the newest note's gate, never an existing note-off. ---
	{
		resetState();
		ArpNote note;
		note.noteCodeOnPostArp[0] = 60;
		ArpReturnInstruction on{.arpNoteOn = &note, .sampleSyncLengthOn = 96};
		for (size_t i = 0; i < kMaxPendingOffs + 4; i++) {
			onNoteEvent(ctx, on);
		}
		assert(countActive() == (int)kMaxPendingOffs);
	}

	// --- flush() drops everything and emits nothing: the transport already cut those notes. ---
	{
		resetState();
		ArpNote note;
		note.noteCodeOnPostArp[0] = 60;
		ArpReturnInstruction on{.arpNoteOn = &note, .sampleSyncLengthOn = 96};
		onNoteEvent(ctx, on);
		assert(countActive() == 1);
		flush(ctx);
		assert(countActive() == 0);
		ArpReturnInstruction after;
		assert(tick(ctx, 10, after) == kNoEvent);
		assert(countOffs(after) == 0);
	}

	// --- The gate never rounds down to zero ticks: a note that never sounds is not a short note.
	{
		for (uint8_t pct = 5; pct <= 100; pct++) {
			for (uint32_t len = 1; len <= 200; len++) {
				resetState();
				settings.gateTimePercent = pct;
				ArpNote note;
				note.noteCodeOnPostArp[0] = 60;
				ArpReturnInstruction on{.arpNoteOn = &note, .sampleSyncLengthOn = len};
				onNoteEvent(ctx, on);
				if (pct >= 100) {
					assert(countActive() == 0);
					continue;
				}
				assert(countActive() == 1);
				for (auto& p : pending_) {
					if (p.active) {
						assert(p.ticksRemaining >= 1);
						assert(p.ticksRemaining <= (int32_t)len && "shortens only, never extends");
					}
				}
			}
		}
	}

	printf("all Stage 4 gate time checks passed\n");
	return 0;
}
