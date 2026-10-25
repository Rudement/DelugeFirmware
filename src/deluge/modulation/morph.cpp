#include "modulation/morph.h"

#include "definitions_cxx.hpp"
#include "gui/ui_timer_manager.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/clip.h"
#include "model/clip/clip_array.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/output.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/midi/midi_param.h"
#include "modulation/midi/midi_param_collection.h"
#include "modulation/params/param.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "storage/storage_manager.h"
#include "util/cfunctions.h"
#include <cstring>

namespace params = deluge::modulation::params;

Morph morph{};

namespace {

/// Popups carry their numbers: the delta count on arm, and both walk counts when the two
/// captures could not be compared. Knowing "armed, 37 values" versus "armed, 2" is the
/// difference between understanding a morph and guessing at it.
char popupBuffer[48];

char const* withNumber(char const* label, int32_t n) {
	strcpy(popupBuffer, label);
	intToString(n, popupBuffer + strlen(popupBuffer), 1);
	return popupBuffer;
}

/// The default popup lasts 800ms, which is not long enough to read a sentence with two
/// numbers in it while your hands are still on the knob. Ask for a persistent popup, then
/// set the dismissal timer by hand so it still clears itself.
void showResult(char const* text) {
	display->displayPopup(text, 0);
	uiTimerManager.setTimer(TimerName::DISPLAY, 3000);
}

/// A sanity cap on one walk, not a working size: every walk buffer is sized to the song it
/// is taken from. Real songs run to tens of thousands of params -- every clip and every kit row
/// carries its own set of around a hundred -- and 12000, the phase 1 figure, refused most of
/// them. 262144 values is 1MB.
constexpr int32_t kMaxSnapshotValues = 262144;

/// If more than this many params changed between two captures, the user did something other
/// than tweak -- loaded a different song, swapped an instrument. Refuse rather than morph
/// two unrelated states into each other.
constexpr int32_t kMaxDeltas = 512;

/// A 7-bit CC cannot produce more than 128 distinct positions across a sweep, so applying on
/// every render window is wasted work.
constexpr uint8_t kApplyEveryNWindows = 8;

/// Visit every param in one ParamManager.
///
/// Guarded the way every param lookup in this tree now has to be: a ParamManager that is
/// missing its collections freezes on E410/E411, and this runs from the audio render path.
template <typename F>
void visitParamManager(TimelineCounter* tc, ModControllable* mc, ParamManager* pm, NoteRow* nr, int32_t noteRowId,
                       bool isSong, bool isMidi, F&& fn) {
	if (!tc || !mc || !pm || !pm->containsAnyMainParamCollections()) {
		return;
	}
	if (isMidi) {
		MIDIParamCollection* midi = pm->getMIDIParamCollection();
		for (int32_t i = 0; i < midi->params.getNumElements(); i++) {
			MIDIParam* param = midi->params.getElement(i);
			fn(tc, mc, pm, nr, noteRowId, MorphTarget::MIDI_CC, param->cc, param->param.getCurrentValue());
		}
		return;
	}

	// Saturation, first, so it keeps a stable position in the walk. Every ModControllable the
	// walk reaches is a ModControllableAudio -- the output-type filter in walkSong() excludes
	// MIDI and CV, which are the only ones that are not.
	fn(tc, mc, pm, nr, noteRowId, MorphTarget::CLIPPING, 0,
	   static_cast<int32_t>(static_cast<ModControllableAudio*>(mc)->clippingAmount));

	UnpatchedParamSet* unpatched = pm->getUnpatchedParamSet();
	if (unpatched) {
		int32_t n = unpatched->getNumParams();
		for (int32_t p = 0; p < n; p++) {
			// The morph position and the slot selector must never be things the morph morphs.
			if (isSong && (p == params::UNPATCHED_MORPH || p == params::UNPATCHED_MORPH_SLOT)) {
				continue;
			}
			fn(tc, mc, pm, nr, noteRowId, MorphTarget::UNPATCHED, p, unpatched->getValue(p));
		}
	}

	if (pm->containsPatchedParamCollections()) {
		PatchedParamSet* patched = pm->getPatchedParamSet();
		if (patched) {
			int32_t n = patched->getNumParams();
			for (int32_t p = 0; p < n; p++) {
				fn(tc, mc, pm, nr, noteRowId, MorphTarget::PATCHED, p, patched->getValue(p));
			}
		}
	}
}

/// Visit every param in the song, in a deterministic order.
///
/// Order matters: the second capture compares positionally against the first, so the walk has
/// to produce the same sequence both times. It does as long as the song's clips and rows have
/// not changed, and the count check in capture() catches it when they have.
/// Visit every param OWNER in the song -- the song itself, each clip, each Sound kit row -- in a
/// fixed order, with the pointers as they are right now.
template <typename F>
void walkOwners(F&& fn) {
	Song* song = currentSong;
	if (!song) {
		return;
	}

	// The song's own FX.
	fn(song, &song->globalEffectable, &song->paramManager, nullptr, 0, true, false);

	ClipArray* arrays[2] = {&song->sessionClips, &song->arrangementOnlyClips};
	// Keep the original audio-owner walk as an exact prefix. Saved banks from before MIDI CC
	// support use these positions, so inserting MIDI clips among them would reinterpret every
	// later entry. MIDI owners are appended in a second pass below.
	for (ClipArray* array : arrays) {
		for (int32_t i = 0; i < array->getNumElements(); i++) {
			Clip* clip = array->getClipAtIndex(i);
			if (!clip || !clip->output) {
				continue;
			}

			// Sounds and audio clips keep an UnpatchedParamSet in summaries[0].
			// setupMIDI() puts a MIDIParamCollection there instead and sets the SAME
			// expressionParamSetOffset as setupUnpatched(), so containsAnyMainParamCollections()
			// waves a MIDI or CV clip straight through and getUnpatchedParamSet() casts the
			// wrong type. getNumParams() then reads garbage and the loop walks off the end of
			// memory -- a hard fault, with no error code to tell you where.
			OutputType outputType = clip->output->type;
			if (outputType != OutputType::SYNTH && outputType != OutputType::KIT && outputType != OutputType::AUDIO) {
				continue;
			}

			fn(clip, clip->output->toModControllable(), &clip->paramManager, nullptr, 0, false, false);

			// Kit rows carry a ParamManager each -- that is where a drum's own filter,
			// envelopes and FX actually live.
			if (clip->type == ClipType::INSTRUMENT && clip->output->type == OutputType::KIT) {
				auto* instrumentClip = static_cast<InstrumentClip*>(clip);
				for (int32_t j = 0; j < instrumentClip->noteRows.getNumElements(); j++) {
					NoteRow* noteRow = instrumentClip->noteRows.getElement(j);
					// Same trap one level down: a MIDI or gate drum's row has no
					// UnpatchedParamSet either.
					if (!noteRow || !noteRow->drum || noteRow->drum->type != DrumType::SOUND) {
						continue;
					}
					fn(clip, noteRow->drum->toModControllable(), &noteRow->paramManager, noteRow, j, false, false);
				}
			}
		}
	}

	// MIDI CC values form a tail after the unchanged audio walk. MIDIParamVector is ordered by CC,
	// so this stays deterministic across capture, apply, save, and load.
	for (ClipArray* array : arrays) {
		for (int32_t i = 0; i < array->getNumElements(); i++) {
			Clip* clip = array->getClipAtIndex(i);
			if (clip && clip->output && clip->output->type == OutputType::MIDI_OUT) {
				fn(clip, clip->output->toModControllable(), &clip->paramManager, nullptr, 0, false, true);
			}
		}
	}
}

/// Visit every param in the song, in a deterministic order: walkOwners, one visitParamManager
/// per owner.
///
/// Order matters: comparisons between captures are positional, so the walk has to produce the
/// same sequence every time. It does as long as the song's clips and rows have not changed, and
/// every caller checks the count against the baseline's to catch it when they have.
template <typename F>
void walkSong(F&& fn) {
	walkOwners([&](TimelineCounter* tc, ModControllable* mc, ParamManager* pm, NoteRow* nr, int32_t noteRowId,
	               bool isSong, bool isMidi) { visitParamManager(tc, mc, pm, nr, noteRowId, isSong, isMidi, fn); });
}

/// How many walk positions one owner occupies, and which param each one is -- without visiting
/// the params. Must agree exactly with visitParamManager(): CLIPPING first, then the unpatched
/// params (less the two the song skips), then the patched ones. This is what lets apply() jump
/// straight to the handful of positions it writes instead of stepping through every param in
/// the song, which on a big song is tens of thousands of calls on the audio path.
struct OwnerLayout {
	int32_t numUnpatched = 0;        ///< in the set
	int32_t numUnpatchedEmitted = 0; ///< in the walk
	int32_t numPatched = 0;
	bool isSong = false;
	bool isMidi = false;
	MIDIParamCollection* midi = nullptr;
	bool valid = false;

