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

#include "gui/ui/keyboard/chord_mem_service.h"
#include "gui/ui/keyboard/chords.h"
#include "gui/ui/keyboard/keyboard_screen.h"
#include "gui/ui/keyboard/layout.h"
#include "gui/ui/keyboard/notes_state.h"
#include "hid/display/display.h"
#include "model/song/song.h"

namespace deluge::gui::ui::keyboard::ChordMemService {

namespace {
// Sentinel written into highlightedNotes[] for a recalled chord. The keyboard layouts render 255 as a
// full-white "shape" highlight (distinct from velocity-tinted incoming-note highlights, which are <=127).
constexpr uint8_t kChordMemHighlightBrightness = 255;
// Which slot's shape is currently lit on the grid (0xFF = none). Module-level: there is a single grid
// highlight buffer, so one highlight is shared across whatever view drives the memory.
uint8_t highlightSlot = 0xFF;
} // namespace

uint8_t noteCount(int32_t slot) {
	return currentSong->chordMemNoteCount[slot];
}

// Read a slot's notes without sounding them (for LEARN / inspection). Returns the note count.
uint8_t peek(int32_t slot, uint8_t* out, uint8_t maxOut) {
	uint8_t n = currentSong->chordMemNoteCount[slot];
	if (n > maxOut) {
		n = maxOut;
	}
	for (uint8_t i = 0; i < n && i < MAX_NOTES_CHORD_MEM; i++) {
		out[i] = currentSong->chordMem[slot][i];
	}
	return n;
}

// Clear the currently-highlighted chord's notes from the grid highlight array.
void clearHighlight() {
	if (highlightSlot == 0xFF) {
		return;
	}
	for (int i = 0; i < currentSong->chordMemNoteCount[highlightSlot] && i < MAX_NOTES_CHORD_MEM; i++) {
		uint8_t note = currentSong->chordMem[highlightSlot][i];
		if (note < kHighestKeyboardNote) {
			keyboardScreen.highlightedNotes[note] = 0;
		}
	}
	highlightSlot = 0xFF;
	keyboardScreen.requestRendering();
}

// Persistently light a stored chord's notes on the grid so its shape stays visible after release.
void setHighlight(int32_t slot) {
	clearHighlight();
	for (int i = 0; i < currentSong->chordMemNoteCount[slot] && i < MAX_NOTES_CHORD_MEM; i++) {
		uint8_t note = currentSong->chordMem[slot][i];
		if (note < kHighestKeyboardNote) {
			keyboardScreen.highlightedNotes[note] = kChordMemHighlightBrightness;
		}
	}
	highlightSlot = slot;
	keyboardScreen.requestRendering();
}

bool recall(int32_t slot, KeyboardLayout* layout) {
	NotesState& notesState = layout->getNotesState();
	auto count = currentSong->chordMemNoteCount[slot];
	for (int i = 0; i < count && i < MAX_NOTES_CHORD_MEM; i++) {
		// generatedNote=true suppresses the per-note name display so the chord NAME (shown below) wins
		// on the iso/in-key grids, matching how the chord library shows chord names.
		notesState.enableNote(currentSong->chordMem[slot][i], layout->velocity, true);
	}
	// Light the recalled chord's shape on the grid; it persists after release.
	if (count > 0) {
		setHighlight(slot);
		// Name the recalled chord and show it (so the slot teaches you what it is), spelled with flats or
		// sharps to match the song's key.
		char chordName[48];
		bool preferFlats = effectivePreferFlats(currentSong->key.rootNote % kOctaveSize, currentSong->key.modeNotes);
		if (nameChordFromNotes(currentSong->chordMem[slot], count, chordName, preferFlats)) {
			if (display->haveOLED()) {
				display->popupTextTemporary(chordName);
			}
			else {
				display->setScrollingText(chordName);
			}
		}
	}
	return count > 0;
}

void store(int32_t slot, NotesState& notesState) {
	// Slot contents about to change — drop its stale highlight first.
	if (highlightSlot == slot) {
		clearHighlight();
	}
	auto count = notesState.count;
	for (int i = 0; i < count && i < MAX_NOTES_CHORD_MEM; i++) {
		currentSong->chordMem[slot][i] = notesState.notes[i].note;
	}
	currentSong->chordMemNoteCount[slot] = count;
}

void clearSlot(int32_t slot) {
	if (highlightSlot == slot) {
		clearHighlight();
	}
	currentSong->chordMemNoteCount[slot] = 0;
}

} // namespace deluge::gui::ui::keyboard::ChordMemService
