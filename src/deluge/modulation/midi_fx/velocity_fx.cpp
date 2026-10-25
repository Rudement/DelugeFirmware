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

#include "modulation/midi_fx/velocity_fx.h"
#include "modulation/arpeggiator.h"
#include <algorithm>

namespace deluge::modulation::midi_fx {

bool VelocityFx::onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction) {
	if (context.settings == nullptr || !context.settings->enabled) {
		return true;
	}
	const int32_t offset = context.settings->velocityOffset;
	if (offset == 0) {
		// Nothing to do, and returning here is what keeps arp velocity spread intact whenever
		// this effect is idle. See the note on the declaration.
		return true;
	}
	ArpNote* arpNote = instruction.arpNoteOn;
	if (arpNote == nullptr) {
		return true;
	}

	// Floor of 1, not 0. Velocity 0 is note-off on MIDI, so emitting it would be a hanging note
	// wearing a disguise - the note-on would silently become a note-off for a note that never
	// started, and nothing downstream would ever release it.
	arpNote->velocity = static_cast<uint8_t>(std::clamp<int32_t>(arpNote->baseVelocity + offset, 1, 127));
	return true;
}

} // namespace deluge::modulation::midi_fx
