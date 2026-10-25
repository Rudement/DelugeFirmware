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

#include "model/model_stack.h"
#include "modulation/midi_fx/groove.h"
#include "modulation/midi_fx/loop_remix.h"
#include "modulation/midi_fx/midi_fx_settings.h"
#include <cstdint>

class AutoParam;
class InstrumentClip;
class NoteRow;
class Song;

/// Temp Hold - the Torso T-1's "Temp" button.
///
/// Hold SHIFT, change anything, and everything you changed goes back to where it was when you let
/// go. It does not add a control; it changes what every control you already have is *for*, by
/// making a change you can commit to and a change you are only trying out two different gestures.
///
/// **Nothing here is a new way to edit.** Every value still travels its normal path - the gold
/// knob writes through `View::modEncoderAction_existentParam`, a MIDI FX menu item writes through
/// `midiFxApply`, Play FX writes through `playFxWriteRows`. Temp Hold only stands at those three
/// doors, writes down what a thing was the first time it is touched, and puts it back on release.
/// That is why it works identically in a clip, in song mode, in the arranger and in automation
/// view without knowing that any of them exist: it never asks where it is, only what changed.
///
/// **Automation is switched off for the duration, not worked around afterwards.** A param write
/// during a hold takes the same route the stutter param already takes in that function - the
/// timeline counter is nulled on a copied model stack, so the write is a live value change that
/// cannot land in an automation lane. Without that, a temp twist would carve a real curve into the
/// clip and the restore would carve a second one over the top, which is the exact opposite of what
/// this is for. The visible consequence, and it belongs in the manual: you cannot record automation
/// while Temp is held.
///
/// **Everything is keyed on the thing changed, never on the control that changed it.** During a
/// hold the user can select another clip, toggle AFFECT ENTIRE, or page the gold knobs -
/// `setActiveModControllableTimelineCounter` even calls `pretendModKnobsUntouchedForAWhile()` when
/// that happens. A table keyed on knob number would then restore the wrong clip's param. Keyed on
/// the `AutoParam*` itself, a hold that spans a clip change still puts back exactly what it took.
namespace deluge::modulation::temp_hold {

/// Distinct params one hold can cover. Sixteen is two full pages of gold knobs; nobody is turning
/// more than that inside one gesture, and the overflow behaviour (§`noteParam`) is safe anyway.
constexpr int32_t kMaxParams = 16;

/// Distinct MIDI FX / Play FX objects one hold can cover. A kit writes one entry per drum under
/// AFFECT ENTIRE, so this is really "biggest kit a hold can cover whole", and 48 clears the 16-row
/// kits people actually build with room for the clip and its rows.
constexpr int32_t kMaxSettings = 48;

/// What one settings entry is holding, because three different things live in this table and the
/// restore has to know which.
enum class SettingsKind : uint8_t {
	NONE,
	/// A whole `MidiFxSettings` - Velocity, Gate, Repeat, Delay, Spice and Dice in one copy.
	MIDI_FX,
	/// A `NoteRow`'s Play FX pair.
	PLAY_FX_ROW,
	/// An `InstrumentClip`'s clip-wide effect settings: the Play FX pair and Strum's three. One
	/// entry rather than two because they live on the same object, so a hold that walks across both
	/// pages of one clip costs one slot and cannot half-restore.
	CLIP_WIDE_FX,
};

class TempHold {
public:
	TempHold() = default;

	[[nodiscard]] bool isActive() const { return active_; }

	/// True when Temp Hold is switched on in Community Features *and* SHIFT is down. Everything
	/// else in this class is only reached through this, so a build with the toggle off behaves
	/// exactly as it did before the feature existed.
	[[nodiscard]] static bool shouldCapture();

	/// The footswitch half of the gesture, driven by GlobalMIDICommand::TEMP_HOLD.
	///
	/// Engages the hold **without** engaging the SHIFT modifier, which is the whole reason this is
	/// its own MIDI command rather than "learn a switch to SHIFT" - that already works, and brings
	/// every other shifted gesture with it, so a pad press with your foot down would do its shifted
	/// job instead of its ordinary one.
	void midiEngage(bool on);

	[[nodiscard]] bool isMidiEngaged() const { return midiEngaged_; }

	/// Starts a hold if one is not already running. Called lazily from the first captured change
	/// rather than from the SHIFT press, so holding SHIFT and changing nothing costs nothing at
	/// all - which matters, because SHIFT is held constantly for other reasons.
	void beginIfNeeded();

