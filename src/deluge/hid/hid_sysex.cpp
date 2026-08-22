#include "hid/hid_sysex.h"
#include "gui/ui/keyboard/chords.h" // Chroma: the synced spelling lean (gChromaSpelling) packed into 0x43
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "gui/ui_timer_manager.h"
#include "hid/display/oled.h"
#include "hid/display/seven_segment.h"
#include "hid/led/pad_leds.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "io/midi/sysex.h"
#include "memory/general_memory_allocator.h"
#include "model/song/song.h" // Chroma: resync — read the live key/scale to push on handshake
#include "processing/engines/audio_engine.h"
#include "util/pack.h"
#include <cstring>

MIDIDevice* midiDisplayDevice = nullptr;
int32_t midiDisplayUntil = 0;
uint8_t* oledDeltaImage = nullptr;
bool oledDeltaForce = true;

// Chroma: the last device that handshaked with us over HID SysEx. LEARN context events go out on this. The
// host registers it by sending any HID request (e.g. the 7-seg request the screen mirror already uses).
static MIDIDevice* lastHidCable = nullptr;

// Chroma: last view slug pushed to the host (0x45 de-dupe). File-scope so the handshake RESYNC can force a re-push.
static UIType lastSentView = UIType::NONE;
// Chroma: whether we've pushed the "fx" context for the current gold-knob session (re-armed on any real view change).
static bool fxContextSent = false;

void HIDSysex::sysexReceived(MIDIDevice* device, uint8_t* data, int32_t len) {
	lastHidCable = device;
	if (len < 3) {
		// Chroma RESYNC: the bare handshake (the bridge sends it on connect + every ~5s). Push the CURRENT
		// global key+scale (no notes) so the CT snaps into sync immediately and can't sit stale.
		if (currentSong != nullptr) {
			sendChordState((uint8_t)(((currentSong->key.rootNote % 12) + 12) % 12), nullptr, 0, "", 0, 0,
			               (uint8_t)currentSong->getCurrentScale());
		}
		// Also re-push the current on-screen view so a just-connected Companion's Learn tab syncs at once.
		lastSentView = UIType::NONE; // clear the de-dupe so the next line always sends
		sendActiveViewIfChanged();
		return;
	}
	// first three bytes are already used, next is command
	switch (data[1]) {
	case 0:
		requestOLEDDisplay(device, data, len);
		break;

	case 1:
		request7SegDisplay(device, data, len);
		break;

	case 3:
		sendPadGrid(device); // Chroma: mirror the live pad-LED grid
		break;

	default:
		break;
	}
}

void HIDSysex::requestOLEDDisplay(MIDIDevice* device, uint8_t* data, int32_t len) {
	//	if (data[4] == 0 or data[4] == 1) {
	if (data[2] == 0 or data[2] == 1) {
		// sendOLEDData(device, (data[4] == 1)); // Adjustting by -2 to correct for payload offset.
		sendOLEDData(device, (data[2] == 1));
	}
	// else if (data[4] == 2 || data[4] == 3) {
	else if (data[2] == 2 || data[2] == 3) {
		// bool force = (data[4] == 3);
		bool force = (data[2] == 3);
		midiDisplayDevice = device;
		// two seconds
		midiDisplayUntil = AudioEngine::audioSampleTimer + 2 * kSampleRate;
		if (display->haveOLED()) {
			if (force) {
				oledDeltaForce = true;
			}

			if (oledDeltaImage == nullptr) {
				oledDeltaImage = (uint8_t*)GeneralMemoryAllocator::get().allocMaxSpeed(
				    sizeof(uint8_t[OLED_MAIN_HEIGHT_PIXELS >> 3][OLED_MAIN_WIDTH_PIXELS]));
			}
		}
		sendDisplayIfChanged();
		if (force && display->have7SEG()) {
			send7SegData(device);
		}
	}
	// else if (data[4] == 4) { // SWAP
	else if (data[2] == 4) { // SWAP
		deluge::hid::display::swapDisplayType();
		oledDeltaForce = true;
	}
}

