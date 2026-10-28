/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
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

#include "gui/l10n/strings.h"
#include "util/container/array/resizeable_array.h"
#include "util/containers.h"
#include "util/d_string.h"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace deluge::gui::menu_item::runtime_feature {
class Setting;
class Settings;
class DevSysexSetting;

} // namespace deluge::gui::menu_item::runtime_feature

// State declarations
enum RuntimeFeatureStateToggle : uint32_t { Off = 0, On = 1 };

// Declare additional enums for specific multi state settings (e.g. like RuntimeFeatureStateTrackLaunchStyle)
enum RuntimeFeatureStateSyncScalingAction : uint32_t { SyncScaling = 0, Fill = 1 };

enum RuntimeFeatureStateEmulatedDisplay : uint32_t { Hardware = 0, Toggle = 1, OnBoot = 2 };

/// Which Visualizer is drawn. Off means the feature is inert: no capture, no menu item.
enum RuntimeFeatureStateVisualizer : uint32_t {
	VisualizerOff = 0,
	VisualizerWaveform = 1,
	VisualizerLineSpectrum = 2,
	VisualizerBarSpectrum = 3, // the Graphic EQ
	VisualizerCube = 4,
	VisualizerStereoLineSpectrum = 5,
	VisualizerStereoBarSpectrum = 6,
	VisualizerTunnel = 7,
	VisualizerStarfield = 8,
	VisualizerSkyline = 9,
	VisualizerPulseGrid = 10,
	VisualizerMidiPianoRoll = 11,
};
// Keep the former On value as MenuOnly to preserve existing settings.
enum class RuntimeFeatureStateShortcutOverlay : uint32_t { Off = 0, MenuOnly = 1, MenuAndClip = 2 };

/// Every setting needs to be declared in here
enum RuntimeFeatureSettingType : uint32_t {
	DrumRandomizer,
	Quantize,
	FineTempoKnob,
	CatchNotes,
	DeleteUnusedKitRows,
	AltGoldenKnobDelayParams,
	DevSysexAllowed,
	SyncScalingAction,
	HighlightIncomingNotes,
	DisplayNornsLayout,
	ShiftIsSticky,
	LightShiftLed,
	EnableDX7Engine,
	EmulatedDisplay,
	EnableKeyboardViewSidebarMenuExit,
	EnableLaunchEventPlayhead,
	DisplayChordKeyboard,
	AlternativePlaybackStartBehaviour,
	EnableGridViewLoopPads,
	AlternativeTapTempoBehaviour,
	HorizontalMenus,
	TrimFromStartOfAudioClip,
	ShowBatteryLevel,
	RoundedCorners,
	ShortcutOverlay,
	KitSplit,
	FourBandEq,
	EnablePlaitsEngine,
	EnableCloudsFX,
	EnableGristleizer,
	EnableSear,
	KeyboardNotePreview,
	SceneCapture,
	GridColumnReorder,
	ChordBrush,
	RetrospectiveCapture,
	EnableAuxSends,
	AuxSelectPulse,
	TempHold,
	TempHoldTails,
	MidiFx,
	Harmonize,
	Strum,
	DelayFilters,
	Visualizer,
	VisualizerPriority,
	HoldLoopPad,
	LoopRemix,
	MidiFollowGridLock,
	ReverbFilterHz,
	Groove,
	MaxElement // Keep as boundary
};

/// Definition for selectable options
struct RuntimeFeatureSettingOption {
	std::string_view displayName;
	uint32_t value; // Value to be defined as typed Enum above
};

/// Every setting keeps its metadata and value in here
struct RuntimeFeatureSetting {
	deluge::l10n::String displayName;
	std::string_view xmlName;
	uint32_t value;
	// Limited to safe memory
	deluge::vector<RuntimeFeatureSettingOption> options;
};

/// A setting read from the file that this firmware does not know - most likely one written by a newer or a
/// different firmware. It is kept so writeSettingsToFile() can write it back, and the other firmware does not lose it.
struct UnknownSetting {
	std::string name;
	uint32_t value;
};

class Serializer;
class Deserializer;

/// Encapsulating class
class RuntimeFeatureSettings {
public:
	RuntimeFeatureSettings();

	// Traded type safety for option values for code simplicity and size, use enum from above to compare
	inline uint32_t get(RuntimeFeatureSettingType type) { return settings[type].value; };
	inline bool isOn(RuntimeFeatureSettingType type) { return get(type) == RuntimeFeatureStateToggle::On; }

	/**
	 * Set a runtime feature setting.
	 *
	 * Make sure when you use this the settings are eventually written back to the SDCard!
	 */
	inline void set(RuntimeFeatureSettingType type, uint32_t value) { settings[type].value = value; }

	inline const char* getStartupSong() { return startupSong.get(); }
	void init();
	void factoryReset(bool showPopup = true);
	void readSettingsFromFile();
	void writeSettingsToFile();

protected:
	std::array<RuntimeFeatureSetting, RuntimeFeatureSettingType::MaxElement> settings = {};
	String startupSong;

private:
	// Rudement: this was a ResizeableArray, which moves its elements with memmove when it grows and never runs their
	// constructors or destructors. A short std::string keeps its text inside itself and points at it, so after a move
	// that pointer aimed at the old copy and the name came back as garbage (seen on a card as raw bytes
	// AC A8 17 20 02); the strings were never freed either. A vector moves them properly.
	deluge::vector<UnknownSetting> unknownSettings;

public:
	friend class deluge::gui::menu_item::runtime_feature::Setting;
	friend class deluge::gui::menu_item::runtime_feature::Settings;
	friend class deluge::gui::menu_item::runtime_feature::DevSysexSetting;
};

/// Static instance for external access
extern RuntimeFeatureSettings runtimeFeatureSettings;
