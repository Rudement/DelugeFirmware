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

#include "modulation/midi_fx/midi_fx_settings.h"
#include "modulation/midi_fx/note_pool.h"
#include <cstdint>

class ArpReturnInstruction;

/// MIDI FX: note processing that runs whether or not the arpeggiator is switched on.
///
/// Stage 1 opened the call sites; this is Stage 2, which gives them something to call. The
/// chain still holds no effects, so nothing audible changes yet — the point of the stage is
/// that the shape is in place and reviewable on its own.
///
/// Three axes, all of which the arpeggiator already uses:
///
///  - `onNoteEvent()` — every note event, arp on or off. `sendNote` runs every note through
///                      the arp unconditionally and the arp passes through when `mode == OFF`,
///                      so this fires either way with no new plumbing. Note-mapping effects
///                      (velocity, transpose, chord) need only this.
///  - `render()`      — free-running, once per audio block. Runs with the transport stopped.
///  - `tick()`        — synced, from `Output::doTickForwardForArp`. Returns ticks until the
///                      next event; callers min it into the global wake-up, so no new
///                      scheduling infrastructure is needed.
///
/// Deliberately NOT here yet: the note pool of `docs/MIDI-FX-DESIGN.md` §3, which lets a
/// generated note outlive the key that produced it. Nothing before the echo needs it, and it
/// is subtle enough to deserve its own stage and its own hardware pass rather than arriving
/// tangled up with an effect's behaviour.
/// NOTE: there is also a `deluge::gui::menu_item::midi_fx` (the menus for these effects). A file
/// that pulls in both parents by using-directive must qualify; see the note in `menus.cpp`.
namespace deluge::modulation::midi_fx {

/// "Nothing scheduled, don't wake me." Same sentinel `ArpeggiatorBase::doTickForward` returns
/// when the arp is off, and the same one the callers seed their `std::min` with.
constexpr int32_t kNoEvent = 2147483647;

/// What an effect needs to do its job, without dragging in `ModelStack`.
struct MidiFxContext {
	MidiFxSettings* settings;

	/// Records for notes that outlive the key that produced them. Owned by the chain and injected
	/// here on every axis, so no call site has to know the pool exists — see `MidiFxChain`.
	///
	/// Null for a context built by hand (the tests do). An effect that needs the pool must check.
	NotePool* pool{nullptr};

	/// Whether the arpeggiator is running on this clip. Effects generally do not care — that is
	/// the whole point of the seam — but a few must. Gate Time is one: the arp has its own Gate
	/// control and two gates fighting over the same note is not a feature.
	bool arpIsOn{false};
};

class MidiFx {
public:
	virtual ~MidiFx() = default;

	/// Called for every note event, arp on or off. May rewrite the instruction in place.
	/// Returns false to swallow the event entirely.
	virtual bool onNoteEvent(const MidiFxContext& /*context*/, ArpReturnInstruction& /*instruction*/) { return true; }

	/// Free-running, once per audio block.
	virtual void render(const MidiFxContext& /*context*/, int32_t /*numSamples*/,
	                    ArpReturnInstruction& /*instruction*/) {}

	/// Synced tick. Emits due events into the instruction and returns ticks until the next
	/// one, or `kNoEvent`.
	[[nodiscard]] virtual int32_t tick(const MidiFxContext& /*context*/, uint32_t /*clipCurrentPos*/,
	                                   ArpReturnInstruction& /*instruction*/) {
		return kNoEvent;
	}

	/// All notes off. Clip stop, transport stop, effect disabled, param change that invalidates
	/// anything pending. Any path that can leave a MIDI note hanging is a bug report from
	/// someone with an external synth attached, so this is the method to be paranoid about.
	///
	/// Takes an instruction because an effect that generated notes is the only thing that knows
	/// about them: the transport's own all-notes-off walks the arpeggiator's records and the note
	/// rows, and a ringing echo is in neither. Emit the note-offs it owns, and return true if the
	/// instruction's four slots were not enough — the caller comes back with a fresh one.
	virtual bool flush(const MidiFxContext& /*context*/, ArpReturnInstruction& /*instruction*/) { return false; }
};

} // namespace deluge::modulation::midi_fx
