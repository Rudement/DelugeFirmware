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

#include "processing/engines/cv_audio_stream.h"
#include "OSLikeStuff/timers_interrupts/timers_interrupts.h"
#include "definitions_cxx.hpp"
#include "hid/display/display.h"
#include "processing/engines/audio_engine.h"
#include "model/settings/runtime_feature_settings.h"
#include "processing/engines/cv_audio_stream_c_interface.h"
#include "util/functions.h"
#include <cmath>
#include <cstring>

extern "C" {
#include "RZA1/cache/cache.h"
#include "RZA1/gpio/gpio.h"
#include "RZA1/system/iodefine.h"
#include "RZA1/system/iodefines/rspi_iodefine.h"
#include "drivers/dmac/dmac.h"
#include "drivers/oled/oled.h"
}

namespace deluge::processing::engines {

/// Inverse of the measured output filter, as a one-pole/one-zero shelf. The pole comes from
/// the socket's measured ~700 Hz corner.
constexpr float kCvEqPole = 0.9049f;

// ============================================================================
// CV audio outputs
// ----------------------------------------------------------------------------
// Streams two independent tracks out of the CV sockets while the main outputs
// carry the usual mix.
//
// The DAC's chip-select sits on P6_1, which is the SPI block's own SSL00 pin and
// is already configured as such at boot. That lets the transfer engine feed the
// DAC continuously with no processor involvement. Words alternate between the
// DAC's two channels, so one buffer serves both sockets.
//
// Each socket's analogue path is a single-pole low-pass with its corner near
// 700 Hz, so each channel gets the inverse applied before it leaves.
// ============================================================================

#define CV_STREAM_DMA_CHANNEL 5 // unassigned on every model

void cvStreamStart();
void cvStreamStop();

namespace {

/// Words in the streamed buffer; alternating, so half belong to each socket. Must hold the
/// target lead plus one whole burst -- the pump writes a window's worth of frames in one
/// go, and the engine's window doubles under load, so the burst is biggest exactly when the
/// ring is fullest. 4096 words (2048 frames/socket) gives a worst-case ~136-frame burst
/// about 6x the margin it needs. Costs 8 KB of SDRAM, no latency cost -- see kCvTargetLead.
constexpr uint32_t kCvStreamWords = 4096;
constexpr uint32_t kCvFramesPerChannel = kCvStreamWords / 2;
constexpr uint32_t kCvMaxWindow = 256;
/// Bytes per frame in the ring: one word per socket.
constexpr uint32_t kCvFrameBytes = 2 * sizeof(uint32_t);

/// How far ahead of the DMA read pointer the pump tries to stay, in frames. This is the CV
/// output's latency behind the main outputs, and how late the engine may run before the
/// buffer runs dry.
///
/// Must exceed the worst phase excursion the engine produces: idle jitter is ~57 frames,
/// loaded ~650. 768 gives headroom over that; 512 underruns under load.
///
/// 768 frames is ~16.4 ms -- fine for a send into a pedal, mixer channel or modular case,
/// but summing it back against the same source in the mains will comb-filter. Lower it only
/// if the resync count stays at zero on real songs; it is the one constant here with an
/// audible cost.
constexpr int32_t kCvTargetLead = 768;

PLACE_SDRAM_DATA uint32_t cvStreamBuffer[kCvStreamWords] __attribute__((aligned(CACHE_LINE_SIZE)));
static_assert(kCvFramesPerChannel * kCvFrameBytes == sizeof(cvStreamBuffer),
              "ring flush arithmetic must cover exactly the whole buffer");
PLACE_SDRAM_DATA int32_t cvSourceMono[2][kCvMaxWindow] __attribute__((aligned(CACHE_LINE_SIZE)));
PLACE_SDRAM_DATA int32_t cvCaptureScratch[kCvMaxWindow * 2] __attribute__((aligned(CACHE_LINE_SIZE)));

/// The permanent descriptor, in writable SDRAM and built at run time rather than sitting in
/// .rodata as a const initialiser.
///
/// It was a const array, which put it in read-only memory, and the channel answered DER --
/// descriptor error -- with DW set beside it. DW is a descriptor write-back: the transfer engine
/// updates the descriptor in place as it works through it, and a write into read-only memory
/// cannot land. That is the descriptor error, and it is why this channel never moved a single
/// frame on any build.
///
/// The SSI's descriptor is the same shape and does work, which is what made this hard to see
/// from reading. Its buffer is an ordinary array in internal RAM reached through the uncached
/// mirror; this one's ring is PLACE_SDRAM_DATA in external SDRAM. Different memory, and only one
/// of the two takes a descriptor write-back it cannot complete.
///
/// Filled by cvStreamBuildDescriptor() because word 7 has to hold its own address, which no
/// static initialiser in this position can express once the array is not const.
PLACE_SDRAM_DATA uint32_t cvStreamDmaLinkDescriptor[8] __attribute__((aligned(CACHE_LINE_SIZE)));

/// Fills the permanent descriptor and flushes it, so the transfer engine reads what the CPU
/// wrote. Idempotent, and cheap enough to call on every engage rather than tracking whether it
/// has been done.
void cvStreamBuildDescriptor() {
	cvStreamDmaLinkDescriptor[0] = 0b1101;                    // Header
	cvStreamDmaLinkDescriptor[1] = (uint32_t)cvStreamBuffer;  // Source
	cvStreamDmaLinkDescriptor[2] = (uint32_t)&RSPI(SPI_CHANNEL_CV).SPDR.LONG; // Destination
	cvStreamDmaLinkDescriptor[3] = sizeof(cvStreamBuffer);    // Transaction size
	cvStreamDmaLinkDescriptor[4] =
	    0b10000001001000100010001000101000 | DMA_LVL_FOR_SSI | (CV_STREAM_DMA_CHANNEL & 7); // Config
	cvStreamDmaLinkDescriptor[5] = 0;                                                       // Interval
	cvStreamDmaLinkDescriptor[6] = 0;                                                       // Extension
	cvStreamDmaLinkDescriptor[7] = (uint32_t)cvStreamDmaLinkDescriptor; // Next link: itself
	invalidate_range_all_caches((uintptr_t)cvStreamDmaLinkDescriptor,
	                            (uintptr_t)cvStreamDmaLinkDescriptor + sizeof(cvStreamDmaLinkDescriptor));
}

/// Rebuilt on every resume: one partial pass from wherever the transfer engine stopped to
/// the end of the ring, then a link into the permanent descriptor above, which links to
/// itself and carries on forever. Only the source and the size differ from it -- the header is
/// copied, so both descriptors say the same thing about write-back. That was believed to mean
/// the hardware never modifies either; the channel's DW bit says otherwise, which is why both
/// now live in writable memory.
///
/// This exists so a resume can pick up mid-ring. Restarting at the top instead would replay
/// whatever of the previous pass had not been reached yet -- up to a whole ring, 46 ms of
/// already-played audio -- for a gap that was under a millisecond.
///
/// Written by the CPU and read by the transfer engine, so it lives in the same SDRAM the
/// ring does and gets the same flush.
PLACE_SDRAM_DATA uint32_t cvStreamResumeDescriptor[8] __attribute__((aligned(CACHE_LINE_SIZE)));

/// Where in the ring the transfer engine stopped, from its own current-source-address
/// register. Set when the bus is given up, read when it is taken back.
uint32_t cvStreamResumeSource = 0;

/// True while the display has the shared bus. Only ever true on an OLED model.
bool cvStreamYielded = false;

/// Set whenever the stream has been off the bus, so the next pump puts the write pointer
/// back where it belongs instead of letting the missed time turn into delay.
bool cvStreamResyncPending = false;

// Diagnostic counters. volatile because every one of them is written from interrupt context
// and read from the menu thread; not atomic, because a diagnostic that perturbs the path it
// measures is worse than one that is occasionally a count behind. See CvStat in the header
// for what each answers.
volatile uint32_t cvStatStarts = 0;
volatile uint32_t cvStatStops = 0;
volatile uint32_t cvStatYields = 0;
volatile uint32_t cvStatTakeBacks = 0;
volatile uint32_t cvStatPumpWritten = 0;
volatile uint32_t cvStatPumpDropped = 0;
volatile uint32_t cvStatResyncs = 0;
volatile uint32_t cvStatDmaStalls = 0;
/// Last read pointer the pump saw, so a stalled transfer engine can be told from a moving one.
uint32_t cvStatLastReadAddress = 0;
/// Frames consumed and frames emitted, summed over the windows counted in cvStatPumpWritten.
/// Kept as totals and divided only when read, so the hot path does no arithmetic it can avoid.
volatile uint32_t cvStatAdvanceTotal = 0;
volatile uint32_t cvStatEmitTotal = 0;
/// Every RSPI status bit seen set, OR-ed. Sticky on purpose: a mode fault that happened once,
/// minutes ago, is exactly the kind of thing a sampled reading would miss.
volatile uint32_t cvStatSpiStatus = 0;
/// The lead as last measured, in frames.
volatile uint32_t cvStatLeadNow = 0;
/// The two DMA channel status registers and the live SPI status, sampled rather than accumulated.
volatile uint32_t cvStatChanStatus = 0;
volatile uint32_t cvStatOledChanStatus = 0;
volatile uint32_t cvStatSpiLive = 0;
/// Pump samples that caught TEND set -- a transmit sequence having completed, which is the
/// chip-select having been negated. Zero means the select is still held and the converter is
/// still never latching; this is the counter that says whether the select framing took.
volatile uint32_t cvStatTendSeen = 0;

/// The SPI block as boot configured it for the DAC, captured before the display's own setup
/// could overwrite it. Restored every time the bus is handed over.
uint8_t cvStreamBootSpbr = 1;
uint8_t cvStreamBootSpdcr = 0x60u;
uint8_t cvStreamBootSpbfcr = 0b00100010;
uint16_t cvStreamBootSpcmd0 = 0b0000001100000010;
bool cvStreamBootSpiCaptured = false;

/// Bound on every wait for the hardware below. A handover runs with interrupts off, so this
/// is what puts a ceiling on how long they stay off: each wait is one 32-bit word at the
/// streaming rate, about 10 us, and this is several times that. Reaching the bound means the
/// hardware is not going to answer, and pressing on -- at worst one lost or truncated DAC
/// word, which is a tick -- beats hanging with interrupts disabled.
///
/// It was 20000, chosen when these waits ran with interrupts on and only had to avoid hanging
/// an interrupt handler forever. That is 20 times longer than the wait can legitimately take.
constexpr uint32_t kCvBusWaitSpins = 2000;

bool cvStereoSplitGlobal = false;
bool cvStreamRunning = false;
uint32_t cvStreamWriteFrame = 0;
bool cvSourceValid[2] = {false, false};
float cvFeedLastIn[2] = {0.0f, 0.0f};
float cvFeedLastOut[2] = {0.0f, 0.0f};

/// Which Clips fed each socket. Accumulated while the outputs render, then
/// compared with the previous render's set: per-clip routing means the set can
/// change mid-performance, and carrying filter state across that change clicks.
uint32_t cvSourceSignatureAccum[2] = {0, 0};
uint32_t cvSourceSignatureLive[2] = {0, 0};
bool cvAnyRoutedAccum = false;
bool cvSplitAccum = false;
bool cvSplitLive = false;

/// Treble correction, as the one number that decides it: how hard the filter inverts the
/// socket's own 700 Hz low-pass.
///
/// The shelf is out = scale*(in - pole*lastIn) + limit*lastOut. At DC (z=1) its gain is
/// scale*(1-pole)/(1-limit); at Nyquist (z=-1) it is scale*(1+pole)/(1+limit). Their ratio
/// is the boost, and it depends on `limit` alone:
///
///     boost = K * (1-limit)/(1+limit),   K = (1+pole)/(1-pole)
///
/// so limit = (K-boost)/(K+boost), and `scale` is fixed by requiring unity gain at DC --
/// which is what keeps this a tone control rather than a volume one: the bass sits where it
/// would with no correction, and only the treble moves.
///
/// K is full inversion, flat to 20 kHz, and is unreachable -- the output stage compresses
/// above about a third of its swing, and every dB of boost spends a dB of headroom. The
/// chosen boost is therefore a listening call, not a calculation: x6.87, flat to roughly
/// 4.8 kHz. Revisit by ear if the correction sounds wrong on real material; the resampler
/// upstream of this shapes where the sidebands fall, so a verdict here can go stale if that
/// changes.
constexpr float kCvEqK = (1.0f + kCvEqPole) / (1.0f - kCvEqPole); // 20.03
constexpr float kCvEqBoost = 6.866f;
constexpr float kCvStreamEqLimit = (kCvEqK - kCvEqBoost) / (kCvEqK + kCvEqBoost);
constexpr float kCvStreamEqScale = (1.0f - kCvStreamEqLimit) / (1.0f - kCvEqPole);

constexpr float kCvFeedBaseScale = 0.098f;
/// Display 50 is x256, and every step below it is a fixed 1.2 dB rather than a fixed
/// multiplier, so the steps sound evenly spaced the whole way down and reach roughly 59 dB
/// down by 1. Display 0 is a true mute rather than the bottom of the taper.
///
/// 0-50 at 1.2 dB/step, matching every other param's display range -- so a master recorded
/// into an automation lane reads the same numbers the menu shows.
constexpr float kCvLevelTopGain = 256.0f;
constexpr float kCvLevelDbPerStep = 1.2f;

float cvLevelToScale(int32_t display) {
	if (display <= 0) {
		return 0.0f;
	}
	if (display > kCvMasterDisplayMax) {
		display = kCvMasterDisplayMax;
	}
	const float decibels = (float)(display - kCvMasterDisplayMax) * kCvLevelDbPerStep;
	return kCvFeedBaseScale * kCvLevelTopGain * powf(10.0f, decibels / 20.0f);
}

float cvFeedScale[2] = {cvLevelToScale(40), cvLevelToScale(40)};
int32_t cvPrevSample[2] = {0, 0};

/// Output samples per input sample. SPBR=9 gives a 3.333 MHz bit clock; at ~35.5 bit-times
/// per frame that puts the stream at ~46.95 kHz per socket against the engine's 44.1 kHz.
/// Only a starting estimate -- the loop below trims it to whatever the hardware actually does.
///
/// Re-estimated for the per-word select framing above: the negation and next-access delays add
/// roughly two bit-times to each word, which drops the stream from about 46.9 kHz to about
/// 44.4 kHz per socket. Softer than the old figure, because the delays are in RSPCK and PCLK
/// units rather than bit-times, so the band below carries the uncertainty.
constexpr float kCvNominalRate = 44444.0f / 44100.0f;

/// How far the ratio may stray from nominal. Both clocks come off the same crystal, so the
/// true ratio is fixed and the only slack needed is for the estimate above being slightly
/// wrong -- the loop finds the real value within this band and holds it.
///
/// On an idle machine the loop settles around the nominal above, so the estimate is good
/// and the band only has to cover the residual: +-0.3% is about 10 cents worst case, at the
/// edge of audible on a sustained tone. If the resync counter climbs steadily on a quiet
/// song, this band is too narrow rather than too wide -- the true rate is outside it and
/// the loop cannot reach it.
///
/// Widened from 0.003 with the select framing. The old band assumed the estimate above was
/// good to a fraction of a percent, which was fair when a word was 32 bits and a little
/// overhead. It is not fair now: the added delays are specified in clock periods whose exact
/// count is not something to guess at, so the band has to be wide enough to contain the truth
/// and let the loop find it. It converges on the real ratio either way -- the band only has to
/// not exclude it. If the resync counter sits near zero once running, this is wide enough.
constexpr float kCvRateSpan = 0.10f;
constexpr float kCvRateMin = kCvNominalRate * (1.0f - kCvRateSpan);
constexpr float kCvRateMax = kCvNominalRate * (1.0f + kCvRateSpan);

/// A rate trim can only fix a *rate* error. A phase error -- the buffer running empty or
/// overfull because the engine arrived late -- can only be nursed back over thousands of
/// windows, during which the integrator winds up and hits a clamp. Past this much error,
/// snap the write pointer back instead: one discontinuity, instantly recovered, rather than
/// continuous pitch movement.
///
/// Sized so ordinary jitter never reaches it and only a real stall does: the lead may
/// wander between 128 and 1408 frames before this fires, inside a 2048-frame ring.
constexpr int32_t kCvResyncThreshold = 640;

/// The lead error is smoothed before it reaches the integrator, with a time constant of
/// roughly three seconds at the ~345 Hz window rate.
///
/// The engine renders in irregular windows, so the raw lead jitters by hundreds of frames
/// under load -- phase jitter, which a rate trim cannot correct and must not chase. Feeding
/// the raw per-window error to the integrator makes the loop bang-bang between its clamps.
/// Averaging leaves only genuine rate drift, which is what a rate trim is for.
constexpr float kCvLeadAvgAlpha = 0.001f;
float cvLeadAvg = 0.0f;

/// Integrator, applied to the *averaged* error. Small enough that a full band traverse
/// takes seconds -- a gain around 1e-7 sweeps the whole band in about a second, which is
/// audible as a slow warble even at only ~10 cents depth. Depth is bounded by the clamps;
/// this constant controls the speed.
constexpr float kCvRateTrackGain = 0.000000003f;

/// Proportional gain, on the SMOOTHED error.
///
/// A rate error integrates into lead, and this loop integrates lead error into rate: two
/// integrators in series, 180 degrees of phase lag, which oscillates however small the gain
/// is made. A double integrator needs damping, not a smaller gain -- this term provides it,
/// fed the smoothed error rather than the raw per-window one so it damps drift without
/// injecting jitter into the ratio.
///
/// Sized so a 100-frame averaged error moves the ratio by ~0.03%, correcting that much lead
/// in about a second.
constexpr float kCvRateDampGain = 0.000003f;

float cvRate = kCvNominalRate;
/// Resampling phase, carried across windows. See cvStreamPump.
float cvResamplePos = 0.0f;

constexpr uint16_t kCvCentre = 32768;
constexpr int32_t kCvClamp = 12000;

/// Command word the DAC expects: write-and-update, given channel, 16-bit value.
inline uint32_t cvWord(uint32_t channel, int32_t sample) {
	if (sample > kCvClamp) {
		sample = kCvClamp;
	}
	else if (sample < -kCvClamp) {
		sample = -kCvClamp;
	}
	const uint16_t voltage = (uint16_t)((int32_t)kCvCentre + sample);
	return ((uint32_t)(0b00110000 | (1u << channel)) << 24) | ((uint32_t)voltage << 8);
}

} // namespace

bool cvOutputsAvailable() {
	// Was !have_oled_screen. The display and the DAC share RSPI channel 0, so a stream that
	// could not give the bus back had to be kept off OLED models entirely; it gives the bus
	// back now, so both models can stream. See cvStreamYieldBusToDisplay().
	//
	// Now also the Community Features toggle, and this is the only place it has to be checked:
	// the render path asks this before capturing anything, so with the feature off no Clip is
	// ever routed, cvStreamRenderComplete() stops the stream on the next window, and the
	// sockets go back to note voltages. It used to gate the menus alone, which meant switching
	// AUX Sends off hid the controls while the capture and the stream carried on -- the exact
	// opposite of what an off switch is for, and it took away the menu you would have used to
	// turn the sends down.
	//
	// Menu visibility deliberately stays separate (cvSendMenusVisible): the sends are ordinary
	// params that live in song files, so hiding their editors is a display choice, while this
	// is the feature actually running or not.
	return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::EnableAuxSends);
}

