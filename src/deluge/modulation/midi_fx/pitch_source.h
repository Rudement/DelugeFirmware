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

/// The whole pitch chain, in order, in one place.
///
/// **Two features now decide a note's pitch at play time, and the order between them is not a
/// preference - it is the difference between the pair composing and the pair sounding broken.**
///
///     written -> Stochastic (wanders in degrees) -> Harmonize (maps the degree onto the chord)
///
/// Stochastic first. It moves the note within the song's key, and Harmonize then takes whatever
/// degree it is handed and puts it on a chord tone, so the wander cannot land anywhere outside the
/// chord. Run the other way round, the wander would walk off the chord tones Harmonize had just
/// picked, and a clip that was supposed to be generative would simply play wrong notes.
///
/// Spread's octave is the third stage and stays at the call sites rather than moving in here,
/// because it is applied to the *latched* note for the reason `InstrumentClip::sendPendingNoteOn`
/// gives: an octave of a chord tone is still a chord tone, so it cannot spoil what Harmonize
/// decided, and the latch is what the note-off reads.
///
/// **Two stages, and nothing between them.** A middle version of this file folded the harmonised
/// result back toward where the written note would have gone, because Harmonize was amplifying a
/// wander of two scale degrees into a jump of one or two octaves. That was a correction layered on
/// top of a bug in the stage below: Harmonize's own register was wrong, with Stochastic switched off
/// entirely. Fixing it at source - `harmonize::fromDegree` now places the chord tone beside the note
/// it was handed - left nothing here to correct, and a correction that stayed would have quietly
/// capped the top of Stochastic's Width range. See HARMONIZE-REGISTER-FIX.md.
///
/// This exists as one function rather than as two calls at each site so that the order lives in
/// exactly one place. There are two note-on paths - the sequenced one in InstrumentClip and the
/// buffer-full overflow in NoteRow - and an order that had to be spelled out at both is an order
/// that will eventually differ between them.
namespace deluge::modulation::midi_fx::pitch_source {

/// The written note after Stochastic, before Harmonize. Exposed for the one caller that wants the
/// stages separately; everything else should use `soundingNoteFor` and get the order for free.
///
/// Returns the written note unchanged whenever Stochastic is off, the clip is a Kit, the song has
/// no usable key, the written note is not in that key, or the wander would leave the note range.
[[nodiscard]] int32_t wanderedNoteFor(InstrumentClip* clip, NoteRow* noteRow, int32_t noteRowId, int32_t notePos);

/// The note code this row should actually send, right now: Stochastic then Harmonize.
[[nodiscard]] int32_t soundingNoteFor(InstrumentClip* clip, NoteRow* noteRow, int32_t noteRowId, int32_t notePos);

} // namespace deluge::modulation::midi_fx::pitch_source
