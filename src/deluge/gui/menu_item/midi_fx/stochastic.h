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
#include "gui/menu_item/midi_fx/spice.h"
#include "gui/menu_item/midi_fx/target.h"
#include "gui/ui/sound_editor.h"
#include "model/clip/instrument_clip.h"
#include "model/song/song.h"
#include "modulation/midi_fx/stochastic.h"

/// NOTE: there is also a `deluge::modulation::midi_fx` (the effects themselves). Any file with
/// using-directives for both `deluge::modulation` and `deluge::gui::menu_item` - `menus.cpp` is
/// one - must qualify as `menu_item::midi_fx::` or the name is ambiguous.
namespace deluge::gui::menu_item::midi_fx {

/// The Stochastic page: Amount, Width and Dice.
///
/// **Clip-wide, and hidden on a Kit**, for the two reasons Spread's page is. Clip-wide because
/// Amount and Width read and write the InstrumentClip directly rather than going through
/// `midiFxApply`, which on a Kit routes every write to the *Drums* and would leave a clip-level
/// setting that could never be switched on. Hidden on a Kit because a kit row is a drum rather than
/// a pitch, so a wandering note code picks a different drum instead of a different note - which is
/// a real feature and a separate one, not this one misfiring.
[[nodiscard]] inline bool stochasticEditable() {
	InstrumentClip* clip = getCurrentInstrumentClip();
	return midiFxSwitchedOn() && clip != nullptr && clip->output != nullptr && clip->output->type != OutputType::KIT;
}

/// Amount: how many of the written notes get their pitch redrawn, in percent.
///
/// **How often, not how far** - Width below is how far. Splitting them is what makes the low end of
/// this knob useful: at 15 you hear your part, played as you wrote it, with the occasional note
/// somewhere else, which is the sound people are actually after. A single knob that widened the
/// pool instead would give a constant shimmer at every setting and could never produce "mostly as
/// written".
///
/// At 100 every note moves, and that is exact rather than approximate: the degree draw excludes
/// zero, so a note that fires always goes somewhere. See stochastic.h.
class StochasticAmount final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->stochasticAmount : deluge::modulation::midi_fx::stochastic::kOff);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->stochasticAmount = static_cast<uint8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::stochastic::kOff; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::stochastic::kAmountMax; }
	const char* getUnit() override { return "%"; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return stochasticEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Width: how far a redrawn note may move, in degrees of the song's key.
///
/// **Degrees rather than semitones, so the result is in key by construction.** A version that moved
/// by semitones and then snapped to the scale would be in key only after correction, and would
/// bunch onto whichever pitches the snap happened to favour. Moving by degree also means the wander
/// follows the song when its scale changes, with nothing to re-dial.
///
/// Starts at 1, not 0: "no width" is what Amount 0 already says, and a control with two ways to
/// spell off is a control people turn to 0 and then report as broken. Tops out at 7 - an octave of
/// a seven-note scale - because past that the contour of what you wrote stops being audible under
/// the wander, and a phrase nobody can hear the shape of is not a variation on it.
class StochasticWidth final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->stochasticWidth
		                               : deluge::modulation::midi_fx::stochastic::kWidthDefault);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->stochasticWidth = static_cast<uint8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::stochastic::kWidthMin; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::stochastic::kWidthMax; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return stochasticEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Dice, again - the same number the Spice page edits, shown here too.
///
/// **One control appearing on two pages, not two controls to keep in agreement.** Spice varies the
/// gates of a phrase and this varies its pitches; they are two halves of one idea, and "which
/// variation am I hearing" should be one number for both. Turn it on either page and the whole
/// take changes together, which is what a user of Spice already expects the number to mean.
///
/// A second class rather than a second instance of Spice's `Dice`, which is `final`, and rather
/// than un-`final`ing it: this page hides itself on a Kit and that one does not, so the only thing
/// that actually differs is `isRelevant`. The stored field, the range and the index-not-a-seed
/// behaviour are all the same field read the same way, so the two stay in step by construction -
/// there is only one number.
class StochasticDice final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		const auto* s = midiFxSettingsForRead();
		this->setValue(s != nullptr ? s->spiceDice : deluge::modulation::midi_fx::spice::kDiceMin);
	}

	void writeCurrentValue() override {
		const auto dice = static_cast<uint8_t>(this->getValue());
		midiFxApply([dice](deluge::modulation::midi_fx::MidiFxSettings& s) { s.spiceDice = dice; });
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::spice::kDiceMin; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::spice::kDiceMax; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return stochasticEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

} // namespace deluge::gui::menu_item::midi_fx
