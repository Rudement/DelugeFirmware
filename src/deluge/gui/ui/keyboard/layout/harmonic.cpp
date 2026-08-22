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

#include "gui/ui/keyboard/layout/harmonic.h"
#include "extern.h"
#include "fatfs/fatfs.hpp"
#include "gui/colour/colour.h"
#include "gui/ui/keyboard/chord_mem_service.h"
#include "gui/ui/keyboard/chords.h"
#include "gui/ui/keyboard/column_controls/chord_mem.h"
#include "gui/ui/keyboard/layout/column_control_state.h"
#include "hid/button.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "io/debug/log.h" // 1.2: pulls in lib/printf.h so sprintf/snprintf alias the
                             // non-allocating implementations (1.3 gets it transitively)
#include "hid/hid_sysex.h"
#include "model/settings/runtime_feature_settings.h"
#include "processing/engines/audio_engine.h"
#include "storage/storage_manager.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace deluge::gui::ui::keyboard::layout {

namespace {

inline int32_t floordiv(int32_t a, int32_t b) {
	int32_t q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0))) {
		q--;
	}
	return q;
}
inline int32_t floormod(int32_t a, int32_t b) {
	int32_t r = a % b;
	if (r != 0 && ((r < 0) != (b < 0))) {
		r += b;
	}
	return r;
}

// ── Column model (the SINGLE source of truth for where each block lives) ───────────────────────────────
// Layout is 16 wide: PALETTE(7) | palette-ctrl(1) | iso-ctrl(1) | ISO(7). The handedness SWAP mirrors the
// two blocks AND their bound control columns, so each control column always sits next to the grid it drives.
enum Region : uint8_t { REG_PAL, REG_PAL_CTRL, REG_ISO_CTRL, REG_ISO, REG_NONE };
struct ColInfo {
	Region region;
	int32_t local; // index within a 7-wide block (0..6); 0 for control columns
};
constexpr int32_t kBlockWidth = 7;

inline ColInfo colInfoFor(int32_t x, bool swapped) {
	int32_t palStart = swapped ? 9 : 0;
	int32_t isoStart = swapped ? 0 : 9;
	int32_t palCtrl = swapped ? 8 : 7;
	int32_t isoCtrl = swapped ? 7 : 8;
	if (x == palCtrl) {
		return {REG_PAL_CTRL, 0};
	}
	if (x == isoCtrl) {
		return {REG_ISO_CTRL, 0};
	}
	if (x >= palStart && x < palStart + kBlockWidth) {
		return {REG_PAL, x - palStart};
	}
	if (x >= isoStart && x < isoStart + kBlockWidth) {
		return {REG_ISO, x - isoStart};
	}
	return {REG_NONE, 0};
}
inline int32_t isoStartCol(bool swapped) {
	return swapped ? 0 : 9;
}

// Additive richness ladder: y=0 (bottom) = the ROOT alone (bass/anchor lane), building UP by stacking
// thirds → 13th at the top. `steps` are diatonic scale-degree offsets from the column's degree. sus2/sus4
// left the ladder — they're ALTERATIONS (voicing edits: toggle the 3rd off, the 2nd/4th on), not richness.
struct Richness {
	const char* suffix;
	int8_t steps[kMaxChordKeyboardSize];
	uint8_t count;
	int8_t add; // chromatic semitone to add above the root (-1 = none); used for the BRIGHT major-6th
};
const Richness kLadder[kDisplayHeight] = {
    {"", {0, 0, 0, 0, 0, 0, 0}, 1, -1},     // row0 ROOT  — single note; the bass / "where it starts" anchor
    {"5", {0, 4, 0, 0, 0, 0, 0}, 2, -1},    // row1 DYAD  — root + 5th (power)
    {"", {0, 2, 4, 0, 0, 0, 0}, 3, -1},     // row2 TRIAD
    {"6", {0, 2, 4, 0, 0, 0, 0}, 3, 9},     // row3 6th  — triad + a BRIGHT major 6th (root+9), borrowed if needed
    {"7", {0, 2, 4, 6, 0, 0, 0}, 4, -1},    // row4 7th
    {"9", {0, 2, 4, 6, 8, 0, 0}, 5, -1},    // row5 9th
    {"11", {0, 2, 4, 6, 8, 10, 0}, 6, -1},  // row6 11th
    {"13", {0, 2, 4, 6, 8, 10, 12}, 7, -1}, // row7 13th
};

// One colour per scale degree, ordered so ADJACENT columns jump across the colour wheel (warm/cool
// interleaved) — never grouped, so neighbours always contrast hard. renderPads shades each light->dark.
const RGB kDegreeHue[7] = {
    RGB{.r = 255, .g = 0, .b = 0},   // i   red
    RGB{.r = 0, .g = 220, .b = 220}, // ii  cyan     (opposite red)
    RGB{.r = 235, .g = 220, .b = 0}, // III yellow
    RGB{.r = 45, .g = 65, .b = 255}, // iv  blue
    RGB{.r = 0, .g = 220, .b = 0},   // v   green
    RGB{.r = 235, .g = 0, .b = 235}, // VI  magenta
    RGB{.r = 255, .g = 120, .b = 0}, // VII orange
};

// Brightness ramp for the richness rows (bottom=triad brightest -> top=complex dimmest). Brightened
// floor so the upper (lusher) rows still read clearly on the LEDs — "big and bright".
const uint8_t kRichBright[kDisplayHeight] = {255, 195, 150, 115, 88, 68, 54, 42};

// DEFAULT key-mood palette: the iso tints to the current key's colour so you can SEE the key (brightness
// still carries the chord relationships). Mood-based, not a flat wheel — bright/dark per key's feel. This
// is just the default; a custom + saveable per-user table is the natural next step. Indexed by root pc.
const RGB kKeyColour[12] = {
    RGB{.r = 255, .g = 150, .b = 110}, // C   open / warm       — warm coral (NOT white; white = the chord)
    RGB{.r = 130, .g = 55, .b = 180},  // C#  dark / mysterious — deep violet
    RGB{.r = 255, .g = 225, .b = 50},  // D   bright / joyful   — yellow
    RGB{.r = 190, .g = 90, .b = 50},   // D#  noble / warm-dark — deep red-gold
    RGB{.r = 25, .g = 190, .b = 70},   // E   radiant           — deeper green (less blue so it doesn't wash white)
    RGB{.r = 120, .g = 195, .b = 95},  // F   calm / pastoral   — soft green
    RGB{.r = 25, .g = 160, .b = 175},  // F#  deep / mysterious — teal
    RGB{.r = 255, .g = 140, .b = 30},  // G   warm / friendly   — orange
    RGB{.r = 75, .g = 70, .b = 170},   // G#  dark / deep       — indigo
    RGB{.r = 255, .g = 190, .b = 20},  // A   energy / bright   — gold
    RGB{.r = 220, .g = 110, .b = 90},  // A#  rich / warm       — rose-amber
    RGB{.r = 55, .g = 130, .b = 255},  // B   brilliant / intense — bright blue
};

