/*
 * Copyright (c) 2025 Bruce Zawadzki (Tone Coder)
 * Copyright (c) 2026 Rudement
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

#include "visualizer_bar_spectrum.h"
#include "RZA1/cpu_specific.h"
#include "definitions_cxx.hpp"
#include "hid/display/oled.h"
#include "visualizer_common.h"
#include "visualizer_fft.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace deluge::hid::display {

namespace {

constexpr int32_t kNumBars = 16;

/// Sixteen third-octave centres, the usual graphic-EQ set.
constexpr std::array<float, kNumBars> kBandCentres = {31.0f,   50.0f,    80.0f,    125.0f,  200.0f,  315.0f,
                                                      500.0f,  800.0f,   1250.0f,  2000.0f, 3150.0f, 5000.0f,
                                                      8000.0f, 12500.0f, 16000.0f, 20000.0f};

/// What a bin magnitude has to reach for a bar to stand at full height. Tuned on hardware upstream
/// against the windowing in visualizer_fft.cpp, so the two go together.
constexpr float kReferenceMagnitude = 60000000.0f;

/// Upstream's compression exponent is 0.5, which is a square root and is taken as one below.
constexpr float kFrequencyBoostExponent = 0.05f;
constexpr float kFrequencyNormalisationHz = 1000.0f;
constexpr float kAmplitudeBoost = 1.1f;

/// First-order IIR on each bar. Higher alpha is steadier and lazier.
constexpr float kSmoothingAlpha = 0.75f;
constexpr float kSmoothingBeta = 0.25f;

/// Peak hold falls by the square of an accumulating rate, so it hangs then drops.
constexpr float kPeakDecayRate = 0.005f;

std::array<float, kNumBars> smoothed{};
std::array<float, kNumBars> peak_heights{};
std::array<float, kNumBars> peak_decay{};

/// The frequency term of the compression is constant per band, so it is computed once instead of
/// being a powf per bar per frame.
std::array<float, kNumBars> frequency_boost{};
bool frequency_boost_ready = false;

void initFrequencyBoost() {
	if (frequency_boost_ready) {
		return;
	}
	frequency_boost_ready = true;
	for (int32_t bar = 0; bar < kNumBars; bar++) {
		frequency_boost[bar] =
		    std::pow(kBandCentres[bar] / kFrequencyNormalisationHz, kFrequencyBoostExponent) * kAmplitudeBoost;
	}
}

/// Each band runs to the midpoint of its neighbours; the end bands run out to 20 Hz and 20 kHz.
void bandRange(int32_t bar, float& lower_hz, float& upper_hz) {
	float centre = kBandCentres[bar];
	if (bar == 0) {
		lower_hz = 20.0f;
		upper_hz = (centre + kBandCentres[bar + 1]) * 0.5f;
	}
	else if (bar == kNumBars - 1) {
		lower_hz = (kBandCentres[bar - 1] + centre) * 0.5f;
		upper_hz = 20000.0f;
	}
	else {
		lower_hz = (kBandCentres[bar - 1] + centre) * 0.5f;
		upper_hz = (centre + kBandCentres[bar + 1]) * 0.5f;
	}
}

// Geometry. 16 bars of 5 px with 2 px between them leaves a 7 px margin each side:
// 9 + 15*7 + 5 = 119, inside the 125 px right edge.
constexpr int32_t kDisplayHeightPixels = OLED_MAIN_HEIGHT_PIXELS - OLED_MAIN_TOPMOST_PIXEL;
constexpr int32_t kGraphMinX = kDisplayMargin;
constexpr int32_t kGraphMaxX = OLED_MAIN_WIDTH_PIXELS - kDisplayMargin - 1;
constexpr int32_t kGraphMinY = OLED_MAIN_TOPMOST_PIXEL + kDisplayMargin;
constexpr int32_t kGraphMaxY = OLED_MAIN_TOPMOST_PIXEL + kDisplayHeightPixels - kDisplayMargin - 1;
constexpr int32_t kGraphHeight = kDisplayHeightPixels - (kDisplayMargin * 2);

constexpr int32_t kBarWidth = 5;
constexpr int32_t kBarGap = 2;
constexpr int32_t kBarsMargin = 7;
constexpr int32_t kBarsStartX = kGraphMinX + kBarsMargin;

static_assert(kBarsStartX + (kNumBars - 1) * (kBarWidth + kBarGap) + kBarWidth - 1 <= kGraphMaxX,
              "the bars do not fit between the margins");
static_assert(kGraphMaxY < OLED_MAIN_HEIGHT_PIXELS, "the graph runs off the bottom of the screen");

/// Fills one column of a bar straight into the canvas's byte rows.
///
/// The obvious way is a drawPixel per lit pixel, which upstream does: 16 bars of 5 by 39 is up to
/// 3,440 bounds-checked read-modify-writes a frame, and it was the single most expensive thing in
/// the feature. The canvas stores eight vertical pixels per byte, so a column is at most six masked
/// ORs instead of 39 calls. The checkerboard dither comes out of the parity mask for free.
void fillBarColumn(oled_canvas::Canvas::ImageStore& image, int32_t x, int32_t top_y, int32_t bottom_y) {
	if (top_y > bottom_y) {
		return;
	}
	// Upstream lights a pixel when (x + y) is even, which for a given column is every other row.
	const uint8_t parity_mask = ((x & 1) != 0) ? 0xAA : 0x55;

	const int32_t first_row = top_y >> 3;
	const int32_t last_row = bottom_y >> 3;
	for (int32_t row = first_row; row <= last_row; row++) {
		const int32_t row_top = row << 3;
		const int32_t lo = std::max(top_y, row_top) & 0x7;
		const int32_t hi = std::min(bottom_y, row_top + 7) & 0x7;
		const auto span_mask = static_cast<uint8_t>((0xFF << lo) & (0xFF >> (7 - hi)));
		image[row][x] |= span_mask & parity_mask;
	}
}

void updateAndDrawPeak(oled_canvas::Canvas& canvas, int32_t bar, float normalised_height, int32_t left_x,
                       int32_t right_x) {
	normalised_height = std::min(1.0f, normalised_height);

	if (normalised_height > peak_heights[bar]) {
		peak_heights[bar] = normalised_height;
		peak_decay[bar] = 0.0f;
	}
	else {
		peak_decay[bar] += kPeakDecayRate;
		peak_heights[bar] = std::max(normalised_height, peak_heights[bar] - (peak_decay[bar] * peak_decay[bar]));
	}

	if (peak_heights[bar] <= 0.0f) {
		return;
	}

	auto peak_pixels = static_cast<int32_t>(peak_heights[bar] * static_cast<float>(kGraphHeight));
	int32_t peak_y = std::clamp(kGraphMaxY - peak_pixels, kGraphMinY, kGraphMaxY);

	// Two pixels thick, so it reads as a marker rather than as part of the bar.
	for (int32_t thickness = 0; thickness <= 1; thickness++) {
		int32_t y = peak_y + thickness;
		if (y >= kGraphMinY && y <= kGraphMaxY) {
			canvas.drawHorizontalLine(y, left_x, right_x);
		}
	}
}

} // namespace

void resetBarSpectrumState() {
	smoothed.fill(0.0f);
	peak_heights.fill(0.0f);
	peak_decay.fill(0.0f);
}

void renderVisualizerBarSpectrum(oled_canvas::Canvas& canvas) {
	FFTResult fft = computeVisualizerFFT();
	if (!fft.isValid || fft.isSilent) {
		// Not enough audio captured yet, or a brief gap between notes. Either way the previous
		// frame standing is better than a blank box.
		return;
	}

	initFrequencyBoost();

	canvas.clearAreaExact(kGraphMinX, kGraphMinY, kGraphMaxX, kGraphMaxY + 1);
	oled_canvas::Canvas::ImageStore& image = canvas.hackGetImageStore();

	constexpr float kBinHz = static_cast<float>(kSampleRate) / static_cast<float>(kVisualizerFFTSize);

	for (int32_t bar = 0; bar < kNumBars; bar++) {
		float lower_hz = 0.0f;
		float upper_hz = 0.0f;
		bandRange(bar, lower_hz, upper_hz);

		float magnitude = calculateWeightedMagnitude(fft, lower_hz, upper_hz, kBinHz);
		float amplitude = std::clamp(magnitude / kReferenceMagnitude, 0.0f, 1.0f);

		// sqrtf rather than powf(x, 0.5f): the same number, a fraction of the cost, sixteen a frame.
		float value = std::sqrt(amplitude) * frequency_boost[bar];

		smoothed[bar] = (smoothed[bar] * kSmoothingAlpha) + (value * kSmoothingBeta);
		value = std::clamp(smoothed[bar], 0.0f, 1.0f);

		auto height = std::min(static_cast<int32_t>(value * static_cast<float>(kGraphHeight)), kGraphHeight);

		int32_t left_x = kBarsStartX + (bar * (kBarWidth + kBarGap));
		int32_t right_x = left_x + kBarWidth - 1;
		int32_t top_y = std::clamp(kGraphMaxY - height, kGraphMinY, kGraphMaxY);

		for (int32_t x = left_x; x <= right_x; x++) {
			fillBarColumn(image, x, top_y, kGraphMaxY);
		}

		updateAndDrawPeak(canvas, bar, static_cast<float>(height) / static_cast<float>(kGraphHeight), left_x, right_x);
	}

	OLED::markChanged();
}

} // namespace deluge::hid::display
