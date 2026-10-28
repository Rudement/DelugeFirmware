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

#include "runtime_feature_settings.h"
#include "gui/l10n/l10n.h"
#include "hid/display/display.h"
#include "model/song/song.h"
#include "storage/storage_manager.h"
#include "util/d_string.h"
#include <cstring>
#include <string_view>

#define SETTINGS_FOLDER "SETTINGS"
#define RUNTIME_FEATURE_SETTINGS_FILE "SETTINGS/CommunityFeatures.XML"
#define TAG_RUNTIME_FEATURE_SETTINGS "runtimeFeatureSettings"
#define TAG_RUNTIME_FEATURE_SETTING "setting"
#define TAG_RUNTIME_FEATURE_SETTING_ATTR_NAME "name"
#define TAG_RUNTIME_FEATURE_SETTING_ATTR_VALUE "value"

RuntimeFeatureSettings runtimeFeatureSettings{};

RuntimeFeatureSettings::RuntimeFeatureSettings() = default;

/// True for a non-empty name made only of printable ASCII, which every real setting name is.
static bool isPrintableName(const char* name) {
	if (name == nullptr || *name == '\0') {
		return false;
	}
	for (const char* p = name; *p != '\0'; ++p) {
		const auto ch = static_cast<unsigned char>(*p);
		if (ch < 0x20 || ch > 0x7E) {
			return false;
		}
	}
	return true;
}

static void SetupOnOffSetting(RuntimeFeatureSetting& setting, deluge::l10n::String displayName,
                              std::string_view xmlName, RuntimeFeatureStateToggle def) {
	setting.displayName = displayName;
	setting.xmlName = xmlName;
	setting.value = static_cast<uint32_t>(def);

	setting.options = {
	    {
	        .displayName = "Off",
	        .value = RuntimeFeatureStateToggle::Off,
	    },
	    {
	        .displayName = "On",
	        .value = RuntimeFeatureStateToggle::On,
	    },
	};
}

static void SetupSyncScalingActionSetting(RuntimeFeatureSetting& setting, deluge::l10n::String displayName,
                                          std::string_view xmlName, RuntimeFeatureStateSyncScalingAction def) {
	setting.displayName = displayName;
	setting.xmlName = xmlName;
	setting.value = static_cast<uint32_t>(def);

	setting.options = {
	    {
	        .displayName = display->haveOLED() ? "Sync Scaling" : "SCAL",
	        .value = RuntimeFeatureStateSyncScalingAction::SyncScaling,
	    },
	    {
	        .displayName = display->haveOLED() ? "Fill mode" : "FILL",
	        .value = RuntimeFeatureStateSyncScalingAction::Fill,
	    },
	};
}

static void SetupEmulatedDisplaySetting(RuntimeFeatureSetting& setting, deluge::l10n::String displayName,
                                        std::string_view xmlName, RuntimeFeatureStateEmulatedDisplay def) {
	setting.displayName = displayName;
	setting.xmlName = xmlName;
	setting.value = static_cast<uint32_t>(def);

	// what is displayed depends on the physical display type more the active mode
	bool have_oled = deluge::hid::display::have_oled_screen;
	setting.options = {
	    {
	        .displayName = have_oled ? "OLED" : "7SEG",
	        .value = RuntimeFeatureStateEmulatedDisplay::Hardware,
	    },
	    {
	        .displayName = display->haveOLED() ? "Toggle" : "TOGL",
	        .value = RuntimeFeatureStateEmulatedDisplay::Toggle,
	    },
	    {
	        .displayName = have_oled ? "7SEG" : "OLED",
	        .value = RuntimeFeatureStateEmulatedDisplay::OnBoot,
	    },
	};
}

