// Copyright 2016 Emilie Gillet.
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
// Utility DSP routines.

#ifndef PLAITS_DSP_DSP_H_
#define PLAITS_DSP_DSP_H_

#include "stmlib/stmlib.h"

namespace plaits {
  
// DELUGE PORT: upstream 48000.0f. The Deluge's audio engine runs at 44100
// (kSampleRate in src/definitions_cxx.hpp). Everything rate-dependent in
// Plaits routes through this constant, kCorrectedSampleRate, or a0 below --
// 41 references across 13 files, all via these three -- so retuning is this
// edit plus the one literal in dsp/fx/diffuser.h.
//
// KNOWN AND ACCEPTED: the time-constant tables in resources.cc were generated
// at 48 kHz, so envelopes, LPG decays, drum decays and the LPC speech frame
// rate run ~8.8% slower than on the module. Pitch is unaffected (it comes from
// a0). Correctable later by regenerating resources.cc from upstream's
// resources.py at 44100; not worth doing until it sounds wrong.
static const float kSampleRate = 44100.0f;

// There is no proper PLL for I2S, only a divider on the system clock to derive
// the bit clock.
// The division ratio is set to 47 (23 EVEN, 1 ODD) by the ST libraries.
//
// Bit clock = 72000000 / 47 = 1531.91 kHz
// Frame clock = Bit clock / 32 = 47872.34 Hz
//
// That's only 4.6 cts of error, but we care!

// DELUGE PORT: upstream 47872.34f, which is Plaits' actual I2S frame clock
// (72 MHz / 47 / 32) rather than its nominal 48 kHz. The Deluge's clock is not
// detuned in that way, so the corrected rate is simply the real rate. Getting
// this wrong detunes the whole engine by ~4.6 cents against the Deluge's own
// oscillators, which is exactly the kind of thing that gets blamed on the
// tuning system later.
static const float kCorrectedSampleRate = 44100.0f;
const float a0 = (440.0f / 8.0f) / kCorrectedSampleRate;

const size_t kMaxBlockSize = 24;
const size_t kBlockSize = 12;

}  // namespace plaits

#endif  // PLAITS_DSP_DSP_H_