bool cvSendMenusVisible() {
	return true;
}

bool cvStreamIsRunning() {
	return cvStreamRunning;
}

namespace {

/// Adds one Clip's contribution to a socket. Several Clips can share a socket, so
/// the first one this window sets the buffer and the rest add into it.
/// `channel` picks what the socket gets: 0 for left, 1 for right, 2 for the mono sum.
///
/// The Clip's contribution is `post` minus `pre` -- the mix after it rendered minus the
/// snapshot taken before. That subtraction happens here, per sample, rather than in a
/// pass of its own beforehand: this loop already touches every sample it needs, so a
/// separate pass was a second walk over the same data for nothing.
///
/// The mono sum halves each side before adding, so a centred Clip comes out at the
/// level of one channel. A single channel is therefore taken at full scale, not
/// halved -- otherwise turning STEREO SPLIT on would drop a centred Clip by 6 dB.
///
/// `gainFrom` and `gainTo` are Q16 -- kCvSendGainUnity is unity, 0 is silence. The gain
/// ramps across the window rather than stepping at its edge: the window is only ~128
/// samples (~345 Hz), so a step per window is plainly audible as zipper noise while a
/// send is being ridden. At unity throughout there is no multiply at all, which keeps the
/// common case exactly as cheap as it was before sends existed -- this loop runs with all
/// interrupts disabled.
void cvAccumulateInto(uint32_t socket, uint32_t channel, const int32_t* post, const int32_t* pre, uint32_t numSamples,
                      int32_t gainFrom, int32_t gainTo) {
	const bool first = !cvSourceValid[socket];
	int32_t* const dest = cvSourceMono[socket];
	const bool unity = (gainFrom == kCvSendGainUnity) && (gainTo == kCvSendGainUnity);

	// Q16.16 accumulator so the per-sample step needs no divide inside the loop. 64-bit
	// because unity is 65536, and 65536 << 16 does not fit in a signed 32-bit.
	int64_t gain = (int64_t)gainFrom << 16;
	const int64_t gainStep = (((int64_t)(gainTo - gainFrom)) << 16) / (int32_t)numSamples;

	for (uint32_t i = 0; i < numSamples; i++) {
		int32_t sample;
		if (channel == 2) {
			// Halved per side before summing rather than after, so rounding lands the
			// same way regardless of how the two channels are combined.
			const int32_t diffL = post[i * 2] - pre[i * 2];
			const int32_t diffR = post[i * 2 + 1] - pre[i * 2 + 1];
			sample = (diffL >> 1) + (diffR >> 1);
		}
		else {
			sample = post[i * 2 + channel] - pre[i * 2 + channel];
		}
		if (!unity) {
			sample = (int32_t)(((int64_t)sample * (int32_t)(gain >> 16)) >> 16);
			gain += gainStep;
		}
		if (first) {
			dest[i] = sample;
		}
		else {
			dest[i] += sample;
		}
	}
	cvSourceValid[socket] = true;
}

} // namespace