/// The Visualizer is the one Rudement feature that is not a switch: the value picks which of the
/// eleven modes is drawn, and Off makes the whole feature inert.
static void SetupVisualizerSetting(RuntimeFeatureSetting& setting, deluge::l10n::String displayName,
                                   std::string_view xmlName, RuntimeFeatureStateVisualizer def) {
	setting.displayName = displayName;
	setting.xmlName = xmlName;
	setting.value = static_cast<uint32_t>(def);

	setting.options = {
	    {.displayName = "OFF", .value = RuntimeFeatureStateVisualizer::VisualizerOff},
	    {.displayName = "WAVEFORM", .value = RuntimeFeatureStateVisualizer::VisualizerWaveform},
	    {.displayName = "LINE SPECTRUM", .value = RuntimeFeatureStateVisualizer::VisualizerLineSpectrum},
	    {.displayName = "GRAPHIC EQ", .value = RuntimeFeatureStateVisualizer::VisualizerBarSpectrum},
	    {.displayName = "CUBE", .value = RuntimeFeatureStateVisualizer::VisualizerCube},
	    {.displayName = "STEREO SPECTRUM", .value = RuntimeFeatureStateVisualizer::VisualizerStereoLineSpectrum},
	    {.displayName = "STEREO EQ", .value = RuntimeFeatureStateVisualizer::VisualizerStereoBarSpectrum},
	    {.displayName = "TUNNEL", .value = RuntimeFeatureStateVisualizer::VisualizerTunnel},
	    {.displayName = "STARFIELD", .value = RuntimeFeatureStateVisualizer::VisualizerStarfield},
	    {.displayName = "SKYLINE", .value = RuntimeFeatureStateVisualizer::VisualizerSkyline},
	    {.displayName = "PULSE GRID", .value = RuntimeFeatureStateVisualizer::VisualizerPulseGrid},
	    {.displayName = "PIANO ROLL", .value = RuntimeFeatureStateVisualizer::VisualizerMidiPianoRoll},
	};
}

