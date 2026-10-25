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

#include "modulation/midi_fx/delay_fx.h"
#include "modulation/midi_fx/gate_time_fx.h"
#include "modulation/midi_fx/midi_fx.h"
#include "modulation/midi_fx/note_pool.h"
#include "modulation/midi_fx/retrig_fx.h"
#include "modulation/midi_fx/velocity_fx.h"
#include <array>
#include <cstddef>

namespace deluge::modulation::midi_fx {

/// Chain capacity. Sized for the v1 effect set — velocity, gate time, retrigger, delay — plus
/// one spare. A fixed array rather than a container: no allocation anywhere on the note path.
constexpr size_t kMaxEffects = 5;

/// Holds the effects for one instrument and fans the three axes out across them.
///
/// Lives on `MelodicInstrument`, next to `arpeggiator`, because that is where per-instrument
/// note state already lives. The *settings* live on `InstrumentClip` next to `arpSettings`, so
/// they are per-clip and save with the song. Same split the arpeggiator uses.
///
/// Effects are owned by value and registered in the constructor. Nothing is allocated, and the
/// order they are installed in is the order notes pass through them.
///
/// **Invariant every call site depends on: the note-event axis runs immediately before every
/// dispatch, on every path.** Notes reach the output from more places than `sendNote` — an
/// arpeggiating note is emitted from `render()` and `doTickForwardForArp()` instead — so an
/// effect wired only into `sendNote` would work with the arp off and silently do nothing with
/// it on. Effects must also tolerate being run more than once on the same `ArpNote`, because a
/// note left PENDING for want of a voice comes back round; see `VelocityFx::onNoteEvent`.
class MidiFxChain {
public:
	/// Registration order is the order notes pass through. Velocity first because it only
	/// rewrites the note about to go out; Gate Time second because it schedules against that
	/// note and should see the velocity it will actually be played at.
	MidiFxChain() {
		install(&velocity_);
		install(&gateTime_);
		// Retrigger before Delay: it copies the note record as it will actually be played, so
		// velocity must already have been applied to it.
		install(&retrig_);
		// Delay last, so its repeats carry whatever the effects before it did to the note. It also
		// means a retriggered hit can feed the delay, which is a real combination rather than an
		// accident — bounded, because the delay refuses to arm from a record the pool issued.
		install(&delay_);
	}

	/// True when there is nothing to do. Never true for a chain the firmware built — the effects
	/// are always installed — so the early-outs at the call sites are future-proofing rather than
	/// a live optimisation. The real cost of an idle chain is each effect's own early return.
	[[nodiscard]] bool isEmpty() const { return count_ == 0; }

	bool onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction);
	void render(const MidiFxContext& context, int32_t numSamples, ArpReturnInstruction& instruction);
	[[nodiscard]] int32_t tick(const MidiFxContext& context, uint32_t clipCurrentPos,
	                           ArpReturnInstruction& instruction);

	/// Returns true when there are still notes to release and the four note-off slots ran out.
	bool flush(const MidiFxContext& context, ArpReturnInstruction& instruction);

	/// The records this chain's effects issued for notes that outlive their key. Const, because
	/// the only caller outside the chain is the MPE member-channel survey in
	/// `MIDIInstrument::noteOnPostArp`, which counts them and must not disturb them.
	[[nodiscard]] const NotePool& notePool() const { return pool_; }

protected:
	/// Registers an effect. Called from the constructor; protected only so the tests can build a
	/// chain with stand-ins. Silently ignores anything past `kMaxEffects`: dropping an effect is
	/// bad, a hard fault in the audio path is worse.
	void install(MidiFx* effect);

private:
	/// Injects the pool into a copy of the caller's context. The pool belongs to the chain, so no
	/// call site has to know about it — the alternative was threading it through five places that
	/// construct a MidiFxContext, each of which would be a place to forget it.
	[[nodiscard]] MidiFxContext withPool(const MidiFxContext& context) {
		MidiFxContext local = context;
		local.pool = &pool_;
		return local;
	}

	NotePool pool_;

	VelocityFx velocity_;
	GateTimeFx gateTime_;
	RetrigFx retrig_;
	DelayFx delay_;

	std::array<MidiFx*, kMaxEffects> effects_{};
	size_t count_{0};
};

} // namespace deluge::modulation::midi_fx