int32_t cvSendParamToGain(int32_t paramValue) {
	// Param space is the full signed range; the bottom is silence and the top is unity.
	const uint32_t position = (uint32_t)paramValue ^ 0x80000000u;
	const uint32_t linear = position >> 16; // 0 .. 65535
	if (linear >= 65535) {
		return kCvSendGainUnity;
	}
	// Unsigned deliberately: 65534 squared is 4.29e9, which overflows a signed 32-bit.
	return (int32_t)((linear * linear) >> 16);
}

/// `sendGain` is this Clip's two send amounts for this window, Q16. `lastSendGain` is the
/// same pair from the previous window, owned by the Clip and updated here -- it is what the
/// ramp starts from, so it has to persist between windows and be per Clip rather than per
/// socket, since several Clips can feed one socket at different amounts.
void cvStreamCapture(uint32_t sourceId, const int32_t* post, const int32_t* pre, uint32_t numSamples,
                     const int32_t* sendGain, int32_t* lastSendGain) {
	if (numSamples == 0 || numSamples > kCvMaxWindow) {
		return;
	}

	// Global, not a per-Clip bit. The old per-Clip bit is still parsed from song files so a
	// song saved before this existed loads without complaint -- it no longer decides
	// anything, since two cables cannot be a stereo pair for one Clip and two mono outs for
	// another at the same time.
	const bool split = cvStereoSplitGlobal;
	// Mixed in rather than summed raw so that two Clips swapping sockets, or one
	// Clip changing its split mode, both register as a change.
	const uint32_t stamp = (sourceId ^ (split ? 0x5bd1e995u : 0u)) * 2654435761u;

	for (uint32_t socket = 0; socket < 2; socket++) {
		const int32_t gainTo = sendGain[socket];
		const int32_t gainFrom = lastSendGain[socket];
		lastSendGain[socket] = gainTo;

		// A send off at both ends of the window is simply not routed to this socket, and
		// must not stamp -- otherwise a send falling to zero never changes the socket's
		// signature and the correction filter is never reset.
		if (gainFrom == 0 && gainTo == 0) {
			continue;
		}

		cvAnyRoutedAccum = true;
		cvSplitAccum |= split;
		cvAccumulateInto(socket, split ? socket : 2, post, pre, numSamples, gainFrom, gainTo);
		cvSourceSignatureAccum[socket] += stamp + socket;
	}
}

