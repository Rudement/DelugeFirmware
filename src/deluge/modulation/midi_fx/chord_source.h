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

#include <cstdint>

class InstrumentClip;
class NoteRow;
class Output;

/// Where the chord comes from, for Harmonize.
///
/// The chord track is one Output, song-wide, named on the Song. An Output rather than a Clip
/// because a Deluge user saying "the chord track" means the instrument column and expects it to keep
/// working when they launch a different clip on it - so this follows `Output::activeClip`.
///
/// **The chord is read from the source's written notes at its current position, not from what it is
/// actually sounding.** Reading live state would have been free - every NoteRow already carries
/// `sequenced` - and it is wrong twice over. Clips are processed in list order within one sequencer
/// tick, so a harmonised clip processed before the chord clip would read *last* tick's chord and put
/// a wrong note on the downbeat of every chord change, intermittently, depending on what order the
/// tracks happen to sit in. And a muted chord track sounds nothing, so live state would stop
/// steering exactly when someone wants the chords to conduct without being heard. Reading the
/// written notes is independent of both.
namespace deluge::modulation::midi_fx::chord_source {

/// Passed as `writtenOverride` to mean "no override" - the row's own note code is used.
///
/// A named sentinel rather than a bare -1 test, because note code 0 is a legal pitch: a `>= 0`
/// check would silently ignore an override of 0 and harmonise the row's written note instead,
/// which is a wrong note at one pitch in one octave and would be found by ear years later.
constexpr int32_t kNoOverride = -1;

/// The note code this row should actually send, right now.
///
/// Returns the row's written `y` unchanged whenever Harmonize is off, unavailable, or has no chord
/// to work with - so the un-harmonised path is exactly what it was before this existed.
///
/// `writtenOverride` replaces the row's own note code as the *input* to the degree mapping, for
/// callers that have already moved the pitch - Stochastic, which must wander in degrees before the
/// chord is applied or its wander walks off the chord tones. It is threaded in at the single place
/// `written` is established rather than at each `return`, so every one of this function's early
/// exits honours it too. Default `kNoOverride` leaves behaviour exactly as it was.
///
/// Added for Stochastic on 2026-09-14. To back it out: delete the parameter here and the ternary
/// in the .cpp, restoring `const int32_t written = noteRow->getNoteCode();`. STOCHASTIC-REVERSE.md
/// in the repo root carries the exact before/after.
[[nodiscard]] int32_t soundingNoteFor(InstrumentClip* clip, NoteRow* noteRow, int32_t writtenOverride = kNoOverride);

/// Drops the cached chord and the held chord. Called when the song changes underneath us, so a new
/// song cannot inherit the last chord of the old one.
void forget();

/// The Output currently acting as the song's chord track, or null. Resolved by name every call, and
/// deliberately: a cached pointer would dangle the moment that track was deleted.
[[nodiscard]] Output* resolveChordTrack();

} // namespace deluge::modulation::midi_fx::chord_source
