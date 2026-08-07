/*
 * Copyright (c) 2025 Leonid Burygin
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
#include "gui/menu_item/horizontal_menu.h"
#include "hid/display/oled.h"
#include "hid/display/oled_canvas/canvas.h"

#include <util/comparison.h>

using namespace deluge::hid::display;

namespace deluge::gui::menu_item::eq {
class EqMenu final : public HorizontalMenu {
public:
	EqMenu(l10n::String newName, std::initializer_list<MenuItem*> newItems) : HorizontalMenu(newName, newItems) {}

	void renderMenuItems(std::span<MenuItem*> /*visible_items*/, const MenuItem* currentItem) override {
		// NOTE: with 8 items this menu paginates, so the span passed in is only the current page —
		// never index it directly. The graph is always drawn from the full (reordered) item list.
		const auto [bass, treble, bass_freq, treble_freq, low_mid, low_mid_freq, high_mid, high_mid_freq,
		            order_changed] = ensureCorrectItemsOrderAndGetValues();
		if (order_changed) {
			renderOLED();
			return;
		}

		constexpr uint8_t start_y = OLED_MAIN_TOPMOST_PIXEL + kTextTitleSizeY + 5;
		constexpr uint8_t end_y = OLED_MAIN_HEIGHT_PIXELS - 6;
		constexpr uint8_t center_y = start_y + (end_y - start_y) / 2;
		constexpr uint8_t height = end_y - start_y;

		constexpr uint8_t padding_x = 4;
		constexpr uint8_t start_x = padding_x - 1;
		constexpr uint8_t end_x = OLED_MAIN_WIDTH_PIXELS - padding_x;
		constexpr uint8_t slope_width = 12;
		// Half as wide as the single-bell version used, so two bells fit side by side between the
		// shelf corners without their skirts swallowing each other.
		constexpr uint8_t bell_half_width = slope_width / 2;
		constexpr uint8_t bass_band_travel_width = (end_x - start_x) / 2 - slope_width;
		constexpr uint8_t treble_band_travel_width = (end_x - start_x) * 0.75f;

		constexpr uint8_t bass_x0 = start_x;
		uint8_t bass_x1 = std::lerp(bass_x0, bass_x0 + bass_band_travel_width, bass_freq);
		uint8_t bass_x2 = bass_x1 + slope_width;
		uint8_t bass_y1 = std::lerp(end_y, end_y - height, bass);
		uint8_t bass_y2 = center_y;

		constexpr uint8_t treble_x0 = end_x;
		uint8_t treble_x1 = std::lerp(end_x - treble_band_travel_width, end_x, treble_freq);
		uint8_t treble_x2 = treble_x1 - slope_width;
		uint8_t treble_y1 = std::lerp(end_y, end_y - height, treble);
		uint8_t treble_y2 = center_y;

		// Treble EQ also can affect mid & bass frequencies, and it has higher priority,
		// so we allow to move the treble band to the bass' territory
		if (bass_x2 > treble_x2) {
			const uint8_t diff = bass_x2 - treble_x2;
			bass_x2 -= diff;
			bass_x1 -= diff;
		}

		// If bass freq and treble freq adjustment points are close to each other, smoothly adjust their y positions
		// to morph between a slope line and a straight line
		auto center_between = [](uint8_t a, uint8_t b) { return std::min(a, b) + std::abs(b - a) / 2; };
		const float morph = 1.0f - (treble_x2 - bass_x2) / 14.f;
		if (morph > 0.f) {
			const uint8_t target_y = center_between(bass_y1, treble_y1);
			bass_y2 = std::lerp(bass_y2, target_y, morph);
			treble_y2 = std::lerp(treble_y2, target_y, morph);
		}

		auto y_on_center_segment = [&](uint8_t x) -> uint8_t {
			if (treble_x2 <= bass_x2) {
				return center_between(bass_y2, treble_y2);
			}
			return std::lerp(bass_y2, treble_y2, (x - bass_x2) / static_cast<float>(treble_x2 - bass_x2));
		};

		// The two mid bells, drawn on the segment between the two shelf corners. Their frequency
		// sweeps overlap (the low mid reaches ~7.5 kHz, the high mid starts at ~900 Hz), so the
		// high mid can legitimately end up to the LEFT of the low mid on screen. Everything below
		// is therefore written in terms of "the left bell" and "the right bell" rather than
		// low/high, and the polyline is walked in x order. Drawing them in param order instead
		// would make the trace double back on itself whenever they cross.
		auto bell_x = [&](float freq) {
			uint8_t x = std::lerp(
			    static_cast<float>(bass_x2 + bell_half_width),
			    static_cast<float>(std::max<int32_t>(treble_x2 - bell_half_width, bass_x2 + bell_half_width)), freq);
			return std::clamp<uint8_t>(x, bass_x2, treble_x2);
		};
		low_mid_x_ = bell_x(low_mid_freq);
		high_mid_x_ = bell_x(high_mid_freq);
		low_mid_y_ = std::lerp(end_y, end_y - height, low_mid);
		high_mid_y_ = std::lerp(end_y, end_y - height, high_mid);

		const bool low_is_left = low_mid_x_ <= high_mid_x_;
		const uint8_t left_x = low_is_left ? low_mid_x_ : high_mid_x_;
		const uint8_t right_x = low_is_left ? high_mid_x_ : low_mid_x_;
		const uint8_t left_y = low_is_left ? low_mid_y_ : high_mid_y_;
		const uint8_t right_y = low_is_left ? high_mid_y_ : low_mid_y_;

		// Each bell's skirt is clipped at the midpoint between the two centres, so the skirts meet
		// rather than overlapping. When the two centres coincide the midpoint collapses onto both
		// and the shared edge degenerates to a point, which drawLine handles.
		const uint8_t between = center_between(left_x, right_x);
		const uint8_t left_xl = std::max<int32_t>(left_x - bell_half_width, bass_x2);
		const uint8_t left_xr = std::clamp<int32_t>(left_x + bell_half_width, left_x, between);
		const uint8_t right_xl = std::clamp<int32_t>(right_x - bell_half_width, between, right_x);
		const uint8_t right_xr = std::min<int32_t>(right_x + bell_half_width, treble_x2);

		oled_canvas::Canvas& image = OLED::main;
		image.drawLine(bass_x0, bass_y1, bass_x1, bass_y1);
		image.drawLine(bass_x1, bass_y1, bass_x2, bass_y2);
		image.drawLine(bass_x2, bass_y2, left_xl, y_on_center_segment(left_xl));
		image.drawLine(left_xl, y_on_center_segment(left_xl), left_x, left_y);
		image.drawLine(left_x, left_y, left_xr, y_on_center_segment(left_xr));
		image.drawLine(left_xr, y_on_center_segment(left_xr), right_xl, y_on_center_segment(right_xl));
		image.drawLine(right_xl, y_on_center_segment(right_xl), right_x, right_y);
		image.drawLine(right_x, right_y, right_xr, y_on_center_segment(right_xr));
		image.drawLine(right_xr, y_on_center_segment(right_xr), treble_x2, treble_y2);
		image.drawLine(treble_x2, treble_y2, treble_x1, treble_y1);
		image.drawLine(treble_x1, treble_y1, treble_x0, treble_y1);

		// Draw reference lines
		if (std::abs(center_y - bass_y1) > 1) {
			for (uint8_t x = 0; x <= bass_x2; x++) {
				if (x % 6 == 3 && std::abs(x - bass_x2) > 1 && std::abs(x - treble_x2) > 1) {
					image.drawPixel(x, center_y);
				}
			}
		}
		if (std::abs(center_y - treble_y1) > 1) {
			for (uint8_t x = 0; x <= end_x; x++) {
				if (x % 6 == 3 && std::abs(x - bass_x2) > 1 && std::abs(x - treble_x2) > 1) {
					image.drawPixel(x, center_y);
				}
			}
		}
		for (uint8_t y = start_y - 1; y <= end_y + 1; y += 4) {
			image.drawPixel(bass_x2, y);
			image.drawPixel(treble_x2, y);
		}

		// Draw control indicators. Compare against the reordered full item list, never the page span.
		selected_x_ = -1, selected_y_ = -1;
		drawControlIndicator(center_between(bass_x0, bass_x1), bass_y1, currentItem == ordered_items_[0]);
		drawControlIndicator(bass_x2, bass_y2, currentItem == ordered_items_[1]);
		drawControlIndicator(treble_x2, treble_y2, currentItem == ordered_items_[2]);
		drawControlIndicator(center_between(treble_x1, treble_x0), treble_y1, currentItem == ordered_items_[3]);
		drawControlIndicator(low_mid_x_, low_mid_y_, currentItem == ordered_items_[4]);
		drawControlIndicator(low_mid_x_, y_on_center_segment(low_mid_x_), currentItem == ordered_items_[5]);
		drawControlIndicator(high_mid_x_, high_mid_y_, currentItem == ordered_items_[6]);
		drawControlIndicator(high_mid_x_, y_on_center_segment(high_mid_x_), currentItem == ordered_items_[7]);
	}