/// Called once at the end of the render loop, when the full set of Clips feeding
/// each socket for this window is known. Resets the correction filter for any
/// socket whose sources changed, and starts or stops the stream to match.
void cvStreamRenderComplete() {
	for (uint32_t socket = 0; socket < 2; socket++) {
		if (cvSourceSignatureAccum[socket] != cvSourceSignatureLive[socket]) {
			cvSourceSignatureLive[socket] = cvSourceSignatureAccum[socket];
			cvFeedLastIn[socket] = 0.0f;
			cvFeedLastOut[socket] = 0.0f;
			cvPrevSample[socket] = 0;
		}
		cvSourceSignatureAccum[socket] = 0;
	}

	if (cvAnyRoutedAccum && !cvStreamRunning) {
		cvStreamStart();
	}
	else if (!cvAnyRoutedAccum && cvStreamRunning) {
		cvStreamStop();
	}
	cvAnyRoutedAccum = false;

	cvSplitLive = cvSplitAccum;
	cvSplitAccum = false;
}

bool cvStreamNeedsStereo() {
	return cvSplitLive;
}

bool cvGetStereoSplit() {
	return cvStereoSplitGlobal;
}

void cvSetStereoSplit(bool on) {
	cvStereoSplitGlobal = on;
}

int32_t cvMasterParamToDisplay(int32_t paramValue) {
	// Param space is the full signed range. Rounded rather than truncated so that the value
	// written back from a display number reads back as the same number -- truncating makes the
	// menu appear to drop a step every time you leave it and come back.
	const uint32_t position = (uint32_t)paramValue ^ 0x80000000u;
	return (int32_t)(((uint64_t)position * kCvMasterDisplayMax + 0x80000000ull) >> 32);
}

int32_t cvMasterDisplayToParam(int32_t display) {
	if (display <= 0) {
		return -2147483648;
	}
	if (display >= kCvMasterDisplayMax) {
		return 2147483647;
	}
	// Land in the middle of the display step's band, so a round trip through
	// cvMasterParamToDisplay returns what went in rather than sitting on a boundary. The
	// rounding term is half the *display* range, not half of param space -- getting that wrong
	// put steps 12 and 37 one out, which a simulation caught and no amount of listening would.
	const uint64_t position =
	    ((uint64_t)display * 4294967296ull + (uint64_t)(kCvMasterDisplayMax / 2)) / (uint64_t)kCvMasterDisplayMax;
	return (int32_t)((uint32_t)position ^ 0x80000000u);
}

uint8_t cvMasterKnobSocket = 0;

void cvSetMasterFromParams(int32_t cv1Value, int32_t cv2Value) {
	cvFeedScale[0] = cvLevelToScale(cvMasterParamToDisplay(cv1Value));
	// With the pair patched as one stereo destination there is one master, not two: the
	// sockets carry left and right of the same thing, so CV1's value drives both. This is the
	// same rule the sends follow, and it is why the second knob is idle when split is on.
	cvFeedScale[1] = cvLevelToScale(cvMasterParamToDisplay(cvStereoSplitGlobal ? cv1Value : cv2Value));
}

int32_t* cvStreamCaptureScratch() {
	return cvCaptureScratch;
}

