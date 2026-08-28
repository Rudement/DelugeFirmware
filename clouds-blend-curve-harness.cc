// Blend curve: what comes out of each mode as Blend sweeps, through the exact path
// the firmware uses -- engine crossfade for five modes, adapter crossfade for
// Resonestor (distortion 0, trim 0.668).
#include "clouds/dsp/granular_processor.h"
#include "stmlib/dsp/dsp.h"
#include "clouds/resources.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <random>
using namespace clouds;
static const size_t kLarge = 118784, kSmall = 65536 - 128;

static void run(PlaybackMode mode, float blend, bool adapterMix, float trim, double* outRms, double* dryRms) {
  static std::vector<uint8_t> big, sml;
  big.assign(kLarge, 0); sml.assign(kSmall, 0);
  auto* gp = new GranularProcessor();
  memset((void*)gp, 0, sizeof(GranularProcessor));
  gp->Init(big.data(), kLarge, sml.data(), kSmall);
  gp->set_silence(false); gp->set_bypass(false);
  gp->set_playback_mode(mode); gp->set_quality(0);
  Parameters* p = gp->mutable_parameters();
  p->position=0.5f; p->size=0.5f; p->pitch=0.0f; p->density=0.80f; p->texture=0.5f;
  p->stereo_spread=0.5f; p->feedback=0.0f; p->reverb=0.0f; p->freeze=false;
  p->trigger=false; p->gate=false;
  p->dry_wet = adapterMix ? 0.0f : blend;   // Resonestor: distortion 0
  gp->Prepare();

  float wetGain = stmlib::Interpolate(lut_xfade_in, blend, 16.0f);
  float dryGain = stmlib::Interpolate(lut_xfade_out, blend, 16.0f);

  std::mt19937 rng(12345); std::uniform_real_distribution<float> noise(-1.f,1.f);
  const size_t block = kMaxBlockSize;
  ShortFrame in[kMaxBlockSize], out[kMaxBlockSize];
  double sum=0, dsum=0; size_t n=0, phase=0;
  const double sr=32000.0;
  size_t total=(size_t)(20.0*sr/block), from=(size_t)(19.6*sr/block);
  for (size_t b=0;b<total;++b){
    for(size_t i=0;i<block;++i){
      double t=phase/sr;
      double s=0.30*sin(2*M_PI*220.0*t)+0.22*sin(2*M_PI*277.18*t)+0.18*sin(2*M_PI*329.63*t)+0.05*noise(rng);
      phase++;
      float v=(float)(s*0.32);
      in[i].l=(int16_t)(fmax(-1.f,fmin(1.f,v))*32767.f); in[i].r=in[i].l;
    }
    gp->Prepare();
    ShortFrame dry[kMaxBlockSize]; memcpy(dry,in,sizeof(dry));
    gp->Process(in,out,block);
    if(b>=from){
      for(size_t i=0;i<block;++i){
        double l=out[i].l/32768.0;
        if (adapterMix) l = (dry[i].l/32768.0)*0.5*dryGain + l*trim*wetGain;
        sum+=l*l; double dl=dry[i].l/32768.0; dsum+=dl*dl; n++;
      }
    }
  }
  delete gp;
  *outRms=sqrt(sum/n); *dryRms=sqrt(dsum/n);
}

int main(){
  struct { PlaybackMode m; const char* n; bool adapter; float trim; } modes[]={
    {PLAYBACK_MODE_GRANULAR,"Granular",false,1.f},
    {PLAYBACK_MODE_LOOPING_DELAY,"Delay",false,1.f},
    {PLAYBACK_MODE_OLIVERB,"Oliverb",false,1.f},
    {PLAYBACK_MODE_RESONESTOR,"Resonestor",true,0.668f},
  };
  float blends[]={0.0f,0.25f,0.5f,0.75f,1.0f};
  printf("%-12s", "blend ->"); for(float b:blends) printf("%9.2f",b); printf("\n");
  for(auto&e:modes){
    printf("%-12s",e.n);
    for(float b:blends){ double o,d; run(e.m,b,e.adapter,e.trim,&o,&d); printf("%9.4f",o/d); }
    printf("   (x dry)\n");
  }
  return 0;
}