const char* const kNumerals[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
const uint8_t kMajorIv[7] = {0, 2, 4, 5, 7, 9, 11};

// The Calculator marks each suggested degree's standard core: triad + 7th. The user chooses richer voicings
// themselves. (Additive ladder: kLadder[2] = triad, kLadder[4] = "7".)
constexpr int32_t kRowTriad = 2;
constexpr int32_t kRow7th = 4;

// ── Progression presets (MVP "Walker") ─────────────────────────────────────────────────────────────────
// A preset is an ordered list of diatonic steps {degree 0-6, richness ladder-row}. Degrees are SCALE-relative,
// so a preset RESOLVES into whatever key + scale is current. Default richness = the "7" row (kRow7th) for the
// colour Cody likes ("more never less"). Names are 4-char for the 7-seg.
struct ProgStep {
	int8_t degree;
	int8_t rich;
	uint8_t oct;   // baked octave-STACK bitmask (bits 0..6 = octaves -3..+3, bit3 = base). 0 = bare (base only).
	int8_t spread; // baked SPREAD (drop-root) 0..3
	int8_t inv;    // baked INVERSION 0..3
};
struct ProgPreset {
	const char* name;
	ProgStep steps[8];
	uint8_t count;
};
const ProgPreset kPresets[] = {
    {"251", {{1, kRow7th}, {4, kRow7th}, {0, kRow7th}}, 3},                // ii – V – I
    {"1564", {{0, kRow7th}, {4, kRow7th}, {5, kRow7th}, {3, kRow7th}}, 4}, // I – V – vi – IV
    {"EPIC", {{0, kRow7th}, {5, kRow7th}, {2, kRow7th}, {6, kRow7th}}, 4}, // i – VI – III – VII (minor loop)
    {"ANDL", {{0, kRow7th}, {6, kRow7th}, {5, kRow7th}, {4, kRow7th}}, 4}, // Andalusian: i – VII – VI – V
    {"DORI", {{0, kRow7th}, {3, kRow7th}}, 2},                             // Dorian vamp: i – IV
};
constexpr int32_t kNumPresets = (int32_t)(sizeof(kPresets) / sizeof(kPresets[0]));

// Runtime progression list = the built-in presets above PLUS any progressions loaded from CHORDS/*.chordpack
// files on the SD card. Scanned LAZILY (first PROG use), never at boot — so a malformed pack can never brick.
constexpr int32_t kMaxProgSteps = 8;
constexpr int32_t kMaxProgressions = 48;
struct ProgEntry {
	char name[8];
	ProgStep steps[kMaxProgSteps];
	uint8_t count;
};
ProgEntry gProgs[kMaxProgressions];
int32_t gNumProgs = 0;
bool gProgsScanned = false;

// Map a chordpack <step rich="..."> label to a richness ladder row.
int8_t richRowFromLabel(const char* v) {
	if (v == nullptr) {
		return (int8_t)kRow7th;
	}
	if (strcmp(v, "triad") == 0) {
		return (int8_t)kRowTriad;
	}
	if (strcmp(v, "root") == 0 || strcmp(v, "1") == 0) {
		return 0;
	}
	if (strcmp(v, "5") == 0) {
		return 1;
	}
	if (strcmp(v, "6") == 0) {
		return 3;
	}
	if (strcmp(v, "9") == 0) {
		return 5;
	}
	if (strcmp(v, "11") == 0) {
		return 6;
	}
	if (strcmp(v, "13") == 0) {
		return 7;
	}
	return (int8_t)kRow7th; // "7" and anything unrecognised
}

// Parse one CHORDS/*.chordpack file, appending its <progression>s to gProgs. Follows the canonical Deluge
// deserializer pattern: read a value then exitTag(name); a catch-all else exitTag(name) skips anything unknown.
void parseChordPack(FilePointer* fp) {
	if (storageManager.openXMLFile(fp, smDeserializer, "chordpack") != Error::NONE) {
		return;
	}
	char const* tag;
	while (*(tag = smDeserializer.readNextTagOrAttributeName())) {
		if (strcmp(tag, "progression") == 0 && gNumProgs < kMaxProgressions) {
			ProgEntry& en = gProgs[gNumProgs];
			en.count = 0;
			en.name[0] = 0;
			char const* t2;
			while (*(t2 = smDeserializer.readNextTagOrAttributeName())) {
				if (strcmp(t2, "name") == 0) {
					char const* nm = smDeserializer.readTagOrAttributeValue();
					if (nm != nullptr) {
						strncpy(en.name, nm, sizeof(en.name) - 1);
						en.name[sizeof(en.name) - 1] = 0;
					}
					smDeserializer.exitTag("name");
				}
				else if (strcmp(t2, "step") == 0) {
					int32_t deg = 1;
					int8_t rich = (int8_t)kRow7th;
					int32_t octup = 0, octdn = 0, spread = 0, inv = 0;
					char const* t3;
					while (*(t3 = smDeserializer.readNextTagOrAttributeName())) {
						if (strcmp(t3, "degree") == 0) {
							deg = smDeserializer.readTagOrAttributeValueInt();
							smDeserializer.exitTag("degree");
						}
						else if (strcmp(t3, "rich") == 0) {
							rich = richRowFromLabel(smDeserializer.readTagOrAttributeValue());
							smDeserializer.exitTag("rich");
						}
						else if (strcmp(t3, "octup") == 0) {
							octup = smDeserializer.readTagOrAttributeValueInt();
							smDeserializer.exitTag("octup");
						}
						else if (strcmp(t3, "octdn") == 0) {
							octdn = smDeserializer.readTagOrAttributeValueInt();
							smDeserializer.exitTag("octdn");
						}
						else if (strcmp(t3, "spread") == 0) {
							spread = smDeserializer.readTagOrAttributeValueInt();
							smDeserializer.exitTag("spread");
						}
						else if (strcmp(t3, "inv") == 0) {
							inv = smDeserializer.readTagOrAttributeValueInt();
							smDeserializer.exitTag("inv");
						}
						else {
							smDeserializer.exitTag(t3);
						}
					}
					if (en.count < kMaxProgSteps) {
						int32_t d = deg - 1; // file is 1-based (degree 1 = tonic)
						if (d < 0) {
							d = 0;
						}
						if (d > 6) {
							d = 6;
						}
						// Build the octave-STACK bitmask: bit3 = base, bits 4..6 = +1..+3 oct, bits 2..0 = -1..-3.
						uint8_t octMask = (uint8_t)(1u << 3);
						for (int32_t k = 1; k <= octup && k <= 3; k++) {
							octMask |= (uint8_t)(1u << (3 + k));
						}
						for (int32_t k = 1; k <= octdn && k <= 3; k++) {
							octMask |= (uint8_t)(1u << (3 - k));
						}
						if (spread < 0) {
							spread = 0;
						}
						if (spread > 3) {
							spread = 3;
						}
						if (inv < 0) {
							inv = 0;
						}
						if (inv > 3) {
							inv = 3;
						}
						en.steps[en.count].degree = (int8_t)d;
						en.steps[en.count].rich = rich;
						en.steps[en.count].oct = octMask;
						en.steps[en.count].spread = (int8_t)spread;
						en.steps[en.count].inv = (int8_t)inv;
						en.count++;
					}
					smDeserializer.exitTag("step");
				}
				else {
					smDeserializer.exitTag(t2);
				}
			}
			if (en.count > 0) {
				if (en.name[0] == 0) {
					strncpy(en.name, "PACK", sizeof(en.name) - 1);
					en.name[sizeof(en.name) - 1] = 0;
				}
				gNumProgs++;
			}
			smDeserializer.exitTag("progression");
		}
		else {
			smDeserializer.exitTag(tag);
		}
	}
	storageManager.closeFile(); // 1.2 storage API: the deserializer has no closeWriter()
}

// Lazy one-time scan: seed gProgs with the built-in presets, then append every CHORDS/*.chordpack progression.
void ensureProgsLoaded() {
	if (gProgsScanned) {
		return;
	}
	if (sdRoutineLock) {
		return; // never touch the SD card from inside the audio/card routine — retry on the next (safe) call
	}
	gProgsScanned = true; // set up-front: a failed / partial scan must never retry-loop
	gNumProgs = 0;
	for (int32_t i = 0; i < kNumPresets && gNumProgs < kMaxProgressions; i++) {
		ProgEntry& e = gProgs[gNumProgs++];
		strncpy(e.name, kPresets[i].name, sizeof(e.name) - 1);
		e.name[sizeof(e.name) - 1] = 0;
		e.count = kPresets[i].count;
		for (int32_t s = 0; s < e.count && s < kMaxProgSteps; s++) {
			e.steps[s] = kPresets[i].steps[s];
		}
	}
	if (storageManager.initSD() != Error::NONE) {
		return;
	}
	auto dres = FatFS::Directory::open("CHORDS");
	if (!dres) {
		return; // no CHORDS folder — built-ins only
	}
	FatFS::Directory& dir = *dres;
	FilePointer packs[16];
	int32_t nPacks = 0;
	while (nPacks < 16) {
		auto rr = dir.read_and_get_filepointer();
		if (!rr) {
			break;
		}
		FatFS::FileInfo info = rr->first;
		if (info.fname[0] == 0) {
			break; // end of directory
		}
		if ((info.fattrib & AM_DIR) != 0) {
			continue;
		}
		const char* dot = strrchr(info.fname, '.');
		if (dot == nullptr || strcasecmp(dot, ".chordpack") != 0) {
			continue;
		}
		packs[nPacks++] = rr->second;
	}
	for (int32_t i = 0; i < nPacks; i++) {
		parseChordPack(&packs[i]);
	}
}

// ── Control columns ────────────────────────────────────────────────────────────────────────────────────
// Each control column hosts toggles at the TOP; dark pads below act as CLEAR pads. Achromatic (white = on,
// dim grey = off) so they never blend with the colourful palette. The modifiers adjust the VISUALS.
//   ISO control (bound to the iso): in-key/chromatic view, show-chord, sticky-voicing.
//   PALETTE control (bound to the explorer): Calculator on/off, handedness swap.
constexpr int32_t kBtnIsoView = kDisplayHeight - 1;   // iso-ctrl: in-key <-> chromatic
constexpr int32_t kBtnShowChord = kDisplayHeight - 2; // iso-ctrl: show / hide the chord shape
constexpr int32_t kBtnSticky = kDisplayHeight - 3;    // iso-ctrl: sticky chord voicing
constexpr int32_t kBtnLattice = kDisplayHeight - 4;   // iso-ctrl: full chord lattice on/off
constexpr int32_t kBtnSnap = 3;                       // iso-ctrl: SNAP — iso jumps to the chord's octave on pick
constexpr int32_t kBtnProg = 2;                       // iso-ctrl: PROGRESSION hold-to-dial (encoders)
constexpr int32_t kBtnEdit = 1;                       // iso-ctrl: voice-edit toggle (sculpt notes ON the iso surface)
constexpr int32_t kBtnAudition = 0;      // iso-ctrl: AUDITION the voicing — momentary, hold to hear the chord
constexpr uint8_t kIsoCtrlClearMask = 0; // iso-ctrl: row 2 free (for the Diff View); no clear pads here

// VOICING controls live on the pal-ctrl column (they shape the CHORD, not the surface).
constexpr int32_t kBtnCalc = kDisplayHeight - 1;                // pal-ctrl: next-chord Calculator on/off
constexpr int32_t kBtnSwap = kDisplayHeight - 2;                // pal-ctrl: swap the two sides (handedness)
constexpr int32_t kBtnPalOctUp = kDisplayHeight - 3;            // pal-ctrl: PALETTE octave up
constexpr int32_t kBtnPalOctDown = kDisplayHeight - 4;          // pal-ctrl: PALETTE octave down
constexpr int32_t kBtnStack = 3;                                // pal-ctrl: octave STACK / picker
constexpr int32_t kBtnSpread = 2;                               // pal-ctrl: SPREAD (drop-root open)
constexpr int32_t kBtnInversion = 1;                            // pal-ctrl: INVERSION (cycle 0..3)
constexpr uint8_t kPalCtrlClearMask = (uint8_t)((1u << 1) - 1); // row 0 = clear pad

// LEARN-mode help: hold the LEARN button and tap a control pad to read its function (it won't activate).
// Names scroll on the 7-seg, show whole on OLED — so you don't have to memorise the control columns.
const char* controlPadName(Region region, int32_t y) {
	if (region == REG_PAL_CTRL) { // CRIMSON — the chord
		switch (y) {
		case kBtnCalc:
			return "CALCULATOR";
		case kBtnSwap:
			return "SWAP SIDES";
		case kBtnPalOctUp:
			return "PALETTE OCTAVE UP";
		case kBtnPalOctDown:
			return "PALETTE OCTAVE DOWN";
		case kBtnStack:
			return "OCTAVE STACK PICKER";
		case kBtnSpread:
			return "SPREAD (DROP-ROOT)";
		case kBtnInversion:
			return "INVERSION";
		default:
			return "CLEAR";
		}
	}
	switch (y) { // PURPLE — the surface
	case kBtnIsoView:
		return "VIEW: IN-KEY / CHROMATIC";
	case kBtnShowChord:
		return "SHOW CHORD SHAPE";
	case kBtnSticky:
		return "STICKY (HOLD CHORD)";
	case kBtnLattice:
		return "OVERLAY: ONE / LATTICE / DIFF";
	case kBtnSnap:
		return "SNAP ISO TO CHORD";
	case kBtnProg:
		return "PROGRESSION (HOLD + DIAL)";
	case kBtnEdit:
		return "EDIT NOTES";
	default:
		return "AUDITION (HOLD TO HEAR)";
	}
}

// Short, stable token for a control pad's contextId (control:<TOKEN>) — what the host LEARN event carries.
// Distinct from controlPadName (the human label shown on the device). See chroma-schema SCHEMA.md 2.1.
const char* controlPadId(Region region, int32_t y) {
	if (region == REG_PAL_CTRL) {
		switch (y) {
		case kBtnCalc:
			return "CALC";
		case kBtnSwap:
			return "SWAP";
		case kBtnPalOctUp:
			return "OCTUP";
		case kBtnPalOctDown:
			return "OCTDN";
		case kBtnStack:
			return "STACK";
		case kBtnSpread:
			return "SPREAD";
		case kBtnInversion:
			return "INV";
		default:
			return "CLEAR";
		}
	}
	switch (y) {
	case kBtnIsoView:
		return "VIEW";
	case kBtnShowChord:
		return "SHOW";
	case kBtnSticky:
		return "STICKY";
	case kBtnLattice:
		return "OVERLAY";
	case kBtnSnap:
		return "SNAP";
	case kBtnProg:
		return "PROG";
	case kBtnEdit:
		return "EDIT";
	default:
		return "AUDITION";
	}
}

// Reserved control-zone colours — a "control family" (CRIMSON + PURPLE) used NOWHERE else in Chroma. The two
// centre columns get DIFFERENT hues so they're instantly distinguishable: palette-control = CRIMSON,
// iso-control = PURPLE. On = bright, off = dim, whole column faintly tinted so each reads as its own zone.
const RGB kCtrlHue =
    RGB{.r = 180,
        .g = 20,
        .b = 55}; // palette-control = CRIMSON (dark+warm; far from the white suggestion flash + magenta degree)
const RGB kCtrlHueIso = RGB{.r = 150, .g = 80, .b = 255}; // iso-control = PURPLE
// The iso-control column reads in three BANDS (SEE → SHAPE → HEAR), each a distinct shade of the purple
// family so the column groups at a glance instead of one undifferentiated list.
const RGB kHueSee = RGB{.r = 90, .g = 115, .b = 255};   // SEE   band (lens / overlays): bluer purple
const RGB kHueShape = RGB{.r = 165, .g = 70, .b = 255}; // SHAPE band (voicing engine): mid violet
const RGB kHueHear = RGB{.r = 215, .g = 65, .b = 235};  // HEAR  band (audition): warmer pink-purple
// Control columns read as DARK negative space — only what's ENGAGED lights up. This keeps them visually
// distinct from the colourful palette + the velocity/mod sidebar, and makes "what's on" instantly legible.
constexpr uint8_t kCtrlOn = 235;   // a toggle that is ON — bright in its control hue
constexpr uint8_t kCtrlReady = 50; // a momentary/action button (OCT, Audition) — faint ember so it's findable
constexpr uint8_t kCtrlOff = 10;   // an OFF toggle / clear pad — near-black (the "blank unless toggled" floor)
constexpr uint8_t kCtrlFaint = 10; // column base — blank

} // namespace

