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
// Correlation is computed by XOR-ing the bit sign of samples - this allows
// 32 samples to be matched in one single XOR operation.

#ifndef CLOUDS_DSP_CORRELATOR_H_
#define CLOUDS_DSP_CORRELATOR_H_

#include "stmlib/stmlib.h"

namespace clouds {
  
class Correlator {
 public:
  Correlator() { }
  ~Correlator() { }
  
  void Init(uint32_t* source, uint32_t* destination);

  void StartSearch(int32_t size, int32_t offset, int32_t increment);
  
  inline int32_t best_match() const {
    return offset_ + (best_match_ * (increment_ >> 4) >> 12);
  }

  inline void EvaluateSomeCandidates() {
    size_t num_candidates = (size_ >> 2) + 16;
    while (num_candidates) {
      EvaluateNextCandidate();
      --num_candidates;
    }
  }

  void EvaluateNextCandidate();

  // Deluge: EvaluateSomeCandidates() on a budget. Evaluates candidates while
  // *credit > 0, charging each its approximate ARM cost (NEON popcount: ~10
  // instructions per 4 words plus overhead). The search itself is unchanged.
  inline void EvaluateCandidates(int32_t* credit) {
    // Measured ~260 instructions per candidate at the sizes Stretch and
    // Oliverb use; a fixed floor plus the per-word part, so small windows
    // are not undercharged.
    const int32_t cost = 120 + static_cast<int32_t>(size_ >> 5) * 3;
    while (*credit > 0 && !done_) {
      EvaluateNextCandidate();
      *credit -= cost;
    }
  }
  inline bool done() const { return done_; }

  inline uint32_t* source() { return source_; }
  inline uint32_t* destination() { return destination_; }
  inline int32_t candidate() { return candidate_; }

  inline bool done() { return done_; }
  
 private:
  uint32_t* source_;
  uint32_t* destination_;
  
  int32_t offset_;
  int32_t increment_;
  int32_t size_;
  int32_t candidate_;

  uint32_t best_score_;
  int32_t best_match_;
  
  int32_t trace_;
  
  bool done_;
  
  DISALLOW_COPY_AND_ASSIGN(Correlator);
};

}  // namespace clouds

#endif  // CLOUDS_DSP_CORRELATOR_H_
