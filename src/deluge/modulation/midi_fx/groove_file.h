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

#include "modulation/midi_fx/groove.h"

/// Ableton groove files on the SD card, in /GROOVES. UI thread only - these read the card.
///
/// A chosen file is copied into the session library and from there into the song when it is saved, so a
/// song keeps its groove even if the file is later deleted or the card is swapped.
namespace deluge::modulation::midi_fx::groove::file {

constexpr const char* kFolder = "GROOVES";
constexpr int32_t kMaxFiles = 64;

/// Re-reads the folder if it has not been read in the last few seconds. Returns how many .agr files
/// it holds (up to kMaxFiles), sorted by name.
int32_t scan();
/// The count from the last scan, scanning only if there has never been one. For display code that
/// runs on every redraw and should not touch the card.
int32_t cachedCount();
/// The display name (file name without .agr) of file i from the last scan.
const char* name(int32_t i);
/// Loads file i into the library. Returns its template id, or kOff if it could not be read. If
/// `amounts` is given it receives the Groove Pool amounts the file was saved with.
uint8_t load(int32_t i, FileAmounts* amounts = nullptr);
/// The file index whose name matches a library template, or -1.
int32_t indexOfName(const char* templateName);

} // namespace deluge::modulation::midi_fx::groove::file
