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

#include "processing/engines/usb_audio_stream.h"
#include "definitions_cxx.hpp"
#include "dsp/stereo_sample.h"
#include "io/debug/print.h"
#include "model/usb_route.h"
#include "util/functions.h"
#include <cstdint>
#include <cstring>

extern "C" {
#include "OSLikeStuff/timers_interrupts/clock_type.h"
#include "OSLikeStuff/timers_interrupts/timers_interrupts.h"
#include "RZA1/cpu_specific.h"
#include "RZA1/intc/devdrv_intc.h"
#include "RZA1/mtu/mtu.h"
#include "RZA1/ostm/ostm.h"
#include "RZA1/system/iodefine.h"
#include "RZA1/system/iodefines/dmac_iodefine.h"
#include "RZA1/system/iodefines/usb20_iodefine.h"
#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"
#include "RZA1/usb/r_usb_basic/src/hw/inc/r_usb_bitdefine.h"
#include "RZA1/usb/r_usb_basic/src/hw/inc/r_usb_reg_access.h"
#include "definitions.h"
#include "deluge/drivers/usb/userdef/r_usb_paudio_config.h"
#include "drivers/dmac/dmac.h"

// Declared here rather than by including r_usb_extern.h, which only compiles as C.
extern uint16_t g_usb_peri_connected;
extern uint16_t g_usb_pstd_alt_num[];
usb_er_t usb_pstd_transfer_start(usb_utr_t* ptr);
void usb_pstd_forced_termination(uint16_t pipe, uint16_t status);
usb_regadr_t usb_hstd_get_usb_ip_adr(uint16_t ipno);
void invalidate_range_all_caches(uintptr_t start, uintptr_t end);

/// The driver's shadow of the CPU FIFO port selector. Changing the port behind the driver's back has to go through
/// hw_usb_rmw_fifosel, which updates both.
extern uint16_t fifoSels[];

volatile uint8_t usbReturnPipeUnderDma = 0;
}

