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
#include "gui/menu_item/selection.h"
#include "gui/menu_item/submenu.h"
#include "gui/menu_item/toggle.h"
#include "model/output.h"
#include "model/song/song.h"
#include "model/usb_route.h"
#include "processing/engines/usb_audio_stream.h"
#include "storage/flash_storage.h"

namespace deluge::gui::menu_item::usb_audio {

/// One USB destination for the current track: a mono channel or a stereo pair, by bit.
class ChannelToggle final : public Toggle {
public:
	ChannelToggle(l10n::String newName, l10n::String title, uint16_t newBit) : Toggle(newName, title), bit(newBit) {}

	void readCurrentValue() override {
		Output* output = getCurrentOutput();
		this->setValue(output != nullptr && (output->usbRouting & bit) != 0);
	}

	void writeCurrentValue() override {
		Output* output = getCurrentOutput();
		if (output == nullptr) {
			return;
		}
		if (this->getValue()) {
			output->usbRouting |= bit;
		}
		else {
			output->usbRouting &= (uint16_t)~bit;
		}
	}

private:
	const uint16_t bit;
};

/// Whether the current track stays in the Deluge's own outputs. Off leaves USB as its only way out.
class MainToggle final : public Toggle {
public:
	using Toggle::Toggle;

	void readCurrentValue() override {
		Output* output = getCurrentOutput();
		this->setValue(output == nullptr || output->isInMainMix());
	}

	void writeCurrentValue() override {
		Output* output = getCurrentOutput();
		if (output == nullptr) {
			return;
		}
		if (this->getValue()) {
			output->usbRouting |= UsbRoute::MAIN;
		}
		else {
			output->usbRouting &= (uint16_t)~UsbRoute::MAIN;
		}
	}
};

/// Hidden on MIDI and CV tracks, which render no audio.
class RoutingSubmenu final : public Submenu {
public:
	using Submenu::Submenu;

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		const OutputType type = getCurrentOutputType();
		return type != OutputType::MIDI_OUT && type != OutputType::CV;
	}
};

/// Which returning pair is summed into the song's mix: off, 1-2 or 3-4.
class ReturnPair final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override {
		this->setValue((int32_t)deluge::processing::engines::USBAudioStream::getMasterReturnPair());
	}

	void writeCurrentValue() override {
		deluge::processing::engines::USBAudioStream::setMasterReturnPair((uint32_t)this->getValue());
		FlashStorage::usbAudioReturnPair = (uint8_t)this->getValue();
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		return {l10n::getView(l10n::String::STRING_FOR_OFF), l10n::getView(l10n::String::STRING_FOR_USB_PAIR_12),
		        l10n::getView(l10n::String::STRING_FOR_USB_PAIR_34)};
	}
};

/// Level of the returning pair, 0-50 in 1.2 dB steps: 0 silence, 40 unity, 50 is +12 dB.
class ReturnLevel final : public Integer {
public:
	using Integer::Integer;

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override {
		return (int32_t)deluge::processing::engines::USBAudioStream::kReturnLevelMax;
	}

	void readCurrentValue() override {
		this->setValue((int32_t)deluge::processing::engines::USBAudioStream::getReturnLevel());
	}

	void writeCurrentValue() override {
		deluge::processing::engines::USBAudioStream::setReturnLevel((uint32_t)this->getValue());
		FlashStorage::usbAudioReturnLevel = (uint8_t)this->getValue();
	}
};

/// Level of every outgoing USB channel, 0-50 in 1.2 dB steps: 0 silence, 40 unity (the default), 50 is +12 dB.
class OutLevel final : public Integer {
public:
	using Integer::Integer;

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override {
		return (int32_t)deluge::processing::engines::USBAudioStream::kOutLevelMax;
	}

	void readCurrentValue() override {
		this->setValue((int32_t)deluge::processing::engines::USBAudioStream::getOutLevel());
	}

	void writeCurrentValue() override {
		deluge::processing::engines::USBAudioStream::setOutLevel((uint32_t)this->getValue());
		FlashStorage::usbAudioOutLevel = (uint8_t)this->getValue();
	}
};

/// How far, in ms, the Deluge holds its own audio back so audio returning from an external processor lands in time.
/// 0 is off.
class Compensation final : public Integer {
public:
	using Integer::Integer;

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override {
		return (int32_t)deluge::processing::engines::USBAudioStream::kCompensationMaxMs;
	}

	void readCurrentValue() override {
		this->setValue((int32_t)deluge::processing::engines::USBAudioStream::getCompensationMs());
	}

	void writeCurrentValue() override {
		deluge::processing::engines::USBAudioStream::setCompensationMs((uint32_t)this->getValue());
		FlashStorage::usbAudioCompensationMs = (uint8_t)this->getValue();
	}
};

} // namespace deluge::gui::menu_item::usb_audio