	[[nodiscard]] int32_t size() const {
		return valid ? (isMidi ? midi->params.getNumElements() : 1 + numUnpatchedEmitted + numPatched) : 0;
	}

	void locate(int32_t offset, MorphTarget& target, int32_t& paramId) const {
		if (isMidi) {
			target = MorphTarget::MIDI_CC;
			paramId = midi->params.getElement(offset)->cc;
			return;
		}
		if (offset == 0) {
			target = MorphTarget::CLIPPING;
			paramId = 0;
			return;
		}
		offset -= 1;
		if (offset < numUnpatchedEmitted) {
			target = MorphTarget::UNPATCHED;
			paramId = offset;
			if (isSong) {
				// The two skipped ids, ascending, so each shift is seen by the next test.
				if (paramId >= params::UNPATCHED_MORPH) {
					paramId++;
				}
				if (paramId >= params::UNPATCHED_MORPH_SLOT) {
					paramId++;
				}
			}
			return;
		}
		target = MorphTarget::PATCHED;
		paramId = offset - numUnpatchedEmitted;
	}
};

OwnerLayout layoutOf(TimelineCounter* tc, ModControllable* mc, ParamManager* pm, bool isSong, bool isMidi) {
	OwnerLayout layout;
	layout.isSong = isSong;
	layout.isMidi = isMidi;
	if (!tc || !mc || !pm || !pm->containsAnyMainParamCollections()) {
		return layout;
	}
	layout.valid = true;
	if (isMidi) {
		layout.midi = pm->getMIDIParamCollection();
		return layout;
	}
	UnpatchedParamSet* unpatched = pm->getUnpatchedParamSet();
	if (unpatched) {
		layout.numUnpatched = unpatched->getNumParams();
		layout.numUnpatchedEmitted = layout.numUnpatched;
		if (isSong) {
			layout.numUnpatchedEmitted -= (params::UNPATCHED_MORPH < layout.numUnpatched) ? 1 : 0;
			layout.numUnpatchedEmitted -= (params::UNPATCHED_MORPH_SLOT < layout.numUnpatched) ? 1 : 0;
		}
	}
	if (pm->containsPatchedParamCollections()) {
		PatchedParamSet* patched = pm->getPatchedParamSet();
		if (patched) {
			layout.numPatched = patched->getNumParams();
		}
	}
	return layout;
}

/// The number of positions a full walk would produce, at the cost of one call per owner.
int32_t countSong() {
	int32_t total = 0;
	walkOwners([&](TimelineCounter* tc, ModControllable* mc, ParamManager* pm, NoteRow*, int32_t, bool isSong,
	               bool isMidi) { total += layoutOf(tc, mc, pm, isSong, isMidi).size(); });
	return total;
}

/// Number of positions appended by MIDI CC support. Used only to recognize an older saved bank
/// whose walk is the unchanged audio prefix of the current walk.
int32_t countMidiTail() {
	Song* song = currentSong;
	if (!song) {
		return 0;
	}
	int32_t total = 0;
	ClipArray* arrays[2] = {&song->sessionClips, &song->arrangementOnlyClips};
	for (ClipArray* array : arrays) {
		for (int32_t i = 0; i < array->getNumElements(); i++) {
			Clip* clip = array->getClipAtIndex(i);
			if (clip && clip->output && clip->output->type == OutputType::MIDI_OUT
			    && clip->paramManager.containsAnyMainParamCollections()) {
				total += clip->paramManager.getMIDIParamCollection()->params.getNumElements();
			}
		}
	}
	return total;
}

/// A slot is sparse, so it can differ from the baseline in more places than one morph can
/// carry: you drift further from slot 1 with every capture. 2048 entries is 16KB, and with the
/// baseline the whole bank tops out near 290KB -- see the SDRAM note in the phase 2 handoff.
constexpr int32_t kMaxSlotEntries = 2048;

/// Reads one slot's value at successive walk positions. Positions must be asked for in
/// ascending order, which is how the walk produces them, so reading a slot is one forward merge
/// of its entries over the baseline and never a search.
struct SlotReader {
	MorphSlot const* slot;
	int32_t const* baseline;
	int32_t next = 0;

