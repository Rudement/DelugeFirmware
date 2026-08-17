/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
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
#include "deluge/dsp/granular/GranularProcessor.h"
#include "dsp/compressor/rms_feedback.h"
#include "dsp/delay/delay.h"
#include "dsp/gristle.hpp"
#include "dsp/stereo_sample.h"
#include "hid/button.h"
#include "model/fx/stutterer.h"
#include "model/mod_controllable/ModFXProcessor.h"
#include "model/mod_controllable/filters/filter_config.h"
#include "model/mod_controllable/mod_controllable.h"
#include "modulation/knob.h"
#include "modulation/lfo.h"
#include "modulation/midi/midi_knob_array.h"
#include "modulation/params/param_descriptor.h"
#include "modulation/params/param_set.h"
#include "modulation/sidechain/sidechain.h"

class Clip;
class CloudsAdapter;
class Knob;
class MIDICable;
class ModelStack;
class ModelStackWithTimelineCounter;
class ParamManager;
class Serializer;
class Serializer;
class Deserializer;

class ModControllableAudio : public ModControllable {
public:
	ModControllableAudio();
	virtual ~ModControllableAudio();
	virtual void cloneFrom(ModControllableAudio* other);

	void processStutter(std::span<StereoSample> buffer, ParamManager* paramManager);
	void processReverbSendAndVolume(std::span<StereoSample> buffer, int32_t* reverbBuffer, int32_t postFXVolume,
	                                int32_t postReverbVolume, int32_t reverbSendAmount, int32_t pan = 0,
	                                bool doAmplitudeIncrement = false);
	void writeAttributesToFile(Serializer& writer);
	void writeTagsToFile(Serializer& writer);
	virtual Error readTagFromFile(Deserializer& reader, char const* tagName, ParamManagerForTimeline* paramManager,
	                              int32_t readAutomationUpToPos, ArpeggiatorSettings* arpSettings, Song* song);
	void processSRRAndBitcrushing(std::span<StereoSample> buffer, int32_t* postFXVolume, ParamManager* paramManager);
	static void writeParamAttributesToFile(Serializer& writer, ParamManager* paramManager, bool writeAutomation,
	                                       int32_t* valuesForOverride = nullptr);
	static void writeParamTagsToFile(Serializer& writer, ParamManager* paramManager, bool writeAutomation,
	                                 int32_t* valuesForOverride = nullptr);
	static bool readParamTagFromFile(Deserializer& reader, char const* tagName, ParamManagerForTimeline* paramManager,
	                                 int32_t readAutomationUpToPos);
	static void initParams(ParamManager* paramManager);
	virtual void wontBeRenderedForAWhile();
	void beginStutter(ParamManagerForTimeline* paramManager);
	void endStutter(ParamManagerForTimeline* paramManager);
	virtual ModFXType getModFXType() = 0;
	virtual bool setModFXType(ModFXType newType);
	bool offerReceivedCCToLearnedParamsForClip(MIDICable& cable, uint8_t channel, uint8_t ccNumber, uint8_t value,
	                                           ModelStackWithTimelineCounter* modelStack, int32_t noteRowIndex = -1);
	bool offerReceivedCCToLearnedParamsForSong(MIDICable& cable, uint8_t channel, uint8_t ccNumber, uint8_t value,
	                                           ModelStackWithThreeMainThings* modelStackWithThreeMainThings);
	bool offerReceivedPitchBendToLearnedParams(MIDICable& cable, uint8_t channel, uint8_t data1, uint8_t data2,
	                                           ModelStackWithTimelineCounter* modelStack, int32_t noteRowIndex = -1);
	/// Learna knob to a particular parameter.
	///
	/// @param cable The source MIDI cable, or null if learning a mod knob
	virtual bool learnKnob(MIDICable* cable, ParamDescriptor paramDescriptor, uint8_t whichKnob, uint8_t modKnobMode,
	                       uint8_t midiChannel, Song* song);
	bool unlearnKnobs(ParamDescriptor paramDescriptor, Song* song);
	virtual void ensureInaccessibleParamPresetValuesWithoutKnobsAreZero(Song* song) {} // Song may be NULL
	bool isBitcrushingEnabled(ParamManager* paramManager);
	bool isSRREnabled(ParamManager* paramManager);
	bool hasBassAdjusted(ParamManager* paramManager);
	bool hasTrebleAdjusted(ParamManager* paramManager);
	bool hasLowMidAdjusted(ParamManager* paramManager);
	bool hasHighMidAdjusted(ParamManager* paramManager);
	ModelStackWithAutoParam* getParamFromMIDIKnob(MIDIKnob& knob, ModelStackWithThreeMainThings* modelStack) override;

	// The Gristleizer's LFO phase plus its state-variable filter memory. The two channels MUST
	// keep separate filter state: running one filter over an interleaved buffer combs instead
	// of filtering. The LFO phase is deliberately shared so both sides chop together.
	deluge::dsp::gristle::Memory gristleMemory{};

	// EQ
	int32_t bassFreq{}; // These two should eventually not be variables like this
	int32_t trebleFreq{};
	// One-pole coefficients for the two lowpasses bracketing each mid bell (an octave below / above centre)
	int32_t lowMidFreqLo{};
	int32_t lowMidFreqHi{};
	int32_t highMidFreqLo{};
	int32_t highMidFreqHi{};

