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

#include "modulation/midi_fx/groove_file.h"

#include "definitions.h"
#include "fatfs/ff.h"
#include "memory/general_memory_allocator.h"
#include "processing/engines/audio_engine.h"
#include "storage/storage_manager.h"

#include <cstdio>
#include <cstring>

// Last, after <cstdio>: the firmware's own snprintf. Plain snprintf would link newlib's, which needs
// file syscalls this firmware does not have.
#include "lib/printf.h"

namespace deluge::modulation::midi_fx::groove::file {

namespace {

PLACE_SDRAM_BSS char names[kMaxFiles][kNameMax];
int32_t numFiles = 0;
bool scannedOnce = false;
uint32_t lastScanTime = 0;

/// Largest file we will read. Live's own grooves are 6-30 KB; a groove clip with thousands of notes is
/// not a groove.
constexpr uint32_t kMaxFileSize = 256 * 1024;

/// About three seconds at 44.1 kHz. Long enough that scrolling the menu never re-reads the card, short
/// enough that a card swapped while the menu is shut is noticed when it is next opened.
constexpr uint32_t kRescanSamples = 3 * 44100;

bool hasAgrExtension(const char* fname) {
	const size_t len = strlen(fname);
	if (len < 5) {
		return false;
	}
	const char* ext = fname + len - 4;
	return ext[0] == '.' && (ext[1] == 'a' || ext[1] == 'A') && (ext[2] == 'g' || ext[2] == 'G')
	       && (ext[3] == 'r' || ext[3] == 'R');
}

int compareNames(const char* a, const char* b) {
	for (;; a++, b++) {
		char ca = *a;
		char cb = *b;
		if (ca >= 'a' && ca <= 'z') {
			ca -= 32;
		}
		if (cb >= 'a' && cb <= 'z') {
			cb -= 32;
		}
		if (ca != cb || ca == 0) {
			return ca - cb;
		}
	}
}

} // namespace

int32_t scan() {
	const uint32_t now = AudioEngine::audioSampleTimer;
	if (scannedOnce && now - lastScanTime < kRescanSamples) {
		return numFiles;
	}
	scannedOnce = true;
	lastScanTime = now;
	numFiles = 0;

	if (!isCardReady()) {
		return 0;
	}
	DIR dir;
	if (f_opendir(&dir, kFolder) != FR_OK) {
		return 0;
	}
	FILINFO fno;
	while (numFiles < kMaxFiles && f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
		if ((fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) || fno.fname[0] == '.' || !hasAgrExtension(fno.fname)) {
			continue;
		}
		char display[kNameMax];
		const size_t stem = strlen(fno.fname) - 4;
		const size_t keep = stem < static_cast<size_t>(kNameMax - 1) ? stem : static_cast<size_t>(kNameMax - 1);
		memcpy(display, fno.fname, keep);
		display[keep] = 0;
		// Insert sorted, so the list reads the way the card's folder does on a computer.
		int32_t at = numFiles;
		while (at > 0 && compareNames(names[at - 1], display) > 0) {
			memcpy(names[at], names[at - 1], kNameMax);
			at--;
		}
		memcpy(names[at], display, kNameMax);
		numFiles++;
	}
	f_closedir(&dir);
	return numFiles;
}

int32_t cachedCount() {
	return scannedOnce ? numFiles : scan();
}

const char* name(int32_t i) {
	return (i >= 0 && i < numFiles) ? names[i] : "";
}

int32_t indexOfName(const char* templateName) {
	for (int32_t i = 0; i < numFiles; i++) {
		if (strcmp(names[i], templateName) == 0) {
			return i;
		}
	}
	return -1;
}

uint8_t load(int32_t i, FileAmounts* amounts) {
	if (i < 0 || i >= numFiles || !isCardReady()) {
		return kOff;
	}
	// The display name was cut to fit; find the file again by its stem rather than trusting the cut.
	DIR dir;
	if (f_opendir(&dir, kFolder) != FR_OK) {
		return kOff;
	}
	FILINFO fno;
	char path[300];
	path[0] = 0;
	while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
		if ((fno.fattrib & AM_DIR) || !hasAgrExtension(fno.fname)) {
			continue;
		}
		const size_t stem = strlen(fno.fname) - 4;
		const size_t keep = stem < static_cast<size_t>(kNameMax - 1) ? stem : static_cast<size_t>(kNameMax - 1);
		if (keep == strlen(names[i]) && strncmp(fno.fname, names[i], keep) == 0) {
			snprintf(path, sizeof(path), "%s/%s", kFolder, fno.fname);
			break;
		}
	}
	f_closedir(&dir);
	if (path[0] == 0) {
		return kOff;
	}

	FIL fil;
	if (f_open(&fil, path, FA_READ) != FR_OK) {
		return kOff;
	}
	const uint32_t size = f_size(&fil);
	if (size == 0 || size > kMaxFileSize) {
		f_close(&fil);
		return kOff;
	}
	char* buffer = static_cast<char*>(GeneralMemoryAllocator::get().allocLowSpeed(size + 1));
	if (buffer == nullptr) {
		f_close(&fil);
		return kOff;
	}
	UINT got = 0;
	const FRESULT result = f_read(&fil, buffer, size, &got);
	f_close(&fil);

	uint8_t id = kOff;
	// Live 11 and 12 write plain XML. Older Live gzipped its files; those start 1F 8B and are refused
	// here rather than misread - open the groove in a current Live and save it again.
	const bool gzipped = got >= 2 && static_cast<uint8_t>(buffer[0]) == 0x1F && static_cast<uint8_t>(buffer[1]) == 0x8B;
	if (result == FR_OK && got == size && !gzipped) {
		buffer[got] = 0;
		Template t{};
		if (parseAgr(buffer, got, names[i], t)) {
			id = internUser(t);
			if (amounts != nullptr) {
				parseAgrAmounts(buffer, got, *amounts);
			}
		}
	}
	delugeDealloc(buffer);
	return id;
}

} // namespace deluge::modulation::midi_fx::groove::file