	int32_t at(int32_t index) {
		while (next < slot->count && slot->entries[next].index < index) {
			next++;
		}
		if (next < slot->count && slot->entries[next].index == index) {
			return slot->entries[next].value;
		}
		return baseline[index];
	}
};

/// "SLOT 8", "SLOT 8  moved 12" -- slot numbers are shown one-based, as on the knob.
char const* slotMessage(char const* label, int32_t slot, char const* tail = nullptr, int32_t n = -1) {
	strcpy(popupBuffer, label);
	intToString(slot + 1, popupBuffer + strlen(popupBuffer), 1);
	if (tail) {
		strcat(popupBuffer, tail);
	}
	if (n >= 0) {
		intToString(n, popupBuffer + strlen(popupBuffer), 1);
	}
	return popupBuffer;
}

/// "3 TO 4  moved 12", "3 TO 7  no change" -- names both ends of the morph, so
/// after a press you know what the upper knob sweeps between. fromSlot < 0 means the live song.
char const* fromToMessage(int32_t fromSlot, int32_t toSlot, char const* tail = nullptr, int32_t n = -1,
                          bool fromOld = false) {
	popupBuffer[0] = 0;
	if (fromOld && fromSlot >= 0) {
		strcat(popupBuffer, "OLD "); // replacing the slot you were on: A is its previous version
	}
	if (fromSlot < 0) {
		strcat(popupBuffer, "SLOT "); // nothing to come from: just name the slot
	}
	else {
		intToString(fromSlot + 1, popupBuffer + strlen(popupBuffer), 1);
		strcat(popupBuffer, " TO ");
	}
	intToString(toSlot + 1, popupBuffer + strlen(popupBuffer), 1);
	if (tail) {
		strcat(popupBuffer, tail);
	}
	if (n >= 0) {
		intToString(n, popupBuffer + strlen(popupBuffer), 1);
	}
	return popupBuffer;
}

} // namespace

void Morph::setPosition(int32_t raw) {
	Song* song = currentSong;
	if (!song) {
		return;
	}

	char stackMemory[MODEL_STACK_MAX_SIZE];
	ModelStack* modelStack = setupModelStackWithSong(stackMemory, song);
	ModelStackWithThreeMainThings* three = modelStack->addTimelineCounter(song)->addOtherTwoThingsButNoNoteRow(
	    &song->globalEffectable, &song->paramManager);
	if (!three) {
		return;
	}

	ModelStackWithAutoParam* withParam = three->getUnpatchedAutoParamFromId(params::UNPATCHED_MORPH);
	if (!withParam || !withParam->autoParam) {
		return;
	}

	withParam->autoParam->setCurrentValueWithNoReversionOrRecording(withParam, raw);

	// Tell apply() this position is already reflected in the song, so it does not immediately
	// write the whole delta list again for a value nothing actually moved to.
	lastAppliedPos = raw;
	everApplied = true;

	view.setKnobIndicatorLevels();
}

void Morph::freeBank() {
	if (baseline) {
		delugeDealloc(baseline);
		baseline = nullptr;
	}
	baselineCount = 0;
	for (MorphSlot& slot : slots) {
		if (slot.entries) {
			delugeDealloc(slot.entries);
		}
		slot = MorphSlot{};
	}
	numSlotsUsed = 0;
	referenceSlot = -1;
}