void HIDSysex::sendDisplayIfChanged() {
	// Chroma: piggyback the active-view check on the display poll; it self-gates and de-dupes.
	sendActiveViewIfChanged();
	// NB: timer is only used for throttling, under good conditions sending
	// is driven by the display subsystem only
	uiTimerManager.unsetTimer(TimerName::SYSEX_DISPLAY);
	if (midiDisplayDevice == nullptr || AudioEngine::audioSampleTimer > midiDisplayUntil) {
		return;
	}
	// not exact, but if more than half than the serial buffer is still full,
	// we need to slow down a little. (USB buffer is larger and should be consumed much quicker)
	if (midiDisplayDevice->sendBufferSpace() < 512) {
		uiTimerManager.setTimer(TimerName::SYSEX_DISPLAY, 100);
		return;
	}

	if (display->haveOLED()) {
		sendOLEDDataDelta(midiDisplayDevice, false);
	}
	if (display->have7SEG()) {
		send7SegData(midiDisplayDevice);
	}
}

void HIDSysex::sendOLEDData(MIDIDevice* device, bool rle) {
	if (display->haveOLED()) {
		const int32_t data_size = 768;
		const int32_t max_packed_size = 922;
		// 		uint8_t reply_hdr[5] = {0xf0, 0x7d, 0x02, 0x40, rle ? 0x01_u8 : 0x00_u8};
		uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x40, rle ? 0x01_u8 : 0x00_u8};
		uint8_t* reply = midiEngine.sysex_fmt_buffer;
		// 		memcpy(reply, reply_hdr, 5);
		memcpy(reply, reply_hdr, 8); //
		                             //		reply[5] = 0; // nominally 32*data[5] is start pos for a delta
		reply[8] = 0;                // nominally 32*data[8] is start pos for a delta //

		int32_t packed;
		if (rle) {
			packed =
			    //				pack_8to7_rle(reply + 6, max_packed_size,
			    // deluge::hid::display::OLED::oledCurrentImage[0], data_size);
			    pack_8to7_rle(reply + 9, max_packed_size, deluge::hid::display::OLED::oledCurrentImage[0], data_size);
		}
		else {
			//			packed = pack_8bit_to_7bit(reply + 6, max_packed_size,
			// deluge::hid::display::OLED::oledCurrentImage[0], 			                           data_size);
			packed = pack_8bit_to_7bit(reply + 9, max_packed_size, deluge::hid::display::OLED::oledCurrentImage[0],
			                           data_size); //
		}
		if (packed < 0) {
			display->popupTextTemporary("error: fail");
		}
		//		reply[6 + packed] = 0xf7; // end of transmission
		reply[9 + packed] = 0xf7;              // end of transmission
		                                       // 		device->sendSysex(reply, packed + 7); //
		device->sendSysex(reply, packed + 10); //
	}
}

void HIDSysex::request7SegDisplay(MIDIDevice* device, uint8_t* data, int32_t len) {
	if (data[2] == 0) { // was 4
		send7SegData(device);
	}
	else if (data[2] == 1) { // Chroma: request the real text behind the segments
		send7SegText(device);
	}
}

void HIDSysex::send7SegData(MIDIDevice* device) {
	if (display->have7SEG()) {
		// aschually 8 segments if you count the dot
		auto data = display->getLast();
		const int32_t packed_data_size = 5;
		//		uint8_t reply[12] = {0xf0, 0x7d, 0x02, 0x41, 0x00, 0x00};
		uint8_t reply[15] = {0xf0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x41, 0x00, 0x00};
		//  	pack_8bit_to_7bit(reply + 6, packed_data_size, data.data(), data.size());
		// int32_t pack_8bit_to_7bit(uint8_t* dst, int32_t dst_size, uint8_t* src, int32_t src_len);
		pack_8bit_to_7bit(reply + 9, packed_data_size, data.data(), data.size());
		//		reply[6 + packed_data_size] = 0xf7; // end of transmission
		reply[9 + packed_data_size] = 0xf7; // end of transmission
		                                    //		device->sendSysex(reply, packed_data_size + 7);
		device->sendSysex(reply, packed_data_size + 10);
	}
}

