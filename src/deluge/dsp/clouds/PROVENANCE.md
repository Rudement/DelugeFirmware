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

## Not done in this pass

This vendoring + the adapter in `../clouds_adapter.{h,cc}` gets Clouds
compiling and processing a 32 kHz buffer in isolation. Still open, per the
feasibility doc's own "open questions" section:

1. Effect-chain wiring — where Clouds hangs off the audio graph (it is an
   effect, not an oscillator source like Plaits; `GranularProcessor` doesn't
   fit the `Source`/`Voice` shape `plaits_adapter` uses). The doc points at
   `dsp/granular/GranularProcessor`'s existing stealable-buffer pattern as
   the closer precedent to study, not `plaits_adapter.*`.
2. Menu items / params / preset save-load, mirroring how `PLTS` did it.
3. The 44.1 kHz ↔ 32 kHz resampler in `clouds_adapter.cpp` is a plain linear
   interpolator, functionally correct but not the polyphase resampler
   `dsp/interpolation/` already provides elsewhere in this codebase. Swap it
   in before this is judged on audio quality.
4. Hardware measurement: `.text`/`.rodata` cost from the linker map, and
   whether resampling cost is trivial next to the Clouds engine itself
   (question 1 in the feasibility doc).
