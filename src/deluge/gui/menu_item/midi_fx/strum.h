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
#include "gui/menu_item/midi_fx/play_fx.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "model/clip/instrument_clip.h"
#include "modulation/midi_fx/strum.h"

/// NOTE: there is also a `deluge::modulation::midi_fx` (the effects themselves). Any file with
/// using-directives for both `deluge::modulation` and `deluge::gui::menu_item` - `menus.cpp` is
/// one - must qualify as `menu_item::midi_fx::` or the name is ambiguous.
namespace deluge::gui::menu_item::midi_fx {

/// The Strum page: Spread, Direction and Contour.
///
/// **Clip-wide, and on a Kit too.** A strum is a property of the group of notes hitting together,
/// so two drums of one kit holding different amounts is not a coherent question - see
/// STRUM-FEASIBILITY.md §7. That makes these three read and write the Clip directly, the way Play
/// FX does, rather than going through `midiFxApply`: on a Kit that helper routes every write to the
/// *Drums*, which is right for a repeat and would be wrong here.
///
/// The practical consequence is the same one Play FX has, and it is the one you want: the page
/// stays visible and editable on a Kit with no drum selected, and AFFECT ENTIRE means nothing here
/// because there is only ever one value.
[[nodiscard]] inline bool strumEditable() {
	return midiFxSwitchedOn() && getCurrentInstrumentClip() != nullptr;
}

/// Spread: ticks between successive notes of the chord. 0 is off.
///
/// Tempo-relative, and worth being plain about: there are 48 ticks to a quarter note, so one tick
/// is about 10 ms at 120 bpm and the strum widens as the tempo drops. Real strums are physical and
/// fixed, but most gear that offers this is tempo-relative anyway and going absolute would mean
/// carrying a second timing frame through the sequencer for very little. §6.
class StrumAmount final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->strumAmount : deluge::modulation::midi_fx::strum::kAmountOff);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->strumAmount = static_cast<uint8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::strum::kAmountOff; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::strum::kAmountMax; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return strumEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Direction: which end of the chord sounds first.
///
/// Three in phase 1. Alternate - up, then down on the next chord - and Inward/Outward each need a
/// scrap of state that survives between chords, which is a different shape from everything here,
/// where the answer is a function of the chord in hand.
class StrumDirection final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->strumDirection : 0);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->strumDirection = static_cast<uint8_t>(this->getValue());
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		// The order here IS strum::Direction's order, and the stored value is the index - so these
		// three lines are the file format as much as the menu. Reordering them silently rewrites
		// every saved song's direction.
		return {
		    l10n::getView(STRING_FOR_MIDI_FX_STRUM_UP),     //<
		    l10n::getView(STRING_FOR_MIDI_FX_STRUM_DOWN),   //<
		    l10n::getView(STRING_FOR_MIDI_FX_STRUM_RANDOM), //<
		};
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return strumEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	bool shouldEnterSubmenu() override { return false; }
};

/// Contour: how much the velocity has changed by the last note of the sweep, in percent.
///
/// The first note keeps its written velocity and the rest are interpolated to this, so a three-note
/// chord and a six-note chord finish in the same place. Negative is a hand losing force through the
/// stroke, positive is one gaining it - and this is the single thing that separates "spread out in
/// time" from "sounds strummed".
class StrumContour final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->strumContour : deluge::modulation::midi_fx::strum::kContourOff);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->strumContour = static_cast<int8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::strum::kContourMin; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::strum::kContourMax; }
	const char* getUnit() override { return "%"; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return strumEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

} // namespace deluge::gui::menu_item::midi_fx
