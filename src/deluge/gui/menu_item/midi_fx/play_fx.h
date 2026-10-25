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
#include "gui/menu_item/midi_fx/ratio_step.h"
#include "gui/menu_item/midi_fx/switched_on.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "model/clip/instrument_clip.h"
#include "model/clock_division.h"
#include "model/instrument/kit.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "modulation/temp_hold.h"

/// The Play FX page: Clock Shift and Beat Stretch.
///
/// These two sit in the MIDI FX page group but are not MIDI FX, and the menu says so by putting
/// them on a page of their own with their own name. They act on the phrase rather than on note
/// events, and they are **clip-wide** - which is why they read the Clip directly rather than
/// `soundEditor.currentMidiFxSettings`, whose Kit copies are per-drum.
///
/// The practical consequence, and it is the right one: on a Kit these two stay visible with no
/// drum selected, while Velocity, Gate, Repeat and Delay hide themselves.
namespace deluge::gui::menu_item::midi_fx {

/// Where a Play FX value lives depends on what is being edited, and there are three answers.
///
///  - A melodic clip: on the Clip. A row there is a pitch, and per-pitch Clock Shift would be a
///    different and much stranger feature.
///  - A Kit with a drum selected: on that drum's row. A row there is a drum, and pushing the hats
///    late against the kick is the thing people actually want.
///  - A Kit with AFFECT ENTIRE held: every row at once. That is the kit-wide gesture the Deluge
///    already uses everywhere else, so there is no separate kit-wide value to keep in step.
///
/// With a Kit and no drum selected and no AFFECT ENTIRE there is nothing to edit, and the items
/// hide themselves rather than writing somewhere arbitrary.

[[nodiscard]] inline NoteRow* playFxSelectedRow() {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr || clip->output == nullptr || clip->output->type != OutputType::KIT) {
		return nullptr;
	}
	Kit* kit = getCurrentKit();
	if (kit == nullptr || kit->selectedDrum == nullptr) {
		return nullptr;
	}
	return clip->getNoteRowForDrum(kit->selectedDrum);
}

[[nodiscard]] inline bool playFxIsKit() {
	InstrumentClip* clip = getCurrentInstrumentClip();
	return clip != nullptr && clip->output != nullptr && clip->output->type == OutputType::KIT;
}

[[nodiscard]] inline bool playFxEditable() {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr || !midiFxSwitchedOn()) {
		return false;
	}
	if (!playFxIsKit()) {
		return true;
	}
	return clip->affectEntire || playFxSelectedRow() != nullptr;
}

/// Reads from the selected drum's row, or the first row under AFFECT ENTIRE, or the Clip.
template <typename Fn>
[[nodiscard]] inline int32_t playFxRead(Fn&& fromRow, int32_t fromClip, int32_t ifNothing) {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr) {
		return ifNothing;
	}
	if (!playFxIsKit()) {
		return fromClip;
	}
	if (NoteRow* row = playFxSelectedRow(); row != nullptr) {
		return fromRow(row);
	}
	if (clip->affectEntire && clip->noteRows.getNumElements() > 0) {
		// Under AFFECT ENTIRE with no drum selected, the first row stands in for the kit. They are
		// written together, so after the first edit they all agree anyway.
		return fromRow(clip->noteRows.getElement(0));
	}
	return ifNothing;
}

/// Writes to the selected drum's row, or to every row under AFFECT ENTIRE.
///
/// Returns true when the value belongs on the Clip instead - a melodic clip - which the caller
/// does itself, because only it knows which of the two fields it is writing.
/// Records a Play FX owner with Temp Hold before it is written.
inline void playFxTempHoldNoteRow(NoteRow* row) {
	if (deluge::modulation::temp_hold::TempHold::shouldCapture()) {
		deluge::modulation::temp_hold::tempHold.beginIfNeeded();
		deluge::modulation::temp_hold::tempHold.notePlayFxRow(row);
	}
}

inline void playFxTempHoldNoteClip(InstrumentClip* clip) {
	if (deluge::modulation::temp_hold::TempHold::shouldCapture()) {
		deluge::modulation::temp_hold::tempHold.beginIfNeeded();
		deluge::modulation::temp_hold::tempHold.notePlayFxClip(clip);
	}
}

template <typename Fn>
inline bool playFxWriteRows(Fn&& toRow) {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr) {
		return false;
	}
	// The clip is recorded whichever way this goes. On a melodic clip the caller writes the clip
	// fields as soon as this returns true, and on a Kit the entry is a few spare bytes that restore
	// to the values they already had - which is cheaper than a second hook in every caller.
	playFxTempHoldNoteClip(clip);
	if (!playFxIsKit()) {
		return true;
	}
	if (clip->affectEntire) {
		for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
			playFxTempHoldNoteRow(clip->noteRows.getElement(i));
			toRow(clip->noteRows.getElement(i));
		}
		return false;
	}
	if (NoteRow* row = playFxSelectedRow(); row != nullptr) {
		playFxTempHoldNoteRow(row);
		toRow(row);
	}
	return false;
}

