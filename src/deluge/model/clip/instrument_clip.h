/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
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

#include "definitions_cxx.hpp"
#include "gui/ui/keyboard/state_data.h"
#include "gui/views/instrument_clip_view.h"
#include "model/note/note_row_vector.h"
#include "modulation/arpeggiator.h"
#include "modulation/midi_fx/groove.h"
#include "modulation/midi_fx/loop_remix.h"
#include "modulation/midi_fx/midi_fx_settings.h"

class Song;

class NoteRow;
class InstrumentClip;
class Instrument;
class ModControllable;
class Drum;
class StereoSample;
class ParamManagerForTimeline;
class Sound;
class Note;
class Action;
class ParamManagerForTimeline;
class TimelineView;
class ModelStackWithTimelineCounter;
class ModelStackWithModControllable;
class ModelStackWithNoteRow;

struct PendingNoteOn;

enum class VerticalNudgeType { ROW, OCTAVE };

class InstrumentClip final : public Clip {
public:
	explicit InstrumentClip(Song* song = nullptr);
	~InstrumentClip() override;
	void increaseLengthWithRepeats(ModelStackWithTimelineCounter* modelStack, int32_t newLength,
	                               IndependentNoteRowLengthIncrease independentNoteRowInstruction,
	                               bool completelyRenderOutIterationDependence = false,
	                               Action* action = nullptr) override;
	void halveNoteRowsWithIndependentLength(ModelStackWithTimelineCounter* modelStack);
	void repeatOrChopToExactLength(ModelStackWithTimelineCounter* modelStack, int32_t newLength);
	void processCurrentPos(ModelStackWithTimelineCounter* modelStack, uint32_t posIncrement) override;
	bool renderAsSingleRow(ModelStackWithTimelineCounter* modelStack, TimelineView* editorScreen, int32_t xScroll,
	                       uint32_t xZoom, RGB* image, uint8_t occupancyMask[], bool addUndefinedArea,
	                       int32_t noteRowIndexStart = 0, int32_t noteRowIndexEnd = 2147483647, int32_t xStart = 0,
	                       int32_t xEnd = kDisplayWidth, bool allowBlur = true, bool drawRepeats = false) override;
	void toggleNoteRowMute(ModelStackWithNoteRow* modelStack);
	RGB getMainColourFromY(int32_t yNote, int8_t);
	void stopAllNotesPlaying(ModelStackWithTimelineCounter* modelStack, bool actuallySoundChange = true);
	void resumePlayback(ModelStackWithTimelineCounter* modelStack, bool mayMakeSound = true) override;
	void setPos(ModelStackWithTimelineCounter* modelStack, int32_t newPos,
	            bool useActualPosForParamManagers = true) override;
	void replaceMusicalMode(const ScaleChange& changes, ModelStackWithTimelineCounter* modelStack);
	void seeWhatNotesWithinOctaveArePresent(NoteSet&, MusicalKey);
	void transpose(int32_t, ModelStackWithTimelineCounter* modelStack);
	/// Returns false, changing nothing, if the transpose would leave the playable note range.
	bool nudgeNotesVertically(int32_t direction, VerticalNudgeType, ModelStackWithTimelineCounter* modelStack);
	void expectNoFurtherTicks(Song* song, bool actuallySoundChange = true) override;
	Error clone(ModelStackWithTimelineCounter* modelStack, bool shouldFlattenReversing = false) const override;
	NoteRow* createNewNoteRowForYVisual(int32_t, Song* song);
	int32_t getYVisualFromYNote(int32_t, Song* song);
	int32_t getYNoteFromYVisual(int32_t, Song* song);
	int32_t getYNoteFromYDisplay(int32_t yDisplay, Song* song);
	int32_t guessRootNote(Song* song, int32_t previousRoot);
	int32_t getNumNoteRows();
	void ensureInaccessibleParamPresetValuesWithoutKnobsAreZero(ModelStackWithTimelineCounter* modelStack,
	                                                            Sound* sound);
	bool deleteSoundsWhichWontSound(Song* song) override;
	void setBackedUpParamManagerMIDI(ParamManagerForTimeline* newOne);
	void restoreBackedUpParamManagerMIDI(ModelStackWithModControllable* modelStack);
	int32_t getNoteRowId(NoteRow* noteRow, int32_t noteRowIndex);
	NoteRow* getNoteRowFromId(int32_t id);
	/// Return true if successfully shifted. Instrument clips always succeed
	bool shiftHorizontally(ModelStackWithTimelineCounter* modelStack, int32_t amount, bool shiftAutomation,
	                       bool shiftSequenceAndMPE) override;
	bool isEmpty(bool displayPopup = true) override;
	bool containsAnyNotes();
	ModelStackWithNoteRow* getNoteRowOnScreen(int32_t yDisplay, ModelStackWithTimelineCounter* modelStack);
	NoteRow* getNoteRowOnScreen(int32_t yDisplay, Song* song, int32_t* getIndex = nullptr);
	bool currentlyScrollableAndZoomable() override;
	void recordNoteOn(ModelStackWithNoteRow* modelStack, int32_t velocity, bool forcePos0 = false,
	                  int16_t const* mpeValuesOrNull = nullptr, int32_t fromMIDIChannel = MIDI_CHANNEL_NONE);
	void recordNoteOff(ModelStackWithNoteRow* modelStack, int32_t velocity = kDefaultLiftValue);