void Morph::freeDeltas() {
	if (deltas) {
		delugeDealloc(deltas);
		deltas = nullptr;
	}
	numDeltas = 0;
	everApplied = false;
	endSlotA = -1;
	endSlotB = -1;
	endAIsOld = false;
}

void Morph::showEnd(bool atB) {
	int32_t slot = atB ? endSlotB : endSlotA;
	if (numDeltas <= 0 || slot < 0) {
		return;
	}
	showResult(slotMessage((!atB && endAIsOld) ? "OLD SLOT " : "SLOT ", slot));
}

void Morph::clear() {
	freeBank();
	freeDeltas();
}

void Morph::startBank(int32_t* walk, int32_t count, int32_t slot) {
	freeBank();
	// The walk buffer was sized for the largest song; keep only what this one used.
	if (count > 0) {
		GeneralMemoryAllocator::get().shortenRight(walk, count * sizeof(int32_t));
	}
	// The baseline is a hidden reference, not a slot. The first slot captured is identical to
	// it, so it has no entries -- but it is an ordinary slot all the same, and replacing it later
	// only gives it entries. The baseline itself never changes, so no other slot moves.
	baseline = walk;
	baselineCount = count;
	slots[slot].used = true;
	numSlotsUsed = 1;
	referenceSlot = slot;
}

void Morph::showSlot(int32_t slot) {
	if (slot < 0 || slot >= kMorphNumSlots) {
		return;
	}
	showResult(slots[slot].used ? slotMessage("SLOT ", slot) : slotMessage("SLOT ", slot, " empty"));
}

void Morph::capture() {
	captureInto(-1);
}

void Morph::replaceSlot(int32_t slot) {
	if (slot < 0 || slot >= kMorphNumSlots) {
		return;
	}
	captureInto(slot);
}

void Morph::captureInto(int32_t targetSlot) {
	// Capture is reached from a gold-knob press, and the knob's own value popup is throttled
	// and replayed on a timer -- without this, every message below is overwritten by a stale
	// "MORPH: n" a fraction of a second after it appears.
	view.cancelPendingModEncoderValuePopup();

	static char const* kMsgMem[2] = {"MEM", "Out of memory"};
	static char const* kMsgFull[2] = {"FULL", "Song too large to snapshot"};

	// A plain capture takes the first empty slot and never overwrites; a full bank stays full.
	// Replacing is the deliberate gesture for reusing a slot.
	const bool replacing = (targetSlot >= 0);
	int32_t slot = targetSlot;
	if (!replacing) {
		for (int32_t s = 0; s < kMorphNumSlots; s++) {
			if (!slots[s].used) {
				slot = s;
				break;
			}
		}
		if (slot < 0) {
			showResult("BANK FULL");
			return;
		}
	}

	// Size the walk buffer to this song. The count is one call per owner, so it costs nothing.
	const int32_t capacity = countSong();
	if (capacity > kMaxSnapshotValues) {
		display->displayPopup(kMsgFull);
		return;
	}
	auto* now = static_cast<int32_t*>(
	    GeneralMemoryAllocator::get().allocLowSpeed((capacity > 0 ? capacity : 1) * sizeof(int32_t)));
	if (!now) {
		display->displayPopup(kMsgMem);
		return;
	}

	const bool haveBank = (baseline != nullptr);
	const bool havePrev = haveBank && referenceSlot >= 0 && slots[referenceSlot].used;
	MorphDelta* newDeltas = nullptr;
	if (havePrev) {
		newDeltas =
		    static_cast<MorphDelta*>(GeneralMemoryAllocator::get().allocLowSpeed(kMaxDeltas * sizeof(MorphDelta)));
		if (!newDeltas) {
			delugeDealloc(now);
			display->displayPopup(kMsgMem);
			return;
		}
	}

	SlotReader prev{havePrev ? &slots[referenceSlot] : nullptr, baseline};
	int32_t count = 0;
	int32_t found = 0;
	int32_t differsFromBaseline = 0;
	bool overflowed = false;

	// One walk does three jobs: it records the state, counts how far it has drifted from the
	// baseline (the size of the slot it will become), and, where it differs from the slot it is
	// morphing from, records the identity needed to write the param back later.
	walkSong([&](TimelineCounter* tc, ModControllable* mc, ParamManager* pm, NoteRow* nr, int32_t noteRowId,
	             MorphTarget target, int32_t paramId, int32_t value) {
		if (count < capacity) {
			now[count] = value;
			if (haveBank && count < baselineCount) {
				if (baseline[count] != value) {
					differsFromBaseline++;
				}
				if (havePrev) {
					int32_t from = prev.at(count);
					if (from != value) {
						if (found < kMaxDeltas) {
							MorphDelta& d = newDeltas[found++];
							d.index = count;
							d.valueA = from;
							d.valueB = value;
						}
						else {
							overflowed = true;
						}
					}
				}
			}
		}
		count++;
	});

	auto discard = [&]() {
		delugeDealloc(now);
		if (newDeltas) {
			delugeDealloc(newDeltas);
		}
	};

	// The count and the walk disagree only if the song changed between the two, which it cannot
	// on this thread -- but never trust a buffer size you did not check.
	if (count != capacity) {
		discard();
		display->displayPopup(kMsgFull);
		return;
	}

	// First capture: this walk becomes the baseline, and the slot it went into matches it
	// exactly. Nothing to morph from yet.
	if (!haveBank) {
		freeDeltas();
		startBank(now, count, slot);
		setPosition(params::kMorphDefault);
		showResult(slotMessage("SLOT ", slot));
		return;
	}

	// The walk produced a different number of params than the baseline, so the song's clips or
	// rows changed and every slot's positions now point at the wrong params. All sixteen go at
	// once, and loudly: losing them silently is worse than being told. This capture starts the
	// new bank, in the slot it was aimed at.
	if (count != baselineCount) {
		if (newDeltas) {
			delugeDealloc(newDeltas);
		}
		freeDeltas();
		startBank(now, count, slot);
		setPosition(params::kMorphDefault);
		showResult("BANK RESET  song changed");
		return;
	}

	// Too much changed to morph, or to store. Refuse and leave the bank and the current morph
	// exactly as they were.
	if (overflowed || differsFromBaseline > kMaxSlotEntries) {
		discard();
		showResult(withNumber("TOO MANY  moved ", overflowed ? found : differsFromBaseline));
		return;
	}

	// A plain capture identical to the slot it would morph from would only fill the bank with a
	// duplicate, and the current morph is still valid, so leave it armed. A replace goes ahead
	// regardless: you asked for that slot to hold this state.
	if (found == 0 && !replacing) {
		discard();
		showResult(slotMessage("NO CHANGE  from slot ", referenceSlot));
		return;
	}

	MorphSlotEntry* entries = nullptr;
	if (differsFromBaseline > 0) {
		entries = static_cast<MorphSlotEntry*>(
		    GeneralMemoryAllocator::get().allocLowSpeed(differsFromBaseline * sizeof(MorphSlotEntry)));
		if (!entries) {
			discard();
			display->displayPopup(kMsgMem);
			return;
		}
		int32_t e = 0;
		for (int32_t i = 0; i < count && e < differsFromBaseline; i++) {
			if (now[i] != baseline[i]) {
				entries[e].index = i;
				entries[e].value = now[i];
				e++;
			}
		}
	}
	delugeDealloc(now);

	// Only now is the old content let go: the walk above read it through `prev` if this slot was
	// also the one being morphed from.
	MorphSlot& dest = slots[slot];
	const bool wasUsed = dest.used;
	const int32_t fromSlot = referenceSlot;
	// Replacing the slot you are on: the sweep runs from its old version to its new one.
	const bool fromOld = replacing && (fromSlot == slot) && dest.used;
	if (dest.entries) {
		delugeDealloc(dest.entries);
	}
	dest.entries = entries;
	dest.count = differsFromBaseline;
	dest.used = true;
	if (!wasUsed) {
		numSlotsUsed++;
	}
	referenceSlot = slot;

	freeDeltas();
	if (found > 0) {
		deltas = newDeltas;
		deltaWalkCount = count;
		numDeltas = found;
		endSlotA = fromSlot;
		endSlotB = slot;
		endAIsOld = fromOld;
		// B is the state the song is in right now -- you just stamped it. Parking the position
		// at the B end means capturing changes nothing you can hear, and turning sweeps back
		// toward A.
		setPosition(2147483647);
	}
	else if (newDeltas) {
		delugeDealloc(newDeltas);
	}
	// Name both ends. A replace also says so, since the slot number alone looks like a capture.
	if (found > 0) {
		showResult(
		    fromToMessage(fromSlot, slot, (replacing && wasUsed) ? "  REPLACED  moved " : "  moved ", found, fromOld));
	}
	else {
		showResult(fromToMessage(fromSlot, slot, (replacing && wasUsed) ? "  REPLACED  no change" : "  no change", -1,
		                         fromOld));
	}
}

