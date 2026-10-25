/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
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

#include <cstdint>

namespace deluge::processing::engines {

/// An assignable auxiliary audio send bus on the two CV sockets.
///
/// The DAC feeding the CV sockets is a real 16-bit converter on a fast SPI bus, and its
/// chip-select sits on P6_1 -- the SPI block's own SSL00 pin, already configured as such at
/// boot. That lets the transfer engine feed it continuously with no processor involvement,
/// so the sockets can carry audio rather than only the occasional note voltage.

/// Whether the sockets can carry audio on this hardware.
///
/// True on every model. It was false on OLED until the bus arbitration below existed: the
/// DAC and the display sit on the same RSPI channel -- SPI_CHANNEL_CV and
/// SPI_CHANNEL_OLED_MAIN are both 0 -- and boot hands the DAC's chip-select to software
/// there (deluge.cpp) precisely so the two can take turns. A free-running stream owns that
/// bus outright, which is why it had to be kept off it entirely. It now gives the bus back
/// on demand; see cvStreamYieldBusToDisplay().
///
/// Kept as a function rather than deleted. It is the one place the processing path asks
/// this question, the capture path still reads it, and a model that genuinely could not
/// stream would answer here.
bool cvOutputsAvailable();

/// Records the SPI block as boot configured it for the DAC.
///
/// Call once, after R_RSPI_Create/R_RSPI_Start on SPI_CHANNEL_CV and before anything else
/// touches the channel -- on an OLED model setupOLED() reconfigures it for 8-bit display
/// writes within a few lines, and those are not the values to hand back to.
///
/// Needed because the display's own setup only writes the registers it cares about. The
/// clock divider is not one of them: the stream runs the bus at its own rate, and without
/// this the display would inherit whatever the stream last set.
void cvStreamRecordBootSpiConfig();

/// Hands the shared SPI bus to the display.
///
/// Only OLED models have anything to hand it to, and only there does this do any work.
/// Stops the transfer engine, remembers where in the ring it stopped, puts the SPI block
/// back the way boot left it, and parks the DAC's chip-select high as a GPIO so display
/// data on the shared bus is not also clocked into the converter.
///
/// Idempotent, and a no-op while the stream is not running. Reached from interrupt
/// context, so every wait on the hardware here is bounded rather than open.
///
/// The sockets hold their last voltage for as long as the display keeps the bus. That is
/// what this feature costs on OLED, and it is not constant: OLED::sendMainImage() sends
/// nothing while nothing on screen has changed, so a still screen costs nothing at all and
/// a redraw costs one gap.
void cvStreamYieldBusToDisplay();

/// Takes the bus back and resumes from where the transfer engine stopped, rather than from
/// the top of the ring -- so the gap is exactly as long as the display needed, with no
/// audio replayed and none skipped past.
///
/// Marks a phase resync rather than letting the rate loop nurse the lead back. The loop
/// can only trim by +-0.3%, so a millisecond of missed time takes seconds to shed and a
/// busy screen would outrun it -- the lead would climb until the pump's own resync
/// threshold fired, turning many small gaps into one large discontinuity plus a delay that
/// grew the whole time. Resyncing here instead drops the frames that would have played
/// during the gap: bounded, and it does not accumulate.
void cvStreamTakeBusBack();

/// Whether the AUX menus appear.
///
/// Deliberately not cvOutputsAvailable(). The sends are ordinary params: they are stored in
/// the song file by name, they automate, and they LEARN to a knob. Songs move between
/// machines, so tying the menus to whether the *local* sockets can stream makes those
/// params uneditable on a machine you might be authoring on, and leaves any automation
/// lane on them with no visible source.
///
/// These two were one predicate while the feature was 7-seg-only, which is why the whole
/// thing disappeared on OLED rather than merely falling silent.
bool cvSendMenusVisible();

/// True while the stream owns the shared bus: running, and not handed to the display. The
/// C-callable cvStreamHoldsBus() is a thin wrapper on this.
bool cvStreamHoldsBusInternal();

/// True while the CV sockets are being streamed to.
bool cvStreamIsRunning();

/// True while a playing Clip is routed with STEREO SPLIT on.
///
/// INTENDED BEHAVIOUR, not an oversight. STEREO SPLIT only has anything to split when
/// the engine is rendering in stereo, which it does only with headphones or the right
/// main output connected, or while internally recording. Rendering in mono is not just a
/// missing pan -- stereo samples have their channels combined and every voice costs about
/// half as much. So this must not be used to force stereo on: that would make a CV routing
/// choice change the main mix's level and its CPU cost, which is far worse than the split
/// quietly falling back to the mono sum on both sockets.
bool cvStreamNeedsStereo();

/// Hands one Clip's isolated audio to the sockets its sends name. A send above zero is what
/// "routed to that socket" means. `sourceId` identifies the Clip, so the correction filter
/// can be reset when a socket's set of sources changes.
///
/// The Clip is isolated here rather than by the caller: `post` is the interleaved mix after
/// the Clip rendered, `pre` the snapshot taken before it did, and the difference is taken
/// per sample inside the accumulate loop that was going to read them anyway.
void cvStreamCapture(uint32_t sourceId, const int32_t* post, const int32_t* pre, uint32_t numSamples,
                     const int32_t* sendGain, int32_t* lastSendGain);

/// Snapshot space for the mix as it stood before a Clip rendered.
int32_t* cvStreamCaptureScratch();

/// Converts everything captured this window and queues it for output.
void cvStreamPump(uint32_t numSamples);

/// Called once when the render loop has finished, so the full set of Clips feeding
/// each socket is known. Resets the correction filter where that set changed, and
/// starts or stops the stream to match -- nothing else has to.
void cvStreamRenderComplete();

/// Stereo split, GLOBAL rather than per Clip. It describes how the two cables are plugged
/// in -- one stereo destination, or two mono ones -- which is a property of the rig and not
/// of any clip. Per-Clip it permitted a state where one clip treated the pair as stereo
/// while another treated them as two mono outs, which is never what anyone meant.
bool cvGetStereoSplit();
void cvSetStereoSplit(bool on);

/// AUX MASTER, one per socket. Set once per render window from the song's two master params,
/// which is where the level now lives.
///
/// Displayed 0-50 like every other param on the machine, and 0 is a true mute rather than the
/// bottom of the taper: "quiet enough" is not silence, and a mute is what an off position is
/// for. 50 is x256; each step below it is a fixed 1.2 dB, so the steps sound evenly spaced the
/// whole way down.
void cvSetMasterFromParams(int32_t cv1Value, int32_t cv2Value);

/// Which socket the AUX MASTER gold knob is currently editing, 0 or 1. Only meaningful when
/// split is off; with split on there is one master and the knob always drives CV1's param.
/// Deliberately not saved anywhere -- it is where a knob is pointing, not a setting.
extern uint8_t cvMasterKnobSocket;

/// Menu display value, 0-50, for a raw master param value. The menu and the gold knob edit one
/// param between them, so this is only a rendering of it -- there is no second copy to drift.
int32_t cvMasterParamToDisplay(int32_t paramValue);
int32_t cvMasterDisplayToParam(int32_t display);
constexpr int32_t kCvMasterDisplayMax = 50;

/// Diagnostic counters for the shared-bus handover, all free-running since boot.
///
/// Here because the failure they exist for cannot be caught any other way: the display and
/// the converter share one SPI channel, the handover between them happens in interrupt
/// context, and when it goes wrong the screen is the first thing to stop -- so there is
/// nothing left to print to. These are read back through SETTINGS > AUX > STATS, which is
/// reachable whether or not the feature itself is switched on.
///
/// What each one answers:
///
///   Starts / Stops   -- is the stream being started and stopped once, or thrashing? A pair
///                       of large and near-equal numbers means the render path is changing
///                       its mind every window, which is a fault in its own right.
///   Yields/TakeBacks -- how often the display takes the bus. These should be close to each
///                       other; a Yields count that runs away from TakeBacks means the bus
///                       is being handed over and not handed back, which is the deadlock.
///   PumpWritten      -- windows of audio actually written into the ring.
///   PumpDropped      -- windows thrown away because the display had the bus. If this is
///                       comparable to or larger than PumpWritten, the sockets are starving:
///                       the audio is being discarded faster than it is delivered, and no
///                       amount of correct bus handling downstream will make a sound.
///   Resyncs          -- phase resyncs, one per resumed handover plus any the pump forces.
///   DmaStalls        -- pump calls where the transfer engine's read pointer had not moved
///                       since the previous one. A large number here means the converter is
///                       not being fed at all, whatever the rest of the counters say.
///   YieldedNow       -- 1 if the display holds the bus at this instant, 0 if the stream does.
enum class CvStat : uint8_t {
	Starts,
	Stops,
	Yields,
	TakeBacks,
	PumpWritten,
	PumpDropped,
	Resyncs,
	DmaStalls,
	YieldedNow,
	/// Frames the transfer engine actually consumed per pump window, x10 -- so 1360 means 136.0
	/// frames a window, which is what a correct 47 kHz stream against a 128-sample window looks
	/// like. This is the measurement the boolean DmaStalls could only hint at.
	AdvancePerWindowX10,
	/// Frames the pump emitted per window, x10, over the same windows. Read against
	/// AdvancePerWindowX10: writer and reader must agree, and the ratio between them is the rate
	/// error in one number.
	EmitPerWindowX10,
	/// Every RSPI status bit seen set since boot, OR-ed together. Bit 0 OVRF (overrun), bit 2
	/// MODF (mode fault), bit 5 SPTEF, bit 6 TEND, bit 7 SPRF. MODF is the one worth crossing the
	/// room for: a mode fault means something else drove the chip-select line while the block was
	/// master, and the block stops transmitting -- which is what a contested SSL pin looks like,
	/// and would stall the transfer engine exactly like this.
	SpiStatusBits,
	/// The lead, in frames, as the pump last measured it. Target is 768 and the resync threshold
	/// is 640 either side; a value parked at an extreme says the loop never converges.
	LeadNow,
	/// CHSTAT_n for the stream's own DMA channel, raw. Five theories about why this channel never
	/// advances have now been tested against its effects; this asks the channel directly.
	///
	///   bit 0 EN    enabled
	///   bit 1 RQST  a request is pending
	///   bit 2 TACT  actively transferring
	///   bit 3 SUS   suspended
	///   bit 4 ER    error
	///   bit 5 END   transfer ended
	///   bit 6 TC    transfer count met
	///   bit 8 DL    descriptor loaded
	///   bit 9 DW    descriptor write-back
	///   bit 10 DER  descriptor error
	///
	/// EN clear means it was never armed and dmaChannelStart is not doing its job. EN set with
	/// RQST and TACT clear means it is armed and waiting for a request that never comes, which
	/// is a routing problem. ER or DER set means it was asked and refused, which is the
	/// descriptor. Those are three different faults and nothing measured so far separates them.
	DmaChannelStatus,
	/// CHSTAT_n for the display's DMA channel, as a control. The display works, so whatever this
	/// reads is what a healthy channel on this bus looks like.
	OledChannelStatus,
	/// The RSPI status register as it reads right now, not OR-ed since boot. The sticky version
	/// answered 128 in both builds, but sticky means a bit set once during the display's own
	/// traffic minutes ago is indistinguishable from one set during the stream's ownership.
	SpiStatusLive,
	/// Pump samples that caught a transmit sequence completed, which is the chip-select having
	/// been negated. Zero means it is still held and the converter still never latches.
	TendSeen,
};

/// Reads one counter. Not synchronised: these are incremented from interrupt context and read
/// from the menu, so a value can be one behind. That is the right trade for a diagnostic --
/// locking the audio path to read a number would change the thing being measured.
uint32_t cvStreamStat(CvStat which);

/// Per-Clip send amount, Q16. Attenuation only, so this is the ceiling.
constexpr int32_t kCvSendGainUnity = 65536;

/// Convert a send param value into the Q16 gain the capture path wants. Squared, which is
/// how the Deluge treats every other volume-ish param -- a linear send feels dead until the
/// top of its travel.
int32_t cvSendParamToGain(int32_t paramValue);

} // namespace deluge::processing::engines