	void copyBasicsFrom(Clip const* otherClip) override;

	ArpeggiatorSettings arpSettings;

	/// Per-clip, with an instrument-level default, exactly like arpSettings above.
	deluge::modulation::midi_fx::MidiFxSettings midiFxSettings;

	/// The MIDI FX settings that govern one row: the Drum's on a Kit, this Clip's own otherwise.
	///
	/// The same rule the MIDI FX menus follow, in the one place the sequencer needs it. Returns null
	/// only for a Kit row with no Drum, which callers must read as "no MIDI FX here" rather than as
	/// an error - a row can legitimately be in that state while a kit is being built.
	deluge::modulation::midi_fx::MidiFxSettings* midiFxSettingsForRow(NoteRow* noteRow);

	// --- Play FX: the two RS7000 effects that act on the phrase rather than on note events ---
	//
	// On the Clip and not in MidiFxSettings, deliberately. MidiFxSettings is per-drum on a Kit,
	// which is right for a repeat and wrong for these: shifting one drum's phrase relative to the
	// others is a different feature with a different name. These are clip-wide.
	//
	// Both are read in NoteRow::processCurrentPos and nowhere else, which is what keeps them
	// non-destructive: the notes are untouched, only the position they are read at moves.

	/// Ticks the phrase plays early (negative) or late (positive). 0 is off.
	int16_t playFxClockShift{0};

	/// The value at which Beat Stretch does nothing. Named because it appears in NoteRow as the
	/// early-out, as the numerator of the position transform, and as the denominator of the
	/// ticks-out scaling, and a bare 100 in three places invites someone to change one of them.
	static constexpr int32_t kBeatStretchOff = 100;

	/// Rate the phrase plays at against the clip's length, as a percentage. 100 is off. Under 100
	/// the phrase plays faster and repeats within the loop; over 100 it plays slower and the tail
	/// of it is truncated at the loop point, which is what the RS7000 does.
	uint16_t playFxBeatStretch{100};

	/// Clock division for the phrase, as an exact ratio: positive divides, negative multiplies, 0
	/// is off. Read on melodic clips only - on a Kit the row owns it, because a row there is a
	/// drum and dividing one drum against the others is the point of the feature.
	int8_t playFxDivision{0};

	// --- Strum: a chord lands as a sweep rather than as a block ---
	//
	// On the Clip rather than in MidiFxSettings, for the same reason Play FX is and for one more.
	// The first reason is the feature's own shape: a strum is a property of the group of notes
	// hitting together, so two drums of one kit holding different Strum amounts is not a coherent
	// question - see STRUM-FEASIBILITY.md §7.
	//
	// The second is that MidiFxSettings on a Kit clip is a decoy. Every MIDI FX menu writes through
	// midiFxApply, which on a Kit routes to the *Drums*, so the Clip's own copy - including its
	// `enabled` - is never written there and a clip-level setting stored in it could never be
	// switched on. Play FX had this problem first and solved it by living here, with Amount 0 as its
	// own off switch and the Community Features toggle as the master.

	/// Ticks between successive notes of a chord. 0 is off, so an old song plays back untouched.
	uint8_t strumAmount{0};

	/// Which end of the chord sounds first. Stored as the underlying type so the file format is a
	/// plain number and this header stays free of the effect's own enum.
	uint8_t strumDirection{0};

	/// Velocity tilt across the sweep, in percent applied to the last note. 0 is off.
	int8_t strumContour{0};

