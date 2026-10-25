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
#pragma once

#include <cstdint>

/// SETTINGS > CPU -- what the audio engine is actually costing, on the device.
///
/// WHY THIS EXISTS. Every CPU number in this fork is either a ratio measured on a desktop host
/// (PLAITS-CLOUDS-CPU-MEASURED.md) or an operation count read off the source
/// (CPU-LOAD-RANKING.md). Neither says what fraction of the Deluge's budget anything takes, how
/// many Plaits voices fit before the engine culls, or whether Sear's per-block divide matters.
/// Those are absolutes, and absolutes need the hardware.
///
/// WHAT IT DELIBERATELY IS NOT. There are no per-stage timers here -- nothing brackets Plaits, or
/// Clouds, or Sear individually. Timing individual stages means reading a counter twice per voice
/// per block, which is real cost in the one place cost is being measured, and it needs a hook in
/// every DSP call site. This page instead exposes the numbers the engine ALREADY computes for its
/// own culling decisions, and you get per-feature costs by difference: play the same part twice,
/// once with the feature on and once off, and read DSP both times. The instrument cannot
/// meaningfully perturb what it measures, which is the whole point -- see the cost note on
/// cpuStatsNoteRender().
///
/// HOW TO READ IT. `DSP` is the headline. It is how many samples' worth of CPU one render call
/// costs, against a window that is at most 128 samples and a cull threshold (`numSamplesLimit` in
/// audio_engine.cpp) of 80. At DSP 20 you have three quarters of the machine spare; at DSP 75 the
/// next voice starts culling. `HARD` and `SOFT` are the failure events themselves.
///
/// Counters run from boot or from the last `RESET`. None of them are synchronised: they are
/// written from the audio path and read from the menu, so a value can be one render behind. That
/// is the right trade for a diagnostic -- locking the audio path to read a number would change
/// the thing being measured. The same reasoning, and the same wording, as CvStat.
namespace deluge::processing::engines {

enum class CpuStat : uint8_t {
	/// Audio-task CPU as a percentage of wall time since reset, x10 -- so 873 means 87.3%. This is
	/// a duty cycle over everything the audio task does, and it is the one number that answers
	/// "how loaded is this song". It counts only renders the scheduler counted: very short windows
	/// call ignoreForStats() and are not in here.
	LoadPercentX10,
	/// Mean cost of one render call, in samples. Compare against 80 (`numSamplesLimit`, where
	/// culling starts) and 128 (a full window). THE headline number.
	DspSamples,
	/// Worst single render call since reset, in samples. Read from the scheduler's own duration
	/// stats rather than accumulated here, so it is a true maximum and not a sampled one.
	///
	/// There is no matching minimum, deliberately: StatBlock::min in task.h initialises to
	/// Time(std::numeric_limits<Time>::infinity()), and numeric_limits of a non-arithmetic type
	/// returns a value-initialised Time -- i.e. zero. So min is pinned at 0 and means nothing.
	/// The maximum starts at 0 and grows, which is correct.
	PeakDspSamples,
	/// Mean DMA advance per render call, in samples: how far the output buffer drained while the
	/// engine was busy elsewhere. Rises with load, independently of DspSamples, and is the number
	/// that actually drives the engine's window sizing.
	WindowSamples,
	/// Largest DMA advance seen since reset.
	PeakWindowSamples,
	/// cpuDireness right now, 0-14. Nonzero means the engine has started making load decisions.
	Direness,
	/// Highest direness seen since reset. Nonzero after a quiet passage means something spiked.
	PeakDireness,
	/// Live voice count, read at the moment the menu draws. Not a peak -- for "how many voices
	/// fit", dial in a number you chose and watch HARD and SOFT.
	Voices,
	/// Voices soft-culled (forced into release) since reset. Audible as a part fading early.
	SoftCulls,
	/// Voices hard-culled (terminated or killed outright) since reset. Audible as a click or a
	/// note vanishing. **If this is not zero, the song is over budget.**
	HardCulls,
	/// Render calls counted since reset. The denominator for everything above; also a quick check
	/// that the page is live.
	Renders,
};

/// Reads one counter. See the note on synchronisation above.
uint32_t cpuStat(CpuStat which);

/// Zeroes every counter and peak, and restarts the load window. Also resets the task scheduler's
/// own duration statistics, which is where PeakDspSamples comes from.
///
/// Call this at the start of each A/B trial: the numbers are otherwise lifetime-since-boot, and a
/// peak from a song you loaded twenty minutes ago will sit at the top of the page forever.
void cpuStatsReset();

/// Once per render call, from setDireness().
///
/// COST. Two 64-bit adds, two compares, one increment. Against a render window whose budget is
/// ~9,000 core cycles per sample, this is unmeasurable -- which is why it is unconditional rather
/// than behind a build flag or a runtime toggle. A toggle would need its own branch here anyway,
/// and would make the page lie by default.
void cpuStatsNoteRender(uint32_t windowSamples, int32_t dspSamples, int32_t direness);

/// From cullVoices(), which only runs when direness is already high -- so this is never on the
/// quiet path. `hard` counts terminate and kill, `soft` counts forced releases.
void cpuStatsNoteCulls(uint32_t hard, uint32_t soft);

} // namespace deluge::processing::engines
