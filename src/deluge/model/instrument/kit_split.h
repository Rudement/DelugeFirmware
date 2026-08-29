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

/// How many new Kits a split of this clip's Kit would produce - i.e. how many of the Kit's Drums have notes in at
/// least one Clip on that Kit. Drums with no notes anywhere are not counted; they are discarded with the source Kit.
int32_t countSplittableRows(InstrumentClip* clip);

/// True if `clip` is a Kit clip with at least two splittable rows.
bool canSplit(InstrumentClip* clip);

/// Split the Kit this clip belongs to into one new Kit per splittable Drum.
///
/// The unit of the operation is the Kit, not the Clip it was invoked from: EVERY session Clip on that Kit is split
/// together, and each new Kit receives one Clip per source Clip, carrying only that Drum's row and keeping its
/// original section. A part and its variations therefore come out stacked in the same Grid columns, the way they
/// went in. Splitting one Clip at a time is not an option - the Drums move, so a sibling Clip left behind would be
/// holding rows that point into other Kits.
///
/// Each new Kit receives exactly one Drum, moved - not copied - out of the source Kit, so no Kit deep-copy is
/// needed and no sample data is duplicated. Per-row FX travel with the Drum because they live in the NoteRow.
/// Kit-global (affect-entire) FX are NOT inherited: each new Clip is handed a freshly initialised kit ParamManager,
/// so the split Kits come up neutral and whatever the song is doing stays with the song. Volume and pan are the
/// exception and are carried across, per source Clip.
///
/// On full success the source Clips and their now-empty Kit are removed. If a later step fails, the source Clips are
/// left in place holding whatever Drums remain, so nothing is lost.
///
/// The new Kits and Clips take the source's place - same row order, same neighbours in the session clip list and
/// on the grid - rather than always landing at the front of the list or the leftmost grid columns.
///
/// Playback may be running, but nothing on the source Kit may be playing - mute the track first. The new Clips
/// arrive muted and at position zero; launch them when you want them.
///
/// Returns the number of Kits created, or 0 if nothing was done.
int32_t performSplit(InstrumentClip* clip);

} // namespace deluge::model::kit_split
