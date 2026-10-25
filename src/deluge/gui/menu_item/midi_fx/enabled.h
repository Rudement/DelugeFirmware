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
#pragma once
#include "gui/menu_item/midi_fx/target.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"

/// NOTE: there is also a `deluge::modulation::midi_fx` (the effects themselves). Any file with
/// using-directives for both `deluge::modulation` and `deluge::gui::menu_item` - `menus.cpp` is
/// one - must qualify as `menu_item::midi_fx::` or the name is ambiguous.
namespace deluge::gui::menu_item::midi_fx {

/// The MIDI FX master switch, modelled on arpeggiator::Mode.
///
/// Unlike the arp's switch this does not have to cut sound when it changes. Switching the arp
/// off strands notes the arp was responsible for; MIDI FX effects are responsible for their own
/// cleanup and `MidiFxChain::flush()` is deliberately not gated on `enabled`, so turning this
/// off releases anything pending through the normal path.
class Enabled final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override {
		this->setValue(midiFxSettingsForRead() != nullptr && midiFxSettingsForRead()->enabled);
	}

	void writeCurrentValue() override {
		// getValue<E>() is constrained to `util::enumeration` (value.h:46), so getValue<bool>()
		// does not compile. Value<int32_t>::getValue() is the one that exists, and comparing
		// against 0 is what every other toggle in the tree does.
		const bool on = this->getValue() != 0;
		midiFxApply([on](deluge::modulation::midi_fx::MidiFxSettings& s) { s.enabled = on; });
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {
		    l10n::getView(STRING_FOR_OFF), //<
		    l10n::getView(STRING_FOR_ON),  //<
		};
	}

	/// Null on Kits — ArpeggiatorForDrum is out of scope for v1, so the pages are hidden there
	/// rather than shown and inert.
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return midiFxEditable(); }

	bool isToggle() override { return true; }
	bool shouldEnterSubmenu() override { return false; }
};

} // namespace deluge::gui::menu_item::midi_fx
