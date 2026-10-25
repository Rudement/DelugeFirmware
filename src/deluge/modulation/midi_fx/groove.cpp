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

#include "modulation/midi_fx/groove.h"

#include <cstdio>
#include <cstdlib> // llabs
#include <cstring>

#ifndef GROOVE_NATIVE_TEST
#include "definitions.h"
// Last, after <cstdio>: the firmware's own snprintf. Plain snprintf would link newlib's, which needs
// file syscalls this firmware does not have.
#include "lib/printf.h"
#define GROOVE_SDRAM_BSS PLACE_SDRAM_BSS
#else
#define GROOVE_SDRAM_BSS
#endif

namespace deluge::modulation::midi_fx::groove {

namespace {

// --- Built-in templates -------------------------------------------------------------------------
//
// Our own, not copies of anybody's - the OXI's factory grooves are not published, and neither are
// the numbers inside an MPC. Fourteen, as the OXI has. Timing in 1/250ths of a step (+/-125 is half a
// step), velocity as offsets (+/-64).
//
// For a 16th swing of p percent the late 16th of each pair sits at 2p - 1 of a step, so 54% is 0.08
// of a step (20), 57% is 0.14 (35), 60% is 0.2 (50), 63% is 0.26 (65) and 66% is 0.32 (80).

#define T16(a) {0, a, 0, a, 0, a, 0, a, 0, a, 0, a, 0, a, 0, a}
#define SWINGV {20, -15, 5, -15, 20, -15, 5, -15, 20, -15, 5, -15, 20, -15, 5, -15}

const Template kBuiltIn[] = {
    {"Swing 54", 16, 4, T16(20), SWINGV},
    {"Swing 57", 16, 4, T16(35), SWINGV},
    {"Swing 60", 16, 4, T16(50), SWINGV},
    {"Swing 63", 16, 4, T16(65), SWINGV},
    {"Swing 66", 16, 4, T16(80), SWINGV},
    // 8th-note steps: the late 8th of each pair at 2p - 1 of an 8th.
    {"8th Swing 58", 8, 2, {0, 40, 0, 40, 0, 40, 0, 40}, {20, -12, 5, -12, 20, -12, 5, -12}},
    // Triplet shuffle: the late 8th two thirds of the way through the beat.
    {"Shuffle", 8, 2, {0, 83, 0, 83, 0, 83, 0, 83}, {20, -10, 5, -10, 20, -10, 5, -10}},
    // Behind the beat: the backbeats lag, the in-between 16ths lag a little more.
    {"Laid Back",
     16,
     4,
     {0, 15, 5, 18, 25, 15, 5, 18, 0, 15, 5, 18, 25, 15, 5, 18},
     {15, -12, 0, -10, 18, -12, 0, -10, 15, -12, 0, -10, 18, -12, 0, -10}},
    // Ahead of the beat: everything but the downbeats rushes.
    {"Push",
     16,
     4,
     {0, -15, -8, -18, -10, -15, -8, -18, 0, -15, -8, -18, -10, -15, -8, -18},
     {18, -8, 4, -8, 10, -8, 4, -8, 18, -8, 4, -8, 10, -8, 4, -8}},
    // A player's small irregularities. Two bars, so it does not repeat every bar.
    {"Humanize",
     32,
     4,
     {0, 6,  -4, 9, 3, -5, 7, 11, -3, 4, -6, 8,  2, -2, 6, 10,
      0, -4, 5,  7, -6, 3, 9, 4,  -2, 8, -3, 12, 5, -5, 3, 7},
     {6,  -8, 3,  -5, 9, -10, 2, -4, 5, -7, 4, -9, 8,  -6, 1, -3,
      7, -5, 2, -9, 4, -6,  5, -8, 3, -4, 6, -7, 10, -9, 0, -2}},
    // Loose and late, kick a hair early, snare dragging, the 16ths sliding around.
    {"Drunk",
     16,
     4,
     {0, 40, -10, 55, 20, 35, -15, 50, -8, 45, 0, 60, 25, 30, -12, 50},
     {18, -20, 2, -14, 14, -22, -4, -12, 16, -18, 0, -16, 12, -24, -6, -10}},
    // Accent templates: velocity only.
    {"Downbeat Acc", 16, 4, {}, {40, -25, 0, -25, 40, -25, 0, -25, 40, -25, 0, -25, 40, -25, 0, -25}},
    {"Offbeat Acc", 16, 4, {}, {-10, -25, 40, -25, -10, -25, 40, -25, -10, -25, 40, -25, -10, -25, 40, -25}},
    {"Hat Dynamics",
     16,
     4,
     {0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10},
     {30, -30, 10, -20, 30, -30, 10, -20, 30, -30, 10, -20, 30, -30, 10, -20}},
};

#undef T16
#undef SWINGV

constexpr int32_t kNumBuiltIn = sizeof(kBuiltIn) / sizeof(kBuiltIn[0]);

const Template kFlat{"", 16, 4, {}, {}};
static_assert(kNumBuiltIn < kUserBase, "built-in ids would run into user ids");

GROOVE_SDRAM_BSS Template userLibrary[kMaxUser];
int32_t numUser = 0;

bool sameTemplate(const Template& a, const Template& b) {
	if (a.length != b.length || a.stepsPerQuarter != b.stepsPerQuarter || strcmp(a.name, b.name) != 0) {
		return false;
	}
	return memcmp(a.timing, b.timing, a.length) == 0 && memcmp(a.velocity, b.velocity, a.length) == 0;
}

void sanitiseName(char* name) {
	for (char* c = name; *c; c++) {
		if (*c == '"' || *c == '<' || *c == '>' || *c == '&' || static_cast<unsigned char>(*c) < 32) {
			*c = '_';
		}
	}
}

// --- A tiny number reader for the .agr parser ---------------------------------------------------

/// Parses a decimal like "0.284999895937395942" or "-3" into millionths. Returns false on garbage.
bool parseMicros(const char* p, const char* end, int64_t& out) {
	bool neg = false;
	if (p < end && (*p == '-' || *p == '+')) {
		neg = (*p == '-');
		p++;
	}
	int64_t whole = 0;
	bool any = false;
	while (p < end && *p >= '0' && *p <= '9') {
		whole = whole * 10 + (*p - '0');
		if (whole > 1000000000LL) {
			return false;
		}
		p++;
		any = true;
	}
	int64_t frac = 0;
	int64_t scale = 100000;
	if (p < end && *p == '.') {
		p++;
		while (p < end && *p >= '0' && *p <= '9') {
			if (scale > 0) {
				frac += (*p - '0') * scale;
				scale /= 10;
			}
			p++;
			any = true;
		}
	}
	// Exponents are not written by Live, but refuse rather than misread one.
	if (p < end && (*p == 'e' || *p == 'E')) {
		return false;
	}
	if (!any) {
		return false;
	}
	out = whole * 1000000 + frac;
	if (neg) {
		out = -out;
	}
	return true;
}

const char* findIn(const char* p, const char* end, const char* needle) {
	const size_t n = strlen(needle);
	if (n == 0 || p >= end) {
		return nullptr;
	}
	for (const char* q = p; q + n <= end; q++) {
		if (*q == *needle && memcmp(q, needle, n) == 0) {
			return q;
		}
	}
	return nullptr;
}

/// Reads attribute `attr` (written as ` attr="..."`) inside the tag running from tagStart to tagEnd.
bool attrMicros(const char* tagStart, const char* tagEnd, const char* attr, int64_t& out) {
	char needle[32];
	snprintf(needle, sizeof(needle), " %s=\"", attr);
	const char* a = findIn(tagStart, tagEnd, needle);
	if (a == nullptr) {
		return false;
	}
	a += strlen(needle);
	const char* close = static_cast<const char*>(memchr(a, '"', tagEnd - a));
	if (close == nullptr) {
		return false;
	}
	return parseMicros(a, close, out);
}

bool attrIs(const char* tagStart, const char* tagEnd, const char* attr, const char* value) {
	char needle[48];
	snprintf(needle, sizeof(needle), " %s=\"%s\"", attr, value);
	return findIn(tagStart, tagEnd, needle) != nullptr;
}

/// The Value of the first <tag Value="..."/> at or after p.
bool valueOf(const char* p, const char* end, const char* tag, int64_t& out, const char** after = nullptr) {
	char needle[40];
	snprintf(needle, sizeof(needle), "<%s ", tag);
	const char* t = findIn(p, end, needle);
	if (t == nullptr) {
		return false;
	}
	const char* close = static_cast<const char*>(memchr(t, '>', end - t));
	if (close == nullptr) {
		return false;
	}
	if (after != nullptr) {
		*after = close;
	}
	return attrMicros(t, close, "Value", out);
}

} // namespace

int32_t numBuiltIn() {
	return kNumBuiltIn;
}

int32_t numUserLoaded() {
	return numUser;
}

const Template* flatTemplate() {
	return &kFlat;
}

void parseAgrAmounts(const char* data, size_t length, FileAmounts& out) {
	out = FileAmounts{};
	const char* const end = data + length;
	// The amounts sit on the Groove, after its clip.
	const char* from = findIn(data, end, "</MidiClip>");
	if (from == nullptr) {
		from = data;
	}
	int64_t v = 0;
	if (valueOf(from, end, "TimingAmount", v)) {
		out.timing = static_cast<int32_t>(std::clamp<int64_t>(v / 1000000, 0, kAmountMax));
	}
	if (valueOf(from, end, "VelocityAmount", v)) {
		out.velocity = static_cast<int32_t>(std::clamp<int64_t>(v / 1000000, kAmountMin, kAmountMax));
	}
	if (valueOf(from, end, "RandomAmount", v)) {
		out.random = static_cast<int32_t>(std::clamp<int64_t>(v / 1000000, 0, kRandomMax));
	}
}

const Template* templateFor(uint8_t id) {
	if (id == kOff) {
		return nullptr;
	}
	if (id <= kNumBuiltIn) {
		return &kBuiltIn[id - 1];
	}
	if (id >= kUserBase && id - kUserBase < numUser) {
		return &userLibrary[id - kUserBase];
	}
	return nullptr;
}

bool isValidStepsPerQuarter(int32_t spq) {
	return spq == 1 || spq == 2 || spq == 3 || spq == 4 || spq == 6 || spq == 8;
}

uint8_t internUser(const Template& t) {
	Template clean = t;
	clean.name[kNameMax - 1] = 0;
	sanitiseName(clean.name);
	if (clean.length == 0 || clean.length > kMaxSteps || !isValidStepsPerQuarter(clean.stepsPerQuarter)) {
		return kOff;
	}
	for (int32_t i = clean.length; i < kMaxSteps; i++) {
		clean.timing[i] = 0;
		clean.velocity[i] = 0;
	}
	for (int32_t i = 0; i < numUser; i++) {
		if (sameTemplate(userLibrary[i], clean)) {
			return static_cast<uint8_t>(kUserBase + i);
		}
	}
	// Same name, different data: a file edited since it was first loaded. Replace in place, so the
	// library does not fill up with old versions of one groove while someone tweaks it in Live.
	for (int32_t i = 0; i < numUser; i++) {
		if (strcmp(userLibrary[i].name, clean.name) == 0) {
			userLibrary[i] = clean;
			return static_cast<uint8_t>(kUserBase + i);
		}
	}
	if (numUser >= kMaxUser) {
		return kOff;
	}
	userLibrary[numUser] = clean;
	return static_cast<uint8_t>(kUserBase + numUser++);
}

bool parseAgr(const char* data, size_t length, const char* name, Template& out) {
	const char* const end = data + length;

	const char* clip = findIn(data, end, "<MidiClip");
	if (clip == nullptr) {
		return false;
	}
	const char* clipEnd = findIn(clip, end, "</MidiClip>");
	if (clipEnd == nullptr) {
		clipEnd = end;
	}

	int64_t clipLen = 0;
	if (!valueOf(clip, clipEnd, "CurrentEnd", clipLen) || clipLen <= 0) {
		int64_t loopEnd = 0;
		if (!valueOf(clip, clipEnd, "LoopEnd", loopEnd) || loopEnd <= 0) {
			return false;
		}
		clipLen = loopEnd;
	}
	int64_t clipStart = 0;
	valueOf(clip, clipEnd, "CurrentStart", clipStart);
	clipLen -= clipStart;
	if (clipLen <= 0) {
		return false;
	}

	// Notes, in the order written. Live writes them sorted by time within a key, but a groove clip
	// may have several keys, so they are sorted below.
	constexpr int32_t kMaxNotes = 128;
	int64_t times[kMaxNotes];
	int32_t vels[kMaxNotes];
	int32_t n = 0;
	for (const char* p = clip; n < kMaxNotes;) {
		const char* tag = findIn(p, clipEnd, "<MidiNoteEvent ");
		if (tag == nullptr) {
			break;
		}
		const char* close = static_cast<const char*>(memchr(tag, '>', clipEnd - tag));
		if (close == nullptr) {
			break;
		}
		p = close;
		if (attrIs(tag, close, "IsEnabled", "false")) {
			continue;
		}
		int64_t t = 0;
		int64_t v = 100000000; // 100
		if (!attrMicros(tag, close, "Time", t)) {
			continue;
		}
		attrMicros(tag, close, "Velocity", v);
		t -= clipStart;
		if (t < 0 || t >= clipLen) {
			continue;
		}
		times[n] = t;
		vels[n] = static_cast<int32_t>(std::clamp<int64_t>(v / 1000000, 1, 127));
		n++;
	}
	if (n == 0) {
		return false;
	}
	// Insertion sort, keeping each velocity with its note.
	for (int32_t i = 1; i < n; i++) {
		const int64_t t = times[i];
		const int32_t v = vels[i];
		int32_t j = i - 1;
		while (j >= 0 && times[j] > t) {
			times[j + 1] = times[j];
			vels[j + 1] = vels[j];
			j--;
		}
		times[j + 1] = t;
		vels[j + 1] = v;
	}

	// The grid. Live's Groove Pool "Base" is stored as <Grid Value=.../> on the Groove itself (after the
	// clip): 0 = 1/4, 1 = 1/8, 2 = 1/8T, 3 = 1/16, 4 = 1/16T, 5 = 1/32. Prefer what the notes say when
	// the clip is the usual one note per cell - that is the shape a groove clip has - and fall back to
	// Base otherwise.
	static constexpr int32_t kBaseSpq[6] = {1, 2, 3, 4, 6, 8};
	int32_t spq = 0;
	{
		static constexpr int32_t kCandidates[6] = {4, 2, 8, 6, 3, 1};
		for (int32_t c : kCandidates) {
			const int64_t cellMicros = 1000000 / c;
			const int64_t cells = (clipLen + cellMicros / 2) / cellMicros;
			if (cells == n && std::llabs(cells * cellMicros - clipLen) <= clipLen / 100 + 1) {
				spq = c;
				break;
			}
		}
	}
	if (spq == 0) {
		int64_t base = -1;
		if (valueOf(clipEnd, end, "Grid", base)) {
			const int32_t b = static_cast<int32_t>(base / 1000000);
			if (b >= 0 && b < 6) {
				spq = kBaseSpq[b];
			}
		}
		if (spq == 0) {
			spq = 4;
		}
	}

	const int64_t cell = 1000000 / spq; // in beats-millionths; exact for every option but 3 and 6
	int32_t steps = static_cast<int32_t>((clipLen + cell / 2) / cell);
	steps = std::clamp<int32_t>(steps, 1, kMaxSteps);

	int32_t slotVel[kMaxSteps];
	int64_t slotOff[kMaxSteps];
	bool filled[kMaxSteps] = {};
	if (n == steps) {
		// One note per cell: the i-th note belongs to cell i however far its feel moves it, so a 75%
		// swing - half a cell late - still lands in its own cell.
		for (int32_t i = 0; i < n; i++) {
			slotOff[i] = times[i] - i * cell;
			slotVel[i] = vels[i];
			filled[i] = true;
		}
	}
	else {
		// Rests: each note to its nearest grid line; the first claim on a cell wins.
		for (int32_t i = 0; i < n; i++) {
			int32_t s = static_cast<int32_t>((times[i] + cell / 2) / cell);
			if (s >= steps) {
				s -= steps; // a note pushed early across the loop point belongs to cell 0
			}
			if (s < 0 || s >= steps || filled[s]) {
				continue;
			}
			slotOff[s] = times[i] - s * cell;
			if (s == 0 && slotOff[s] > cell) {
				slotOff[s] -= clipLen;
			}
			slotVel[s] = vels[i];
			filled[s] = true;
		}
	}

	// Velocity offsets around the mean of the notes there are, so the groove neither lifts nor drops the
	// part overall - Accent then decides how much of the shape you hear.
	int32_t sum = 0;
	int32_t count = 0;
	for (int32_t i = 0; i < steps; i++) {
		if (filled[i]) {
			sum += slotVel[i];
			count++;
		}
	}
	const int32_t mean = count ? (sum + count / 2) / count : 100;

	Template t{};
	snprintf(t.name, sizeof(t.name), "%s", name != nullptr && *name ? name : "Groove");
	t.length = static_cast<uint8_t>(steps);
	t.stepsPerQuarter = static_cast<uint8_t>(spq);
	for (int32_t i = 0; i < steps; i++) {
		if (!filled[i]) {
			continue;
		}
		const int64_t units = (slotOff[i] * kTimingUnit + (slotOff[i] >= 0 ? cell / 2 : -cell / 2)) / cell;
		t.timing[i] = static_cast<int8_t>(std::clamp<int64_t>(units, -kTimingMax, kTimingMax));
		t.velocity[i] = static_cast<int8_t>(std::clamp<int32_t>(slotVel[i] - mean, -kVelocityMax, kVelocityMax));
	}
	out = t;
	return true;
}

// --- Song-file encoding ---------------------------------------------------------------------------

namespace {
constexpr char kHex[] = "0123456789ABCDEF";

int32_t hexNibble(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

/// Next '|'-separated field as an integer. Advances p past the separator.
/// Hand-rolled rather than strtol: strtol drags newlib's stdio into the link, and this firmware has
/// no file syscalls for it to call.
bool nextInt(const char*& p, int32_t& out) {
	const char* q = p;
	bool neg = false;
	if (*q == '-') {
		neg = true;
		q++;
	}
	int32_t v = 0;
	const char* digits = q;
	while (*q >= '0' && *q <= '9' && q - digits < 9) {
		v = v * 10 + (*q - '0');
		q++;
	}
	if (q == digits || (*q != '|' && *q != 0)) {
		return false;
	}
	out = neg ? -v : v;
	p = (*q == '|') ? q + 1 : q;
	return true;
}

bool nextHex(const char*& p, int8_t* dst, int32_t count) {
	for (int32_t i = 0; i < count; i++) {
		const int32_t hi = hexNibble(p[0]);
		const int32_t lo = hi < 0 ? -1 : hexNibble(p[1]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		dst[i] = static_cast<int8_t>(static_cast<uint8_t>((hi << 4) | lo));
		p += 2;
	}
	if (*p != '|') {
		return false;
	}
	p++;
	return true;
}
} // namespace

size_t encode(const Settings& s, char* buf, size_t bufLen) {
	if (s.isDefault() || bufLen == 0) {
		return 0;
	}
	const Template* t = templateFor(s.templateId);
	int w = 0;
	if (s.templateId == kOff || t == nullptr || s.templateId <= kNumBuiltIn) {
		// Off with amounts dialled in is saved as built-in 0, so the amounts survive.
		const int32_t id = (t == nullptr) ? 0 : s.templateId;
		w = snprintf(buf, bufLen, "b|%d|%d|%d|%d|%d", static_cast<int>(id), s.accent, s.time, s.random, s.offset);
		return (w > 0 && static_cast<size_t>(w) < bufLen) ? static_cast<size_t>(w) : 0;
	}
	w = snprintf(buf, bufLen, "u|%d|%d|%d|%d|%d|%d|", s.accent, s.time, s.random, s.offset, t->stepsPerQuarter,
	             t->length);
	if (w <= 0) {
		return 0;
	}
	size_t pos = static_cast<size_t>(w);
	const size_t needed = pos + 4 * t->length + 2 + strlen(t->name) + 1;
	if (needed > bufLen) {
		return 0;
	}
	for (int32_t pass = 0; pass < 2; pass++) {
		const int8_t* src = pass == 0 ? t->timing : t->velocity;
		for (int32_t i = 0; i < t->length; i++) {
			const auto b = static_cast<uint8_t>(src[i]);
			buf[pos++] = kHex[b >> 4];
			buf[pos++] = kHex[b & 15];
		}
		buf[pos++] = '|';
	}
	const size_t nameLen = strlen(t->name);
	memcpy(buf + pos, t->name, nameLen);
	pos += nameLen;
	buf[pos] = 0;
	return pos;
}

bool decode(const char* str, Settings& s) {
	s = Settings{};
	if (str == nullptr || (str[0] != 'b' && str[0] != 'u') || str[1] != '|') {
		return false;
	}
	const bool user = str[0] == 'u';
	const char* p = str + 2;
	Settings r;
	int32_t id = 0;
	if (!user && !nextInt(p, id)) {
		return false;
	}
	int32_t accent = 0;
	int32_t time = 0;
	int32_t random = 0;
	int32_t offset = 0;
	if (!nextInt(p, accent) || !nextInt(p, time) || !nextInt(p, random) || !nextInt(p, offset)) {
		return false;
	}
	r.accent = static_cast<int8_t>(std::clamp<int32_t>(accent, kAmountMin, kAmountMax));
	r.time = static_cast<int8_t>(std::clamp<int32_t>(time, kAmountMin, kAmountMax));
	r.random = static_cast<uint8_t>(std::clamp<int32_t>(random, 0, kRandomMax));
	r.offset = static_cast<uint8_t>(std::clamp<int32_t>(offset, 0, kMaxSteps - 1));
	if (!user) {
		r.templateId = (id >= 1 && id <= kNumBuiltIn) ? static_cast<uint8_t>(id) : kOff;
		s = r;
		return true;
	}
	int32_t spq = 0;
	int32_t length = 0;
	if (!nextInt(p, spq) || !nextInt(p, length) || !isValidStepsPerQuarter(spq) || length < 1
	    || length > kMaxSteps) {
		return false;
	}
	Template t{};
	t.stepsPerQuarter = static_cast<uint8_t>(spq);
	t.length = static_cast<uint8_t>(length);
	if (!nextHex(p, t.timing, length) || !nextHex(p, t.velocity, length)) {
		return false;
	}
	for (int32_t i = 0; i < length; i++) {
		t.timing[i] = static_cast<int8_t>(std::clamp<int32_t>(t.timing[i], -kTimingMax, kTimingMax));
		t.velocity[i] = static_cast<int8_t>(std::clamp<int32_t>(t.velocity[i], -kVelocityMax, kVelocityMax));
	}
	snprintf(t.name, sizeof(t.name), "%s", *p ? p : "Groove");
	// A full library still keeps the amounts, so the song saves them back out; the groove itself is off
	// until there is room.
	r.templateId = internUser(t);
	s = r;
	return true;
}

} // namespace deluge::modulation::midi_fx::groove