	// --- Generate: the sequencer writes a rhythm into a row ---
	//
	// Here rather than in MidiFxSettings for two reasons, and the first one is hard. That struct is
	// stored per clip AND per drum and is already exactly at the 16-byte ceiling its own test
	// asserts (tests/headers/midi_fx_headers_test.cpp); three more fields would break the assert and
	// grow every drum of every kit in the song by a quarter, to carry settings for a command that
	// acts on one row at a time.
	//
	// The second is that it would be in the wrong place anyway. Generate writes into a single note
	// row - the one you last auditioned - so there is nothing per-drum to say, exactly as with Strum
	// and Play FX above. One set of settings per clip is the whole of the question.
	//
	// See GENERATIVE-FEASIBILITY.md §2 and modulation/midi_fx/generate.h for why this one writes
	// where everything around it transforms at read time.

	/// How likely each step is to carry a note, 0..100. Unlike the effects above, the default is not
	/// inert - an inert default would mean pressing Generate and getting silence. Nothing happens
	/// until the command is run, so an untouched clip is untouched either way.
	uint8_t generateDensity{30};

	/// Which pattern, 0..99. An index rather than a drawn seed, exactly like Spice's Dice: the same
	/// number is the same pattern on any build, so one you liked is one turn back rather than gone.
	uint8_t generateSeed{0};

	/// Generated note length as a percentage of one step, 5..200. Clamped at write time so a note
	/// can never run into the next one.
	uint16_t generateGatePercent{100};

	/// How far above the auditioned row the generated line may climb, in scale degrees. 0 keeps every
	/// note in that one row, which is a rhythm at one pitch - and is what a Kit wants, where a row is
	/// a drum rather than a pitch, so Kits are held at 0 whatever this says.
	uint8_t generateRange{0};

	/// How often the line jumps rather than steps to a neighbouring degree, 0..100. Inert at Range 0,
	/// where there is nowhere to move.
	uint8_t generateLeap{25};

	// --- Spread: the written part stops playing itself identically every bar ---
	//
	// Here for the same two reasons Strum and Play FX are. On a Kit, MidiFxSettings is per-drum and
	// every MIDI FX menu writes through midiFxApply, which routes to the Drums - so a clip-level
	// setting stored there could never be switched on. And that struct is already exactly at the
	// 16-byte ceiling its own test asserts.
	//
	// **Non-kit only, by design rather than by accident.** A kit row's velocity can already be
	// randomised through the mod matrix by patching master volume to Random, so a second way to do
	// it would be a duplicate; and an octave means nothing on a row that is a drum rather than a
	// pitch. All three read-time hooks test the output type. See SPREAD-FEASIBILITY.md §11.

	/// Velocity deviation either side of what is written, in absolute velocity steps. 0 is off.
	uint8_t spreadVelocity{0};

	/// Gate deviation either side of the written length, as a percentage of it. 0 is off.
	uint8_t spreadGate{0};

	/// How often a note is thrown an octave, up or down, in percent. 0 is off. A probability rather
	/// than a range: an octave is coarse and quantised, so there is no "a little bit" of one.
	uint8_t spreadOctave{0};

	// --- Loop Remix: the loop is chopped into slices and read out of order ---
	//
	// Clip-wide, and on a Kit too, for the reason Play FX and Strum are: a remix is a property of
	// the track, and giving each drum its own permutation would not be a remix. See
	// LOOP-REMIX-FEASIBILITY.md section 5.
	//
	// Nothing here is ever written into the phrase. The sequencer reads the written notes through
	// deluge::modulation::midi_fx::loop_remix::map(), so Family 0 plays the clip exactly as it was
	// written and every one of these is saved with the song.

	/// A melodic clip's remix. On a Kit each NoteRow carries its own instead, and this is unused.
	deluge::modulation::midi_fx::loop_remix::Slot loopRemix;

	// --- Groove: OXI One / Ableton groove templates ---
	//
	// Where it lives follows Loop Remix: a melodic clip's is here, a Kit's drums each carry their own on
	// their NoteRow (AFFECT ENTIRE writes them all). Like Loop Remix it is a transform on how the phrase is
	// read, never written into it - see modulation/midi_fx/groove.h.
	deluge::modulation::midi_fx::groove::Settings groove;

	// --- Stochastic: the rhythm is locked and the pitch is drawn afresh ---
	//
	// Here for the same two reasons Strum, Generate and Spread are: MidiFxSettings is exactly at the
	// 16-byte ceiling its own test asserts, and on a Kit it is a decoy - every MIDI FX menu writes
	// through midiFxApply, which routes to the Drums, so a clip-level setting stored there could
	// never be switched on.
	//
	// The variation index is the exception and is NOT here: Stochastic shares Spice's `spiceDice`
	// rather than carrying a second one, so "which variation am I hearing" is a single number across
	// the group. Non-kit only, so reading it back out of MidiFxSettings is safe. See
	// STOCHASTIC-FEASIBILITY.md §4 and modulation/midi_fx/stochastic.h.

