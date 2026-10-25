/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
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
#include "definitions_cxx.hpp"
#include "gui/ui/keyboard/chords.h"
#include "gui/ui/keyboard/layout/column_control_state.h"
#include "storage/flash_storage.h"

namespace deluge::gui::ui::keyboard {

constexpr int32_t kDefaultIsometricRowInterval = 5;
struct KeyboardStateIsomorphic {
	int32_t scrollOffset = (60 - (kDisplayHeight >> 2) * kDefaultIsometricRowInterval);
	int32_t rowInterval = kDefaultIsometricRowInterval;
};

struct KeyboardStateDrums {
	int32_t scroll_offset = 0;
	int32_t zoom_level = 8;
};

constexpr int32_t kDefaultInKeyRowInterval = 3;
struct KeyboardStateInKey {
	// Init scales have 7 elements, multipled by three octaves gives us C1 as first pad
	int32_t scrollOffset = (7 * 3);
	int32_t rowInterval = kDefaultInKeyRowInterval;
};

struct KeyboardStatePiano {
	// default octave = 1 (0 = -2oct), use a vertical scroll to change it
	int32_t scrollOffset = 3;
	// default note=0 (C)
	int32_t noteOffset = 0;
};

struct KeyboardStateChordLibrary {
	int32_t rowInterval = kOctaveSize;
	int32_t scrollOffset = 0;
	int32_t noteOffset = (rowInterval * 4);
	int32_t rowColorMultiplier = 5;
	ChordList chordList{};
};

struct KeyboardStateChord {
	int32_t noteOffset = (kOctaveSize * 4);
	int32_t modOffset = 0;
	int32_t scaleOffset = 0;
	bool autoVoiceLeading = false;
};

// Harmonic layout: degree columns × richness rows. Horizontal scroll moves through scale degrees;
// octaveBase sets the register of the tonic (MIDI tonic = octaveBase*12 + key root pitch class).
struct KeyboardStateHarmonic {
	int32_t scrollSteps = 0;   // horizontal scroll, in scale-degree steps (0 = tonic at column 0)
	int32_t octaveBase = 3;    // PALETTE octave: register the chords are built in (pal-ctrl OCT+/- buttons)
	int32_t isoOctave = 3;     // ISO octave SHOWN: the iso scrolls independently (vertical encoder)
	bool isoChromatic = false; // right panel: false = in-key (matches In-Key kbd), true = standard chromatic iso
	bool stickyChord = false;  // true = selected chord shape persists through iso free-play; false = clears
	bool calculatorOn = true;  // true = show next-chord brain suggestions on the left
	bool showChord = true;     // true = light the selected chord's shape on the iso; false = clean grid
	bool swapped = false;      // handedness: false = palette LEFT / iso RIGHT; true = mirror the two blocks
	bool latticeOn = false;    // true = light the FULL chord lattice (every repeat) with an upward fade
	bool diffOn = false; // DIFF/voice-leading view: prev->current motion (common hold, arriving breathe, leaving ghost)
	uint16_t prevChordPcMask = 0; // pitch-class set of the PREVIOUS chord — drives the DIFF / voice-leading view
	int8_t prevSelDeg = -1;       // degree of the PREVIOUS chord — so DIFF ghosts wear that chord's palette colour
	bool spreadRows = false;      // true = bottom 2 iso rows become the spread/voicing strip; false = full iso
	bool editVoicing = false;     // true = iso taps TOGGLE notes in/out of the selected chord's voicing
	uint8_t voiceOctaves = 0x08;  // octave STACK bitmask: bits 0..6 = octave offsets -3..+3; bit3 (base) always on
	bool stackPick = false;       // true = iso bottom row is the octave-PICKER strip (toggle which octaves)
	int8_t voiceSpread = 0;       // SPREAD: 0..3 lowest notes dropped an octave to open the voicing (drop-root)
	int8_t voiceInversion = 0;    // INVERSION: rotate the chord — move the lowest N notes up an octave
	int8_t voicingWalk = 0; // VOICING DIAL: walk the voicing ±N one-note-at-a-time; springs to voicingHome on release
	int8_t voicingHome = 0; // the voicing the dial springs back to (hold the encoder in and turn to dial it)
	bool isoFollowsChord = true; // SNAP: on pick, jump iso to the chord's octave; off = leave iso parked (riff high)
	// PROGRESSION WALKER (MVP): HOLD purple row 2, then dial — vertical wheel picks which progression, horizontal
	// wheel walks the chords. Holding sustains the current step; release leaves the chord loaded. No grid strip.
	int8_t progPreset = -1; // loaded preset index (-1 = none dialled yet)
	int8_t progStep = 0;    // current step in the loaded progression
	bool progVoiced = true; // VOICED = load each step with its baked-in voicing; BARE = plain chord (CLEAR toggles)
};
/// Please note that saving and restoring currently needs to be added manually in instrument_clip.cpp and all layouts
/// share one struct for storage
struct KeyboardState {
	KeyboardLayoutType currentLayout = FlashStorage::defaultKeyboardLayout;

	KeyboardStateIsomorphic isomorphic;
	KeyboardStateDrums drums;
	KeyboardStateInKey inKey;
	KeyboardStatePiano piano;
	KeyboardStateChord chord;
	KeyboardStateChordLibrary chordLibrary;
	KeyboardStateHarmonic harmonic;

	layout::ColumnControlState columnControl;
};

}; // namespace deluge::gui::ui::keyboard
