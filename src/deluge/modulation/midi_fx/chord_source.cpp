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

#include "modulation/midi_fx/chord_source.h"
#include "definitions_cxx.hpp"
#include "model/clip/instrument_clip.h"
#include "model/note/note.h"
#include "model/note/note_row.h"
#include "model/output.h"
#include "model/scale/musical_key.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "modulation/midi_fx/harmonize.h"
#include "modulation/midi_fx/midi_fx_settings.h"

namespace deluge::modulation::midi_fx::chord_source {

namespace {

/// The chord read for one position of one source clip.
///
/// Cached on the source clip's own position rather than on a tick counter, which is exactly the
/// right key: the written notes under a position cannot change while the position does not, so
/// every harmonised row firing on the same tick reads the chord once between them.
InstrumentClip* cachedClip = nullptr;
int32_t cachedPos = -1;
harmonize::Chord cachedChord;

/// The last chord that was actually sounding, carried across the gaps.
///
/// Between chords - the chord track resting, or stopped - the phrase keeps the chord it had rather
/// than falling back to its written pitches. Falling back would re-pitch the whole phrase for the
/// length of a rest and then re-pitch it again, which is heard as a stumble; an accompanist holds.
harmonize::Chord heldChord;

/// Pitches already sent by this clip at this position, so two rows cannot land on one note.
///
/// Different degrees can map onto the same chord tone. Sending the same pitch twice costs a voice,
/// and on external MIDI the second note-off is discarded - so the second row is bumped up a chord
/// tone instead. Scoped to one clip at one position; a note-on from another clip resets it, which is
/// fine because the collision only matters within one instrument.
InstrumentClip* claimClip = nullptr;
int32_t claimPos = -1;
int8_t claimedPitches[harmonize::kMaxChordTones * 2];
int32_t claimCount = 0;

[[nodiscard]] InstrumentClip* chordClipOf(Output* source) {
	if (source == nullptr) {
		return nullptr;
	}
	// getActiveClip() rather than the member: activeClip is protected on Output.
	Clip* active = source->getActiveClip();
	if (active == nullptr || active->type != ClipType::INSTRUMENT) {
		return nullptr;
	}
	return (InstrumentClip*)active;
}

/// Reads what is written under the source clip's current position.
void readChordAt(InstrumentClip* source, harmonize::Chord& out) {
	out.clear();

	const int32_t clipLength = source->loopLength;
	if (clipLength <= 0) {
		return;
	}
	const int32_t clipPos = source->lastProcessedPos;

	const int32_t numRows = source->noteRows.getNumElements();
	for (int32_t i = 0; i < numRows; i++) {
		NoteRow* row = source->noteRows.getElement(i);
		if (row == nullptr || row->muted) {
			// A muted *row* is a note the user has taken out of the chord, which is different from a
			// muted *track* - that one still steers, on purpose.
			continue;
		}

		const int32_t rowLength = row->loopLengthIfIndependent != 0 ? row->loopLengthIfIndependent : clipLength;
		if (rowLength <= 0) {
			continue;
		}
		const int32_t pos = harmonize::floorMod(clipPos, rowLength);

		// The last note starting at or before this position - the same search the row's own
		// rendering uses. LESS returns the index of the last element strictly below the key, so the
		// key is pos + 1.
		const int32_t index = row->notes.search(pos + 1, LESS);
		if (index < 0) {
			continue;
		}
		Note* note = row->notes.getElement(index);
		if (note == nullptr) {
			continue;
		}
		// Still spanning? A note whose end is exactly here has already finished.
		if (note->pos + note->getLength() > pos) {
			out.add(row->y);
		}
	}
}

[[nodiscard]] bool isClaimed(int32_t pitch) {
	for (int32_t i = 0; i < claimCount; i++) {
		if (claimedPitches[i] == pitch) {
			return true;
		}
	}
	return false;
}

void claim(InstrumentClip* clip, int32_t pos, int32_t pitch) {
	if (claimClip != clip || claimPos != pos) {
		claimClip = clip;
		claimPos = pos;
		claimCount = 0;
	}
	if (claimCount < static_cast<int32_t>(sizeof(claimedPitches))) {
		claimedPitches[claimCount++] = static_cast<int8_t>(pitch);
	}
}

void resetClaimsIfMoved(InstrumentClip* clip, int32_t pos) {
	if (claimClip != clip || claimPos != pos) {
		claimClip = clip;
		claimPos = pos;
		claimCount = 0;
	}
}

} // namespace

void forget() {
	cachedClip = nullptr;
	cachedPos = -1;
	cachedChord.clear();
	heldChord.clear();
	claimClip = nullptr;
	claimPos = -1;
	claimCount = 0;
}

Output* resolveChordTrack() {
	if (currentSong == nullptr || currentSong->harmonizeChordTrack.isEmpty()) {
		return nullptr;
	}
	for (Output* output = currentSong->firstOutput; output != nullptr; output = output->next) {
		if (output->type == OutputType::KIT || output->type == OutputType::AUDIO) {
			// A kit row is a drum index rather than a pitch, so a kit cannot supply a chord.
			continue;
		}
		if (currentSong->harmonizeChordTrack.equalsCaseIrrespective(output->name.get())) {
			return output;
		}
	}
	return nullptr;
}

int32_t soundingNoteFor(InstrumentClip* clip, NoteRow* noteRow, int32_t writtenOverride) {
	if (clip == nullptr || noteRow == nullptr) {
		return noteRow != nullptr ? noteRow->getNoteCode() : 0;
	}
	// One line, and it is the whole of the override: `written` is what every early return below
	// hands back - Harmonize off, Kit, no chord track, no chord yet, result out of range - so
	// establishing it once here is what makes all of them honour a caller that has already moved
	// the pitch. Added for Stochastic; see the header, and STOCHASTIC-REVERSE.md to back it out.
	const int32_t written = writtenOverride != kNoOverride ? writtenOverride : noteRow->getNoteCode();

	if (runtimeFeatureSettings.get(RuntimeFeatureSettingType::Harmonize) != RuntimeFeatureStateToggle::On) {
		return written;
	}
	if (clip->output == nullptr || clip->output->type == OutputType::KIT || clip->output->type == OutputType::AUDIO) {
		return written;
	}

	// Same gate as every other MIDI FX: the page's own switch AND the master switch, so one toggle
	// silences the whole group the way a user expects.
	deluge::modulation::midi_fx::MidiFxSettings const* settings = clip->midiFxSettingsForRow(noteRow);
	if (settings == nullptr || !settings->enabled || !settings->harmonize) {
		return written;
	}

	Output* source = resolveChordTrack();
	if (source == nullptr || source == clip->output) {
		// Harmonising a track to itself is a feedback loop with no musical reading, so it is simply
		// off rather than an error.
		return written;
	}
	InstrumentClip* sourceClip = chordClipOf(source);
	if (sourceClip == nullptr) {
		return written;
	}

	const int32_t sourcePos = sourceClip->lastProcessedPos;
	if (cachedClip != sourceClip || cachedPos != sourcePos) {
		readChordAt(sourceClip, cachedChord);
		cachedClip = sourceClip;
		cachedPos = sourcePos;
		if (!cachedChord.empty()) {
			heldChord = cachedChord;
		}
	}

	harmonize::Chord const& chord = cachedChord.empty() ? heldChord : cachedChord;
	if (chord.empty()) {
		// Nothing has been heard from the chord track yet in this song. Play as written until it
		// says something - there is no last chord to hold.
		return written;
	}

	MusicalKey const& key = currentSong->key;
	const int32_t scaleSize = key.modeNotes.count();
	// The key root decides which DEGREE the written note is. It deliberately no longer decides which
	// octave the answer lands in - harmonize::fromDegree takes that from the chord's own register
	// now. See the note above fromDegree: taking it from here counted the register twice and threw
	// the whole line octaves up.
	const int32_t relative = written - key.rootNote;
	const int32_t interval = harmonize::floorMod(relative, 12);
	const int8_t degreeInScale = key.modeNotes.degreeOf(static_cast<uint8_t>(interval));

	int32_t result;
	if (scaleSize <= 0 || degreeInScale < 0) {
		result = harmonize::nearestTone(written, chord);
	}
	else {
		int32_t degree = degreeInScale;
		result = harmonize::fromDegree(written, degree, chord);

		// Collision: step to the next chord tone, but never more than one trip round the chord - past
		// that every tone is taken and doubling is the lesser evil. Since fromDegree places the tone
		// beside the written note, a step is to a neighbouring tone rather than strictly upward.
		resetClaimsIfMoved(clip, sourcePos);
		for (int32_t attempt = 0; attempt < chord.count && result >= 0 && isClaimed(result); attempt++) {
			degree++;
			result = harmonize::fromDegree(written, degree, chord);
		}
	}

	if (result < harmonize::kLowestNote || result > harmonize::kHighestNote) {
		return written;
	}

	claim(clip, sourcePos, result);
	return result;
}

} // namespace deluge::modulation::midi_fx::chord_source
