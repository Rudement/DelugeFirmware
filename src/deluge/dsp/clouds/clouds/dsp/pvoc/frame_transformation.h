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
// Transformations applied to a single STFT slice.

#ifndef CLOUDS_DSP_PVOC_FRAME_TRANSFORMATION_H_
#define CLOUDS_DSP_PVOC_FRAME_TRANSFORMATION_H_

#include "stmlib/stmlib.h"

#include "clouds/dsp/pvoc/stft.h"

#include "clouds/resources.h"

namespace clouds {

const int32_t kMaxNumTextures = 7;
const int32_t kHighFrequencyTruncation = 16;

struct Parameters;

class FrameTransformation {
 public:
  FrameTransformation() { }
  ~FrameTransformation() { }
  
  void Init(float* buffer, int32_t fft_size, int32_t num_textures);
  void Reset();
  
  void Process(
      const Parameters& parameters,
      float* fft_out,
      float* ifft_in);

  // Deluge: Process() in kNumProcessSteps resumable pieces, run in order
  // 0..kNumProcessSteps-1. Same calls in the same order as Process() -- the
  // only differences are that RectangularToPolar is cut into four bin ranges,
  // and that freeze, gate and pitch are latched at step 0 so one frame is
  // processed with one set of them, as Process() does.
  static const int32_t kNumProcessSteps = 12;
  // Approximate ARM cost of each step, in instructions (qemu-arm, firmware
  // flags, 4096-point frame): what STFT::BufferStep() charges for it.
  static int32_t ProcessStepCost(int32_t step) {
    static const int32_t kCost[kNumProcessSteps] = {
        30000, 30000, 30000, 30000, 24000, 16000,
        42000, 14000, 14000, 22000, 34000, 30000};
    return kCost[step];
  }
  void ProcessStep(
      int32_t step,
      const Parameters& parameters,
      float* fft_out,
      float* ifft_in);
  
 private:
  void RectangularToPolar(float* fft_data);
  void RectangularToPolarRange(float* fft_data, int32_t begin, int32_t end);
  void PolarToRectangular(float* fft_data);
  void AddGlitch(float* xf_polar);
  void ShiftMagnitudes(
      float* source,
      float* xf_polar,
      float pitch_ratio);
  void WarpMagnitudes(
      float* source,
      float* xf_polar,
      float amount);
  void QuantizeMagnitudes(float* xf_polar, float amount);
  void StoreMagnitudes(float* xf_polar, float position, float feedback);
  void SetPhases(float* destination, float diffusion, float pitch_ratio);
  void SetPhasesAdvance(float* destination, float pitch_ratio);
  void SetPhasesRandomize(float* destination, float diffusion);
  void ReplayMagnitudes(float* xf_polar, float position);
  void DiffuseMagnitudes(float* xf_polar, float diffusion);
  
  inline void fast_p2r(float magnitude, uint16_t angle, float* re, float* im) {
    angle >>= 6;
    *re = magnitude * lut_sin[angle + 256];
    *im = magnitude * lut_sin[angle];
  }
  
  int32_t fft_size_;
  int32_t num_textures_;
  int32_t size_;
  
  // Magnitude buffers.
  float* textures_[kMaxNumTextures];
  
  // Original phase and phase unrolling buffers.
  uint16_t* phases_;
  uint16_t* phases_delta_;

  int8_t glitch_algorithm_;

  // Deluge: per-frame state latched by ProcessStep(0).
  bool step_freeze_;
  bool step_glitch_;
  float step_pitch_ratio_;
  
  DISALLOW_COPY_AND_ASSIGN(FrameTransformation);
};

}  // namespace clouds

#endif  // CLOUDS_DSP_PVOC_FRAME_TRANSFORMATION_H_
