/*
 * Copyright © 2026 Rudement
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

#include "model/instrument/kit_split.h"
#include "definitions_cxx.hpp"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "gui/views/session_view.h"
#include "hid/display/display.h"
#include "io/midi/midi_follow.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/clip/clip_instance.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/global_effectable/global_effectable_for_clip.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "playback/mode/playback_mode.h"
#include "playback/mode/session.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "storage/storage_manager.h"
#include "util/d_string.h"
#include <algorithm>

namespace params = deluge::modulation::params;

namespace deluge::model::kit_split {

namespace {

/// Sanity bound. A kit can hold more rows than this, but splitting hundreds of tracks in one keypress is not a
/// thing anyone means to do, and it keeps the pre-allocation arrays on the stack.
constexpr int32_t kMaxSplitKits = 64;

/// Append digits until no Instrument in the song (hibernating or not) holds this name.
void makeKitNameUnique(String* name, Song* song) {
	if (!song->getInstrumentFromPresetSlot(OutputType::KIT, 0, 0, name->get(), "KITS", true, true)) {
		return;
	}

	String base;
	base.set(name);

	for (int32_t suffix = 2; suffix < 1000; suffix++) {
		String candidate;
		candidate.set(&base);
		if (candidate.concatenateInt(suffix) != Error::NONE) {
			return; // Out of RAM for a name - keep the colliding one rather than failing the whole split.
		}
		if (!song->getInstrumentFromPresetSlot(OutputType::KIT, 0, 0, candidate.get(), "KITS", true, true)) {
			name->set(&candidate);
			return;
		}
	}
}

/// How many source Clips one Kit may carry into a split. Variations of a part, not an archive.
constexpr int32_t kMaxSourceClips = 16;

/// Every session Clip that plays this Kit, in session-list order. Returns the true count even when it exceeds
/// `max` (only the first `max` are stored), so the caller can refuse rather than silently split a subset.
int32_t gatherKitClips(Song* song, Kit* kit, InstrumentClip** out, int32_t max) {
	int32_t count = 0;
	for (int32_t c = 0; c < song->sessionClips.getNumElements(); c++) {
		Clip* clip = song->sessionClips.getClipAtIndex(c);
		if (clip->output != kit) {
			continue;
		}
		if (count < max) {
			out[count] = (InstrumentClip*)clip;
		}
		count++;
	}
	return count;
}

/// True if any Clip on this Kit has notes for this Drum. Kit-wide, deliberately: a Drum whose part lives only in
/// the variation still has to come out of the split, or that variation's part is silently thrown away.
bool drumHasNotesOnKit(Drum* drum, InstrumentClip** clips, int32_t numClips) {
	for (int32_t m = 0; m < numClips; m++) {
		NoteRow* noteRow = clips[m]->getNoteRowForDrum(drum);
		if (noteRow && !noteRow->hasNoNotes()) {
			return true;
		}
	}
	return false;
}

/// The Drums this split will produce Kits for, in the order they will appear. Returns the true count even when it
/// exceeds `max` (only the first `max` are stored), so the menu can name a real number and the caller can refuse.
int32_t collectDrums(InstrumentClip* invoked, Kit* kit, InstrumentClip** clips, int32_t numClips, Drum** out,
                     int32_t max) {
	int32_t count = 0;

	// The invoked Clip's row order first, so the split reads across the grid the way its rows read up the screen.
	//
	// Deduplicated even though two NoteRows should never share a Drum. If one ever did, Pass 2 would run the move
	// sequence twice: the second removeDrumFromLinkedList() is a silent no-op, and addDrum() would then link the same
	// Drum into a second Kit, and both would free it. A refusal to split is a better failure than a double free.
	for (int32_t i = 0; i < invoked->noteRows.getNumElements(); i++) {
		Drum* drum = invoked->noteRows.getElement(i)->drum;
		if (!drum || !drumHasNotesOnKit(drum, clips, numClips)) {
			continue;
		}
		bool already = false;
		for (int32_t j = 0; j < count && j < max; j++) {
			if (out[j] == drum) {
				already = true;
				break;
			}
		}
		if (already) {
			continue;
		}
		if (count < max) {
			out[count] = drum;
		}
		count++;
	}

	// Then any Drum that only carries notes in one of the other Clips.
	for (Drum* drum = kit->firstDrum; drum != nullptr; drum = drum->next) {
		bool already = false;
		for (int32_t i = 0; i < count && i < max; i++) {
			if (out[i] == drum) {
				already = true;
				break;
			}
		}
		if (already || !drumHasNotesOnKit(drum, clips, numClips)) {
			continue;
		}
		if (count < max) {
			out[count] = drum;
		}
		count++;
	}

	return count;
}

/// Free a Clip that was cloned but never handed to the song.
void discardUnusedClip(Clip* clip) {
	if (!clip) {
		return;
	}
	// Song::deleteClipObject() would do this via prepareForDestruction(). We can't use that path - these Clips were
	// never handed to the song - but we still must not leave freed Clip pointers as search keys in the song's
	// backed-up ParamManager list.
	currentSong->deleteBackedUpParamManagersForClip(clip);
	void* toDealloc = dynamic_cast<void*>(clip);
	clip->~Clip();
	delugeDealloc(toDealloc);
}

/// Free a Kit that was created but never added to the song.
void discardUnusedKit(Kit* kit) {
	if (!kit) {
		return;
	}
	void* toDealloc = dynamic_cast<void*>(kit);
	kit->~Kit();
	delugeDealloc(toDealloc);
}

/// Splice `newOutput` into the song's output list immediately after `afterThis`. Song::addOutput() only offers
/// the head or the tail; this is the interior case, used to drop a split-out Kit back where the source Kit used
/// to sit rather than always at the end of the list. Mirrors the bookkeeping addOutput() does for the tail case -
/// a freshly created Kit can never be mid-solo, so there is no anyOutputsSoloingInArrangement check to repeat.
void insertOutputAfter(Song* song, Output* afterThis, Output* newOutput) {
	newOutput->next = afterThis->next;
	afterThis->next = newOutput;

	if (song == currentSong) {
		newOutput->resyncLFOs();
	}
}

} // namespace

int32_t countSplittableRows(InstrumentClip* clip) {
	if (!clip || clip->output == nullptr || clip->output->type != OutputType::KIT || currentSong == nullptr) {
		return 0;
	}

	Kit* kit = (Kit*)clip->output;

	InstrumentClip* srcClips[kMaxSourceClips];
	int32_t numSrcClips = gatherKitClips(currentSong, kit, srcClips, kMaxSourceClips);
	if (numSrcClips <= 0) {
		return 0;
	}

	Drum* drums[kMaxSplitKits];
	return collectDrums(clip, kit, srcClips, std::min(numSrcClips, kMaxSourceClips), drums, kMaxSplitKits);
}

bool canSplit(InstrumentClip* clip) {
	return countSplittableRows(clip) >= 2;
}

int32_t performSplit(InstrumentClip* invoked) {

	Song* song = currentSong;

	if (!invoked || invoked->output == nullptr || invoked->output->type != OutputType::KIT) {
		return 0;
	}

	Kit* srcKit = (Kit*)invoked->output;

	// ---- What we are consuming ----
	// The unit of this operation is the KIT, not the Clip it was invoked from. A Kit can carry several session Clips -
	// a part and its variations, stacked in one Grid column - and they all share the same Drums. Splitting only the
	// invoked Clip would move those Drums out from under its siblings, leaving them holding rows that point into other
	// Kits; splitting a sibling afterwards would then hand the same Drum to a second Kit and both would later free it.
	// So every Clip on the Kit is split together, and each new Kit receives one Clip per source Clip, at that Clip's
	// own section - which is also what you want musically: the variations line up underneath their part.
	InstrumentClip* srcClips[kMaxSourceClips];
	int32_t numSrcClips = gatherKitClips(song, srcKit, srcClips, kMaxSourceClips);
	if (numSrcClips <= 0 || numSrcClips > kMaxSourceClips) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_TOO_MANY));
		return 0;
	}

	Drum* drums[kMaxSplitKits];
	int32_t numDrums = collectDrums(invoked, srcKit, srcClips, numSrcClips, drums, kMaxSplitKits);

	if (numDrums < 2) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_NEEDS_ROWS));
		return 0;
	}
	if (numDrums > kMaxSplitKits) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_TOO_MANY));
		return 0;
	}

	// Deleting the Clip that drives sync scaling would be a mess. Every Clip here is about to go, so check them all.
	for (int32_t m = 0; m < numSrcClips; m++) {
		if (srcClips[m] == song->getSyncScalingClip()) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_SYNC_CLIP));
			return 0;
		}
	}

	// This moves Drums between Kits, which must not happen underneath a running playhead. During Pass 2 a source
	// Clip briefly holds a NoteRow pointing at a Drum that has already been handed to another Kit; a note landing in
	// that window triggers it against the wrong Kit - note_row.cpp passes the Clip's own output into Drum::noteOn()
	// while drum->kit already says otherwise, and Kit::drumRemoved()'s E321 is the tripwire for exactly that.
	//
	// The fix is not to demand the whole song stop. The playhead only ever reaches these Drums through a Clip that
	// is playing, so it is enough that nothing on this Kit is playing: mute the track and the operation is invisible
	// to the transport. The rest of the song carries on. The new Kits arrive muted, which is the honest outcome for
	// something destructive and non-undoable - you get a look at what you did before committing it to the mix.
	if (playbackHandler.isEitherClockActive()) {

		// The arranger can be playing this Kit from an arrangement-only Clip, and an arrangement is not something to
		// restructure underneath in any case.
		if (currentPlaybackMode != &session) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_STOP_PLAYBACK));
			return 0;
		}

		// Recording to the arranger is laying down ClipInstances as we go, including for this Kit.
		if (playbackHandler.recording == RecordingMode::ARRANGEMENT) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_STOP_PLAYBACK));
			return 0;
		}

		// Every Clip on the Kit, not just the invoked one - they all reach these Drums.
		for (int32_t m = 0; m < numSrcClips; m++) {
			if (song->isClipActive(srcClips[m])) {
				display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_MUTE_TRACK));
				return 0;
			}
		}
	}

	// The whole Kit is being consumed now, so any arranger use of it is a reason to refuse - not just an instance of
	// the invoked Clip. Song::removeSessionClip() does not delete a Clip that has instances in the arranger, it moves
	// it to arrangementOnlyClips, and we would have already stripped its rows and Drums by then.
	if (srcKit->clipInstances.getNumElements() > 0) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_IN_ARRANGER));
		return 0;
	}

	// Volume and pan are the two kit-global params that do NOT get reset along with the rest - see Pass 3. Snapshot
	// them per source Clip, before anything is allowed to move; each Clip carries its own kit-global ParamManager.
	// changeInstrument() -> setInstrument() -> setAudioInstrument() force-clears affectEntire whenever the new Output
	// is a Kit. That is the right default for a conversion and the wrong one here, so snapshot that too.
	// Kit::drumRemoved() clears selectedDrum as each Drum leaves. On full success srcKit is consumed and it does not
	// matter; if we stop early it survives, and a Kit with no selected Drum comes up with dead gold knobs.
	Drum* srcSelectedDrum = srcKit->selectedDrum;

	bool srcAffectEntire[kMaxSourceClips];
	int32_t srcVolume[kMaxSourceClips];
	int32_t srcPan[kMaxSourceClips];
	for (int32_t m = 0; m < numSrcClips; m++) {
		srcAffectEntire[m] = srcClips[m]->affectEntire;
		srcVolume[m] = 0;
		srcPan[m] = 0;
		if (srcClips[m]->paramManager.containsAnyParamCollectionsIncludingExpression()) {
			UnpatchedParamSet* srcUnpatched = srcClips[m]->paramManager.getUnpatchedParamSet();
			srcVolume[m] = srcUnpatched->getValue(params::UNPATCHED_VOLUME);
			srcPan[m] = srcUnpatched->getValue(params::UNPATCHED_PAN);
		}
	}

	// Can't undo past this.
	actionLogger.deleteAllLogs();

	song->stopAllAuditioning();
	srcKit->cutAllSound();

	// The source Clips are muted, but they can still be armed and blinking, and can still be holding notes that
	// arrived over MIDI rather than from the playhead. Silence and disarm them here, while they still have all their
	// rows and before anything moves.
	//
	// Note this is the ModelStack overload. Clip::stopAllNotesPlaying(Song*) is an empty virtual that nothing on this
	// line overrides, so SessionView::removeClip()'s call to it does nothing at all - do not copy that one.
	bool anyWasArmed = false;
	for (int32_t m = 0; m < numSrcClips; m++) {
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithTimelineCounter* modelStack =
		    setupModelStackWithTimelineCounter(modelStackMemory, song, srcClips[m]);
		srcClips[m]->stopAllNotesPlaying(modelStack);
		anyWasArmed |= srcClips[m]->cancelAnyArming();
	}
	if (anyWasArmed && playbackHandler.isEitherClockActive() && currentPlaybackMode == &session) {
		session.launchSchedulingMightNeedCancelling();
	}

	AudioEngine::routineWithClusterLoading();

	// ---- Pass 1: build every new Kit and Clip before mutating anything ----
	// One new Kit per Drum, and one Clip per (source Clip, Drum) pair that actually has a row. The pair grid is
	// numSrcClips * numDrums, which is too big to want on the stack, so it comes off the heap and is freed at the end.
	int32_t numPairs = numSrcClips * numDrums;
	Clip** clones = (Clip**)GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Clip*) * numPairs);
	if (!clones) {
		display->displayError(Error::INSUFFICIENT_RAM);
		return 0;
	}
	for (int32_t i = 0; i < numPairs; i++) {
		clones[i] = nullptr;
	}

	Kit* kits[kMaxSplitKits] = {nullptr};

	// Reserve the session clip slots now, so that handing the new Clips over later cannot fail partway through.
	if (!song->sessionClips.ensureEnoughSpaceAllocated(numPairs)) {
		delugeDealloc(clones);
		display->displayError(Error::INSUFFICIENT_RAM);
		return 0;
	}

	bool allocatedOk = true;

	for (int32_t n = 0; n < numDrums && allocatedOk; n++) {
		Instrument* newInstrument = storageManager.createNewInstrument(OutputType::KIT, nullptr);
		if (!newInstrument) {
			allocatedOk = false;
			break;
		}
		kits[n] = (Kit*)newInstrument;
		if (kits[n]->dirPath.set("KITS") != Error::NONE) {
			allocatedOk = false;
			break;
		}
		AudioEngine::routineWithClusterLoading();
	}

	// Each clone is trimmed to its single row immediately, not in a second pass over the finished grid. Clip::clone()
	// deep-copies every NoteRow, and a NoteRow's ParamManager is several KB, so holding numSrcClips * numDrums
	// untrimmed clones at once would be tens of megabytes on a 24-row kit - it would fail for RAM, or evict the whole
	// sample cache getting there. Trimming as we go keeps the peak at one clone's worth of rows, and because
	// deleteNoteRow() frees as it goes it makes the later clones more likely to succeed rather than less.
	//
	// This is safe to do here because it depends only on the source Clip and the clone: nothing has moved yet, so
	// every NoteRow being dropped still finds its Drum on the source Kit and the ParamManager bookkeeping stays keyed
	// to something that still exists. That remains true right up until Pass 2.
	for (int32_t m = 0; m < numSrcClips && allocatedOk; m++) {
		for (int32_t n = 0; n < numDrums && allocatedOk; n++) {

			// No row for this Drum in this Clip means this variation simply has nothing to say about that Drum. The
			// new Kit gets one fewer Clip; it does not get an empty one.
			//
			// The clone will mirror its source's row order, so the source's index for this Drum is the clone's too.
			int32_t keepRow = -1;
			if (!srcClips[m]->getNoteRowForDrum(drums[n], &keepRow) || keepRow < 0) {
				continue;
			}

			InstrumentClip* clone;
			{
				char modelStackMemory[MODEL_STACK_MAX_SIZE];
				ModelStackWithTimelineCounter* modelStack =
				    setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(srcClips[m]);

				if (srcClips[m]->clone(modelStack, false) != Error::NONE) {
					allocatedOk = false;
					break;
				}
				clone = (InstrumentClip*)modelStack->getTimelineCounter();
				clones[m * numDrums + n] = clone;
			}

			{
				char modelStackMemory[MODEL_STACK_MAX_SIZE];
				ModelStackWithTimelineCounter* modelStack =
				    setupModelStackWithTimelineCounter(modelStackMemory, song, clone);

				// Walking top-down keeps the kept row's index above the cursor the whole way, so comparing against
				// the original index stays correct even as rows below it shift down.
				for (int32_t i = clone->noteRows.getNumElements() - 1; i >= 0; i--) {
					if (i == keepRow) {
						continue;
					}
					clone->deleteNoteRow(modelStack, i);
				}
			}

			clone->yScroll = 0;

			// Each deleteNoteRow() above stashed that row's params in the song, keyed (drum, clone) - correct in
			// general, but here those pairs will never be reunited, and across the whole grid it is a lot of orphaned
			// ParamManagers held for the life of the song. The row we kept still holds its own params directly, so
			// nothing we need is in this list yet.
			song->deleteBackedUpParamManagersForClip(clone);

			AudioEngine::routineWithClusterLoading();
		}
	}

	if (!allocatedOk) {
		for (int32_t i = 0; i < numPairs; i++) {
			discardUnusedClip(clones[i]);
		}
		for (int32_t n = 0; n < numDrums; n++) {
			discardUnusedKit(kits[n]);
		}
		delugeDealloc(clones);
		display->displayError(Error::INSUFFICIENT_RAM);
		return 0;
	}

	// We are about to delete the Clips the user is almost certainly standing in.
	changeRootUI(&sessionView);

	// ---- Pass 2: hand each Drum and its Clips over ----
	// Backwards, and always inserted right next to where the source Kit still is, so that once every Drum is placed,
	// the whole block sits exactly where the source used to be - same row order, same neighbours on both sides -
	// rather than always landing at the front of the output list or the leftmost grid columns.
	Output* outputInsertionAnchor = srcKit;
	int32_t created = 0;
	Clip* firstCreatedClip = nullptr;

	for (int32_t n = numDrums - 1; n >= 0; n--) {

		Drum* drum = drums[n];
		Kit* newKit = kits[n];

		// Move the Drum, don't copy it. Once, no matter how many Clips reference it.
		//
		// wontBeRenderedForAWhile() kills its voices and forces skippingRendering true, which is what actually pulls
		// it out of srcKit->drumsWithRenderingActive - SoundDrum::setSkippingRendering is the only thing that
		// maintains that list. Editing the list by hand instead would leave the Drum with skippingRendering false and
		// in nobody's render list, i.e. permanently silent on its new Kit. It also keeps Kit::drumRemoved()'s E321
		// check happy.
		drum->drumWontBeRenderedForAWhile();
		srcKit->removeDrum(drum);

		// Kit::removeDrumFromLinkedList() only rewrites the predecessor's link, so the Drum still points at its old
		// successor - and Kit::addDrum() never clears it, because every other caller in the tree passes a brand new
		// Drum. Without this, the new Kit's list runs straight on into Drums still owned by the source Kit, and they
		// get double-freed when the source is torn down.
		drum->next = nullptr;
		newKit->addDrum(drum); // Also sets drum->kit

		// createNewInstrument() leaves selectedDrum null. Without this,
		// InstrumentClip::getActiveModControllable() takes its !affectEntire branch, finds no selected SoundDrum and
		// returns nothing at all: the gold knobs come up unlit and neither the kit FX nor the drum's own params can
		// be reached or heard.
		newKit->selectedDrum = drum;

		bool drumOk = true;
		int32_t placedForThisDrum = 0;

		for (int32_t m = 0; m < numSrcClips && drumOk; m++) {

			InstrumentClip* clone = (InstrumentClip*)clones[m * numDrums + n];
			if (!clone) {
				continue;
			}
			if (clone->noteRows.getNumElements() != 1 || clone->noteRows.getElement(0)->drum != drum) {
				drumOk = false; // Not something we understand - stop rather than press on.
				break;
			}

			// A brand new, neutral kit ParamManager.
			//
			// changeInstrument() below hands the clone's inherited (cloned from the source) kit-global FX back to the
			// song as a backup keyed to the old Kit, which leaves the clone holding no param collections - so this one
			// gets taken up in its place. That is what stops kit-global FX multiplying across the split. Per-row FX
			// are untouched: they live in the NoteRow and travel with it.
			ParamManagerForTimeline newParamManager;
			if (newParamManager.setupUnpatched() != Error::NONE) {
				display->displayError(Error::INSUFFICIENT_RAM);
				drumOk = false;
				break;
			}
			GlobalEffectableForClip::initParams(&newParamManager);

			// Volume and pan are the exception to that reset. They are not wet effects that stack - they are this
			// output's gain and its position, and each new Kit holds exactly one Drum. Carrying the source's values
			// across therefore hands every Drum the same level and placement it had before the split, and the mix
			// comes out where it started. Leaving them neutral is the thing that changes the sound. Delay, reverb,
			// filter and mod FX stay neutral above, because those genuinely would multiply.
			//
			// Values only - automation on the source's volume or pan is not carried across.
			UnpatchedParamSet* newUnpatched = newParamManager.getUnpatchedParamSet();
			newUnpatched->params[params::UNPATCHED_VOLUME].setCurrentValueBasicForSetup(srcVolume[m]);
			newUnpatched->params[params::UNPATCHED_PAN].setCurrentValueBasicForSetup(srcPan[m]);

			{
				char modelStackMemory[MODEL_STACK_MAX_SIZE];
				ModelStackWithTimelineCounter* modelStack =
				    setupModelStackWithTimelineCounter(modelStackMemory, song, clone);

				Error error = clone->changeInstrument(modelStack, newKit, &newParamManager, InstrumentRemoval::NONE);
				if (error != Error::NONE) {
					display->displayError(error);
					drumOk = false;
					break;
				}
			}

			// Put back what setAudioInstrument() cleared.
			clone->affectEntire = srcAffectEntire[m];
			clone->section = srcClips[m]->section;
			clone->onKeyboardScreen = false;

			// Insert next to this clone's own source Clip, so the new block sits where that variation sat. Grid
			// position comes from (output, section) rather than this index, so this is about Rows mode staying
			// readable; the index is re-read each time because every insertion shifts what is after it.
			int32_t insertAt = song->sessionClips.getIndexForClip(srcClips[m]);
			if (insertAt < 0) {
				insertAt = 0;
			}
			if (song->sessionClips.insertClipAtIndex(clone, insertAt) != Error::NONE) {
				display->displayError(Error::INSUFFICIENT_RAM);
				drumOk = false;
				break;
			}

			if (!newKit->getActiveClip()) {
				char modelStackMemory[MODEL_STACK_MAX_SIZE];
				ModelStackWithTimelineCounter* modelStack =
				    setupModelStackWithTimelineCounter(modelStackMemory, song, clone);
				newKit->setActiveClip(modelStack, PgmChangeSend::NEVER);
			}

			if (!firstCreatedClip) {
				firstCreatedClip = clone;
			}

			// Handed over to the song - no longer ours to free.
			clones[m * numDrums + n] = nullptr;
			placedForThisDrum++;
		}

		// Failed before a single Clip for this Drum reached the song: put the Drum back rather than letting it die
		// with the Kit we are about to throw away. It loses nothing - the source Clips still hold its rows.
		if (placedForThisDrum == 0) {
			newKit->removeDrum(drum);
			drum->next = nullptr;
			srcKit->addDrum(drum);
			break;
		}

		// Otherwise some Clips for this Drum are already in the song and pointing at this Kit, so the Kit has to be
		// finished and handed over even if a later sibling failed. Fall through, then stop.

		// Now that the Drum is safely on its new Kit, drop the matching rows from the source Clips, so nothing is left
		// pointing at a Drum that has moved away. Indices are re-read per Clip because earlier deletions shift them.
		for (int32_t m = 0; m < numSrcClips; m++) {
			int32_t rowIndex = -1;
			if (!srcClips[m]->getNoteRowForDrum(drum, &rowIndex) || rowIndex < 0) {
				continue;
			}
			char modelStackMemory[MODEL_STACK_MAX_SIZE];
			ModelStackWithTimelineCounter* modelStack =
			    setupModelStackWithTimelineCounter(modelStackMemory, song, srcClips[m]);
			srcClips[m]->deleteNoteRow(modelStack, rowIndex);
		}

		// Name it after the Drum it now holds.
		{
			String newName;
			// 1.2.1 has no Drum::getDrumName(). SoundDrum::name is the only name a Drum carries on this line; MIDI and
			// gate drums have none, so they fall through to the generic name and pick up a number from
			// makeKitNameUnique().
			if (drum->type == DrumType::SOUND) {
				SoundDrum* soundDrum = (SoundDrum*)drum;
				if (!soundDrum->name.isEmpty()) {
					if (newName.set(soundDrum->name.get()) != Error::NONE) {
						newName.clear();
					}
				}
			}
			if (newName.isEmpty()) {
				if (newName.set("SPLIT") != Error::NONE) {
					newName.clear();
				}
			}
			if (!newName.isEmpty()) {
				makeKitNameUnique(&newName, song);
				newKit->name.set(&newName);
			}
		}
		newKit->editedByUser = true;
		newKit->existsOnCard = false;

		// Chained onto the previous insertion rather than onto srcKit every time, so the block's internal order comes
		// out the way Song::addOutput(atStart=false) used to build it against the end of the whole list - just
		// anchored at srcKit's actual neighbours instead. Grid columns run opposite to the output list, which is why
		// this walks n downwards.
		insertOutputAfter(song, outputInsertionAnchor, newKit);
		outputInsertionAnchor = newKit;

		kits[n] = nullptr; // Handed over.
		created++;

		if (!drumOk) {
			break;
		}

		AudioEngine::routineWithClusterLoading();
	}

	// Anything we never got to, or that failed, never reached the song.
	for (int32_t i = 0; i < numPairs; i++) {
		discardUnusedClip(clones[i]);
	}
	for (int32_t n = 0; n < numDrums; n++) {
		discardUnusedKit(kits[n]);
	}
	delugeDealloc(clones);

	if (created != numDrums && srcSelectedDrum != nullptr && srcSelectedDrum->kit == srcKit) {
		srcKit->selectedDrum = srcSelectedDrum;
	}

	if (created == 0) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_FAILED));
		uiNeedsRendering(&sessionView);
		return 0;
	}

	// Only consume the source once every Drum made it out. If we stopped early the source Kit keeps whatever Drums
	// remain, and its Clips keep the rows for them.
	if (created == numDrums && firstCreatedClip) {

		// Called twice on purpose: the first call would otherwise leave Song::previousClip pointing at a clip we are
		// about to destroy.
		song->setCurrentClip(firstCreatedClip);
		song->setCurrentClip(firstCreatedClip);

		for (int32_t m = 0; m < numSrcClips; m++) {
			int32_t srcIndex = song->sessionClips.getIndexForClip(srcClips[m]);
			if (srcIndex < 0 || song->sessionClips.getNumElements() <= 1) {
				continue;
			}
			// SessionView::removeClip() does this before removing, and it is the only thing that scrubs midiFollow's
			// per-note clip cache. Skip it and an incoming MIDI note dereferences a freed Clip.
			midiFollow.removeClip(srcClips[m]);
			song->removeSessionClip(srcClips[m], srcIndex);
		}

		// We may have just deleted Clips the launch scheduling was still counting on.
		if (playbackHandler.isEitherClockActive() && currentPlaybackMode == &session) {
			session.launchSchedulingMightNeedCancelling();
		}
	}

	uiNeedsRendering(&sessionView);
	return created;
}

} // namespace deluge::model::kit_split
