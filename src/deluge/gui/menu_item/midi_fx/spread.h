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
#include "gui/ui/sound_editor.h"
#include "model/clip/instrument_clip.h"
#include "model/song/song.h"
#include "modulation/midi_fx/spread.h"

/// NOTE: there is also a `deluge::modulation::midi_fx` (the effects themselves). Any file with
/// using-directives for both `deluge::modulation` and `deluge::gui::menu_item` - `menus.cpp` is
/// one - must qualify as `menu_item::midi_fx::` or the name is ambiguous.
namespace deluge::gui::menu_item::midi_fx {

/// The Spread page: Velocity, Gate and Octave.
///
/// **Clip-wide, and hidden on a Kit.** Clip-wide for the reason Strum and Play FX are: these read
/// and write the InstrumentClip directly rather than going through `midiFxApply`, which on a Kit
/// routes every write to the *Drums* and would leave a clip-level setting that could never be
/// switched on.
///
/// Hidden on a Kit because both of the things it offers are already answered there. A kit row's
/// velocity is randomisable through the mod matrix by patching master volume to Random, and an
/// octave means nothing on a row that is a drum rather than a pitch. A page that showed three
/// controls of which one duplicates the matrix and two do nothing is worse than no page.
/// SPREAD-FEASIBILITY.md §11.
[[nodiscard]] inline bool spreadEditable() {
	InstrumentClip* clip = getCurrentInstrumentClip();
	return midiFxSwitchedOn() && clip != nullptr && clip->output != nullptr && clip->output->type != OutputType::KIT;
}

/// Velocity: how far either side of the written velocity a note may land, in velocity steps.
///
/// Absolute rather than proportional, which is the useful choice on a part that already has
/// written accents: at 10 the quiet ghost notes and the loud accents both wander by up to ten,
/// rather than the accents wandering four times as far and swamping the shape you wrote.
class SpreadVelocity final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->spreadVelocity : deluge::modulation::midi_fx::spread::kOff);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->spreadVelocity = static_cast<uint8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::spread::kOff; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::spread::kVelocityMax; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return spreadEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Gate: how far either side of its written length a note may run, as a percentage of that length.
///
/// Both ways, unlike Spice's gate, which only ever shortens. Lengthening is safe here because the
/// sequencer clamps it against the next note of the row before scheduling the note-off - see the
/// note at the hook in NoteRow::processCurrentPos, which explains what an unclamped one would cost.
///
/// Tops out at 90 rather than 100 so the bottom of the knob stays useful: a note that could reach
/// 0% of its length is a note that can vanish, and the floor of one tick would be doing all the
/// work down there rather than the knob.
class SpreadGate final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->spreadGate : deluge::modulation::midi_fx::spread::kOff);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->spreadGate = static_cast<uint8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::spread::kOff; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::spread::kGateMax; }
	const char* getUnit() override { return "%"; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return spreadEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// Octave: how often a note is thrown an octave, up or down, in percent.
///
/// **A probability, where the two above it are spreads, and the difference is the point.** Velocity
/// and gate are continuous, so "a bit more, a bit less" means something and every note can differ
/// slightly. An octave is quantised and coarse: there is no small amount of one, the note is
/// displaced or it is not - so the only honest knob is how often. For four-to-the-floor material
/// that is also the knob you want, because the repetition is the part you are keeping. Note that
/// the arpeggiator's Randomizer page spells its own Spread Octave the other way, as a range.
///
/// Both directions, and the edges need no special handling: a displacement that would leave the
/// note range is declined rather than clamped, so near the bottom of a bassline the down-jumps
/// simply do not happen instead of piling onto the lowest note.
class SpreadOctave final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(clip != nullptr ? clip->spreadOctave : deluge::modulation::midi_fx::spread::kOff);
	}

	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr) {
			return;
		}
		playFxTempHoldNoteClip(clip);
		clip->spreadOctave = static_cast<uint8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return deluge::modulation::midi_fx::spread::kOff; }
	[[nodiscard]] int32_t getMaxValue() const override { return deluge::modulation::midi_fx::spread::kOctaveMax; }
	const char* getUnit() override { return "%"; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return spreadEditable(); }

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

} // namespace deluge::gui::menu_item::midi_fx
