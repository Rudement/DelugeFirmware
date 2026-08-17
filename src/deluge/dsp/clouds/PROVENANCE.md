# Provenance — Mutable Instruments Clouds (Parasites)

Vendored 2026-08-16 for the Deluge community firmware, as recommended by
`DSP-PORTING-FEASIBILITY.md` (2026-08-16): lowest risk, lowest code cost,
cleanest license of the DSP-porting candidates surveyed, and the fork has
already run this exact play once, for Plaits.

## Upstream

| | |
|---|---|
| Clouds | `https://github.com/mqtthiqs/parasites` @ `32fa66f5acce1bff2f0a7bdd041e29bad3222557`, subdirectory `clouds/` |
| stmlib additions | `https://github.com/mqtthiqs/stmlib` @ `8ab2aaee77cbacb47b646d46d22ee5d358effe2d` — only `dsp/atan.{h,cc}` and `fft/shy_fft.h`, the two files Clouds needs that Plaits' stmlib subset didn't already carry |
| Author | Émilie Gillet |
| Licence | **MIT** for the STM32F (Clouds) sources — see this repo's `README.md`: "Code (STM32F projects): MIT license." Same licence as Plaits, same MIT/GPL-3.0 compatibility. `LICENSE-MIT` here is the identical text used for Plaits. Every original copyright header is intact and must stay that way. |

`mqtthiqs/parasites` is the community-maintained continuation of Mutable
Instruments' own STM32F sources (Mutable closed in 2022) plus the Parasites
alternate firmware; the Clouds DSP itself — `clouds/dsp/**` — is Gillet's
unmodified original, MIT-licensed either way. There is no permission to seek
and nobody to seek it from.

## What was taken

Only the granular/texture DSP engine reachable from `GranularProcessor`,
walked as an include closure from `clouds/dsp/granular_processor.{h,cc}`:

- `clouds/dsp/audio_buffer.h`, `correlator.{h,cc}`, `frame.h`, `grain.h`,
  `granular_processor.{h,cc}`, `granular_sample_player.h`,
  `looping_sample_player.h`, `mu_law.{h,cc}`, `parameters.h`,
  `random_oscillator.h`, `resonestor.h`, `sample_rate_converter.h`,
  `window.h`, `wsola_sample_player.h`
- `clouds/dsp/fx/{diffuser,fx_engine,oliverb,pitch_shifter,reverb}.h`
- `clouds/dsp/pvoc/{frame_transformation,phase_vocoder,stft}.{h,cc}`
- `clouds/resources.{cc,h}` — the mu-law tables and window tables the DSP
  reads
- `clouds/drivers/debug_pin.h` — kept, not stubbed; see Build notes below

## What was NOT taken

Module-specific code with no Deluge meaning: `clouds.cc` (module main loop),
`ui.*`, `cv_scaler.*`, `settings.*`, `meter.h`, `drivers/` other than
`debug_pin.h`, `bootloader/`, `hardware_design/`, `test/`.

## Deliberate modifications

**None.** `clouds/dsp/**` and `clouds/resources.*` are byte-identical to
upstream. Unlike Plaits, Clouds does not centralise its sample rate in one
constant — `32000.0f` is a literal scattered across `resonestor.h`,
`reverb.h`, and the `sample_rate()` helper in `granular_processor.h` — so
retuning it in place would not be the small, auditable edit it was for
Plaits' `dsp.h`. Per the feasibility doc's recommendation (option 1 of 3):
run Clouds internally at its native 32 kHz, byte-for-byte, and resample at
the adapter boundary instead. See `../clouds_adapter.h`.

## Build notes

- `clouds/drivers/debug_pin.h` is upstream's own `TEST`-gated stub: with
  `TEST` defined, `DebugPin::Init/High/Low` compile to empty inline
  functions instead of touching `stm32f4xx_conf.h` GPIO registers. This is
  the same escape hatch upstream's own `clouds/dsp/test/` build uses, so no
  Deluge-side stub file was needed the way Plaits needed one for
  `user_data.h`. `clouds_dsp` is compiled with `-DTEST` — see
  `CMakeLists.txt` in this directory. Confirmed by grep that `TEST` is not
  referenced anywhere else in the files taken, so this define changes
  nothing else.
- Needs `-Wno-narrowing`. `dsp/granular_processor.cc` (the
  `PLAYBACK_MODE_OLIVERB` branch) initialises `Parameters::freeze` and
  `::gate` -- both `bool` -- from `0.0f` inside a braced-init-list. Legal in
  the dialect Mutable wrote against, ill-formed from C++11 on, and gcc emits
  it as an **error**, which the `-w` above does not suppress. Fixing it in
  place would mean editing upstream and losing the byte-for-byte identity
  this file promises, so it is a build flag. Two occurrences, both in that
  one initialiser; a re-vendor that removes them can drop the flag.
- Needs GNU extensions on, same reason as Plaits: `stmlib/dsp/filter.h` and
  several Clouds headers assume `M_PI` is visible, which strict ANSI hides.
- stmlib's inline assembly footprint used by the two Clouds-only files
  (`atan.h`/`atan.cc`, `shy_fft.h`) is ordinary NEON-compatible float code,
  no new architecture assumptions beyond what Plaits already exercises.
- Working set, from `clouds/clouds.cc`'s own buffer declarations (not
  vendored, but where the reference numbers below come from):
  `block_mem[118784]` + `block_ccm[65536 - 128]` ≈ 180 KB. On the module
  `block_ccm` lives in fast core-coupled RAM; on the Deluge both buffers are
  expected to come from the stealable SDRAM allocator instead (per the
  feasibility doc — internal RAM is the tight budget here, SDRAM is not).
  `clouds_adapter.cpp` currently allocates them with plain `new[]` as a
  scaffold; routing them through `GeneralMemoryAllocator`/`Stealable` is
  follow-up work, not done in this commit.