uint8_t KeyboardLayoutHarmonic::getScaleIntervals(uint8_t* ivOut) {
	NoteSet& scale = getScaleNotes();
	uint8_t count = 0;
	for (uint8_t i = 0; i < kOctaveSize && count < 12; i++) {
		if (scale.has(i)) {
			ivOut[count++] = i;
		}
	}
	return count;
}

int32_t KeyboardLayoutHarmonic::isoNoteAt(int32_t localX, int32_t y) {
	uint8_t sc = getScaleNoteCount();
	if (sc == 0) {
		return getRootNote();
	}
	// In-Key keyboard mapping (scale-step layout + colours). Anchor the panel ONE OCTAVE BELOW the chord
	// register (octaveBase) so the voicing — built at octaveBase — lands up in the middle of the panel.
	int32_t padIndex = (getState().harmonic.isoOctave - 1) * (int32_t)sc + localX + y * getState().inKey.rowInterval;
	if (padIndex < 0) {
		padIndex = 0;
	}
	int32_t octave = padIndex / sc;
	int32_t idx = padIndex % sc;
	return octave * 12 + getRootNote() + getScaleNotes()[idx];
}

int32_t KeyboardLayoutHarmonic::isoNoteChromatic(int32_t localX, int32_t y) {
	// Standard chromatic isomorphic mapping (semitone per column, rowInterval per row), anchored ONE
	// OCTAVE BELOW the chord register so the voicing lands in the middle of the panel, not at the bottom.
	return (getState().harmonic.isoOctave - 1) * 12 + getRootNote() + localX + y * getState().isomorphic.rowInterval;
}

uint8_t KeyboardLayoutHarmonic::buildVoicing(int16_t* out, uint8_t maxOut) {
	// Gather + sort the base chord notes.
	int16_t tmp[kMaxChordKeyboardSize];
	uint8_t n = 0;
	for (uint8_t i = 0; i < chordNoteCount && n < kMaxChordKeyboardSize; i++) {
		tmp[n++] = chordNotes[i];
	}
	for (uint8_t i = 1; i < n; i++) {
		int16_t v = tmp[i];
		int32_t j = (int32_t)i - 1;
		while (j >= 0 && tmp[j] > v) {
			tmp[j + 1] = tmp[j];
			j--;
		}
		tmp[j + 1] = v;
	}
	// INVERSION: rotate — move the lowest `inv` notes up an octave, then re-sort so the chord re-roots.
	int8_t inv = getState().harmonic.voiceInversion;
	for (int8_t k = 0; k < inv && k < (int8_t)n; k++) {
		tmp[k] = (int16_t)(tmp[k] + 12);
	}
	for (uint8_t i = 1; i < n; i++) {
		int16_t v = tmp[i];
		int32_t j = (int32_t)i - 1;
		while (j >= 0 && tmp[j] > v) {
			tmp[j + 1] = tmp[j];
			j--;
		}
		tmp[j + 1] = v;
	}
	// SPREAD: drop the lowest notes down an octave to open the voicing (drop-root).
	int8_t spread = getState().harmonic.voiceSpread;
	for (int8_t k = 0; k < spread && k < (int8_t)n; k++) {
		if (tmp[k] - 12 >= 0) {
			tmp[k] = (int16_t)(tmp[k] - 12);
		}
	}
	// VOICING DIAL (Orchid-style): walk the whole voicing one note at a time. Each +step lifts the current
	// LOWEST note an octave; each -step drops the current HIGHEST — re-sorting between, so the chord cascades
	// through inversions and opens across the range. Applied to the core voicing here, before STACK replicates
	// it. Sticky across chord picks, so it reads as a persistent "voicing character" you dial in and leave.
	int8_t walk = getState().harmonic.voicingWalk;
	if (walk != 0 && n > 1) {
		for (uint8_t i = 1; i < n; i++) { // ensure sorted (SPREAD above doesn't re-sort)
			int16_t v = tmp[i];
			int32_t j = (int32_t)i - 1;
			while (j >= 0 && tmp[j] > v) {
				tmp[j + 1] = tmp[j];
				j--;
			}
			tmp[j + 1] = v;
		}
		int8_t steps = walk;
		while (steps != 0) {
			if (steps > 0) {
				if (tmp[0] + 12 <= 127) {
					tmp[0] = (int16_t)(tmp[0] + 12); // lowest up an octave
				}
				steps--;
			}
			else {
				if (tmp[n - 1] - 12 >= 0) {
					tmp[n - 1] = (int16_t)(tmp[n - 1] - 12); // highest down an octave
				}
				steps++;
			}
			for (uint8_t i = 1; i < n; i++) { // re-sort after the single move
				int16_t v = tmp[i];
				int32_t j = (int32_t)i - 1;
				while (j >= 0 && tmp[j] > v) {
					tmp[j + 1] = tmp[j];
					j--;
				}
				tmp[j + 1] = v;
			}
		}
	}
	// STACK: copy the chord into each SELECTED octave (bits 0..6 = offsets -3..+3; bit3 = base). Dedup + clamp.
	uint8_t octaves = getState().harmonic.voiceOctaves;
	uint8_t cnt = 0;
	for (int8_t b = 0; b < 7; b++) {
		if (!(octaves & (uint8_t)(1u << b))) {
			continue;
		}
		int32_t off = ((int32_t)b - 3) * 12;
		for (uint8_t i = 0; i < n; i++) {
			int32_t v = (int32_t)tmp[i] + off;
			if (v < 0 || v > 127 || cnt >= maxOut) {
				continue;
			}
			bool dup = false;
			for (uint8_t k = 0; k < cnt; k++) {
				if (out[k] == v) {
					dup = true;
					break;
				}
			}
			if (!dup) {
				out[cnt++] = (int16_t)v;
			}
		}
	}
	return cnt;
}

// Chroma: re-broadcast the currently-selected chord (same key + context) with its freshly-rebuilt
// voicing, so the host dashboard tracks SPRD/INV/STACK changes live, not only on a fresh palette pick.
void KeyboardLayoutHarmonic::pushChordState() {
	if (chordNoteCount == 0) {
		return; // nothing selected — nothing to broadcast
	}
	int16_t voiced[kMaxVoice];
	uint8_t vn = buildVoicing(voiced, kMaxVoice);
	HIDSysex::sendChordState(curKeyRoot_, voiced, vn, curCtx_, getState().harmonic.voiceSpread,
	                         getState().harmonic.voiceInversion, (uint8_t)currentSong->getCurrentScale());
}

// Re-render continuously while the Calculator is suggesting (pulsing) or an inbound voicing-mod is queued
// (so renderPads gets a UI-thread tick to drain it). See requestsChordApply flow in hid_sysex.cpp.
bool KeyboardLayoutHarmonic::requestsContinuousRender() {
	return numSuggestions > 0 || HIDSysex::hasChordMod();
}

// Chroma WRITE direction: apply a host voicing-mod (0x44) to the currently held chord. Mirrors what the
// SPRD / INV controls do (set the voicing state, then pushChordState so the CT/HUD track it), but WITHOUT
// sounding — this runs on the render tick, and the mod is heard the next time the chord is played (buildVoicing
// reads the new state). The Deluge stays the source of truth: it re-broadcasts its own voicing via 0x43.
void KeyboardLayoutHarmonic::applyInboundMod(uint8_t modType, uint8_t value) {
	KeyboardStateHarmonic& hs = getState().harmonic;
	int8_t v = (int8_t)(value > 3 ? 3 : value); // voicing range is 0..3 (spread/inversion only)
	switch (modType) {
	case 0: // spread
		hs.voiceSpread = v;
		break;
	case 1: // inversion
		hs.voiceInversion = v;
		break;
	case 2: { // key: adopt a host-led modulation (the CT wheel confirmed with SAVE). Change the song's
		// key root to the target pitch class, keeping the current octave — the same operation the SCALE
		// button performs. The Deluge stays the source of truth: it then re-broadcasts 0x43 below so the
		// CT/Companion re-sync to the new key.
		int32_t oldRoot = currentSong->key.rootNote;
		int32_t oldPc = ((oldRoot % 12) + 12) % 12;
		int32_t newRoot = oldRoot - oldPc + (int32_t)(value % 12);
		if (newRoot != oldRoot) {
			currentSong->setRootNote(newRoot, nullptr);
		}
		break;
	}
	case 3: { // scale/mode: adopt a host-led modulation. The CT's mode index (0..6 = major..locrian) maps
		// 1:1 onto the preset Scale enum, so it applies directly. Change the song scale, then re-broadcast.
		if (value < NUM_PRESET_SCALES) {
			currentSong->setScale((Scale)value);
		}
		break;
	}
	case 4: // octave: the CT's register knob shifts the harmonic layout's base octave (bounded 1..8).
		if (value >= 1 && value <= 8) {
			hs.octaveBase = (int8_t)value;
			hs.isoOctave = hs.octaveBase; // keep the iso panel anchored to the new base, like the OCT +/- controls
		}
		break;
	case 5: // spelling lean: any surface sets the device-wide flats/sharps override; the Deluge adopts it and
		// re-broadcasts, so every surface (incl. the Deluge's own screen) respells identically.
		// 0=Auto,1=Flats,2=Sharps.
		if (value <= 2) {
			gChromaSpelling = (ChromaSpelling)value;
		}
		break;
	default:
		return; // unknown mod type — ignore safely (forward-compatible with richer hosts)
	}
	pushChordState(); // re-voice + re-broadcast; no-op if nothing is selected
}

void KeyboardLayoutHarmonic::recomputeSuggestions(uint8_t keyRoot, const uint8_t* iv, uint8_t sc, uint8_t homeRootPc) {
	numSuggestions = 0;
	topDeg = -1;
	for (uint8_t d = 0; d < 7; d++) {
		degBright[d] = 0;
	}
	// Off when the Calculator is toggled off; otherwise diatonic-7-note only (like the Chord Library's Calculator).
	if (!getState().harmonic.calculatorOn || sc != 7 || !getScaleModeEnabled()) {
		return;
	}
	// Exactly like Chord Library: take the TOP 3 next chords only (not all degrees), so just those few
	// columns light. degBright shades them by rank (strongest brightest); the rest stay dark.
	numSuggestions = (uint8_t)suggestNextChords(keyRoot, getScaleNotes(), sc, homeRootPc, suggestions, 3);
	for (uint8_t s = 0; s < numSuggestions; s++) {
		for (uint8_t d = 0; d < sc; d++) {
			if ((uint8_t)((keyRoot + iv[d]) % 12) == suggestions[s].rootNote) {
				degBright[d] = (numSuggestions > 1) ? (uint8_t)(235 - (uint32_t)s * 165 / (numSuggestions - 1)) : 235;
				if (s == 0) {
					topDeg = (int8_t)d;
				}
				break;
			}
		}
	}
	// The chord you're on (home) stays full so you always see where you are.
	for (uint8_t d = 0; d < sc; d++) {
		if ((uint8_t)((keyRoot + iv[d]) % 12) == homeRootPc) {
			degBright[d] = 255;
			break;
		}
	}
}