/// Called once per audio window. Resamples both captured tracks to whatever rate
/// the transfer engine is running at, applies the correction, and interleaves
/// them into the buffer being streamed. Staying half a buffer ahead of the read
/// position is what keeps the two rates matched without knowing the ratio.
void cvStreamPump(uint32_t numSamples) {
	if (!cvStreamRunning || numSamples == 0 || numSamples > kCvMaxWindow) {
		cvSourceValid[0] = cvSourceValid[1] = false;
		return;
	}

	// Off the bus while the display has it. Drop the window rather than write it into a ring
	// nothing is currently reading: the read pointer is frozen, so anything written now would
	// emerge late by however long the display took, and every window after it would inherit
	// that same delay. Dropping costs the gap and nothing beyond it.
	if (cvStreamYielded) {
		cvStatPumpDropped = cvStatPumpDropped + 1;
		cvStreamResyncPending = true;
		cvSourceValid[0] = cvSourceValid[1] = false;
		return;
	}

	const uint32_t readAddress = DMACn(CV_STREAM_DMA_CHANNEL).CRSA_n;
	// How far the transfer engine got since the previous window, in frames, wrap included. The
	// boolean version of this could only say "it moved" or "it did not"; what actually settles
	// the question is the distance, because that is the converter's real rate and it can be
	// compared directly against what the pump emitted.
	if (cvStatLastReadAddress != 0) {
		uint32_t advanceBytes = readAddress - cvStatLastReadAddress;
		if (readAddress < cvStatLastReadAddress) {
			advanceBytes += sizeof(cvStreamBuffer); // wrapped the ring
		}
		cvStatAdvanceTotal = cvStatAdvanceTotal + (advanceBytes >> 3);
		if (advanceBytes == 0) {
			cvStatDmaStalls = cvStatDmaStalls + 1;
		}
	}
	cvStatLastReadAddress = readAddress;
	cvStatPumpWritten = cvStatPumpWritten + 1;

	// Sticky, and read here because the pump is the one place that runs at a known rate while
	// the stream is supposed to be live. MODF is the bit that matters: the block gives up
	// transmitting when it sees one, which stalls the transfer engine feeding it.
	cvStatSpiStatus = cvStatSpiStatus | (uint32_t)RSPI(SPI_CHANNEL_CV).SPSR.BYTE;
	// Live, unaccumulated, and sampled here because the pump only reaches this line while the
	// stream owns the bus -- so these describe the stream's own ownership rather than whatever
	// the display was doing at some point since boot.
	cvStatSpiLive = (uint32_t)RSPI(SPI_CHANNEL_CV).SPSR.BYTE;
	if ((cvStatSpiLive & 0x40u) != 0u) {
		cvStatTendSeen = cvStatTendSeen + 1;
	}
	cvStatChanStatus = DMACn(CV_STREAM_DMA_CHANNEL).CHSTAT_n;
	cvStatOledChanStatus = DMACn(OLED_SPI_DMA_CHANNEL).CHSTAT_n;

	const uint32_t readFrame = ((readAddress - (uint32_t)cvStreamBuffer) >> 3) & (kCvFramesPerChannel - 1);
	const uint32_t lead = (cvStreamWriteFrame - readFrame) & (kCvFramesPerChannel - 1);
	// Deliberately NOT half the ring: the lead sets latency, while the ring only has to be
	// big enough that a burst cannot lap the reader. Tying the two together would make every
	// buffer enlargement cost delay for no reason.
	constexpr int32_t targetLead = kCvTargetLead;
	int32_t leadError = targetLead - (int32_t)lead;
	cvStatLeadNow = lead;

	// Phase resync, for a genuine stall only. A rate trim cannot fix a phase error in any
	// useful time, so past the threshold the write pointer is simply put where it belongs.
	// Costs one discontinuity.
	//
	// The rate estimate is deliberately KEPT. Only the phase is broken here; the ratio is a
	// property of two crystals and is still correct, and throwing it away made the loop
	// re-acquire from nominal every time this fired.
	// cvStreamResyncPending forces the same treatment after the bus has been away, where the
	// lead is wrong by exactly the time the display held it and there is nothing to converge
	// on -- the phase simply moved.
	if (cvStreamResyncPending || leadError > kCvResyncThreshold || leadError < -kCvResyncThreshold) {
		cvStatResyncs = cvStatResyncs + 1;
		cvStreamResyncPending = false;
		cvStreamWriteFrame = ((uint32_t)readFrame + (uint32_t)targetLead) & (kCvFramesPerChannel - 1);
		cvResamplePos = 0.0f;
		for (uint32_t socket = 0; socket < 2; socket++) {
			cvFeedLastIn[socket] = 0.0f;
			cvFeedLastOut[socket] = 0.0f;
			cvPrevSample[socket] = 0;
		}
		leadError = 0;
	}

	// Both clocks come off the same crystal, so the true ratio between them is a
	// constant -- there is nothing to chase, only something to converge on. The
	// integrator finds it and then stops moving; the proportional term only damps
	// acquisition, and is tiny so it does not move the ratio once locked.
	cvLeadAvg += ((float)leadError - cvLeadAvg) * kCvLeadAvgAlpha;

	// PI on the smoothed error. The integrator alone is a second integrator in series with
	// the one the buffer already provides, and oscillates; the proportional term damps it.
	cvRate += cvLeadAvg * kCvRateTrackGain;
	if (cvRate < kCvRateMin) {
		cvRate = kCvRateMin;
	}
	else if (cvRate > kCvRateMax) {
		cvRate = kCvRateMax;
	}
	float rateNow = cvRate + cvLeadAvg * kCvRateDampGain;
	if (rateNow < kCvRateMin) {
		rateNow = kCvRateMin;
	}
	else if (rateNow > kCvRateMax) {
		rateNow = kCvRateMax;
	}

	// Input samples consumed per output sample. Held constant, with the phase carried
	// across windows rather than restarted at zero each window and the window's input
	// stretched to fit a whole number of outputs.
	//
	// Emitting a whole number of samples per window forces the ratio to alternate
	// between neighbouring integers -- about 0.73% either way -- and an alternating
	// resampling ratio is frequency modulation at the window rate. The sidebands grow
	// with frequency, from about -51 dB at 260 Hz to -25 dB by 5 kHz, audible as grain
	// on anything with strong harmonics even when a plain sine sounds clean. Carrying
	// the phase keeps the instantaneous ratio fixed and lets the sample count per
	// window vary instead, which costs nothing and holds the jitter to about 0.07%.
	const float step = 1.0f / rateNow;

	const float startPos = cvResamplePos;
	int32_t toEmit = 0;
	{
		float p = startPos;
		while (p < (float)numSamples && toEmit < (int32_t)kCvMaxWindow) {
			p += step;
			toEmit++;
		}
		if (toEmit < 1 || toEmit >= (int32_t)kCvMaxWindow) {
			toEmit = (toEmit < 1) ? 1 : (int32_t)kCvMaxWindow;
			cvResamplePos = 0.0f; // resync rather than carry a position we did not reach
		}
		else {
			cvResamplePos = p - (float)numSamples;
		}
	}

	cvStatEmitTotal = cvStatEmitTotal + (uint32_t)toEmit;

	for (uint32_t socket = 0; socket < 2; socket++) {
		// Folded into the scale rather than shifted off the integer first. Shifting
		// first quantised the signal to steps of `scale` DAC counts -- at the tuned
		// level that is a step of about 6 counts in a working range of 12000, so
		// roughly 11 bits of the converter's resolution instead of 13.5. Audible as
		// grit on quiet material, which is exactly where it hurts most.
		const float scale = cvFeedScale[socket] * (1.0f / 32768.0f);
		const int32_t* const source = cvSourceMono[socket];
		const bool valid = cvSourceValid[socket];
		float position = startPos;
		uint32_t frame = cvStreamWriteFrame;

		for (int32_t i = 0; i < toEmit; i++) {
			float in = 0.0f;
			if (valid) {
				uint32_t index = (uint32_t)position;
				if (index >= numSamples) {
					index = numSamples - 1;
				}
				// Interpolated rather than nearest-neighbour. 44.1 kHz into a stream
				// running near 47 kHz is a ratio no whole number of samples fits, so
				// picking the nearest one holds each value for an irregular length of
				// time. That irregularity is a modulation of the signal, and it lands
				// as inharmonic grit right across the band -- much louder than the
				// converter's own noise. The first output of a window interpolates
				// from the last sample of the previous one, hence cvPrevSample.
				const float frac = position - (float)index;
				const float a = (float)((index == 0) ? cvPrevSample[socket] : source[index - 1]);
				const float b = (float)source[index];
				in = (a + (b - a) * frac) * scale;
			}
			position += step;

			const float out =
			    kCvStreamEqScale * (in - kCvEqPole * cvFeedLastIn[socket]) + kCvStreamEqLimit * cvFeedLastOut[socket];
			cvFeedLastIn[socket] = in;
			cvFeedLastOut[socket] = out;

			cvStreamBuffer[frame * 2 + socket] = cvWord(socket, (int32_t)out);
			frame = (frame + 1) & (kCvFramesPerChannel - 1);
		}

		// Carried so the next window can interpolate across the join rather than
		// stepping. Cleared when a socket falls silent, so a stale sample can't
		// reappear when it comes back.
		cvPrevSample[socket] = valid ? source[numSamples - 1] : 0;
	}

	const uint32_t writtenFrom = cvStreamWriteFrame;
	cvStreamWriteFrame = (cvStreamWriteFrame + (uint32_t)toEmit) & (kCvFramesPerChannel - 1);
	cvSourceValid[0] = cvSourceValid[1] = false;

	// All caches, not just L1: this firmware enables the L2 data cache (L2CacheUnlockData at
	// boot), so an L1-only flush leaves the words the DMA is about to read sitting in L2 and
	// the converter is fed whatever RAM happened to hold. Every other DMA user here does the
	// same -- see the OLED and SD paths.
	//
	// Only the frames just written, not the whole ring. The ring is 16 KB, which is 512 L2
	// line operations each with a spin-wait, and this runs every window on a machine already
	// at its CPU limit; the written run is nearer 136 frames, about 34 lines. Split in two
	// when the run wraps.
	const uintptr_t base = (uintptr_t)cvStreamBuffer;
	const uint32_t untilEnd = kCvFramesPerChannel - writtenFrom;
	const uint32_t firstRun = ((uint32_t)toEmit < untilEnd) ? (uint32_t)toEmit : untilEnd;
	invalidate_range_all_caches(base + writtenFrom * kCvFrameBytes, base + (writtenFrom + firstRun) * kCvFrameBytes);
	if ((uint32_t)toEmit > firstRun) {
		invalidate_range_all_caches(base, base + ((uint32_t)toEmit - firstRun) * kCvFrameBytes);
	}
}

