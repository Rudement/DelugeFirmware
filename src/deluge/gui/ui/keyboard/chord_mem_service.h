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

#pragma once

#include <cstdint>

namespace deluge::gui::ui::keyboard {

class KeyboardLayout;
struct NotesState;

/// @brief Shared chord-memory service over the Song's chordMem store.
///
/// Store / recall / clear chords, persist the recalled-chord SHAPE highlight on the grid, and name recalled
/// chords — the logic that used to live inside SongChordMemColumn, lifted out so multiple views (the Song
/// Chord Memory sidebar column today, the center Chord/Harmony Bank later) drive the SAME memory without
/// duplication. ONE harmonic memory, many views. Behaviour is identical to the original column code.
namespace ChordMemService {

/// Recall slot's chord: sound its notes into @p layout, persist its shape highlight, and name it.
/// Returns true if the slot had any notes.
bool recall(int32_t slot, KeyboardLayout* layout);

/// Store the layout's currently-played notes into @p slot.
void store(int32_t slot, NotesState& notesState);

/// Empty a slot.
void clearSlot(int32_t slot);

/// Number of notes stored in a slot.
uint8_t noteCount(int32_t slot);

/// Read a slot's notes without sounding them (for LEARN / inspection). Returns the note count.
uint8_t peek(int32_t slot, uint8_t* out, uint8_t maxOut);

/// Persist a recalled chord's shape on the grid (writes into the keyboard's highlightedNotes[]).
void setHighlight(int32_t slot);
/// Clear the currently-lit recalled-chord shape.
void clearHighlight();

} // namespace ChordMemService
} // namespace deluge::gui::ui::keyboard