uint8_t KeyboardLayoutHarmonic::buildChordAtDegree(uint8_t deg, int32_t y, const uint8_t* iv, uint8_t sc,
                                                   uint8_t keyRoot, int16_t* notesOut, uint8_t maxNotes,
                                                   uint8_t* rootPcOut, char* romanOut, char* absOut) {
	if (romanOut) {
		romanOut[0] = '\0';
	}
	if (absOut) {
		absOut[0] = '\0';
	}
	if (sc == 0) {
		return 0;
	}
	int32_t anchor = getState().harmonic.octaveBase * 12 + keyRoot;
	uint8_t rootPc = (uint8_t)((keyRoot + iv[deg]) % 12);
	if (rootPcOut) {
		*rootPcOut = rootPc;
	}

	int32_t yc = (y < 0) ? 0 : (y >= kDisplayHeight ? (kDisplayHeight - 1) : y);
	const Richness& rich = kLadder[yc];
	uint8_t count = 0;
	for (uint8_t k = 0; k < rich.count && count < maxNotes; k++) {
		int32_t st = (int32_t)deg + rich.steps[k];
		int32_t midi = anchor + floordiv(st, sc) * 12 + iv[floormod(st, sc)];
		if (midi < 0) {
			midi = 0;
		}
		if (midi > 127) {
			midi = 127;
		}
		notesOut[count++] = (int16_t)midi;
	}
	// Chromatic add (the BRIGHT major-6th): always a real major 6th above the root, borrowed if the scale
	// only has the flat 6 — so the "6" rung is a bright minor-6/major-6, not a dark b6.
	// NOTE: every write is guarded by `count < maxNotes`; callers must pass maxNotes <= sizeof(notesOut) (the
	// ladder's widest row is the 13th with count==kMaxChordKeyboardSize, so the buffer is exactly full there).
	if (rich.add >= 0 && count > 0 && count < maxNotes) {
		int32_t midi = (int32_t)notesOut[0] + rich.add;
		if (midi > 127) {
			midi = 127;
		}
		notesOut[count++] = (int16_t)midi;
	}

	bool flats = effectivePreferFlats(keyRoot, getScaleNotes());
	if (absOut) {
		sprintf(absOut, "%s%s", noteNameInKey(rootPc, flats), rich.suffix);
	}
	if (romanOut && sc == 7) {
		int8_t third = (int8_t)(((int32_t)iv[(deg + 2) % 7] - iv[deg] + 12) % 12);
		int8_t fifth = (int8_t)(((int32_t)iv[(deg + 4) % 7] - iv[deg] + 12) % 12);
		bool minorish = (third == 3);
		bool dim = (fifth == 6);
		bool aug = (fifth == 8);
		int8_t accidental = (int8_t)iv[deg] - (int8_t)kMajorIv[deg];
		char buf[32];
		int p = 0;
		for (int8_t a = 0; a < -accidental; a++) {
			buf[p++] = 'b';
		}
		for (int8_t a = 0; a < accidental; a++) {
			buf[p++] = '#';
		}
		for (const char* c = kNumerals[deg]; *c; c++) {
			buf[p++] = (minorish || dim) ? (char)(*c - 'A' + 'a') : *c;
		}
		if (dim) {
			buf[p++] = 'o';
		}
		else if (aug) {
			buf[p++] = '+';
		}
		buf[p] = '\0';
		sprintf(romanOut, "%s%s", buf, rich.suffix);
	}
	return count;
}

void KeyboardLayoutHarmonic::drawName(const char* roman, const char* abs) {
	char full[80];
	if (roman && roman[0]) {
		sprintf(full, "%s  %s", abs, roman);
	}
	else {
		sprintf(full, "%s", abs);
	}
	if (display->haveOLED()) {
		display->popupTextTemporary(full);
	}
	else {
		display->setScrollingText(full, 0);
	}
}