bool Morph::cue(int32_t slot) {
	view.cancelPendingModEncoderValuePopup();

	static char const* kMsgMem[2] = {"MEM", "Out of memory"};

	if (!isSlotUsed(slot) || !baseline) {
		showSlot(slot); // "SLOT 7 empty"
		return false;
	}

	auto* newDeltas =
	    static_cast<MorphDelta*>(GeneralMemoryAllocator::get().allocLowSpeed(kMaxDeltas * sizeof(MorphDelta)));
	if (!newDeltas) {
		display->displayPopup(kMsgMem);
		return false;
	}

	// A is the song as it is right now, B is the slot. No stored state is needed for A: the walk
	// reads it live, which is what makes a cue mean "take me there from here".
	SlotReader target{&slots[slot], baseline};
	int32_t count = 0;
	int32_t found = 0;
	bool overflowed = false;

	walkSong([&](TimelineCounter* tc, ModControllable* mc, ParamManager* pm, NoteRow* nr, int32_t noteRowId,
	             MorphTarget kind, int32_t paramId, int32_t value) {
		if (count < baselineCount) {
			int32_t to = target.at(count);
			if (to != value) {
				if (found < kMaxDeltas) {
					MorphDelta& d = newDeltas[found++];
					d.index = count;
					d.valueA = value;
					d.valueB = to;
				}
				else {
					overflowed = true;
				}
			}
		}
		count++;
	});

	if (count != baselineCount) {
		delugeDealloc(newDeltas);
		clear();
		setPosition(params::kMorphDefault);
		showResult("BANK RESET  song changed");
		return false;
	}

	if (overflowed) {
		delugeDealloc(newDeltas);
		showResult(withNumber("TOO MANY  moved ", found));
		return false;
	}

	freeDeltas();
	// Show where you were -- the slot captured or cued last -- not "NOW", which does not say.
	const int32_t cameFrom = referenceSlot;
	referenceSlot = slot;

	if (found == 0) {
		delugeDealloc(newDeltas);
		showResult(fromToMessage(cameFrom, slot, "  no change"));
		return true;
	}

	deltas = newDeltas;
	deltaWalkCount = count;
	numDeltas = found;
	endSlotA = cameFrom;
	endSlotB = slot;
	endAIsOld = false;

	// Park at the A end: A is what is sounding, so cueing changes nothing you hear until the
	// upper knob is turned.
	setPosition(params::kMorphDefault);
	showResult(fromToMessage(cameFrom, slot, "  moved ", found));
	return true;
}