	/// How many of the written notes get their pitch redrawn, in percent. 0 is off, so a song saved
	/// before this existed plays back untouched.
	uint8_t stochasticAmount{0};

	/// How far a redrawn note may move, in scale degrees either side of what is written. Inert at
	/// Amount 0. Never 0 itself - "no width" is what Amount 0 already says, and two spellings of off
	/// is one too many.
	uint8_t stochasticWidth{2};

	ParamManagerForTimeline backedUpParamManagerMIDI;

	bool inScaleMode; // Probably don't quiz this directly - call isScaleModeClip() instead

	int32_t yScroll;

	// TODO: Unscope this once namespacing is done
	deluge::gui::ui::keyboard::KeyboardState keyboardState;

	int32_t ticksTilNextNoteRowEvent{};
	int32_t noteRowsNumTicksBehindClip{};

	LearnedMIDI soundMidiCommand; // This is now handled by the Instrument, but for loading old songs, we need to
	                              // capture and store this

	NoteRowVector noteRows;

	bool wrapEditing;
	uint32_t wrapEditLevel{};

	// These *only* store a valid preset number for the instrument-types that the Clip is not currently on
	int8_t backedUpInstrumentSlot[4]{};
	int8_t backedUpInstrumentSubSlot[4]{};
	String backedUpInstrumentName[2];
	String backedUpInstrumentDirPath[2];

	bool affectEntire;

	bool onKeyboardScreen;

	uint8_t midiBank; // 128 means none
	uint8_t midiSub;  // 128 means none
	uint8_t midiPGM;  // 128 means none

	OutputType outputTypeWhileLoading; // For use only while loading song

	void lengthChanged(ModelStackWithTimelineCounter* modelStack, int32_t oldLength, Action* action = nullptr) override;
	NoteRow* createNewNoteRowForKit(ModelStackWithTimelineCounter* modelStack, bool atStart,
	                                int32_t* getIndex = nullptr);
	Error changeInstrument(ModelStackWithTimelineCounter* modelStack, Instrument* newInstrument,
	                       ParamManagerForTimeline* paramManager, InstrumentRemoval instrumentRemovalInstruction,
	                       InstrumentClip* favourClipForCloningParamManager = nullptr,
	                       bool keepNoteRowsWithMIDIInput = true, bool giveMidiAssignmentsToNewInstrument = false);
	void detachFromOutput(ModelStackWithTimelineCounter* modelStack, bool shouldRememberDrumName,
	                      bool shouldDeleteEmptyNoteRowsAtEndOfList = false, bool shouldRetainLinksToSounds = false,
	                      bool keepNoteRowsWithMIDIInput = true, bool shouldGrabMidiCommands = false,
	                      bool shouldBackUpExpressionParamsToo = true) override;
	void assignDrumsToNoteRows(ModelStackWithTimelineCounter* modelStack, bool shouldGiveMIDICommandsToDrums = false,
	                           int32_t numNoteRowsPreviouslyDeletedFromBottom = 0);
	void unassignAllNoteRowsFromDrums(ModelStackWithTimelineCounter* modelStack, bool shouldRememberDrumNames,
	                                  bool shouldRetainLinksToSounds, bool shouldGrabMidiCommands,
	                                  bool shouldBackUpExpressionParamsToo);
	Error readFromFile(Deserializer& reader, Song* song) override;
	void writeDataToFile(Serializer& writer, Song* song) override;
	void prepNoteRowsForExitingKitMode(Song* song);
	void deleteNoteRow(ModelStackWithTimelineCounter* modelStack, int32_t i);
	int16_t getTopYNote();
	int16_t getBottomYNote();
	uint32_t getWrapEditLevel();
	ModelStackWithNoteRow* getNoteRowForYNote(int32_t yNote, ModelStackWithTimelineCounter* modelStack);
	NoteRow* getNoteRowForYNote(int32_t, int32_t* getIndex = nullptr);
	ModelStackWithNoteRow* getNoteRowForSelectedDrum(ModelStackWithTimelineCounter* modelStack);
	ModelStackWithNoteRow* getNoteRowForDrum(ModelStackWithTimelineCounter* modelStack, Drum* drum);
	NoteRow* getNoteRowForDrum(Drum* drum, int32_t* getIndex = nullptr);
	ModelStackWithNoteRow* getOrCreateNoteRowForYNote(int32_t yNote, ModelStackWithTimelineCounter* modelStack,
	                                                  Action* action = nullptr, bool* scaleAltered = nullptr);

