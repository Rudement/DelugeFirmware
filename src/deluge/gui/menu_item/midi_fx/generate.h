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
#include "gui/menu_item/integer.h"
#include "gui/menu_item/menu_item.h"
#include "gui/menu_item/midi_fx/play_fx.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/instrument_clip_view.h"
#include "hid/display/display.h"
#include "model/clip/instrument_clip.h"
#include "modulation/midi_fx/generate.h"

/// NOTE: there is also a `deluge::modulation::midi_fx` (the effects themselves). Any file with
/// using-directives for both `deluge::modulation` and `deluge::gui::menu_item` - `menus.cpp` is
/// one - must qualify as `menu_item::midi_fx::` or the name is ambiguous.
namespace deluge::gui::menu_item::midi_fx {

/// The Generate page: Density, Seed, Gate and the command itself.
///
/// **Clip-wide, and on a Kit too**, so these read and write the Clip directly the way Strum and Play
/// FX do rather than going through `midiFxApply` - on a Kit that helper routes every write to the
/// *Drums*, which is right for a repeat and wrong for a command that writes into one row. It is also
/// what keeps the page editable on a Kit with no drum selected.
///
/// The other half of the reason is a hard one: `MidiFxSettings` is stored per clip *and* per drum and
/// is already at the 16-byte ceiling its own test asserts. See the note on the fields in
/// model/clip/instrument_clip.h.
[[nodiscard]] inline bool generateEditable() {
	return midiFxSwitchedOn() && getCurrentInstrumentClip() != nullptr;
}

/// Density: how likely each step is to carry a note.
///
/// 0 does not clear the row - the command refuses instead. Emptying a row because a knob was left at
/// the bottom is not a thing a generator should do to you.
class GenerateDensity final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->generateDensity
		                               : deluge::modulation::midi_fx::generate::kDensityDefault);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->generateDensity = static_cast<uint8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::generate::kDensityMin; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::generate::kDensityMax; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return generateEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Range: how far above the auditioned row the line may climb, in scale degrees.
///
/// **0 is a rhythm at one pitch - the whole of what this command did before it could write a line.**
/// That is the default, so nothing about an existing habit changes until this is turned up, and a
/// Kit ignores it entirely: there a row is a drum, and a line climbing through rows would walk
/// across the kit rather than up a scale.
///
/// Above 0 the generator writes across rows, creating the ones it needs, in degrees of the song's
/// key - so the line is in key by construction rather than by correction, and it follows the song if
/// the scale changes later.
class GenerateRange final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->generateRange : deluge::modulation::midi_fx::generate::kRangeDefault);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->generateRange = static_cast<uint8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::generate::kRangeMin; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::generate::kRangeMax; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return generateEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Leap: how often the line jumps rather than stepping to a neighbouring degree.
///
/// Of everything on this page it is the control that most decides whether the result sounds like a
/// melody or like noise: near 0 it is a scale run, near 100 it is scattered arpeggio, and the middle
/// is where tunes live. Inert at Range 0, where there is nowhere for the line to move.
class GenerateLeap final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->generateLeap : deluge::modulation::midi_fx::generate::kLeapDefault);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->generateLeap = static_cast<uint8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::generate::kLeapMin; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::generate::kLeapMax; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return generateEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Seed: which pattern.
///
/// **An index, not a drawn seed** - the same idea as Spice's Dice, and the thing that makes this a
/// generator rather than a slot machine. Seed 7 is always the same pattern, so one you liked two
/// turns ago is two turns back rather than gone. Nothing is stored per note; the pattern comes out
/// of the hash every time it is asked for.
class GenerateSeed final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->generateSeed : deluge::modulation::midi_fx::generate::kSeedMin);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->generateSeed = static_cast<uint8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::generate::kSeedMin; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::generate::kSeedMax; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return generateEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Gate: generated note length, as a percentage of one step.
///
/// Over 100 the note reaches into the next step and is clamped at write time to stop where the next
/// note starts - so turning it up makes a sparse pattern legato and leaves a dense one alone, which
/// is the behaviour you want out of one knob.
class GenerateGate final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->generateGatePercent
		                               : deluge::modulation::midi_fx::generate::kGateDefault);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->generateGatePercent = static_cast<uint16_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::generate::kGateMin; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::generate::kGateMax; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return generateEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// The command itself.
///
/// An action rather than a value, the same shape as ResetAll and for the same reason: a Selection
/// sitting in a vertical submenu has nothing to turn, so it would do nothing at all on hardware.
///
/// It writes into the row you last auditioned in the clip view, which is where Euclid writes too.
/// Everything that can stop it - no clip, the menu reached from somewhere other than the clip view,
/// no row under that index, Density at zero - comes back as false and says so, rather than editing
/// something arbitrary.
class GenerateGo final : public MenuItem {
public:
	using MenuItem::MenuItem;

	MenuItem* selectButtonPress() override {
		if (!instrumentClipView.commandGenerateNotes()) {
			display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_MIDI_FX_GENERATE_NO_ROW));
		}
		// No popup on success: only the clip view knows how many notes landed, so it shows that one.
		return NO_NAVIGATION;
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return generateEditable(); }

	bool shouldEnterSubmenu() override { return false; }
};

} // namespace deluge::gui::menu_item::midi_fx
