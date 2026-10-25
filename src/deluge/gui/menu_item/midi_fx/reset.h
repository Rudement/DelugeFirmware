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

#include "gui/menu_item/menu_item.h"
#include "gui/menu_item/midi_fx/groove.h"
#include "gui/menu_item/midi_fx/loop_remix.h"
#include "gui/menu_item/midi_fx/play_fx.h"
#include "gui/menu_item/midi_fx/spread.h"
#include "gui/menu_item/midi_fx/stochastic.h"
#include "gui/menu_item/midi_fx/strum.h"
#include "gui/menu_item/midi_fx/target.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "model/clip/instrument_clip.h"
#include "model/clock_division.h"
#include "model/song/song.h"
#include "modulation/midi_fx/midi_fx_settings.h"

/// Reset, one per effect page.
///
/// An effect with four controls has a lot of places to end up, and no way back. This is the way
/// back, and it does a second job on the way: it *reads* as well as writes. The value shows
/// `Default` when that effect is untouched and `Edited` when it is not, so a glance across the
/// pages says which effects are actually doing something - which is the harder half of the
/// problem when a patch does not sound the way you expect.
///
/// Turning it to `Default` performs the reset. Turning it to `Edited` does nothing and springs
/// back, because "un-reset" is not a thing this can honestly offer - undo is.
///
/// The defaults themselves come from a default-constructed MidiFxSettings rather than from
/// numbers written out again here. Anything else drifts: the day someone changes a default in
/// the settings header, this would quietly keep resetting to the old one.
namespace deluge::gui::menu_item::midi_fx {

enum class ResetTarget : uint8_t {
	VELOCITY,
	GATE,
	RETRIG,
	DELAY,
	/// Spice amount and Dice. Dice is included even though it is not saved with the song: on this
	/// page it is a control like any other, and a Reset that put the amount back but left you on
	/// variation 63 would not have got you back.
	SPICE,
	/// Clock Shift and Beat Stretch, which live on the Clip rather than in MidiFxSettings.
	PLAY_FX,
	/// Time, Direction and Contour, which live on the Clip for the same reason Play FX does.
	STRUM,
	/// Velocity, Gate and Octave, which live on the Clip for the same reason again.
	SPREAD,
	/// Amount, Width, and Dice - which the page shows and which therefore resets with it, exactly as
	/// it does on the Spice page. It is one shared number, so this does roll the gates back to
	/// variation 0 as well; that is the price of sharing it, and a Reset that visibly left a column
	/// untouched reads as broken, which is worse.
	STOCHASTIC,
	/// Family, Type, Variation and Slice, which live on the Clip like Play FX's do.
	LOOP_REMIX,
	/// Template, Accent, Time, Random and Offset - the Clip's, or every drum's on a Kit, as Loop Remix.
	GROOVE,
	/// Every one of the above, for the entry that hangs off the MIDI FX submenu rather than a page.
	ALL,
};

/// Whether everything the target covers is sitting at its default.
[[nodiscard]] inline bool midiFxIsAtDefault(ResetTarget target) {
	const auto covers = [target](ResetTarget t) { return target == t || target == ResetTarget::ALL; };
	const deluge::modulation::midi_fx::MidiFxSettings defaults;
	const auto* s = midiFxSettingsForRead();
	// Not const: reading the rows out of the vector is a non-const call on the container.
	InstrumentClip* clip = getCurrentInstrumentClip();

	if (s != nullptr) {
		if (covers(ResetTarget::VELOCITY) && s->velocityOffset != defaults.velocityOffset) {
			return false;
		}
		if (covers(ResetTarget::GATE) && s->gateTimePercent != defaults.gateTimePercent) {
			return false;
		}
		if (covers(ResetTarget::RETRIG)
		    && (s->retrigRepeats != defaults.retrigRepeats || s->retrigSyncLevel != defaults.retrigSyncLevel
		        || s->retrigSyncType != defaults.retrigSyncType
		        || s->retrigDecayPercent != defaults.retrigDecayPercent)) {
			return false;
		}
		if (covers(ResetTarget::DELAY)
		    && (s->delayRepeats != defaults.delayRepeats || s->delaySyncLevel != defaults.delaySyncLevel
		        || s->delaySyncType != defaults.delaySyncType || s->delayLevelPercent != defaults.delayLevelPercent
		        || s->delayFbVelocity != defaults.delayFbVelocity || s->delayPitchShift != defaults.delayPitchShift
		        || s->delayNoteRandom != defaults.delayNoteRandom
		        || s->delayFbClockPercent != defaults.delayFbClockPercent)) {
			return false;
		}
		if (covers(ResetTarget::SPICE)
		    && (s->spiceAmount != defaults.spiceAmount || s->spiceDice != defaults.spiceDice)) {
			return false;
		}
	}
	if (covers(ResetTarget::STRUM) && clip != nullptr) {
		// Direction and Contour are checked as well as Spread, even though Spread at 0 already
		// silences the effect: this control's job is to say whether the page has been touched, and
		// a page showing Down and -40% has been.
		if (clip->strumAmount != deluge::modulation::midi_fx::strum::kAmountOff || clip->strumDirection != 0
		    || clip->strumContour != deluge::modulation::midi_fx::strum::kContourOff) {
			return false;
		}
	}
	if (covers(ResetTarget::SPREAD) && clip != nullptr) {
		namespace sp = deluge::modulation::midi_fx::spread;
		if (clip->spreadVelocity != sp::kOff || clip->spreadGate != sp::kOff || clip->spreadOctave != sp::kOff) {
			return false;
		}
	}
	if (covers(ResetTarget::STOCHASTIC) && clip != nullptr) {
		namespace so = deluge::modulation::midi_fx::stochastic;
		if (clip->stochasticAmount != so::kOff || clip->stochasticWidth != so::kWidthDefault) {
			return false;
		}
		// Dice as well, for the same reason the Spice page checks it: this control's job is to say
		// whether the page has been touched, and a page sitting on variation 63 has been.
		if (s != nullptr && s->spiceDice != defaults.spiceDice) {
			return false;
		}
	}
	if (covers(ResetTarget::LOOP_REMIX) && clip != nullptr) {
		if (!clip->loopRemix.isDefault()) {
			return false;
		}
		// Every row, as Play FX does below: editing is per-drum on a Kit, but a control whose job is
		// "get me back" has to answer for the whole kit.
		for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
			if (!clip->noteRows.getElement(i)->loopRemix.isDefault()) {
				return false;
			}
		}
	}
	if (covers(ResetTarget::GROOVE) && clip != nullptr) {
		if (!clip->groove.isDefault()) {
			return false;
		}
		for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
			if (!clip->noteRows.getElement(i)->groove.isDefault()) {
				return false;
			}
		}
	}
	if (covers(ResetTarget::PLAY_FX) && clip != nullptr) {
		if (clip->playFxClockShift != 0 || clip->playFxBeatStretch != InstrumentClip::kBeatStretchOff
		    || !deluge::model::clock_division::isOff(clip->playFxDivision)) {
			return false;
		}
		// Every row, not just the selected drum. Editing is per-drum on a Kit, but a control whose
		// job is "get me back" has to answer for the whole kit - otherwise it reads Default while
		// three other drums are still shifted, which is worse than not having it.
		for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
			const NoteRow* row = clip->noteRows.getElement(i);
			if (row->playFxClockShift != 0 || row->playFxBeatStretch != InstrumentClip::kBeatStretchOff
			    || !deluge::model::clock_division::isOff(row->playFxDivision)) {
				return false;
			}
		}
	}
	return true;
}

