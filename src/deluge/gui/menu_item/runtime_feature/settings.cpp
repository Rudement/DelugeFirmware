/*
 * Copyright © 2021-2023 Synthstrom Audible Limited
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

#include "settings.h"
#include "devSysexSetting.h"
#include "emulated_display.h"
#include "hid/display/display.h"
#include "hid/display/oled_canvas/canvas.h"
#include "io/midi/midi_follow.h"
#include "setting.h"
#include "shift_is_sticky.h"
#include <array>

extern deluge::gui::menu_item::runtime_feature::Setting runtimeFeatureSettingMenuItem;

namespace deluge::gui::menu_item::runtime_feature {

// A SettingToggle that is only relevant (and thus only shown) when an OLED display is
// active — real or emulated. Used for features that have no effect in 7-segment mode.
class OledOnlySettingToggle : public SettingToggle {
public:
	using SettingToggle::SettingToggle;
	bool isRelevant(ModControllableAudio*, int32_t) const override { return display->haveOLED(); }
};

// Only meaningful with a Visualizer mode chosen, so it hides rather than offering a dead switch.
class VisualizerPriorityToggle final : public SettingToggle {
public:
	using SettingToggle::SettingToggle;
	bool isRelevant(ModControllableAudio*, int32_t) const override {
		return display->haveOLED()
		       && runtimeFeatureSettings.get(RuntimeFeatureSettingType::Visualizer)
		              != RuntimeFeatureStateVisualizer::VisualizerOff;
	}
};

// A multi-value Setting that is only shown on an OLED, for the same reason as the toggle above.
class OledOnlySetting : public Setting {
public:
	using Setting::Setting;
	bool isRelevant(ModControllableAudio*, int32_t) const override { return display->haveOLED(); }
};

// The RoundedCorners toggle additionally pushes its value into the Canvas drawing flag,
// so the low-level primitives don't have to read runtime feature settings themselves.
class RoundedCornersSettingToggle final : public OledOnlySettingToggle {
public:
	using OledOnlySettingToggle::OledOnlySettingToggle;
	void writeCurrentValue() override {
		OledOnlySettingToggle::writeCurrentValue();
		hid::display::oled_canvas::Canvas::roundedCornersEnabled =
		    runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::RoundedCorners);
	}
};

// Only meaningful with Temp Hold itself on, so it hides otherwise rather than offering a dead switch.
class TempHoldTailsToggle final : public SettingToggle {
public:
	using SettingToggle::SettingToggle;
	bool isRelevant(ModControllableAudio*, int32_t) const override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::TempHold);
	}
};

// Turning Grid Follow Lock off drops any lock that is standing, so the grid pulse and MIDI Follow are back to
// stock the moment the switch changes, not at the next read.
class MidiFollowGridLockToggle final : public SettingToggle {
public:
	using SettingToggle::SettingToggle;
	void writeCurrentValue() override {
		SettingToggle::writeCurrentValue();
		if (!runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::MidiFollowGridLock)) {
			midiFollow.gridSelectedClip = nullptr;
		}
	}
};

// A SettingToggle withheld from the Community Features menu entirely, so the build cannot expose or
// re-enable the feature. The feature's code stays in the tree; change HiddenSettingToggle back to
// SettingToggle (and drop the forced Off in RuntimeFeatureSettings::readSettingsFromFile) to bring it back.
class HiddenSettingToggle final : public SettingToggle {
public:
	using SettingToggle::SettingToggle;
	bool isRelevant(ModControllableAudio*, int32_t) const override { return false; }
};

// Generic menu item instances
SettingToggle menuDrumRandomizer(RuntimeFeatureSettingType::DrumRandomizer);
SettingToggle menuFineTempo(RuntimeFeatureSettingType::FineTempoKnob);
SettingToggle menuQuantize(RuntimeFeatureSettingType::Quantize);
SettingToggle menuCatchNotes(RuntimeFeatureSettingType::CatchNotes);
SettingToggle menuDeleteUnusedKitRows(RuntimeFeatureSettingType::DeleteUnusedKitRows);
SettingToggle menuAltGoldenKnobDelayParams(RuntimeFeatureSettingType::AltGoldenKnobDelayParams);
Setting menuSyncScalingAction(RuntimeFeatureSettingType::SyncScalingAction);
DevSysexSetting menuDevSysexAllowed(RuntimeFeatureSettingType::DevSysexAllowed);
SettingToggle menuHighlightIncomingNotes(RuntimeFeatureSettingType::HighlightIncomingNotes);
SettingToggle menuDisplayNornsLayout(RuntimeFeatureSettingType::DisplayNornsLayout);
ShiftIsSticky menuShiftIsSticky{};
SettingToggle menuLightShiftLed(RuntimeFeatureSettingType::LightShiftLed);
SettingToggle menuEnableDX7Engine(RuntimeFeatureSettingType::EnableDX7Engine);
EmulatedDisplay menuEmulatedDisplay{};
SettingToggle menuEnableKeyboardViewSidebarMenuExit(RuntimeFeatureSettingType::EnableKeyboardViewSidebarMenuExit);
SettingToggle menuEnableLaunchEventPlayhead(RuntimeFeatureSettingType::EnableLaunchEventPlayhead);
SettingToggle menuDisplayChordLayout(RuntimeFeatureSettingType::DisplayChordKeyboard);
SettingToggle menuChordBrush(RuntimeFeatureSettingType::ChordBrush);
SettingToggle menuAlternativePlaybackStartBehaviour(RuntimeFeatureSettingType::AlternativePlaybackStartBehaviour);
SettingToggle menuEnableGridViewLoopPads(RuntimeFeatureSettingType::EnableGridViewLoopPads);
SettingToggle menuAlternativeTapTempoBehaviour(RuntimeFeatureSettingType::AlternativeTapTempoBehaviour);
OledOnlySettingToggle menuHorizontalMenus(RuntimeFeatureSettingType::HorizontalMenus);
SettingToggle menuTrimFromStartOfAudioClip(RuntimeFeatureSettingType::TrimFromStartOfAudioClip);
SettingToggle menuShowBatteryLevel(RuntimeFeatureSettingType::ShowBatteryLevel);
RoundedCornersSettingToggle menuRoundedCorners(RuntimeFeatureSettingType::RoundedCorners);
Setting menuShortcutOverlay(RuntimeFeatureSettingType::ShortcutOverlay);
SettingToggle menuKitSplit(RuntimeFeatureSettingType::KitSplit);
SettingToggle menuFourBandEq(RuntimeFeatureSettingType::FourBandEq);
SettingToggle menuEnablePlaitsEngine(RuntimeFeatureSettingType::EnablePlaitsEngine);
SettingToggle menuEnableCloudsFX(RuntimeFeatureSettingType::EnableCloudsFX);
SettingToggle menuEnableGristleizer(RuntimeFeatureSettingType::EnableGristleizer);
SettingToggle menuEnableSear(RuntimeFeatureSettingType::EnableSear);
SettingToggle menuKeyboardNotePreview(RuntimeFeatureSettingType::KeyboardNotePreview);
SettingToggle menuSceneCapture(RuntimeFeatureSettingType::SceneCapture);
SettingToggle menuGridColumnReorder(RuntimeFeatureSettingType::GridColumnReorder);
SettingToggle menuRetrospectiveCapture(RuntimeFeatureSettingType::RetrospectiveCapture);
HiddenSettingToggle menuEnableAuxSends(RuntimeFeatureSettingType::EnableAuxSends); // withheld 2026-10-22
HiddenSettingToggle menuAuxSelectPulse(RuntimeFeatureSettingType::AuxSelectPulse); // withheld 2026-10-22
SettingToggle menuTempHold(RuntimeFeatureSettingType::TempHold);
TempHoldTailsToggle menuTempHoldTails(RuntimeFeatureSettingType::TempHoldTails);
SettingToggle menuMidiFx(RuntimeFeatureSettingType::MidiFx);
SettingToggle menuHarmonize(RuntimeFeatureSettingType::Harmonize);
SettingToggle menuStrum(RuntimeFeatureSettingType::Strum);
SettingToggle menuDelayFilters(RuntimeFeatureSettingType::DelayFilters);
OledOnlySetting menuVisualizer(RuntimeFeatureSettingType::Visualizer);
VisualizerPriorityToggle menuVisualizerPriority(RuntimeFeatureSettingType::VisualizerPriority);
HiddenSettingToggle menuHoldLoopPad(RuntimeFeatureSettingType::HoldLoopPad); // withheld 2026-10-25: Hold to Loop
SettingToggle menuLoopRemix(RuntimeFeatureSettingType::LoopRemix);
MidiFollowGridLockToggle menuMidiFollowGridLock(RuntimeFeatureSettingType::MidiFollowGridLock);
SettingToggle menuReverbFilterHz(RuntimeFeatureSettingType::ReverbFilterHz);
SettingToggle menuGroove(RuntimeFeatureSettingType::Groove);

// The fork's own additions, gathered into one folder so the Community Features list stays short.
std::array<MenuItem*, 23> rudementUpgradesEntries{
    &menuKitSplit,        &menuFourBandEq,       &menuEnablePlaitsEngine, &menuEnableCloudsFX,
    &menuEnableGristleizer, &menuEnableSear,     &menuSceneCapture,       &menuGridColumnReorder,
    &menuEnableAuxSends,  &menuAuxSelectPulse,   &menuTempHold,           &menuTempHoldTails,
    &menuMidiFx,          &menuHarmonize,        &menuStrum,              &menuDelayFilters,
    &menuVisualizer,      &menuVisualizerPriority, &menuHoldLoopPad,      &menuLoopRemix,
    &menuMidiFollowGridLock, &menuReverbFilterHz, &menuGroove};
Submenu menuRudementUpgrades{l10n::String::STRING_FOR_RUDEMENT_UPGRADES, rudementUpgradesEntries};

// One entry per enum value, and the size comes FROM the enum - so adding a
// RuntimeFeatureSettingType without adding its pointer here leaves a null at the end of the
// array, and scrolling onto it is a hard fault rather than a missing menu line. If you add a
// feature toggle, you add it in three places: the enum, SetupOnOffSetting, and this list.
// The twenty-three entries in the Rudement upgrades folder above are counted by kNonTopLevelSettings (settings.h):
// moving a toggle in or out of the folder means changing that constant too.
std::array<MenuItem*, RuntimeFeatureSettingType::MaxElement - kNonTopLevelSettings> subMenuEntries{
    &menuDrumRandomizer,
    &menuFineTempo,
    &menuQuantize,
    &menuCatchNotes,
    &menuDeleteUnusedKitRows,
    &menuAltGoldenKnobDelayParams,
    &menuDevSysexAllowed,
    &menuSyncScalingAction,
    &menuHighlightIncomingNotes,
    &menuDisplayNornsLayout,
    &menuShiftIsSticky,
    &menuLightShiftLed,
    &menuEnableDX7Engine,
    &menuEmulatedDisplay,
    &menuEnableKeyboardViewSidebarMenuExit,
    &menuEnableLaunchEventPlayhead,
    &menuDisplayChordLayout,
    &menuChordBrush,
    &menuRetrospectiveCapture,
    &menuAlternativePlaybackStartBehaviour,
    &menuEnableGridViewLoopPads,
    &menuAlternativeTapTempoBehaviour,
    &menuHorizontalMenus,
    &menuRoundedCorners,
    &menuTrimFromStartOfAudioClip,
    &menuShowBatteryLevel,
    &menuShortcutOverlay,
    &menuKeyboardNotePreview,
    &menuRudementUpgrades};

Settings::Settings(l10n::String name, l10n::String title) : menu_item::Submenu(name, title, subMenuEntries) {
}

} // namespace deluge::gui::menu_item::runtime_feature
