/*
 * Song-wide snapshot morph.
 *
 * Capture stamps the value of every parameter in the song. Capture again and the two are
 * diffed: only the parameters that actually changed between the two stamps are kept, and the
 * MORPH param crossfades that short list.
 *
 * It rolls. Every capture after the first becomes the new B, and the state you stamped
 * previously becomes the A -- so one press always leaves you morphing between the last two
 * things you stamped, and you never have to close a pair before starting the next one.
 *
 * The diff is the whole trick. A song is several thousand parameters across every clip and
 * every kit row, and writing all of them from the audio render path is not viable. What you
 * actually tweaked between two stamps is a few dozen.
 *
 * Phase 2 keeps every capture. Captures fill a bank of 16 numbered slots, and cue() makes any
 * one of them the morph target, sweeping from wherever the song is right now. The rolling pair
 * above is what the bank does on its own: each capture is morphed against the slot captured or
 * cued before it.
 *
 * The bank is one full baseline -- the walk taken at the first capture -- plus, for every slot, only
 * the walk positions where it differs from that baseline. Sixteen full walks would be 768KB;
 * this is the baseline plus whatever you actually changed.
 *
 * Session-only, deliberately: every delta points at a ParamManager in the song that is loaded
 * right now, and every slot is a list of positions in one walk over the song's current
 * structure. Nothing here survives a song load, and clear() is called when one happens.
 */

#pragma once

#include <cstdint>

class Clip;
class Deserializer;
class Serializer;
class Song;
class ModControllable;
class NoteRow;
class ParamManager;
class TimelineCounter;

/// What a delta actually points at. Almost everything is a param; saturation is not.
enum class MorphTarget : uint8_t {
	UNPATCHED,
	PATCHED,
	/// A value in a MIDI clip's MIDIParamCollection. The param id is the CC number.
	MIDI_CC,
	/// ModControllableAudio::clippingAmount -- a plain uint8_t member, not a param, so it
	/// lives in no ParamSet and the param walk cannot see it. It is the only *continuous*
	/// value in that position; the other non-param members (lpfMode, hpfMode, filterRoute,
	/// modFXType_) are discrete and have no meaningful halfway point, so they stay out.
	CLIPPING,
};

/// One value that differs between the two ends of the morph. Holds a walk POSITION, never a
/// pointer: apply() re-walks the song and takes its pointers live, because clips, kit rows and
/// ParamManagers are freed and moved by ordinary edits while a morph is armed.
struct MorphDelta {
	int32_t index; ///< position in walk order
	int32_t valueA;
	int32_t valueB;
};

constexpr int32_t kMorphNumSlots = 16;

/// The slot selector is an ordinary 0-128 knob param. Sixteen slots sit on every eighth knob
/// position (-64, -56 ... +56), and View steps the knob a whole slot per click, so each detent
/// is exactly one slot. Reading rounds down, so an automated or MIDI value between detents still
/// lands on a slot.
constexpr int32_t kMorphSlotKnobStep = 8;

[[nodiscard]] constexpr int32_t morphSlotFromKnobPos(int32_t knobPos) {
	int32_t slot = (knobPos + 64) / kMorphSlotKnobStep;
	return slot < 0 ? 0 : (slot >= kMorphNumSlots ? kMorphNumSlots - 1 : slot);
}

[[nodiscard]] constexpr int32_t morphSlotToKnobPos(int32_t slot) {
	return -64 + slot * kMorphSlotKnobStep;
}

/// Same conversion ParamCollection::paramValueToKnobPos does, for callers holding a raw value.
[[nodiscard]] constexpr int32_t morphSlotFromValue(int32_t value) {
	int32_t knobPos = (value >= static_cast<int32_t>(0x80000000u - (1u << 24))) ? 64 : ((value + (1 << 24)) >> 25);
	return morphSlotFromKnobPos(knobPos);
}

/// One walk position where a slot differs from the bank's baseline.
struct MorphSlotEntry {
	int32_t index; ///< position in walk order
	int32_t value;
};