void RuntimeFeatureSettings::init() {
	using enum deluge::l10n::String;
	// Drum randomizer
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::DrumRandomizer], STRING_FOR_COMMUNITY_FEATURE_DRUM_RANDOMIZER,
	                  "drumRandomizer", RuntimeFeatureStateToggle::On);
	// Quantize
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::Quantize], STRING_FOR_COMMUNITY_FEATURE_QUANTIZE, "quantize",
	                  RuntimeFeatureStateToggle::On);
	// FineTempoKnob
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::FineTempoKnob], STRING_FOR_COMMUNITY_FEATURE_FINE_TEMPO_KNOB,
	                  "fineTempoKnob", RuntimeFeatureStateToggle::On);
	// CatchNotes
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::CatchNotes], STRING_FOR_COMMUNITY_FEATURE_CATCH_NOTES,
	                  "catchNotes", RuntimeFeatureStateToggle::On);
	// DeleteUnusedKitRows
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::DeleteUnusedKitRows],
	                  STRING_FOR_COMMUNITY_FEATURE_DELETE_UNUSED_KIT_ROWS, "deleteUnusedKitRows",
	                  RuntimeFeatureStateToggle::On);
	// AltGoldenKnobDelayParams
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::AltGoldenKnobDelayParams],
	                  STRING_FOR_COMMUNITY_FEATURE_ALT_DELAY_PARAMS, "altGoldenKnobDelayParams",
	                  RuntimeFeatureStateToggle::Off);
	// devSysexAllowed
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::DevSysexAllowed], STRING_FOR_COMMUNITY_FEATURE_DEV_SYSEX,
	                  "devSysexAllowed", RuntimeFeatureStateToggle::Off);
	// SyncScalingAction
	SetupSyncScalingActionSetting(settings[RuntimeFeatureSettingType::SyncScalingAction],
	                              STRING_FOR_COMMUNITY_FEATURE_SYNC_SCALING_ACTION, "syncScalingAction",
	                              RuntimeFeatureStateSyncScalingAction::SyncScaling);
	// HighlightIncomingNotes
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::HighlightIncomingNotes],
	                  STRING_FOR_COMMUNITY_FEATURE_HIGHLIGHT_INCOMING_NOTES, "highlightIncomingNotes",
	                  RuntimeFeatureStateToggle::On);
	// DisplayNornsLayout
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::DisplayNornsLayout],
	                  STRING_FOR_COMMUNITY_FEATURE_NORNS_LAYOUT, "displayNornsLayout", RuntimeFeatureStateToggle::Off);

	// ShiftIsSticky
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::ShiftIsSticky], STRING_FOR_COMMUNITY_FEATURE_STICKY_SHIFT,
	                  "stickyShift", RuntimeFeatureStateToggle::Off);

	// LightShiftLed
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::LightShiftLed], STRING_FOR_COMMUNITY_FEATURE_LIGHT_SHIFT,
	                  "lightShift", RuntimeFeatureStateToggle::Off);

	// EnableDX7Engine
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::EnableDX7Engine], STRING_FOR_COMMUNITY_FEATURE_DX7_ENGINE,
	                  "EnableDX7Engine", RuntimeFeatureStateToggle::Off);

	// EmulatedDisplay
	SetupEmulatedDisplaySetting(settings[RuntimeFeatureSettingType::EmulatedDisplay],
	                            STRING_FOR_COMMUNITY_FEATURE_EMULATED_DISPLAY, "emulatedDisplay",
	                            RuntimeFeatureStateEmulatedDisplay::Hardware);

	// EnableKeyboardViewSidebarMenuExit
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::EnableKeyboardViewSidebarMenuExit],
	                  STRING_FOR_COMMUNITY_FEATURE_KEYBOARD_VIEW_SIDEBAR_MENU_EXIT, "enableKeyboardViewSidebarMenuExit",
	                  RuntimeFeatureStateToggle::Off);

	// EnableLaunchEventPlayhead
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::EnableLaunchEventPlayhead],
	                  STRING_FOR_COMMUNITY_FEATURE_LAUNCH_EVENT_PLAYHEAD, "enableLaunchEventPlayhead",
	                  RuntimeFeatureStateToggle::On);

	// DisplayChordKeyboard
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::DisplayChordKeyboard],
	                  STRING_FOR_COMMUNITY_FEATURE_CHORD_KEYBOARD, "displayChordKeyboard",
	                  RuntimeFeatureStateToggle::Off);

	// ChordBrush
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::ChordBrush], STRING_FOR_COMMUNITY_FEATURE_CHORD_BRUSH,
	                  "chordBrush", RuntimeFeatureStateToggle::Off);

	// KeyboardNotePreview — light up played notes on the keyboard grid (incl. during playback)
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::KeyboardNotePreview],
	                  STRING_FOR_COMMUNITY_FEATURE_KEYBOARD_NOTE_PREVIEW, "keyboardNotePreview",
	                  RuntimeFeatureStateToggle::On);

	// RetrospectiveCapture — background-buffer notes played in a keyboard layout (even when not
	// recording); SHIFT+RECORD dumps them into the clip. "Never lose a noodled idea."
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::RetrospectiveCapture],
	                  STRING_FOR_COMMUNITY_FEATURE_RETROSPECTIVE_CAPTURE, "retrospectiveCapture",
	                  RuntimeFeatureStateToggle::Off);

	// AlternativePlaybackStartBehaviour
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::AlternativePlaybackStartBehaviour],
	                  STRING_FOR_COMMUNITY_FEATURE_ALTERNATIVE_PLAYBACK_START_BEHAVIOUR,
	                  "alternativePlaybackStartBehaviour", RuntimeFeatureStateToggle::Off);

	// EnableGridViewLoopPads
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::EnableGridViewLoopPads],
	                  STRING_FOR_COMMUNITY_FEATURE_GRID_VIEW_LOOP_PADS, "enableGridViewLoopPads",
	                  RuntimeFeatureStateToggle::Off);

	// AlternativeTapTempoBehaviour
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::AlternativeTapTempoBehaviour],
	                  STRING_FOR_COMMUNITY_FEATURE_ALTERNATIVE_TAP_TEMPO_BEHAVIOUR, "alternativeTapTempoBehaviour",
	                  RuntimeFeatureStateToggle::Off);

	// Horizontal menus
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::HorizontalMenus],
	                  STRING_FOR_COMMUNITY_FEATURE_HORIZONTAL_MENUS, "enableHorizontalMenus",
	                  RuntimeFeatureStateToggle::On);

	// Trim from start of audio clip
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::TrimFromStartOfAudioClip],
	                  STRING_FOR_COMMUNITY_FEATURE_TRIM_FROM_START_OF_AUDIO_CLIP, "trimFromStartOfAudioClip",
	                  RuntimeFeatureStateToggle::On);

	// Show Battery Level
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::ShowBatteryLevel],
	                  STRING_FOR_COMMUNITY_FEATURE_SHOW_BATTERY_LEVEL, "showBatteryLevel",
	                  RuntimeFeatureStateToggle::On);

	// Rounded Corners
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::RoundedCorners], STRING_FOR_COMMUNITY_FEATURE_ROUNDED_CORNERS,
	                  "roundedCorners", RuntimeFeatureStateToggle::On);

	// Shortcut Overlay
	auto& overlay = settings[RuntimeFeatureSettingType::ShortcutOverlay];
	overlay.displayName = STRING_FOR_COMMUNITY_FEATURE_SHORTCUT_OVERLAY;
	overlay.xmlName = "shortcutOverlay";
	overlay.value = static_cast<uint32_t>(RuntimeFeatureStateShortcutOverlay::MenuOnly);
	overlay.options = {
	    {l10n::getView(STRING_FOR_OFF), static_cast<uint32_t>(RuntimeFeatureStateShortcutOverlay::Off)},
	    {l10n::getView(STRING_FOR_SHORTCUT_OVERLAY_MENU_ONLY),
	     static_cast<uint32_t>(RuntimeFeatureStateShortcutOverlay::MenuOnly)},
	    {l10n::getView(STRING_FOR_SHORTCUT_OVERLAY_MENU_AND_CLIP),
	     static_cast<uint32_t>(RuntimeFeatureStateShortcutOverlay::MenuAndClip)},
	};
	// Kit Split
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::KitSplit], STRING_FOR_COMMUNITY_FEATURE_KIT_SPLIT, "kitSplit",
	                  RuntimeFeatureStateToggle::Off);

	// The Rudement additions. On by default - the toggle is there to take them away, not to opt in.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::FourBandEq], STRING_FOR_COMMUNITY_FEATURE_FOUR_BAND_EQ,
	                  "fourBandEq", RuntimeFeatureStateToggle::On);
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::EnablePlaitsEngine], STRING_FOR_COMMUNITY_FEATURE_PLAITS,
	                  "enablePlaitsEngine", RuntimeFeatureStateToggle::On);
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::EnableCloudsFX], STRING_FOR_COMMUNITY_FEATURE_CLOUDS,
	                  "enableCloudsFX", RuntimeFeatureStateToggle::On);
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::EnableGristleizer], STRING_FOR_COMMUNITY_FEATURE_GRISTLEIZER,
	                  "enableGristleizer", RuntimeFeatureStateToggle::On);
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::EnableSear], STRING_FOR_COMMUNITY_FEATURE_SEAR, "enableSear",
	                  RuntimeFeatureStateToggle::On);

	// SceneCapture -- SAVE + a grid section pad copies the currently sounding clips into a new,
	// muted row below the populated ones, recalled later by that row's section pad.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::SceneCapture], STRING_FOR_COMMUNITY_FEATURE_SCENE_CAPTURE,
	                  "sceneCapture", RuntimeFeatureStateToggle::Off);

	// GridColumnReorder -- holding a clip pad in grid view and turning the horizontal encoder drags
	// that whole column sideways, reordering the tracks (and so the arranger's rows).
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::GridColumnReorder],
	                  STRING_FOR_COMMUNITY_FEATURE_GRID_COLUMN_REORDER, "gridColumnReorder",
	                  RuntimeFeatureStateToggle::Off);
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::EnableAuxSends], STRING_FOR_COMMUNITY_FEATURE_AUX_SENDS,
	                  "enableAuxSends", RuntimeFeatureStateToggle::Off); // withheld 2026-10-22: was On

	// AuxSelectPulse -- makes the converter's chip-select pulse once per word instead of staying
	// asserted while the stream runs. Off by default, and deliberately: the converter needs that
	// edge to latch a sample, so with this off the sends are silent, but turning it on has so far
	// destabilised the machine on the load page. Off is a working instrument that makes no sound;
	// on is the thing being tested. Kept as a toggle so testing it costs a menu entry rather than
	// a reflash.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::AuxSelectPulse], STRING_FOR_COMMUNITY_FEATURE_AUX_PULSE,
	                  "auxSelectPulse", RuntimeFeatureStateToggle::Off);

	// TempHold -- hold SHIFT, change anything, and it all goes back when you let go. Off by default,
	// and that is not timidity: SHIFT is held constantly for other reasons, so turning every held
	// SHIFT into a hold that reverts would change what the instrument does under people who never
	// asked for it. Opt in, then it is the most useful button on the machine.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::TempHold], STRING_FOR_COMMUNITY_FEATURE_TEMP_HOLD, "tempHold",
	                  RuntimeFeatureStateToggle::Off);

	// TempHoldTails -- when a hold ends, let a delay that was turned up during it ring out instead of
	// stopping dead. On by default: the cut was reported as a problem, and with it on nothing changes
	// unless a hold actually touched a delay. See Delay::beginTail().
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::TempHoldTails], STRING_FOR_COMMUNITY_FEATURE_TEMP_HOLD_TAILS,
	                  "tempHoldTails", RuntimeFeatureStateToggle::On);

	// MidiFx -- the whole MIDI FX group, in place of the per-sound Enabled switch it used to have.
	// That switch was redundant: every effect is neutral at its default, each page has a Reset and
	// the group has Reset all, so "off" for one sound is just "at default". What is left to switch is
	// whether the feature is on the machine at all, and that belongs here with the other Rudement
	// additions. On by default, like them. Off hides the menus; like every Gated feature it hides the
	// door, not the room - a song that uses MIDI FX still plays the same.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::MidiFx], STRING_FOR_COMMUNITY_FEATURE_MIDI_FX, "midiFx",
	                  RuntimeFeatureStateToggle::On);

	// Harmonize -- a clip plays its written phrase as degrees of the chord track's chord. Off by
	// default like every other note effect here, and a song with no chord track set does nothing
	// even with this on, so turning it on cannot change how an existing song sounds.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::Harmonize], STRING_FOR_COMMUNITY_FEATURE_HARMONIZE,
	                  "harmonize", RuntimeFeatureStateToggle::Off);

	// Strum -- a chord lands as a sweep rather than as a block. Off by default like every other note
	// effect here, and a clip with Strum Amount at 0 does nothing even with this on, so turning it
	// on cannot change how an existing song sounds.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::Strum], STRING_FOR_COMMUNITY_FEATURE_STRUM, "strum",
	                  RuntimeFeatureStateToggle::Off);

	// LoopRemix -- the clip's loop is chopped into slices and read out of order. A transform on where the
	// phrase is read, not a change to it, so it ships Off and Off plays every clip exactly as written. A
	// song saved with a remix switched on keeps its values and gets them back the moment this is turned On.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::LoopRemix], STRING_FOR_COMMUNITY_FEATURE_LOOP_REMIX,
	                  "loopRemix", RuntimeFeatureStateToggle::Off);

	// DelayFilters -- the delay's own HPF and LPF, inside the feedback loop. It changes a stock sound path, so it is a
	// switch, and it ships Off: Off hides the two menu items AND skips the filters in Delay::process(), which leaves
	// the delay exactly as the original firmware has it. Values a song saved are kept either way, and come back
	// into play the moment the switch is turned On.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::DelayFilters], STRING_FOR_COMMUNITY_FEATURE_DELAY_FILTERS,
	                  "delayFilters", RuntimeFeatureStateToggle::Off);

	// Visualizer -- eleven ways of drawing what is coming out of the machine, on the OLED. It is a
	// meter: it changes no audio and saves nothing but this choice. Off skips the capture in
	// AudioEngine::renderAudio as well as hiding the menu item.
	SetupVisualizerSetting(settings[RuntimeFeatureSettingType::Visualizer], STRING_FOR_COMMUNITY_FEATURE_VISUALIZER,
	                       "visualizer", RuntimeFeatureStateVisualizer::VisualizerOff);

	// VisualizerPriority -- who wins the screen. Off: popups and the launch countdown win and
	// the Visualizer stands down, which is the sane default. On: the Visualizer wins and the
	// interruptions are not drawn at all, for watching a sweep rather than reading it.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::VisualizerPriority],
	                  STRING_FOR_COMMUNITY_FEATURE_VISUALIZER_PRIORITY, "visualizerPriority",
	                  RuntimeFeatureStateToggle::Off);

	// HoldLoopPad -- the grid view's red loop pad becomes hold-to-loop: hold to record, let go and it plays, the same
	// as the Loop (hold) MIDI command. It changes what a stock pad does, so it is a switch and ships Off; Off leaves
	// the pad as the stock press-to-toggle LOOP.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::HoldLoopPad], STRING_FOR_COMMUNITY_FEATURE_HOLD_LOOP_PAD,
	                  "holdLoopPad", RuntimeFeatureStateToggle::Off);

	// MidiFollowGridLock -- in Session grid view (Launch mode), a grid pad held past Hold Time and let go inside two
	// seconds locks MIDI Follow onto that clip, and the lock stays put while other clips are launched and other views
	// are opened. Hold Time is 100 ms by default, so a slightly slow launch tap is enough to claim it, and from then on
	// the controller ignores whatever is on screen -- which reads as a broken machine to anyone who has not read the
	// manual. So it is a switch and ships Off; Off is stock MIDI Follow: whatever pad is down, or the clip on screen.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::MidiFollowGridLock],
	                  STRING_FOR_COMMUNITY_FEATURE_MIDI_FOLLOW_GRID_LOCK, "midiFollowGridLock",
	                  RuntimeFeatureStateToggle::Off);

	// ReverbFilterHz -- the song reverb's HPF and LPF read in Hz instead of 0-50, the way the delay filters
	// already do. Only the label changes: the stored value, the 0-50 knob and the sound are exactly stock.
	// Off by default so the stock menu is untouched unless someone asks for it.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::ReverbFilterHz], STRING_FOR_COMMUNITY_FEATURE_REVERB_FILTER_HZ,
	                  "reverbFilterHz", RuntimeFeatureStateToggle::Off);

	// Groove -- OXI One / Ableton style groove templates: each clip (or kit row) plays through a bent clock
	// that pushes steps early or late and lifts or drops their velocity. A transform on how the phrase is
	// read, never a change to the notes, so it ships Off like Loop Remix and Off plays every clip exactly
	// as written. A song saved with grooves keeps them and gets them back when this is turned On.
	SetupOnOffSetting(settings[RuntimeFeatureSettingType::Groove], STRING_FOR_COMMUNITY_FEATURE_GROOVE, "groove",
	                  RuntimeFeatureStateToggle::Off);
}

