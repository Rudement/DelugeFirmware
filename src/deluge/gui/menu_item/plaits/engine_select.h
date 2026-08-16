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
#include "gui/menu_item/menu_item.h"
#include "gui/menu_item/patched_param/integer.h"
#include "gui/menu_item/toggle.h"
#include "modulation/params/param.h"

namespace deluge::gui::menu_item {

/// Picks which of Plaits' 24 models a source renders.
///
/// A discrete choice rather than a patched param, so it is a bare MenuItem
/// driving Source::plaitsEngine -- modelled directly on DxEngineSelect, which
/// solves the same problem for the DX7 engine mode and is already proven on
/// both display types.
class PlaitsEngineSelect final : public MenuItem {
public:
	using MenuItem::MenuItem;
	PlaitsEngineSelect(l10n::String newName) : MenuItem(newName) {}

	void beginSession(MenuItem* navigatedBackwardFrom) override;
	void drawPixelsForOled() override;
	void readValueAgain() final;
	void selectEncoderAction(int32_t offset) final;
	void drawValue();

	int32_t currentValue = kPlaitsDefaultEngine;

	/// Index of the first model visible on the OLED list. drawItemsForOled only
	/// renders kOLEDMenuNumOptionsVisible rows and takes the selection as a ROW
	/// index, not an option index -- so a list longer than the screen needs this
	/// or the highlight walks off the bottom and the list never scrolls.
	/// DxEngineSelect has three items and gets away without one; 24 does not.
	int32_t scrollPos = 0;
};

extern PlaitsEngineSelect plaitsEngineSelect;

/// Swaps the source between Plaits' main and AUX outputs. Both are always
/// rendered -- Plaits computes them in the same pass -- so this costs nothing.
class PlaitsAux final : public Toggle {
public:
	using Toggle::Toggle;
	void readCurrentValue() override;
	void writeCurrentValue() override;
};

extern PlaitsAux plaitsAuxToggle;

/// Harmonics / Timbre / Morph, surfaced inside the Plaits menu.
///
/// These edit the SAME patched params the oscillator menus do -- this is a
/// second door onto one room, not a copy. Automation, MIDI CCs, grid pads and
/// the mod matrix are all unaffected, because nothing about the underlying
/// param changes.
///
/// They exist because the borrowed params are scattered: Harmonics and Timbre
/// sit in Osc 1's list among the sample entries, and Morph is over in Osc 2.
/// That is defensible internally and indefensible to use -- it took four
/// separate attempts to explain where they were. All four Plaits controls now
/// live behind PLTS, in a sensible order.
///
/// Bound to fixed param IDs rather than deriving them from
/// soundEditor.currentSourceIndex, because Morph is hardwired to OSC B's phase
/// width while the others are OSC A's. Plaits is source-0-only, so nothing is
/// lost by pinning them.
class PlaitsHalfPrecisionParam final : public patched_param::Integer {
public:
	using patched_param::Integer::Integer;
	/// Phase-width params are half-precision menu items: knob 0..50 maps to
	/// patched 0..INT32_MAX. Must match osc::PulseWidth exactly, or the same
	/// param would read differently depending on which door you came through.
	int32_t getFinalValue() override;
	void readCurrentValue() override;
};

extern PlaitsHalfPrecisionParam plaitsHarmonicsMenu;
extern patched_param::Integer plaitsTimbreMenu;
extern PlaitsHalfPrecisionParam plaitsMorphMenu;
} // namespace deluge::gui::menu_item