/// Puts everything the target covers back to its default.
///
/// Deliberately leaves `enabled` alone. That is the master switch, not a setting of any one effect,
/// and resetting a page should not silently switch the whole feature off underneath the other four.
///
/// Also deliberately does not flush. A delay tail already ringing keeps its own repeat count and
/// fades out normally, which is what happens when you turn Repeats to zero by hand. Cutting it here
/// would make Reset the one control that stops sound, and that is not what someone reaching for it
/// expects.
inline void midiFxApplyDefaults(ResetTarget target) {
	const auto covers = [target](ResetTarget t) { return target == t || target == ResetTarget::ALL; };
	const deluge::modulation::midi_fx::MidiFxSettings defaults;
	InstrumentClip* clip = getCurrentInstrumentClip();

	// Through midiFxApply, so a reset reaches exactly what an edit would: one drum, or the whole
	// kit under AFFECT ENTIRE.
	midiFxApply([&covers, &defaults](deluge::modulation::midi_fx::MidiFxSettings& s) {
		if (covers(ResetTarget::VELOCITY)) {
			s.velocityOffset = defaults.velocityOffset;
		}
		if (covers(ResetTarget::GATE)) {
			s.gateTimePercent = defaults.gateTimePercent;
		}
		if (covers(ResetTarget::RETRIG)) {
			s.retrigRepeats = defaults.retrigRepeats;
			s.retrigSyncLevel = defaults.retrigSyncLevel;
			s.retrigSyncType = defaults.retrigSyncType;
			s.retrigDecayPercent = defaults.retrigDecayPercent;
		}
		if (covers(ResetTarget::DELAY)) {
			s.delayLevelPercent = defaults.delayLevelPercent;
			s.delayRepeats = defaults.delayRepeats;
			s.delaySyncLevel = defaults.delaySyncLevel;
			s.delaySyncType = defaults.delaySyncType;
			s.delayFbVelocity = defaults.delayFbVelocity;
			s.delayPitchShift = defaults.delayPitchShift;
			s.delayNoteRandom = defaults.delayNoteRandom;
			s.delayFbClockPercent = defaults.delayFbClockPercent;
		}
		if (covers(ResetTarget::SPICE)) {
			s.spiceAmount = defaults.spiceAmount;
			s.spiceDice = defaults.spiceDice;
		}
		// The Stochastic page shows Dice as well, so its Reset puts it back as well. It is one
		// shared number, so this does roll the gates back to variation 0 too - that is the price of
		// sharing it, and a Reset that visibly leaves a column of its own page untouched reads as
		// broken, which is worse. Found on hardware, 2026-09-14.
		if (covers(ResetTarget::STOCHASTIC)) {
			s.spiceDice = defaults.spiceDice;
		}
	});
	if (covers(ResetTarget::STRUM) && clip != nullptr) {
		playFxTempHoldNoteClip(clip);
		clip->strumAmount = deluge::modulation::midi_fx::strum::kAmountOff;
		clip->strumDirection = 0;
		clip->strumContour = deluge::modulation::midi_fx::strum::kContourOff;
	}
	if (covers(ResetTarget::SPREAD) && clip != nullptr) {
		namespace sp = deluge::modulation::midi_fx::spread;
		playFxTempHoldNoteClip(clip);
		clip->spreadVelocity = sp::kOff;
		clip->spreadGate = sp::kOff;
		clip->spreadOctave = sp::kOff;
	}
	if (covers(ResetTarget::STOCHASTIC) && clip != nullptr) {
		namespace so = deluge::modulation::midi_fx::stochastic;
		playFxTempHoldNoteClip(clip);
		clip->stochasticAmount = so::kOff;
		// Back to the default width rather than to the minimum: this control has no off, so "get me
		// back" means the value the clip started life with.
		clip->stochasticWidth = so::kWidthDefault;
		// Dice is put back in the single midiFxApply above, with the rest of the MidiFxSettings
		// fields, rather than in a second pass here.
	}
	if (covers(ResetTarget::LOOP_REMIX) && clip != nullptr) {
		playFxTempHoldNoteClip(clip);
		// Page and live both: Reset is the way back, so it has to stop the remix, not just clear the
		// page. The whole kit, for the reason in midiFxIsAtDefault.
		clip->loopRemix = {};
		for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
			NoteRow* row = clip->noteRows.getElement(i);
			playFxTempHoldNoteRow(row);
			row->loopRemix = {};
		}
	}
	if (covers(ResetTarget::GROOVE) && clip != nullptr) {
		playFxTempHoldNoteClip(clip);
		// The whole kit, for the reason in midiFxIsAtDefault.
		clip->groove = {};
		for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
			NoteRow* row = clip->noteRows.getElement(i);
			playFxTempHoldNoteRow(row);
			row->groove = {};
		}
	}
	if (covers(ResetTarget::PLAY_FX) && clip != nullptr) {
		clip->playFxClockShift = 0;
		clip->playFxBeatStretch = InstrumentClip::kBeatStretchOff;
		clip->playFxDivision = 0;
		// The whole kit, for the reason in midiFxIsAtDefault.
		for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
			NoteRow* row = clip->noteRows.getElement(i);
			row->playFxClockShift = 0;
			row->playFxBeatStretch = InstrumentClip::kBeatStretchOff;
			row->playFxDivision = 0;
		}
	}
}