void RuntimeFeatureSettings::factoryReset(bool showPopup) {
	if (showPopup) {
		display->displayPopup(display->haveOLED() ? l10n::get(l10n::String::STRING_FOR_RESET_COMMUNITY_FEATURES)
		                                          : l10n::get(l10n::String::STRING_FOR_FACTORY_RESET));
	}

	f_unlink(RUNTIME_FEATURE_SETTINGS_FILE);
	unknownSettings.clear();
	startupSong.clear();
	init();
	readSettingsFromFile();
}

void RuntimeFeatureSettings::readSettingsFromFile() {
	FilePointer fp;
	// CommunityFeatures.XML
	bool success = StorageManager::fileExists(RUNTIME_FEATURE_SETTINGS_FILE, &fp);
	if (!success) {
		// since we changed the file path for the CommunityFeatures.XML in c1.3, it's possible
		// that a CommunityFeatures file may exists in the root of the SD card
		// if so, let's move it to the new SETTINGS folder (but first make sure folder exists)
		FRESULT result = f_mkdir(SETTINGS_FOLDER);
		if (result == FR_OK || result == FR_EXIST) {
			result = f_rename("CommunityFeatures.XML", RUNTIME_FEATURE_SETTINGS_FILE);
			if (result == FR_OK) {
				// this means we moved it
				// now let's open it
				success = StorageManager::fileExists(RUNTIME_FEATURE_SETTINGS_FILE, &fp);
			}
		}
		if (!success) {
			return;
		}
	}

	Error error = StorageManager::openXMLFile(&fp, smDeserializer, TAG_RUNTIME_FEATURE_SETTINGS);
	if (error != Error::NONE) {
		return;
	}
	Deserializer& reader = *activeDeserializer;
	String currentName;
	int32_t currentValue = 0;
	char const* currentTag = nullptr;

	while (*(currentTag = reader.readNextTagOrAttributeName())) {

		if (strcmp(currentTag, "startupSong") == 0) {
			reader.readTagOrAttributeValueString(&startupSong);
		}
		if (strcmp(currentTag, TAG_RUNTIME_FEATURE_SETTING) == 0) {
			// Read name
			currentTag = reader.readNextTagOrAttributeName();
			if (strcmp(currentTag, TAG_RUNTIME_FEATURE_SETTING_ATTR_NAME) != 0) {
				display->displayPopup("Community file err");
				break;
			}
			reader.readTagOrAttributeValueString(&currentName);
			reader.exitTag();

			// Read value
			currentTag = reader.readNextTagOrAttributeName();
			if (strcmp(currentTag, TAG_RUNTIME_FEATURE_SETTING_ATTR_VALUE) != 0) {
				display->displayPopup("Community file err");
				break;
			}

			currentValue = reader.readTagOrAttributeValueInt();
			reader.exitTag();

			bool found = false;
			for (auto& setting : settings) {
				if (strcmp(setting.xmlName.data(), currentName.get()) == 0) {
					found = true;
					setting.value = currentValue;
				}
			}

			// Remember unknown settings for writing them back - but only ones with a real name. A name that is empty
			// or not plain printable text is damage from the old container (see the header), not a setting another
			// firmware wrote, so dropping it here is what cleans an affected card on its next save.
			if (!found && isPrintableName(currentName.get())) {
				unknownSettings.push_back({currentName.get(), static_cast<uint32_t>(currentValue)});
			}
		}
		reader.exitTag(currentTag);
	}
	smDeserializer.closeWriter();

	// AUX Sends and AUX Select are withheld (hidden switch, code kept in the tree). A card that ran an
	// earlier build has enableAuxSends="1" saved in CommunityFeatures.XML, and with the switch hidden there
	// would be no way to turn it back off, so whatever the file says, both stay Off. To bring the feature
	// back, delete these two lines and un-hide the switches in gui/menu_item/runtime_feature/settings.cpp.
	settings[RuntimeFeatureSettingType::EnableAuxSends].value = RuntimeFeatureStateToggle::Off;
	settings[RuntimeFeatureSettingType::AuxSelectPulse].value = RuntimeFeatureStateToggle::Off;
	// Hold to Loop is withheld the same way (hidden switch, code kept). A card that had Hold Loop Pad On stays Off.
	settings[RuntimeFeatureSettingType::HoldLoopPad].value = RuntimeFeatureStateToggle::Off;
}