namespace {

using deluge::processing::engines::USBAudioStream;

/* ---- Shared ------------------------------------------------------------------------------------------------- */

/// Must match the interface numbers in r_usb_pmidi_descriptor.c.
constexpr uint16_t kAudioInterfaceNumber = 2;
constexpr uint16_t kReturnInterfaceNumber = 3;
constexpr uint16_t kStreamingAltSetting = 1;

constexpr uint32_t kRate = 44100;
constexpr uint32_t kSubframeBytes = 2;

/// USB_DATA_STOP, r_usb_basic_define.h, which does not compile as C++.
constexpr uint16_t kUsbDataStop = 8u;

/// CHCTRL bits, as the vendor's r_usb_dmac.h names them.
constexpr uint32_t kDmaChctrlClearEnd = 0x00000020u;    ///< CLREND
constexpr uint32_t kDmaChctrlClearTc = 0x00000040u;     ///< CLRTC
constexpr uint32_t kDmaChctrlSwReset = 0x00000008u;     ///< SWRST
constexpr uint32_t kDmaChctrlSetEnable = 0x00000001u;   ///< SETEN
constexpr uint32_t kDmaChctrlClearEnable = 0x00000002u; ///< CLREN

/// One step of the 1.2 dB ladder the AUX send levels use, as a Q16 factor, and its reciprocal.
constexpr int64_t kLadderDown = 57139;
constexpr int64_t kLadderUp = 75167;

/* ---- Out: device to host ------------------------------------------------------------------------------------ */

constexpr uint32_t kChannels = USB_CFG_PAUDIO_CHANNELS;
constexpr uint32_t kFrameBytes = kChannels * kSubframeBytes;
constexpr uint32_t kStemChannels = USBAudioStream::kStemChannels;
static_assert(kChannels == kStemChannels, "every outgoing channel carries one routable track");

/// 44.1 kHz does not divide into 1 ms frames: 44 frames a packet, with the remainder carried forward.
constexpr uint32_t kFramesPerPacket = kSampleRate / 1000;
constexpr uint32_t kFrameRemainder = kSampleRate % 1000;

/// Every packet is a whole number of 32-byte blocks, the unit the DMA controller moves in. A block is a whole
/// number of frames, so a packet can never end in a partial frame - a host that receives one rotates its channel
/// mapping permanently.
constexpr uint32_t kDmaBlockBytes = 32u;
constexpr uint32_t kFramesPerBlock = kDmaBlockBytes / kFrameBytes;
static_assert(kDmaBlockBytes % kFrameBytes == 0, "a DMA block must be a whole number of audio frames");

/// The endpoint's declared ceiling, rounded down to whole blocks, so the builder never emits more than the host
/// reserved bandwidth for.
constexpr uint32_t kMaxFramesPerPacket = USB_CFG_PAUDIO_MAX_FRAMES / kFramesPerBlock * kFramesPerBlock;
constexpr uint32_t kMaxPacketBytes = kMaxFramesPerPacket * kFrameBytes;
static_assert(USB_CFG_PAUDIO_MAX_FRAMES * kFrameBytes <= 1023, "a Full Speed isochronous packet is 1023 bytes");
static_assert(kMaxFramesPerPacket > kFramesPerPacket, "a packet must be able to carry more than the nominal rate");
static_assert(kMaxPacketBytes <= USB_CFG_PAUDIO_BUF_BYTES, "a packet must fit the pipe buffer");

/// Between the audio engine and the packet builder. 186 ms, so a card load that stalls the builder is caught up
/// rather than discarded. Latency is set by kLeadFrames, not by this.
constexpr uint32_t kRingFrames = 8192u;
constexpr uint32_t kRingMask = kRingFrames - 1u;

/// How far ahead of the host the ring is held: two render windows, ~5.8 ms.
constexpr uint32_t kLeadFrames = 256u;

/// Inside this band every packet is the nominal size, so the host sees a steady rate. Outside it the packet grows
/// or shrinks by one block until the lead is back.
constexpr uint32_t kLeadBandFrames = 64u;

/// Past this the stream is recovering from a stall and grows by four blocks, draining the backlog in well under
/// a second without a step on the wire.
constexpr uint32_t kLeadCatchUpFrames = kLeadFrames;

/// Past this the writer is about to lap the reader, so the reader jumps back to the lead and takes one
/// discontinuity.
constexpr uint32_t kResyncFrames = kRingFrames - kLeadFrames - kMaxFramesPerPacket;

PLACE_SDRAM_BSS int16_t ring[kRingFrames * kChannels];

/// Free-running, masked only where they index the ring. writeFrame belongs to the audio engine and readFrame to the
/// packet builder; each has one writer and is a 32-bit aligned store, so no lock is needed.
volatile uint32_t writeFrame = 0;
volatile uint32_t readFrame = 0;
bool ringCleared = false;
bool primed = false;

/// A host has the streaming interface selected.
bool streamActive = false;

/// One render window per channel, accumulated while tracks render and read back by the output stage.
constexpr uint32_t kStemWindowSamples = SSI_TX_BUFFER_NUM_SAMPLES;
int32_t stemAccumulator[kStemChannels][kStemWindowSamples];
int32_t stemSnapshot[kStemWindowSamples * 2];
uint32_t stemWindowSamples = 0;

/// Channels written this window. The first track to name a channel assigns it, any after it add.
uint16_t channelsWritten = 0;

/// The fixed conversion from mix scale to the 16 bits on the cable, as a multiplier over 2^24. Ten 1.2 dB steps
/// below unity: a stem is taken before the master compressor and runs hotter than the mix, and this leaves about
/// 5 dB of headroom on a dense song.
constexpr int32_t computeStemTrimMultiplier(uint32_t stepsDown) {
	int64_t multiplier = 1 << 24;
	for (uint32_t step = 0; step < stepsDown; step++) {
		multiplier = (multiplier * kLadderDown) >> 16;
	}
	return (int32_t)multiplier;
}
constexpr int32_t kStemTrimMultiplier = computeStemTrimMultiplier(10);

/// What feedMix actually applies: kStemTrimMultiplier moved by the Out Level setting. The return keeps its unity
/// against kStemTrimMultiplier, so its level does not move with this. One aligned 32-bit store from the UI, read by the
/// audio routine, so no lock is needed.
uint32_t outLevelSetting = USBAudioStream::kOutLevelDefault;
int32_t computeStemOutMultiplier(uint32_t level) {
	if (level == 0) {
		return 0;
	}
	// Steps below the shipped level: unity's own offset, plus however far below unity this is (negative above it).
	const int32_t stepsDown = (int32_t)USBAudioStream::kOutLevelUnityStepsBelowShipped
	                          + (int32_t)USBAudioStream::kOutLevelUnity - (int32_t)level;
	int64_t multiplier = kStemTrimMultiplier;
	for (int32_t step = 0; step < stepsDown; step++) {
		multiplier = (multiplier * kLadderDown) >> 16;
	}
	for (int32_t step = 0; step < -stepsDown; step++) {
		multiplier = (multiplier * kLadderUp) >> 16;
	}
	return (int32_t)multiplier;
}
volatile int32_t stemOutMultiplier = computeStemOutMultiplier(USBAudioStream::kOutLevelDefault);

/// Staging buffers for the packets that go through the driver, which reads them after submit() returns.
alignas(4) uint8_t packets[2][kMaxPacketBytes] = {};
uint32_t packetSlot = 0;

usb_utr_t transfer;
bool transferInitialised = false;
bool transferInFlight = false;

/// The pipe has been opened through the driver and is answering the host at BUF. From then on the DMA controller,
/// on the write timer, is the only thing that feeds it; the driver is used again only if something shuts the pipe.
bool pipeOpen = false;
uint32_t frameAccumulator = 0;

/// Packets the task has built and the write timer hands to the DMA controller. Single producer, single consumer.
///
/// Written through the uncached mirror and read by the controller from the ordinary address, so nothing may share
/// a cache line with a payload: the lengths live in their own array and each slot is padded to whole lines.
constexpr uint32_t kQueueSlots = 4u;
struct QueuedPacket {
	alignas(CACHE_LINE_SIZE) uint8_t data[(kMaxPacketBytes + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE * CACHE_LINE_SIZE];
};
QueuedPacket packetQueue[kQueueSlots];
uint16_t packetQueueBytes[kQueueSlots];
volatile uint32_t queueWrite = 0;
volatile uint32_t queueRead = 0;

uint8_t* uncachedSlot(uint32_t slot) {
	return (uint8_t*)((uint32_t)packetQueue[slot].data + UNCACHED_MIRROR_OFFSET);
}

/// A packet of silence, sent when the queue is empty because the audio task is frozen (a card load). An endpoint
/// that answers nothing makes the host's reader overtake and replay old audio; silence is what actually happened.
QueuedPacket standbyPacket;
uint16_t standbyBytes = 0;
bool standbyReady = false;
bool audioDmaStandby = false;

/// The write timer retries every 400 us, re-phased to the host's frame on each SOF. 2.5 attempts per frame gives
/// the same write rate as 10, for a quarter of the interrupt load.
constexpr int kAudioWriteTimer = 3; ///< The only unused MTU2 channel.
constexpr uint32_t kTimerTicksPerMs = DELUGE_CLOCKS_PER / 1000u;
constexpr uint32_t kWriteRetryTicks = (kTimerTicksPerMs * 2u) / 5u;
bool audioTimerRunning = false;

/// INTENB0.SOFE. Off in device mode until a stream starts.
constexpr uint16_t kIntEnbSofe = 0x2000u;
bool sofEnabled = false;

/// Transmit DMA: channel 0, D0FIFO's request line (r_usb_dma.c), 32-byte blocks into the FIFO's block registers.
/// D0FIFO is a port of its own, so the audio pipe never shares the CPU port with MIDI.
constexpr uint32_t kAudioDmaChannel = 0;
constexpr uint32_t kAudioDmaRequest = 0x00000083u;
constexpr uint32_t kDmaChcfgAmBusCycle = 0x00000200u; ///< AM = bus cycle mode
constexpr uint32_t kDmaChcfgLevel = 0x00000040u;      ///< LVL
constexpr uint32_t kDmaChcfgHighEnable = 0x00000020u; ///< HIEN
constexpr uint32_t kDmaChcfgSelCh0 = 0x00000000u;     ///< SEL = channel 0
constexpr uint32_t kDmaChcfgDest256 = 0x00050000u;    ///< DDS: 32 bytes a bus cycle
constexpr uint32_t kDmaChcfgDestFixed = 0x00200000u;  ///< DAD: destination does not advance
constexpr uint32_t kDmaChcfgSrc256 = 0x00005000u;     ///< SDS: 32 bytes a bus cycle
constexpr uint32_t kDmaChcfgReqDest = 0x00000008u;    ///< REQD: the requester is the destination
constexpr uint32_t kAudioDmaConfig = kDmaChcfgAmBusCycle | kDmaChcfgLevel | kDmaChcfgHighEnable | kDmaChcfgSelCh0
                                     | kDmaChcfgDest256 | kDmaChcfgDestFixed | kDmaChcfgSrc256 | kDmaChcfgReqDest;
constexpr uint16_t kFifoAccess32Byte = 0x2000u; ///< D0FBCFG.DFACC, 32-byte continuous access
constexpr uint32_t kDmaBusy = 0x05u;            ///< CHSTAT EN | TACT

bool audioDmaConfigured = false;
bool audioDmaInterruptArmed = false;
volatile bool audioDmaBusy = false;

/// Passes on which the transmit pipe has looked idle while a transfer is still outstanding.
uint32_t stuckPasses = 0;

/* ---- In: host to device ------------------------------------------------------------------------------------- */

constexpr uint32_t kRxChannels = USB_CFG_PAUDIO_RX_CHANNELS;
constexpr uint32_t kRxPairs = kRxChannels / 2u;
constexpr uint32_t kRxFrameBytes = kRxChannels * kSubframeBytes;
constexpr uint32_t kRxMaxPacketBytes = USB_CFG_PAUDIO_RX_PACKET_BYTES;

/// The receive DMA moves four bytes at a time. A frame that is not a multiple of that strands the tail of every
/// full-size packet, which is why one channel cannot work at 16 bits.
static_assert(kRxFrameBytes % 4u == 0u, "the return must carry an even number of channels");
static_assert(kRxPairs >= 1u, "the return carries at least one pair");
static_assert(kRxMaxPacketBytes / kRxFrameBytes >= (kSampleRate + 999u) / 1000u, "a 45-frame packet must fit");
static_assert(kRxMaxPacketBytes <= USB_CFG_PAUDIO_RX_BUF_BYTES, "a return packet must fit the pipe buffer");

/// A Full Speed frame has a fixed allowance for periodic traffic in both directions, and the host reserves against
/// what each endpoint declares. 8 out with 4 in opens on macOS and 8 out with 6 in is refused before any data
/// moves, so the declared total is held to the largest pair measured to open.
constexpr uint32_t kDeclaredOut = USB_CFG_PAUDIO_MAX_FRAMES * kFrameBytes;
constexpr uint32_t kDeclaredIn = kRxMaxPacketBytes;
static_assert(kDeclaredOut + kDeclaredIn <= 46u * 16u + 45u * 8u,
              "wider than any configuration a host has been measured to accept");

/// Pipe buffer blocks, double buffering counted. The block number is valid from 4 to 127.
constexpr uint32_t kRxBufBlocks = (USB_CFG_PAUDIO_RX_BUF_BYTES / 64u) * 2u;
static_assert(USB_CFG_PAUDIO_RX_BUF_BYTES % 64u == 0u, "pipe buffers are allocated in 64-byte blocks");
static_assert(USB_CFG_PAUDIO_RX_BUF_START >= 88u, "the return sits above USB MIDI's outgoing pipe at 72-87");
static_assert(USB_CFG_PAUDIO_RX_BUF_START + kRxBufBlocks - 1u <= 127u, "the return must fit the buffer memory");

/// 186 ms of returned audio.
constexpr uint32_t kRxRingFrames = 8192u;
constexpr uint32_t kRxRingMask = kRxRingFrames - 1u;

/// How far behind the arriving audio the reader sits: 512 frames, ~11.6 ms. This is the return's latency and its
/// tolerance for a late engine or host. Swept from 2048 down to 256 through repeated card loads without a
/// failure; 512 keeps more than two render windows above the floor.
constexpr uint32_t kRxLeadFrames = 512u;

/// Below this the cushion is gone, and the reader stops and rebuilds it rather than reading a few frames short every
/// window indefinitely.
constexpr uint32_t kRxRePrimeFloor = kRxLeadFrames / 4u;

/// Above this the reader takes one extra frame a window. There is no feedback endpoint at Full Speed, so the
/// ordinary clock difference between host and device - about one frame a second - is dropped here rather than
/// filling the ring until it buzzes.
constexpr uint32_t kRxDriftCeiling = kRxLeadFrames + kRxLeadFrames / 8u;

PLACE_SDRAM_BSS int16_t rxRing[kRxRingFrames * kRxChannels];

/// Free-running like the outgoing pair. rxWriteFrame belongs to the drain, rxReadFrame to the output stage.
volatile uint32_t rxWriteFrame = 0;
volatile uint32_t rxReadFrame = 0;
bool rxRingCleared = false;
bool rxPrimed = false;

/// A host has the return interface selected.
bool returnActive = false;

/// The first transfer on the return pipe goes through the driver, because that is what configures the pipe. Once
/// it is armed the pipe is handed to the DMA controller and the driver's receive path is not used again.
alignas(4) uint8_t rxPackets[2][kRxMaxPacketBytes] = {};
uint32_t rxPacketSlot = 0;
usb_utr_t rxTransfer = {};
bool rxTransferInitialised = false;
bool rxTransferInFlight = false;

/// Receive DMA on channel 1, D1FIFO's request line (r_usb_dma.c). The driver NAKs an isochronous OUT pipe when a
/// packet lands and only an interrupt re-opens it - and the audio engine masks interrupts for the whole of each
/// track's render, so under load packets were dropped by the hardware. With DCLRM set the module clears each plane
/// itself once the controller has read it (hardware manual p1563), so the pipe never fills and a deaf CPU costs
/// latency rather than audio.
constexpr uint32_t kRxDmaChannel = 1u;
constexpr uint32_t kRxDmaRequest = 0x0087u;

/// How long the controller can run before lapping the drain: 4096 frames, 93 ms.
constexpr uint32_t kRxDmaFrames = 4096u;
constexpr uint32_t kRxDmaBytes = kRxDmaFrames * kRxFrameBytes;
PLACE_SDRAM_BSS alignas(CACHE_LINE_SIZE) uint8_t rxDmaBuffer[kRxDmaBytes];

constexpr uint32_t kRxDmaLevel = (1u << 6);

/// A descriptor that links to itself, so the controller runs forever. The same shape as ssiDmaRxLinkDescriptor:
/// source fixed on the FIFO, destination walking the buffer, 32 bits a transfer.
const uint32_t rxDmaLinkDescriptor[] __attribute__((aligned(CACHE_LINE_SIZE))) = {
    0b1101,                                                                 // Header
    (uint32_t)&USB200.D1FIFO.UINT32,                                        // Source address
    (uint32_t)rxDmaBuffer,                                                  // Destination address
    sizeof(rxDmaBuffer),                                                    // Transaction size
    0b10000001000100100010001000100000 | kRxDmaLevel | (kRxDmaChannel & 7), // Config
    0,                                                                      // Interval
    0,                                                                      // Extension
    (uint32_t)rxDmaLinkDescriptor                                           // Next link: this one again
};

bool rxDmaRunning = false;
uint32_t rxDmaReadOffset = 0;

/// Level of the master return, 0-50 on the 1.2 dB ladder with unity at 40.
uint32_t returnLevelSetting = USBAudioStream::kReturnLevelDefault;

/// Which pair is summed into the song's mix, 1-based, or zero for none.
uint32_t masterReturnPair = 1;

/// Pairs a track has taken as its input this window, as (1 << (pair - 1)). Rebuilt every window from the tracks
/// that actually read, so a track that stops monitoring hands its pair back to the master.
uint32_t returnPairsClaimed = 0;

/// What the whole window reads, settled once before any track renders so every reader agrees.
bool returnWindowStreaming = false;
uint32_t returnWindowAvailable = 0;
int32_t returnWindowFadeStart = 0;

/// The frames the last output pass took off the ring, which is what a recorder of a USB input owes.
uint32_t returnConsumedFrom = 0;
uint32_t returnConsumedFrames = 0;
uint32_t returnConsumedRequested = 0;

/// Arriving sample to mix scale: the exact inverse of the outgoing conversion, so a device that hands back what it
/// was given is level-transparent at unity.
constexpr uint32_t kReturnShift = 1u + 16u - AUDIO_OUTPUT_GAIN_DOUBLINGS;
constexpr uint32_t kReturnMultiplierBits = 16u;
constexpr int32_t kReturnUnityMultiplier =
    (int32_t)(((uint64_t)1u << (24u + kReturnMultiplierBits)) / (uint64_t)(uint32_t)kStemTrimMultiplier);
int32_t returnMultiplierQ16 = 0;

/// Below this multiplier a full-scale sample cannot overflow, so the steady-state loop skips the saturation.
static_assert(31u + kReturnMultiplierBits > 15u + kReturnShift + 1u, "the saturation bound must fit an int32");
constexpr int32_t kReturnSaturationFreeMax = (int32_t)(1u << (31u + kReturnMultiplierBits - 15u - kReturnShift));
bool returnSaturationFree = false;

/// A 3 ms fade, so a host that stops sending, or a cushion that runs dry, does not click.
constexpr int32_t kReturnFadeFull = 1 << 16;
constexpr int32_t kReturnFadeStep = kReturnFadeFull / 128;
int32_t returnFade = 0;

/* ---- Compensation ------------------------------------------------------------------------------------------- */

/// Holds the Deluge's own audio back so a track sent out to an external processor and returned lands in time with
/// one that never left. 186 ms of line against a 100 ms ceiling.
constexpr uint32_t kCompensationFrames = 8192u;
constexpr uint32_t kCompensationMask = kCompensationFrames - 1u;
static_assert(USBAudioStream::kCompensationMaxMs * kSampleRate / 1000u < kCompensationFrames,
              "the delay line must hold the longest delay");
PLACE_SDRAM_BSS StereoSample compensationLine[kCompensationFrames];
PLACE_SDRAM_BSS int32_t compensationReverbLine[kCompensationFrames];
uint32_t compensationWrite = 0;
uint32_t compensationMs = 0;
uint32_t compensationFrames = 0;

/* ---- Return: receive ---------------------------------------------------------------------------------------- */

usb_regadr_t usbRegisters() {
	return usb_hstd_get_usb_ip_adr(USB_CFG_USE_USBIP);
}

volatile uint16_t* pipeControl(uint16_t pipe) {
	return &usbRegisters()->PIPE1CTR + (pipe - 1);
}

/// Copies one packet the driver delivered into the ring. Only ever reached for the transfer that configures the
/// pipe; everything after it arrives through the DMA drain.
void ingestReturnPacket(const uint8_t* data, uint32_t bytes) {
	const uint32_t frames = bytes / kRxFrameBytes;
	const uint32_t held = rxWriteFrame - rxReadFrame;
	const uint32_t room = held >= kRxRingFrames ? 0u : kRxRingFrames - held;
	const uint32_t toWrite = frames <= room ? frames : room;
	const int16_t* samples = (const int16_t*)data;
	const uint32_t w = rxWriteFrame;
	for (uint32_t f = 0; f < toWrite; f++) {
		int16_t* const dst = &rxRing[((w + f) & kRxRingMask) * kRxChannels];
		for (uint32_t c = 0; c < kRxChannels; c++) {
			dst[c] = samples[f * kRxChannels + c];
		}
	}
	rxWriteFrame = w + toWrite;
}

void armReturnTransfer();

/// The driver reports the remaining count in tranlen. Re-arms before decoding, so a second packet arriving while
/// this runs cannot land in the buffer being read.
void returnTransferComplete(usb_utr_t* ptr, uint16_t /*data1*/, uint16_t /*data2*/) {
	rxTransferInFlight = false;
	const uint32_t remaining = ptr != nullptr ? (uint32_t)ptr->tranlen : kRxMaxPacketBytes;
	const uint32_t received = remaining > kRxMaxPacketBytes ? 0u : kRxMaxPacketBytes - remaining;
	const uint32_t finished = rxPacketSlot;
	rxPacketSlot ^= 1u;
	if (returnActive && !rxDmaRunning) {
		armReturnTransfer();
	}
	ingestReturnPacket(rxPackets[finished], received);
}

/// Refuses once the controller owns the pipe: the driver's receive path ends by re-enabling the pipe's ready
/// interrupt, which would give the pipe a second consumer.
void armReturnTransfer() {
	if (rxDmaRunning) {
		return;
	}
	if (!rxTransferInitialised) {
		rxTransfer.complete = returnTransferComplete;
		rxTransfer.p_setup = nullptr;
		rxTransfer.segment = USB_TRAN_END;
		rxTransfer.ip = USB_CFG_USE_USBIP;
		rxTransfer.ipp = usbRegisters();
		rxTransferInitialised = true;
	}
	rxTransfer.keyword = USB_CFG_PAUDIO_ISO_OUT;
	rxTransfer.tranlen = kRxMaxPacketBytes;
	rxTransfer.p_tranadr = (void*)rxPackets[rxPacketSlot];
	rxTransferInFlight = (usb_pstd_transfer_start(&rxTransfer) == USB_OK);
}

void setReturnDmaRunning(bool running) {
	rxDmaRunning = running;
	usbReturnPipeUnderDma = running ? 1u : 0u;
}

void stopReturnDma() {
	DMACn(kRxDmaChannel).CHCTRL_n = kDmaChctrlClearEnable;
	usbRegisters()->D1FIFOSEL = USB_MBW_32;
	setReturnDmaRunning(false);
}

/// Hands the return pipe to the DMA controller, in the vendor driver's order: stop the pipe, take it off the CPU
/// port, point D1FIFO at it and confirm, then raise the request line and re-open the pipe.
///
/// All of it under one interrupt mask. The driver steers the shared port from its own interrupts, and an
/// interleaving here leaves USB MIDI unable to use the port while the audio carries on. Every wait is bounded.
bool startReturnDma() {
	if (rxDmaRunning) {
		return true;
	}
	usb_regadr_t reg = usbRegisters();
	DISABLE_ALL_INTERRUPTS();

	hw_usb_clear_brdyenb(USB_NULL, USB_CFG_PAUDIO_ISO_OUT);
	hw_usb_set_pid_nonzero_pipe_rohan(USB_CFG_PAUDIO_ISO_OUT, USB_PID_NAK);
	for (uint32_t settle = 0; (*pipeControl(USB_CFG_PAUDIO_ISO_OUT) & USB_PBUSY) != 0u && settle < 2000u; settle++) {}

	// A pipe may be selected by one port at a time. Through the driver's accessor, which keeps its shadow in step.
	if ((fifoSels[USB_CUSE] & USB_CURPIPE) == USB_CFG_PAUDIO_ISO_OUT) {
		hw_usb_rmw_fifosel(USB_NULL, USB_CUSE, 0u, USB_CURPIPE);
		for (uint32_t parked = 0; (reg->CFIFOSEL & USB_CURPIPE) != 0u && parked < 2000u; parked++) {}
	}

	const uint16_t want = (uint16_t)(USB_CFG_PAUDIO_ISO_OUT | USB_MBW_32);
	uint32_t guard = 0;
	do {
		reg->D1FIFOSEL = want;
		if (++guard > 2000u) {
			ENABLE_ALL_INTERRUPTS();
			return false;
		}
	} while ((reg->D1FIFOSEL & USB_CURPIPE) != USB_CFG_PAUDIO_ISO_OUT);

	reg->D1FIFOSEL = (uint16_t)(want | USB_DCLRM | USB_DREQE);

	rxDmaReadOffset = 0;
	initDMAWithLinkDescriptor((int32_t)kRxDmaChannel, rxDmaLinkDescriptor, kRxDmaRequest);
	dmaChannelStart(kRxDmaChannel);

	// Last, so the pipe accepts only once something is collecting from it.
	hw_usb_set_pid_nonzero_pipe_rohan(USB_CFG_PAUDIO_ISO_OUT, USB_PID_BUF);
	setReturnDmaRunning(true);
	ENABLE_ALL_INTERRUPTS();
	return true;
}

/// Moves whatever the controller has landed since last time into the ring. Not deadline-bound: the controller
/// keeps collecting whether or not this runs.
void drainReturnDma() {
	usb_regadr_t reg = usbRegisters();

	// If anything has re-enabled the pipe's ready interrupt or shut the pipe, the pipe and the controller no longer
	// agree on whose plane is next, and no single register write puts that right. Tear the handover down and let
	// serviceReturn build it again; the ring keeps its contents.
	const bool interruptBack = (reg->BRDYENB & (1u << USB_CFG_PAUDIO_ISO_OUT)) != 0u;
	const bool pipeShut = (*pipeControl(USB_CFG_PAUDIO_ISO_OUT) & USB_PID_BUF) != USB_PID_BUF;
	if (interruptBack || pipeShut) {
		stopReturnDma();
		hw_usb_clear_brdyenb(USB_NULL, USB_CFG_PAUDIO_ISO_OUT);
		rxTransferInFlight = false;
		return;
	}

	// Where the controller is writing, rounded down to a whole frame so a part-written one waits for next time.
	const uint32_t base = (uint32_t)rxDmaBuffer;
	uint32_t writeOffset = (DMACn(kRxDmaChannel).CRDA_n - base) % kRxDmaBytes;
	writeOffset -= writeOffset % kRxFrameBytes;
	const uint32_t available = (writeOffset - rxDmaReadOffset) % kRxDmaBytes;
	if (available == 0u) {
		return;
	}

	// Read cached after invalidating what the controller wrote - a quarter of the cost of the uncached mirror. Not
	// v7_dma_inv_range, which stops short of the outer cache on this part and returns stale frames.
	const uint32_t to = rxDmaReadOffset + available;
	if (to <= kRxDmaBytes) {
		invalidate_range_all_caches(base + rxDmaReadOffset, base + to);
	}
	else {
		invalidate_range_all_caches(base + rxDmaReadOffset, base + kRxDmaBytes);
		invalidate_range_all_caches(base, base + (to - kRxDmaBytes));
	}

	const uint32_t held = rxWriteFrame - rxReadFrame;
	const uint32_t room = held >= kRxRingFrames ? 0u : kRxRingFrames - held;
	uint32_t frames = available / kRxFrameBytes;
	if (frames > room) {
		frames = room;
	}
	uint32_t offset = rxDmaReadOffset;
	uint32_t w = rxWriteFrame;
	for (uint32_t f = 0; f < frames; f++) {
		const int16_t* const src = (const int16_t*)(base + offset);
		int16_t* const dst = &rxRing[(w & kRxRingMask) * kRxChannels];
		for (uint32_t c = 0; c < kRxChannels; c++) {
			dst[c] = src[c];
		}
		offset += kRxFrameBytes;
		if (offset >= kRxDmaBytes) {
			offset = 0;
		}
		w++;
	}
	rxWriteFrame = w;
	// Past everything the controller produced, including anything dropped for want of room, so the two never drift.
	rxDmaReadOffset = writeOffset;
}

/// Follows the host in and out of the return interface, independently of the outgoing one.
void serviceReturn() {
	if (!rxRingCleared) {
		memset(rxRing, 0, sizeof(rxRing));
		rxRingCleared = true;
	}

	if (!g_usb_peri_connected || g_usb_pstd_alt_num[kReturnInterfaceNumber] != kStreamingAltSetting) {
		returnActive = false;
		rxTransferInFlight = false;
		rxPrimed = false;
		rxReadFrame = rxWriteFrame;
		if (rxDmaRunning) {
			stopReturnDma();
		}
		return;
	}
	returnActive = true;

	if (!rxDmaRunning) {
		// The test belongs inside the mask: the completion interrupt re-arms on its own, and an arm landing between
		// the test and the call would re-point a live transfer.
		DISABLE_ALL_INTERRUPTS();
		if (!rxTransferInFlight) {
			armReturnTransfer();
		}
		ENABLE_ALL_INTERRUPTS();
		startReturnDma();
	}
	if (rxDmaRunning) {
		drainReturnDma();
	}
}

/* ---- Return: mix -------------------------------------------------------------------------------------------- */

void recomputeReturnMultiplier() {
	if (returnLevelSetting == 0) {
		returnMultiplierQ16 = 0;
		returnSaturationFree = true;
		return;
	}
	int64_t multiplier = kReturnUnityMultiplier;
	for (uint32_t step = returnLevelSetting; step < USBAudioStream::kReturnLevelUnity; step++) {
		multiplier = (multiplier * kLadderDown) >> 16;
	}
	for (uint32_t step = USBAudioStream::kReturnLevelUnity; step < returnLevelSetting; step++) {
		multiplier = (multiplier * kLadderUp) >> 16;
	}
	returnMultiplierQ16 = (int32_t)multiplier;
	returnSaturationFree = returnMultiplierQ16 < kReturnSaturationFreeMax;
}

[[gnu::always_inline]] inline int32_t returnToMixScaleFast(int16_t sample) {
	return (int32_t)(((int64_t)((int32_t)sample << kReturnShift) * returnMultiplierQ16) >> kReturnMultiplierBits);
}

[[gnu::always_inline]] inline int32_t returnToMixScale(int16_t sample) {
	const int64_t wide = ((int64_t)((int32_t)sample << kReturnShift) * returnMultiplierQ16) >> kReturnMultiplierBits;
	if (wide > INT32_MAX) {
		return INT32_MAX;
	}
	if (wide < INT32_MIN) {
		return INT32_MIN;
	}
	return (int32_t)wide;
}

/// A track input is scaled as a line input is - left-justified, then the track's own amplitude - so a returned
/// pair and a jack at the same level sound the same.
[[gnu::always_inline]] inline int32_t returnToTrackScale(int16_t sample) {
	return (int32_t)sample << 16;
}

/// Settles what this render window reads from the return, before any of it is read.
///
/// Takes what is there rather than insisting on a whole window: a short window costs the missing frames, not a
/// fade down and back up.
void beginReturnWindow(uint32_t numSamples) {
	returnPairsClaimed = 0;

	if (!returnActive) {
		rxReadFrame = rxWriteFrame;
		rxPrimed = false;
	}
	else if (!rxPrimed && (rxWriteFrame - rxReadFrame) >= kRxLeadFrames + numSamples) {
		rxPrimed = true;
	}

	const uint32_t held = rxWriteFrame - rxReadFrame;
	if (rxPrimed && held < kRxRePrimeFloor) {
		rxPrimed = false;
	}

	returnWindowStreaming = returnActive && rxPrimed;
	returnWindowAvailable = returnWindowStreaming ? (held < numSamples ? held : numSamples) : 0u;

	// Advanced once here, so a window with two readers does not run the fade twice.
	returnWindowFadeStart = returnFade;
	const int32_t travel = (int32_t)numSamples * kReturnFadeStep;
	returnFade = returnWindowStreaming ? (returnFade + travel) : (returnFade - travel);
	if (returnFade > kReturnFadeFull) {
		returnFade = kReturnFadeFull;
	}
	if (returnFade < 0) {
		returnFade = 0;
	}
}

void claimReturnPair(uint32_t pair) {
	if (pair >= 1u && pair <= kRxPairs) {
		returnPairsClaimed |= 1u << (pair - 1u);
	}
}

bool returnReadable() {
	return returnWindowStreaming || returnWindowFadeStart != 0;
}

/// Adds two arriving channels into a buffer as its left and right. Naming one channel twice gives mono on both
/// sides. With no amplitude it adds at the master's scale and at unity; with one it adds at track scale, ramping
/// across the window so a moving fader does not step.
void addReturnChannelsInto(uint32_t chL, uint32_t chR, StereoSample* buffer, uint32_t numSamples,
                           const int32_t* amplitudeStart, const int32_t* amplitudeEnd) {
	if (!returnReadable() || chL >= kRxChannels || chR >= kRxChannels) {
		return;
	}
	const uint32_t r = rxReadFrame;
	const bool unity = (amplitudeStart == nullptr);

	// The steady state: a full window, the fade up, no amplitude, no possible overflow. Walked in straight runs so
	// the wrap is tested per run rather than per frame. This runs inside the render, which has the hard deadline.
	if (unity && returnWindowStreaming && returnWindowFadeStart >= kReturnFadeFull
	    && returnWindowAvailable == numSamples && returnSaturationFree) {
		uint32_t done = 0;
		while (done < numSamples) {
			const uint32_t slot = (r + done) & kRxRingMask;
			uint32_t run = kRxRingFrames - slot;
			if (run > numSamples - done) {
				run = numSamples - done;
			}
			const int16_t* __restrict__ src = &rxRing[slot * kRxChannels];
			StereoSample* __restrict__ dst = &buffer[done];
			for (uint32_t i = 0; i < run; i++) {
				dst[i].l += returnToMixScaleFast(src[chL]);
				dst[i].r += returnToMixScaleFast(src[chR]);
				src += kRxChannels;
			}
			done += run;
		}
		return;
	}

	int32_t fade = returnWindowFadeStart;
	const int32_t fadeStep = returnWindowStreaming ? kReturnFadeStep : -kReturnFadeStep;
	int32_t amplitude = unity ? 0 : *amplitudeStart;
	const int32_t amplitudeStep =
	    unity ? 0 : (int32_t)((int64_t)(*amplitudeEnd - *amplitudeStart) / (int32_t)(numSamples ? numSamples : 1));

	for (uint32_t i = 0; i < numSamples; i++) {
		fade += fadeStep;
		if (fade > kReturnFadeFull) {
			fade = kReturnFadeFull;
		}
		if (fade < 0) {
			fade = 0;
		}
		amplitude += amplitudeStep;
		if (i >= returnWindowAvailable) {
			continue;
		}
		const int16_t* const frame = &rxRing[((r + i) & kRxRingMask) * kRxChannels];
		int32_t left;
		int32_t right;
		if (unity) {
			left = returnToMixScale(frame[chL]);
			right = returnToMixScale(frame[chR]);
		}
		else {
			left = multiply_32x32_rshift32(returnToTrackScale(frame[chL]), amplitude) << 2;
			right = multiply_32x32_rshift32(returnToTrackScale(frame[chR]), amplitude) << 2;
		}
		if (fade >= kReturnFadeFull) {
			buffer[i].l += left;
			buffer[i].r += right;
		}
		else {
			buffer[i].l += (int32_t)(((int64_t)left * fade) >> 16);
			buffer[i].r += (int32_t)(((int64_t)right * fade) >> 16);
		}
	}
}

/// Advances the return by what actually reached the codec, and records that batch for a recorder.
void advanceReturnBy(uint32_t numSamplesOutputted) {
	returnConsumedFrom = rxReadFrame;
	returnConsumedRequested = numSamplesOutputted;
	returnConsumedFrames = 0;

	if (!returnActive || !rxPrimed) {
		return;
	}
	const uint32_t held = rxWriteFrame - rxReadFrame;
	uint32_t take = numSamplesOutputted < held ? numSamplesOutputted : held;
	if (held > kRxDriftCeiling && take < held) {
		take++;
	}
	returnConsumedFrames = take;
	rxReadFrame += take;
}

/* ---- Out: packets ------------------------------------------------------------------------------------------- */

/// Builds one packet into buffer and returns its size in frames.
///
/// Packets stay at the nominal 44.1 frames while the lead is inside its band, and step a block at a time to bring
/// it back. The ring absorbs the engine's burstiness; the wire does not see it.
uint32_t buildNextPacket(uint8_t* buffer) {
	// Even sizes only: 44 frames, with a whole block added every twentieth packet to average 44.1.
	uint32_t frames = kFramesPerPacket / kFramesPerBlock * kFramesPerBlock;
	frameAccumulator += kFrameRemainder;
	if (frameAccumulator >= 1000u * kFramesPerBlock) {
		frames += kFramesPerBlock;
		frameAccumulator -= 1000u * kFramesPerBlock;
	}

	// Signed: a reader one frame past the writer is behind, not a full ring.
	const int32_t lead = (int32_t)(writeFrame - readFrame);
	uint32_t available = lead > 0 ? (uint32_t)lead : 0u;

	if (!primed) {
		if (available < kLeadFrames + kMaxFramesPerPacket) {
			memset(buffer, 0, frames * kFrameBytes);
			return frames;
		}
		primed = true;
		readFrame = writeFrame - kLeadFrames;
		available = kLeadFrames;
	}

	if (available >= kResyncFrames) {
		readFrame = writeFrame - kLeadFrames;
		available = kLeadFrames;
	}

	// Not enough for a full packet: silence rather than a runt, and the audio waits in the ring for the next one.
	if (available < frames) {
		memset(buffer, 0, frames * kFrameBytes);
		return frames;
	}

	const int32_t leadError = (int32_t)available - (int32_t)kLeadFrames;
	uint32_t send = frames;
	if (leadError > (int32_t)kLeadCatchUpFrames) {
		send = frames + 4u * kFramesPerBlock;
	}
	else if (leadError > (int32_t)kLeadBandFrames) {
		send = frames + kFramesPerBlock;
	}
	else if (leadError < -(int32_t)kLeadBandFrames) {
		send = (frames > kFramesPerBlock) ? frames - kFramesPerBlock : kFramesPerBlock;
	}
	if (send > available) {
		send = available;
	}
	if (send > kMaxFramesPerPacket) {
		send = kMaxFramesPerPacket;
	}
	// Down to a whole block, never up: up would read frames the engine has not written.
	send = send / kFramesPerBlock * kFramesPerBlock;
	if (send == 0u) {
		send = kFramesPerBlock;
	}

	const uint32_t start = readFrame & kRingMask;
	const uint32_t firstRun = (start + send > kRingFrames) ? (kRingFrames - start) : send;
	memcpy(buffer, &ring[start * kChannels], firstRun * kFrameBytes);
	if (firstRun < send) {
		memcpy(&buffer[firstRun * kFrameBytes], &ring[0], (send - firstRun) * kFrameBytes);
	}
	readFrame = readFrame + send;
	return send;
}

/// The task is the only builder; the completion only releases the transfer.
void transferComplete(usb_utr_t* /*ptr*/, uint16_t /*data1*/, uint16_t /*data2*/) {
	transferInFlight = false;
}

/// The packet that opens the pipe is the next one in the queue, not a new one built from the ring: the queue
/// already holds the next few milliseconds, so a packet built now would be audio from after them, and would reach
/// the host ahead of them. Runs with interrupts masked, after the write timer's transfer has been retired.
void submitNextPacket() {
	uint8_t* buffer = packets[packetSlot];
	packetSlot ^= 1u;
	uint32_t bytes;
	if (queueWrite != queueRead) {
		const uint32_t slot = queueRead % kQueueSlots;
		bytes = packetQueueBytes[slot];
		memcpy(buffer, uncachedSlot(slot), bytes);
		queueRead = queueRead + 1;
	}
	else {
		bytes = (kFramesPerPacket / kFramesPerBlock * kFramesPerBlock) * kFrameBytes;
		memset(buffer, 0, bytes);
	}
	transfer.keyword = USB_CFG_PAUDIO_ISO_IN;
	transfer.tranlen = bytes;
	transfer.p_tranadr = (void*)buffer;
	transferInFlight = (usb_pstd_transfer_start(&transfer) == USB_OK);
}

/// Ends a transfer the hardware has finished but never reported. An isochronous IN the device was not ready for
/// ends with a not-ready event, which the interrupt handler clears before anything downstream sees it, so the
/// transfer would stay outstanding for good. An empty, idle pipe on two consecutive passes while a transfer is
/// still held is that state; one reading could land in the microsecond before a healthy completion.
void endAbandonedTransfer() {
	if ((*pipeControl(USB_CFG_PAUDIO_ISO_IN) & (USB_INBUFM | USB_PBUSY)) != 0) {
		stuckPasses = 0;
		return;
	}
	if (++stuckPasses < 2) {
		return;
	}
	stuckPasses = 0;
	// Through the driver, which also releases its own record of the pipe.
	usb_pstd_forced_termination(USB_CFG_PAUDIO_ISO_IN, kUsbDataStop);
	transferInFlight = false;
}

void prepareStandbyPacket() {
	const uint32_t frames = kFramesPerPacket / kFramesPerBlock * kFramesPerBlock;
	memset((uint8_t*)((uint32_t)standbyPacket.data + UNCACHED_MIRROR_OFFSET), 0, frames * kFrameBytes);
	standbyBytes = (uint16_t)(frames * kFrameBytes);
	standbyReady = true;
}

/// Points D0FIFO at the audio pipe for the life of the stream. The request line goes up only after the select has
/// taken, as the vendor driver does it.
void configureAudioDmaFifo() {
	usb_regadr_t reg = usbRegisters();
	const uint16_t want = (uint16_t)(USB_CFG_PAUDIO_ISO_IN | USB_MBW_32);
	do {
		reg->D0FIFOSEL = want;
	} while ((reg->D0FIFOSEL & USB_CURPIPE) != USB_CFG_PAUDIO_ISO_IN);
	reg->D0FBCFG = kFifoAccess32Byte;
	reg->D0FIFOSEL = (uint16_t)(want | USB_DREQE);
	audioDmaConfigured = true;
}

/// Commits the packet the controller has finished moving. A transmit DMA leaves the plane loaded but unsent, so
/// software sets BVAL to release it.
void retireAudioDma() {
	if (!audioDmaBusy) {
		return;
	}
	DMACn(kAudioDmaChannel).CHCTRL_n = kDmaChctrlClearEnd | kDmaChctrlClearTc;
	usbRegisters()->D0FIFOCTR |= USB_BVAL;
	if (audioDmaStandby) {
		audioDmaStandby = false;
	}
	else {
		queueRead = queueRead + 1;
	}
	audioDmaBusy = false;
}

extern "C" void audioDmaTransferComplete(uint32_t /*intSense*/) {
	retireAudioDma();
}

/// Releases D0FIFO so the driver can write the same pipe through the CPU port; one pipe may not be selected on two
/// ports. Runs with interrupts masked, so a transfer that will not finish within 10 us is given up on rather than
/// waited for.
bool parkAudioDmaPort() {
	const uint32_t deadline = Debug::readCycleCounter() + 4000u;
	while ((DMACn(kAudioDmaChannel).CHSTAT_n & kDmaBusy) != 0u) {
		if ((int32_t)(Debug::readCycleCounter() - deadline) > 0) {
			return false;
		}
	}
	retireAudioDma();
	usbRegisters()->D0FIFOSEL = USB_MBW_32;
	audioDmaConfigured = false;
	return true;
}

bool startAudioDma(const uint8_t* source, uint16_t bytes) {
	if ((DMACn(kAudioDmaChannel).CHSTAT_n & kDmaBusy) != 0u) {
		return false;
	}
	// The ordinary address: the builder wrote through the uncached mirror, so memory is already current.
	DMACn(kAudioDmaChannel).N0SA_n = (uint32_t)source;
	DMACn(kAudioDmaChannel).N0DA_n = (uint32_t)&(usbRegisters()->D0FIFOB0);
	DMACn(kAudioDmaChannel).N0TB_n = bytes;
	DMACn(kAudioDmaChannel).CHCFG_n = kAudioDmaConfig;
	DMACn(kAudioDmaChannel).CHITVL_n = 0u;
	setDMARS(kAudioDmaChannel, kAudioDmaRequest);
	DMACn(kAudioDmaChannel).CHCTRL_n = kDmaChctrlSwReset;
	audioDmaBusy = true;
	DMACn(kAudioDmaChannel).CHCTRL_n = kDmaChctrlSetEnable;
	return true;
}

/// Hands one pre-built packet to the DMA controller. Builds nothing: the copy out of the ring belongs to the task,
/// because at 1 kHz inside an interrupt it starves the audio engine.
void audioWriteTimerInterrupt(uint32_t /*intSense*/) {
	timerClearCompareMatchTGRA(kAudioWriteTimer);

	if (!streamActive || !transferInitialised) {
		return;
	}
	const bool queueEmpty = (queueWrite == queueRead);
	if (queueEmpty && !standbyReady) {
		return;
	}

	if (audioDmaBusy) {
		// A finished transfer whose interrupt has not run yet. Retiring it here means one missed interrupt cannot
		// stall the stream.
		if ((DMACn(kAudioDmaChannel).CHSTAT_n & kDmaBusy) != 0u) {
			return;
		}
		retireAudioDma();
	}

	if ((usbRegisters()->D0FIFOCTR & USB_FRDY) == 0u) {
		return;
	}

	if (queueEmpty) {
		audioDmaStandby = true;
		if (!startAudioDma(standbyPacket.data, standbyBytes)) {
			audioDmaStandby = false;
		}
		return;
	}

	// queueRead advances when the transfer retires, not here: the controller is still reading this slot.
	const uint32_t slot = queueRead % kQueueSlots;
	startAudioDma(packetQueue[slot].data, packetQueueBytes[slot]);
}

void buildQueuedPackets() {
	while ((uint32_t)(queueWrite - queueRead) < kQueueSlots) {
		const uint32_t slot = queueWrite % kQueueSlots;
		packetQueueBytes[slot] = (uint16_t)(buildNextPacket(uncachedSlot(slot)) * kFrameBytes);
		// The slot's contents must be visible before the index that publishes it.
		__asm volatile("DSB" ::: "memory");
		queueWrite = queueWrite + 1;
	}
}

void startAudioWriteTimer() {
	if (audioTimerRunning) {
		return;
	}
	setupTimerWithInterruptHandler(kAudioWriteTimer, 1, audioWriteTimerInterrupt, 5);
	*TGRA[kAudioWriteTimer] = (uint16_t)kTimerTicksPerMs;
	R_INTC_Enable(INTC_ID_TGIA[kAudioWriteTimer]);
	enableTimer(kAudioWriteTimer);
	audioTimerRunning = true;
}

void stopAudioWriteTimer() {
	if (!audioTimerRunning) {
		return;
	}
	disableTimer(kAudioWriteTimer);
	R_INTC_Disable(INTC_ID_TGIA[kAudioWriteTimer]);
	audioTimerRunning = false;
}

void stopStream() {
	transferInFlight = false;
	pipeOpen = false;
	frameAccumulator = 0;
	primed = false;
	streamActive = false;
	readFrame = writeFrame;
	queueRead = queueWrite;
	// Drop the request line with the channel, so nothing is left asking for a pipe nobody services.
	if (audioDmaConfigured) {
		DMACn(kAudioDmaChannel).CHCTRL_n = kDmaChctrlClearEnable;
		usbRegisters()->D0FIFOSEL = USB_MBW_32;
		audioDmaConfigured = false;
	}
	audioDmaBusy = false;
	stopAudioWriteTimer();
	if (sofEnabled) {
		usbRegisters()->INTENB0 &= (uint16_t)~kIntEnbSofe;
		sofEnabled = false;
	}
}

void serviceStream() {
	if (!ringCleared) {
		memset(ring, 0, sizeof(ring));
		ringCleared = true;
	}

	// The host parks the interface at alt 0 when it is not streaming.
	if (!g_usb_peri_connected || g_usb_pstd_alt_num[kAudioInterfaceNumber] != kStreamingAltSetting) {
		if (streamActive || audioDmaConfigured || audioTimerRunning) {
			stopStream();
		}
		readFrame = writeFrame;
		return;
	}
	streamActive = true;

	// Every pass, in every state: a queue left unfilled while nothing is in flight starves the timer for good.
	buildQueuedPackets();

	// Once open, the pipe is left to the write timer. Anything that shuts it - a SetInterface that re-initialises
	// the pipe, a forced termination, a stall - shows as a PID other than BUF, and the pipe is opened again below.
	if (pipeOpen) {
		if ((*pipeControl(USB_CFG_PAUDIO_ISO_IN) & USB_PID) == USB_PID_BUF) {
			return;
		}
		pipeOpen = false;
		transferInFlight = false;
	}

	if (transferInFlight) {
		endAbandonedTransfer();
		return;
	}

	if (!transferInitialised) {
		transfer.complete = transferComplete;
		transfer.p_setup = nullptr;
		transfer.segment = USB_TRAN_END;
		transfer.ip = USB_CFG_USE_USBIP;
		transfer.ipp = usbRegisters();
		transferInitialised = true;
	}

	// The pipe only stays armed while the driver holds a transfer, so it is re-armed through the driver whenever
	// none is in flight. The DMA port is handed back for the write, and the write is skipped rather than made if
	// the port cannot be freed in time.
	DISABLE_ALL_INTERRUPTS();
	if (audioDmaConfigured && !parkAudioDmaPort()) {
		ENABLE_ALL_INTERRUPTS();
		return;
	}
	submitNextPacket();
	pipeOpen = transferInFlight;
	ENABLE_ALL_INTERRUPTS();

	if (!audioDmaConfigured) {
		if (!standbyReady) {
			prepareStandbyPacket();
		}
		configureAudioDmaFifo();
		if (!audioDmaInterruptArmed) {
			setupAndEnableInterrupt(audioDmaTransferComplete, INTC_ID_DMAINT0 + kAudioDmaChannel, 5);
			audioDmaInterruptArmed = true;
		}
	}
	startAudioWriteTimer();
	if (!sofEnabled) {
		usbRegisters()->INTENB0 |= kIntEnbSofe;
		sofEnabled = true;
	}
}

/* ---- Stems -------------------------------------------------------------------------------------------------- */

// Built at -O3: at -O2 these half-vectorise and store one sample at a time from the vector unit, which stalls this
// core. Four walks rather than one with a branch, which would put the loop back on the main processor.

[[gnu::optimize("O3")]] void walkMonoAssign(int32_t* __restrict out, const int32_t* __restrict mixNow,
                                            const int32_t* __restrict snapshot, uint32_t numSamples) {
	for (uint32_t i = 0; i < numSamples; i++) {
		// Halved to mono, so a centred track keeps its level.
		const int32_t l = mixNow[i * 2] - snapshot[i * 2];
		const int32_t r = mixNow[i * 2 + 1] - snapshot[i * 2 + 1];
		out[i] = (l >> 1) + (r >> 1);
	}
}

[[gnu::optimize("O3")]] void walkMonoAdd(int32_t* __restrict out, const int32_t* __restrict mixNow,
                                         const int32_t* __restrict snapshot, uint32_t numSamples) {
	for (uint32_t i = 0; i < numSamples; i++) {
		const int32_t l = mixNow[i * 2] - snapshot[i * 2];
		const int32_t r = mixNow[i * 2 + 1] - snapshot[i * 2 + 1];
		out[i] += (l >> 1) + (r >> 1);
	}
}

/// side is 0 for left, 1 for right.
[[gnu::optimize("O3")]] void walkSideAssign(int32_t* __restrict out, const int32_t* __restrict mixNow,
                                            const int32_t* __restrict snapshot, uint32_t numSamples, uint32_t side) {
	for (uint32_t i = 0; i < numSamples; i++) {
		out[i] = mixNow[i * 2 + side] - snapshot[i * 2 + side];
	}
}

[[gnu::optimize("O3")]] void walkSideAdd(int32_t* __restrict out, const int32_t* __restrict mixNow,
                                         const int32_t* __restrict snapshot, uint32_t numSamples, uint32_t side) {
	for (uint32_t i = 0; i < numSamples; i++) {
		out[i] += mixNow[i * 2 + side] - snapshot[i * 2 + side];
	}
}

void captureInto(uint32_t channel, const int32_t* mixNow, uint32_t numSamples, int32_t side) {
	int32_t* const out = stemAccumulator[channel];
	const uint16_t bit = (uint16_t)(1u << channel);
	const bool first = (channelsWritten & bit) == 0;
	channelsWritten |= bit;
	if (side < 0) {
		first ? walkMonoAssign(out, mixNow, stemSnapshot, numSamples)
		      : walkMonoAdd(out, mixNow, stemSnapshot, numSamples);
	}
	else {
		first ? walkSideAssign(out, mixNow, stemSnapshot, numSamples, (uint32_t)side)
		      : walkSideAdd(out, mixNow, stemSnapshot, numSamples, (uint32_t)side);
	}
}

} // namespace

/* ---- Public ------------------------------------------------------------------------------------------------- */

namespace deluge::processing::engines {

void USBAudioStream::service() {
	// parkAudioDmaPort times out on the cycle counter, which a release build does not otherwise start.
	static bool cycleCounterStarted = false;
	if (!cycleCounterStarted) {
		Debug::init();
		cycleCounterStarted = true;
	}
	serviceReturn();
	serviceStream();
}

bool USBAudioStream::stemsWanted() {
	return streamActive;
}

void USBAudioStream::beginRender(uint32_t numSamples) {
	beginReturnWindow(numSamples);
	// Set whether or not a host is listening: taking a track out of the main mix must not depend on a cable.
	stemWindowSamples = (numSamples > kStemWindowSamples) ? kStemWindowSamples : numSamples;
	channelsWritten = 0;
	if (!streamActive) {
		return;
	}
	for (uint32_t c = 0; c < kStemChannels; c++) {
		memset(stemAccumulator[c], 0, stemWindowSamples * sizeof(int32_t));
	}
}

void USBAudioStream::snapshotBeforeTrack(const int32_t* mixNow, uint32_t numSamples) {
	if (numSamples > stemWindowSamples) {
		return;
	}
	memcpy(stemSnapshot, mixNow, numSamples * 2 * sizeof(int32_t));
}

void USBAudioStream::captureStem(uint16_t route, const int32_t* mixNow, uint32_t numSamples) {
	if (numSamples > stemWindowSamples || (route & UsbRoute::ANY_USB) == 0) {
		return;
	}
	for (uint32_t c = 0; c < kStemChannels; c++) {
		if ((route & (UsbRoute::FIRST_MONO << c)) != 0) {
			captureInto(c, mixNow, numSamples, -1);
		}
	}
	for (uint32_t pair = 0; pair < kStemChannels / 2; pair++) {
		if ((route & (UsbRoute::PAIR12 << pair)) != 0) {
			captureInto(pair * 2, mixNow, numSamples, 0);
			captureInto(pair * 2 + 1, mixNow, numSamples, 1);
		}
	}
}

void USBAudioStream::removeTrackFromMix(int32_t* mixNow, uint32_t numSamples) {
	if (numSamples > stemWindowSamples) {
		return;
	}
	// x - (x - y) = y in two's complement, so restoring the snapshot is exactly removing the track.
	memcpy(mixNow, stemSnapshot, numSamples * 2 * sizeof(int32_t));
}

void USBAudioStream::compensate(StereoSample* buffer, int32_t* reverbSend, uint32_t numSamples) {
	if (compensationFrames == 0 || buffer == nullptr || reverbSend == nullptr) {
		return;
	}
	const uint32_t delay = compensationFrames;
	uint32_t w = compensationWrite;
	for (uint32_t i = 0; i < numSamples; i++) {
		const uint32_t from = (w - delay) & kCompensationMask;
		const uint32_t to = w & kCompensationMask;
		const StereoSample in = buffer[i];
		buffer[i] = compensationLine[from];
		compensationLine[to] = in;
		const int32_t send = reverbSend[i];
		reverbSend[i] = compensationReverbLine[from];
		compensationReverbLine[to] = send;
		w++;
	}
	compensationWrite = w;
}

void USBAudioStream::setCompensationMs(uint32_t ms) {
	if (ms > kCompensationMaxMs) {
		ms = kCompensationMaxMs;
	}
	// The line is only written while compensation is on, so it holds stale audio when switched on.
	if (compensationFrames == 0 && ms != 0) {
		memset(compensationLine, 0, sizeof(compensationLine));
		memset(compensationReverbLine, 0, sizeof(compensationReverbLine));
	}
	compensationMs = ms;
	compensationFrames = ms * kSampleRate / 1000u;
}

uint32_t USBAudioStream::getCompensationMs() {
	return compensationMs;
}

void USBAudioStream::mixReturn(StereoSample* buffer, uint32_t numSamples) {
	if (buffer == nullptr || numSamples == 0 || masterReturnPair == 0) {
		return;
	}
	if ((returnPairsClaimed & (1u << (masterReturnPair - 1u))) != 0) {
		return;
	}
	const uint32_t chL = (masterReturnPair - 1u) * 2u;
	addReturnChannelsInto(chL, chL + 1u, buffer, numSamples, nullptr, nullptr);
}

void USBAudioStream::setReturnLevel(uint32_t level) {
	returnLevelSetting = level > kReturnLevelMax ? kReturnLevelMax : level;
	recomputeReturnMultiplier();
}

uint32_t USBAudioStream::getReturnLevel() {
	return returnLevelSetting;
}

void USBAudioStream::setOutLevel(uint32_t level) {
	outLevelSetting = level > kOutLevelMax ? kOutLevelMax : level;
	stemOutMultiplier = computeStemOutMultiplier(outLevelSetting);
}

uint32_t USBAudioStream::getOutLevel() {
	return outLevelSetting;
}

void USBAudioStream::setMasterReturnPair(uint32_t pair) {
	masterReturnPair = (pair <= kRxPairs) ? pair : 0u;
}

uint32_t USBAudioStream::getMasterReturnPair() {
	return masterReturnPair;
}

uint32_t USBAudioStream::numReturnPairs() {
	return kRxPairs;
}

uint32_t USBAudioStream::consumedReturnFrames() {
	return returnConsumedRequested;
}

uint32_t USBAudioStream::readConsumedReturn(uint32_t chL, uint32_t chR, StereoSample* dst, uint32_t offset,
                                            uint32_t maxSamples) {
	if (dst == nullptr || chL >= kRxChannels || chR >= kRxChannels || offset >= returnConsumedRequested) {
		return 0;
	}
	uint32_t count = returnConsumedRequested - offset;
	if (count > maxSamples) {
		count = maxSamples;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t at = offset + i;
		// Frames the return did not have are recorded as silence, so the file stays in time.
		if (at >= returnConsumedFrames) {
			dst[i].l = 0;
			dst[i].r = 0;
			continue;
		}
		const int16_t* const frame = &rxRing[((returnConsumedFrom + at) & kRxRingMask) * kRxChannels];
		dst[i].l = returnToTrackScale(frame[chL]);
		dst[i].r = returnToTrackScale(frame[chR]);
	}
	return count;
}

bool USBAudioStream::readReturnPair(uint32_t pair, StereoSample* buffer, uint32_t numSamples, int32_t amplitudeStart,
                                    int32_t amplitudeEnd) {
	if (buffer == nullptr || numSamples == 0 || pair == 0 || pair > kRxPairs) {
		return false;
	}
	claimReturnPair(pair);
	if (!returnReadable()) {
		return false;
	}
	const uint32_t chL = (pair - 1u) * 2u;
	addReturnChannelsInto(chL, chL + 1u, buffer, numSamples, &amplitudeStart, &amplitudeEnd);
	return true;
}

bool USBAudioStream::readReturnChannel(uint32_t channel, StereoSample* buffer, uint32_t numSamples,
                                       int32_t amplitudeStart, int32_t amplitudeEnd) {
	if (buffer == nullptr || numSamples == 0 || channel == 0 || channel > kRxChannels) {
		return false;
	}
	// A single channel takes its whole pair off the master, which can only sum pairs.
	claimReturnPair(((channel - 1u) / 2u) + 1u);
	if (!returnReadable()) {
		return false;
	}
	const uint32_t ch = channel - 1u;
	addReturnChannelsInto(ch, ch, buffer, numSamples, &amplitudeStart, &amplitudeEnd);
	return true;
}

void USBAudioStream::feedMix(uint32_t numSamples, uint32_t renderOffset) {
	advanceReturnBy(numSamples);

	if (!streamActive || numSamples == 0) {
		return;
	}

	uint32_t w = writeFrame;
	const int32_t outMultiplier = stemOutMultiplier;
	for (uint32_t i = 0; i < numSamples; i++) {
		int16_t* const frame = &ring[(w & kRingMask) * kChannels];
		const uint32_t stemIndex = renderOffset + i;
		if (stemIndex < stemWindowSamples) {
			for (uint32_t c = 0; c < kStemChannels; c++) {
				// Trimmed before saturating, so the saturation is a guard rather than the normal case.
				const int32_t trimmed = (int32_t)(((int64_t)stemAccumulator[c][stemIndex] * outMultiplier) >> 24);
				frame[c] = (int16_t)(lshiftAndSaturate<AUDIO_OUTPUT_GAIN_DOUBLINGS>(trimmed >> 1) >> 16);
			}
		}
		else {
			memset(frame, 0, kFrameBytes);
		}
		w++;
	}
	writeFrame = w;
}

} // namespace deluge::processing::engines

extern "C" {

void usbAudioStreamRoutine() {
	deluge::processing::engines::USBAudioStream::service();
}

/// From the peripheral interrupt's frame branch, on the host's 1 kHz clock. Re-phases the write timer to the host's
/// frame so its attempts stay spread across the frame rather than drifting into a beat against it.
void usbAudioStreamStartOfFrame(void) {
	if (!audioTimerRunning) {
		return;
	}
	*TCNT[kAudioWriteTimer] = 0;
	*TGRA[kAudioWriteTimer] = (uint16_t)kWriteRetryTicks;
}
}