	int32_t withoutTrebleL;
	int32_t bassOnlyL;
	int32_t withoutTrebleR;
	int32_t bassOnlyR;
	// Mid bell filter state. Stereo state must stay separate per channel — see HANDOFF.md.
	int32_t lowMidLoL;
	int32_t lowMidHiL;
	int32_t lowMidLoR;
	int32_t lowMidHiR;
	// The second bell keeps its own state. Sharing it would couple the two bands, and per-channel
	// state is what keeps stereo material from combing.
	int32_t highMidLoL;
	int32_t highMidHiL;
	int32_t highMidLoR;
	int32_t highMidHiR;

	// Delay
	Delay delay;
	StutterConfig stutterConfig;

	bool sampleRateReductionOnLastTime;
	uint8_t clippingAmount; // Song probably doesn't currently use this?
	FilterMode lpfMode;
	FilterMode hpfMode;
	FilterRoute filterRoute;

	// Mod FX
	ModFXType modFXType_;
	ModFXProcessor modfx{};
	RMSFeedbackCompressor compressor;
	GranularProcessor* grainFX{nullptr};

	// Clouds ----------------------------------------------------------------
	// State lives here, alongside modFXType_/grainFX, so the menu items can
	// reach it through soundEditor.currentModControllable like every other FX
	// setting. RENDERING, though, happens only in
	// GlobalEffectable::processFXForGlobalEffectable -- Clouds' nine
	// parameters are UNPATCHED_GLOBAL, and a Sound's unpatched param set is
	// UnpatchedSound, which is far shorter. Reading them off a Sound would
	// run off the end of that array. The Clouds menu is therefore only ever
	// added to the global FX tree; do not hang it off a Sound.
	CloudsMode cloudsMode{CloudsMode::OFF};
	bool cloudsFreeze{false};
	CloudsAdapter* cloudsFX{nullptr};

	uint32_t lowSampleRatePos{};
	uint32_t highSampleRatePos{};
	StereoSample lastSample;
	StereoSample grabbedSample;
	StereoSample lastGrabbedSample;

	SideChain sidechain; // Song doesn't use this, despite extending this class

	deluge::fast_vector<MIDIKnob> midi_knobs;
	int32_t postReverbVolumeLastTime{};

protected:
	void processFX(std::span<StereoSample> buffer, ModFXType modFXType, int32_t modFXRate, int32_t modFXDepth,
	               const Delay::State& delayWorkingState, int32_t* postFXVolume, ParamManager* paramManager,
	               bool anySoundComingIn, q31_t reverbSendAmount);
	void switchDelayPingPong();
	void switchDelayAnalog();
	void switchDelaySyncType();
	void switchDelaySyncLevel();
	void switchLPFMode();
	void switchHPFMode();
	void clearModFXMemory();

	/// What kind of unpatched parameters this ModControllable uses.
	///
	/// This should be UNPATCHED_GLOBAL for GlobalEffectable and UNPATCHED_SOUND for Sound. If a new ModControllable
	/// subclass is
	deluge::modulation::params::Kind unpatchedParamKind_;

	char const* getFilterTypeDisplayName(FilterType currentFilterType);
	char const* getFilterModeDisplayName(FilterType currentFilterType);
	char const* getLPFModeDisplayName();
	char const* getHPFModeDisplayName();
	char const* getDelayTypeDisplayName();
	char const* getDelayPingPongStatusDisplayName();
	char const* getDelaySyncTypeDisplayName();
	void getDelaySyncLevelDisplayName(char* displayName);
	char const* getSidechainDisplayName();

	void displayFilterSettings(bool on, FilterType currentFilterType);
	void displayDelaySettings(bool on);
	void displaySidechainAndReverbSettings(bool on);
	void displayOtherModKnobSettings(uint8_t whichModButton, bool on);

protected:
	// returns whether it succeeded
	bool enableGrain();
	void disableGrain();

public:
	/// Switch Clouds to `mode`. Constructs the adapter on the first non-OFF
	/// mode and tears it down again on OFF, so the engine costs nothing --
	/// neither the ~180 KB working buffer nor the per-block resampling --
	/// while it is switched off. Returns false if the adapter could not be
	/// allocated, in which case the mode stays OFF.
	bool setCloudsMode(CloudsMode mode);
	void disableClouds();

private:
	void doEQ(bool doBass, bool doTreble, bool doLowMid, bool doHighMid, int32_t* inputL, int32_t* inputR,
	          int32_t bassAmount, int32_t trebleAmount, int32_t lowMidAmount, int32_t highMidAmount);
	ModelStackWithThreeMainThings* addNoteRowIndexAndStuff(ModelStackWithTimelineCounter* modelStack,
	                                                       int32_t noteRowIndex);
	void switchHPFModeWithOff();
	void switchLPFModeWithOff();

	void processGrainFX(std::span<StereoSample> buffer, int32_t modFXRate, int32_t modFXDepth, int32_t* postFXVolume,
	                    UnpatchedParamSet* unpatchedParams, bool anySoundComingIn, q31_t verbAmount);
};
