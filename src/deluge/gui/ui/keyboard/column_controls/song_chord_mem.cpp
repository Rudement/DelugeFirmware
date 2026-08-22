/*
 * Copyright © 2016-2024 Synthstrom Audible Limited
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

#include "song_chord_mem.h"
#include "gui/ui/keyboard/chord_mem_service.h"
#include "gui/ui/keyboard/layout/column_controls.h"
#include "hid/buttons.h"
#include "model/song/song.h"

namespace deluge::gui::ui::keyboard::controls {

void SongChordMemColumn::renderColumn(RGB image[][kDisplayWidth + kSideBarWidth], int32_t column,
                                      KeyboardLayout* layout) {
	uint8_t otherChannels = 0;
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		bool chord_selected = y == activeChordMem;
		uint8_t chord_slot_filled = currentSong->chordMemNoteCount[y] > 0 ? 0x7f : 0;
		otherChannels = chord_selected ? 0xf0 : 0;
		uint8_t base = chord_selected ? 0xff : chord_slot_filled;
		image[y][column] = {otherChannels, base, base};
	}
}

bool SongChordMemColumn::handleVerticalEncoder(int8_t pad, int32_t offset) {
	return false;
};

void SongChordMemColumn::handleLeavingColumn(ModelStackWithTimelineCounter* modelStackWithTimelineCounter,
                                             KeyboardLayout* layout){};

void SongChordMemColumn::handlePad(ModelStackWithTimelineCounter* modelStackWithTimelineCounter, PressedPad pad,
                                   KeyboardLayout* layout) {
	NotesState& currentNotesState = layout->getNotesState();

	if (pad.active) {
		// Press: recall the slot (sound it, light its shape, name it) — all in ChordMemService.
		activeChordMem = pad.y;
		ChordMemService::recall(pad.y, layout);
	}
	else {
		activeChordMem = 0xFF;
		// Release: store the held notes into the slot if it's empty (or Shift), else Shift clears it.
		if ((!ChordMemService::noteCount(pad.y) || Buttons::isShiftButtonPressed()) && currentNotesState.count) {
			ChordMemService::store(pad.y, currentNotesState);
		}
		else if (Buttons::isShiftButtonPressed()) {
			ChordMemService::clearSlot(pad.y);
		}
	}
};

} // namespace deluge::gui::ui::keyboard::controls
