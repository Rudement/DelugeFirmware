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
#include "definitions_cxx.hpp"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/selection.h"
#include "gui/menu_item/toggle.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "dsp/clouds_adapter.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "gui/menu_item/unpatched_param.h"
#include "model/model_stack.h"
#include "model/song/song.h"

// NOT `clouds`: the vendored DSP already occupies a global `clouds::`
// namespace, and menus.cpp has `using namespace gui::menu_item;`, which would
// make a bare `clouds::Mode` there ambiguous between the two.
namespace deluge::gui::menu_item::clouds_fx {

/// Playback-mode selector for the vendored Clouds engine, and its on/off
/// switch: OFF is the first option, and selecting it frees the engine's whole
/// ~180 KB working buffer rather than merely muting it.
class Mode final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override {
		this->setValue(currentSong->globalEffectable.cloudsMode);
	}

	void writeCurrentValue() override {
		auto mode = this->getValue<CloudsMode>();
		if (!currentSong->globalEffectable.setCloudsMode(mode)) {
			// Allocation failed. setCloudsMode leaves the mode OFF in that
			// case, so tell the user rather than showing a mode that is not
			// actually running.
			display->displayError(Error::INSUFFICIENT_RAM);
			readCurrentValue();
		}
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum deluge::l10n::String;
		// Order must match CloudsMode exactly -- this is indexed by value.
		return {
		    l10n::getView(STRING_FOR_DISABLED),
		    l10n::getView(STRING_FOR_CLOUDS_MODE_GRANULAR),
		    l10n::getView(STRING_FOR_CLOUDS_MODE_STRETCH),
		    l10n::getView(STRING_FOR_CLOUDS_MODE_DELAY),
		    l10n::getView(STRING_FOR_CLOUDS_MODE_SPECTRAL),
		    l10n::getView(STRING_FOR_CLOUDS_MODE_OLIVERB),
		    l10n::getView(STRING_FOR_CLOUDS_MODE_RESONESTOR),
		};
	}
};

/// Freeze holds the recording buffer: grains keep playing, but nothing new is
/// written in. Upstream exposes it as a momentary button; here it is a latch,
/// because the Deluge has no spare panel button for it and a latched freeze is
/// the more useful of the two live.
class Freeze final : public Toggle {
public:
	using Toggle::Toggle;

	void readCurrentValue() override { this->setValue(currentSong->globalEffectable.cloudsFreeze); }

	void writeCurrentValue() override {
		bool frozen = this->getValue();
		currentSong->globalEffectable.cloudsFreeze = frozen;
		if (currentSong->globalEffectable.cloudsFX != nullptr) {
			currentSong->globalEffectable.cloudsFX->setFreeze(frozen);
		}
	}

	/// Pointless while the engine is off, and actively confusing: freezing a
	/// buffer that is not being recorded into does nothing audible.
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return currentSong != nullptr && currentSong->globalEffectable.cloudsMode != CloudsMode::OFF;
	}
};

/// One of the nine engine parameters. Always edits the SONG's param manager,
/// whatever context the menu was opened from.
///
/// This is what makes Clouds controllable from a synth clip. There is exactly
/// one engine, so there is exactly one set of engine parameters, and a plain
/// UnpatchedParam would read and write soundEditor.currentParamManager -- the
/// synth's own, where these UNPATCHED_GLOBAL ids are out of range. Only the
/// Send amount is per-source, and that one is a normal UnpatchedParam because
/// UNPATCHED_CLOUDS_SEND lives in the shared block.
class SongParam final : public UnpatchedParam {
public:
	using UnpatchedParam::UnpatchedParam;

	[[nodiscard]] ParamSet* getParamSet() final {
		return currentSong->paramManager.getUnpatchedParamSet();
	}

	ModelStackWithAutoParam* getModelStack(void* memory) final {
		ModelStackWithThreeMainThings* modelStack = setupModelStackWithThreeMainThingsButNoNoteRow(
		    memory, currentSong, &currentSong->globalEffectable, nullptr, &currentSong->paramManager);
		return modelStack->getUnpatchedAutoParamFromId(getP());
	}

	void readCurrentValue() final {
		this->setValue(computeCurrentValueForStandardMenuItem(
		    currentSong->paramManager.getUnpatchedParamSet()->getValue(getP())));
	}
};

} // namespace deluge::gui::menu_item::clouds_fx