void Morph::apply() {
	if (numDeltas <= 0) {
		return;
	}

	Song* song = currentSong;
	if (!song) {
		return;
	}

	UnpatchedParamSet* songUnpatched = song->paramManager.getUnpatchedParamSet();
	if (!songUnpatched) {
		return;
	}

	int32_t raw = songUnpatched->getValue(params::UNPATCHED_MORPH);
	if (everApplied && raw == lastAppliedPos) {
		return;
	}

	if (++applyDivider < kApplyEveryNWindows) {
		return;
	}
	applyDivider = 0;

	// The delta list holds walk POSITIONS, never pointers. Clips, kit rows and ParamManagers are
	// freed and moved by ordinary edits -- adding a drum reallocates every NoteRow in the kit,
	// deleting a clip frees its ParamManager -- and a delta list of raw pointers kept writing
	// through them from the render path: the hard fault of 2026-09-26. So every apply re-walks
	// the live song's owners and takes its pointers fresh.
	//
	// If the song no longer walks to the count the deltas were built against, the positions mean
	// different params now. Drop the morph rather than write into the wrong ones.
	if (countSong() != deltaWalkCount) {
		freeDeltas();
		return;
	}

	lastAppliedPos = raw;
	everApplied = true;

	// 0 at the A end, 65536 at the B end.
	int32_t frac = static_cast<int32_t>((static_cast<int64_t>(raw) + 2147483648LL) >> 16);

	auto interpolate = [frac](int32_t a, int32_t b) -> int32_t {
		if (frac <= 0) {
			return a;
		}
		if (frac >= 65535) {
			return b;
		}
		return a + static_cast<int32_t>(((static_cast<int64_t>(b) - a) * frac) >> 16);
	};

	// Static, not on the stack: this runs inside the audio render path. Never re-entered.
	static char stackMemory[MODEL_STACK_MAX_SIZE];
	ModelStack* modelStack = setupModelStackWithSong(stackMemory, song);

	// Owner by owner: skip every owner that holds no delta, and inside one that does, jump
	// straight to each delta's param. Cost is one step per owner plus one per delta.
	int32_t base = 0;
	int32_t next = 0;
	walkOwners([&](TimelineCounter* tc, ModControllable* mc, ParamManager* pm, NoteRow* nr, int32_t noteRowId,
	               bool isSong, bool isMidi) {
		if (next >= numDeltas) {
			return;
		}
		OwnerLayout layout = layoutOf(tc, mc, pm, isSong, isMidi);
		int32_t end = base + layout.size();
		ModelStackWithThreeMainThings* three = nullptr;

		while (next < numDeltas && deltas[next].index < end) {
			MorphDelta const& d = deltas[next++];
			if (d.index < base) {
				continue; // cannot happen with sorted deltas; never write a position we cannot place
			}
			MorphTarget target;
			int32_t paramId;
			layout.locate(d.index - base, target, paramId);

			if (target == MorphTarget::MIDI_CC) {
				MIDIParamCollection* midi = pm->getMIDIParamCollection();
				MIDIParam* midiParam = midi->params.getParamFromCC(paramId);
				if (!midiParam || midiParam->param.isAutomated()) {
					continue;
				}
				if (!three) {
					ModelStackWithTimelineCounter* withCounter = modelStack->addTimelineCounter(tc);
					three = withCounter->addOtherTwoThingsButNoNoteRow(mc, pm);
					if (!three) {
						break;
					}
				}
				ParamCollectionSummary* summary = pm->getMIDIParamCollectionSummary();
				ModelStackWithParamCollection* withCollection = three->addParamCollection(midi, summary);
				ModelStackWithAutoParam* withParam = withCollection->addAutoParam(paramId, &midiParam->param);
				int32_t oldValue = midiParam->param.getCurrentValue();
				midiParam->param.setCurrentValueWithNoReversionOrRecording(withParam, interpolate(d.valueA, d.valueB));
				midi->notifyParamModifiedInSomeWay(withParam, oldValue, false, false, false);
				continue;
			}

			// Saturation is not a param -- write the member directly, interpolated like the rest.
			if (target == MorphTarget::CLIPPING) {
				int32_t v = interpolate(d.valueA, d.valueB);
				v = (v < 0) ? 0 : ((v > 255) ? 255 : v);
				static_cast<ModControllableAudio*>(mc)->clippingAmount = static_cast<uint8_t>(v);
				continue;
			}

			if (!three) {
				ModelStackWithTimelineCounter* withCounter = modelStack->addTimelineCounter(tc);
				three = nr ? withCounter->addNoteRow(noteRowId, nr)->addOtherTwoThings(mc, pm)
				           : withCounter->addOtherTwoThingsButNoNoteRow(mc, pm);
				if (!three) {
					break;
				}
			}

			ModelStackWithAutoParam* withParam = (target == MorphTarget::PATCHED)
			                                         ? three->getPatchedAutoParamFromId(paramId)
			                                         : three->getUnpatchedAutoParamFromId(paramId);
			if (!withParam || !withParam->autoParam) {
				continue;
			}

			// An automated param writes its own value from its nodes on every tick, so a morph
			// write would be overwritten and the control would go dead under the hand.
			if (withParam->autoParam->isAutomated()) {
				continue;
			}

			withParam->autoParam->setCurrentValueWithNoReversionOrRecording(withParam, interpolate(d.valueA, d.valueB));
		}
		// Anything left in this owner's range after a failed stack build is skipped, not carried.
		while (next < numDeltas && deltas[next].index < end) {
			next++;
		}
		base = end;
	});
}

