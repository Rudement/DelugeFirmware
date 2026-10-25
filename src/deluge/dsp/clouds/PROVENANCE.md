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

**Oct 2026, all additive and marked `// Deluge:`** -- see "Spreading
upstream's periodic work across renders" at the end of this file:
`correlator.{h,cc}` (NEON popcount; budgeted search), `wsola_sample_player.h`
(resumable correlator load), `pvoc/stft.{h,cc}`, `pvoc/phase_vocoder.{h,cc}`,
`pvoc/frame_transformation.{h,cc}` (resumable STFT hop; NEON FFT passes),
`granular_processor.{h,cc}` (`PrepareSliced()`; `Prepare()` split into its
structural part and its periodic tail, otherwise unchanged; reverb and
diffuser skipped at zero amount), `fx/{reverb,diffuser}.h` (`Clear()`), and
`stmlib/fft/shy_fft.h` (read accessors for the bit-reversal table and phasor).
Upstream's own entry points are all still there and still behave as before.

**One sound change** (also marked `// Deluge:`): Oliverb's decay is
`density * 1.11` instead of `density * 1.3`, so self-oscillation starts at
density 0.9 (menu ~45) rather than 0.77 (menu ~38). The Deluge's default
Density is 0.8 (menu 40, out of Granular's dead zone), which with upstream's
scaling made Oliverb feed back from the moment it was switched on.
Everything else in `clouds/dsp/**` and `clouds/resources.*` is byte-identical. Unlike Plaits, Clouds does not centralise its sample rate in one
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

### Where the CPU goes

Measured by compiling the vendored engine natively and driving it through the
adapter's exact plumbing. Absolute figures are host figures and mean nothing
for the Deluge; the **ratios** are the transferable part.

| mode | total | engine | resampler's share |
|---|---|---|---|
| Granular | 0.369% | 0.300% | **18.6%** |
| Looping delay | 0.475% | 0.407% | 14.4% |
| Resonestor | 0.678% | 0.610% | 10.1% |
| Stretch | 0.745% | 0.677% | 9.2% |
| Oliverb | 0.792% | 0.724% | 8.6% |
| Spectral | 0.892% | 0.823% | 7.7% |

Two things follow, and the first contradicts an assumption in the feasibility
doc:

- **Resampling is not trivial next to the engine.** The doc's question 1 asked
  whether it would be; the answer is no. It is 8-19% of the whole stage, and
  nearly a fifth of it in Granular, the cheapest and most-used mode.
- **Therefore the polyphase swap has a real price.** Replacing 2-point linear
  interpolation with a windowed-sinc kernel multiplies the per-sample work in
  a stage that is already a fifth of the cost. Better audio, measurably more
  CPU. That trade should be made with the device numbers in hand, not
  assumed.

Spectral costs 2.4x Granular, so mode choice matters more than anything else
here.

Caveat: these are x86 ratios. On Cortex-A9 with NEON the engine's float DSP
vectorises well and the scalar resampler does not, so the resampler's share on
hardware is more likely to be higher than these numbers than lower.

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

## Two more parameters with non-obvious dead spots

Density's dead zone (above) is not the only one. Both were found by running
the engine natively rather than by reading it, and both silence a mode
outright at the value a Deluge q31 default would naturally pick.

**Spread at minimum silences Resonestor.** `stereo_spread` drives two settings
from one control:

```c
set_stereo(ss < 0.5f ? 0.0f : (ss - 0.5f) * 2.0f);
set_separation(ss > 0.5f ? 0.0f : (0.5f - ss) * 2.0f);
```

so `ss = 0` means `separation = 1.0`, and Resonestor emits nothing. Measured:
at Spread 0 it is silent for *every* combination of density, reverb, feedback
and distortion; at Spread 0.5 it produces output at the defaults. Spread now
defaults to centre.

**Blend/Return is distortion in Resonestor**:
`resonestor_.set_distortion(parameters_.dry_wet)`. The adapter keeps that
hidden control neutral (0) and does the Blend itself, as in every mode -- see
"Adapter audio path" below.

Also note Resonestor's feedback mapping cubes its input three times --
effectively `density^27` -- so it does nothing below about 0.9 and then rises
very steeply.

## Still not done

1. ~~Linear 44.1 kHz <-> 32 kHz resampler~~ -- replaced Oct 2026, see
   "Adapter audio path" below.
2. Absolute CPU headroom on the device. The *relative* costs are measured
   (see "Where the CPU goes" below); what fraction of the Deluge's budget one
   instance eats still needs a device test.
3. Gold-knob / shortcut-grid assignments and automation-view entries. The
   params are automatable but are not on the shortcut grid.
4. Granular-specific sub-parameters upstream exposes (`granular.overlap`,
   `window_shape`, `reverse`, and the `spectral.*` block) are left at their
   defaults; only the nine main panel controls are mapped.

## Adapter audio path (Oct 2026 rework)

Measured first with a native harness that compiles the real
`clouds_adapter.cpp` and this engine on a desktop, next to the bare engine run
at 32 kHz / 16-bit the way the module runs it. That separates what the adapter
does from what Clouds itself does. The engine was fine; the adapter was not.

Found, and fixed in `clouds_adapter.cpp` (nothing here in `clouds/` changed):

- **Level.** Upstream's last step is `SoftConvert()` = `SoftLimit(x * 0.5f)`:
  it halves the signal for the codec and the module's analog stage makes it
  back. Its dry/wet crossfade also peaks at 0.7071 at both ends. Nothing made
  up for either, so fully wet sat 7.5 dB under the input.
  The wet return is now scaled by 2 / 0.7071, which keeps upstream's own
  wet-to-dry ratio (its 1.2 post-gain): fully wet is +1.6 dB, as on the module.
- **Blend.** Any Blend above zero put the *dry* signal through upstream's
  internal mix, i.e. through the 32 kHz round trip and that halving: dry fell
  9 dB the moment Blend left zero and lost its top octave. The engine now runs
  fully wet in every mode and the adapter mixes against the exact q31 dry,
  using upstream's own equal-power curve (`lut_xfade_in/out`, normalised to
  unity ends), smoothed per sample. Blend 0 is still an exact bypass; reaching
  it takes one block that ramps the wet return and the engine's input out
  (toggle click: -8 dB -> -46 dB re the tone).
