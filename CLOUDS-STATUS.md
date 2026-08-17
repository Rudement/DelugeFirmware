# Clouds on 1.3 — status, 2026-08-17

Branch `feat/clouds-fx-13`. Read this before touching anything.

## What works

- Clouds is vendored, builds, links, and runs on hardware.
- **Granular works.** Grains, all nine parameters, from any context.
- Reachable from Song, Kit, audio clips **and synth clips** — one page,
  one shared engine, `clouds_fx::SongParam` resolves the engine params to
  the song's param manager wherever the menu was opened from.
- Wired as a **send**: `UNPATCHED_CLOUDS_SEND` (shared param) per source,
  stereo bus in AudioEngine, one instance owned by the song. Blend is the
  return level; the engine runs permanently fully wet.
- Save/load round-trips all nine params plus mode and freeze.

## What does not work

| Symptom | State |
|---|---|
| **Stretch drops out** | Unsolved. Recovers. Only Stretch. |
| **Continuous crackle** | Unsolved. Present in all modes. |
| **Parameters respond wrongly** | Uninvestigated. Suspect the q31 mappings in `clouds_adapter.cpp`. |
| **Resonestor** | Untested since the mode-change fixes landed. |
| **Click on mode change** | Reduced, not gone. |

All of these arrived with, or were exposed by, the send conversion
(`35821ca7`). Every mode reportedly worked when Clouds was an insert.

## The single most useful thing to try next

**Revert the send conversion.** All six modes worked under the insert
design. `clouds_fx::SongParam` and the one-page-everywhere menu are
independent of it and can stay, so you keep the synth-clip access that
motivated the change. You lose per-source send amounts — Clouds sits on
the song bus and affects everything.

That is one revert and one build to a known-good effect, versus an
unknown number of cycles debugging the send.

## Measurements (real, trust these)

Code size, `f2ac9a16` vs the same tree without Clouds reachable:

| | bytes |
|---|---|
| `.text` | +103,348 |
| `.data` | +1,160 |
| `.bss` | +488 (+1,120 after the send bus) |
| binary | +104,556 (+5.6%) |

`.bss` barely moves because the ~180 KB working set is runtime-allocated
from the stealable SDRAM pool, which was the point of `CloudsBuffer`.

Relative CPU, engine compiled natively and driven through the adapter's
exact plumbing. Host absolutes are meaningless; the ratios transfer:

| mode | total | engine | resampler's share |
|---|---|---|---|
| Granular | 0.369% | 0.300% | **18.6%** |
| Looping delay | 0.475% | 0.407% | 14.4% |
| Resonestor | 0.678% | 0.610% | 10.1% |
| Stretch | 0.745% | 0.677% | 9.2% |
| Oliverb | 0.792% | 0.724% | 8.6% |
| Spectral | 0.892% | 0.823% | 7.7% |

Two conclusions. Resampling is **not** trivial beside the engine — the
feasibility doc assumed it would be; it is 8–19% of the stage. And
Stretch, Oliverb and Spectral cost roughly twice Granular, which is
consistent with Stretch being the one that falls over.

Resampler quality (linear interpolation, round trip, engine bypassed):
60 dB SNR at 100 Hz, 39 dB at 1 kHz, 17 dB at 5 kHz, 3 dB at 14 kHz,
~44 samples latency. Fine in the bass, poor in the top octaves. Note the
polyphase swap would *increase* CPU in a stage already costing a fifth
of Granular.

## Traps that cost hours — do not rediscover these

1. **`g_english.cpp` is generated.** Edit `english.json` and run
   `generate.py`. Hand edits vanish on the next build, leaving menu items
   with no text — which looks exactly like the menu not existing.
2. **The vertical `Submenu` tree is the primary navigation**, not the
   horizontal chains. Chains are only reached via SHIFT + Cross
   Screen/Scale. A menu must be in `soundFXMenu` / `globalFXMenu` /
   `audioClipFXMenu` to be findable.
3. **Upstream assumes `.bss`.** `GranularProcessor` is a global in
   `clouds.cc`, so members it never assigns — `silence_` above all —
   start zeroed. Placement-new into allocator memory must `memset` first
   or the engine outputs digital silence forever, with no other symptom.
   Thirteen members across the vendored tree depend on this.
4. **Density has a dead zone.** Upstream zeroes grain overlap around the
   middle of the control; the audible dead band is roughly 0.40–0.75. A
   q31 default of 0 lands dead centre. Default is now 0.80.
5. **Spread at minimum silences Resonestor.** `separation` goes to 1.0.
   Default is now centre.
6. **Blend is distortion in Resonestor**, not a mix
   (`set_distortion(dry_wet)`). The send pins `dry_wet` to 1.0, so it is
   stuck at maximum and unreachable. Still open.
7. **`Prepare()` is a main-loop function upstream.** On a mode change it
   does fourteen Init/Allocate calls. Doing that in the audio render
   clicks and culls voices. Now done on the UI thread under
   `CriticalSectionGuard`, and the per-block call is rate-limited to
   ~1.4 kHz because the Deluge's render block varies 4–128 samples.

## The tool that actually worked

The vendored DSP compiles for the host with `-DTEST` (upstream's own
escape hatch for `debug_pin.h`):

```
g++ -std=c++17 -O2 -w -DTEST -Isrc/deluge/dsp/clouds -Isrc/deluge/dsp \
    harness.cpp src/deluge/dsp/clouds/clouds/dsp/*.cc \
    src/deluge/dsp/clouds/clouds/dsp/pvoc/*.cc \
    src/deluge/dsp/clouds/clouds/resources.cc \
    src/deluge/dsp/stmlib/dsp/units.cc src/deluge/dsp/stmlib/dsp/atan.cc \
    src/deluge/dsp/stmlib/utils/random.cc
```

This found the Density dead zone, the Spread/Resonestor interaction and
the CPU ratios in minutes. Reasoning from source found nothing and was
wrong repeatedly. **Reach for the harness first.** Its limitation is that
it cannot model the device's threading, block-size variation in situ, or
memory pressure — which is precisely where the remaining bugs live.