// ------------------------------------------------------------------------------------------------
// Saving the bank with the song.
//
// The song file already holds every param at the moment of saving, so that state is the reference:
// each slot is written as only the walk positions where it differs from the song as saved. On load
// the song as loaded IS that state, so it becomes the new baseline and each slot's entries apply on
// top of it unchanged. Nothing baseline-sized is ever written.
//
// Positions are only meaningful for the walk they came from, so every slot carries the walk count.
// If the loaded song does not walk to the same count, the bank is dropped rather than applied to the
// wrong params.
// ------------------------------------------------------------------------------------------------

namespace {
/// A saved slot can differ from the saved song in more places than from the baseline, since the song
/// itself has moved on. 8192 entries is 64KB in memory, 128KB of hex in the file.
constexpr int32_t kMaxSavedEntries = 8192;
} // namespace

void Morph::writeToFile(Serializer& writer) {
	if (!baseline || numSlotsUsed <= 0 || !currentSong) {
		return;
	}
	const int32_t capacity = countSong();
	if (capacity != baselineCount || capacity <= 0) {
		return; // the song changed shape since the bank was captured: its positions mean nothing now
	}
	auto* now = static_cast<int32_t*>(GeneralMemoryAllocator::get().allocLowSpeed(capacity * sizeof(int32_t)));
	if (!now) {
		return;
	}
	int32_t count = 0;
	walkSong(
	    [&](TimelineCounter*, ModControllable*, ParamManager*, NoteRow*, int32_t, MorphTarget, int32_t, int32_t value) {
		    if (count < capacity) {
			    now[count] = value;
		    }
		    count++;
	    });
	if (count != capacity) {
		delugeDealloc(now);
		return;
	}

	// Which two slots the upper knob sweeps between, so it comes back armed. An "OLD" end (the
	// previous version of a replaced slot) exists nowhere once replaced, so that pair is not saved.
	int32_t endA = (numDeltas > 0 && !endAIsOld) ? endSlotA : -1;
	int32_t endB = (numDeltas > 0) ? endSlotB : -1;
	if (endA < 0 || endB < 0 || endA == endB) {
		endA = endB = -1;
	}

	writer.writeArrayStart("morphBank");
	for (int32_t s = 0; s < kMorphNumSlots; s++) {
		if (!slots[s].used) {
			continue;
		}
		// Two forward passes: count, then fill an exact-size buffer.
		int32_t n = 0;
		{
			SlotReader reader{&slots[s], baseline};
			for (int32_t i = 0; i < count; i++) {
				if (reader.at(i) != now[i]) {
					n++;
				}
			}
		}
		if (n > kMaxSavedEntries) {
			continue;
		}
		MorphSlotEntry* entries = nullptr;
		if (n > 0) {
			entries =
			    static_cast<MorphSlotEntry*>(GeneralMemoryAllocator::get().allocLowSpeed(n * sizeof(MorphSlotEntry)));
			if (!entries) {
				continue;
			}
			SlotReader reader{&slots[s], baseline};
			int32_t e = 0;
			for (int32_t i = 0; i < count && e < n; i++) {
				int32_t v = reader.at(i);
				if (v != now[i]) {
					entries[e].index = i;
					entries[e].value = v;
					e++;
				}
			}
		}
		writer.writeOpeningTagBeginning("morphSlot", true);
		writer.writeAttribute("n", s, false);
		writer.writeAttribute("walk", count, false);
		writer.writeAttribute("entries", n, false);
		if (s == endA) {
			writer.writeAttribute("end", 1, false);
		}
		else if (s == endB) {
			writer.writeAttribute("end", 2, false);
		}
		if (entries) {
			writer.writeAttributeHexBytes("data", reinterpret_cast<uint8_t*>(entries), n * sizeof(MorphSlotEntry),
			                              false);
			delugeDealloc(entries);
		}
		writer.closeTag(true);
	}
	writer.writeArrayEnding("morphBank");
	delugeDealloc(now);
}

void Morph::discardPending() {
	for (MorphSlot& slot : pendingSlots) {
		if (slot.entries) {
			delugeDealloc(slot.entries);
		}
		slot = MorphSlot{};
	}
	pendingWalkCount = -1;
	pendingSong = nullptr;
	pendingEndA = -1;
	pendingEndB = -1;
}

void Morph::beginReading(Song* song) {
	discardPending();
	pendingSong = song;
}