void KeyboardLayoutHarmonic::evaluatePads(PressedPad presses[kMaxNumKeyboardPadPresses]) {
	currentNotesState = NotesState{}; // Erase active notes
	uint8_t iv[12];
	uint8_t sc = getScaleIntervals(iv);
	uint8_t keyRoot = (uint8_t)getRootNote();
	uint8_t numCols = (sc > 7) ? 7 : sc;
	bool swapped = getState().harmonic.swapped;
	heldCols = 0;

	// Live held-pad feedback: rebuild from the current press state every call (the array is the full set of
	// active presses, not a delta), so renderPads can glow exactly the pads under a finger. Display region only.
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		heldPadRows[y] = 0;
	}
	for (int32_t idx = 0; idx < kMaxNumKeyboardPadPresses; idx++) {
		const PressedPad& pp = presses[idx];
		if (pp.active && pp.x >= 0 && pp.x < kDisplayWidth && pp.y >= 0 && pp.y < kDisplayHeight) {
			heldPadRows[pp.y] |= (uint16_t)(1u << pp.x);
		}
	}

	uint8_t palCtrlNow = 0;
	uint8_t isoCtrlNow = 0;
	// LEARN held = "what's this pad?" mode for the WHOLE surface: every pad (control, palette chord, iso note)
	// names itself on the display and NOTHING sounds or changes — pure inspect. (Cody: "it's not just for the
	// Deluge to learn, it's for YOU to learn.") Notes are off because currentNotesState was reset above and we
	// return before any enableNote. Naming fires on the rising edge only (learnHeld* masks), so a held pad pops
	// once instead of every frame — repeated popups would thrash the display and crackle the audio.
	bool learnHeld = Buttons::isButtonPressed(deluge::hid::button::LEARN);
	if (!learnHeld) {
		learnHeldLo = 0;
		learnHeldHi = 0;
		learnSbHeld = 0;
	}
	uint64_t learnCurLo = 0, learnCurHi = 0; // pads named this frame
	uint16_t learnSbCur = 0;                 // sidebar pads named this frame
	uint64_t isoNowMask = 0; // iso pads pressed this frame (bit = localX*8+y) — for voice-edit rising edge
	bool isoPlayed = false;
	bool leftPicked = false;
	for (int32_t idx = kMaxNumKeyboardPadPresses - 1; idx >= 0; --idx) {
		PressedPad pressed = presses[idx];
		if (!pressed.active) {
			continue;
		}
		// LEARN over the SIDEBAR: name velocity/mod, or — for a chord-bank column — emit the STORED chord's
		// notes so the Companion can name it ("the Deluge tells me what I banked by ear"). Rising-edge, silent.
		if (learnHeld && pressed.x >= kDisplayWidth && pressed.x < kDisplayWidth + kSideBarWidth && pressed.y >= 0
		    && pressed.y < kDisplayHeight) {
			uint16_t sbBit = (uint16_t)1u << ((pressed.x - kDisplayWidth) * kDisplayHeight + pressed.y);
			bool rising = !(learnSbHeld & sbBit);
			learnSbCur |= sbBit;
			if (rising) {
				ColumnControlState& cc = getState().columnControl;
				ColumnControlFunction func = (pressed.x == kDisplayWidth) ? cc.leftColFunc : cc.rightColFunc;
				char ctx[64];
				ctx[0] = '\0';
				uint8_t notes[controls::kMaxNotesChordMem];
				uint8_t n = 0;
				if (func == CHORD_MEM) {
					n = cc.chordMemColumn.peekChord(pressed.y, notes, controls::kMaxNotesChordMem);
				}
				else if (func == SONG_CHORD_MEM) {
					n = ChordMemService::peek(pressed.y, notes, controls::kMaxNotesChordMem);
				}
				if (func == CHORD_MEM || func == SONG_CHORD_MEM) {
					if (n > 0) {
						int p = snprintf(ctx, sizeof ctx, "notes:");
						for (uint8_t i = 0; i < n && p < (int)sizeof ctx - 5; i++) {
							p += snprintf(ctx + p, sizeof ctx - (size_t)p, "%s%d", i ? "," : "", (int)notes[i]);
						}
						display->displayPopup("BANK");
					}
					else {
						snprintf(ctx, sizeof ctx, "control:BANK"); // empty slot
						display->displayPopup("EMPTY");
					}
				}
				else if (func == VELOCITY) {
					snprintf(ctx, sizeof ctx, "control:VELOCITY");
					display->displayPopup("VELOCITY");
				}
				else if (func == MOD) {
					snprintf(ctx, sizeof ctx, "control:MOD");
					display->displayPopup("MOD");
				}
				if (ctx[0] != '\0') {
					HIDSysex::sendLearnContext(4 /*SIDEBAR*/, (uint8_t)pressed.x, (uint8_t)pressed.y, ctx);
				}
			}
			continue;
		}
		if (pressed.x >= kDisplayWidth) {
			continue;
		}
		ColInfo ci = colInfoFor(pressed.x, swapped);
		if (learnHeld) {
			// Inspect mode: name whatever this pad is, once per press, and fire nothing.
			if (pressed.y >= 0 && pressed.y < kDisplayHeight) {
				int32_t padId = pressed.x * kDisplayHeight + pressed.y; // 0..127
				uint64_t bit = (uint64_t)1u << (padId & 63);
				uint64_t& cur = (padId < 64) ? learnCurLo : learnCurHi;
				uint64_t& prev = (padId < 64) ? learnHeldLo : learnHeldHi;
				bool rising = !(prev & bit);
				cur |= bit;
				if (rising) {
					// Name it on the device, AND emit a context event to the host so the Companion can show the
					// full card (see chroma-schema SCHEMA.md 2.1). contextId is the stable key into the graph.
					char ctx[48];
					ctx[0] = '\0';
					if (ci.region == REG_PAL_CTRL) {
						display->displayPopup(controlPadName(REG_PAL_CTRL, pressed.y));
						snprintf(ctx, sizeof ctx, "control:%s", controlPadId(REG_PAL_CTRL, pressed.y));
					}
					else if (ci.region == REG_ISO_CTRL) {
						display->displayPopup(controlPadName(REG_ISO_CTRL, pressed.y));
						snprintf(ctx, sizeof ctx, "control:%s", controlPadId(REG_ISO_CTRL, pressed.y));
					}
					else if (ci.region == REG_PAL && ci.local < numCols) {
						int16_t nbuf[kMaxChordKeyboardSize];
						uint8_t rpc = 0;
						char roman[32], abs[32];
						buildChordAtDegree((uint8_t)ci.local, pressed.y, iv, sc, keyRoot, nbuf, kMaxChordKeyboardSize,
						                   &rpc, roman, abs);
						char buf[40];
						snprintf(buf, sizeof buf, "%s  %s", roman, abs);
						display->displayPopup(buf);
						// contextId = clean degree roman (triad row) + richness label of the row tapped.
						int16_t nb2[kMaxChordKeyboardSize];
						uint8_t rpc2 = 0;
						char roman2[32], abs2[32];
						buildChordAtDegree((uint8_t)ci.local, 2, iv, sc, keyRoot, nb2, kMaxChordKeyboardSize, &rpc2,
						                   roman2, abs2);
						if (roman2[0] == '\0') {
							snprintf(roman2, sizeof roman2, "deg%d", (int)ci.local + 1);
						}
						int32_t yc =
						    (pressed.y < 0) ? 0 : (pressed.y >= kDisplayHeight ? kDisplayHeight - 1 : pressed.y);
						const char* richLbl = (yc == 0) ? "root" : (yc == 2) ? "triad" : kLadder[yc].suffix;
						snprintf(ctx, sizeof ctx, "harmonicObject:%s:%s", roman2, richLbl);
					}
					else if (ci.region == REG_ISO && ci.local >= 0 && ci.local < kBlockWidth) {
						int32_t note = getState().harmonic.isoChromatic ? isoNoteChromatic(ci.local, pressed.y)
						                                                : isoNoteAt(ci.local, pressed.y);
						if (note >= 0 && note <= 127) {
							char buf[8];
							snprintf(buf, sizeof buf, "%s%d", noteNameInKey((uint8_t)(note % 12), false),
							         (int)(note / 12 - 1));
							display->displayPopup(buf);
							snprintf(ctx, sizeof ctx, "isoNote:%s%d", noteNameInKey((uint8_t)(note % 12), false),
							         (int)(note / 12 - 1));
						}
					}
					if (ctx[0] != '\0') {
						HIDSysex::sendLearnContext((uint8_t)ci.region, (uint8_t)pressed.x, (uint8_t)pressed.y, ctx);
					}
				}
			}
			continue;
		}
		if (ci.region == REG_PAL_CTRL) {
			if (pressed.y >= 0 && pressed.y < kDisplayHeight) {
				palCtrlNow |= (uint8_t)(1u << pressed.y);
			}
			continue;
		}
		if (ci.region == REG_ISO_CTRL) {
			if (pressed.y >= 0 && pressed.y < kDisplayHeight) {
				isoCtrlNow |= (uint8_t)(1u << pressed.y);
			}
			continue;
		}
		if (ci.region == REG_ISO) {
			// The iso panel. Sound the held note (free-play, or feedback while voice-editing). In EDIT mode a
			// rising-edge press toggles this note in/out of the voicing (handled after the loop).
			isoPlayed = true;
			if (ci.local >= 0 && ci.local < kBlockWidth && pressed.y >= 0 && pressed.y < kDisplayHeight) {
				isoNowMask |= (uint64_t)1u << (ci.local * kDisplayHeight + pressed.y);
			}
			// Free play sounds the tapped note. In EDIT mode we DON'T sound single taps — the whole voicing
			// drones (after the loop) so you hear the CHORD you're sculpting, not isolated pings.
			bool stripPad = (getState().harmonic.stackPick && pressed.y == 0); // bottom row = octave picker
			if (!getState().harmonic.editVoicing && !stripPad) {
				int32_t note = getState().harmonic.isoChromatic ? isoNoteChromatic(ci.local, pressed.y)
				                                                : isoNoteAt(ci.local, pressed.y);
				if (note >= 0 && note <= 127) {
					enableNote((uint8_t)note, velocity);
				}
			}
			continue;
		}
		if (ci.region == REG_PAL && ci.local < numCols) {
			// The chord explorer — columns are the diatonic degrees, in order.
			uint8_t deg = (uint8_t)ci.local;
			int16_t notes[kMaxChordKeyboardSize];
			uint8_t rootPc = 0;
			char roman[32], abs[32];
			uint8_t n =
			    buildChordAtDegree(deg, pressed.y, iv, sc, keyRoot, notes, kMaxChordKeyboardSize, &rootPc, roman, abs);
			// Name the pick ONCE (or when the richness row changes under a held finger). evaluatePads runs every
			// frame while the pad is held; re-calling drawName each frame restarts the scroll so the roman never
			// reaches the 7-seg. namedDeg/namedRich reset to -1 on release (below), so a re-press re-shows it.
			int8_t thisRich =
			    (int8_t)((pressed.y < 0) ? 0 : (pressed.y >= kDisplayHeight ? kDisplayHeight - 1 : pressed.y));
			if (deg != namedDeg || thisRich != namedRich) {
				drawName(roman, abs);
				namedDeg = (int8_t)deg;
				namedRich = thisRich;
			}
			// Remember the chord we're LEAVING (its pitch classes) for the DIFF / voice-leading view.
			{
				uint16_t m = 0;
				for (uint8_t i = 0; i < chordNoteCount; i++) {
					m |= (uint16_t)(1u << (((chordNotes[i] % 12) + 12) % 12));
				}
				getState().harmonic.prevChordPcMask = m;
				getState().harmonic.prevSelDeg = selDeg;
			}
			// Remember the EXACT voiced notes so the iso panel lights this one voicing.
			chordNoteCount = 0;
			for (uint8_t i = 0; i < n; i++) {
				chordNotes[chordNoteCount++] = notes[i];
			}
			// Play the VOICED chord (stack + spread applied) so what you PLAY sounds like the voicing.
			int16_t voicedPlay[kMaxVoice];
			uint8_t vnPlay = buildVoicing(voicedPlay, kMaxVoice);
			for (uint8_t i = 0; i < vnPlay; i++) {
				enableNote((uint8_t)voicedPlay[i], velocity);
			}
			// Chroma: push the chord-state to the host on this NORMAL pick (the host renders the real chord from
			// key + voiced notes + contextId). Same contextId grammar as LEARN. Not reached while LEARN is held.
			{
				int32_t yc = (pressed.y < 0) ? 0 : (pressed.y >= kDisplayHeight ? kDisplayHeight - 1 : pressed.y);
				const char* richLbl = (yc == 0) ? "root" : (yc == 2) ? "triad" : kLadder[yc].suffix;
				char ctxState[40];
				snprintf(ctxState, sizeof ctxState, "harmonicObject:%s:%s", roman[0] ? roman : "deg", richLbl);
				// Remember this chord's key + context so voicing changes (SPRD/INV/STACK) can re-broadcast it.
				curKeyRoot_ = (uint8_t)keyRoot;
				strncpy(curCtx_, ctxState, sizeof(curCtx_) - 1);
				curCtx_[sizeof(curCtx_) - 1] = '\0';
				HIDSysex::sendChordState((uint8_t)keyRoot, voicedPlay, vnPlay, ctxState,
				                         getState().harmonic.voiceSpread, getState().harmonic.voiceInversion,
				                         (uint8_t)currentSong->getCurrentScale());
			}
			heldCols |= (uint16_t)(1u << ci.local);
			leftPicked = true;
			getState().harmonic.showChord = true; // picking a chord always shows it (no silent hidden state)
			// SMART SNAP (optional): bring the iso to the chord's register so the voicing always lights up on
			// pick. Turn it OFF (purple SNAP pad) to keep the iso parked where you scrolled it — play the chord
			// low and riff the melody up high on the right. The iso can still be scrolled freely either way.
			if (getState().harmonic.isoFollowsChord) {
				getState().harmonic.isoOctave = getState().harmonic.octaveBase;
			}
			// Persist the selection so the highlight + iso shape stay after release, and ask the Calculator
			// where to go next (suggested degree columns flash in renderPads).
			selDeg = (int8_t)deg;
			selRichness =
			    (int8_t)((pressed.y < 0) ? 0 : (pressed.y >= kDisplayHeight ? kDisplayHeight - 1 : pressed.y));
			recomputeSuggestions(keyRoot, iv, sc, rootPc);
		}
	}

	// LEARN inspect mode ends here: commit which pads were named, wipe the rising-edge trackers so releasing
	// LEARN doesn't fire a phantom control press, and return BEFORE any control/PROG/note processing — the
	// surface stays silent and only the name popups happened.
	if (learnHeld) {
		learnHeldLo = learnCurLo;
		learnHeldHi = learnCurHi;
		learnSbHeld = learnSbCur;
		palCtrlHeldMask = 0;
		isoCtrlHeldMask = 0;
		isoHeldMask = 0;
		progPadHeld = false;
		heldCols = 0;
		return;
	}

	KeyboardStateHarmonic& hs = getState().harmonic;

	auto clearSelection = [&]() {
		chordNoteCount = 0;
		selDeg = -1;
		selRichness = -1;
		numSuggestions = 0;
		topDeg = -1;
		for (uint8_t d = 0; d < 7; d++) {
			degBright[d] = 0;
		}
		// Also drop the shared chord-memory "shape" highlight (the 255 sentinels) so it doesn't linger on the
		// iso / in-key / piano layouts after you clear in Chroma.
		keyboardScreen.clearChordShapeHighlight();
	};

	// OCTAVE PICKER: in stack-pick mode, rising bottom-row iso pads toggle which octaves are in the stack
	// (local 0..6 = offsets -3..+3; local 3 = base octave, always on).
	if (hs.stackPick) {
		uint64_t risingPads = isoNowMask & ~isoHeldMask;
		bool octChanged = false;
		for (int32_t lx = 0; lx < kBlockWidth; lx++) {
			if ((risingPads & ((uint64_t)1u << (lx * kDisplayHeight))) && lx != 3) {
				hs.voiceOctaves ^= (uint8_t)(1u << lx);
				octChanged = true;
			}
		}
		hs.voiceOctaves |= (uint8_t)(1u << 3);
		// Re-strike the voicing so you HEAR the stack change immediately (Theory-Board feel).
		if (octChanged && chordNoteCount > 0) {
			int16_t v[kMaxVoice];
			uint8_t vn = buildVoicing(v, kMaxVoice);
			for (uint8_t i = 0; i < vn; i++) {
				if (v[i] >= 0 && v[i] <= 127) {
					enableNote((uint8_t)v[i], velocity);
				}
			}
		}
	}

	// ISO voice-EDIT: a rising-edge iso press toggles that note IN/OUT of the selected chord's voicing.
	if (hs.editVoicing) {
		uint64_t risingIsoPads = isoNowMask & ~isoHeldMask;
		bool edited = false;
		for (int32_t b = 0; b < kBlockWidth * kDisplayHeight; b++) {
			if (!(risingIsoPads & ((uint64_t)1u << b))) {
				continue;
			}
			int32_t lx = b / kDisplayHeight, yy = b % kDisplayHeight;
			if (hs.stackPick && yy == 0) {
				continue; // bottom row belongs to the octave picker
			}
			int32_t note = hs.isoChromatic ? isoNoteChromatic(lx, yy) : isoNoteAt(lx, yy);
			if (note < 0 || note > 127) {
				continue;
			}
			int32_t found = -1;
			for (uint8_t i = 0; i < chordNoteCount; i++) {
				if (chordNotes[i] == note) {
					found = (int32_t)i;
					break;
				}
			}
			if (found >= 0) { // remove this note from the voicing
				for (uint8_t i = (uint8_t)found; i + 1 < chordNoteCount; i++) {
					chordNotes[i] = chordNotes[i + 1];
				}
				chordNoteCount--;
			}
			else if (chordNoteCount < kMaxChordKeyboardSize) { // add it
				chordNotes[chordNoteCount++] = (int16_t)note;
			}
			edited = true;
		}
		// Each edit, CALCULATE + name the chord you've built so you can SEE what you made (works even when
		// you started from nothing). No auto-hold — tap AUDITION to strike the voicing and hear it ring.
		if (edited && chordNoteCount > 0) {
			uint8_t nu[kMaxChordKeyboardSize];
			uint8_t nc = 0;
			for (uint8_t i = 0; i < chordNoteCount && nc < kMaxChordKeyboardSize; i++) {
				if (chordNotes[i] >= 0 && chordNotes[i] <= 127) {
					nu[nc++] = (uint8_t)chordNotes[i];
				}
			}
			char nm[48];
			if (nameChordFromNotes(nu, nc, nm, effectivePreferFlats(keyRoot, getScaleNotes()))) {
				drawName(nullptr, nm);
			}
		}
	}
	// Free play on the iso (without picking a chord) resets out of "chord mode" — unless sticky is on (or
	// voice-edit, handled above, which never clears).
	else if (isoPlayed && !leftPicked && !hs.stickyChord && !hs.stackPick) {
		clearSelection();
	}
	isoHeldMask = isoNowMask;

	// ── ISO control column (rising-edge so holding doesn't repeat) ──
	uint8_t risingIso = (uint8_t)(isoCtrlNow & ~isoCtrlHeldMask);
	if (risingIso & (uint8_t)(1u << kBtnIsoView)) {
		hs.isoChromatic = !hs.isoChromatic;
		display->displayPopup(hs.isoChromatic ? "CHRO" : "KEY");
	}
	if (risingIso & (uint8_t)(1u << kBtnShowChord)) {
		hs.showChord = !hs.showChord;
		display->displayPopup(hs.showChord ? "SHOW" : "HIDE");
	}
	if (risingIso & (uint8_t)(1u << kBtnSticky)) {
		hs.stickyChord = !hs.stickyChord; // keep the selection on-screen for editing (no sustained sound)
		display->displayPopup(hs.stickyChord ? "HOLD" : "FREE");
	}
	if (risingIso & (uint8_t)(1u << kBtnLattice)) {
		// The overlay pad cycles: ONE (off) -> LATT (full lattice) -> DIFF (voice-leading) -> ONE.
		if (!hs.latticeOn && !hs.diffOn) {
			hs.latticeOn = true;
			hs.showChord = true; // an overlay needs the chord shown so it can't silently do nothing
			display->displayPopup("LATT");
		}
		else if (hs.latticeOn) {
			hs.latticeOn = false;
			hs.diffOn = true;
			hs.showChord = true;
			display->displayPopup("DIFF");
		}
		else {
			hs.diffOn = false;
			display->displayPopup("ONE");
		}
	}
	if (risingIso & (uint8_t)(1u << kBtnSnap)) {
		hs.isoFollowsChord = !hs.isoFollowsChord;
		// SNAP = iso jumps to the chord on pick. STAY = iso holds where you left it (play chord low, riff high).
		display->displayPopup(hs.isoFollowsChord ? "SNAP" : "STAY");
	}
	// PROGRESSION (hold-to-dial): while purple row 2 is HELD, the encoders dial the progression — vertical wheel
	// picks WHICH progression, horizontal wheel WALKS the chords. Holding also SUSTAINS the current step so you
	// hear it; release leaves the chord loaded. First press previews the current (or first) progression.
	bool progNow = (isoCtrlNow & (uint8_t)(1u << kBtnProg)) != 0;
	if (progNow && !progPadHeld) {
		ensureProgsLoaded(); // lazy scan of CHORDS/*.chordpack on first use
		if (hs.progPreset < 0 || hs.progPreset >= gNumProgs) {
			hs.progPreset = 0;
			hs.progStep = 0;
		}
		loadProgStep();
		if (gNumProgs > 0) {
			display->displayPopup(gProgs[hs.progPreset].name);
		}
	}
	if (progNow && hs.progPreset >= 0) {
		int16_t pv[kMaxVoice];
		uint8_t pvn = buildVoicing(pv, kMaxVoice);
		for (uint8_t i = 0; i < pvn; i++) {
			if (pv[i] >= 0 && pv[i] <= 127) {
				enableNote((uint8_t)pv[i], velocity);
			}
		}
	}
	progPadHeld = progNow;
	if (risingIso & (uint8_t)(1u << kBtnEdit)) {
		hs.editVoicing = !hs.editVoicing;
		if (hs.editVoicing) {
			hs.showChord = true;   // editing needs the chord shown
			hs.stickyChord = true; // and HELD — so the chord survives editing/playing, doesn't vanish on a tap
		}
		display->displayPopup(hs.editVoicing ? "EDIT" : "PLAY");
	}
	if (risingIso & kIsoCtrlClearMask) {
		clearSelection();
		display->displayPopup("CLR");
	}
	// AUDITION (momentary): while held, sound the current VOICING (base chord through spread + stack).
	if (isoCtrlNow & (uint8_t)(1u << kBtnAudition)) {
		int16_t voiced[kMaxVoice];
		uint8_t vn = buildVoicing(voiced, kMaxVoice);
		for (uint8_t i = 0; i < vn; i++) {
			if (voiced[i] >= 0 && voiced[i] <= 127) {
				enableNote((uint8_t)voiced[i], velocity);
			}
		}
	}
	isoCtrlHeldMask = isoCtrlNow;

	// ── PALETTE control column ──
	uint8_t risingPal = (uint8_t)(palCtrlNow & ~palCtrlHeldMask);
	if (risingPal & (uint8_t)(1u << kBtnCalc)) {
		hs.calculatorOn = !hs.calculatorOn;
		if (!hs.calculatorOn) {
			numSuggestions = 0;
			topDeg = -1;
			for (uint8_t d = 0; d < 7; d++) {
				degBright[d] = 0;
			}
		}
		display->displayPopup(hs.calculatorOn ? "CALC" : "OFF");
	}
	if (risingPal & (uint8_t)(1u << kBtnSwap)) {
		hs.swapped = !hs.swapped;
		display->displayPopup(hs.swapped ? "SWAP" : "NORM");
	}
	if (risingPal & (uint8_t)(1u << kBtnPalOctUp)) {
		if (hs.octaveBase < 8) {
			hs.octaveBase++;
		}
		char buf[8];
		sprintf(buf, "OCT%d", (int)hs.octaveBase);
		display->displayPopup(buf);
	}
	if (risingPal & (uint8_t)(1u << kBtnPalOctDown)) {
		if (hs.octaveBase > 1) {
			hs.octaveBase--;
		}
		char buf[8];
		sprintf(buf, "OCT%d", (int)hs.octaveBase);
		display->displayPopup(buf);
	}
	if (risingPal & (uint8_t)(1u << kBtnStack)) {
		hs.stackPick = !hs.stackPick;
		hs.voiceOctaves |= (uint8_t)(1u << 3);
		display->displayPopup(hs.stackPick ? "PICK" : "STAK"); // "PICK" = octave picker open (distinct from OCT3)
		pushChordState();                                      // dashboard tracks the change live
	}
	if (risingPal & (uint8_t)(1u << kBtnSpread)) {
		hs.voiceSpread = (int8_t)((hs.voiceSpread + 1) % 4);
		char sbuf[8];
		sprintf(sbuf, "SPR%d", (int)hs.voiceSpread);
		display->displayPopup(sbuf);
		pushChordState(); // dashboard tracks the change live
	}
	if (risingPal & (uint8_t)(1u << kBtnInversion)) {
		hs.voiceInversion = (int8_t)((hs.voiceInversion + 1) % 4); // root → 1st → 2nd → 3rd → root
		char ibuf[8];
		sprintf(ibuf, "INV%d", (int)hs.voiceInversion);
		display->displayPopup(ibuf);
		if (chordNoteCount > 0) { // re-strike so you HEAR the inversion immediately
			int16_t v[kMaxVoice];
			uint8_t vn = buildVoicing(v, kMaxVoice);
			for (uint8_t i = 0; i < vn; i++) {
				if (v[i] >= 0 && v[i] <= 127) {
					enableNote((uint8_t)v[i], velocity);
				}
			}
		}
		pushChordState(); // dashboard tracks the change live
	}
	if (risingPal & kPalCtrlClearMask) {
		if (progPadHeld) {
			// While walking a progression, CLEAR strips/restores the baked voicings (VOICED <-> BARE).
			hs.progVoiced = !hs.progVoiced;
			loadProgStep(); // re-apply the current step with / without its voicing
			display->displayPopup(hs.progVoiced ? "VOIC" : "BARE");
		}
		else {
			clearSelection();
			display->displayPopup("CLR");
		}
	}
	palCtrlHeldMask = palCtrlNow;

	// Spring-loaded voicing: the instant the last chord pad is released (heldCols falls to 0), the voicing dial
	// springs back to HOME so the next chord starts from your set voice (or the plain default if none was set).
	// Sweep it while you hold, let go and it settles home — no button needed. (Press resets without releasing;
	// Shift+press sets the current voice as home.)
	if (prevHeldCols != 0 && heldCols == 0 && hs.voicingWalk != hs.voicingHome) {
		hs.voicingWalk = hs.voicingHome;
	}
	if (heldCols == 0) {
		namedDeg = -1; // released — let the next pick (even the same chord) scroll its name again
		namedRich = -1;
	}
	prevHeldCols = heldCols;

	ColumnControlsKeyboard::evaluatePads(presses);
}

