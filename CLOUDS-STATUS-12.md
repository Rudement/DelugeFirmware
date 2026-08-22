# Clouds on 1.2.1 — status

Branch `feat/clouds-12`. Read this and `CLOUDS-STATUS.md` (the 1.3 notes)
before touching anything. Everything in the 1.3 document about the engine
itself — the Density dead zone, the `.bss` assumption, `Prepare()` being a
main-loop function, Blend being distortion in Resonestor — applies here
unchanged, because the vendored tree is byte-identical.

## What this branch is

The 1.3 insert design, ported. Clouds is an insert at the head of the Song /
Kit / audio-clip FX chain, ahead of mod FX, EQ, delay and reverb. Blend is its
dry/wet. Not available on synth clips — Clouds' nine params are
`UNPATCHED_GLOBAL`, and a Sound's unpatched set is `UnpatchedSound`, which is
shorter; reading them off a Sound runs off the end of that array.

Ported from `feat/aux-sends-oled-kitsplit-13` (`0d1d9e51`), which is the insert
build carrying the Spread-centre fix and the off-thread `Prepare()` work.
**Not** from `origin/feat/clouds-fx-13` (`84cfc3a2`) — that ref still contains
the send experiment, `UNPATCHED_CLOUDS_SEND` and all. The clean insert delta
is `d2cd22a7..8c991733` (the local branch tip, post-revert); `0d1d9e51` is that
plus the fixes, and is what was actually used.

## Base state

`0d1d9e51` was flashed and all six modes confirmed on hardware before this port
was written. A fault found from here on is this port's, not the base's.

## Decisions specific to this line

**Enum position.** The nine `UNPATCHED_CLOUDS_*` go **after**
`UNPATCHED_CV1_MASTER` / `UNPATCHED_CV2_MASTER`, which is the opposite of the
1.3 line, where Clouds landed before them. On 1.3 the CV masters arrived after
Clouds; on 1.2.1 they already shipped. These IDs are the index automation and
MIDI-follow store, so anything inserted above an existing entry silently
repoints saved automation onto a different parameter. New global params go on
the end, always — the end is just in a different place on each line.

**Nine params are menu-only.** None of the eleven Clouds items appears in a
shortcut grid. All three 1.2.1 grid tables were swept against
`defaultParamToCCMapping` and there is no free CC-bearing position left: every
grid cell carrying a CC is already spoken for, so adding Clouds would have to
evict an existing param's CC and break MIDI-learn for it in songs already
saved. The same call was made for Gristle On. Clouds is reachable through the
vertical `Submenu` tree only — `globalFXMenu` and `audioClipFXMenu`.

**`mode.h` is not byte-identical to 1.3.** One line: 1.2.1's
`Selection::getOptions()` takes no `OptType` — that parameter arrives with the
1.3 menu rework. Signature adapted, body unchanged.

**Sear, not Heat.** No `heat.hpp`, no `LOCAL_HEAT`, no `STRING_FOR_HEAT*`, no
`"heat"` / `"heatTone"` XML tags anywhere in this branch. Verified by grep.

**`EqGainParam` / `EqFreqParam` stay non-`final`** — `rf::Gated<>` derives from
them for the Four-Band EQ toggle. Untouched by this port.

## Files beyond the seven the port plan listed

- `model/settings/runtime_feature_settings.{h,cpp}` and
  `gui/menu_item/runtime_feature/settings.cpp` — `EnableCloudsFX`. The vendored
  `mode.h` calls `runtimeFeatureSettings.isOn(...EnableCloudsFX)`, so without
  this nothing compiles. On by default, consistent with the other Rudement
  toggles; it hides menus only, the DSP keeps running.
- `gui/l10n/seven_segment.json` — `CLDS` for the community-feature entry,
  matching 1.3.
- `gui/l10n/g_english.cpp` and `g_seven_segment.cpp` are **generated**. They
  were regenerated with `generate.py`, which also picked up thirteen strings
  that had gone stale in the checked-in copies (Kit Split, the community
  feature names). Never hand-edit these.

## Verified