- **Resampler.** Linear interpolation, no filtering: -4 dB at 10 kHz,
  -9 dB at 14 kHz, distortion/alias products only 20-33 dB under 4-8 kHz tones,
  and everything above 16 kHz folded back into the audio. Now a Kaiser-windowed
  sinc polyphase pair (48 taps at 44.1 kHz down, 32 taps at 32 kHz up, cutoff
  15.5 kHz, beta 9, kernel tables 128x oversampled in SDRAM, built once off the
  audio thread). Phases are exact integers (441:320), so no drift. Measured
  against the bare engine: flat (+/-0.05 dB) to 13 kHz, -0.8 dB at 14 kHz;
  residual junk -63 to -66 dB, which is the engine's own floor (identical in the
  reference); input above 17 kHz is gone.
- **Fixed latency.** 94 samples (2.1 ms) on the wet path. The output side
  only ever reads engine frames that a complete block has produced, for any
  sequence of render block sizes: output is bit-identical for blocks of 1, 7,
  128 or random 1..256 (after the first block's Blend ramp), zero under-runs.
- **16-bit input.** Rounded instead of truncated. The engine records 16-bit
  audio by design, so precision is otherwise unchanged; it only matters below
  about -50 dBFS.
- **Spectral stalls.** The STFT needs its periodic `Prepare()` within one hop
  (32 ms); later than that it played stale frames (-5 dB of error). The
  background task's ceiling is now 20 ms (was 50 ms), and if it stalls anyway
  -- song load, SD card -- `process()` runs the overdue `Prepare()` itself
  after 25 ms, unless the task is inside one. Bit-identical output even with
  the task never running.
- **Freeze** is re-applied after any in-render engine reset (upstream's reset
  path clears it).

Cost, first pass: on the desktop the new resampler looked cheap (~0.2% of
real time). It was not on the Deluge -- see below.

Not a bug, confirmed: the Granular feedback cap (x0.225). The bare engine
itself runs away from feedback 0.2 at high Density.

## On the Deluge: what ARM code costs (Oct 21 2026)

On-unit testing of the first rework sounded quieter, broke up and turned
gritty in Spectral, which the desktop harness could not reproduce. So the
harness was cross-compiled for the Deluge's CPU with the firmware's own flags
(`-mcpu=cortex-a9 -mfpu=neon -mthumb -O2 -ffast-math -funsafe-math-optimizations`)
and run under qemu-arm, counting executed instructions.

The finding that matters: **on the Deluge the audio is rendered from the main
loop**, with a 128-sample (2.9 ms) output buffer and voice culling once a render
falls ~50 samples behind. On the module, audio runs in an interrupt and
`Prepare()`'s heavy work in the main loop cannot block it. Here it can.

Measured per call, in ARM instructions (A9 at 400 MHz: very roughly
200-400 million a second on this code):

| mode | per 128-sample render | worst background `Prepare()` |
|---|---|---|
| Granular | 140k | -- |
| Stretch | 143k | 715k (correlator) -> 382k after NEON |
| Delay | 150k | -- |
| Spectral | 100k | **2,360k** (one STFT hop, both channels) |
| Oliverb | 210k | 696k (correlator) -> 369k after NEON |
| Resonestor | 196k | -- |

Spectral's hop was the dropout heard on the unit: two 4096-point FFT+IFFT
pairs (670k instructions each, shy_fft) plus the frame transformation, all in
one go every 32 ms. Fixed -- see the next section.

Changes from this:

- **Adapter rewritten for ARM.** The first rework's polyphase loops compiled to
  scalar VFP and cost ~26 M instructions/s per instance. Now hand-written NEON
  (per-channel mirrored histories, four taps per instruction), chunked
  processing with a NEON mix/soft-knee/q31 stage, and no `lrintf` library
  calls: ~13 M/s. NEON build matches the reference to -113 dB.
- **`clouds/dsp/correlator.cc` -- first deliberate edit to upstream.** The
  WSOLA popcount loop has a NEON path (`vcnt`), bit-for-bit identical
  (verified over 2 s of Stretch and Oliverb on ARM). Typical background call
  322k -> 88k instructions.
- **Poison guard was never compiled in.** `-ffast-math` lets GCC delete
  `std::isfinite()`; the NaN check now reads the exponent bits.
- **Soft knee** before full scale: dense Granular clouds on a near-full-scale
  input clipped hard once the wet was at the module's level.

## Spreading upstream's periodic work across renders (Oct 21 2026)

On-unit SETTINGS > CPU readings: no culls in any mode, but Window peak (DMA
advance between renders; 128 = buffer ran dry) reached 82-125 in Stretch,
Spectral, Oliverb and others, against 19 with Wisps off.

The background task is gone. Its work -- Spectral's STFT hop, Stretch and
Oliverb's WSOLA load and search -- now runs inside `process()`, a slice per
chunk, paid for from a per-sample allowance (`sliceCredit_`: 2200 instructions
per sample in Spectral, 1600 for the WSOLA modes; debt from a step that
overran carries to the next call). Upstream gets resumable versions of each:

- `STFT::BufferStep()`: one hop is window, 11 FFT passes, the 11 steps of
  `FrameTransformation::ProcessStep()` (RectangularToPolar in four bin ranges,
  then upstream's own calls in upstream's order), 11 inverse passes,
  overlap-add. The FFT passes are transcribed from `stmlib::DirectTransform` /
  `InverseTransform` (the phasor restarts per block, so stopping between passes
  changes nothing). Both channels share FFT scratch, so a hop always completes
  before the other channel's starts -- same order as `Buffer()`.
- `WSOLASamplePlayer::LoadCorrelatorStep()`: the sign-bit load, one 32-sample
  correlator word per step; bit k is read at phase k * increment, as
  `ReadSignBits()` accumulates it. `ScheduleAlignedWindow()` bumps a generation
  counter, which restarts a load in progress.
- `Correlator::EvaluateCandidates()`: the same search, on a budget.

Measured (native, strict IEEE): Spectral and Stretch output **bit-identical**
to upstream's `Prepare()` called every 32 samples -- i.e. to upstream run
promptly, which the old 5 ms task was not (Stretch under the 5 ms task
differed from that by +0.7 dB). Oliverb's output depends on search timing in
upstream too (5 ms task vs 32-sample: +3.8 dB difference), level unchanged.

Worst single render on ARM, 19-sample renders (instructions):

| mode | before | after |
|---|---|---|
| Granular / Delay / Resonestor | 41k / 43k / 57k | unchanged |
| Spectral | 2,360k (one background call) | 153k |
| Stretch | 382k (background) + render | 130k |
| Oliverb | 369k (background) + render | 152k |

Average cost fell for the WSOLA modes (Stretch 92 -> 70 M instructions/s,
Oliverb 110 -> 91 M/s on ARM): the old task evaluated a quarter of all
candidates per call whether or not the result would still be used.

Also: the resampler's kernel tables are generated at build time
(`clouds_adapter_kernels.h`, 40 KB in `.sdram_rodata`, from
`scripts/clouds/gen_clouds_kernels.cpp`) -- building them on the
unit was 4.3M instructions the first time Wisps was switched on; first enable
is now 112k (the buffer clear).

## Spectral in mono, NEON FFT (Oct 21 2026)

On the unit, after the slicing: every mode ran with zero culls except
Spectral -- DSP 58 average, peak 266 samples, 3,485 hard culls, audible
crackle. Instruction counts had undersold it: the hop sweeps ~200 KB of SDRAM
(analysis/synthesis rings, FFT scratch, seven texture frames per channel)
every 32 ms, far beyond the caches. The old background task had hidden the
same cost as output stalls.

- **Spectral runs mono** -- upstream's own quality setting
  (`set_num_channels(1)`): one STFT channel, input mixed to mono, result copied
  to both outputs. The adapter's dry path stays stereo. Other modes stay stereo.
  `CloudsAdapter::applyPlaybackMode()`.
- **NEON FFT passes** (`pvoc/stft.cc`, passes with n/2 >= 8): twiddles from an
  exact cos/sin table (16 KB, `.sdram_bss`, built on first `STFT::Init()`)
  instead of RotationPhasor's serial recurrence, four butterflies per
  instruction. Not bit-identical to the recurrence (slightly more accurate);
  Spectral output on ARM differs from the scalar build by -79 to -91 dB, same
  level, same band balance. Non-NEON builds (the native harness) keep the
  scalar passes, so the bit-identity results above still hold there.
- Spectral's allowance drops to 1500 instructions per sample (a mono hop is
  charged ~1.16M, so it completes in ~18 of its 32 ms).