void RuntimeFeatureSettings::writeSettingsToFile() {
	f_unlink(RUNTIME_FEATURE_SETTINGS_FILE); // May give error, but no real consequence from that.

	Error error = StorageManager::createXMLFile(RUNTIME_FEATURE_SETTINGS_FILE, smSerializer, true);
	if (error != Error::NONE) {
		return;
	}
	Serializer& writer = GetSerializer();
	writer.writeOpeningTagBeginning(TAG_RUNTIME_FEATURE_SETTINGS);
	writer.writeFirmwareVersion();
	writer.writeEarliestCompatibleFirmwareVersion("4.1.3");
	writer.writeAttribute("startupSong", currentSong->getSongFullPath().get());
	writer.writeOpeningTagEnd();

	for (auto& setting : settings) {
		writer.writeOpeningTagBeginning(TAG_RUNTIME_FEATURE_SETTING);
		writer.writeAttribute(TAG_RUNTIME_FEATURE_SETTING_ATTR_NAME, setting.xmlName.data(), false);
		writer.writeAttribute(TAG_RUNTIME_FEATURE_SETTING_ATTR_VALUE, setting.value, false);
		writer.writeOpeningTagEnd(false);
		writer.writeClosingTag(TAG_RUNTIME_FEATURE_SETTING, false);
	}

	// Write unknown elements
	for (const UnknownSetting& unknownSetting : unknownSettings) {
		writer.writeOpeningTagBeginning(TAG_RUNTIME_FEATURE_SETTING);
		writer.writeAttribute(TAG_RUNTIME_FEATURE_SETTING_ATTR_NAME, unknownSetting.name.c_str(), false);
		writer.writeAttribute(TAG_RUNTIME_FEATURE_SETTING_ATTR_VALUE, unknownSetting.value, false);
		writer.writeOpeningTagEnd(false);
		writer.writeClosingTag(TAG_RUNTIME_FEATURE_SETTING, false);
	}

	writer.writeClosingTag(TAG_RUNTIME_FEATURE_SETTINGS);
	writer.closeFileAfterWriting();
}