/// A captured state, stored as its differences from the baseline. Entries are in ascending
/// index order because the walk produces them that way, which is what lets a slot be read back
/// with one forward merge.
struct MorphSlot {
	MorphSlotEntry* entries = nullptr;
	int32_t count = 0;
	bool used = false;
};

class Morph {
public:
	/// One press of the capture command: stamp the song into the first empty slot and morph
	/// from the slot captured or cued before it. BANK FULL once all sixteen are used.
	void capture();

	/// Make slot `slot` (0-based) the B end, with the song as it is right now as the A end.
	/// Returns false if the slot is empty or the bank had to be thrown away.
	bool cue(int32_t slot);

	/// Name the slot at one end of the sweep, for when the upper knob lands on 0 (A) or 50 (B).
	/// Does nothing while no morph is armed.
	void showEnd(bool atB);

	/// Say what is in a slot, for the selector knob. Does not change anything.
	void showSlot(int32_t slot);

	/// Stamp the song into slot `slot` (0-based), replacing whatever it held. The deliberate way
	/// to reuse a slot once the bank is full; the other fifteen are untouched.
	void replaceSlot(int32_t slot);

	/// Once per audio render window from Song::renderAudio. Returns immediately unless the
	/// morph position has actually moved.
	void apply();

	/// Drop everything. Called on song load and whenever the pointers could go stale.
	void clear();

	/// Saving and loading the bank with the song. writeToFile() is called while the current song is
	/// written. Reading happens into the song being loaded, which is not current yet, so the slots are
	/// held as pending and only become the bank in adoptPending(), once that song is current.
	void writeToFile(Serializer& writer);
	void beginReading(Song* song);
	void readSlotFromFile(Deserializer& reader);
	void adoptPending();
	void discardPending();

	[[nodiscard]] bool isArmed() const { return numDeltas > 0; }
	[[nodiscard]] bool hasSnapshot() const { return baseline != nullptr; }
	[[nodiscard]] int32_t getNumDeltas() const { return numDeltas; }
	[[nodiscard]] int32_t getNumSlotsUsed() const { return numSlotsUsed; }
	[[nodiscard]] bool isSlotUsed(int32_t slot) const { return slot >= 0 && slot < kMorphNumSlots && slots[slot].used; }

private:
	/// Park the morph position at one end, so the knob agrees with what is sounding.
	void setPosition(int32_t raw);

	/// Shared body of capture() and replaceSlot(); targetSlot < 0 means the first empty slot.
	void captureInto(int32_t targetSlot);

	/// Take ownership of a fresh walk as the baseline, and record it as slot `slot`.
	void startBank(int32_t* walk, int32_t count, int32_t slot);

	void freeBank();
	void freeDeltas();

	/// The walk taken at the first capture. A hidden reference, never rewritten: every slot's
	/// entries index into it, so replacing any slot -- including the first -- moves no other.
	int32_t* baseline = nullptr;
	int32_t baselineCount = 0;
	MorphSlot slots[kMorphNumSlots]{};
	int32_t numSlotsUsed = 0;
	/// The slot the next capture morphs FROM: whichever was captured or cued last.
	int32_t referenceSlot = -1;

	MorphDelta* deltas = nullptr;
	int32_t numDeltas = 0;
	/// The walk count the deltas were built against. apply() drops the morph if the song no
	/// longer walks to this count, rather than write into positions that now mean other params.
	int32_t deltaWalkCount = 0;
	/// The slot numbers shown at each end of the sweep: 0 on the knob is A, 50 is B.
	int32_t endSlotA = -1;
	int32_t endSlotB = -1;
	/// The A end is the previous version of endSlotA (you replaced the slot you were on).
	bool endAIsOld = false;

	/// Slots read from a song file that is still loading. See adoptPending().
	Song* pendingSong = nullptr;
	int32_t pendingWalkCount = -1;
	int32_t pendingEndA = -1; ///< saved upper-knob ends, re-armed on load
	int32_t pendingEndB = -1;
	MorphSlot pendingSlots[kMorphNumSlots]{};
	int32_t lastAppliedPos = 0;
	bool everApplied = false;
	uint8_t applyDivider = 0;
};

extern Morph morph;