- Builds and links clean with toolchain **v16**, `Ninja Multi-Config`, Release.
- `static_assert(validateParams())` passes with the nine names in range —
  that is a **compile-time proof** that every Clouds param round-trips
  `paramNameForFile` → `fileStringToParam`, so save/load by name works.
  `fileStringToParam` needed no edit: it iterates and calls
  `paramNameForFileConst`, and `kMaxNumUnpatchedParams` derives from
  `UNPATCHED_GLOBAL_MAX_NUM`, so the new params are inside its range.
- `static_assert(kMaxNumUnpatchedParams < STATIC_START)` still passes — nine
  more unpatched params do not collide with the static range.
- No duplicate `stmlib` symbols in the linked ELF. The hoist to a shared
  `stmlib_dsp` target holds; `random.cc` and `units.cc` are defined once.
- No Heat symbols. No Clouds entries in any shortcut table.

## Size, measured

`f4b73ca4` (the branch point, no Clouds) vs this branch, same toolchain and
flags:

| | base | with Clouds | delta |
|---|---|---|---|
| `.text` | 1,469,148 | 1,579,348 | **+110,200** |
| `.data` | 49,072 | 50,264 | +1,192 |
| `.bss` | 442,212 | 442,600 | +388 |
| binary | 1,518,224 | 1,629,640 | **+111,416 (+7.3%)** |

Comparable to the 1.3 measurement (+103,348 `.text`). `.bss` barely moves
because the ~180 KB working set is runtime-allocated from the stealable SDRAM
pool, exactly as on 1.3.

## Host-harness results (this line's vendored engine)

The harness in the 1.3 notes was rebuilt against this branch and run on the
defaults actually committed here. All six modes produce output; Blend forced
full wet, 20 s of 220 Hz sine, RMS over the final 0.4 s:

| mode | wet RMS | vs dry |
|---|---|---|
| Granular | 7,595 | 0.67x |
| Stretch | 4,167 | 0.37x |
| Looping delay | 4,202 | 0.37x |
| Spectral | 10,514 | 0.93x |
| Oliverb | 9,089 | 0.80x |
| Resonestor | 3,299 | 0.29x |

Two things this confirmed, and one it corrected:

- **The Density dead zone is real and the default clears it.** Sweeping
  Granular: density 0.40, 0.50 and 0.60 give exactly 0.0 RMS; 0.70 gives
  2,571 and the committed 0.80 gives 6,810. The 1.3 figure of 0.47-0.53 is
  narrower than what is actually audible, as that note suspected.
- **Granular needs about one second to fill before it makes any sound.** A
  0.4 s measurement reads as digital silence and looks exactly like the
  memset trap. Not a fault; do not go hunting for one.
- **The Resonestor/Spread finding is one channel, not the whole mode.** At
  Spread 0 the LEFT channel is exactly 0.0 RMS across every combination of
  feedback, reverb and distortion swept, while the right keeps producing
  ~3,300. At the committed centre both channels sit level at ~3,300. The 1.3
  note recorded this as the mode being silent outright; on a mono monitor
  that is indistinguishable, but the accurate statement is left-channel loss.

The harness cannot model the device's threading, in-situ block-size variation
or memory pressure -- which is exactly where the remaining open faults live.

## NOT tested on hardware

This branch has never been flashed. Everything below is inherited from the 1.3
notes and carried forward unverified on this line:

| Symptom | Notes |
|---|---|
| Crackle | Present in all modes under the send on 1.3. Never tested on the insert, on either line. |
| Stretch drops out | Recovers. Only Stretch. Rate-limiting `Prepare()` did not fix it. Costs ~2x Granular; may simply be too expensive. |
| Click on mode change | Reduced by a 20 ms fade, never eliminated. |

First hardware run should be: each of the six modes in turn on a Song FX chain,
then Resonestor specifically at default Spread (it must not be silent), then a
save/load round-trip of all nine params plus mode and freeze.

## Build

```
.\dbt.cmd nuke          # required when switching lines; CMake caches the compiler path
.\dbt.cmd build release -m
```

The `-m` matters: without it `RELEASE_TYPE=release` labels the binary `c1.2.0`
and ignores `DISPLAY_VERSION`. Commit with `--no-verify` on this line — the
pre-commit hook reads `.pre-commit-config.yaml`, which only exists on 1.3.