	bool hasSameInstrument(InstrumentClip* otherClip);
	bool isScaleModeClip();
	bool allowNoteTails(ModelStackWithNoteRow* modelStack);
	Error setAudioInstrument(Instrument* newInstrument, Song* song, bool shouldNotifyInstrument,
	                         ParamManager* newParamManager, InstrumentClip* favourClipForCloningParamManager = nullptr);

	void expectEvent() override;
	int32_t getDistanceToNextNote(Note* givenNote, ModelStackWithNoteRow* modelStack);
	void reGetParameterAutomation(ModelStackWithTimelineCounter* modelStack) override;
	void stopAllNotesForMIDIOrCV(ModelStackWithTimelineCounter* modelStack);
	void sendMIDIPGM();
	void noteRemovedFromMode(int32_t yNoteWithinOctave, Song* song);
	void clear(Action* action, ModelStackWithTimelineCounter* modelStack, bool clearAutomation,
	           bool clearSequenceAndMPE) override;
	bool doesProbabilityExist(int32_t apartFromPos, int32_t probability, int32_t secondProbability = -1);
	void clearArea(ModelStackWithTimelineCounter* modelStack, int32_t startPos, int32_t endPos, Action* action);
	ScaleType getScaleType();
	void backupPresetSlot();
	void setPosForParamManagers(ModelStackWithTimelineCounter* modelStack, bool useActualPos = true) override;
	ModelStackWithNoteRow* getNoteRowForDrumName(ModelStackWithTimelineCounter* modelStack, char const* name);
	void compensateVolumeForResonance(ModelStackWithTimelineCounter* modelStack);
	Error undoDetachmentFromOutput(ModelStackWithTimelineCounter* modelStack) override;
	Error setNonAudioInstrument(Instrument* newInstrument, Song* song, ParamManager* newParamManager = nullptr);
	Error setInstrument(Instrument* newInstrument, Song* song, ParamManager* newParamManager,
	                    InstrumentClip* favourClipForCloningParamManager = nullptr);
	void deleteOldDrumNames();
	void ensureScrollWithinKitBounds();
	bool isScrollWithinRange(int32_t scrollAmount, int32_t newYNote);
	Error appendClip(ModelStackWithTimelineCounter* thisModelStack,
	                 ModelStackWithTimelineCounter* otherModelStack) override;
	void instrumentBeenEdited() override;
	Instrument* changeOutputType(ModelStackWithTimelineCounter* modelStack, OutputType newOutputType);
	Error transferVoicesToOriginalClipFromThisClone(ModelStackWithTimelineCounter* modelStackOriginal,
	                                                ModelStackWithTimelineCounter* modelStackClone) override;
	void getSuggestedParamManager(Clip* newClip, ParamManagerForTimeline** suggestedParamManager,
	                              Sound* sound) override;
	ParamManagerForTimeline* getCurrentParamManager() override;
	Error claimOutput(ModelStackWithTimelineCounter* modelStack) override;
	char const* getXMLTag() override { return "instrumentClip"; }
	void finishLinearRecording(ModelStackWithTimelineCounter* modelStack, Clip* nextPendingLoop,
	                           int32_t buttonLatencyForTempolessRecord) override;
	Error beginLinearRecording(ModelStackWithTimelineCounter* modelStack, int32_t buttonPressLatency) override;
	Clip* cloneAsNewOverdub(ModelStackWithTimelineCounter* modelStack, OverDubType newOverdubNature) override;
	bool isAbandonedOverdub() override;
	void quantizeLengthForArrangementRecording(ModelStackWithTimelineCounter* modelStack, int32_t lengthSoFar,
	                                           uint32_t timeRemainder, int32_t suggestedLength,
	                                           int32_t alternativeLongerLength) override;
	void setupAsNewKitClipIfNecessary(ModelStackWithTimelineCounter* modelStack);
	bool getCurrentlyRecordingLinearly() override;
	void abortRecording() override;
	void yDisplayNoLongerAuditioning(int32_t yDisplay, Song* song);
	void shiftOnlyOneNoteRowHorizontally(ModelStackWithNoteRow* modelStack, int32_t shiftAmount, bool shiftAutomation,
	                                     bool shiftSequenceAndMPE);
	int32_t getMaxLength() override;
	bool hasAnyPitchExpressionAutomationOnNoteRows();
	ModelStackWithNoteRow* duplicateModelStackForClipBeingRecordedFrom(ModelStackWithNoteRow* modelStack,
	                                                                   char* otherModelStackMemory);
	void incrementPos(ModelStackWithTimelineCounter* modelStack, int32_t numTicks) override;