ARM, per half second of audio: Spectral 52.7M -> 30.1M instructions (stereo
scalar -> mono NEON), now below Stretch (35.3M) and Oliverb (45.7M), both of
which ran without culls on the unit.

## Headroom (Oct 21 2026, evening)

On the unit after the mono change, all six modes ran without culls, but DSP
peak against a Wisps-off baseline of 22 samples was 70 (Granular), 81
(Delay), 85 (Resonestor), 110 (Oliverb), 129 (Spectral), 133 (Stretch) --
close to the 80-sample cull threshold. Four changes, measured on ARM:

- **Engine blocks of 16 frames** (`kEngineBlockFrames`), not upstream's codec
  block of 32. The engine takes any size up to 32. A ~19-sample Deluge render
  now carries at most one half-size block instead of a whole one every second
  or third render. +2-4% total work.
- **Reverb and diffuser skipped at zero amount** in `GranularProcessor::Process`.
  At amount 0 both return their input exactly, so the skip changes nothing; they
  are cleared on their next use so no stale tail replays. The reverb alone was
  ~20% of Granular's cost on ARM with Reverb at 0.
- **Slice work charged at measured cost, in finer steps.** Window split in 2,
  overlap-add in 4 (it was 92k instructions in one step), SetPhases split at its
  two loops (`FrameTransformation` now 12 steps), each step charged its
  measured ARM cost, and a render stops before a step that would overrun its
  share rather than after. Correlator candidates charged with a floor (they
  cost ~260 instructions each; small windows were undercharged by half).
- **One slice call per render**, spending its own accrual x 1.25 or 30k,
  whichever is larger; any backlog carries. A render that was long for another
  reason no longer also pays off the backlog.

Spectral, Stretch and Oliverb output is now **identical whatever the render
sizes** (128-sample, 19-sample, random 1-128) -- the background work always
finishes in time. (Oliverb was timing-dependent before.)

ARM, total per half second of audio / worst single 19-sample render:

| mode | before | after |
|---|---|---|
| Granular | 23.6M / 40.5k | 17.7M / 18.3k |
| Delay | 25.6M / 42.6k | 21.1M / 23.0k |
| Resonestor | 33.2M / 56.9k | 33.9M / 33.2k |
| Stretch | 35.3M / 129.9k | 30.8M / 69.6k |
| Oliverb | 45.7M / 152.1k | 46.1M / 84.7k |
| Spectral | 30.1M / 135.3k | 25.9M / 52.4k |

Granular, measured: our port's grain choppiness matches the bare engine at
every setting tried. At the Wisps defaults (Density 40, Texture 25, Size 25)
the wet output on a steady pad is near-silent ~10% of the time; at Density 47,
Texture 36, Size 35 the gaps go and the flicker halves. That is Clouds.
