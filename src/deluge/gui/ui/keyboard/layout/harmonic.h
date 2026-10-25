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

#include "definitions.h"
#include "gui/ui/keyboard/chords.h"
#include "gui/ui/keyboard/layout/column_controls.h"
#include <array>

namespace deluge::gui::ui::keyboard::layout {

/// @brief The "Harmonic" layout: a chord explorer (left) beside an in-key isomorphic visualiser (right).
///
/// Left: the 7 in-key chords, one per column, each a DISTINCT colour, shading light->dark up the rows as
/// chord richness builds (triad -> 13). Press a pad and it plays, named in Roman + absolute. Right: an
/// in-key iso panel showing the scale grid in colour with the tonic as a steady anchor; the voiced chord
/// you pick lights as one shape (the isomorphism repeats it a couple of times), and notes you play on it
/// light in their colours like a keyboard.
class KeyboardLayoutHarmonic : public ColumnControlsKeyboard {
public:
	KeyboardLayoutHarmonic() = default;
	~KeyboardLayoutHarmonic() override = default;

	void evaluatePads(PressedPad presses[kMaxNumKeyboardPadPresses]) override;
	void handleVerticalEncoder(int32_t offset) override;
	bool voicingPressBegin() override; // vertical-encoder press-down: arm the spring-loaded voicing
	bool voicingPressEnd() override;   // release: a clean click springs to home; press+turn dialed the home
	void handleHorizontalEncoder(int32_t offset, bool shiftEnabled, PressedPad presses[kMaxNumKeyboardPadPresses],
	                             bool encoderPressed = false) override;
	void precalculate() override {}

	void renderPads(RGB image[][kDisplayWidth + kSideBarWidth]) override;

	l10n::String name() override { return l10n::String::STRING_FOR_KEYBOARD_LAYOUT_HARMONIC; }
	bool supportsInstrument() override { return true; }
	bool supportsKit() override { return false; }
	RequiredScaleMode requiredScaleMode() override { return RequiredScaleMode::Enabled; }
	// Keep re-rendering while the Calculator has suggestions (the next-chord pulse breathes) OR while an inbound
	// Chroma voicing-mod (0x44) is waiting, so renderPads gets a UI-thread tick to drain and apply it.
	bool requestsContinuousRender() override;

protected:
	bool allowSidebarType(ColumnControlFunction sidebarType) override;

private:
	// New model: two 7-wide grids with two bound control columns between them (7 + 2 + 7 = 16).
	// PALETTE 0-6 | palette-ctrl 7 | iso-ctrl 8 | ISO 9-15. Handedness swap mirrors the blocks (see colInfo).
	static constexpr int32_t kExplorerCols = 7; // legacy alias: palette block width
	static constexpr int32_t kBlockW = 7;       // each grid block is 7 columns wide

	uint8_t getScaleIntervals(uint8_t* ivOut);
	uint8_t buildChordAtDegree(uint8_t deg, int32_t y, const uint8_t* iv, uint8_t sc, uint8_t keyRoot,
	                           int16_t* notesOut, uint8_t maxNotes, uint8_t* rootPcOut, char* romanOut, char* absOut);
	int32_t isoNoteAt(int32_t localX, int32_t y);        // in-key mapping (localX = iso column 0..6)
	int32_t isoNoteChromatic(int32_t localX, int32_t y); // chromatic isomorphic mapping (localX 0..6)
	// Expand the base chord (chordNotes) into the played/shown VOICING per the SPREAD + STACK controls.
	uint8_t buildVoicing(int16_t* out, uint8_t maxOut);
	static constexpr uint8_t kMaxVoice = 16; // capacity for a stacked voicing
	// Chroma: re-broadcast the currently-selected chord's state to the host (CT dashboard) after ANY
	// voicing change, so the dash tracks every change live instead of only on a fresh palette pick.
	void pushChordState();
	// Chroma WRITE direction (0x44): apply an inbound voicing-mod from a host (CT/Companion) to the held chord,
	// then re-broadcast via pushChordState so every surface stays in sync. Drained on the UI thread in renderPads.
	// State + broadcast only (never sounds here); modType 0=spread, 1=inversion, value clamped to 0..3.
	void applyInboundMod(uint8_t modType, uint8_t value);
	char curCtx_[40] = {};   // contextId of the last picked chord (degree + richness), reused on re-broadcast
	uint8_t curKeyRoot_ = 0; // key root of the last picked chord
	void recomputeSuggestions(uint8_t keyRoot, const uint8_t* iv, uint8_t sc, uint8_t homeRootPc);
	void drawName(const char* roman, const char* abs);
	void loadProgStep(); // PROG: latch the current preset's current step as the selected Harmonic Object