## Wiring status

The vendoring commit left this compiling but unreachable. It is now wired in
as a standalone GlobalEffectable effect on `feat/clouds-fx-13`:

- **Not a mod-FX type.** Mod FX has four parameter slots (Rate, Depth,
  Feedback, Offset) and Clouds needs nine, so it is its own stage at the head
  of the GlobalEffectable chain, ahead of mod FX, EQ, delay and reverb, with
  its own submenu under Song FX.
- **Nine automatable params**, appended to `UnpatchedGlobal` *after*
  `UNPATCHED_TEMPO` so no existing param ID moved: Position, Size, Pitch,
  Density, Texture, Blend, Spread, Feedback, Reverb. Pitch is the only
  bipolar one.
- **`CloudsMode`** (definitions_cxx.hpp) is the on/off switch and the
  playback-mode selector in one. OFF is the zero value and the default, and
  while it is selected no adapter exists and no working buffer is allocated.
- **Memory**: the ~180 KB working set is one `CloudsBuffer : Stealable`
  allocation from the SDRAM pool, following `dsp/granular/GrainBuffer`. It is
  pinned for the duration of one audio block and released at the end of each
  `process()`, so under pressure the allocator can take it back; the next
  block re-acquires and re-`Init`s. Losing it costs the recording, not
  stability.
- **Save/load**: all nine params plus `cloudsMode` and `cloudsFreeze`
  round-trip. The mode attribute is only written when Clouds is on, so songs
  that never touch it are unchanged; an absent attribute reads back as OFF.

### Measured cost

First successful hardware-toolchain build, `f2ac9a16`, arm-none-eabi-gcc
14.2.1, Release:

| | bytes |
|---|---|
| Binary before (`d2cd22a7`, Clouds vendored but unreachable) | 1,883,520 |
| Binary after (Clouds wired in) | 1,988,076 |
| **Delta** | **+104,556 (+5.6%)** |

From the linker map, by object:

| | `.text` | `.rodata` | `.data` | `.bss` | total |
|---|---|---|---|---|---|
| `clouds_dsp` (vendored DSP) | 58,940 | 47,330 | 568 | -- | 106,838 |
| `clouds_adapter.cpp` | 2,432 | 63 | -- | 708 | 3,203 |
| `stmlib_dsp` (shared with Plaits) | -- | 3,082 | 4 | 3,086 |

Map attribution (~113 KB) slightly exceeds the binary delta because
`stmlib_dsp` is shared with Plaits and was already partly linked in, and
because section packing differs. Take +104.5 KB as the real cost.

Note what is *not* in there: the ~180 KB working set. That is allocated at
runtime from the stealable SDRAM pool and never appears in the image, which
is the whole point of the `CloudsBuffer` design. `clouds_adapter.cpp`'s 708
bytes of `.bss` is just the adapter's own resampler rings.

### Cost this introduces

`kMaxNumUnpatchedParams` is the max across `UnpatchedSound` and
`UnpatchedGlobal`, and `UnpatchedParamSet` sizes its `AutoParam` array from
it. Adding nine global params therefore grows **every** `UnpatchedParamSet`,
Sounds included, by nine `AutoParam`s -- ~64 bytes each on ARM32 by field
layout (no vtable in `ResizeableArray`), so ~576 bytes per param manager.
That is the main thing to measure on hardware before this goes further; if it
proves too expensive the fix is a Clouds-only param array rather than
extending the shared one.

## Density has a dead zone -- this is upstream behaviour, not a fault

`ProcessGranular()` treats Density as a meta parameter and zeroes grain
overlap across the middle of its range:

```c
if (density >= 0.53f)      overlap = (density - 0.53f) * 2.12f;
else if (density <= 0.47f) overlap = (0.47f - density) * 2.12f;
else                       overlap = 0.0f;   // no grains
```

The audible dead zone is wider than that band, because overlap stays near
zero for a while either side. Measured against the engine compiled natively,
with everything else centred: silent from roughly 0.40 to 0.75, and confirmed
on hardware as menu positions 26-30 of 50.

This cost a long debugging session, because a Deluge q31 default of 0 maps to
density 0.50 -- dead centre. Clouds was therefore silent out of the box at any
Blend setting, which looks exactly like a broken port. **The default is now
menu 40 / density 0.80.** If Clouds ever appears silent again, check Density
before anything else.

## Still not done

1. The 44.1 kHz <-> 32 kHz resampler is still the plain linear interpolator,
   not `dsp/interpolation/`'s polyphase one. Measured round-trip SNR against
   a sine, upstream processor replaced by identity: **60 dB at 100 Hz, 46 dB
   at 500 Hz, 39 dB at 1 kHz, 31 dB at 2 kHz, 17 dB at 5 kHz, 7 dB at 10 kHz,
   3 dB at 14 kHz**, with ~44 samples (1 ms) of latency. Fine in the bass and
   low mids, poor in the top two octaves. Swap it before this is judged on
   audio quality. (Note also that Clouds' own 32 kHz rate puts Nyquist at
   16 kHz regardless of resampler quality.)
2. CPU headroom. The code-size half of the feasibility doc's question 1 is
   now answered (see "Measured cost" below); how much DSP time Clouds plus
   the resampling actually eats at 44.1 kHz is not, and needs a device.
3. Gold-knob / shortcut-grid assignments and automation-view entries. The
   params are automatable but are not on the shortcut grid.
4. Granular-specific sub-parameters upstream exposes (`granular.overlap`,
   `window_shape`, `reverse`, and the `spectral.*` block) are left at their
   defaults; only the nine main panel controls are mapped.
