/*
 * Copyright © 2026 Rudement
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
#include "gui/menu_item/submenu.h"
#include "gui/menu_item/toggle.h"
#include "processing/engines/cpu_stats.h"

namespace deluge::gui::menu_item::cpu {

/// One CPU counter, read-only.
///
/// Shaped exactly like cv_output::Stat, for the same reasons, and deliberately so: the AUX stats
/// page is the thing this is a second instance of, and the two should stay recognisable as one
/// pattern. Shown modulo a million so the value fits the menu's own range.
///
/// Not gated on any Community Features toggle. There is no toggle to add -- which is the point.
/// `RuntimeFeatureSettingType` is the one place in this codebase where a missed edit is a hard CPU
/// fault rather than a blank line (the enum, the SetupOnOffSetting call, AND the subMenuEntries
/// array in runtime_feature/settings.cpp all have to move together), and a diagnostic page has no
/// business taking that risk to hide itself.
class Stat final : public Integer {
public:
	Stat(l10n::String newName, l10n::String title, deluge::processing::engines::CpuStat which)
	    : Integer(newName, title), which_(which) {}

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 999999; }

	void readCurrentValue() override {
		this->setValue(static_cast<int32_t>(deluge::processing::engines::cpuStat(which_)));
	}

	/// Read-only. The encoder will still move the number on screen -- Integer owns that behaviour
	/// -- but nothing is stored, and leaving the item and coming back re-reads the counter.
	void writeCurrentValue() override {}

private:
	const deluge::processing::engines::CpuStat which_;
};

/// SETTINGS > CPU > RESET. Zeroes every counter and peak.
///
/// A Toggle rather than an action item because Toggle already has the gesture: press select, it
/// calls readCurrentValue / setValue / writeCurrentValue in that order. readCurrentValue always
/// reports OFF, so the checkbox flicks on and is back off the next time it is drawn, which reads
/// as a button press rather than as a setting. Nothing persists.
///
/// This is not a nicety. Every counter on this page is lifetime-since-boot otherwise, so a peak
/// from the song you loaded twenty minutes ago sits at the top of the page forever and every A/B
/// comparison is contaminated by whatever you did before it.
class Reset final : public Toggle {
public:
	using Toggle::Toggle;

	void readCurrentValue() override { this->setValue(false); }

	void writeCurrentValue() override {
		if (this->getValue()) {
			deluge::processing::engines::cpuStatsReset();
		}
		this->setValue(false);
	}
};

/// SETTINGS > CPU.
class StatsSubmenu final : public Submenu {
public:
	using Submenu::Submenu;
};

} // namespace deluge::gui::menu_item::cpu
