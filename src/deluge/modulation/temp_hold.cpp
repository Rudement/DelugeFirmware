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

#include "modulation/temp_hold.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "gui/views/view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "model/clip/instrument_clip.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/note/note_row.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param_collection.h"
#include "processing/engines/audio_engine.h"

namespace deluge::modulation::temp_hold {

TempHold tempHold{};

bool TempHold::shouldCapture() {
	if (!runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::TempHold)) {
		return false;
	}
	// Either half holds. A hand on SHIFT and a foot on the switch are the same gesture as far as
	// everything below here is concerned.
	return Buttons::isShiftButtonPressed() || tempHold.midiEngaged_;
}

void TempHold::midiEngage(bool on) {
	if (on) {
		midiEngaged_ = true;
		return;
	}
	midiEngaged_ = false;
	// Cleared first, so the guard at the top of end() sees the true state of both halves.
	end();
}

void TempHold::beginIfNeeded() {
	if (active_) {
		return;
	}
	active_ = true;
	song_ = currentSong;
	overflowed_ = false;
	numParams_ = 0;
	numSettings_ = 0;
}

void TempHold::abandon() {
	// Also called when what the tables point at is about to go away (song load, clip delete), so any
	// glide still running goes too.
	numGlides_ = 0;
	clearHold();
}

void TempHold::clearHold() {
	active_ = false;
	song_ = nullptr;
	overflowed_ = false;
	numParams_ = 0;
	numSettings_ = 0;
}

void TempHold::end() {
	if (!active_) {
		return;
	}

	// One half letting go is not the end of the hold while the other is still down. SHIFT released
	// with a foot still on the switch has to leave everything where it is, and the reverse likewise -
	// otherwise the two halves fight and whichever is let go of first wins.
	//
	// Both callers clear their own flag before getting here: Buttons::commandToggleShift() sets
	// shiftCurrentlyPressed false first, and midiEngage(false) clears midiEngaged_ first.
	if (midiEngaged_ || Buttons::isShiftButtonPressed()) {
		return;
	}

	// A hold that spans a song load has nothing left to put anything back into. Everything in both
	// tables points at objects the loader has already freed, so the only safe move is to drop it.
	const bool sameSong = (song_ != nullptr && song_ == currentSong);
	const bool didAnything = (numParams_ > 0 || numSettings_ > 0);

	if (sameSong) {
		restoreParams();
		restoreSettings();
	}

	const bool tellThemItOverflowed = overflowed_;

	// Not abandon(): that would also kill the glides restoreParams() has just started.
	clearHold();

	// **Nothing is refreshed from here, deliberately.**
	//
	// This used to call View::setKnobIndicatorLevels() and View::sendMidiFollowFeedback() so the knob
	// rings and a following controller would catch up with the restored values. Both are functions
	// this fork has already had to guard once each - setKnobIndicatorLevels() reaches
	// Sound::getParamFromModEncoderDeeper(), and sendMidiFollowFeedback() walks the selected-clip
	// lookup the E411 family lives on - and calling them from inside the SHIFT release handler,
	// straight after writing values back through stored model stacks, is a moment nothing else in the
	// firmware calls them at.
	//
	// They were never needed. Both are cosmetic: the indicators and any following controller refresh
	// on the next UI event anyway, and the values themselves are already back by this point. A refresh
	// that cannot fail is worth more than one that is prettier by half a second.
	//
	// The one exception, added after hardware found it: a menu left open on a restored value kept
	// showing the value the hold put there, so the sound snapped back and the screen did not. That is
	// not cosmetic in the same way - it is the screen telling you something untrue about the state of
	// the song, on the page you are looking at.
	//
	// This refresh is a different animal from the two that were removed. It renders; it does not walk
	// the model the way setKnobIndicatorLevels() reaches into Sound::getParamFromModEncoderDeeper() or
	// sendMidiFollowFeedback() walks the selected-clip lookup. And it fires once per SHIFT release,
	// which is human rate - the FULL freeze (claude/oled-queue-full-freeze.md) was a popup per MIDI CC,
	// three orders of magnitude away from this. Guarded on the sound editor actually being open, so
	// nothing pays for it when no menu is showing.
	//
	// Both shapes are covered: a leaf item re-reads and redraws itself through readValueAgain(), and a
	// horizontal menu re-reads every item on the page as part of rendering it.
	if (didAnything && sameSong && getCurrentUI() == &soundEditor) {
		if (MenuItem* item = soundEditor.getCurrentMenuItem(); item != nullptr) {
			item->readValueAgain();
		}
		if (display->haveOLED()) {
			renderUIsForOled();
		}
	}

	if (tellThemItOverflowed) {
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_TEMP_HOLD_FULL));
	}
}

