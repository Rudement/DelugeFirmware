// Stage 3: what VelocityFx decides.
//
// Standalone, same reasoning as tests/stage1 and tests/stage2 - the tests/ project needs -m32
// and CppUTest and this needs neither.
//
// ArpNote is stubbed rather than included, because pulling in the real arpeggiator.h drags in
// most of the firmware. Only the two fields the effect touches are modelled, and the stub is
// checked against the real definition by eye at arpeggiator.h:155-184. If those fields change
// type this stub goes stale - same hazard as tests/stage1, same warning.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>

struct ArpNote {
	uint8_t velocity{0};
	uint8_t baseVelocity{0};
};
struct ArpReturnInstruction {
	ArpNote* arpNoteOn{nullptr};
};

struct MidiFxSettings {
	bool enabled{false};
	int8_t velocityOffset{0};
};
struct MidiFxContext {
	MidiFxSettings* settings;
};

// Body copied verbatim from modulation/midi_fx/velocity_fx.cpp.
static bool onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction) {
	if (context.settings == nullptr || !context.settings->enabled) {
		return true;
	}
	const int32_t offset = context.settings->velocityOffset;
	if (offset == 0) {
		return true;
	}
	ArpNote* arpNote = instruction.arpNoteOn;
	if (arpNote == nullptr) {
		return true;
	}
	arpNote->velocity = static_cast<uint8_t>(std::clamp<int32_t>(arpNote->baseVelocity + offset, 1, 127));
	return true;
}

int main() {
	MidiFxSettings settings;
	MidiFxContext ctx{.settings = &settings};

	// Disabled, and enabled-with-zero-offset, must not touch the note at all. This is what keeps
	// the arpeggiator's own velocity spread intact whenever MIDI FX velocity is idle.
	{
		ArpNote note{.velocity = 100, .baseVelocity = 64};
		ArpReturnInstruction in{.arpNoteOn = &note};
		assert(onNoteEvent(ctx, in));
		assert(note.velocity == 100 && "disabled must not write");

		settings.enabled = true;
		settings.velocityOffset = 0;
		assert(onNoteEvent(ctx, in));
		assert(note.velocity == 100 && "zero offset must not write");
	}

	settings.enabled = true;

	// Offsets both ways, anchored on baseVelocity.
	{
		settings.velocityOffset = 20;
		ArpNote note{.velocity = 64, .baseVelocity = 64};
		ArpReturnInstruction in{.arpNoteOn = &note};
		onNoteEvent(ctx, in);
		assert(note.velocity == 84);

		settings.velocityOffset = -20;
		note = {.velocity = 64, .baseVelocity = 64};
		onNoteEvent(ctx, in);
		assert(note.velocity == 44);
	}

	// THE ONE THAT DROVE THE DESIGN. The seam runs before every dispatch, and with the arp off a
	// Sound that could not get a voice leaves the note PENDING - handlePendingNotes hands the same
	// array element back on a later render. Running twice must equal running once.
	{
		settings.velocityOffset = 20;
		ArpNote note{.velocity = 64, .baseVelocity = 64};
		ArpReturnInstruction in{.arpNoteOn = &note};
		onNoteEvent(ctx, in);
		const uint8_t once = note.velocity;
		onNoteEvent(ctx, in);
		onNoteEvent(ctx, in);
		assert(note.velocity == once && "must be idempotent, or a stolen voice doubles the offset");
	}

	// Clamped at both ends. The floor is 1, not 0: velocity 0 is note-off on MIDI, so a note-on
	// at 0 would become a note-off for a note that never started and nothing would release it.
	{
		settings.velocityOffset = -64;
		ArpNote note{.velocity = 1, .baseVelocity = 1};
		ArpReturnInstruction in{.arpNoteOn = &note};
		onNoteEvent(ctx, in);
		assert(note.velocity == 1 && "must never emit velocity 0");

		settings.velocityOffset = 63;
		note = {.velocity = 127, .baseVelocity = 127};
		onNoteEvent(ctx, in);
		assert(note.velocity == 127 && "must not wrap past 127");
	}

	// Whole legal input range, exhaustively: never 0, never over 127, never wraps, and always
	// idempotent.
	//
	// Note the seed - velocity starts at base, as it does in the firmware, rather than at 0. The
	// first version of this seeded velocity to 0 and asserted >= 1 for every offset, which fails
	// at offset 0 for the good reason that the effect deliberately writes nothing there. The
	// assertion was wrong, not the effect, but it is exactly the shape of mistake that would hide
	// a real off-by-one, so it is worth the comment.
	{
		for (int32_t off = -64; off <= 63; off++) {
			settings.velocityOffset = static_cast<int8_t>(off);
			for (int32_t base = 1; base <= 127; base++) {
				ArpNote note{.velocity = static_cast<uint8_t>(base), .baseVelocity = static_cast<uint8_t>(base)};
				ArpReturnInstruction in{.arpNoteOn = &note};
				onNoteEvent(ctx, in);
				assert(note.velocity >= 1 && note.velocity <= 127);
				const uint8_t once = note.velocity;
				onNoteEvent(ctx, in);
				assert(note.velocity == once);
			}
		}
	}

	// A note-off carries no arpNoteOn. Must not fault.
	{
		ArpReturnInstruction in{.arpNoteOn = nullptr};
		assert(onNoteEvent(ctx, in));
	}

	printf("all Stage 3 velocity checks passed\n");
	return 0;
}
