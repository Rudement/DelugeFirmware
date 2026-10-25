// Stage 2 chain semantics. The chain is the only thing in Stage 2 with behaviour, so it is the
// only thing that can be tested without a Deluge attached.
#include "modulation/midi_fx/midi_fx_chain.h"
#include <cassert>
#include <cstddef>
#include <cstdio>
class ArpReturnInstruction {};
namespace mf = deluge::modulation::midi_fx;

// MidiFxChain owns its effects by value, so linking needs their definitions — but both .cpp
// files include arpeggiator.h, which reaches <expected> and will not build on a host g++ older
// than 12. Stubbing them here keeps this test about the chain, which is all it claims to test.
// The effects' own behaviour is covered by tests/stage3 and tests/stage4, which stub in the
// other direction. Every effect the chain gains needs a stub added here, and the link error if
// you forget is loud.
bool mf::VelocityFx::onNoteEvent(const mf::MidiFxContext&, ArpReturnInstruction&) {
	return true;
}
bool mf::GateTimeFx::onNoteEvent(const mf::MidiFxContext&, ArpReturnInstruction&) {
	return true;
}
int32_t mf::GateTimeFx::tick(const mf::MidiFxContext&, uint32_t, ArpReturnInstruction&) {
	return mf::kNoEvent;
}
bool mf::GateTimeFx::flush(const mf::MidiFxContext&, ArpReturnInstruction&) {
	return false;
}
bool mf::RetrigFx::onNoteEvent(const mf::MidiFxContext&, ArpReturnInstruction&) {
	return true;
}
void mf::RetrigFx::render(const mf::MidiFxContext&, int32_t, ArpReturnInstruction&) {
}
bool mf::RetrigFx::flush(const mf::MidiFxContext&, ArpReturnInstruction&) {
	return false;
}

struct Recorder : mf::MidiFx {
	int notes = 0, renders = 0, ticks = 0, flushes = 0;
	bool swallow = false;
	int32_t nextEvent = mf::kNoEvent;
	bool onNoteEvent(const mf::MidiFxContext&, ArpReturnInstruction&) override {
		notes++;
		return !swallow;
	}
	void render(const mf::MidiFxContext&, int32_t, ArpReturnInstruction&) override { renders++; }
	int32_t tick(const mf::MidiFxContext&, uint32_t, ArpReturnInstruction&) override {
		ticks++;
		return nextEvent;
	}
	bool flush(const mf::MidiFxContext&, ArpReturnInstruction&) override {
		flushes++;
		return false;
	}
};

// install() is called from MidiFxChain's own constructor, so a chain of stand-ins has to be a
// subclass. Note this inherits the real chain's own effects too, so kOwnEffects of the
// kMaxEffects slots are spoken for before a test adds anything. That is the point of the
// overflow check below - and a reminder that kMaxEffects has to grow with the effect set.
constexpr size_t kOwnEffects = 4; // VelocityFx, GateTimeFx, RetrigFx, DelayFx

struct TestChain : mf::MidiFxChain {
	TestChain(std::initializer_list<mf::MidiFx*> fx) {
		for (auto* f : fx) {
			install(f);
		}
	}
};

int main() {
	mf::MidiFxSettings settings;
	mf::MidiFxContext ctx{.settings = &settings};
	ArpReturnInstruction in;

	Recorder a, b;
	TestChain chain{&a, &b};
	assert(!chain.isEmpty());

	// A chain built by the firmware is never empty either - it always carries VelocityFx. The
	// isEmpty() early-outs at the call sites are therefore about future-proofing, not a live
	// optimisation, and the cost of an idle chain is VelocityFx's own two-comparison early return.
	mf::MidiFxChain stock;
	assert(!stock.isEmpty());

	// Every effect sees a note nobody swallows.
	assert(chain.onNoteEvent(ctx, in));
	assert(a.notes == 1 && b.notes == 1);

	// A swallowed note stops the chain dead - later effects must not see it.
	a.swallow = true;
	assert(!chain.onNoteEvent(ctx, in));
	assert(a.notes == 2 && b.notes == 1);
	a.swallow = false;

	// tick() returns the soonest event across the chain, not the last one.
	a.nextEvent = 96;
	b.nextEvent = 24;
	assert(chain.tick(ctx, 0, in) == 24);
	b.nextEvent = mf::kNoEvent;
	assert(chain.tick(ctx, 0, in) == 96);
	a.nextEvent = mf::kNoEvent;
	assert(chain.tick(ctx, 0, in) == mf::kNoEvent);

	// render and flush reach everything.
	chain.render(ctx, 64, in);
	chain.flush(ctx, in);
	assert(a.renders == 1 && b.renders == 1);
	assert(a.flushes == 1 && b.flushes == 1);

	// Overflow drops silently rather than faulting in the audio path. The chain's own effects
	// are installed first, so only kMaxEffects - kOwnEffects stand-ins get in.
	Recorder many[mf::kMaxEffects + 2];
	TestChain full{&many[0], &many[1], &many[2], &many[3], &many[4], &many[5]};
	full.render(ctx, 64, in);
	int reached = 0;
	for (auto& r : many) {
		reached += (r.renders > 0);
	}
	assert(reached == (int)(mf::kMaxEffects - kOwnEffects));

	printf("all Stage 2 chain checks passed\n");
	return 0;
}