void TempHold::noteParam(ModelStackWithAutoParam* modelStackWithParam, int32_t currentValue) {
	if (!active_ || modelStackWithParam == nullptr || modelStackWithParam->autoParam == nullptr) {
		return;
	}

	// A new hold grabbing a param that is still gliding back from the last one: the value it should
	// come back to is where the glide was headed, not wherever the glide happened to be.
	for (int32_t i = 0; i < numGlides_; i++) {
		if (glides_[i].autoParam == modelStackWithParam->autoParam) {
			currentValue = glides_[i].toValue;
			glides_[i] = glides_[--numGlides_];
			break;
		}
	}

	// Keyed on the AutoParam, not on the knob that moved it - see the header note. Turning the same
	// knob ten times inside one hold must still restore the value it had before the first turn.
	for (int32_t i = 0; i < numParams_; i++) {
		if (params_[i].autoParam == modelStackWithParam->autoParam) {
			return;
		}
	}

	if (numParams_ >= kMaxParams) {
		overflowed_ = true;
		return;
	}

	ParamEntry& entry = params_[numParams_];
	entry.autoParam = modelStackWithParam->autoParam;
	entry.originalValue = currentValue;
	copyModelStack(entry.stackMemory, modelStackWithParam, sizeof(ModelStackWithAutoParam));
	numParams_++;
}

bool TempHold::noteObject(SettingsKind kind, void* object, SettingsEntry** entryOut) {
	if (!active_ || object == nullptr) {
		return false;
	}
	for (int32_t i = 0; i < numSettings_; i++) {
		if (settings_[i].kind == kind && settings_[i].object == object) {
			return false;
		}
	}
	if (numSettings_ >= kMaxSettings) {
		overflowed_ = true;
		return false;
	}
	SettingsEntry& entry = settings_[numSettings_];
	entry.kind = kind;
	entry.object = object;
	*entryOut = &entry;
	numSettings_++;
	return true;
}

void TempHold::noteMidiFx(deluge::modulation::midi_fx::MidiFxSettings* settings) {
	SettingsEntry* entry = nullptr;
	if (!noteObject(SettingsKind::MIDI_FX, settings, &entry)) {
		return;
	}
	// The whole struct, not the one field being edited. It is twenty-odd bytes, and a page-at-a-time
	// copy means a hold that walks across Velocity, Gate and Spice takes one entry rather than three
	// and cannot half-restore a page.
	entry->midiFx.cloneFrom(settings);
}

void TempHold::notePlayFxRow(NoteRow* row) {
	SettingsEntry* entry = nullptr;
	if (!noteObject(SettingsKind::PLAY_FX_ROW, row, &entry)) {
		return;
	}
	entry->clockShift = row->playFxClockShift;
	entry->beatStretch = row->playFxBeatStretch;
	entry->division = row->playFxDivision;
	entry->loopRemix = row->loopRemix;
	entry->groove = row->groove;
}

void TempHold::notePlayFxClip(InstrumentClip* clip) {
	SettingsEntry* entry = nullptr;
	if (!noteObject(SettingsKind::CLIP_WIDE_FX, clip, &entry)) {
		return;
	}
	entry->clockShift = clip->playFxClockShift;
	entry->beatStretch = clip->playFxBeatStretch;
	entry->division = clip->playFxDivision;
	entry->strumAmount = clip->strumAmount;
	entry->strumDirection = clip->strumDirection;
	entry->strumContour = clip->strumContour;
	entry->loopRemix = clip->loopRemix;
	entry->groove = clip->groove;
}

namespace {
/// The delay "amount" (which on the Deluge is the feedback) in either of its two homes: the patched
/// GLOBAL_DELAY_FEEDBACK on a Sound, the unpatched UNPATCHED_DELAY_AMOUNT on a Kit, audio clip or the
/// song. Both of those owners are ModControllableAudio, which is what makes the cast in
/// restoreParams() safe - nothing else carries either param.
/// Mod FX "how much" params - depth, feedback, offset - in both homes: Sound (depth patched; feedback
/// and offset unpatched-shared) and GlobalEffectable (all unpatched-global). Rate is left out on
/// purpose: a rate that sweeps back over a second and a half is a swoop, not a tail.
bool isModFxTailParam(ModelStackWithAutoParam* stack) {
	if (stack->paramCollection == nullptr) {
		return false;
	}
	namespace p = deluge::modulation::params;
	const auto kind = stack->paramCollection->getParamKind();
	const int32_t id = stack->paramId;
	if (kind == p::Kind::PATCHED) {
		return id == p::GLOBAL_MOD_FX_DEPTH;
	}
	if (kind == p::Kind::UNPATCHED_SOUND || kind == p::Kind::UNPATCHED_GLOBAL) {
		if (id == p::UNPATCHED_MOD_FX_FEEDBACK || id == p::UNPATCHED_MOD_FX_OFFSET) {
			return true;
		}
		return kind == p::Kind::UNPATCHED_GLOBAL && id == p::UNPATCHED_MOD_FX_DEPTH;
	}
	return false;
}

bool isDelayAmount(ModelStackWithAutoParam* stack) {
	if (stack->modControllable == nullptr || stack->paramCollection == nullptr) {
		return false;
	}
	const auto kind = stack->paramCollection->getParamKind();
	return (kind == deluge::modulation::params::Kind::PATCHED
	        && stack->paramId == deluge::modulation::params::GLOBAL_DELAY_FEEDBACK)
	       || (kind == deluge::modulation::params::Kind::UNPATCHED_GLOBAL
	           && stack->paramId == deluge::modulation::params::UNPATCHED_DELAY_AMOUNT);
}
} // namespace

