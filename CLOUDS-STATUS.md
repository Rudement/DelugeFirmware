# Clouds on 1.3 — status, 2026-08-17

Branch `feat/clouds-fx-13`. Read this before touching anything.

> **The send experiment was reverted.** Source is back to `1c4b2095`, the
> last state in which all six modes worked. Clouds is an **insert** on the
> Song / Kit / audio-clip FX chain, with Blend as its dry/wet. It is *not*
> available on synth clips — that access came with the send work and went
> back with it. The send attempt is preserved in history, `35821ca7`
> onwards, and everything below about why it failed still applies.

## What works, as the branch now stands

- Clouds is vendored, builds, links, and runs on hardware.
- **All six modes worked at this commit**, per device testing before the
  send experiment.
- Insert on the Song / Kit / audio-clip FX chain. Blend is dry/wet.
- Density defaults to 0.80, clear of upstream's dead zone.
- Save/load round-trips all nine params plus mode and freeze.

**Untested since the revert.** This state was verified on hardware before
`35821ca7`; it has not been re-flashed since going back to it. Build and
confirm before building anything on top.

## What the send experiment cost, and what it was for

The send (`35821ca7` … `84cfc3a2`) existed to answer one need: *drive
Clouds from a synth clip*. One instance is too expensive to run per
voice, so the design was one engine fed from a per-source send bus.

It regressed the effect. After it, Granular eventually worked but
crackled, Stretch dropped out, parameters responded wrongly, and mode
changes clicked and cut voices. None of it was diagnosed to a root cause.

Worth keeping in mind if you try again: the thing that actually solved
the *access* problem was `clouds_fx::SongParam` (`a838babe`), a menu item
that resolves engine params to the song's param manager whatever context
it was opened from. That is independent of the send and could be
re-applied to the insert design on its own — giving synth-clip control of
a song-level Clouds without any of the routing work.

## Open faults recorded from the send attempt

Kept because several are probably not send-specific:

| Symptom | Notes |
|---|---|
| Click on mode change | Reduced by a 20 ms fade, never eliminated. Root cause is `Prepare()` doing 14 Init/Allocate calls; partly addressed by moving it to the UI thread (`c38de034`, `30ae1af7`). |
| Stretch drops out | Recovers. Only Stretch. Rate-limiting `Prepare()` (`84cfc3a2`) did not fix it. May simply be too expensive — it costs 2x Granular. |
| Crackle | Present in all modes under the send. Untested on the insert. |
| Resonestor | Needs Spread at centre or it is silent (`c05c49ae`) — that fix is NOT in the current tree, and is worth re-applying. |

Fixes made during the send work that are independent of it and worth
cherry-picking if the symptoms reappear on the insert:

- `c05c49ae` Spread defaults to centre — without it Resonestor is silent
- `26170c98` `bypassCulling` around the heavy Prepare
- `c38de034` / `30ae1af7` Prepare on the UI thread, whole change guarded
- `84cfc3a2` Prepare rate-limited to ~1.4 kHz
- `df216e35` non-finite output recovery

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
