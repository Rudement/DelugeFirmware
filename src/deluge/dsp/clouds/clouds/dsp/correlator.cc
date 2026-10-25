// Copyright 2014 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Search for stretch/shift splicing points by maximizing correlation.

#include "clouds/dsp/correlator.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include <algorithm>

namespace clouds {

using namespace std;

void Correlator::Init(uint32_t* source, uint32_t* destination) {
  source_ = source;
  destination_ = destination;
  offset_ = 0;
  best_match_ = 0;
  done_ = true;
}

void Correlator::EvaluateNextCandidate() {
  if (done_) {
    return;
  }
  uint32_t num_words = size_ >> 5;
  uint32_t offset_words = candidate_ >> 5;
  uint32_t offset_bits = candidate_ & 0x1f;
  uint32_t* source = &source_[0];
  uint32_t* destination = &destination_[offset_words];
  
  uint32_t xcorr = 0;
  uint32_t i = 0;
#if defined(__ARM_NEON)
  // Deluge: the same popcount, four words per instruction. Upstream runs this
  // from the module's main loop while audio runs in an interrupt; on the Deluge
  // the main loop also renders the audio, and the scalar loop below was the
  // largest single stall Stretch and Oliverb put on it (~700k instructions in
  // one background call against a 2.9 ms output buffer). Bit-for-bit the same
  // score: shifts of 32 give 0, exactly as the ARM scalar code does.
  {
    const int32x4_t shift_left = vdupq_n_s32(static_cast<int32_t>(offset_bits));
    const int32x4_t shift_right = vdupq_n_s32(static_cast<int32_t>(offset_bits) - 32);
    uint16x8_t acc = vdupq_n_u16(0);
    for (; i + 4 <= num_words; i += 4) {
      const uint32x4_t s = vld1q_u32(&source[i]);
      const uint32x4_t d = vorrq_u32(
          vshlq_u32(vld1q_u32(&destination[i]), shift_left),
          vshlq_u32(vld1q_u32(&destination[i + 1]), shift_right));
      const uint8x16_t same = vreinterpretq_u8_u32(vmvnq_u32(veorq_u32(s, d)));
      acc = vpadalq_u8(acc, vcntq_u8(same));  // <= 16 per lane per pass, 32 passes max
    }
    const uint32x4_t acc32 = vpaddlq_u16(acc);
    const uint64x2_t acc64 = vpaddlq_u32(acc32);
    xcorr = static_cast<uint32_t>(vgetq_lane_u64(acc64, 0) + vgetq_lane_u64(acc64, 1));
  }
#endif  // __ARM_NEON
  for (; i < num_words; ++i) {
    uint32_t source_bits = source[i];
    uint32_t destination_bits = 0;
    destination_bits |= destination[i] << offset_bits;
    destination_bits |= destination[i + 1] >> (32 - offset_bits);
    uint32_t count = ~(source_bits ^ destination_bits);
    count = count - ((count >> 1) & 0x55555555);
    count = (count & 0x33333333) + ((count >> 2) & 0x33333333);
    count = (((count + (count >> 4)) & 0xf0f0f0f) * 0x1010101) >> 24;
    xcorr += count;
  }
  if (xcorr > best_score_) {
    best_match_ = candidate_;
    best_score_ = xcorr;
  }
  ++candidate_;
  done_ = candidate_ >= size_;
}

void Correlator::StartSearch(
    int32_t size,
    int32_t offset,
    int32_t increment) {
  offset_ = offset;
  increment_ = increment;
  best_score_ = 0;
  best_match_ = 0;
  candidate_ = 0;
  size_ = size;
  done_ = false;
}

}  // namespace clouds
