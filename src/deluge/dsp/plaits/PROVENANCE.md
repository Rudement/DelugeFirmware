# Provenance — Mutable Instruments Plaits

Vendored 2026-08-15 for the Deluge community firmware.

## Upstream

| | |
|---|---|
| Plaits | `https://github.com/pichenettes/eurorack` @ `08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4`, subdirectory `plaits/` |
| stmlib | `https://github.com/pichenettes/stmlib` @ `d18def816c51d1da0c108236928b2bbd25c17481` |
| Version | Plaits firmware 1.2 — 24 engines (16 original + 8 added in the 1.2 update) |
| Author | Émilie Gillet |
| Licence | **MIT** — see `LICENSE-MIT`. MIT is GPL-3.0 compatible, so this can be incorporated into the Deluge firmware. Every original copyright header is intact and must stay that way. |

Mutable Instruments closed in 2022 and released these sources deliberately for
reuse. There is no permission to seek and nobody to seek it from.

## What was taken

- `plaits/dsp/**` — all 24 engines, physical modelling, speech, FM, chords, FX
- `plaits/resources.{cc,h}` — wavetables and lookup tables
- `stmlib/` — only the 13 headers and 3 sources Plaits actually includes

## What was NOT taken

Module-specific code with no Deluge meaning: `plaits.cc`, `ui.*`,
`pot_controller.h`, `settings.*`, `drivers/`, `bootloader/`,
`user_data_receiver.*`, `hardware_design/`, `test/`.

## Deliberate modifications

Kept to the absolute minimum so that re-vendoring from upstream is a copy
rather than a merge. Every change is marked `DELUGE PORT` in the source.

| File | Change |
|---|---|
| `plaits/dsp/dsp.h` | `kSampleRate` and `kCorrectedSampleRate` 48000 / 47872.34 → 44100 |
| `plaits/dsp/fx/diffuser.h` | one hardcoded `48000.0f` → `44100.0f` |
| `plaits/user_data.h` | **replaced** with a stub. Upstream reads a user wavetable bank out of STM32 internal flash and includes `<stm32f37x_conf.h>`; the stub reports "no user data", which is what an unprogrammed Plaits reports. |

Nothing else in the tree is modified. `plaits/dsp/**` and `stmlib/**` are
otherwise byte-identical to upstream, which is what makes the 1.2.1 backport a
straight copy of this directory.

## Build notes

- Needs GNU extensions on: strict ANSI hides `M_PI`, which
  `stmlib/dsp/filter.h` and `plaits/dsp/oscillator/sine_oscillator.h` use.
- stmlib's inline assembly (`vsqrt.f32`, `ssat`, `smmul`) is valid on the
  Cortex-A9 with `-mfpu=neon -mfloat-abi=hard`, the same as on the Cortex-M4F
  it was written for. No changes were needed.