	uint16_t heldCols = 0;                  // explorer columns currently held (light up as feedback)
	uint16_t prevHeldCols = 0;              // held columns last frame — falling edge = chord released (springs voicing)
	bool voicingTurnedWhilePressed = false; // press+turn happened this hold → release must NOT spring to home

	// The EXACT voiced notes of the selected chord (absolute MIDI), so the iso panel shows the real
	// shape once in the octave you picked — not every-octave pitch classes (that was an overlapping mess).
	int16_t chordNotes[kMaxChordKeyboardSize] = {};
	uint8_t chordNoteCount = 0; // 0 = cleared (free play)

	// Persisted selection: the chord you last picked. Drives the white highlight on the left and the
	// shape on the iso panel, and stays put after you release so you can study the shape.
	int8_t selDeg = -1;      // selected degree column 0-6 (-1 = none)
	int8_t selRichness = -1; // selected richness row 0-(kDisplayHeight-1)
	// The pick we've already scrolled the name for. evaluatePads runs every frame while a pad is held, so
	// drawName must fire ONCE per pick (else setScrollingText restarts the scroll every frame and the roman
	// at the tail never appears on the 7-seg). Reset to -1 on release so re-picking the same chord re-shows it.
	int8_t namedDeg = -1;
	int8_t namedRich = -1;

	// Calculator: ranks the diatonic next chords. Each suggested degree's triad + 7th flash, brightness = how
	// strong a move it is. Richness beyond the core is the user's choice.
	ChordSuggestion suggestions[7];
	uint8_t numSuggestions = 0;
	uint8_t degBright[7] = {}; // per-degree brightness (0-255) = strength as a next move; 0 when no Calculator
	int8_t topDeg = -1;        // the single strongest next degree; -1 = none

	uint8_t palCtrlHeldMask = 0; // palette-control column rows held last frame (rising-edge detect)
	uint8_t isoCtrlHeldMask = 0; // iso-control column rows held last frame (rising-edge detect)
	uint64_t isoHeldMask = 0;    // iso pads held last frame (bit = localX*8+y) — rising-edge for voice edit
	bool progPadHeld = false;    // PROG (purple row 2) held last frame — gates the encoder dial + step sustain

	// LEARN inspect mode: while LEARN is held the whole surface goes SILENT and each pad just names itself.
	// These track which pads were already named last frame (pad id = x*kDisplayHeight + y, 0..127) so a held
	// pad pops its name once, not every frame (which would thrash the display and starve audio).
	uint64_t learnHeldLo = 0; // pad ids 0..63 named last frame
	uint64_t learnHeldHi = 0; // pad ids 64..127 named last frame
	uint16_t learnSbHeld = 0; // sidebar pads named last frame (bit = (x-kDisplayWidth)*kDisplayHeight + y)

	// Live held-pad feedback: which display-area pads (x in 0..kDisplayWidth) are physically under a finger
	// right now, per row (bit = x). renderPads paints a white overlay on these so the user (and the Companion
	// mirror) sees exactly which pads are pressed. Display-only — never touches audio/timing.
	uint16_t heldPadRows[kDisplayHeight] = {};
};

}; // namespace deluge::gui::ui::keyboard::layout
