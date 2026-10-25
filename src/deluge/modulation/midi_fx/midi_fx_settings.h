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

#include "model/sync.h"
#include <cstdint>

class Serializer;
class Deserializer;

namespace deluge::modulation::midi_fx {

/// Per-clip MIDI FX settings, mirroring `ArpeggiatorSettings`.
///
/// Per-clip with an instrument-level default, exactly like `arpSettings` — see
/// `docs/MIDI-FX-DESIGN.md` §6 and open question 1. Consistency with the arp is the whole
/// argument: a user who knows how arp settings behave already knows how these behave.
///
/// One field in Stage 2, on purpose. It proves the save/load path end to end while there is
/// only one thing that can be wrong with it. Effects add their own fields as they land.
class MidiFxSettings {
public:
	MidiFxSettings() = default;

	void cloneFrom(MidiFxSettings const* other);

	/// Returns true if it consumed the tag. Same contract as
	/// `ArpeggiatorSettings::readCommonTagsFromFile`.
	bool readCommonTagsFromFile(Deserializer& reader, char const* tagName);

	void writeCommonParamsToFile(Serializer& writer);

	/// Call once a <midiFx> block has been read. A song saved while the old per-sound Enabled switch
	/// was off may still carry edited values nobody was hearing; those go back to default here, so the
	/// song plays exactly as it did when it was saved.
	void finishReading();

	/// Always true now. This was the per-sound master switch; the menu item is gone (every effect is
	/// neutral at its default, so off was the same as "at default") and the feature-wide switch is
	/// Community Features > Rudement upgrades > MIDI FX. Kept, and still saved as 1, so a song from
	/// this build opens with its effects on in an older build of the fork. finishReading() is what
	/// turns a saved 0 back into true.
	bool enabled{true};

	/// Velocity offset, −64..+63, added to the note's original velocity and clamped to 1..127.
	///
	/// A plain setting rather than an `UnpatchedShared` param, deliberately. Every shared
	/// unpatched param is an `AutoParam` on *every* `ParamManager`, used or not, and the arp
	/// already has to fork its menus into synth and MIDI/CV variants because MIDI instruments
	/// do not carry the same param set. A plain setting works identically on synth, MIDI and CV
	/// with one menu class and costs nothing on clips that never touch MIDI FX.
	///
	/// The trade is that it is not automatable. Promoting a setting to a param later is
	/// straightforward — keep reading the old attribute for compatibility — while demoting one
	/// is not, so this is the reversible direction. The question genuinely bites at the delay,
	/// which wants four or more params; see `docs/MIDI-FX-DESIGN.md` open question 7.
	int8_t velocityOffset{0};

	/// Gate Time as a percentage of a sequenced note's written length, 5..400. 100 means off, so
	/// the default is inert.
	///
	/// Over 100 the note outlives its written end, which is a different mechanism from shortening
	/// and needs both the note pool and a suppressed note-off — see GateTimeFx. uint16_t rather
	/// than uint8_t because 400 does not fit in a byte, and a gate that silently wrapped at 255
	/// would be a very confusing bug.
	uint16_t gateTimePercent{100};

	// --- MIDI Delay (the RS7000's, on the tick axis) ---

	/// DELAY LEVEL. The velocity of the first repeat as a percentage of the source note's, 0..127 —
	/// the RS7000's "DELAY LEVEL". 100 echoes at the played level; over 100 the echo starts louder
	/// than the note. 0 silences the echo.
	uint8_t delayLevelPercent{0};

	/// How many repeats follow the note. 0 is off. Default 3; the whole MIDI FX block is still
	/// off until enabled per clip, and Level defaults to 0, so a fresh clip is silent until dialled in.
	uint8_t delayRepeats{3};

	/// The interval between repeats. SyncLevel/SyncType exactly as the arpeggiator uses them, so
	/// the divisions line up with the arp's and triplet and dotted come free. SYNC_LEVEL_16TH,
	/// not _8TH: the sync label is magnitude-relative and at the factory default magnitude (2)
	/// this enum displays as "1/8" - the same mapping Retrig and the arp use for their defaults.
	SyncLevel delaySyncLevel{SYNC_LEVEL_16TH};
	SyncType delaySyncType{SYNC_TYPE_EVEN};

