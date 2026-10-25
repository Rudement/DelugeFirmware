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

#include "modulation/midi_fx/midi_fx_chain.h"
#include <algorithm>

namespace deluge::modulation::midi_fx {

void MidiFxChain::install(MidiFx* effect) {
	if (count_ >= kMaxEffects) {
		return;
	}
	effects_[count_++] = effect;
}

bool MidiFxChain::onNoteEvent(const MidiFxContext& context, ArpReturnInstruction& instruction) {
	const MidiFxContext local = withPool(context);
	for (size_t i = 0; i < count_; i++) {
		// First effect to swallow the event ends the chain. The ones after it never see the
		// note, which is the point: a swallowed note did not happen.
		if (!effects_[i]->onNoteEvent(local, instruction)) {
			return false;
		}
	}
	return true;
}

void MidiFxChain::render(const MidiFxContext& context, int32_t numSamples, ArpReturnInstruction& instruction) {
	const MidiFxContext local = withPool(context);
	for (size_t i = 0; i < count_; i++) {
		effects_[i]->render(local, numSamples, instruction);
	}
}

int32_t MidiFxChain::tick(const MidiFxContext& context, uint32_t clipCurrentPos, ArpReturnInstruction& instruction) {
	const MidiFxContext local = withPool(context);
	int32_t soonest = kNoEvent;
	for (size_t i = 0; i < count_; i++) {
		soonest = std::min(soonest, effects_[i]->tick(local, clipCurrentPos, instruction));
	}
	return soonest;
}

bool MidiFxChain::flush(const MidiFxContext& context, ArpReturnInstruction& instruction) {
	// Deliberately not gated on isEmpty() or on settings->enabled. Flush must work when the
	// effect has just been switched off, which is exactly when it is most needed and exactly
	// when a gate on "enabled" would skip it.
	const MidiFxContext local = withPool(context);
	bool moreToDo = false;
	for (size_t i = 0; i < count_; i++) {
		moreToDo |= effects_[i]->flush(local, instruction);
	}
	return moreToDo;
}

} // namespace deluge::modulation::midi_fx
