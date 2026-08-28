/*
 * Copyright © 2026 Rudement
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

class InstrumentClip;

namespace deluge::model::kit_split {

/// How many NoteRows of this clip would be split out - i.e. how many rows have both a Drum and at least one Note.
/// Rows with no notes are not counted; their Drums are discarded along with the source Kit.
int32_t countSplittableRows(InstrumentClip* clip);

/// True if `clip` is a Kit clip with at least two splittable rows.
bool canSplit(InstrumentClip* clip);

/// Split the given Kit clip into one new Kit (and one new session Clip) per splittable row.
///
/// Each new Kit receives exactly one Drum, moved - not copied - out of the source Kit, so no Kit deep-copy is
/// needed and no sample data is duplicated. Per-row FX travel with the Drum because they live in the NoteRow.
/// Kit-global (affect-entire) FX are NOT inherited: each new Clip is handed a freshly initialised kit ParamManager,
/// so the split Kits come up neutral and whatever the song is doing stays with the song.
///
/// On full success the source Clip and its now-empty Kit are removed. If a later step fails, the source Clip is
/// left in place holding whatever Drums remain, so nothing is lost.
///
/// The new Kits and Clips take the source's place - same row order, same neighbours in the session clip list and
/// on the grid - rather than always landing at the front of the list or the leftmost grid columns.
///
/// Must be called with playback stopped. Returns the number of Kits created, or 0 if nothing was done.
int32_t performSplit(InstrumentClip* clip);

} // namespace deluge::model::kit_split