void HIDSysex::sendOLEDDataDelta(MIDIDevice* device, bool force) {
	const int32_t data_size = 768;
	const int32_t max_packed_size = 922;

	uint8_t* current = deluge::hid::display::OLED::oledCurrentImage[0];

	int32_t first_change = 9000;
	int32_t last_change = 0;
	int32_t* blkdata_new = (int32_t*)current;
	int32_t* blkdata_old = (int32_t*)oledDeltaImage;

	const int32_t word_size = data_size >> 2;

	if (force || oledDeltaForce) {
		first_change = 0;
		last_change = word_size - 1;
	}
	else {
		for (int32_t blk = 0; blk < word_size; blk++) {
			if (blkdata_new[blk] != blkdata_old[blk]) {
				if (first_change > blk) {
					first_change = blk;
				}
				last_change = blk;
			}
		}
	}

	if (first_change > word_size) {
		return;
	}

	int start = first_change / 2;
	int len = (last_change / 2) - start + 1;
	//	int8_t reply_hdr[5] = {0xf0, 0x7d, 0x02, 0x40, 0x02};
	uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x40, 0x02};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	memcpy(reply, reply_hdr, sizeof(reply_hdr));
	//	reply[5] = start;
	reply[sizeof(reply_hdr) + 0] = start;
	//  reply[6] = len;
	reply[sizeof(reply_hdr) + 1] = len;
	// 	int32_t packed = pack_8to7_rle(reply + 7, max_packed_size, current + 8 * start, 8 * len);
	int32_t packed = pack_8to7_rle(reply + 10, max_packed_size, current + 8 * start, 8 * len);
	if (packed <= 0) {
		return;
	}
	memcpy(oledDeltaImage + (8 * start), current + (8 * start), 8 * len);
	oledDeltaForce = false;
	// reply[7 + packed] = 0xf7; // end of transmission
	reply[10 + packed] = 0xf7; // end of transmission //
	// device->sendSysex(reply, packed + 8);
	device->sendSysex(reply, packed + 11);
}

// Chroma: reply with the actual text on the 7-seg as 7-bit ASCII, so the host shows "BASS" instead of
// reverse-engineering ambiguous segments (S/5 and Z/2 share a pattern). The display kept the literal
// string when it was set, so there's nothing to guess.
//   reply: F0 00 21 7B 01 02 42 <n> <ascii x n> F7
void HIDSysex::send7SegText(MIDIDevice* device) {
	if (!display->have7SEG()) {
		return;
	}
	std::string_view text = display->getLastTextForHost();
	uint8_t reply[8 + 64 + 1] = {0xf0, 0x00, 0x21, 0x7b, 0x01, 0x02, 0x42, 0x00};
	uint8_t n = 0;
	for (size_t i = 0; i < text.size() && n < 64; i++) {
		reply[8 + n++] = (uint8_t)text[i] & 0x7f; // 7-bit ASCII rides SysEx unescaped
	}
	reply[7] = n; // count
	reply[8 + n] = 0xf7;
	device->sendSysex(reply, 9 + n);
}

// Chroma: F0 00 21 7B 01 4C <region> <x> <y> <n> <contextId ASCII...> F7  (see chroma-schema SCHEMA.md 2.1).
// contextId is 7-bit ASCII so it rides SysEx unescaped. No-op until a host has handshaked over HID SysEx.
void HIDSysex::sendLearnContext(uint8_t region, uint8_t x, uint8_t y, const char* contextId) {
	if (lastHidCable == nullptr || contextId == nullptr) {
		return;
	}
	uint8_t msg[11 + 127];
	msg[0] = 0xf0;
	msg[1] = 0x00;
	msg[2] = 0x21;
	msg[3] = 0x7b;
	msg[4] = 0x01;
	msg[5] = SysEx::SysexCommands::LearnContext; // 0x4C
	msg[6] = region & 0x7f;
	msg[7] = x & 0x7f;
	msg[8] = y & 0x7f;
	uint8_t n = 0;
	for (const char* c = contextId; *c && n < 127; c++) {
		msg[10 + n++] = (uint8_t)(*c) & 0x7f; // mask to 7-bit; contextIds are ASCII by contract
	}
	msg[9] = n;
	msg[10 + n] = 0xf7;
	lastHidCable->sendSysex(msg, 11 + n);
}