/// Ticks the phrase plays early or late. +/-192 is a bar either way at the default resolution.
class ClockShift final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		this->setValue(playFxRead([](NoteRow* row) { return static_cast<int32_t>(row->playFxClockShift); },
		                          clip != nullptr ? clip->playFxClockShift : 0, 0));
	}
	void writeCurrentValue() override {
		const auto value = static_cast<int16_t>(this->getValue());
		if (playFxWriteRows([value](NoteRow* row) { row->playFxClockShift = value; })) {
			getCurrentInstrumentClip()->playFxClockShift = value;
		}
	}
	[[nodiscard]] int32_t getMinValue() const override { return -192; }
	[[nodiscard]] int32_t getMaxValue() const override { return 192; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return playFxEditable(); }
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return NUMBER; }
};

/// The rate the phrase plays at against the clip's length - the fine trim beside Division.
///
/// Faster than neutral and the phrase repeats inside the loop; slower and its tail is truncated
/// at the loop point. Same transform as Division, and the two multiply.
///
/// A Selection rather than an Integer, purely so the readout can be a fraction where one is
/// exact - "/2" and "4/3" cannot come out of a number plus a unit. The stored value is unchanged:
/// still the same percentage in the same field, still the same 49 semitone-ratio steps. Only what
/// the screen says changed. See kRatioStepLabels in ratio_step.h for which positions read as
/// fractions and why the rest do not.
class BeatStretch final : public Selection {
public:
	using Selection::Selection;
	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		const int32_t percent = playFxRead([](NoteRow* row) { return static_cast<int32_t>(row->playFxBeatStretch); },
		                                   clip != nullptr ? clip->playFxBeatStretch : InstrumentClip::kBeatStretchOff,
		                                   InstrumentClip::kBeatStretchOff);
		this->setValue(ratioStepToSelection(percentToRatioStep(percent)));
	}
	void writeCurrentValue() override {
		const auto value = static_cast<uint16_t>(ratioStepToPercent(selectionToRatioStep(this->getValue())));
		if (playFxWriteRows([value](NoteRow* row) { row->playFxBeatStretch = value; })) {
			getCurrentInstrumentClip()->playFxBeatStretch = value;
		}
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return playFxEditable(); }
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	/// One list for both displays: every label is four characters or fewer, so there is nothing
	/// for a SHORT form to shorten.
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		deluge::vector<std::string_view> options;
		options.reserve(kNumRatioStepLabels);
		for (int32_t i = 0; i < kNumRatioStepLabels; i++) {
			options.emplace_back(kRatioStepLabels[i]);
		}
		return options;
	}
};

/// The rate this row's phrase plays at, as an exact musical division.
///
/// Beat Stretch is the same transform and can reach most of these, but it stores a percentage and
/// an integer percentage cannot say x3 - 33% is x3.0303, and the drift arrives at the ear as a
/// fault rather than as a rounding. So a division is stored as a ratio and applied as one. See
/// model/clock_division.h.
///
/// The two compose rather than override: the division is the musical step, Beat Stretch the fine
/// trim on top of it. Neutral on both costs exactly what it costs today.
///
/// On a Kit this lands on the selected drum's row, or on every row under AFFECT ENTIRE, by the
/// same three-way rule as the two items above - which is what makes "clock divisions per kit row"
/// a thing you can actually play.
class Division final : public Selection {
public:
	using Selection::Selection;
	void readCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		const int32_t division = playFxRead([](NoteRow* row) { return static_cast<int32_t>(row->playFxDivision); },
		                                    clip != nullptr ? clip->playFxDivision : 0, 0);
		this->setValue(deluge::model::clock_division::stepForDivision(division));
	}
	void writeCurrentValue() override {
		const int8_t value = deluge::model::clock_division::divisionForStep(this->getValue());
		if (playFxWriteRows([value](NoteRow* row) { row->playFxDivision = value; })) {
			getCurrentInstrumentClip()->playFxDivision = value;
		}
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return playFxEditable(); }
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	/// Not l10n strings. These are symbols, not words - there is nothing in "/2" to translate, and
	/// eleven entries in five language files to keep in step would be five ways to get it wrong.
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		if (optType == OptType::SHORT) {
			// 7SEG has four characters and no slash worth looking at, so divisions read "d2".
			return {"X8", "X6", "X4", "X3", "X2", "1", "d2", "d3", "d4", "d6", "d8"};
		}
		return {"x8", "x6", "x4", "x3", "x2", "1", "/2", "/3", "/4", "/6", "/8"};
	}
};

} // namespace deluge::gui::menu_item::midi_fx
