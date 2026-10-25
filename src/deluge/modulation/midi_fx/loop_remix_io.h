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

#include "modulation/midi_fx/loop_remix.h"

#include <algorithm>
#include <cstring>

/// Saving and loading a Loop Remix Slot. Templates over the writer and reader so the same eight
/// attributes serve both homes - the Clip's <loopRemix> tag and a Kit row's own attributes - without
/// this header having to know either type.
namespace deluge::modulation::midi_fx::loop_remix {

template <class Writer>
void writeAttributes(Writer& writer, const Slot& s) {
	writer.writeAttribute("lrType", s.page.type);
	writer.writeAttribute("lrVariation", s.page.variation);
	writer.writeAttribute("lrInterval", s.page.interval);
	writer.writeAttribute("lrLiveType", s.live.type);
	writer.writeAttribute("lrLiveVariation", s.live.variation);
	writer.writeAttribute("lrLiveInterval", s.live.interval);
	writer.writeAttribute("lrActive", s.live.active ? 1 : 0);
}

/// Reads one attribute if it is one of ours. Clamped on the way in, as Spread is: the hash indexes
/// by Type and Variation and none of it checks. Returns whether it consumed the tag, so the caller
/// knows to exit it.
template <class Reader>
bool readAttribute(const char* name, Reader& reader, Slot& s) {
	const auto clamped = [&reader](int32_t lo, int32_t hi) {
		return static_cast<uint8_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), lo, hi));
	};
	if (!strcmp(name, "lrType")) {
		s.page.type = clamped(kTypeMin, kTypeMax);
	}
	else if (!strcmp(name, "lrVariation")) {
		s.page.variation = clamped(kVariationMin, kVariationMax);
	}
	else if (!strcmp(name, "lrInterval")) {
		s.page.interval = clamped(kIntervalMin, kIntervalMax);
	}
	else if (!strcmp(name, "lrLiveType")) {
		s.live.type = clamped(kTypeMin, kTypeMax);
	}
	else if (!strcmp(name, "lrLiveVariation")) {
		s.live.variation = clamped(kVariationMin, kVariationMax);
	}
	else if (!strcmp(name, "lrLiveInterval")) {
		s.live.interval = clamped(kIntervalMin, kIntervalMax);
	}
	else if (!strcmp(name, "lrActive")) {
		s.live.active = reader.readTagOrAttributeValueInt() != 0;
	}
	else {
		return false;
	}
	return true;
}

} // namespace deluge::modulation::midi_fx::loop_remix