// Chroma: F0 00 21 7B 01 43 <keyRoot> <nNotes> <note...> <nCtx> <ctx ASCII> F7.
// Pushed on every NORMAL palette pick. Carries the runtime truth (voiced MIDI notes) + the graph key, so the
// host renders the real chord regardless of what the 7-seg shows. No-op until a host handshakes.
void HIDSysex::sendChordState(uint8_t keyRoot, const int16_t* notes, uint8_t numNotes, const char* contextId,
                              int8_t spread, int8_t inversion, uint8_t scale) {
	if (lastHidCable == nullptr) {
		return;
	}
	uint8_t msg[200];
	uint8_t i = 0;
	msg[i++] = 0xf0;
	msg[i++] = 0x00;
	msg[i++] = 0x21;
	msg[i++] = 0x7b;
	msg[i++] = 0x01;
	msg[i++] = SysEx::SysexCommands::ChordState; // 0x43
	msg[i++] = keyRoot & 0x7f;
	uint8_t cnt = (numNotes > 24) ? 24 : numNotes;
	msg[i++] = cnt;
	for (uint8_t k = 0; k < cnt; k++) {
		msg[i++] = (uint8_t)(notes[k]) & 0x7f;
	}
	uint8_t nc = 0;
	if (contextId != nullptr) {
		for (const char* c = contextId; *c && nc < 120; c++) {
			nc++;
		}
	}
	msg[i++] = nc;
	for (uint8_t k = 0; k < nc; k++) {
		msg[i++] = (uint8_t)(contextId[k]) & 0x7f;
	}
	// Chroma complete-sync: extensible voicing-params block after the contextId (old hosts ignore the
	// tail). Format: [count][p0][p1]... positional: 0=spread, 1=inversion. Add more by bumping count.
	msg[i++] = 4; // extras count. positional: 0=spread, 1=inversion, 2=scale/mode index, 3=spelling lean
	msg[i++] = (uint8_t)spread & 0x7f;
	msg[i++] = (uint8_t)inversion & 0x7f;
	msg[i++] = scale & 0x7f;
	msg[i++] = (uint8_t)deluge::gui::ui::keyboard::gChromaSpelling & 0x7f; // 0=Auto,1=Flats,2=Sharps (synced spelling)
	msg[i++] = 0xf7;
	lastHidCable->sendSysex(msg, i);
}

// Chroma: F0 00 21 7B 01 45 <n> <viewId ASCII...> F7.
// A tiny outbound push naming the on-screen view, so the Companion's Learn tab auto-follows what the user is doing.
// viewId is a short 7-bit ASCII slug ("clip","song","arranger","kit","fx-menu",...). No-op until a host handshakes.
void HIDSysex::sendActiveView(const char* viewId) {
	if (lastHidCable == nullptr || viewId == nullptr) {
		return;
	}
	uint8_t msg[8 + 32];
	msg[0] = 0xf0;
	msg[1] = 0x00;
	msg[2] = 0x21;
	msg[3] = 0x7b;
	msg[4] = 0x01;
	msg[5] = SysEx::SysexCommands::ActiveView; // 0x45
	uint8_t n = 0;
	for (const char* c = viewId; *c && n < 31; c++) {
		msg[7 + n++] = (uint8_t)(*c) & 0x7f; // slugs are ASCII by contract
	}
	msg[6] = n;
	msg[7 + n] = 0xf7;
	lastHidCable->sendSysex(msg, 8 + n);
}

// Map the on-screen UI to a stable, host-neutral view slug. The Companion owns the slug->hub mapping, so its
// hub taxonomy can change without a firmware update. Returns nullptr for views with no useful "learn this"
// target (context menus, transient/unknown) — the caller then leaves the host on whatever view it last showed.
static const char* viewSlug(UIType t) {
	switch (t) {
	case UIType::SESSION:
		return "session";
	case UIType::ARRANGER:
		return "arranger";
	case UIType::INSTRUMENT_CLIP:
		return "instrument_clip";
	case UIType::AUDIO_CLIP:
		return "audio_clip";
	case UIType::KEYBOARD_SCREEN:
		return "keyboard";
	case UIType::AUTOMATION:
		return "automation";
	case UIType::PERFORMANCE_SESSION:
		return "performance";
	case UIType::SOUND_EDITOR:
		return "sound_editor";
	case UIType::BROWSER:
		return "browser";
	case UIType::SAMPLE_MARKER_EDITOR:
		return "sample_marker";
	case UIType::SLICER:
		return "slicer";
	case UIType::AUDIO_RECORDER:
		return "audio_recorder";
	case UIType::LOAD_SONG:
		return "load_song";
	case UIType::LOAD_INSTRUMENT_PRESET:
		return "load_preset";
	case UIType::RENAME_DRUM:
	case UIType::RENAME_OUTPUT:
	case UIType::RENAME_CLIPNAME:
		return "rename";
	default:
		return nullptr; // CONTEXT_MENU, TIMELINE, NONE — keep the host on its last view
	}
}

