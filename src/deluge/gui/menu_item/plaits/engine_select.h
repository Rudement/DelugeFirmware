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
#include "gui/menu_item/integer.h"
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

/// Engages Plaits' low-pass gate.
///
/// Off is the port's original behaviour and stays the default so existing
/// presets do not change under anyone. On is the more faithful setting: on the
/// module the LPG is in circuit unless you patch LEVEL, and it is doing most of
/// the work in every demo video.
class PlaitsLpg final : public Toggle {
public:
	using Toggle::Toggle;
	void readCurrentValue() override;
	void writeCurrentValue() override;
};

extern PlaitsLpg plaitsLpgToggle;

/// LPG decay and colour, 0..50.
///
/// Plain per-source integers rather than patched params -- these are settings
/// like the model, not per-note modulation targets, and Plaits is already
/// borrowing three of OSC A/B's params for Harmonics/Timbre/Morph. Note the
/// asymmetry: Decay reaches the eight self-enveloped engines even with the LPG
/// off (it is their envelope time, and it is what finally gives the three drums
/// a decay control), while Colour does nothing unless the LPG is on.
class PlaitsDecay final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override;
	void writeCurrentValue() override;
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 50; }
};

extern PlaitsDecay plaitsDecayMenu;

class PlaitsLpgColour final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override;
	void writeCurrentValue() override;
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 50; }
};

extern PlaitsLpgColour plaitsLpgColourMenu;

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
	/// Half-precision menu items: knob 0..50 maps to patched 0..INT32_MAX.
	/// Must match osc::PulseWidth exactly, or the same param would read
	/// differently depending on which door you came through.
	///
	/// All THREE of Harmonics, Timbre and Morph use this, including Timbre --
	/// which borrows a wave-index param, whose standard menu scaling is
	/// CENTRED (knob 25 == patched 0). Plaits wants 0.0 .. 1.0 and
	/// hybridParamToUnit() clamps negatives, so under standard scaling the
	/// whole bottom half of Timbre's travel arrived at the engine as zero:
	/// dead below 25, and half the resolution of the other two above it.
	/// Confirmed by ear on hardware before this was changed.
	///
	/// Half-precision scaling costs nothing in compatibility: the stored param
	/// value and the engine-side mapping are both untouched, so a saved sound
	/// plays back exactly as before -- only the number the menu shows for it
	/// changes. osc::source::WaveIndex applies the same scaling when the osc
	/// type is PLAITS, so both doors onto this param still agree.
	///
	/// NOT fixed by this: MIDI CC 25, the automation lane and the gold knob
	/// write the param directly across its full bipolar range, so their bottom
	/// halves still land on zero. Fixing those means remapping Timbre in
	/// plaits_adapter.cpp, which WOULD move every saved sound.
	int32_t getFinalValue() override;
	void readCurrentValue() override;
};

extern PlaitsHalfPrecisionParam plaitsHarmonicsMenu;
extern PlaitsHalfPrecisionParam plaitsTimbreMenu;
extern PlaitsHalfPrecisionParam plaitsMorphMenu;
} // namespace deluge::gui::menu_item
