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
#include "gui/menu_item/midi_fx/target.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "model/output.h"
#include "model/song/song.h"
#include "modulation/midi_fx/chord_source.h"
#include <cstring>

/// NOTE: there is also a `deluge::modulation::midi_fx` (the effects themselves). Any file with
/// using-directives for both `deluge::modulation` and `deluge::gui::menu_item` - `menus.cpp` is
/// one - must qualify as `menu_item::midi_fx::` or the name is ambiguous.
namespace deluge::gui::menu_item::midi_fx {

/// Harmonize is per clip, but only where a pitch means something.
///
/// On a Kit the row's y is a drum index rather than a note, so harmonising it would scramble which
/// drum plays rather than re-pitching anything. The page hides itself there instead of showing a
/// switch that does nothing - the OXI leaves drums out of HARMO for the same reason.
[[nodiscard]] inline bool harmonizeEditable() {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr || clip->output == nullptr) {
		return false;
	}
	if (clip->output->type == OutputType::KIT || clip->output->type == OutputType::AUDIO) {
		return false;
	}
	return midiFxEditable();
}

/// Follow Chords: this clip plays its written phrase as degrees of the chord track's chord.
class HarmonizeFollow final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override {
		const auto* s = midiFxSettingsForRead();
		this->setValue(s != nullptr && s->harmonize);
	}

	void writeCurrentValue() override {
		// getValue<E>() is constrained to `util::enumeration`, so getValue<bool>() does not compile -
		// comparing Value<int32_t>::getValue() against 0 is what every other toggle here does.
		const bool on = this->getValue() != 0;
		midiFxApply([on](deluge::modulation::midi_fx::MidiFxSettings& s) { s.harmonize = on; });
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {
		    l10n::getView(STRING_FOR_OFF), //<
		    l10n::getView(STRING_FOR_ON),  //<
		};
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return harmonizeEditable(); }

	bool isToggle() override { return true; }
	bool shouldEnterSubmenu() override { return false; }
};

/// Chord Track: which track supplies the chords. **Song-wide**, not per clip.
///
/// One source for the song is the whole point - you set it once and switch Follow on per clip. Two
/// progressions at once would need this per clip, which is a menu on every clip to maintain for a
/// case that has not come up yet.
///
/// The options are built from the song's outputs each time the list is drawn, because tracks get
/// added, renamed and deleted while this menu is one button press away. Names are copied into this
/// object rather than pointed at: a `string_view` into an Output that gets deleted mid-menu is a
/// crash, and copying a couple of dozen short names costs nothing next to drawing the screen.
///
/// The *stored* setting is the track's name, not its index or its pointer - an index breaks the
/// moment a track above it is deleted, and a pointer dangles. The cost is that renaming the chord
/// track detaches it, which is visible here and one press to fix.
class ChordTrack final : public Selection {
public:
	using Selection::Selection;

	static constexpr int32_t kMaxTracks = 24;
	static constexpr int32_t kNameChars = 16;

	void readCurrentValue() override {
		refresh();
		this->setValue(0);
		if (currentSong == nullptr || currentSong->harmonizeChordTrack.isEmpty()) {
			return;
		}
		for (int32_t i = 0; i < numTracks; i++) {
			if (currentSong->harmonizeChordTrack.equalsCaseIrrespective(names[i])) {
				this->setValue(i + 1);
				return;
			}
		}
	}

	void writeCurrentValue() override {
		if (currentSong == nullptr) {
			return;
		}
		const int32_t chosen = this->getValue();
		if (chosen <= 0) {
			currentSong->harmonizeChordTrack.clear();
			deluge::modulation::midi_fx::chord_source::forget();
			return;
		}
		// Walk the same filtered order the options were built from and take the Output's real name,
		// not the (possibly truncated) display copy - the truncated one would never resolve.
		int32_t index = 0;
		for (Output* output = currentSong->firstOutput; output != nullptr; output = output->next) {
			if (!eligible(output)) {
				continue;
			}
			index++;
			if (index == chosen) {
				currentSong->harmonizeChordTrack.set(output->name.get());
				deluge::modulation::midi_fx::chord_source::forget();
				return;
			}
		}
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		refresh();
		deluge::vector<std::string_view> options;
		options.push_back(l10n::getView(l10n::String::STRING_FOR_OFF));
		for (int32_t i = 0; i < numTracks; i++) {
			options.push_back(std::string_view{names[i]});
		}
		return options;
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return harmonizeEditable(); }

private:
	/// A track can supply chords if it carries pitches and can be named. Kits are drum indices, audio
	/// tracks have no notes, and an unnamed track cannot be stored by name - so it is left out rather
	/// than offered and then silently failing to resolve.
	[[nodiscard]] static bool eligible(Output* output) {
		if (output->type == OutputType::KIT || output->type == OutputType::AUDIO) {
			return false;
		}
		char const* name = output->name.get();
		return name != nullptr && name[0] != 0;
	}

	void refresh() {
		numTracks = 0;
		if (currentSong == nullptr) {
			return;
		}
		for (Output* output = currentSong->firstOutput; output != nullptr && numTracks < kMaxTracks;
		     output = output->next) {
			if (!eligible(output)) {
				continue;
			}
			strncpy(names[numTracks], output->name.get(), kNameChars);
			names[numTracks][kNameChars] = 0;
			numTracks++;
		}
	}

	char names[kMaxTracks][kNameChars + 1]{};
	int32_t numTracks{0};
};

} // namespace deluge::gui::menu_item::midi_fx
