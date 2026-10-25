/*
 * Copyright © 2026 Rudement
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

#include "processing/engines/cpu_stats.h"

#include "definitions_cxx.hpp"
#include "processing/engines/audio_engine.h"
#include "scheduler_api.h"

namespace deluge::processing::engines {

namespace {

/// Everything accumulated from the audio path. Plain scalars, no locking -- see the header.
///
/// 64-bit sums rather than 32: renders happen as often as several thousand a second when windows
/// are short, so a 32-bit sum of window lengths wraps in well under a day of playing. A pair of
/// 64-bit adds per render is still nothing.
uint64_t windowSum = 0;
uint64_t dspSum = 0;
uint64_t renderCount = 0;
uint32_t windowPeak = 0;
int32_t direnessPeak = 0;
uint32_t hardCulls = 0;
uint32_t softCulls = 0;

/// Wall time, in seconds, at the last reset. The load percentage is task CPU since then over wall
/// time since then, so it is a duty cycle over the window you chose rather than over all of boot.
double loadWindowStart = 0.0;

/// Task CPU already spent when the window opened. resetTaskStats() zeroes the scheduler's
/// accumulator, so this is 0 in practice -- it is kept explicit so the arithmetic still reads
/// correctly if the reset ever becomes a soft one.
double loadTaskTimeAtStart = 0.0;

constexpr uint32_t kDisplayModulus = 1000000;

/// Clamp into the range an Integer menu item can show, matching what CvStat's readout does.
[[nodiscard]] uint32_t forDisplay(uint64_t v) {
	return static_cast<uint32_t>(v % kDisplayModulus);
}

[[nodiscard]] double audioTaskTotalSeconds() {
	return getTotalRunTimeForTask(AudioEngine::routine_task_id);
}

} // namespace

void cpuStatsReset() {
	windowSum = 0;
	dspSum = 0;
	renderCount = 0;
	windowPeak = 0;
	direnessPeak = 0;
	hardCulls = 0;
	softCulls = 0;

	// Order matters: clear the scheduler first, then sample the clock, so the load window cannot
	// start before the CPU accumulator it is divided against.
	resetTaskStats();
	loadTaskTimeAtStart = audioTaskTotalSeconds();
	loadWindowStart = getSystemTime();
}

void cpuStatsNoteRender(uint32_t windowSamples, int32_t dspSamples, int32_t direness) {
	windowSum += windowSamples;
	if (windowSamples > windowPeak) {
		windowPeak = windowSamples;
	}
	// dspSamples comes from a running average that can read slightly negative early in boot, before
	// the scheduler has a duration for the audio task at all. Clamping here rather than at the read
	// keeps the mean from being dragged below zero by the first few renders after a cold start.
	if (dspSamples > 0) {
		dspSum += static_cast<uint64_t>(dspSamples);
	}
	if (direness > direnessPeak) {
		direnessPeak = direness;
	}
	renderCount++;
}

void cpuStatsNoteCulls(uint32_t hard, uint32_t soft) {
	hardCulls += hard;
	softCulls += soft;
}

uint32_t cpuStat(CpuStat which) {
	switch (which) {

	case CpuStat::LoadPercentX10: {
		const double elapsed = getSystemTime() - loadWindowStart;
		if (elapsed <= 0.0) {
			return 0;
		}
		const double busy = audioTaskTotalSeconds() - loadTaskTimeAtStart;
		const double pct = (busy / elapsed) * 1000.0;
		if (pct <= 0.0) {
			return 0;
		}
		// Over 100% is not impossible here and is not a bug worth hiding: the audio task can be
		// re-entered through routineWithClusterLoading(), so its accumulated time can exceed wall
		// time. Seeing 1100 is itself information -- it means the engine is spending more than the
		// whole clock inside nested renders.
		return static_cast<uint32_t>(pct) % kDisplayModulus;
	}

	case CpuStat::DspSamples:
		return renderCount ? forDisplay(dspSum / renderCount) : 0;

	case CpuStat::PeakDspSamples: {
		const double worst = getMaxRunTimeForTask(AudioEngine::routine_task_id);
		if (worst <= 0.0) {
			return 0;
		}
		return static_cast<uint32_t>(worst * static_cast<double>(kSampleRate)) % kDisplayModulus;
	}

	case CpuStat::WindowSamples:
		return renderCount ? forDisplay(windowSum / renderCount) : 0;

	case CpuStat::PeakWindowSamples:
		return windowPeak;

	case CpuStat::Direness:
		return static_cast<uint32_t>(AudioEngine::cpuDireness < 0 ? 0 : AudioEngine::cpuDireness);

	case CpuStat::PeakDireness:
		return static_cast<uint32_t>(direnessPeak < 0 ? 0 : direnessPeak);

	case CpuStat::Voices: {
		const int32_t n = AudioEngine::getNumVoices();
		return static_cast<uint32_t>(n < 0 ? 0 : n);
	}

	case CpuStat::SoftCulls:
		return softCulls % kDisplayModulus;

	case CpuStat::HardCulls:
		return hardCulls % kDisplayModulus;

	case CpuStat::Renders:
		return forDisplay(renderCount);
	}
	return 0;
}

} // namespace deluge::processing::engines