namespace {

/// Puts the SPI block and the DAC's chip-select into the shape the stream needs, then sets
/// the transfer engine running from cvStreamResumeSource.
void cvStreamEngageBus() {
	// Before anything else: the descriptor has to exist and be visible to the transfer engine.
	cvStreamBuildDescriptor();

	if (deluge::hid::display::have_oled_screen) {
		// Take the RSPI transmit request off the display's DMA channel before claiming it for
		// ours. This is the resource the handover never arbitrated, and it is why the stream
		// has never made a sound on an OLED machine.
		//
		// oledDMAInit() assigns DMARS_FOR_RSPI_TX to OLED_SPI_DMA_CHANNEL (4) once at boot and
		// leaves it there; initDMAWithLinkDescriptor below assigns the same MID/RID to
		// CV_STREAM_DMA_CHANNEL (5). setDMARS writes only its own half of the shared DMARS2
		// register, so both assignments survive -- and the hardware manual says not to do that:
		// a request resource belongs to one channel. With two claiming it the request goes to
		// one of them, and measurement says it is not ours. Frames consumed per window came back
		// as exactly zero while the pump was emitting normally and the lead ran away to 1278
		// against a target of 768.
		//
		// On a 7-segment machine oledDMAInit() never runs, channel 4 never claims the resource,
		// channel 5 has it uncontested and the stream works. That is the whole of the
		// 7SEG-works / OLED-does-not split, and no amount of correct SPI and chip-select
		// handling was ever going to bridge it.
		//
		// Released before ours is claimed, so the two are never both assigned. The display and
		// the stream already never transfer at the same time -- that is what the yield exists
		// for -- so exclusive ownership is exactly the right shape.
		setDMARS(OLED_SPI_DMA_CHANNEL, 0);

		// Hardware chip-select. Boot leaves this pin a GPIO on OLED models so the display and
		// the DAC can take turns on the bus, but the stream needs a select pulse per 32-bit
		// word -- roughly 94,000 a second -- and only the SPI block itself can do that. It goes
		// back to being a GPIO the moment the bus is handed over.
		setPinMux(SPI_SSL.port, SPI_SSL.pin, 3);
	}

	RSPI(SPI_CHANNEL_CV).SPCR &= ~(1 << 6); // disable while reconfiguring

	// 32-bit frames. On 7SEG boot's settings are still in place and these write what is
	// already there; on OLED the display path leaves the block set up for 8-bit writes, so
	// this is what puts it back to talking to a converter.
	RSPI(SPI_CHANNEL_CV).SPDCR = 0x60u;
	// SLNDEN and SPNDEN, bits 14 and 13, on top of the word format. They are what make the
	// converter's chip-select pulse once per word instead of staying asserted.
	//
	// Without them the block negates SSL only when it reaches the end of a transmit sequence,
	// and a sequence ends when the transmit buffer runs dry. The stream's whole design is to
	// never let that happen -- a self-linking DMA descriptor keeps the FIFO fed forever -- so
	// the sequence never ended, SSL never rose, and the converter, which latches on that edge,
	// never took a single sample. Measured: TEND never set once, live or accumulated across a
	// whole session, with SPTEF never set beside it.
	//
	// It is also where the pops came from. A handover stops the DMA, the FIFO drains, the
	// sequence finally ends, SSL rises and the converter latches whatever it happens to hold --
	// one step, one pop, which is why they tracked the screen redraws that cause handovers.
	//
	// The delay registers are left at their defaults: one RSPCK for the negation, one RSPCK
	// plus two PCLK before the next access. Minimum in both cases, because the only thing
	// needed here is the edge itself.
	//
	// sendCVWord() -- the note-voltage path that has always worked on this hardware -- writes
	// this same SPDCR and SPCMD0 and differs only in driving the select by hand, one pulse per
	// word. This makes the stream do what that path already proved the converter wants.
	RSPI(SPI_CHANNEL_CV).SPCMD0 = 0b0110001100000010;
	// Both FIFOs flushed before the trigger levels are set. Anything left in them belongs to
	// whoever had the bus last and is the wrong width for what follows.
	RSPI(SPI_CHANNEL_CV).SPBFCR.BYTE = 0b00100010 | (1 << 7) | (1 << 6);
	RSPI(SPI_CHANNEL_CV).SPBFCR.BYTE = 0b00100010;

	RSPI(SPI_CHANNEL_CV).SPBR = 9;         // ~3.3 MHz -> ~47 kHz per socket
	RSPI(SPI_CHANNEL_CV).SPCR |= (1 << 1); // transmit only
	RSPI(SPI_CHANNEL_CV).SPCR |= (1 << 6);

	const uint32_t bufferStart = (uint32_t)cvStreamBuffer;
	const uint32_t bufferEnd = bufferStart + sizeof(cvStreamBuffer);
	if (cvStreamResumeSource > bufferStart && cvStreamResumeSource < bufferEnd) {
		// Mid-ring. One partial pass to the end, then into the permanent descriptor.
		for (uint32_t word = 0; word < 8; word++) {
			cvStreamResumeDescriptor[word] = cvStreamDmaLinkDescriptor[word];
		}
		cvStreamResumeDescriptor[1] = cvStreamResumeSource;
		cvStreamResumeDescriptor[3] = bufferEnd - cvStreamResumeSource;
		cvStreamResumeDescriptor[7] = (uint32_t)cvStreamDmaLinkDescriptor;
		invalidate_range_all_caches((uintptr_t)cvStreamResumeDescriptor,
		                            (uintptr_t)cvStreamResumeDescriptor + sizeof(cvStreamResumeDescriptor));
		initDMAWithLinkDescriptor(CV_STREAM_DMA_CHANNEL, cvStreamResumeDescriptor, DMARS_FOR_RSPI_TX);
	}
	else {
		// At the top of the ring, or an address that is not in it at all -- which the hardware
		// should never report, but if it does, starting cleanly beats following it.
		initDMAWithLinkDescriptor(CV_STREAM_DMA_CHANNEL, cvStreamDmaLinkDescriptor, DMARS_FOR_RSPI_TX);
	}
	dmaChannelStart(CV_STREAM_DMA_CHANNEL);
}

/// Stops the transfer engine, notes where it stopped, and leaves the SPI block and the
/// chip-select as boot had them so somebody else can use the bus.
void cvStreamReleaseBus() {
	// Stop the transfer engine before touching the SPI block, or it keeps writing into a
	// register being reconfigured.
	DMACn(CV_STREAM_DMA_CHANNEL).CHCTRL_n |= DMAC0_CHCTRL_n_CLREN;

	// It finishes the unit it is on rather than dropping it, so wait for it to actually go
	// inactive before reading the address it reached.
	for (uint32_t spins = 0; spins < kCvBusWaitSpins; spins++) {
		if (!(DMACn(CV_STREAM_DMA_CHANNEL).CHSTAT_n & DMAC0_CHSTAT_n_TACT)) {
			break;
		}
	}
	cvStreamResumeSource = DMACn(CV_STREAM_DMA_CHANNEL).CRSA_n;

	// Then let the word already in the SPI block finish, so the converter gets all 32 bits
	// and its select pulse before the pin stops being a select. A word cut off partway is a
	// wrong sample latched at full scale, which is a click rather than a gap.
	for (uint32_t spins = 0; spins < kCvBusWaitSpins; spins++) {
		if (RSPI(SPI_CHANNEL_CV).SPSR.BIT.TEND) {
			break;
		}
	}

	RSPI(SPI_CHANNEL_CV).SPCR &= ~(1 << 6); // disable while reconfiguring
	// Flush both FIFOs. Whatever is still queued is 32-bit converter data, and the next user
	// of this bus is writing bytes to a display.
	RSPI(SPI_CHANNEL_CV).SPBFCR.BYTE = cvStreamBootSpbfcr | (1 << 7) | (1 << 6);
	RSPI(SPI_CHANNEL_CV).SPBFCR.BYTE = cvStreamBootSpbfcr;
	RSPI(SPI_CHANNEL_CV).SPBR = cvStreamBootSpbr;
	RSPI(SPI_CHANNEL_CV).SPDCR = cvStreamBootSpdcr;
	RSPI(SPI_CHANNEL_CV).SPCMD0 = cvStreamBootSpcmd0;
	RSPI(SPI_CHANNEL_CV).SPCR &= ~(1 << 1); // full duplex again, not transmit-only
	RSPI(SPI_CHANNEL_CV).SPCR |= (1 << 6);

	if (deluge::hid::display::have_oled_screen) {
		// Park the select high as a GPIO, the way boot left it, so the display's data is not
		// also clocked into the converter. Driven before it is made an output, so it cannot
		// glitch low on the way.
		setOutputState(SPI_SSL.port, SPI_SSL.pin, true);
		setPinAsOutput(SPI_SSL.port, SPI_SSL.pin);

		// And hand the RSPI transmit request back to the display's channel, ours released
		// first so the resource is never claimed by both. See cvStreamEngageBus(). Without the
		// return leg the display's own frames would stop the moment the stream first ran,
		// which is a worse failure than the one this fixes.
		setDMARS(CV_STREAM_DMA_CHANNEL, 0);
		setDMARS(OLED_SPI_DMA_CHANNEL, DMARS_FOR_RSPI_TX + (SPI_CHANNEL_OLED_MAIN << 2));
	}
	// On 7SEG the pin stays the hardware select it has been since boot: nothing else is on
	// this bus there, and turning it into a GPIO would break the note-voltage path, which
	// relies on the SPI block pulsing it.
}

} // namespace