// PROG: latch the current preset's current step as the selected Harmonic Object — same effect as picking that
// chord from the palette (sets chordNotes, lights its cell, shows the voicing on the iso), but no sound here;
// the audition is sustained by the strip handler while the pad is held.
void KeyboardLayoutHarmonic::loadProgStep() {
	KeyboardStateHarmonic& h = getState().harmonic;
	if (h.progPreset < 0 || h.progPreset >= gNumProgs) {
		return;
	}
	const ProgEntry& p = gProgs[h.progPreset];
	if (h.progStep < 0 || h.progStep >= (int8_t)p.count) {
		return;
	}
	uint8_t iv[12];
	uint8_t sc = getScaleIntervals(iv);
	uint8_t keyRoot = (uint8_t)getRootNote();
	uint8_t numCols = (sc > 7) ? 7 : sc;
	uint8_t deg = (uint8_t)p.steps[h.progStep].degree;
	if (deg >= numCols) {
		deg = 0; // guard for scales with fewer than 7 notes
	}
	int32_t rich = p.steps[h.progStep].rich;
	int16_t notes[kMaxChordKeyboardSize];
	uint8_t rootPc = 0;
	char roman[32], abs[32];
	uint8_t n = buildChordAtDegree(deg, rich, iv, sc, keyRoot, notes, kMaxChordKeyboardSize, &rootPc, roman, abs);
	drawName(roman, abs);
	// Remember the chord we're LEAVING (its pitch classes) for the DIFF / voice-leading view.
	{
		uint16_t m = 0;
		for (uint8_t i = 0; i < chordNoteCount; i++) {
			m |= (uint16_t)(1u << (((chordNotes[i] % 12) + 12) % 12));
		}
		h.prevChordPcMask = m;
		h.prevSelDeg = selDeg;
	}
	chordNoteCount = 0;
	for (uint8_t i = 0; i < n; i++) {
		chordNotes[chordNoteCount++] = notes[i];
	}
	// Apply the step's BAKED voicing — or strip to a bare chord in BARE mode. Live STACK/SPREAD/INV reshape on
	// top either way (this just sets the starting point each time a step loads).
	if (h.progVoiced) {
		const ProgStep& st = p.steps[h.progStep];
		h.voiceOctaves = (st.oct != 0) ? st.oct : (uint8_t)(1u << 3); // 0 = base only (built-ins have no voicing)
		h.voiceSpread = st.spread;
		h.voiceInversion = st.inv;
	}
	else {
		h.voiceOctaves = (uint8_t)(1u << 3); // bare: base octave only
		h.voiceSpread = 0;
		h.voiceInversion = 0;
	}
	h.showChord = true;
	if (h.isoFollowsChord) {
		h.isoOctave = h.octaveBase;
	}
	selDeg = (int8_t)deg;
	selRichness = (int8_t)rich;
	recomputeSuggestions(keyRoot, iv, sc, rootPc);
}

// Spring-loaded voicing, vertical-encoder PRESS-DOWN: just arm. We don't spring yet — we wait for release, so we
// can tell a clean click (spring to home) apart from a press-and-turn (dial the home, handled in the encoder).
bool KeyboardLayoutHarmonic::voicingPressBegin() {
	voicingTurnedWhilePressed = false;
	return true; // consume the press so a plain click doesn't fall through to other actions
}

// RELEASE: if you didn't turn while pressed, it was a clean click — spring the voicing back to home. If you did
// press-and-turn, the home was already dialed, so leave it. (Releasing the CHORD also springs home; see evaluatePads.)
bool KeyboardLayoutHarmonic::voicingPressEnd() {
	if (!voicingTurnedWhilePressed) {
		KeyboardStateHarmonic& s = getState().harmonic;
		if (s.voicingWalk != s.voicingHome) {
			s.voicingWalk = s.voicingHome;
			char buf[8];
			sprintf(buf, "VOI%d", (int)s.voicingWalk);
			display->displayPopup(buf);
			pushChordState();
		}
	}
	return true;
}