	// ----- TimelineCounter implementation -------
	void getActiveModControllable(ModelStackWithTimelineCounter* modelStack) override;

	bool renderSidebar(uint32_t whichRows = 0, RGB image[][kDisplayWidth + kSideBarWidth] = nullptr,
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth] = nullptr) override {
		return instrumentClipView.renderSidebar(whichRows, image, occupancyMask);
	};

protected:
	void posReachedEnd(ModelStackWithTimelineCounter* modelStack) override;
	bool wantsToBeginLinearRecording(Song* song) override;
	bool cloneOutput(ModelStackWithTimelineCounter* modelStack) override;
	void pingpongOccurred(ModelStackWithTimelineCounter* modelStack) override;

private:
	InstrumentClip* instrumentWasLoadedByReferenceFromClip{};

	void deleteEmptyNoteRowsAtEitherEnd(bool onlyIfNoDrum, ModelStackWithTimelineCounter* modelStack,
	                                    bool mustKeepLastOne = true, bool keepOnesWithMIDIInput = true);
	void sendPendingNoteOn(ModelStackWithTimelineCounter* modelStack, PendingNoteOn* pendingNoteOn);

	// --- Strum's deferral ---
	//
	// The notes of a strummed chord after the first are held here until their tick comes round. The
	// entries live in one file-static table in instrument_clip.cpp tagged by Clip rather than in an
	// array on every InstrumentClip: a per-clip queue big enough for a chord costs a couple of
	// hundred bytes on every clip in the song, used or not, and only one clip's worth of them can
	// be in flight at a time in practice. STRUM-FEASIBILITY.md §3a assumed the per-clip version;
	// the tagged table is the same design with the memory taken out, and the dangling-pointer
	// warning in §8.1 is answered the same way either place - by storing the note row's *id* and
	// re-resolving it on the way out.

	/// Holds one note of a chord until `ticksTilDue` has counted down. Returns false if the table
	/// is full, in which case the caller sounds the note now rather than losing it.
	bool strumQueueNote(int32_t noteRowId, PendingNoteOn const* source, int32_t ticksTilDue, uint8_t velocity);

	/// Finds a queued note's row again, or returns null if it is no longer the row that was queued.
	NoteRow* strumResolveRow(int32_t noteRowId, NoteRow* expected);

	/// Forgets anything still queued for one note row. Used when that row is re-triggered, so a
	/// chord arriving on top of the tail of the last one cannot double it.
	void strumDropQueued(int32_t noteRowId);

	/// Forgets everything queued for this clip, and clears the `sequenced` flag of any row that was
	/// only waiting - a note that never sounded has nothing to release.
	void strumFlush();

	/// Sounds everything now due.
	void strumDrainDue(ModelStackWithTimelineCounter* modelStack);

	/// Ticks until the soonest queued note, or -1 when nothing is queued.
	[[nodiscard]] int32_t strumTicksTilNextDue() const;

	/// Brings every queued note `numTicks` closer. Called from incrementPos, alongside the
	/// ticksTilNextNoteRowEvent countdown it mirrors, so a queued note counts down on the same
	/// clock as everything else the clip is waiting for.
	void strumCountDown(int32_t numTicks);
	Error undoUnassignmentOfAllNoteRowsFromDrums(ModelStackWithTimelineCounter* modelStack);
	void deleteBackedUpParamManagerMIDI();
	bool possiblyDeleteEmptyNoteRow(NoteRow* noteRow, bool onlyIfNoDrum, Song* song, bool onlyIfNonNumeric = false,
	                                bool keepIfHasMIDIInput = true);
	void actuallyDeleteEmptyNoteRow(ModelStackWithNoteRow* modelStack);
	void prepareToEnterKitMode(Song* song);
	Error readMIDIParamsFromFile(Deserializer& reader, int32_t readAutomationUpToPos);

	bool lastProbabilities[kNumProbabilityValues]{};
	int32_t lastProbabiltyPos[kNumProbabilityValues]{};
	bool currentlyRecordingLinearly;
};
