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
// STFT with overlap-add.

#include "clouds/dsp/pvoc/stft.h"

#include <algorithm>

#include "clouds/dsp/pvoc/frame_transformation.h"
#include "clouds/dsp/parameters.h"

#include <cmath>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include "stmlib/dsp/dsp.h"

namespace clouds {

using namespace std;
using namespace stmlib;

#if defined(__ARM_NEON)
namespace {
void BuildTwiddles();  // Deluge: see BufferStep() below
}  // namespace
#endif

void STFT::Init(
    FFT* fft,
    size_t fft_size,
    size_t hop_size,
    float* fft_buffer,
    float* ifft_buffer,
    const float* window_lut,
    short* analysis_synthesis_buffer,
    Modifier* modifier) {
  fft_size_ = fft_size;
  hop_size_ = hop_size;
  fft_num_passes_ = 0;
  for (size_t t = fft_size; t > 1; t >>= 1) {
    ++fft_num_passes_;
  }
  buffer_size_ = fft_size_ + hop_size_;
  
  fft_ = fft;
#if defined(__ARM_NEON) && !defined(USE_ARM_FFT)
  BuildTwiddles();  // Deluge: NEON FFT passes, see BufferStep()
#endif
#ifdef USE_ARM_FFT
  arm_rfft_fast_init_f32(fft_, fft_size);
#else
  fft_->Init();
#endif  // USE_ARM_FFT
  
  analysis_ = &analysis_synthesis_buffer[0];
  synthesis_ = &analysis_synthesis_buffer[buffer_size_];

  ifft_in_ = fft_in_ = fft_buffer;
  ifft_out_ = fft_out_ = ifft_buffer;
  
  window_ = window_lut;
  window_stride_ = LUT_SINE_WINDOW_4096_SIZE / fft_size;
  modifier_ = modifier;
  
  parameters_ = NULL;
  
  Reset();
}

void STFT::Reset() {
  buffer_ptr_ = 0;
  process_ptr_ = (2 * hop_size_) % buffer_size_;
  block_size_ = 0;
  fill(&analysis_[0], &analysis_[buffer_size_], 0);
  fill(&synthesis_[0], &synthesis_[buffer_size_], 0);
  ready_ = 0;
  done_ = 0;
  stage_ = 0;
  step_ = 0;
  fft_s_ = NULL;
  fft_d_ = NULL;
}

void STFT::Process(
    const Parameters& parameters,
    const float* input,
    float* output,
    size_t size,
    size_t stride) {
  parameters_ = &parameters;
  while (size) {
    size_t processed = min(size, hop_size_ - block_size_);
    for (size_t i = 0; i < processed; ++i) {
      int32_t sample = *input * 32768.0f;
      analysis_[buffer_ptr_ + i] = Clip16(sample);
      *output = static_cast<float>(synthesis_[buffer_ptr_ + i]) / 16384.0f;
      input += stride;
      output += stride;
    }
    
    block_size_ += processed;
    size -= processed;
    buffer_ptr_ += processed;
    if (buffer_ptr_ >= buffer_size_) {
      buffer_ptr_ -= buffer_size_;
    }
    if (block_size_ >= hop_size_) {
      block_size_ -= hop_size_;
      ++ready_;
    }
  }
}

void STFT::Buffer() {
  if (ready_ == done_) {
    return;
  }
  
  // Copy block to FFT buffer and apply window.
  size_t source_ptr = process_ptr_;
  const float* w = window_;
  for (size_t i = 0; i < fft_size_; ++i) {
    fft_in_[i] = w[0] * analysis_[source_ptr];
    ++source_ptr;
    if (source_ptr >= buffer_size_) {
      source_ptr -= buffer_size_;
    }
    w += window_stride_;
  }
  
  // Compute FFT. fft_in is lost.
#ifdef USE_ARM_FFT
  arm_rfft_fast_f32(fft_, fft_in_, fft_out_, 0);
  copy(&fft_out_[0], &fft_out_[fft_size_], &fft_in_[0]);
  // Re-arrange data.
  for (size_t i = 0; i < fft_size_ / 2; ++i) {
    fft_out_[i] = fft_in_[2 * i];
    fft_out_[i + fft_size_ / 2] = fft_in_[2 * i + 1];
  }
#else
  if (fft_size_ != FFT::max_size) {
    fft_->Direct(fft_in_, fft_out_, fft_num_passes_);
  } else {
    fft_->Direct(fft_in_, fft_out_);
  }
#endif  // USE_ARM_FFT
  // Process in the frequency domain.
  if (modifier_ != NULL && parameters_ != NULL) {
    modifier_->Process(*parameters_, &fft_out_[0], &ifft_in_[0]);
  } else {
    copy(&fft_out_[0], &fft_out_[fft_size_], &ifft_in_[0]);
  }
  
  // Compute IFFT. ifft_in is lost.
#ifdef USE_ARM_FFT
  // Re-arrange data.
  copy(&ifft_in_[0], &ifft_in_[fft_size_], &ifft_out_[0]);
  for (size_t i = 0; i < fft_size_ / 2; ++i) {
    ifft_in_[2 * i] = ifft_out_[i];
    ifft_in_[2 * i + 1] = ifft_out_[i + fft_size_ / 2];
  }
  arm_rfft_fast_f32(fft_, ifft_in_, ifft_out_, 1);
#else
  if (fft_size_ != FFT::max_size) {
    fft_->Inverse(ifft_in_, ifft_out_, fft_num_passes_);
  } else {
    fft_->Inverse(ifft_in_, ifft_out_);
  }
#endif  // USE_ARM_FFT
  
  size_t destination_ptr = process_ptr_;
#ifdef USE_ARM_FFT
  float inverse_window_size = 1.0f / \
      float(fft_size_ / hop_size_ >> 1);
#else
  float inverse_window_size = 1.0f / \
      float(fft_size_ * fft_size_ / hop_size_ >> 1);
#endif  // USE_ARM_FFT
    
  w = window_;
  for (size_t i = 0; i < fft_size_; ++i) {
    float s = ifft_out_[i] * w[0] * inverse_window_size;
    
    int32_t x = static_cast<int32_t>(s);
    if (i < fft_size_ - hop_size_) {
      // Overlap-add.
      x += synthesis_[destination_ptr];
    }
    synthesis_[destination_ptr] = Clip16(x);
    ++destination_ptr;
    if (destination_ptr >= buffer_size_) {
      destination_ptr -= buffer_size_;
    }
    w += window_stride_;
  }

  ++done_;
  process_ptr_ += hop_size_;
  if (process_ptr_ >= buffer_size_) {
    process_ptr_ -= buffer_size_;
  }
}

// ---------------------------------------------------------------------------
// Deluge: resumable Buffer(). Each function below is a verbatim slice of
// Buffer() or of stmlib::DirectTransform / InverseTransform (the num_passes > 8
// branch, which is the one a 4096-point Clouds FFT takes).

namespace {
// Approximate ARM instruction counts per step, measured with qemu-arm on the
// firmware's build flags (see dsp/clouds/PROVENANCE.md). Only their sum per
// hop and their maximum matter: the caller hands out credit per sample.
const int32_t kCostWindowHalf = 28000;
const int32_t kCostFftPass = 21000;
const int32_t kCostOverlapAddQuarter = 25000;
const size_t kOverlapAddSteps = 4;

#if defined(__ARM_NEON)
// NEON FFT passes. shy_fft's RotationPhasor produces each twiddle by rotating
// the previous one -- a serial dependency that cannot be vectorised -- so the
// NEON passes read exact cos/sin from a table instead (slightly more accurate
// than the recurrence; not bit-identical to it). Pass p (n = 2^p) uses entries
// j = 0..n/2-1 at offset 2^(p-1) - 4: cos/sin(j * pi / n). 4096-point FFT:
// passes 3..11, 2044 entries. In SDRAM; built on first STFT::Init(), which runs
// when Spectral is selected, off the audio path.
const size_t kTwiddleEntries = (kMaxFftSize >> 1) - 4;
__attribute__((section(".sdram_bss"))) float twiddle_cos[kTwiddleEntries];
__attribute__((section(".sdram_bss"))) float twiddle_sin[kTwiddleEntries];
bool twiddles_built = false;

void BuildTwiddles() {
  if (twiddles_built) {
    return;
  }
  for (size_t pass = 3; (static_cast<size_t>(1) << pass) <= (kMaxFftSize >> 1); ++pass) {
    const size_t n = static_cast<size_t>(1) << pass;
    const size_t offset = (n >> 1) - 4;
    for (size_t j = 0; j < (n >> 1); ++j) {
      const double angle = 3.14159265358979323846 * static_cast<double>(j) / static_cast<double>(n);
      twiddle_cos[offset + j] = static_cast<float>(cos(angle));
      twiddle_sin[offset + j] = static_cast<float>(sin(angle));
    }
  }
  twiddles_built = true;
}

inline float32x4_t Reverse(float32x4_t x) {
  x = vrev64q_f32(x);
  return vcombine_f32(vget_high_f32(x), vget_low_f32(x));
}
#endif  // __ARM_NEON
}  // namespace

void STFT::WindowStep(size_t begin, size_t end) {
  size_t source_ptr = process_ptr_ + begin;
  if (source_ptr >= buffer_size_) {
    source_ptr -= buffer_size_;
  }
  const float* w = window_ + begin * window_stride_;
  for (size_t i = begin; i < end; ++i) {
    fft_in_[i] = w[0] * analysis_[source_ptr];
    ++source_ptr;
    if (source_ptr >= buffer_size_) {
      source_ptr -= buffer_size_;
    }
    w += window_stride_;
  }
}

void STFT::DirectStep(int32_t step) {
  const size_t num_passes = fft_num_passes_;
  const size_t size = fft_size_;
  const uint8_t* bit_rev = FFT::bit_rev_256_lut();
  Math<float> math;
  float* input = fft_in_;
  float* output = fft_out_;
  if (step == 0) {
    // First and second pass.
    float* d = output;
    for (size_t i = 0; i < size; i += 4) {
      const float* s = input;
      size_t r0 = ((bit_rev[i & 0xff] << 8) | bit_rev[i >> 8]) >> (16 - num_passes);
      size_t r1 = r0 + 2 * (size >> 2);
      size_t r2 = r0 + 1 * (size >> 2);
      size_t r3 = r0 + 3 * (size >> 2);
      d[1] = s[r0] - s[r1];
      d[3] = s[r2] - s[r3];
      float a = s[r0] + s[r1];
      float b = s[r2] + s[r3];
      d[0] = a + b;
      d[2] = a - b;
      d += 4;
    }
    return;
  }
  if (step == 1) {
    // Third pass.
    float* s = output;
    float* d = input;
    for (size_t i = 0; i < size; i += 8) {
      float v;
      d[i] = s[i] + s[i + 4];
      d[i + 4] = s[i] - s[i + 4];
      d[i + 2] = s[i + 2];
      d[i + 6] = s[i + 6];
      v = (s[i + 5] - s[i + 7]) * math.sqrt_2_div_2();
      d[i + 1] = s[i + 1] + v;
      d[i + 3] = s[i + 1] - v;
      v = (s[i + 5] + s[i + 7]) * math.sqrt_2_div_2();
      d[i + 5] = v + s[i + 3];
      d[i + 7] = v - s[i + 3];
    }
    fft_s_ = s;
    fft_d_ = d;
    return;
  }
  // Remaining passes, one per step: step 2 is pass 3.
  const size_t pass = static_cast<size_t>(step) + 1;
  float* s = fft_d_;  // flip source and destination
  float* d = fft_s_;
  size_t n = 1 << pass;
  size_t n_2 = n >> 1;
#if defined(__ARM_NEON)
  if (n_2 >= 8) {
    const float* tc = &twiddle_cos[n_2 - 4];
    const float* ts = &twiddle_sin[n_2 - 4];
    for (size_t i = 0; i < size; i += (n << 1)) {
      float* s1r = s + i;
      float* s2r = s1r + n;
      float* dr = d + i;
      float* di = dr + n;
      dr[0] = s1r[0] + s2r[0];
      di[0] = s1r[0] - s2r[0];
      dr[n_2] = s1r[n_2];
      di[n_2] = s2r[n_2];
      float* s1i = s1r + n_2;
      float* s2i = s1i + n;
      size_t j = 1;
      for (; j < 4; ++j) {
        float c = tc[j];
        float sn = ts[j];
        float v = s2r[j] * c - s2i[j] * sn;
        dr[j] = s1r[j] + v;
        di[-static_cast<ptrdiff_t>(j)] = s1r[j] - v;
        v = s2r[j] * sn + s2i[j] * c;
        di[j] = v + s1i[j];
        di[n - j] = v - s1i[j];
      }
      for (; j < n_2; j += 4) {
        const float32x4_t c = vld1q_f32(tc + j);
        const float32x4_t sn = vld1q_f32(ts + j);
        const float32x4_t ar = vld1q_f32(s2r + j);
        const float32x4_t ai = vld1q_f32(s2i + j);
        const float32x4_t br = vld1q_f32(s1r + j);
        const float32x4_t bi = vld1q_f32(s1i + j);
        const float32x4_t v1 = vmlsq_f32(vmulq_f32(ar, c), ai, sn);
        vst1q_f32(dr + j, vaddq_f32(br, v1));
        vst1q_f32(di - j - 3, Reverse(vsubq_f32(br, v1)));
        const float32x4_t v2 = vmlaq_f32(vmulq_f32(ar, sn), ai, c);
        vst1q_f32(di + j, vaddq_f32(v2, bi));
        vst1q_f32(di + n - j - 3, Reverse(vsubq_f32(v2, bi)));
      }
    }
    fft_s_ = s;
    fft_d_ = d;
    if (pass == num_passes - 1 && d != output) {
      std::copy(&d[0], &d[size], &output[0]);
    }
    return;
  }
#endif  // __ARM_NEON
  FFT::PhasorType_* phasor = fft_->phasor();
  for (size_t i = 0; i < size; i += (n << 1)) {
    float* s1r = s + i;
    float* s2r = s1r + n;
    float* dr = d + i;
    float* di = dr + n;
    dr[0] = s1r[0] + s2r[0];
    di[0] = s1r[0] - s2r[0];
    dr[n_2] = s1r[n_2];
    di[n_2] = s2r[n_2];
    float* s1i = s1r + n_2;
    float* s2i = s1i + n;
    phasor->Start(pass);
    for (size_t j = 1; j < n_2; ++j) {
      float c = phasor->cos();
      float sn = phasor->sin();
      float v;
      v = s2r[j] * c - s2i[j] * sn;
      dr[j] = s1r[j] + v;
      di[-j] = s1r[j] - v;
      v = s2r[j] * sn + s2i[j] * c;
      di[j] = v + s1i[j];
      di[n - j] = v - s1i[j];
      phasor->Rotate();
    }
  }
  fft_s_ = s;
  fft_d_ = d;
  if (pass == num_passes - 1 && d != output) {
    // Annoying additional data copy step.
    std::copy(&d[0], &d[size], &output[0]);
  }
}

void STFT::InverseStep(int32_t step) {
  const size_t num_passes = fft_num_passes_;
  const size_t size = fft_size_;
  const uint8_t* bit_rev = FFT::bit_rev_256_lut();
  Math<float> math;
  float* input = ifft_in_;
  float* output = ifft_out_;
  const int32_t num_pass_steps = static_cast<int32_t>(num_passes) - 3;
  if (step < num_pass_steps) {
    // Remaining passes, from the top: step 0 is pass num_passes - 1.
    float* s = step == 0 ? input : fft_s_;
    float* d = step == 0 ? output : fft_d_;
    const size_t pass = num_passes - 1 - static_cast<size_t>(step);
    size_t n = 1 << pass;
    size_t n_2 = n >> 1;
#if defined(__ARM_NEON)
    if (n_2 >= 8) {
      const float* tc = &twiddle_cos[n_2 - 4];
      const float* ts = &twiddle_sin[n_2 - 4];
      for (size_t i = 0; i < size; i += (n << 1)) {
        float* sr = s + i;
        float* si = sr + n;
        float* d1r = d + i;
        float* d2r = d1r + n;
        d1r[0] = sr[0] + si[0];
        d2r[0] = sr[0] - si[0];
        d1r[n_2] = sr[n_2] * 2.0f;
        d2r[n_2] = si[n_2] * 2.0f;
        float* d1i = d1r + n_2;
        float* d2i = d1i + n;
        size_t j = 1;
        for (; j < 4; ++j) {
          d1r[j] = sr[j] + si[-static_cast<ptrdiff_t>(j)];
          d1i[j] = si[j] - si[n - j];
          float c = tc[j];
          float sn = ts[j];
          float vr = sr[j] - si[-static_cast<ptrdiff_t>(j)];
          float vi = si[j] + si[n - j];
          d2r[j] = vr * c + vi * sn;
          d2i[j] = vi * c - vr * sn;
        }
        for (; j < n_2; j += 4) {
          const float32x4_t c = vld1q_f32(tc + j);
          const float32x4_t sn = vld1q_f32(ts + j);
          const float32x4_t srj = vld1q_f32(sr + j);
          const float32x4_t sij = vld1q_f32(si + j);
          const float32x4_t sim = Reverse(vld1q_f32(si - j - 3));
          const float32x4_t sin_ = Reverse(vld1q_f32(si + n - j - 3));
          vst1q_f32(d1r + j, vaddq_f32(srj, sim));
          vst1q_f32(d1i + j, vsubq_f32(sij, sin_));
          const float32x4_t vr = vsubq_f32(srj, sim);
          const float32x4_t vi = vaddq_f32(sij, sin_);
          vst1q_f32(d2r + j, vmlaq_f32(vmulq_f32(vr, c), vi, sn));
          vst1q_f32(d2i + j, vmlsq_f32(vmulq_f32(vi, c), vr, sn));
        }
      }
    } else
#endif  // __ARM_NEON
    {
    FFT::PhasorType_* phasor = fft_->phasor();
    for (size_t i = 0; i < size; i += (n << 1)) {
      float* sr = s + i;
      float* si = sr + n;
      float* d1r = d + i;
      float* d2r = d1r + n;
      d1r[0] = sr[0] + si[0];
      d2r[0] = sr[0] - si[0];
      d1r[n_2] = sr[n_2] * 2.0f;
      d2r[n_2] = si[n_2] * 2.0f;
      float* d1i = d1r + n_2;
      float* d2i = d1i + n;
      phasor->Start(pass);
      for (size_t j = 1; j < n_2; ++j) {
        d1r[j] = sr[j] + si[-j];
        d1i[j] = si[j] - si[n - j];
        float c = phasor->cos();
        float sn = phasor->sin();
        float vr = sr[j] - si[-j];
        float vi = si[j] + si[n - j];
        d2r[j] = vr * c + vi * sn;
        d2i[j] = vi * c - vr * sn;
        phasor->Rotate();
      }
    }
    }
    // Flip source and destination pointers for the next pass.
    if (d == output) {
      s = output;
      d = input;
    } else {
      s = input;
      d = output;
    }
    fft_s_ = s;
    fft_d_ = d;
    return;
  }
  if (step == num_pass_steps) {
    // Copy data if necessary, then the third pass.
    if (fft_d_ == output) {
      std::copy(&fft_s_[0], &fft_s_[size], &output[0]);
    }
    float* s = output;
    float* d = input;
    for (size_t i = 0; i < size; i += 8) {
      float vr, vi;
      d[i] = s[i] + s[i + 4];
      d[i + 4] = s[i] - s[i + 4];
      d[i + 2] = s[i + 2] * 2.0f;
      d[i + 6] = s[i + 6] * 2.0f;
      d[i + 1] = s[i + 1] + s[i + 3];
      d[i + 3] = s[i + 5] - s[i + 7];
      vr = s[i + 1] - s[i + 3];
      vi = s[i + 5] + s[i + 7];
      d[i + 5] = (vr + vi) * math.sqrt_2_div_2();
      d[i + 7] = (vi - vr) * math.sqrt_2_div_2();
    }
    return;
  }
  // First and second pass.
  float* s = input;
  float* d = output;
  for (size_t i = 0; i < size; i += 4) {
    size_t r0 = ((bit_rev[i & 0xff] << 8) | bit_rev[i >> 8]) >> (16 - num_passes);
    size_t r1 = r0 + 2 * (size >> 2);
    size_t r2 = r0 + 1 * (size >> 2);
    size_t r3 = r0 + 3 * (size >> 2);
    float b_0 = s[0] + s[2];
    float b_2 = s[0] - s[2];
    float b_1 = s[1] * 2.0f;
    float b_3 = s[3] * 2.0f;
    d[r0] = b_0 + b_1;
    d[r1] = b_0 - b_1;
    d[r2] = b_2 + b_3;
    d[r3] = b_2 - b_3;
    s += 4;
  }
}

void STFT::OverlapAddStep(size_t begin, size_t end) {
  size_t destination_ptr = process_ptr_ + begin;
  if (destination_ptr >= buffer_size_) {
    destination_ptr -= buffer_size_;
  }
  float inverse_window_size = 1.0f / \
      float(fft_size_ * fft_size_ / hop_size_ >> 1);
  const float* w = window_ + begin * window_stride_;
  for (size_t i = begin; i < end; ++i) {
    float s = ifft_out_[i] * w[0] * inverse_window_size;
    int32_t x = static_cast<int32_t>(s);
    if (i < fft_size_ - hop_size_) {
      // Overlap-add.
      x += synthesis_[destination_ptr];
    }
    synthesis_[destination_ptr] = Clip16(x);
    ++destination_ptr;
    if (destination_ptr >= buffer_size_) {
      destination_ptr -= buffer_size_;
    }
    w += window_stride_;
  }
}

int32_t STFT::NextStepCost() const {
  switch (stage_) {
    case 1: return kCostWindowHalf;
    case 2: return kCostFftPass;
    case 3: return FrameTransformation::ProcessStepCost(step_);
    case 4: return kCostFftPass;
    case 5: return kCostOverlapAddQuarter;
    default: return 0;
  }
}

bool STFT::BufferStep(int32_t* credit) {
#ifdef USE_ARM_FFT
  Buffer();
  return false;
#else
  if (fft_num_passes_ <= 8) {
    Buffer();  // Not a size Clouds uses; keep the original path.
    return false;
  }
  const int32_t fft_steps = static_cast<int32_t>(fft_num_passes_) - 1;
  bool did_work = false;
  while (*credit > 0) {
    if (stage_ != 0 && did_work && NextStepCost() > *credit) {
      return true;  // would overrun this call's share; next call
    }
    switch (stage_) {
      case 0:
        if (ready_ == done_) {
          return false;
        }
        stage_ = 1;
        step_ = 0;
        continue;  // bookkeeping, not work
      case 1:
        WindowStep(step_ * (fft_size_ >> 1), (step_ + 1) * (fft_size_ >> 1));
        *credit -= kCostWindowHalf;
        if (++step_ >= 2) {
          stage_ = 2;
          step_ = 0;
        }
        break;
      case 2:
        DirectStep(step_);
        *credit -= kCostFftPass;
        if (++step_ >= fft_steps) {
          stage_ = 3;
          step_ = 0;
        }
        break;
      case 3:
        if (modifier_ != NULL && parameters_ != NULL) {
          modifier_->ProcessStep(step_, *parameters_, &fft_out_[0], &ifft_in_[0]);
          *credit -= FrameTransformation::ProcessStepCost(step_);
          if (++step_ < FrameTransformation::kNumProcessSteps) {
            break;
          }
        } else {
          copy(&fft_out_[0], &fft_out_[fft_size_], &ifft_in_[0]);
        }
        stage_ = 4;
        step_ = 0;
        break;
      case 4:
        InverseStep(step_);
        *credit -= kCostFftPass;
        if (++step_ >= fft_steps) {
          stage_ = 5;
          step_ = 0;
        }
        break;
      case 5: {
        const size_t quarter = fft_size_ / kOverlapAddSteps;
        OverlapAddStep(step_ * quarter, (step_ + 1) * quarter);
        *credit -= kCostOverlapAddQuarter;
        if (++step_ < static_cast<int32_t>(kOverlapAddSteps)) {
          break;
        }
        ++done_;
        process_ptr_ += hop_size_;
        if (process_ptr_ >= buffer_size_) {
          process_ptr_ -= buffer_size_;
        }
        stage_ = 0;
        // One hop per visit, so channels alternate exactly as in Buffer().
        return has_work();
      }
    }
    did_work = true;
  }
  return has_work();
#endif  // USE_ARM_FFT
}

}  // namespace clouds
