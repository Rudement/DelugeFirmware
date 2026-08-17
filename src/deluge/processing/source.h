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
#include "model/sample/sample_controls.h"
#include "storage/multi_range/multi_range_array.h"
#include "util/phase_increment_fine_tuner.h"

class Sound;
class ParamManagerForTimeline;
class WaveTable;
class SampleHolder;
class DxPatch;

class Source {
public:
	Source();
	~Source();

	SampleControls sampleControls;

	OscType oscType;

	// These are not valid for Samples
	int16_t transpose;
	int8_t cents;
	PhaseIncrementFineTuner fineTuner;

	MultiRangeArray ranges;

	DxPatch* dxPatch;

	/// Which Plaits model this source renders when oscType == PLAITS.
	uint8_t plaitsEngine = kPlaitsDefaultEngine;
	/// Take Plaits' AUX output instead of its main one.
	bool plaitsAux = false;

	/// Plaits' low-pass gate.
	///
	/// OFF is the original port behaviour: Plaits' LPG and internal decay are
	/// bypassed and the Deluge's own envelopes do all the amplitude work. Right
	/// for pads and anything sustained, and the safe default -- turning this on
	/// by default would change how every existing Plaits preset sounds.
	///
	/// ON pings the LPG on each note-on, which is where the plucks, bongos and
	/// blooming pads in the demo videos come from. On the module the LPG is
	/// always in circuit unless you patch LEVEL, so ON is the more faithful
	/// setting -- it is just not the more compatible one.
	///
	/// No effect on the eight self-enveloped engines (the three six-op FM
	/// banks, Inharmonic String, Modal Resonator and the three drums): upstream
	/// forces lpg_bypass for those regardless. plaitsDecay still reaches them.
	bool plaitsLpg = false;

	/// LPG decay, 0..50 to match the menu. Maps to Plaits' patch.decay.
	///
	/// Read even when plaitsLpg is off, because the self-enveloped engines use
	/// patch.decay for their own envelopes -- this is what finally gives the
	/// three drums a decay control instead of the hardcoded 0.5 they had.
	uint8_t plaitsDecay = 25;

	/// LPG colour, 0..50. Maps to patch.lpg_colour: how much the gate acts as a
	/// filter (dark, VCF-like) versus a plain VCA (bright). Only read when
	/// plaitsLpg is on.
	uint8_t plaitsLpgColour = 25;

	bool dxPatchChanged = false;
	SampleRepeatMode repeatMode;

	int8_t timeStretchAmount;

	int16_t defaultRangeI; // -1 means none yet

	bool renderInStereo(Sound* s, SampleHolder* sampleHolder = nullptr);
	void setCents(int32_t newCents);
	void recalculateFineTuner();
	int32_t getLengthInSamplesAtSystemSampleRate(int32_t note, bool forTimeStretching = false);
	void detachAllAudioFiles();
	Error loadAllSamples(bool mayActuallyReadFiles);
	void setReversed(bool newReversed);
	int32_t getRangeIndex(int32_t note);
	MultiRange* getRange(int32_t note);
	MultiRange* getOrCreateFirstRange();
	bool hasAtLeastOneAudioFileLoaded();
	void doneReadingFromFile(Sound* sound);
	bool hasAnyLoopEndPoint();
	OscType getOscType();
	void setOscType(OscType newType);

	DxPatch* ensureDxPatch();

private:
	void destructAllMultiRanges();
};