void TempHold::restoreParams() {
	const bool tails = runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::TempHoldTails);
	for (int32_t i = 0; i < numParams_; i++) {
		ParamEntry& entry = params_[i];
		auto* stack = reinterpret_cast<ModelStackWithAutoParam*>(entry.stackMemory);
		if (stack->autoParam == nullptr || stack->autoParam != entry.autoParam) {
			continue;
		}
		// Temp Hold tails: a delay amount coming back down lets what is in the delay ring out rather
		// than stop dead. Told before the write, so the delay still knows the amount it was holding.
		if (tails && isDelayAmount(stack)) {
			static_cast<ModControllableAudio*>(stack->modControllable)->delay.beginTail();
		}
		// Mod FX has no buffer to ring out - its "tail" is the sound of the effect leaving. So with
		// Tails on its params glide back instead of snapping. See tickGlides().
		if (tails && isModFxTailParam(stack) && numGlides_ < kMaxParams) {
			GlideEntry& glide = glides_[numGlides_++];
			glide.autoParam = entry.autoParam;
			glide.fromValue = entry.autoParam->getCurrentValue();
			glide.toValue = entry.originalValue;
			glide.startTime = AudioEngine::audioSampleTimer;
			glideSong_ = currentSong;
			copyModelStack(glide.stackMemory, stack, sizeof(ModelStackWithAutoParam));
			continue;
		}
		// The stored stack already has its timeline counter nulled (that is what made the original
		// change non-automated), so this write is a live value change too and modPos / modLength
		// are along for the ride rather than doing anything.
		stack->autoParam->setValuePossiblyForRegion(entry.originalValue, stack, view.modPos, view.modLength);
	}
	numParams_ = 0;
}

void TempHold::dropGlide(AutoParam* autoParam) {
	for (int32_t i = 0; i < numGlides_; i++) {
		if (glides_[i].autoParam == autoParam) {
			glides_[i] = glides_[--numGlides_];
			return;
		}
	}
}

void TempHold::tickGlides() {
	if (numGlides_ == 0) {
		return;
	}
	// The song the glides belong to is gone - abandon() normally catches this first, but a glide must
	// never write through a stack into a freed project.
	if (glideSong_ != currentSong) {
		numGlides_ = 0;
		return;
	}
	const uint32_t now = AudioEngine::audioSampleTimer;
	for (int32_t i = 0; i < numGlides_;) {
		GlideEntry& glide = glides_[i];
		auto* stack = reinterpret_cast<ModelStackWithAutoParam*>(glide.stackMemory);
		const uint32_t elapsed = now - glide.startTime;
		const bool done = elapsed >= kGlideSamples;
		int32_t value = glide.toValue;
		if (!done) {
			const int64_t span = static_cast<int64_t>(glide.toValue) - glide.fromValue;
			value = static_cast<int32_t>(glide.fromValue + (span * elapsed) / kGlideSamples);
		}
		if (stack->autoParam == glide.autoParam) {
			stack->autoParam->setValuePossiblyForRegion(value, stack, view.modPos, view.modLength);
		}
		if (done || stack->autoParam != glide.autoParam) {
			glides_[i] = glides_[--numGlides_];
			continue;
		}
		i++;
	}
}

void TempHold::restoreSettings() {
	for (int32_t i = 0; i < numSettings_; i++) {
		SettingsEntry& entry = settings_[i];
		switch (entry.kind) {
		case SettingsKind::MIDI_FX: {
			auto* settings = static_cast<deluge::modulation::midi_fx::MidiFxSettings*>(entry.object);
			settings->cloneFrom(&entry.midiFx);
			break;
		}
		case SettingsKind::PLAY_FX_ROW: {
			auto* row = static_cast<NoteRow*>(entry.object);
			row->playFxClockShift = entry.clockShift;
			row->playFxBeatStretch = entry.beatStretch;
			row->playFxDivision = entry.division;
			row->loopRemix = entry.loopRemix;
			row->groove = entry.groove;
			break;
		}
		case SettingsKind::CLIP_WIDE_FX: {
			auto* clip = static_cast<InstrumentClip*>(entry.object);
			clip->playFxClockShift = entry.clockShift;
			clip->playFxBeatStretch = entry.beatStretch;
			clip->playFxDivision = entry.division;
			clip->strumAmount = entry.strumAmount;
			clip->strumDirection = entry.strumDirection;
			clip->strumContour = entry.strumContour;
			clip->loopRemix = entry.loopRemix;
			clip->groove = entry.groove;
			break;
		}
		case SettingsKind::NONE:
		default:
			break;
		}
	}
	numSettings_ = 0;
}

} // namespace deluge::modulation::temp_hold
