#include "definitions_cxx.hpp"
#include "io/midi/midi_device_manager.h"

namespace HIDSysex {
void requestOLEDDisplay(MIDICable& cable, uint8_t* data, int32_t len);
void request7SegDisplay(MIDICable& cable, uint8_t* data, int32_t len);
void sysexReceived(MIDICable& cable, uint8_t* data, int32_t len);
void sendOLEDData(MIDICable& cable, bool rle);
void sendOLEDDataDelta(MIDICable& cable, bool force);
void send7SegData(MIDICable& cable);
// Chroma: reply with the real text behind the 7-seg segments as ASCII (so the host mirrors "BASS", not "BA55").
void send7SegText(MIDICable& cable);
void sendDisplayIfChanged();
void readBlock(MIDICable& cable);
// Chroma: emit a LEARN context event to the host (Companion renders the docs). Sent on the last cable that
// talked to us over HID SysEx; a no-op until a host has handshaked. See chroma-schema SCHEMA.md 2.1.
void sendLearnContext(uint8_t region, uint8_t x, uint8_t y, const char* contextId);
// Chroma: outbound chord-state push on every NORMAL palette pick — host renders the chord (key + voiced
// notes + ctx), immune to the note-name overwrite that defeats the text mirror. No-op until a host handshakes.
void sendChordState(uint8_t keyRoot, const int16_t* notes, uint8_t numNotes, const char* contextId, int8_t spread = 0,
                    int8_t inversion = 0, uint8_t scale = 0);
// Chroma: outbound active-view push (0x45). Sent when the on-screen view changes, so the Companion's Learn tab can
// auto-follow what the user is looking at. Payload is a short 7-bit-ASCII view slug (e.g. "clip", "song", "kit").
// No-op until a host handshakes; last-value de-duped by the caller so it only fires on an actual change.
void sendActiveView(const char* viewId);
// Chroma: poll the current UIType and, if it changed, broadcast the view slug (0x45). Cheap + de-duped; called
// from the display passenger poll so it catches every view change without hooking the UI navigation stack.
void sendActiveViewIfChanged();
// Chroma: push the "fx" context (0x45) when a gold knob is turned, so a following host jumps to FX. Throttled to
// once per fx-session (re-armed when the real view changes) and doesn't disturb the view de-dupe.
void sendFxContext();
// Chroma complete-sync WRITE direction: a host (CT/Companion) sends a voicing-mod (0x44); the Deluge applies it to
// its OWN held chord and re-broadcasts 0x43, so every surface stays in sync. The MIDI-receive context only stashes
// it in a one-slot inbox; the Harmonic layout drains it on the UI/graphics thread (where re-voicing + broadcast run
// safely). This receive path NEVER sounds notes — state + re-broadcast only. modType 0=spread, 1=inversion.
void receiveChordApply(uint8_t modType, uint8_t value);
bool hasChordMod();                                  // UI poll: is a voicing-mod waiting to be applied?
bool takeChordMod(uint8_t* modType, uint8_t* value); // UI drain: pop the pending mod (returns false if none)
// Chroma: mirror the actual pad-LED grid (8 rows x 18 cols RGB) the user sees, so the host shows EXACTLY
// the device surface and reflects every control change. Reads the live LED buffer; polled by the host.
void sendPadGrid(MIDICable& cable);
} // namespace HIDSysex
