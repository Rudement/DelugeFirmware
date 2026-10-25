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

#include "gui/menu_item/midi_fx/switched_on.h"
#include "gui/ui/sound_editor.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "modulation/midi_fx/midi_fx_settings.h"
#include "modulation/temp_hold.h"

/// Which settings object a MIDI FX menu item is editing.
///
/// On a synth, MIDI or CV clip there is one and the sound editor already points at it. On a Kit
/// the settings are per drum, and the sound editor points at the selected drum's - which leaves
/// AFFECT ENTIRE with nothing, so every MIDI FX page hid itself. That is not how the rest of the
/// Deluge behaves: AFFECT ENTIRE is the kit-wide gesture, and a menu that vanishes under it reads
/// as broken rather than as scoped.
///
/// So under AFFECT ENTIRE a read comes from the first drum and a write goes to every drum. The
/// same rule Play FX uses for its rows, for the same reason.
namespace deluge::gui::menu_item::midi_fx {

/// The Kit whose drums a kit-wide edit should reach, or null when this is not that situation.
[[nodiscard]] inline Kit* midiFxKitUnderAffectEntire() {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr || clip->output == nullptr || clip->output->type != OutputType::KIT) {
		return nullptr;
	}
	if (!clip->affectEntire) {
		return nullptr;
	}
	return (Kit*)clip->output;
}

/// What to show. Null when there is genuinely nothing to edit - a Kit with no drum selected and no
/// AFFECT ENTIRE - and the items hide themselves rather than editing something arbitrary.
[[nodiscard]] inline deluge::modulation::midi_fx::MidiFxSettings* midiFxSettingsForRead() {
	if (soundEditor.currentMidiFxSettings != nullptr) {
		return soundEditor.currentMidiFxSettings;
	}
	Kit* kit = midiFxKitUnderAffectEntire();
	if (kit != nullptr && kit->firstDrum != nullptr) {
		// The first drum stands in for the kit. They are about to be written together anyway, and
		// showing one of them beats showing nothing.
		return &kit->firstDrum->midiFxSettings;
	}
	return nullptr;
}

/// Applies a change to everything the current selection covers: every drum on a Kit with AFFECT
/// ENTIRE on, otherwise the one settings object being edited.
///
/// **AFFECT ENTIRE is checked first, and that ordering is the whole point.** The sound editor only
/// leaves currentMidiFxSettings null when the menu was opened with the select knob; open the same
/// menu through a grid shortcut and it points at whichever drum the shortcut belongs to - AFFECT
/// ENTIRE lit, one drum edited. Asking the clip rather than the editor is what makes the button
/// mean the same thing however you got here.
/// Records a settings object with Temp Hold before it is written, so SHIFT-release puts it back.
///
/// The whole struct is stored rather than the one field being edited, which means a hold that walks
/// across Velocity, Gate and Spice on one drum costs one table entry instead of three and cannot
/// half-restore a page. Reset goes through `midiFxApply` too, so "hold SHIFT and reset this page"
/// is a temporary reset - which is the right answer, and free.
inline void midiFxTempHoldNote(deluge::modulation::midi_fx::MidiFxSettings* settings) {
	if (deluge::modulation::temp_hold::TempHold::shouldCapture()) {
		deluge::modulation::temp_hold::tempHold.beginIfNeeded();
		deluge::modulation::temp_hold::tempHold.noteMidiFx(settings);
	}
}

template <typename Fn>
inline void midiFxApply(Fn&& apply) {
	if (Kit* kit = midiFxKitUnderAffectEntire(); kit != nullptr) {
		for (Drum* drum = kit->firstDrum; drum != nullptr; drum = drum->next) {
			midiFxTempHoldNote(&drum->midiFxSettings);
			apply(drum->midiFxSettings);
		}
		return;
	}
	if (soundEditor.currentMidiFxSettings != nullptr) {
		midiFxTempHoldNote(soundEditor.currentMidiFxSettings);
		apply(*soundEditor.currentMidiFxSettings);
	}
}

[[nodiscard]] inline bool midiFxEditable() {
	return midiFxSwitchedOn() && midiFxSettingsForRead() != nullptr;
}

} // namespace deluge::gui::menu_item::midi_fx