	/// Puts everything back and ends the hold. Safe to call when no hold is running.
	void end();

	/// Ends the hold *without* restoring. For when what would be restored is gone or must not be
	/// touched - a song load, a clip deletion, a factory reset. Restoring into freed memory is the
	/// one way this feature can crash, and this is the function that stops it.
	void abandon();

	/// Records a param's value before it changes. `modelStackWithParam` must already have had its
	/// timeline counter nulled, so that the restore writes by the same non-automated route the
	/// change did.
	void noteParam(ModelStackWithAutoParam* modelStackWithParam, int32_t currentValue);

	/// Temp Hold tails for mod FX (2026-10-06). Steps any mod FX params that a released hold is gliding
	/// back, rather than having snapped. Called ~50 times a second from its own scheduler task. Cheap
	/// when nothing is gliding, which is nearly always.
	void tickGlides();

	/// A knob turned outside a hold takes the param over: whatever glide it was in stops where it is.
	void dropGlide(AutoParam* autoParam);

	void noteMidiFx(deluge::modulation::midi_fx::MidiFxSettings* settings);
	void notePlayFxRow(NoteRow* row);
	void notePlayFxClip(InstrumentClip* clip);

private:
	struct ParamEntry {
		AutoParam* autoParam;
		int32_t originalValue;
		/// A full copy of the model stack the change was made through, which is what the restore
		/// needs to write back. `MODEL_STACK_MAX_SIZE` is `sizeof(ModelStackWithAutoParam)`, so
		/// this is the same buffer shape the rest of the codebase passes around.
		alignas(ModelStackWithAutoParam) char stackMemory[MODEL_STACK_MAX_SIZE];
	};

	struct SettingsEntry {
		SettingsKind kind;
		void* object;
		deluge::modulation::midi_fx::MidiFxSettings midiFx;
		int32_t clockShift;
		int32_t beatStretch;
		int8_t division;
		/// Clip-wide only - a NoteRow has no Strum of its own, because a strum is a property of the
		/// group of notes hitting together rather than of any one row.
		uint8_t strumAmount;
		uint8_t strumDirection;
		int8_t strumContour;
		/// Loop Remix: a Kit row's own, or a melodic clip's.
		deluge::modulation::midi_fx::loop_remix::Slot loopRemix;
		/// Groove: a Kit row's own, or a melodic clip's.
		deluge::modulation::midi_fx::groove::Settings groove;
	};

	/// A mod FX param on its way back after a hold, with Tails on. Same stored-stack idea as ParamEntry:
	/// the glide writes through the very route the change was made by, timeline counter nulled.
	struct GlideEntry {
		AutoParam* autoParam;
		int32_t fromValue;
		int32_t toValue;
		uint32_t startTime; ///< AudioEngine::audioSampleTimer when the hold ended
		alignas(ModelStackWithAutoParam) char stackMemory[MODEL_STACK_MAX_SIZE];
	};

	/// ~1.5 s, matching the delay's tail glide. In samples.
	static constexpr uint32_t kGlideSamples = 65536;

	[[nodiscard]] bool noteObject(SettingsKind kind, void* object, SettingsEntry** entryOut);
	void clearHold();
	void restoreParams();
	void restoreSettings();

	bool active_{false};
	/// Set while a footswitch learned to GlobalMIDICommand::TEMP_HOLD is down. Either this or a
	/// held SHIFT keeps a hold open, and end() waits for both to be clear.
	bool midiEngaged_{false};
	/// The song the hold started on. If it is not the song we are on when the hold ends, everything
	/// in both tables belongs to a project that is no longer loaded and the restore is skipped.
	Song* song_{nullptr};
	/// Set when a hold touched more than the tables can carry. The extra changes are let through
	/// unrecorded rather than dropped - a knob that does not come back is a smaller surprise than a
	/// knob that did nothing when you turned it - and the user is told once.
	bool overflowed_{false};

	int32_t numParams_{0};
	int32_t numSettings_{0};
	ParamEntry params_[kMaxParams];
	int32_t numGlides_{0};
	/// The song the glides were started on. Separate from song_, which is cleared when a hold ends -
	/// exactly when the glides begin.
	Song* glideSong_{nullptr};
	GlideEntry glides_[kMaxParams];
	SettingsEntry settings_[kMaxSettings];
};

extern TempHold tempHold;

} // namespace deluge::modulation::temp_hold