void cvStreamRecordBootSpiConfig() {
	if (cvStreamBootSpiCaptured) {
		return;
	}
	cvStreamBootSpbr = RSPI(SPI_CHANNEL_CV).SPBR;
	cvStreamBootSpdcr = RSPI(SPI_CHANNEL_CV).SPDCR;
	cvStreamBootSpbfcr = RSPI(SPI_CHANNEL_CV).SPBFCR.BYTE;
	cvStreamBootSpcmd0 = RSPI(SPI_CHANNEL_CV).SPCMD0;
	cvStreamBootSpiCaptured = true;
}

void cvStreamYieldBusToDisplay() {
	// Unguarded, and first, because this is the answer almost every time: no send is active,
	// and this is on the path every CV note voltage and every display frame takes. Disabling
	// interrupts to find that out would put a cost on the ordinary case that the feature does
	// not earn -- and gate timing is measured against that path. Racing a start is not a risk:
	// cvStreamStart() settles for itself, with interrupts off, whether the bus is already
	// somebody else's.
	if (!cvStreamRunning) {
		return;
	}
	// Interrupts off across the flag AND the hardware it describes, because they are one
	// step. The flag on its own does not make a release and a take mutually exclusive: they
	// reach the SPI block from different places and only the flag was ever guarded.
	//
	// oledDeselectionComplete() clears spiBusCurrentlySending before it hands the bus back,
	// so from that instant the bus reads as free while cvStreamEngageBus() is still setting
	// it up -- and anything landing in that window (an interrupt reaching enqueueCVMessage(),
	// the main thread reaching enqueueOLEDFrame()) starts the opposite sequence on the same
	// registers and the same chip-select pin. What comes out is a free-running DMA feeding a
	// block the display believes it owns, which no later handover undoes.
	//
	// A song load is where that window gets hit, which is why the freeze looked like a file
	// problem: FileReader::readDone() runs the audio engine, the UI timer manager and
	// oledRoutine() back to back every 64 reads, so the render path's starts and stops and
	// the display's frames interleave for as long as the load takes.
	//
	// What it costs is the two waits in cvStreamReleaseBus() -- one 32-bit word each at the
	// streaming rate, about 20 us together, and kCvBusWaitSpins as the hard ceiling. An audio
	// window is a hundred times that, and the one path that already held interrupts across
	// this (enqueueCVMessage) has been doing so all along.
	//
	// The flag still moves before the hardware does, so a pump arriving after this sees that
	// the ring is no longer being read.
	CriticalSectionGuard guard;
	if (!cvStreamRunning || cvStreamYielded) {
		return;
	}
	cvStreamYielded = true;
	cvStatYields = cvStatYields + 1;
	cvStreamReleaseBus();
}