private:
	int32_t selected_x_, selected_y_;
	// Bell positions, computed in renderMenuItems() and reused by the control indicators below it.
	uint8_t low_mid_x_{}, low_mid_y_{}, high_mid_x_{}, high_mid_y_{};

	static constexpr size_t kNumEqItems = 8;

	struct EqualizerValues {
		float bass{0.f};
		float treble{0.f};
		float bass_freq{0.f};
		float treble_freq{0.f};
		float low_mid{0.5f};
		float low_mid_freq{0.5f};
		float high_mid{0.5f};
		float high_mid_freq{0.5f};
		bool order_changed{false};
	};

	// Items in display order: bass, bass freq, treble freq, treble (page 1 — same as before any mid
	// band existed), then low mid, low mid freq, high mid, high mid freq (page 2). Filled by
	// ensureCorrectItemsOrderAndGetValues(). Pagination is by slot count, 4 per page, so 8
	// single-slot items give exactly two pages and page 1 still renders as stock.
	UnpatchedParam* ordered_items_[kNumEqItems] = {};

	EqualizerValues ensureCorrectItemsOrderAndGetValues() {
		using namespace deluge::modulation;

		const uint8_t current_item_pos = std::distance(items.begin(), current_item_);
		UnpatchedParam** desired_order_items = ordered_items_;
		for (size_t idx = 0; idx < kNumEqItems; ++idx) {
			desired_order_items[idx] = nullptr;
		}
		EqualizerValues result{};

		for (auto* i : items) {
			switch (const auto as_unpatched = static_cast<UnpatchedParam*>(i); as_unpatched->getP()) {
			case params::UNPATCHED_BASS:
				desired_order_items[0] = as_unpatched;
				result.bass = as_unpatched->getValue() / 50.f;
				break;
			case params::UNPATCHED_BASS_FREQ:
				desired_order_items[1] = as_unpatched;
				result.bass_freq = as_unpatched->getValue() / 50.f;
				break;
			case params::UNPATCHED_TREBLE_FREQ:
				desired_order_items[2] = as_unpatched;
				// Treble boost has no effect on treble freq's values above 32
				result.treble_freq = std::clamp<int32_t>(as_unpatched->getValue(), 0, 32) / 32.f;
				break;
			case params::UNPATCHED_TREBLE:
				desired_order_items[3] = as_unpatched;
				result.treble = as_unpatched->getValue() / 50.f;
				break;
			case params::UNPATCHED_LOW_MID:
				desired_order_items[4] = as_unpatched;
				result.low_mid = as_unpatched->getValue() / 50.f;
				break;
			case params::UNPATCHED_LOW_MID_FREQ:
				desired_order_items[5] = as_unpatched;
				result.low_mid_freq = as_unpatched->getValue() / 50.f;
				break;
			case params::UNPATCHED_HIGH_MID:
				desired_order_items[6] = as_unpatched;
				result.high_mid = as_unpatched->getValue() / 50.f;
				break;
			case params::UNPATCHED_HIGH_MID_FREQ:
				desired_order_items[7] = as_unpatched;
				result.high_mid_freq = as_unpatched->getValue() / 50.f;
				break;
			default:
				break;
			}
		}

		for (size_t idx = 0; idx < items.size() && idx < kNumEqItems; ++idx) {
			if (items[idx] != desired_order_items[idx] && desired_order_items[idx]) {
				items[idx] = desired_order_items[idx];
				result.order_changed = true;
			}
		}

		if (result.order_changed) {
			current_item_ = items.begin() + current_item_pos;
			lastSelectedItemPosition = kNoSelection;
		}

		return result;
	}

	void drawControlIndicator(const float center_x, const float center_y, const bool is_selected) {
		oled_canvas::Canvas& image = OLED::main;

		const int32_t ix = static_cast<int32_t>(center_x);
		const int32_t iy = static_cast<int32_t>(center_y);

		if (!is_selected && ix == selected_x_ && iy == selected_y_) {
			// Overlap occurred, skip drawing
			return;
		}

		// Clear region inside
		constexpr int32_t square_size = 2;
		constexpr int32_t innerSquareSize = square_size - 1;
		for (int32_t x = ix - innerSquareSize; x <= ix + innerSquareSize; x++) {
			for (int32_t y = iy - innerSquareSize; y <= iy + innerSquareSize; y++) {
				image.clearPixel(x, y);
			}
		}

		if (is_selected) {
			// Invert region inside to highlight selection
			selected_x_ = ix, selected_y_ = iy;
			image.invertArea(ix - innerSquareSize, square_size * 2 - 1, iy - innerSquareSize, iy + innerSquareSize);
		}

		// Draw a transition square
		image.drawRectangle(ix - square_size, iy - square_size, ix + square_size, iy + square_size);
	}
};

} // namespace deluge::gui::menu_item::eq