void KeyboardLayoutHarmonic::handleVerticalEncoder(int32_t offset) {
	// PROG hold: the vertical wheel BROWSES the progression library (which progression). Resets to step 1.
	if (progPadHeld) {
		ensureProgsLoaded();
		if (gNumProgs <= 0) {
			return;
		}
		KeyboardStateHarmonic& s = getState().harmonic;
		int32_t p = (s.progPreset < 0 ? 0 : s.progPreset) + ((offset > 0) ? 1 : -1);
		if (p < 0) {
			p = 0;
		}
		if (p >= gNumProgs) {
			p = gNumProgs - 1;
		}
		s.progPreset = (int8_t)p;
		s.progStep = 0;
		loadProgStep();
		display->displayPopup(gProgs[p].name);
		return;
	}
	// VOICING DIAL: while a chord pad is HELD, the vertical wheel walks the voicing one note at a time
	// (Orchid-style) instead of scrolling the iso. The chord is ringing, so you HEAR each step — hold the
	// pad, turn the wheel, sweep the voicing. heldCols is refreshed every evaluatePads, so it's set exactly
	// while a palette pad is under a finger. The held pad re-sounds through buildVoicing on the next evaluate
	// (keyboard_screen re-runs it after this call while auditioning), so no note-triggering is needed here.
	if (heldCols != 0) {
		KeyboardStateHarmonic& s = getState().harmonic;
		// PRESS + TURN: hold the encoder in and turn to dial the HOME the voicing springs back to. The home
		// tracks the walk as you turn, so you hear it settle where you leave it. A plain turn (encoder not
		// pressed) just sweeps the voicing away from home as before.
		if (Buttons::isButtonPressed(deluge::hid::button::Y_ENC)) {
			voicingTurnedWhilePressed = true; // so releasing the click won't spring it away
			int32_t h = (int32_t)s.voicingWalk + offset;
			if (h < -24) {
				h = -24;
			}
			if (h > 24) {
				h = 24;
			}
			s.voicingWalk = (int8_t)h;
			s.voicingHome = s.voicingWalk; // home follows — this is now the spring-back voice
			char buf[8];
			sprintf(buf, "H%d", (int)s.voicingHome); // "H-24".."H24" fits the 4-char 7-seg
			display->displayPopup(buf);
			pushChordState();
			return;
		}
		int32_t w = (int32_t)s.voicingWalk + offset;
		if (w < -24) {
			w = -24;
		}
		if (w > 24) {
			w = 24;
		}
		s.voicingWalk = (int8_t)w;
		char buf[8];
		sprintf(buf, "V%d", (int)s.voicingWalk); // "V-24".."V24" fits the 4-char 7-seg (VOI%d overflowed)
		display->displayPopup(buf);
		pushChordState(); // dashboard tracks the voicing walk live
		return;
	}
	if (verticalEncoderHandledByColumns(offset)) {
		return;
	}
	// Vertical encoder scrolls the ISO independently (like the native iso keyboard) — the "octave shown".
	// The PALETTE octave is separate (pal-ctrl OCT+/- buttons), so you set chord register + iso view apart.
	KeyboardStateHarmonic& state = getState().harmonic;
	state.isoOctave += offset;
	if (state.isoOctave < 1) {
		state.isoOctave = 1;
	}
	if (state.isoOctave > 8) {
		state.isoOctave = 8;
	}
	precalculate();
}

void KeyboardLayoutHarmonic::handleHorizontalEncoder(int32_t offset, bool shiftEnabled,
                                                     PressedPad presses[kMaxNumKeyboardPadPresses],
                                                     bool encoderPressed) {
	// A SELECT-encoder turn calls this with offset==0 purely to recompute scroll bounds. It must NOT trigger the
	// prog/stack/arp actions below — otherwise SELECT doubles as the horizontal wheel (e.g. nudging the arp flavor).
	if (offset == 0) {
		horizontalEncoderHandledByColumns(offset, shiftEnabled);
		return;
	}
	// PROG hold: the horizontal wheel WALKS the loaded progression's chords (wraps, so a vamp loops).
	if (progPadHeld && getState().harmonic.progPreset >= 0 && getState().harmonic.progPreset < gNumProgs) {
		KeyboardStateHarmonic& s = getState().harmonic;
		int32_t cnt = (int32_t)gProgs[s.progPreset].count;
		if (cnt > 0) {
			int32_t st = (s.progStep + ((offset > 0) ? 1 : -1)) % cnt;
			if (st < 0) {
				st += cnt;
			}
			s.progStep = (int8_t)st;
			loadProgStep();
		}
		return;
	}
	// STACK (perform): while a chord pad is HELD, the horizontal wheel blooms the voicing — each step adds or
	// removes an octave layer outward from the base, thickening the chord. Re-sounds live (the chord is ringing).
	if (heldCols != 0) {
		KeyboardStateHarmonic& s = getState().harmonic;
		static const uint8_t order[6] = {2, 4, 1, 5, 0, 6}; // grow outward: -1, +1, -2, +2, -3, +3 octaves
		if (offset > 0) {
			for (int32_t k = 0; k < 6; k++) {
				if (!(s.voiceOctaves & (uint8_t)(1u << order[k]))) {
					s.voiceOctaves |= (uint8_t)(1u << order[k]);
					break;
				}
			}
		}
		else {
			for (int32_t k = 5; k >= 0; k--) {
				if (s.voiceOctaves & (uint8_t)(1u << order[k])) {
					s.voiceOctaves &= (uint8_t)~(1u << order[k]);
					break;
				}
			}
		}
		uint8_t layers = 0;
		for (int32_t k = 0; k < 6; k++) {
			if (s.voiceOctaves & (uint8_t)(1u << order[k])) {
				layers++;
			}
		}
		char buf[8];
		sprintf(buf, "STK%d", (int)layers);
		display->displayPopup(buf);
		pushChordState();
		return;
	}
	horizontalEncoderHandledByColumns(offset, shiftEnabled);
}

