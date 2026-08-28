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
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "storage/storage_manager.h"
#include "util/d_string.h"

namespace params = deluge::modulation::params;

namespace deluge::model::kit_split {

namespace {

/// Sanity bound. A kit can hold more rows than this, but splitting hundreds of tracks in one keypress is not a
/// thing anyone means to do, and it keeps the pre-allocation arrays on the stack.
constexpr int32_t kMaxSplitKits = 64;

/// A row is worth splitting out if it has a Drum and at least one Note. Rows with no notes are skipped - their
/// Drums go away with the source Kit.
bool rowIsSplittable(NoteRow* noteRow) {
	return noteRow->drum != nullptr && !noteRow->hasNoNotes();
}

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
	if (!clip || clip->output == nullptr || clip->output->type != OutputType::KIT) {
		return 0;
	}

	int32_t count = 0;
	for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
		if (rowIsSplittable(clip->noteRows.getElement(i))) {
			count++;
		}
	}
	return count;
}

bool canSplit(InstrumentClip* clip) {
	return countSplittableRows(clip) >= 2;
}

int32_t performSplit(InstrumentClip* src) {

	Song* song = currentSong;

	if (!canSplit(src)) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_NEEDS_ROWS));
		return 0;
	}

	// Deleting the clip that drives sync scaling would be a mess. Refuse plainly.
	if (src == song->getSyncScalingClip()) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_SYNC_CLIP));
		return 0;
	}

	// v1 restriction: this moves Drums between Kits, which is not something to do underneath a running playhead.
	if (playbackHandler.isEitherClockActive()) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_STOP_PLAYBACK));
		return 0;
	}

	Kit* srcKit = (Kit*)src->output;

	// Song::removeSessionClip() does not delete a Clip that has instances in the arranger - it moves it to
	// arrangementOnlyClips. We'd have already stripped its rows and Drums by then, silently gutting the
	// arrangement, with undo wiped. Refuse instead.
	for (int32_t i = 0; i < srcKit->clipInstances.getNumElements(); i++) {
		if (srcKit->clipInstances.getElement(i)->clip == src) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_IN_ARRANGER));
			return 0;
		}
	}

	// Refuse rather than split the first kMaxSplitKits and silently drop the rest along with the source Kit.
	if (countSplittableRows(src) > kMaxSplitKits) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_TOO_MANY));
		return 0;
	}

	// Which rows are we splitting, lowest index first.
	int32_t rowIndices[kMaxSplitKits];
	int32_t numToSplit = 0;
	for (int32_t i = 0; i < src->noteRows.getNumElements() && numToSplit < kMaxSplitKits; i++) {
		if (rowIsSplittable(src->noteRows.getElement(i))) {
			rowIndices[numToSplit++] = i;
		}
	}

	// Volume and pan are the two kit-global params that do NOT get reset along with the rest - see Pass 3.
	// Snapshot them before anything is allowed to move. Both neutral defaults are 0, so a source Clip somehow
	// holding no param collections falls through to exactly what initParams() would have left behind anyway.
	// changeInstrument() -> setInstrument() -> setAudioInstrument() force-clears affectEntire whenever the new
	// Output is a Kit. That is the right default for a conversion, where you have just landed in a kit and want
	// the drums - and the wrong one here, where the user was in affect-entire on the source and every kit we make
	// holds a single sound. Snapshot it alongside the params and put it back once the swap is done.
	bool srcAffectEntire = src->affectEntire;

	int32_t srcVolume = 0;
	int32_t srcPan = 0;
	if (src->paramManager.containsAnyParamCollectionsIncludingExpression()) {
		UnpatchedParamSet* srcUnpatched = src->paramManager.getUnpatchedParamSet();
		srcVolume = srcUnpatched->getValue(params::UNPATCHED_VOLUME);
		srcPan = srcUnpatched->getValue(params::UNPATCHED_PAN);
	}

	// Can't undo past this.
	actionLogger.deleteAllLogs();

	song->stopAllAuditioning();
	srcKit->cutAllSound();
	AudioEngine::routineWithClusterLoading();

	// Reserve the session clip slots now, so that handing the new Clips over later cannot fail partway through.
	if (!song->sessionClips.ensureEnoughSpaceAllocated(numToSplit)) {
		display->displayError(Error::INSUFFICIENT_RAM);
		return 0;
	}

	// ---- Pass 1: allocate everything before mutating anything ----
	// If any of this fails we throw away what we made and the song is untouched.
	Clip* clones[kMaxSplitKits] = {nullptr};
	Kit* kits[kMaxSplitKits] = {nullptr};

	bool allocatedOk = true;
	for (int32_t n = 0; n < numToSplit && allocatedOk; n++) {
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithTimelineCounter* modelStack =
		    setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(src);

		if (src->clone(modelStack, false) != Error::NONE) {
			allocatedOk = false;
			break;
		}
		clones[n] = (Clip*)modelStack->getTimelineCounter();

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

	if (!allocatedOk) {
		for (int32_t n = 0; n < numToSplit; n++) {
			discardUnusedClip(clones[n]);
			discardUnusedKit(kits[n]);
		}
		display->displayError(Error::INSUFFICIENT_RAM);
		return 0;
	}

	// ---- Pass 2: reduce each clone to the single row it is keeping ----
	// Done before any Drum moves, so every NoteRow being dropped still finds its Drum on the source Kit and the
	// ParamManager bookkeeping stays keyed to something that still exists.
	for (int32_t n = 0; n < numToSplit; n++) {
		InstrumentClip* clone = (InstrumentClip*)clones[n];
		int32_t keepRow = rowIndices[n];

		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, song, clone);

		// Walking top-down keeps the kept row's index above the cursor the whole way, so comparing against the
		// original index stays correct even as rows below it shift down.
		for (int32_t i = clone->noteRows.getNumElements() - 1; i >= 0; i--) {
			if (i == keepRow) {
				continue;
			}
			clone->deleteNoteRow(modelStack, i);
		}

		clone->yScroll = 0;

		// Each deleteNoteRow() above stashed that row's params in the song, keyed (drum, clone) - correct in
		// general, but here those pairs will never be reunited, and across N clones it is N*(N-1) orphaned
		// ParamManagers held for the life of the song. The row we kept still holds its own params directly, so
		// nothing we need is in this list yet.
		song->deleteBackedUpParamManagersForClip(clone);

		AudioEngine::routineWithClusterLoading();
	}

	// We are about to delete the Clip the user is almost certainly standing in.
	changeRootUI(&sessionView);

	// ---- Pass 3: hand each Drum and Clip over ----
	// Backwards, and always inserted right next to where the source Clip and Kit still are, so that once every
	// row is placed, the whole block sits exactly where the source used to be - same row order, same neighbours
	// on both sides - rather than always landing at the front of the session clip list or the leftmost grid
	// columns. Snapshot the source's position now, before anything is inserted and shifts indices around it.
	int32_t srcSessionIndex = song->sessionClips.getIndexForClip(src);
	Output* outputInsertionAnchor = srcKit;
	int32_t created = 0;
	for (int32_t n = numToSplit - 1; n >= 0; n--) {

		InstrumentClip* clone = (InstrumentClip*)clones[n];
		Kit* newKit = kits[n];

		if (clone->noteRows.getNumElements() != 1) {
			break; // Not something we understand - stop rather than press on.
		}
		Drum* drum = clone->noteRows.getElement(0)->drum;
		if (!drum) {
			break;
		}

		// A brand new, neutral kit ParamManager, allocated before anything moves so a RAM failure here cannot
		// strand a Drum between two Kits.
		//
		// changeInstrument() below hands the clone's inherited (cloned from the source) kit-global FX back to the
		// song as a backup keyed to the old Kit, which leaves the clone holding no param collections - so this one
		// gets taken up in its place. That is what stops kit-global FX multiplying across the split. Per-row FX are
		// untouched: they live in the NoteRow and travel with it.
		ParamManagerForTimeline newParamManager;
		if (newParamManager.setupUnpatched() != Error::NONE) {
			display->displayError(Error::INSUFFICIENT_RAM);
			break;
		}
		GlobalEffectableForClip::initParams(&newParamManager);

		// Volume and pan are the exception to that reset. They are not wet effects that stack - they are this
		// output's gain and its position, and each new Kit holds exactly one Drum. Carrying the source's values
		// across therefore hands every Drum the same level and placement it had before the split, and the mix
		// comes out where it started. Leaving them neutral is the thing that changes the sound: a kit sitting at
		// 43 becomes N kits at the halfway default. Delay, reverb, filter and mod FX stay neutral above,
		// because those genuinely would multiply N ways.
		//
		// Values only - automation on the source's volume or pan is not carried across.
		UnpatchedParamSet* newUnpatched = newParamManager.getUnpatchedParamSet();
		newUnpatched->params[params::UNPATCHED_VOLUME].setCurrentValueBasicForSetup(srcVolume);
		newUnpatched->params[params::UNPATCHED_PAN].setCurrentValueBasicForSetup(srcPan);

		// Move the Drum, don't copy it.
		//
		// wontBeRenderedForAWhile() kills its voices and forces skippingRendering true, which is what actually
		// pulls it out of srcKit->drumsWithRenderingActive - SoundDrum::setSkippingRendering is the only thing
		// that maintains that list. Editing the list by hand instead would leave the Drum with skippingRendering
		// false and in nobody's render list, i.e. permanently silent on its new Kit. It also keeps
		// Kit::drumRemoved()'s E321 check happy.
		drum->drumWontBeRenderedForAWhile();
		srcKit->removeDrum(drum);

		// Kit::removeDrumFromLinkedList() only rewrites the predecessor's link, so the Drum still points at its
		// old successor - and Kit::addDrum() never clears it, because every other caller in the tree passes a
		// brand new Drum. Without this, the new Kit's list runs straight on into Drums still owned by the source
		// Kit, and they get double-freed when the source is torn down.
		drum->next = nullptr;
		newKit->addDrum(drum); // Also sets drum->kit

		{
			char modelStackMemory[MODEL_STACK_MAX_SIZE];
			ModelStackWithTimelineCounter* modelStack =
			    setupModelStackWithTimelineCounter(modelStackMemory, song, clone);

			Error error = clone->changeInstrument(modelStack, newKit, &newParamManager, InstrumentRemoval::NONE);
			if (error != Error::NONE) {
				// Put the Drum back where it came from rather than letting it die with the Kit we are about to
				// throw away. It loses its row on the source clip, but the sound itself survives.
				newKit->removeDrum(drum);
				drum->next = nullptr;
				srcKit->addDrum(drum);
				display->displayError(error);
				break;
			}
		}

		// Put back what setAudioInstrument() cleared, and give the new Kit its one Drum as the selected one -
		// createNewInstrument() leaves selectedDrum null. Without both of these,
		// InstrumentClip::getActiveModControllable() takes its !affectEntire branch, finds no selected SoundDrum
		// and returns nothing at all: the gold knobs come up unlit and neither the kit FX nor the drum's own
		// params can be reached or heard.
		clone->affectEntire = srcAffectEntire;
		newKit->selectedDrum = drum;

		// Now that the Drum is safely on its new Kit, drop the matching row from the source, so nothing is left
		// pointing at a Drum that has moved away. rowIndices ascend and we walk n downwards, so this always
		// removes the highest remaining one and the lower indices stay valid.
		{
			char modelStackMemory[MODEL_STACK_MAX_SIZE];
			ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, song, src);
			src->deleteNoteRow(modelStack, rowIndices[n]);
		}

		// Name it after the Drum it now holds.
		{
			String newName;
			// 1.2.1 has no Drum::getDrumName(). SoundDrum::name is the only name a Drum carries on this line;
			// MIDI and gate drums have none, so they fall through to the generic name and pick up a number
			// from makeKitNameUnique().
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

		clone->section = src->section;
		clone->onKeyboardScreen = false;

		// Clip first, then output: if the insert somehow fails we can still discard both cleanly. Space was
		// reserved up front, so it should not.
		//
		// Fixed index, not 0: src is still sitting at srcSessionIndex while this runs, so repeatedly inserting
		// there pushes src (and everything after it) down by one each time. The finished block ends up in row
		// order immediately ahead of src's old slot instead of at the top of the list.
		if (song->sessionClips.insertClipAtIndex(clone, srcSessionIndex) != Error::NONE) {
			display->displayError(Error::INSUFFICIENT_RAM);
			break;
		}
		// Chained onto the previous insertion rather than onto srcKit every time, so the block's internal row
		// order comes out the way Song::addOutput(atStart=false) used to build it against the end of the whole
		// list - just anchored at srcKit's actual neighbours instead.
		insertOutputAfter(song, outputInsertionAnchor, newKit);
		outputInsertionAnchor = newKit;

		if (!newKit->getActiveClip()) {
			char modelStackMemory[MODEL_STACK_MAX_SIZE];
			ModelStackWithTimelineCounter* modelStack =
			    setupModelStackWithTimelineCounter(modelStackMemory, song, clone);
			newKit->setActiveClip(modelStack, PgmChangeSend::NEVER);
		}

		// Handed over to the song - no longer ours to free.
		clones[n] = nullptr;
		kits[n] = nullptr;

		created++;
		AudioEngine::routineWithClusterLoading();
	}

	// Anything we never got to, or that failed, never reached the song.
	for (int32_t n = 0; n < numToSplit; n++) {
		discardUnusedClip(clones[n]);
		discardUnusedKit(kits[n]);
	}

	if (created == 0) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SPLIT_KIT_FAILED));
		uiNeedsRendering(&sessionView);
		return 0;
	}

	// Only consume the source once every row made it out. If we stopped early it keeps whatever Drums remain.
	if (created == numToSplit) {
		int32_t srcIndex = song->sessionClips.getIndexForClip(src);
		if (srcIndex >= 0 && song->sessionClips.getNumElements() > 1) {
			Clip* replacement = song->sessionClips.getClipAtIndex(0);
			// Called twice on purpose: the first call would otherwise leave Song::previousClip pointing at the
			// clip we are about to destroy.
			song->setCurrentClip(replacement);
			song->setCurrentClip(replacement);
			// SessionView::removeClip() does this before removing, and it is the only thing that scrubs
			// midiFollow's per-note clip cache. Skip it and an incoming MIDI note dereferences a freed Clip.
			midiFollow.removeClip(src);
			song->removeSessionClip(src, srcIndex);
		}
	}

	uiNeedsRendering(&sessionView);
	return created;
}

} // namespace deluge::model::kit_split
