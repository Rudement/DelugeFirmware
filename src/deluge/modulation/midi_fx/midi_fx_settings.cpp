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

#include "modulation/midi_fx/midi_fx_settings.h"
#include "modulation/midi_fx/spice.h"
#include "storage/storage_manager.h"
#include "util/misc.h"
#include <algorithm>
#include <cstring>

namespace deluge::modulation::midi_fx {

void MidiFxSettings::cloneFrom(MidiFxSettings const* other) {
	enabled = other->enabled;
	velocityOffset = other->velocityOffset;
	gateTimePercent = other->gateTimePercent;
	retrigRepeats = other->retrigRepeats;
	retrigSyncLevel = other->retrigSyncLevel;
	retrigSyncType = other->retrigSyncType;
	retrigDecayPercent = other->retrigDecayPercent;
	delayLevelPercent = other->delayLevelPercent;
	delayRepeats = other->delayRepeats;
	delaySyncLevel = other->delaySyncLevel;
	delaySyncType = other->delaySyncType;
	delayFbVelocity = other->delayFbVelocity;
	delayPitchShift = other->delayPitchShift;
	delayNoteRandom = other->delayNoteRandom;
	delayFbClockPercent = other->delayFbClockPercent;
	spiceAmount = other->spiceAmount;
	// The dice travels with the amount on a clone even though it is not saved to file. A clone is a
	// copy of something you are listening to right now, and it should sound the same.
	spiceDice = other->spiceDice;
	harmonize = other->harmonize;
}

void MidiFxSettings::finishReading() {
	if (!enabled) {
		// Saved with the old Enabled switch off: whatever was dialled in was silent, so keep it silent.
		*this = MidiFxSettings{};
	}
}

void MidiFxSettings::writeCommonParamsToFile(Serializer& writer) {
	// Attribute names are the file format. Once a song is saved with one, renaming it silently
	// orphans the setting - see the four-band EQ note in params/param.h for how that goes.
	writer.writeAttribute("midiFxEnabled", enabled);
	writer.writeAttribute("midiFxVelocityOffset", velocityOffset);
	writer.writeAttribute("midiFxGateTime", gateTimePercent);
	writer.writeAttribute("midiFxRetrigRepeats", retrigRepeats);
	writer.writeAttribute("midiFxRetrigSyncLevel", util::to_underlying(retrigSyncLevel));
	writer.writeAttribute("midiFxRetrigSyncType", util::to_underlying(retrigSyncType));
	writer.writeAttribute("midiFxRetrigDecay", retrigDecayPercent);
	writer.writeAttribute("midiFxDelayLevel", delayLevelPercent);
	writer.writeAttribute("midiFxDelayRepeats", delayRepeats);
	writer.writeAttribute("midiFxDelaySyncLevel", util::to_underlying(delaySyncLevel));
	writer.writeAttribute("midiFxDelaySyncType", util::to_underlying(delaySyncType));
	writer.writeAttribute("midiFxDelayFbVel", delayFbVelocity);
	writer.writeAttribute("midiFxDelayPitch", delayPitchShift);
	writer.writeAttribute("midiFxDelayNoteRnd", delayNoteRandom);
	writer.writeAttribute("midiFxDelayFbClock", delayFbClockPercent);
	writer.writeAttribute("midiFxSpice", spiceAmount);
	writer.writeAttribute("midiFxSpiceDice", spiceDice);
	writer.writeAttribute("midiFxHarmonize", harmonize);
}

bool MidiFxSettings::readCommonTagsFromFile(Deserializer& reader, char const* tagName) {
	if (strcmp(tagName, "midiFxEnabled") == 0) {
		enabled = reader.readTagOrAttributeValueInt() != 0;
	}
	else if (strcmp(tagName, "midiFxVelocityOffset") == 0) {
		// Clamped on the way in as well as on the way out. A hand-edited or truncated song file
		// should not be able to push this out of range and have the effect emit velocity 0.
		// Explicit <int32_t>, and it matters. readTagOrAttributeValueInt() returns int32_t, which is
		// `long int` on arm-none-eabi and plain `int` on x86_64 - so bare std::clamp(value, -64, 63)
		// deduces cleanly on a host compiler and fails on the target with "deduced conflicting types
		// for parameter 'const _Tp'". Any std::clamp/min/max mixing an int32_t with a literal needs
		// the same treatment.
		velocityOffset = static_cast<int8_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), -64, 63));
	}
	else if (strcmp(tagName, "midiFxGateTime") == 0) {
		// Upper bound raised from 100 to 400 when Gate Time learned to lengthen. A song saved by
		// the shorten-only build carries a value in the old range and reads back unchanged; a song
		// saved by this build and opened on that one clamps to 100, which is "off". Both
		// directions land somewhere sane, which is the test that matters.
		gateTimePercent = static_cast<uint16_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), 5, 400));
	}
	else if (strcmp(tagName, "midiFxRetrigRepeats") == 0) {
		retrigRepeats = static_cast<uint8_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), 0, 16));
	}
	else if (strcmp(tagName, "midiFxRetrigSyncLevel") == 0) {
		retrigSyncLevel = static_cast<SyncLevel>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), 0, 9));
	}
	else if (strcmp(tagName, "midiFxRetrigSyncType") == 0) {
		// 0, 10 and 19, not 0 to 2: SyncType values are the index of the first option of that type
		// in the selection menu (model/sync.h:10). Clamping to 2 here silently turned every triplet
		// and dotted retrigger back into something meaningless on reload.
		retrigSyncType = static_cast<SyncType>(
		    std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), SYNC_TYPE_EVEN, SYNC_TYPE_DOTTED));
	}
	else if (strcmp(tagName, "midiFxRetrigDecay") == 0) {
		retrigDecayPercent = static_cast<uint8_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), 0, 100));
	}
	else if (strcmp(tagName, "midiFxDelayRepeats") == 0) {
		delayRepeats = static_cast<uint8_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), 0, 64));
	}
	else if (strcmp(tagName, "midiFxDelaySyncLevel") == 0) {
		delaySyncLevel = static_cast<SyncLevel>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), 0, 9));
	}
	else if (strcmp(tagName, "midiFxDelaySyncType") == 0) {
		delaySyncType = static_cast<SyncType>(
		    std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), SYNC_TYPE_EVEN, SYNC_TYPE_DOTTED));
	}
	else if (strcmp(tagName, "midiFxDelayLevel") == 0) {
		// Absent in a song saved before DELAY LEVEL existed. Such a song had no separate echo level —
		// the old build set it from the (now removed) feedback percent — so it reads as the default
		// 100, a plain echo at the played level, which is the least surprising fallback.
		delayLevelPercent = static_cast<uint8_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), 0, 127));
	}
	else if (strcmp(tagName, "midiFxDelayFbVel") == 0) {
		delayFbVelocity = static_cast<int8_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), -127, 127));
	}
	else if (strcmp(tagName, "midiFxDelayNoteRnd") == 0) {
		delayNoteRandom = reader.readTagOrAttributeValueInt() != 0;
	}
	else if (strcmp(tagName, "midiFxDelayFbClock") == 0) {
		delayFbClockPercent = static_cast<int8_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), -100, 100));
	}
	else if (strcmp(tagName, "midiFxDelayPitch") == 0) {
		delayPitchShift = static_cast<int8_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), -24, 24));
	}
	else if (strcmp(tagName, "midiFxSpice") == 0) {
		spiceAmount = static_cast<uint8_t>(
		    std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), spice::kAmountOff, spice::kAmountMax));
	}
	else if (strcmp(tagName, "midiFxSpiceDice") == 0) {
		// Absent in a song saved by the build that did not write this, which reads as 0 - a valid
		// variation, so such a song still plays rather than misbehaving.
		spiceDice = static_cast<uint8_t>(
		    std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), spice::kDiceMin, spice::kDiceMax));
	}
	else if (strcmp(tagName, "midiFxHarmonize") == 0) {
		// Absent in a song saved before Harmonize existed, which reads as off - the one behaviour
		// that cannot surprise anyone opening an old song.
		harmonize = reader.readTagOrAttributeValueInt() != 0;
	}
	else {
		return false;
	}
	// exitTag with the tag name, and only on the path that consumed it - same shape as
	// ArpeggiatorSettings::readCommonTagsFromFile (arpeggiator.cpp:1786).
	reader.exitTag(tagName);
	return true;
}

} // namespace deluge::modulation::midi_fx