void cvStreamTakeBusBack() {
	if (!cvStreamRunning) {
		return;
	}
	// Interrupts off across the whole take, for the reason set out in
	// cvStreamYieldBusToDisplay(): the bus reads as free the moment the flag clears, and the
	// engage below is not instantaneous.
	CriticalSectionGuard guard;
	if (!cvStreamRunning || !cvStreamYielded) {
		return;
	}
	// Marked before the flag clears, so a pump landing between the two still resyncs rather
	// than trusting a lead measured against a channel that is not running. Such a pump reads
	// the address the transfer engine stopped at, which is exactly where cvStreamEngageBus()
	// is about to resume from -- so the two agree.
	cvStreamResyncPending = true;
	cvStreamYielded = false;
	cvStatTakeBacks = cvStatTakeBacks + 1;
	cvStreamEngageBus();
}

void cvStreamStart() {
	for (uint32_t frame = 0; frame < kCvFramesPerChannel; frame++) {
		cvStreamBuffer[frame * 2] = cvWord(0, 0);
		cvStreamBuffer[frame * 2 + 1] = cvWord(1, 0);
	}
	// All caches, not just L1: main enables the L2 data cache (L2CacheUnlockData at boot),
	// so an L1-only flush leaves the words the DMA is about to read sitting in L2 and the
	// converter is fed whatever RAM happened to hold. Every other DMA user here does the same.
	invalidate_range_all_caches((uintptr_t)cvStreamBuffer, (uintptr_t)cvStreamBuffer + sizeof(cvStreamBuffer));

	// Start already at the target lead rather than at zero. The buffer was just filled with
	// the centre value, so the DMA reads silence until the writer catches up, and the loop
	// begins with no error to correct instead of a full-scale one.
	//
	// Starting at zero makes every stream start an acquisition transient that drives the
	// ratio hard onto one clamp and overshoots onto the other -- inaudible in itself, but it
	// also contaminates any diagnostic reading taken near a start, since the extremes then
	// reflect the transient rather than the running state.
	cvStreamWriteFrame = (uint32_t)kCvTargetLead & (kCvFramesPerChannel - 1);
	cvFeedLastIn[0] = cvFeedLastIn[1] = 0.0f;
	cvFeedLastOut[0] = cvFeedLastOut[1] = 0.0f;
	cvPrevSample[0] = cvPrevSample[1] = 0;
	cvRate = kCvNominalRate;
	cvLeadAvg = 0.0f;
	cvResamplePos = 0.0f;

	// From the top of the ring, which is where a fresh start reads from and what the lead
	// above was set against.
	cvStreamResumeSource = (uint32_t)cvStreamBuffer;

	// With interrupts off, and the engage inside the same window. Reading
	// spiBusCurrentlySending, settling what to do about it, and doing it have to be one step:
	// a transfer completing in between would call cvStreamTakeBusBack() while this function
	// still had cvStreamYielded false, find nothing to do, and leave the stream parked until
	// whatever redrew the screen next happened to release the bus again -- and a transfer
	// *starting* in between would program the block on top of the engage.
	//
	// This is called from the render path, where the bus is free as far as the display is
	// concerned, so nothing is holding a claim on our behalf.
	CriticalSectionGuard guard;
	cvStreamResyncPending = false;
	cvStreamRunning = true;
	cvStatStarts = cvStatStarts + 1;
	// Cleared so the first pump after a start cannot compare against an address from the last
	// time the stream ran and report a phantom stall or a phantom advance.
	cvStatLastReadAddress = 0;

	// On an OLED model the display may be part-way through a frame on this same bus right
	// now. Come up already yielded and let whoever holds it hand it over when they are
	// finished, rather than reprogramming the SPI block underneath a transfer in flight.
	cvStreamYielded = deluge::hid::display::have_oled_screen && spiBusCurrentlySending;

	if (!cvStreamYielded) {
		cvStreamEngageBus();
	}
}

void cvStreamStop() {
	// Put the SPI back exactly as boot left it, rather than clearing it: the CV sockets go
	// back to being note-voltage outputs when nothing is routed here, and that path uses this
	// same channel. If the display already has the bus there is nothing of ours on it to take
	// off, and reconfiguring the block would be reaching into somebody else's transfer.
	//
	// This used to write SPBR = 1 directly, "boot's rate for the 30 MHz request" -- true on
	// 7SEG and wrong on OLED, where boot asks for 10 MHz and gets 3. It restores what was
	// actually captured now.
	// Cleared first, so a handover landing in an interrupt from here on finds no stream to
	// move and leaves the bus alone -- and the release stays inside the same interrupts-off
	// window, so nothing can claim the block between the decision and the act.
	CriticalSectionGuard guard;
	const bool releaseNow = cvStreamRunning && !cvStreamYielded;
	cvStatStops = cvStatStops + 1;
	cvStreamRunning = false;
	cvStreamYielded = false;
	cvStreamResyncPending = false;

	if (releaseNow) {
		cvStreamReleaseBus();
	}
}

uint32_t cvStreamStat(CvStat which) {
	switch (which) {
	case CvStat::Starts:
		return cvStatStarts;
	case CvStat::Stops:
		return cvStatStops;
	case CvStat::Yields:
		return cvStatYields;
	case CvStat::TakeBacks:
		return cvStatTakeBacks;
	case CvStat::PumpWritten:
		return cvStatPumpWritten;
	case CvStat::PumpDropped:
		return cvStatPumpDropped;
	case CvStat::Resyncs:
		return cvStatResyncs;
	case CvStat::DmaStalls:
		return cvStatDmaStalls;
	case CvStat::YieldedNow:
		return cvStreamYielded ? 1u : 0u;
	case CvStat::AdvancePerWindowX10:
		return (cvStatPumpWritten == 0) ? 0u : (cvStatAdvanceTotal * 10u) / cvStatPumpWritten;
	case CvStat::EmitPerWindowX10:
		return (cvStatPumpWritten == 0) ? 0u : (cvStatEmitTotal * 10u) / cvStatPumpWritten;
	case CvStat::SpiStatusBits:
		return cvStatSpiStatus;
	case CvStat::LeadNow:
		return cvStatLeadNow;
	case CvStat::DmaChannelStatus:
		return cvStatChanStatus;
	case CvStat::OledChannelStatus:
		return cvStatOledChanStatus;
	case CvStat::SpiStatusLive:
		return cvStatSpiLive;
	case CvStat::TendSeen:
		return cvStatTendSeen;
	}
	return 0;
}

} // namespace deluge::processing::engines

// The C-callable half of the handover. Thin on purpose: the arbitration decision belongs to
// oled_low_level.c, which is the only place that knows when the shared bus is genuinely
// idle, and the mechanism belongs here.
extern "C" void cvStreamYieldBegin(void) {
	deluge::processing::engines::cvStreamYieldBusToDisplay();
}

extern "C" void cvStreamYieldEnd(void) {
	deluge::processing::engines::cvStreamTakeBusBack();
}