class Reset final : public Selection {
public:
	Reset(l10n::String newName, l10n::String newTitle, ResetTarget target)
	    : Selection(newName, newTitle), target_{target} {}

	void readCurrentValue() override { this->setValue(midiFxIsAtDefault(target_) ? 1 : 0); }

	void writeCurrentValue() override {
		if (this->getValue() == 0) {
			// "Edited" is a readout, not a command. Put the value back where the settings say it
			// should be, so the display cannot sit there claiming Edited about something that is
			// not.
			this->setValue(midiFxIsAtDefault(target_) ? 1 : 0);
			return;
		}
		midiFxApplyDefaults(target_);
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {
		    l10n::getView(STRING_FOR_MIDI_FX_EDITED),  //<
		    l10n::getView(STRING_FOR_MIDI_FX_DEFAULT), //<
		};
	}

	/// Push to reset, as well as turning to Default - the same press-to-do-it gesture the Remix button
	/// uses. Nothing to un-reset, so the press only ever applies defaults.
	MenuItem* selectButtonPress() override {
		midiFxApplyDefaults(target_);
		// Re-read (now Default) and redraw, so the value repopulates to "Default" - the same refresh the
		// turn-to-Default path gets. Without it the readout sat stale after the press.
		this->readValueAgain();
		return NO_NAVIGATION;
	}

