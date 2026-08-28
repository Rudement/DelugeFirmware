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
#include "model/settings/runtime_feature_settings.h"
#include <iterator>

// NOT `clouds`: the vendored DSP already occupies a global `clouds::`
// namespace, and menus.cpp has `using namespace gui::menu_item;`, which would
// make a bare `clouds::Mode` there ambiguous between the two.
namespace deluge::gui::menu_item::clouds_fx {

/// Playback-mode selector for the vendored Clouds engine, and its on/off
/// switch: OFF is the first option, and selecting it frees the engine's whole
/// ~180 KB working buffer rather than merely muting it.
class Mode final : public Selection {
public:
	/// Hidden along with the rest of Clouds when the community feature is switched off. The effect
	/// itself keeps running, so a song already using Clouds is unaffected by hiding its menus.
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::EnableCloudsFX);
	}
	using Selection::Selection;

	/// Stretch is off the picker on this build. It is the one mode that still takes
	/// the device down here, and five modes you can trust beat six you cannot. The
	/// CloudsMode enum, the nine param IDs and the file format are all untouched --
	/// only what this menu offers changes -- so a song saved either side of this is
	/// unaffected, and putting Stretch back is a one-line revert.
	static constexpr CloudsMode kSelectableModes[] = {
	    CloudsMode::OFF,       CloudsMode::GRANULAR, CloudsMode::DELAY,
	    CloudsMode::SPECTRAL,  CloudsMode::OLIVERB,  CloudsMode::RESONESTOR,
	};

	void readCurrentValue() override {
		CloudsMode current = ModControllableAudio::displayedCloudsModeFor(soundEditor.currentModControllable);
		for (size_t i = 0; i < std::size(kSelectableModes); ++i) {
			if (kSelectableModes[i] == current) {
				this->setValue(i);
				return;
			}
		}
		// A song saved with Stretch, loaded on this build: show OFF rather than an
		// index that is not in the list. The engine keeps running whatever it was
		// given until the user picks something here.
		this->setValue(0);
	}

	/// Deferred, NOT applied here. selectEncoderAction() calls this on every detent,
	/// and building a mode costs a reset-path Prepare() with interrupts disabled --
	/// doing that once per click while the dial spins is what took the device down.
	/// requestCloudsMode() records it; the main-loop tick applies the one the user
	/// stops on, and reports an allocation failure if it comes to that.
	void writeCurrentValue() override {
		size_t index = static_cast<size_t>(this->getValue());
		if (index >= std::size(kSelectableModes)) {
			return;
		}
		ModControllableAudio::requestCloudsMode(soundEditor.currentModControllable, kSelectableModes[index]);
	}

	/// Leaving the menu applies immediately: waiting out the settle window to hear
	/// the mode you just confirmed would feel broken.
	MenuItem* selectButtonPress() override {
		ModControllableAudio::flushPendingCloudsMode();
		return Selection::selectButtonPress();
	}

	// 1.2.1's Selection::getOptions takes no OptType -- that parameter arrives with the 1.3
	// menu rework. Signature adapted here rather than in the caller; the body is unchanged.
	deluge::vector<std::string_view> getOptions() override {
		using enum deluge::l10n::String;
		// Order must match kSelectableModes exactly -- the value is an index into it,
		// not a CloudsMode. Stretch is deliberately absent from both.
		return {
		    l10n::getView(STRING_FOR_DISABLED),
		    l10n::getView(STRING_FOR_CLOUDS_MODE_GRANULAR),
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

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->cloudsFreeze); }

	void writeCurrentValue() override {
		bool frozen = this->getValue();
		soundEditor.currentModControllable->cloudsFreeze = frozen;
		if (soundEditor.currentModControllable->cloudsFX != nullptr) {
			soundEditor.currentModControllable->cloudsFX->setFreeze(frozen);
		}
	}

	/// Pointless while the engine is off, and actively confusing: freezing a
	/// buffer that is not being recorded into does nothing audible. Also goes when the community
	/// feature is switched off, along with the rest of Clouds.
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::EnableCloudsFX) && modControllable != nullptr
		       && modControllable->cloudsMode != CloudsMode::OFF;
	}
};

} // namespace deluge::gui::menu_item::clouds_fx