void Morph::readSlotFromFile(Deserializer& reader) {
	int32_t n = -1;
	int32_t walk = -1;
	int32_t numEntries = 0;
	MorphSlotEntry* entries = nullptr;
	int32_t bytesRead = 0;
	int32_t end = 0;

	reader.match('{');
	char const* tagName;
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "n")) {
			n = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "end")) {
			end = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "walk")) {
			walk = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "entries")) {
			numEntries = reader.readTagOrAttributeValueInt();
			if (numEntries < 0 || numEntries > kMaxSavedEntries) {
				numEntries = -1; // poison: the slot is rejected below
			}
		}
		else if (!strcmp(tagName, "data") && numEntries > 0 && !entries) {
			entries = static_cast<MorphSlotEntry*>(
			    GeneralMemoryAllocator::get().allocLowSpeed(numEntries * sizeof(MorphSlotEntry)));
			if (entries) {
				bytesRead = reader.readTagOrAttributeValueHexBytes(reinterpret_cast<uint8_t*>(entries),
				                                                   numEntries * sizeof(MorphSlotEntry));
			}
		}
		reader.exitTag(tagName);
	}
	reader.match('}');

	// Keep the slot only if it is whole and self-consistent: in range, same walk as the others,
	// every byte present, indices strictly ascending and inside the walk.
	bool ok = pendingSong != nullptr && n >= 0 && n < kMorphNumSlots && walk > 0 && numEntries >= 0
	          && !pendingSlots[n].used && (pendingWalkCount < 0 || pendingWalkCount == walk);
	if (ok && numEntries > 0) {
		ok = entries && bytesRead == numEntries * static_cast<int32_t>(sizeof(MorphSlotEntry));
		for (int32_t i = 0; ok && i < numEntries; i++) {
			if (entries[i].index < 0 || entries[i].index >= walk
			    || (i > 0 && entries[i].index <= entries[i - 1].index)) {
				ok = false;
			}
		}
	}
	if (!ok) {
		if (entries) {
			delugeDealloc(entries);
		}
		return;
	}
	pendingWalkCount = walk;
	pendingSlots[n].entries = (numEntries > 0) ? entries : nullptr;
	pendingSlots[n].count = numEntries;
	pendingSlots[n].used = true;
	if (end == 1) {
		pendingEndA = n;
	}
	else if (end == 2) {
		pendingEndB = n;
	}
}

void Morph::adoptPending() {
	Song* song = pendingSong;
	bool any = false;
	for (MorphSlot const& slot : pendingSlots) {
		any = any || slot.used;
	}
	if (!song || song != currentSong || !any || pendingWalkCount <= 0) {
		discardPending();
		return;
	}

	const int32_t capacity = countSong();
	const bool legacyAudioOnlyWalk = pendingWalkCount + countMidiTail() == capacity;
	if (capacity != pendingWalkCount && !legacyAudioOnlyWalk) {
		discardPending();
		showResult("BANK RESET  song changed");
		return;
	}
	auto* walkNow = static_cast<int32_t*>(GeneralMemoryAllocator::get().allocLowSpeed(capacity * sizeof(int32_t)));
	if (!walkNow) {
		discardPending();
		return;
	}
	int32_t count = 0;
	walkSong(
	    [&](TimelineCounter*, ModControllable*, ParamManager*, NoteRow*, int32_t, MorphTarget, int32_t, int32_t value) {
		    if (count < capacity) {
			    walkNow[count] = value;
		    }
		    count++;
	    });
	if (count != capacity) {
		delugeDealloc(walkNow);
		discardPending();
		return;
	}

	// The loaded song is the state every saved slot was written against: it is the baseline.
	clear();
	baseline = walkNow;
	baselineCount = count;
	numSlotsUsed = 0;
	for (int32_t s = 0; s < kMorphNumSlots; s++) {
		slots[s] = pendingSlots[s];
		pendingSlots[s] = MorphSlot{}; // ownership moved, do not free
		if (slots[s].used) {
			numSlotsUsed++;
		}
	}
	pendingWalkCount = -1;
	pendingSong = nullptr;

	// Morph from the slot the selector points at if it holds something, else the first one.
	referenceSlot = -1;
	UnpatchedParamSet* songUnpatched = currentSong->paramManager.getUnpatchedParamSet();
	if (songUnpatched) {
		int32_t selected = morphSlotFromValue(songUnpatched->getValue(params::UNPATCHED_MORPH_SLOT));
		if (slots[selected].used) {
			referenceSlot = selected;
		}
	}
	for (int32_t s = 0; referenceSlot < 0 && s < kMorphNumSlots; s++) {
		if (slots[s].used) {
			referenceSlot = s;
		}
	}

	// Re-arm the upper knob between the two slots it was saved between. The song was saved with the
	// knob wherever it was, so that position counts as already applied: nothing moves on load, and
	// the next turn of the knob carries on from there.
	const int32_t a = pendingEndA;
	const int32_t b = pendingEndB;
	pendingEndA = -1;
	pendingEndB = -1;
	if (a < 0 || b < 0 || a == b || a >= kMorphNumSlots || b >= kMorphNumSlots || !slots[a].used || !slots[b].used) {
		return;
	}
	auto* newDeltas =
	    static_cast<MorphDelta*>(GeneralMemoryAllocator::get().allocLowSpeed(kMaxDeltas * sizeof(MorphDelta)));
	if (!newDeltas) {
		return;
	}
	SlotReader readA{&slots[a], baseline};
	SlotReader readB{&slots[b], baseline};
	int32_t found = 0;
	for (int32_t i = 0; i < count; i++) {
		int32_t va = readA.at(i);
		int32_t vb = readB.at(i);
		if (va != vb) {
			if (found >= kMaxDeltas) {
				found = -1;
				break;
			}
			newDeltas[found++] = MorphDelta{i, va, vb};
		}
	}
	if (found <= 0) {
		delugeDealloc(newDeltas);
		return;
	}
	deltas = newDeltas;
	numDeltas = found;
	deltaWalkCount = count;
	endSlotA = a;
	endSlotB = b;
	endAIsOld = false;
	referenceSlot = b;
	if (songUnpatched) {
		lastAppliedPos = songUnpatched->getValue(params::UNPATCHED_MORPH);
		everApplied = true;
	}
}
