/*
 * Copyright (c) 2014-2023 Synthstrom Audible Limited
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

// Rudement: delay HPF and LPF, modelled on menu_item/reverb/hpf.h and lpf.h and on delay/analog.h for the
// affect-entire behaviour. Same 0..kMaxMenuValue scale as the reverb's; HPF 0 and LPF max are off.
#pragma once
#include "definitions_cxx.hpp"
#include "gui/menu_item/integer.h"
#include "gui/ui/sound_editor.h"
#include "model/instrument/kit.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include "util/d_stringbuf.h"
#include <cmath>

namespace deluge::gui::menu_item::delay {

class FilterBase : public Integer {
public:
	using Integer::Integer;
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxMenuValue; }
	bool usesAffectEntire() override { return true; }

	// Community Features > Rudement upgrades > Delay Filters Off hides both items (and the DSP skips the filters too).
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DelayFilters);
	}

	// The popup shows where the filter sits, like the EQ: "OFF", "90Hz", "3.4kHz". The stored value and the
	// 0-50 scale are unchanged; this is only a label.
	void getNotificationValue(StringBuf& value) override {
		const float f = static_cast<float>(this->getValue()) / kMaxMenuValue;
		if (isOff(f)) {
			value.append("OFF");
			return;
		}
		const float hz = cutoffHz(f);
		if (hz < 1000.f) {
			value.appendInt(static_cast<int32_t>(std::lround(hz)));
			value.append("Hz");
			return;
		}
		const float kHz = hz / 1000.f;
		value.appendFloat(kHz, 1, kHz < 10.f ? 2 : 1);
		value.append("kHz");
	}

	void readCurrentValue() override {
		this->setValue(static_cast<int32_t>(std::round(readFilter(soundEditor.currentModControllable->delay) * kMaxMenuValue)));
	}

	void writeCurrentValue() override {
		const float value = static_cast<float>(this->getValue()) / kMaxMenuValue;
		// If affect-entire button held, do whole kit
		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					writeFilter(static_cast<SoundDrum*>(thisDrum)->delay, value);
				}
			}
		}
		else {
			writeFilter(soundEditor.currentModControllable->delay, value);
		}
	}

protected:
	virtual float readFilter(const Delay& delay) const = 0;
	virtual void writeFilter(Delay& delay, float value) const = 0;
	virtual bool isOff(float value) const = 0;
	virtual float cutoffHz(float value) const = 0;
};

class HPF final : public FilterBase {
public:
	using FilterBase::FilterBase;
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return RenderingStyle::HPF; }

protected:
	float readFilter(const Delay& delay) const override { return delay.getHPF(); }
	void writeFilter(Delay& delay, float value) const override { delay.setHPF(value); }
	bool isOff(float value) const override { return value <= 0.f; }
	float cutoffHz(float value) const override { return Delay::hpfHz(value); }
};

class LPF final : public FilterBase {
public:
	using FilterBase::FilterBase;
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return RenderingStyle::LPF; }

protected:
	float readFilter(const Delay& delay) const override { return delay.getLPF(); }
	void writeFilter(Delay& delay, float value) const override { delay.setLPF(value); }
	bool isOff(float value) const override { return value >= 1.f; }
	float cutoffHz(float value) const override { return Delay::lpfHz(value); }
};

} // namespace deluge::gui::menu_item::delay