	/// FB VELOCITY. Added to each repeat's velocity, −127..+127 — the RS7000's additive feedback, so
	/// a negative value fades the tail and a positive one swells it. Below a floor of 1 the tail
	/// ends rather than emitting velocity 0, which on MIDI is a note-off.
	int8_t delayFbVelocity{0};

	/// FB NOTE. Semitones added per repeat, −24..+24. 0 is a plain echo. A repeat transposed past
	/// either end of the keyboard ends the tail rather than being clamped, because a clamped tail
	/// repeats on one pitch forever and sounds like a fault.
	int8_t delayPitchShift{0};

	/// FB NOTE = RND. When set, each repeat is transposed by a fresh random interval instead of by
	/// the fixed FB Note step — the RS7000's "RDN" setting on the FB NOTE knob.
	bool delayNoteRandom{false};

	/// FB CLOCK. Scales the interval before each successive repeat, −100..+100 %. 0 keeps the echoes
	/// on the grid; negative accelerates them (the RS7000's bouncing-ball delay), positive slows
	/// them. Floored at one tick so a repeat can never land on top of its predecessor.
	int8_t delayFbClockPercent{0};

	// --- Retrigger (drum repeats) ---

	/// How many extra hits follow the original. 0 is off, so the default is inert.
	uint8_t retrigRepeats{0};

	/// The interval between hits, as a musical division. Reuses SyncLevel/SyncType exactly as the
	/// arpeggiator does, so triplet and dotted come free and the menu item is the shared one.
	SyncLevel retrigSyncLevel{SYNC_LEVEL_16TH};
	SyncType retrigSyncType{SYNC_TYPE_EVEN};

	/// Velocity multiplier applied at each repeat, as a percentage. 100 keeps every hit at full
	/// strength; lower values make the roll fade. Below a floor the effect stops rather than
	/// emitting velocity 0, which on MIDI is a note-off.
	uint8_t retrigDecayPercent{80};

	// --- Spice & Dice (gate variation on the written phrase) ---
	//
	// Unlike every other effect here, these two are not read by the MidiFxChain. Spice has to be
	// answered *before* a note becomes a note event - a dropped step must never reach the chain at
	// all - so it is read in InstrumentClip::processCurrentPos, where the sequencer already decides
	// whether a note sounds, and in NoteRow::processCurrentPos, where the note-off tick is worked
	// out. The settings live here because this is where a user will look for them, and because the
	// per-drum rule and AFFECT ENTIRE come for free.

	/// How far the gates wander from what is written, 0..50. 0 is off, so the default is inert and
	/// a song saved before this existed plays back untouched.
	uint8_t spiceAmount{0};

	/// Which variation is playing, 0..99.
	///
	/// Saved with the song. Arturia does not save Spice & Dice with a preset at all, and this
	/// started out following that - but the Deluge is not a preset machine, and the first thing
	/// that happens in practice is that you roll into a groove you want to keep. A variation you
	/// cannot save is a variation you have to find again by ear.
	///
	/// It costs nothing to save, because the dice is an index rather than a drawn seed: the same
	/// number is the same pattern on any build. A song from before this was written has no
	/// attribute and reads as 0, which is a perfectly good variation.
	uint8_t spiceDice{0};

	// --- Harmonize (play the written phrase as degrees of the chord track's chord) ---

	/// Whether this clip follows the song's chord track.
	///
	/// Off by default, so a song saved before this existed plays back untouched. Read in
	/// `chord_source::soundingNoteFor`, at the four places a note code leaves the sequencer - not in
	/// the MidiFxChain, for the same reason Spice is not: the pitch has to be decided before the note
	/// becomes a note event, and it has to be the same pitch on the way out as on the way in.
	bool harmonize{false};
};

} // namespace deluge::modulation::midi_fx
