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

#include "modulation/midi_fx/pitch_source.h"
#include "definitions_cxx.hpp"
#include "model/clip/instrument_clip.h"
#include "model/note/note_row.h"
#include "model/output.h"
#include "model/scale/musical_key.h"
#include "model/song/song.h"
#include "modulation/midi_fx/chord_source.h"
#include "modulation/midi_fx/midi_fx_settings.h"
#include "modulation/midi_fx/stochastic.h"

namespace deluge::modulation::midi_fx::pitch_source {

int32_t wanderedNoteFor(InstrumentClip* clip, NoteRow* noteRow, int32_t noteRowId, int32_t notePos) {
	if (clip == nullptr || noteRow == nullptr) {
		return noteRow != nullptr ? noteRow->getNoteCode() : 0;
	}
	const int32_t written = noteRow->getNoteCode();

	// The clip's own Amount is the only switch, exactly as Spread's three are. Deliberately NOT
	// gated on MidiFxSettings::enabled: these settings live on the Clip precisely because that
	// struct is a decoy on a Kit, and reaching back into it for an on/off would reintroduce the
	// problem the move was made to avoid.
	if (clip->stochasticAmount == stochastic::kOff) {
		return written;
	}

	// Non-kit only, like Harmonize and Spread. On a Kit a row is a drum rather than a pitch, so a
	// wandering note code scrambles which drum plays rather than transposing anything. The kit
	// reading of this idea - wander to a neighbouring drum - is a genuinely good feature and a
	// separate one; see STOCHASTIC-FEASIBILITY.md §6.
	if (clip->output == nullptr || clip->output->type == OutputType::KIT || clip->output->type == OutputType::AUDIO) {
		return written;
	}

	if (currentSong == nullptr) {
		return written;
	}
	MusicalKey const& key = currentSong->key;
	const int32_t scaleSize = key.modeNotes.count();
	if (scaleSize <= 0) {
		return written;
	}

	const int32_t relative = written - key.rootNote;
	const int32_t octave = stochastic::floorDiv(relative, stochastic::kSemitonesPerOctave);
	const int32_t interval = stochastic::floorMod(relative, stochastic::kSemitonesPerOctave);
	const int8_t degreeInScale = key.modeNotes.degreeOf(static_cast<uint8_t>(interval));
	if (degreeInScale < 0) {
		// A written note that is not in the key has no degree, so there is nothing to add an offset
		// to. Left exactly as written. Harmonize's fallback for the same case is `nearestTone`,
		// which is right for a harmoniser and wrong here: a chromatic passing note moved by "two
		// degrees" has been moved by an arbitrary interval, and the chromaticism was written on
		// purpose.
		return written;
	}

	// Dice is shared with Spice rather than duplicated - one "which variation am I hearing" number
	// across the group. Safe to read here because this path is non-kit only, where
	// midiFxSettingsForRow hands back the Clip's own copy.
	deluge::modulation::midi_fx::MidiFxSettings const* settings = clip->midiFxSettingsForRow(noteRow);
	const uint8_t dice = settings != nullptr ? settings->spiceDice : 0;

	if (!stochastic::fires(clip->stochasticAmount, dice, noteRowId, notePos)) {
		return written;
	}

	const int32_t offset = stochastic::degreeOffset(clip->stochasticWidth, dice, noteRowId, notePos);
	const int32_t target = degreeInScale + offset;
	const int32_t index = stochastic::wrappedDegree(target, scaleSize);
	const int32_t carry = stochastic::octaveCarry(target, scaleSize);
	const int32_t result =
	    stochastic::assemble(key.rootNote, octave, carry, key.modeNotes[static_cast<uint8_t>(index)]);

	// Declined rather than clamped, so a bassline near the floor simply stops making its downward
	// jumps instead of piling every one of them onto the lowest note. Its own function in
	// stochastic.h so the header test can assert it across the whole note range - it is effectively
	// impossible to verify by ear.
	return stochastic::applyOrDecline(written, result);
}

int32_t soundingNoteFor(InstrumentClip* clip, NoteRow* noteRow, int32_t noteRowId, int32_t notePos) {
	// The order, and the only place it is written down. See the header.
	//
	// Two stages, no correction between them. An earlier version folded the harmonised result back
	// toward where the written note would have gone, because Harmonize was amplifying a wander of
	// two scale degrees into a jump of one or two octaves. That amplification is gone at source -
	// harmonize::fromDegree now places the chord tone beside the note it was given - so there is
	// nothing left here to correct, and a correction that stayed would have quietly capped the top
	// of Stochastic's Width range.
	return chord_source::soundingNoteFor(clip, noteRow, wanderedNoteFor(clip, noteRow, noteRowId, notePos));
}

} // namespace deluge::modulation::midi_fx::pitch_source
