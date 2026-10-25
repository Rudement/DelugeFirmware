// Rudement: the song reverb's HPF and LPF as a frequency instead of 0-50, behind
// Community Features > Rudement upgrades > Reverb Filter Hz. Display only: the menu value, what is saved and
// what the reverb does are all exactly stock. The numbers mirror reverb::Base::calcFilterCutoff().
#pragma once
#include "definitions_cxx.hpp"
#include "model/settings/runtime_feature_settings.h"
#include "util/d_stringbuf.h"
#include <cmath>

namespace deluge::gui::menu_item::reverb {

inline float reverbFilterHz(int32_t menuValue, bool highPass) {
	const float f = static_cast<float>(menuValue) / kMaxMenuValue;
	const float minHz = highPass ? 20.0f : 0.0f;
	const float scale = highPass ? 250.0f : 5083.74f;
	return minHz + (std::exp(1.5f * f) - 1.0f) * scale;
}

/// Returns false, writing nothing, while the switch is Off, so the caller shows the stock 0-50 number.
inline bool appendReverbFilterHz(StringBuf& value, int32_t menuValue, bool highPass) {
	if (runtimeFeatureSettings.get(RuntimeFeatureSettingType::ReverbFilterHz) != RuntimeFeatureStateToggle::On) {
		return false;
	}
	if (!highPass && menuValue <= 0) {
		value.append("CLOSED");
		return true;
	}
	const float hz = reverbFilterHz(menuValue, highPass);
	if (hz < 1000.f) {
		value.appendInt(static_cast<int32_t>(std::lround(hz)));
		value.append("Hz");
		return true;
	}
	const float kHz = hz / 1000.f;
	value.appendFloat(kHz, 1, kHz < 10.f ? 2 : 1);
	value.append("kHz");
	return true;
}

} // namespace deluge::gui::menu_item::reverb
