// Host harness: measure per-mode output level of the vendored Clouds engine at the
// exact defaults the Deluge ships, through the same full-wet path the adapter uses.
#include "clouds/dsp/granular_processor.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <random>

using namespace clouds;

static const size_t kLarge = 118784;
static const size_t kSmall = 65536 - 128;

struct Result { const char* name; double rms; };

static double run(PlaybackMode mode, const char* name, float dryWet, float texture, double* dryRmsOut) {
  static std::vector<uint8_t> big, small;
  big.assign(kLarge, 0); small.assign(kSmall, 0);
  auto* gp = new GranularProcessor();
  memset((void*)gp, 0, sizeof(GranularProcessor));
  gp->Init(big.data(), kLarge, small.data(), kSmall);
  gp->set_silence(false);
  gp->set_bypass(false);
  gp->set_playback_mode(mode);
  gp->set_quality(0);

  Parameters* p = gp->mutable_parameters();
  // The Deluge's committed defaults, mapped through q31ToUnipolar: centre -> 0.5.
  p->position = 0.5f;
  p->size = 0.5f;
  p->pitch = 0.0f;
  p->density = 0.80f;   // menu position 40, clear of the dead zone
  p->texture = texture;
  p->dry_wet = dryWet;
  p->stereo_spread = 0.5f;
  p->feedback = 0.0f;
  p->reverb = 0.0f;
  p->freeze = false;
  p->trigger = false;
  p->gate = false;

  gp->Prepare();

  std::mt19937 rng(12345);
  std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

  const size_t block = kMaxBlockSize;
  ShortFrame in[kMaxBlockSize], out[kMaxBlockSize];
  double sum = 0.0, drySum = 0.0; size_t n = 0;
  const double sr = 32000.0;
  double phase = 0.0;
  // 20 s of programme-like material: a chord plus a little noise, ~ -20 dBFS RMS.
  size_t totalBlocks = (size_t)(20.0 * sr / block);
  size_t measureFrom = (size_t)(19.6 * sr / block);
  for (size_t b = 0; b < totalBlocks; ++b) {
    for (size_t i = 0; i < block; ++i) {
      double t = phase / sr;
      double s = 0.30 * sin(2*M_PI*220.0*t) + 0.22 * sin(2*M_PI*277.18*t)
               + 0.18 * sin(2*M_PI*329.63*t) + 0.05 * noise(rng);
      phase += 1.0;
      float v = (float)(s * 0.32);
      in[i].l = (int16_t)(fmax(-1.0f, fmin(1.0f, v)) * 32767.0f);
      in[i].r = in[i].l;
    }
    gp->Prepare();
    gp->Process(in, out, block);
    if (b >= measureFrom) {
      for (size_t i = 0; i < block; ++i) {
        double l = out[i].l / 32768.0, r = out[i].r / 32768.0;
        sum += l*l + r*r;
        double dl = in[i].l / 32768.0;
        drySum += 2.0 * dl*dl;
        n += 2;
      }
    }
  }
  delete gp;
  if (dryRmsOut) *dryRmsOut = sqrt(drySum / n);
  return sqrt(sum / n);
}

int main() {
  struct { PlaybackMode m; const char* n; } modes[] = {
    {PLAYBACK_MODE_GRANULAR, "Granular"},
    {PLAYBACK_MODE_STRETCH, "Stretch"},
    {PLAYBACK_MODE_LOOPING_DELAY, "Delay"},
    {PLAYBACK_MODE_SPECTRAL, "Spectral"},
    {PLAYBACK_MODE_OLIVERB, "Oliverb"},
    {PLAYBACK_MODE_RESONESTOR, "Resonestor"},
  };
  double dry = 0;
  printf("%-12s %10s %10s %8s\n", "mode", "wet RMS", "vs dry", "dB");
  for (auto& e : modes) {
    double r = run(e.m, e.n, (e.m == PLAYBACK_MODE_RESONESTOR) ? 0.0f : 1.0f, 0.5f, &dry);
    printf("%-12s %10.5f %10.3fx %+8.1f\n", e.n, r, r/dry, 20*log10((r+1e-12)/(dry+1e-12)));
  }
  printf("\ndry RMS = %.5f\n\n", dry);
  printf("Resonestor detail (wet only, as the firmware now drives it):\n");
  struct { float dw; float tex; const char* label; } v[] = {
    {0.0f, 0.5f, "distortion 0, texture centre (shipping default)"},
    {1.0f, 0.5f, "distortion 1, texture centre (upstream Blend-up)"},
    {0.0f, 0.35f, "distortion 0, texture 0.35"},
    {0.0f, 0.25f, "distortion 0, texture 0.25"},
    {0.0f, 0.10f, "distortion 0, texture 0.10"},
  };
  for (auto& e : v) {
    double d2 = 0;
    double r = run(PLAYBACK_MODE_RESONESTOR, "R", e.dw, e.tex, &d2);
    printf("  %-46s %8.5f  %6.2fx  %+6.1f dB\n", e.label, r, r/d2, 20*log10((r+1e-12)/(d2+1e-12)));
  }
  return 0;
}