// Chroma: broadcast the active view when it changes, so the Companion's Learn tab auto-follows what's on screen.
// Called from the display passenger poll (sendDisplayIfChanged), which already runs on every redraw — and a view
// change always redraws. Cheap: a getUIType() read + int compare per call, de-duped so 0x45 only goes out on an
// actual change. A view with no hub (nullptr slug) is skipped WITHOUT updating the de-dupe, so returning to the
// underlying view doesn't re-fire.
void HIDSysex::sendActiveViewIfChanged() {
	UI* ui = getCurrentUI();
	UIType t = ui ? ui->getUIType() : UIType::NONE;
	if (t == lastSentView) {
		return;
	}
	const char* slug = viewSlug(t);
	if (slug == nullptr) {
		return;
	}
	lastSentView = t;
	fxContextSent = false; // a real view change re-arms the fx context, so the next knob turn pushes "fx" again
	sendActiveView(slug);
}

// Chroma: a gold-knob (FX) turn isn't a view change, so the view poll never fires for it. Push an "fx" context so a
// following host (the Companion's Learn tab) jumps to FX help. Sent WITHOUT touching lastSentView, so the view poll
// stays de-duped and doesn't snap back; fired once per fx-session, re-armed when the real view next changes.
void HIDSysex::sendFxContext() {
	if (!fxContextSent) {
		sendActiveView("fx");
		fxContextSent = true;
	}
}

// Chroma WRITE direction (0x44): a one-slot inbox for an inbound voicing-mod. The MIDI-receive context stashes it;
// the Harmonic layout drains it on the UI/graphics thread (KeyboardLayoutHarmonic::renderPads), where re-voicing
// and the 0x43 re-broadcast already run safely. Last-write-wins; a single bool flag, no lock needed — both producer
// and consumer run in the Deluge's cooperative main loop. Worst case is a dropped/late mod, never a crash. This path
// deliberately never touches audio; the mod is HEARD next time the chord is played (buildVoicing reads the new state).
static volatile bool gChordModPending = false;
static uint8_t gChordModType = 0;
static uint8_t gChordModValue = 0;

void HIDSysex::receiveChordApply(uint8_t modType, uint8_t value) {
	gChordModType = modType & 0x7f;
	gChordModValue = value & 0x7f;
	gChordModPending = true; // set fields before the flag so the consumer never reads a half-written mod
}

bool HIDSysex::hasChordMod() {
	return gChordModPending;
}

bool HIDSysex::takeChordMod(uint8_t* modType, uint8_t* value) {
	if (!gChordModPending) {
		return false;
	}
	if (modType != nullptr) {
		*modType = gChordModType;
	}
	if (value != nullptr) {
		*value = gChordModValue;
	}
	gChordModPending = false;
	return true;
}

// Chroma: F0 00 21 7B 01 02 50 <packed 8x18 RGB, 8-bit->7-bit> F7. Mirrors the live pad-LED grid the user
// sees — exact colours, and it reflects ANY control change because the host just renders the lit pads.
// Reads PadLEDs::image (the buffer that drives the physical LEDs, always valid, incl. on 7-seg units).
void HIDSysex::sendPadGrid(MIDIDevice* device) {
	const int32_t W = kDisplayWidth + kSideBarWidth; // 18
	const int32_t H = kDisplayHeight;                // 8
	uint8_t raw[8 * 18 * 3];
	int32_t p = 0;
	for (int32_t y = 0; y < H; y++) {
		for (int32_t x = 0; x < W; x++) {
			RGB c = PadLEDs::image[y][x];
			raw[p++] = c.r;
			raw[p++] = c.g;
			raw[p++] = c.b;
		}
	}
	uint8_t reply_hdr[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x50};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	memcpy(reply, reply_hdr, 7);
	int32_t packed = pack_8bit_to_7bit(reply + 7, 600, raw, p);
	if (packed < 0) {
		return;
	}
	reply[7 + packed] = 0xf7;
	device->sendSysex(reply, packed + 8);
}