void KeyboardLayoutHarmonic::renderPads(RGB image[][kDisplayWidth + kSideBarWidth]) {
	// Chroma WRITE direction: drain any inbound voicing-mods (0x44) the host sent. This is the UI-thread tick
	// requestsContinuousRender() asked for, so it's safe to touch voicing state + re-broadcast 0x43 here.
	{
		uint8_t modType, modValue;
		while (HIDSysex::takeChordMod(&modType, &modValue)) {
			applyInboundMod(modType, modValue);
		}
	}
	// Sidebar stays at the familiar factory default (velocity / mod). The chord BANK is opt-in: hold a sidebar
	// column + turn the vertical encoder to switch it to CHORD_MEM. (Removed the auto-default — it pulled the
	// still-maturing bank into the first-run experience and surfaced a clip-duplication bug; see PR notes.)
	uint8_t iv[12];
	uint8_t sc = getScaleIntervals(iv);
	uint8_t keyRoot = (uint8_t)getRootNote();
	uint8_t numCols = (sc > 7) ? 7 : sc;
	bool chromatic = getState().harmonic.isoChromatic;
	bool showChord = getState().harmonic.showChord;
	bool sticky = getState().harmonic.stickyChord;
	bool calc = getState().harmonic.calculatorOn;
	bool swapped = getState().harmonic.swapped;
	bool latticeOn = getState().harmonic.latticeOn;
	bool diffOn = getState().harmonic.diffOn;
	uint16_t prevPcMask = getState().harmonic.prevChordPcMask;
	int8_t prevSelDeg = getState().harmonic.prevSelDeg;
	bool isoFollowsChord = getState().harmonic.isoFollowsChord;
	bool editVoicing = getState().harmonic.editVoicing;
	bool stackPick = getState().harmonic.stackPick;
	uint8_t voiceOctaves = getState().harmonic.voiceOctaves;
	int8_t voiceSpread = getState().harmonic.voiceSpread;
	int8_t voiceInversion = getState().harmonic.voiceInversion;
	int32_t isoStart = isoStartCol(swapped);
	// The highlighted chord on the iso wears its PALETTE colour (the selected degree's hue) — bright primary,
	// faded repeats. Falls back to white when there's no degree (e.g. a voicing built from scratch in EDIT).
	RGB chordHue = (selDeg >= 0 && selDeg < 7) ? kDegreeHue[selDeg] : RGB{.r = 255, .g = 255, .b = 255};
	(void)iv;

	// Breathing pulse for the Calculator's next-chord suggestions (same cadence as the Chord Library).
	uint8_t phase = (AudioEngine::audioSampleTimer >> 7) & 0xFF;     // sawtooth, full cycle ~0.75s
	uint8_t tri = (phase < 128) ? (phase * 2) : ((255 - phase) * 2); // triangle 0..255..0
	uint8_t pulse = 30 + (uint8_t)((uint32_t)tri * 225 / 255);       // breathe between dim and full

	// The actual VOICING (base chord expanded through SPREAD + STACK) — this is what's shown + played.
	int16_t voiced[kMaxVoice];
	uint8_t voicedN = buildVoicing(voiced, kMaxVoice);
	// BASS SPOTLIGHT: the chord's bass note — one octave below the voicing's lowest note. It lives at its TRUE
	// register, so it scrolls with the iso like the main keyboard (and may sit below the current view — that's
	// positional integrity, you scroll down to it). -1 = no chord loaded.
	int32_t bassSpotMidi = -1;
	if (voicedN > 0) {
		int16_t lo = voiced[0];
		for (uint8_t i = 1; i < voicedN; i++) {
			if (voiced[i] < lo) {
				lo = voiced[i];
			}
		}
		bassSpotMidi = (int32_t)lo - 12;
	}
	// Match a pad's note against the EXACT voiced notes (not pitch classes).
	auto inChordExact = [&](int32_t note) {
		for (uint8_t i = 0; i < voicedN; i++) {
			if (voiced[i] == note) {
				return true;
			}
		}
		return false;
	};
	// Pitch-class versions for the full LATTICE (every octave/position of a chord tone, not just the voicing).
	auto inChordPc = [&](uint8_t pcq) {
		for (uint8_t i = 0; i < chordNoteCount; i++) {
			if ((uint8_t)(((chordNotes[i] % 12) + 12) % 12) == pcq) {
				return true;
			}
		}
		return false;
	};
	auto isCorePc = [&](uint8_t pcq) {
		uint8_t coreCount = (chordNoteCount < 4) ? chordNoteCount : 4;
		for (uint8_t i = 0; i < coreCount; i++) {
			if ((uint8_t)(((chordNotes[i] % 12) + 12) % 12) == pcq) {
				return true;
			}
		}
		return false;
	};

	// The voicing repeats up the iso grid. Pick ONE pad per voiced note — the lowest (bottom-most, then
	// left-most) occurrence — to draw at full brightness; repeats render dimmer, so one clean shape reads.
	bool primary[kDisplayHeight][kDisplayWidth] = {};
	for (uint8_t i = 0; i < voicedN; i++) {
		bool placed = false;
		for (int32_t yy = 0; yy < kDisplayHeight && !placed; yy++) {
			for (int32_t lx = 0; lx < kBlockWidth && !placed; lx++) {
				int32_t nn = chromatic ? isoNoteChromatic(lx, yy) : isoNoteAt(lx, yy);
				if (nn == voiced[i]) {
					primary[yy][isoStart + lx] = true;
					placed = true;
				}
			}
		}
	}

	for (int32_t x = 0; x < kDisplayWidth; x++) {
		ColInfo ci = colInfoFor(x, swapped);

		if (ci.region == REG_PAL) {
			// The chord explorer. Each column a distinct hue; the triad (bottom) is lightest and the column
			// darkens upward as the chord grows lusher. Calculator flashes suggested degrees; selected cell pops.
			int32_t local = ci.local;
			if (local >= numCols) {
				for (int32_t y = 0; y < kDisplayHeight; y++) {
					image[y][x] = RGB{};
				}
				continue;
			}
			RGB hue = kDegreeHue[local % 7];
			bool haveCalc = (numSuggestions > 0);
			for (int32_t y = 0; y < kDisplayHeight; y++) {
				RGB c = hue.adjustFractional(kRichBright[y], 255);
				if (local == selDeg && y == selRichness) {
					// Selected chord cell: bright near-white tint of the column colour. Blend the FULL-brightness
					// hue (not the row-dimmed one) so the selection pops just as hard on the lusher upper rows
					// (7/9/11/13) as on the triad — otherwise a dim row makes a picked chord read as "not lit".
					c = RGB{.r = (uint8_t)((hue.r + 255) >> 1),
					        .g = (uint8_t)((hue.g + 255) >> 1),
					        .b = (uint8_t)((hue.b + 255) >> 1)};
				}
				else if (haveCalc && local != selDeg && degBright[local] > 0 && (y >= kRowTriad && y <= kRow7th + 1)) {
					c = RGB::monochrome((uint8_t)((uint32_t)pulse * degBright[local] / 255));
				}
				image[y][x] = c;
			}
		}
		else if (ci.region == REG_PAL_CTRL) {
			// Palette-bound controls: Calculator on/off, handedness swap. Reserved CRIMSON zone; clear pads below.
			for (int32_t y = 0; y < kDisplayHeight; y++) {
				RGB c = kCtrlHue.adjustFractional(kCtrlFaint, 255); // blank by default — only engaged controls light
				if (y == kBtnCalc) {
					c = kCtrlHue.adjustFractional(calc ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnSwap) {
					c = kCtrlHue.adjustFractional(swapped ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnPalOctUp || y == kBtnPalOctDown) {
					c = kCtrlHue.adjustFractional(kCtrlReady, 255); // momentary octave +/- — faint ember, findable
				}
				else if (y == kBtnStack) {
					c = kCtrlHue.adjustFractional(stackPick ? kCtrlOn : kCtrlOff, 255); // octave-picker mode
				}
				else if (y == kBtnSpread) {
					c = kCtrlHue.adjustFractional(voiceSpread ? (uint8_t)(60 + voiceSpread * 58) : kCtrlOff, 255);
				}
				else if (y == kBtnInversion) {
					c = kCtrlHue.adjustFractional(voiceInversion ? (uint8_t)(60 + voiceInversion * 58) : kCtrlOff, 255);
				}
				image[y][x] = c;
			}
		}
		else if (ci.region == REG_ISO_CTRL) {
			// Iso-bound controls, banded SEE (rows 7-4) → SHAPE (3-1) → HEAR (0), each its own purple shade.
			for (int32_t y = 0; y < kDisplayHeight; y++) {
				RGB grp = (y >= 4) ? kHueSee : (y >= 1) ? kHueShape : kHueHear;
				RGB c =
				    grp.adjustFractional(kCtrlFaint, 255); // blank by default; an engaged control glows its band hue
				if (y == kBtnIsoView) {
					c = grp.adjustFractional(chromatic ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnShowChord) {
					c = grp.adjustFractional(showChord ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnSticky) {
					c = grp.adjustFractional(sticky ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnLattice) {
					c = grp.adjustFractional(latticeOn ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnSnap) {
					// A SEE/navigation toggle — tint it with the SEE hue so it groups with the lens controls.
					c = kHueSee.adjustFractional(isoFollowsChord ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnProg) {
					// PROGRESSION = momentary hold-to-dial button; faint ember, brighter while held.
					c = grp.adjustFractional(progPadHeld ? kCtrlOn : kCtrlReady, 255);
				}
				else if (y == kBtnEdit) {
					c = grp.adjustFractional(editVoicing ? kCtrlOn : kCtrlOff, 255);
				}
				else if (y == kBtnAudition) {
					// AUDITION = momentary play button; faint ember when a chord is loaded (ready to strike).
					c = grp.adjustFractional(chordNoteCount > 0 ? kCtrlReady : kCtrlOff, 255);
				}
				image[y][x] = c;
			}
		}
		else if (ci.region == REG_ISO) {
			// The iso play surface, tinted to the key's mood colour; brightness carries the relationships:
			// chord CORE (root/3/5/7) brightest, extensions/repeats dimmer, steady tonic anchor, scale backdrop.
			int32_t local = ci.local;
			for (int32_t y = 0; y < kDisplayHeight; y++) {
				// OCTAVE PICKER strip on the bottom row: local 0..6 = octaves -3..+3, local 3 = base.
				if (stackPick && y == 0) {
					bool on = (voiceOctaves & (uint8_t)(1u << local)) != 0;
					uint8_t pb = (local == 3) ? 255 : (on ? 200 : 28);
					image[y][x] = chordHue.adjustFractional(pb, 255);
					continue;
				}
				int32_t note = chromatic ? isoNoteChromatic(local, y) : isoNoteAt(local, y);
				if (note < 0 || note > 127) {
					image[y][x] = RGB{}; // off the top/bottom of MIDI — no real note here, stay dark
					continue;
				}
				int32_t clamped = note;
				uint8_t pc = (uint8_t)(((clamped % 12) + 12) % 12);
				uint8_t within = (uint8_t)(((pc + kOctaveSize) - keyRoot) % kOctaveSize); // 0 = scale tonic
				bool inScale = getScaleNotes().has(within);
				bool playing = false;
				for (uint8_t i = 0; i < currentNotesState.count; i++) {
					if (currentNotesState.notes[i].note == note) {
						playing = true;
						break;
					}
				}
				uint8_t hi = getHighlightedNotes()[clamped];
				RGB key = kKeyColour[keyRoot % 12];
				RGB out;
				// RULE: WHITE is reserved for THE CHORD (voiced shape + its lattice). Everything else lives in
				// the key's COLOUR — the root pops as the brightest expression of that colour, never white.
				// ORDER MATTERS: the bright states (voicing, playing) win over the faint lattice backdrop, so a
				// note you PLAY still lights up even when the lattice is on (the lattice is only the dim canvas).
				if (diffOn && showChord && chordNoteCount > 0) {
					// VOICE-LEADING view: show the motion from the previous chord to this one.
					bool inPrev = (prevPcMask & (uint16_t)(1u << pc)) != 0;
					if (inChordExact(note)) {
						if (inPrev) {
							// COMMON tone -- the voice that HOLDS through the change: steady, bright anchor.
							out = chordHue.adjustFractional(primary[y][x] ? 235 : 150, 255);
						}
						else {
							// ARRIVING tone -- what moves IN: breathes bright to draw the eye.
							uint8_t ab = (uint8_t)((uint32_t)pulse * (primary[y][x] ? 255 : 200) / 255);
							out = chordHue.adjustFractional(ab, 255);
						}
					}
					else if (inPrev && !inChordPc(pc)) {
						// LEAVING tone -- what moves OUT, wearing the PREVIOUS chord's colour: a ghost breathing
						// OPPOSITE the arrivers, so the old chord's hue fades as the new chord's hue comes in.
						RGB prevHue = (prevSelDeg >= 0 && prevSelDeg < 7) ? kDegreeHue[prevSelDeg] : key;
						uint8_t anti = (uint8_t)(285 - pulse); // pulse runs 30..255; anti runs 255..30
						out = prevHue.adjustFractional((uint8_t)(anti / 4), 255);
					}
					else if (within == 0) {
						out = key.adjustFractional(150, 255); // keep the tonic anchor lit
					}
					else if (!chromatic || inScale) {
						out = key.adjustFractional(8, 255); // faint scale backdrop
					}
					else {
						out = RGB{};
					}
				}
				else if (showChord && inChordExact(note)) {
					// The voicing in the chord's palette colour: primary occurrence full-bright, repeats dimmer.
					// While SOUNDING (audition/play) every occurrence lights to full — the iso lights up so you
					// SEE what you hear.
					uint8_t cb = playing ? 255 : (primary[y][x] ? 255 : 110);
					out = chordHue.adjustFractional(cb, 255);
				}
				else if (playing) {
					out = key.adjustFractional(255, 255); // live free-play notes glow in the KEY colour
				}
				else if (showChord && bassSpotMidi >= 0 && note == bassSpotMidi) {
					// BASS SPOTLIGHT: the chord's bass (root, an octave below the voicing) — the low note to
					// play. Its OWN reserved colour, a warm amber/gold (NOT the chord hue), so it reads clearly
					// as "the bass" in the language. A clean octave under the shape. (Same colour the coming
					// diatonic bass row will wear.)
					out = RGB{.r = 255, .g = 170, .b = 20}.adjustFractional(255, 255);
				}
				else if (showChord && latticeOn && chordNoteCount > 0 && inChordPc(pc)) {
					// Chord-lattice overlay in the chord's PALETTE colour — a glow of every position the chord
					// makes available, fading upward. Kept below the voicing (110+) but clearly ABOVE the scale
					// backdrop (8) so it reads as the lattice, not as the dim grid.
					uint8_t base = isCorePc(pc) ? 95 : 62;
					uint8_t fade = (uint8_t)((uint32_t)base * (uint32_t)(kDisplayHeight - y) / kDisplayHeight);
					if (fade < 20) {
						fade = 20; // floor must clear the scale backdrop so the lattice is always visible
					}
					out = chordHue.adjustFractional(fade, 255);
				}
				else if (hi != 0
				         && (hi >= 254
				             || runtimeFeatureSettings.get(RuntimeFeatureSettingType::HighlightIncomingNotes)
				                    == RuntimeFeatureStateToggle::On)) {
					out = key.adjustFractional((hi >= 254) ? (hi == 255 ? 235 : 120) : 65, 255);
				}
				else if (within == 0) {
					// ROOT/tonic: the BRIGHTEST expression of the key's own colour — pops in-hue, never white.
					out = key.adjustFractional(150, 255);
				}
				else if (!chromatic || inScale) {
					out = key.adjustFractional(8, 255); // faint scale backdrop (dim, to let the root pop)
				}
				else {
					out = RGB{}; // chromatic off-scale: dark
				}
				image[y][x] = out;
			}
		}
		else {
			for (int32_t y = 0; y < kDisplayHeight; y++) {
				image[y][x] = RGB{};
			}
		}
	}

	// LIVE HELD-PAD GLOW: paint a white overlay on every display pad under a finger right now, so the user (and
	// the Companion mirror, which reads this same buffer) sees exactly what's pressed. Drawn last so it sits on
	// top of the chord shape. Blended ~80% toward white — clearly "pressed" but keeps a hint of the pad's hue.
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		uint16_t row = heldPadRows[y];
		if (row == 0) {
			continue;
		}
		for (int32_t x = 0; x < kDisplayWidth; x++) {
			if (row & (uint16_t)(1u << x)) {
				RGB b = image[y][x];
				image[y][x] = RGB{.r = (uint8_t)(b.r + (((255 - b.r) * 205) >> 8)),
				                  .g = (uint8_t)(b.g + (((255 - b.g) * 205) >> 8)),
				                  .b = (uint8_t)(b.b + (((255 - b.b) * 205) >> 8))};
			}
		}
	}
}

bool KeyboardLayoutHarmonic::allowSidebarType(ColumnControlFunction sidebarType) {
	if (sidebarType == ColumnControlFunction::CHORD) {
		return false;
	}
	return true;
}

} // namespace deluge::gui::ui::keyboard::layout
