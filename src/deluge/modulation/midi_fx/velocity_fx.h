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

#include "modulation/midi_fx/midi_fx.h"

/// Velocity offset — the cheapest of the RS7000 Play FX set, and the first one built.
///
/// It needs no clock and no note pool. `sendNote` already runs every note through the
/// arpeggiator unconditionally and the arp passes through when `mode == OFF`, so this works on
/// the note-event axis alone and would have worked before Stage 1 existed. That is exactly why
/// it goes first: if something misbehaves, the fault is the page or the seam, not the effect.
namespace deluge::modulation::midi_fx {

class VelocityFx final : public MidiFx {
public:
	/// Rewrites the outgoing velocity. Never swallows a note, so it always returns true.
	///
	/// **Computed from `baseVelocity`, not from the current velocity, and that is deliberate.**
	/// The seam runs immediately before every dispatch, and there is one path where the same
	/// `ArpNote` reaches it twice: with the arp off, a Sound that could not get a voice leaves
	/// the note PENDING, and `handlePendingNotes` hands the very same array element back on a
	/// later render. Offsetting the running value would apply twice. Anchoring on `baseVelocity`
	/// — which the arpeggiator sets once per note-on and never touches again — makes this
	/// idempotent, so it does not matter how many times it runs.
	///
	/// The cost of that choice: when the arp's own velocity spread is also active, this
	/// overrides it rather than stacking with it, because the spread lives in the difference
	/// between `baseVelocity` and `velocity` and anchoring on the base discards it. Only
	/// happens when a non-zero offset is dialled in — at zero the effect returns early and
	/// touches nothing, so spread behaves exactly as before. Stacking the two is a follow-on,
	/// and wants a decision about which should win rather than an accident of ordering.
	bool onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction) override;
};

} // namespace deluge::modulation::midi_fx