	/// The Play FX pair are clip-wide, so they stay reachable on a Kit with no drum selected. The
	/// rest follow their own pages and hide when there is no settings object to edit.
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		if (target_ == ResetTarget::PLAY_FX) {
			return playFxEditable();
		}
		// Strum is clip-wide, so like Play FX it stays reachable on a Kit with no drum selected.
		if (target_ == ResetTarget::STRUM) {
			return strumEditable();
		}
		// Spread is clip-wide too, but unlike Strum it is hidden on a Kit entirely - so its Reset
		// follows the page rather than the other clip-wide ones.
		if (target_ == ResetTarget::SPREAD) {
			return spreadEditable();
		}
		// Stochastic hides itself on a Kit exactly as Spread does, so its Reset follows its page.
		if (target_ == ResetTarget::STOCHASTIC) {
			return stochasticEditable();
		}
		if (target_ == ResetTarget::LOOP_REMIX) {
			return loopRemixEditable();
		}
		if (target_ == ResetTarget::GROOVE) {
			return grooveEditable();
		}
		// Everything else on the MIDI FX pages needs a settings object to edit.
		return midiFxEditable();
	}

	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(deluge::l10n::built_in::seven_segment, this->name));
	}

	bool shouldEnterSubmenu() override { return false; }

private:
	ResetTarget target_;
};

/// Reset all, for the MIDI FX submenu rather than a page.
///
/// An action, not a value. The paged Reset items work because a horizontal page lets the encoder
/// change the selected column in place; a Selection sitting in a vertical submenu that neither
/// enters nor toggles has nothing to turn, which is why "Reset all" did nothing at all on hardware.
/// This is the same shape as the other one-shot menu items in the tree, e.g.
/// audio_clip::SetClipLengthEqualToSampleLength.
class ResetAll final : public MenuItem {
public:
	using MenuItem::MenuItem;

	MenuItem* selectButtonPress() override {
		midiFxApplyDefaults(ResetTarget::ALL);
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_MIDI_FX_RESET));
		return NO_NAVIGATION;
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return midiFxSwitchedOn() && (midiFxEditable() || getCurrentInstrumentClip() != nullptr);
	}

	bool shouldEnterSubmenu() override { return false; }
};

} // namespace deluge::gui::menu_item::midi_fx
