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

#include "definitions_cxx.hpp"
#include "dsp/convolution/impulse_response_processor.h"
#include "dsp/delay/delay_buffer.h"
#include "model/sync.h"

#include <cstdint>
#include <span>

class Delay {
public:
	struct State {
		bool doDelay;
		int32_t userDelayRate;
		int32_t delayFeedbackAmount;
		int32_t analog_saturation = 8;
		/// Set by setupWorkingState() while a tail is ringing out to a setting that has the delay off:
		/// what is already in the buffer keeps repeating, but nothing new is fed in. See beginTail().
		bool muteInput = false;
	};

	Delay() = default;
	Delay(const Delay& other) = delete;

	// was originally cloneFrom
	Delay& operator=(const Delay& rhs) {
		pingPong = rhs.pingPong;
		analog = rhs.analog;
		syncLevel = rhs.syncLevel;
		setHPF(rhs.hpf_);
		setLPF(rhs.lpf_);
		syncType = rhs.syncType;
		return *this;
	}

	// Rudement: user high- and low-pass filters on the delay, inside the feedback loop, so each repeat loses a
	// little more low (or high) end. 12 dB/oct, no resonance. Stored 0..1 (menu value / 50).
	// HPF: 0 is off; above that it sweeps 20 Hz -> 1 kHz. LPF: 1 is off; below that it sweeps 16 kHz -> 300 Hz.
	// Gated by Community Features > Rudement upgrades > Delay Filters: with that Off, or both filters at off, they
	// are skipped entirely and the delay is exactly the stock one.
	void setHPF(float f);
	void setLPF(float f);
	[[nodiscard]] constexpr float getHPF() const { return hpf_; }
	[[nodiscard]] constexpr float getLPF() const { return lpf_; }
	// Cutoff in Hz for a stored 0..1 value, for the menu readout. Only meaningful when the filter is not off.
	static float hpfHz(float f);
	static float lpfHz(float f);

	[[nodiscard]] constexpr bool isActive() const { return (primaryBuffer.isActive() || secondaryBuffer.isActive()); }

	void informWhetherActive(bool newActive, int32_t userDelayRate = 0);
	void copySecondaryToPrimary();
	void copyPrimaryToSecondary();
	void initializeSecondaryBuffer(int32_t newNativeRate, bool makeNativeRatePreciseRelativeToOtherBuffer);
	void setupWorkingState(State& workingState, uint32_t timePerInternalTickInverse, bool anySoundComingIn = true);

	/// Temp Hold tails (2026-10-05). Called just before a hold puts the delay amount back. Without it,
	/// a delay turned up during the hold and restored to off is discarded on the spot - its buffer
	/// thrown away mid-echo - and one restored to a lower amount snaps to the shorter decay.
	///
	/// With it, the feedback the hold left behind becomes a ceiling that glides down to the restored
	/// amount (time constant ~1.5 s, kTailGlideShift), so the echoes die away rather than stopping.
	/// If the restored amount is "off", the dry signal stops feeding the delay at once - only what was
	/// already in it rings out - and the buffer is discarded once the glide has brought it to silence.
	/// Turning the amount back up past the tail ends it straight away.
	void beginTail();
	void discardBuffers();
	void setTimeToAbandon(const State& workingState);
	void hasWrapped();

	DelayBuffer primaryBuffer;
	DelayBuffer secondaryBuffer;
	ImpulseResponseProcessor ir_processor;

	uint32_t countCyclesWithoutChange;
	int32_t userRateLastTime;
	bool pingPong = true;
	bool analog = false;

	SyncType syncType = SYNC_TYPE_EVEN;

	// Basically, 0 is off, max value is 9. Higher numbers are shorter intervals (higher speed).
	SyncLevel syncLevel = SYNC_LEVEL_16TH;

	int32_t sizeLeftUntilBufferSwap;

	int32_t postLPFL;
	int32_t postLPFR;

	int32_t prevFeedback = 0;

	/// Temp Hold tails - see beginTail().
	bool tailing = false;
	int32_t tailFeedback = 0;
	int32_t tailTarget = 0;
	static constexpr int32_t kTailGlideShift = 16; // 2^16 samples, ~1.5 s at 44.1 kHz

	uint8_t repeatsUntilAbandon = 0; // 0 means never abandon

	void process(std::span<StereoSample> buffer, const State& delayWorkingState);

private:
	// Two-pole state-variable filter (Simper/Cytomic TPT form): stable while the cutoff is being turned.
	struct Svf {
		float a1 = 0.f, a2 = 0.f, a3 = 0.f;
		void setup(float hz);
	};
	struct SvfState {
		float ic1 = 0.f, ic2 = 0.f;
		void reset() { ic1 = ic2 = 0.f; }
		// Returns the low-pass output; the high-pass output is written to *hp when not null.
		float tick(const Svf& c, float in, float* hp) {
			const float v3 = in - ic2;
			const float v1 = c.a1 * ic1 + c.a2 * v3;
			const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
			ic1 = 2.f * v1 - ic1;
			ic2 = 2.f * v2 - ic2;
			if (hp != nullptr) {
				*hp = in - kSvfK * v1 - v2;
			}
			return v2;
		}
	};
	static constexpr float kSvfK = 1.41421356f; // 1/Q, Q = 0.707: Butterworth, no resonance

	void resetUserFilters() {
		hpL_.reset();
		hpR_.reset();
		lpL_.reset();
		lpR_.reset();
	}

	float hpf_ = 0.f;
	float lpf_ = 1.f;
	Svf hpCoef_;
	Svf lpCoef_;
	SvfState hpL_, hpR_, lpL_, lpR_;
	/// Whether the Community Features switch was On last block, so the filter memory starts clean when it flips.
	bool userFiltersWereOn_ = false;

	void prepareToBeginWriting();
	[[nodiscard]] constexpr int32_t getAmountToWriteBeforeReadingBegins() const { return secondaryBuffer.size(); }
};
