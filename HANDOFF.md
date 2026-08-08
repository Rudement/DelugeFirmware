# Handoff — Heat / Sear, and the state of the fork

Last updated 2026-08-08. The DSP comments in `sear.hpp` and `heat.hpp` say "See HANDOFF.md";
this is that file.

## Branch map

| Branch | Base | Drive stage | Other extras | Build label |
|---|---|---|---|---|
| `chopin` | 1.2.1 (`release_1_2_1`) | **Heat**, wired | Mid EQ → four-band EQ, Gristle → Gristleizer standalone, grid shortcut fix | `1.2.1-rudement` |
| `chopin-to-13` | 1.3.0 | **Sear** (Heat renamed + reworked), wired | Gristleizer, four-band EQ | `1.3.0-rudement` |
| `heat` | 1.2.1 | Heat, original feature branch | — | inherits |
| `mid-eq`, `grid-pulse` | 1.2.1 | — | earlier feature branches | — |

`PROJECT_VERSION` is deliberately left at upstream's value on both lines. It feeds
`BUILD_VERSION_STRING_SHORT`, which `storage_manager.cpp` writes into every saved song as the
`firmwareVersion` attribute. `DISPLAY_VERSION` carries the `-rudement` label instead, and only
affects the output filename and the on-device Firmware Version screen, so song files stay
byte-identical to stock.

**Known gap:** the `RELEASE_TYPE STREQUAL "release"` path in `CMakeLists.txt` still uses
`-c${PROJECT_VERSION}` for the filename, so a true release build would come out as
`deluge-c1_2_0.bin` with no `-rudement` and the wrong version. Dev builds are unaffected. One
line in each of `CMakeLists.txt` and `src/deluge/version/CMakeLists.txt` fixes it.

## Heat (1.2.1) — +50% drive, 2026-08-08

Reported too tame on hardware. Applied as a constant offset in exponent space rather than a
per-sample multiply, so the octave/fraction split stays exact and the cost is one add per
buffer:

```cpp
constexpr q31_t kHeatDriveBoost = 39258415; // round(log2(1.5) * (1 << 26))
const q31_t boosted = level + kHeatDriveBoost;
```

Verified through the real fixed-point path, not the ideal formula:

| knob | 0 | 3 | 5 | 10 | 15 | 20 | 25 | 30 | 35 | 40 | 45 | 50 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| before | 1.0 | 1.5 | 1.8 | 3.2 | 5.6 | 9.6 | 16 | 29 | 51 | 90 | 154 | 256 |
| after | 1.6 | 2.1 | 2.8 | 4.7 | 7.9 | 14 | 25 | 44 | 76 | 127 | 228 | 406 |

Two consequences, both documented at the call site:

- The ceiling lands on ~406x, not the nominal 384x, and the realised ratio wobbles 1.42–1.59x
  by knob position. That is the pre-existing linear-interpolation-in-the-exponent
  approximation (`2^s * (1 + f)` overshoots `2^(s+f)` by up to 6% mid-octave), not new error.
- Gain now starts at ~1.5x rather than 1.0x the instant the knob leaves its stop. Inherent to a
  uniform boost. Scaling `level` by 1.073 instead would remove the step at the cost of less
  boost mid-knob.

The `octaves == 0` guard comment was updated with it: the zero window narrows from k < 6.25 to
k < 2.6 of 50. That guard is the one tied to the earlier muting bug, so the stated range has to
stay accurate.

Landed on `chopin` as `a2e333b9 "Chopin 8.8.26"`.

## Sear (1.3) — not yet boosted

Heat became **Sear** on the 1.3 port: `LOCAL_SEAR`, `searMenu` in `menus.cpp`, an automation
entry, l10n strings. Wired and working.

It is not a straight rename. `heatMakeup()` — a static 1.0 → 0.75 droop across the sweep — was
replaced with an envelope-following auto-gain: a `SearLevel` struct tracking mean |sample| in
and out of the clipper, correcting toward unity, with a warmup period
(`kSearWarmupSamples = 1 << kSearDetectorShift`).

**This changes what a drive boost does.** On Heat, extra pre-gain shows up as both dirt and
level. On Sear, the follower actively claws the level back, so the same boost reads mostly as
added harmonic content. Decide which is wanted before copying `kHeatDriveBoost` across:

1. Same 1.5x offset as chopin — curves stay matched, output level stays put.
2. Boost plus a cap on how far the follower may correct — louder, but partly undoes the reason
   auto-gain was added.
3. Model `searBuffer`'s fixed-point path with the follower included and pick a number from the
   measured table.

`src/deluge/dsp/heat.hpp.stranded` on this branch is dead — renamed out of the way in
`5ea71d2a "mid-eq"`, superseded by `sear.hpp`, referenced by nothing. Safe to delete; git
history keeps it.

## Gotchas that have cost time

- **pre-commit hook.** `.git/hooks/pre-commit` runs the `pre-commit` framework, but only the
  1.3 line has a `.pre-commit-config.yaml`. Committing on `chopin` fails with "No
  .pre-commit-config.yaml file was found" until `PRE_COMMIT_ALLOW_NO_CONFIG=1` is set in the
  environment (GitHub Desktop must be restarted to pick it up).
- **Stale lock files.** A crashed git operation can leave `.git/index.lock`, `.git/HEAD.lock`,
  `.git/ORIG_HEAD.lock` and `.git/refs/heads/<branch>.lock`. GitHub Desktop then reports "A
  lock file already exists" and, if the index is half-written, shows ~1,500 phantom changed
  files. Fix: close GitHub Desktop, delete all four, `git reset`.
- **Branch switching with uncommitted work.** Choosing "bring my changes" during a switch is
  what produced most of the stash pile; prefer committing first.

## Build

```
.\dbt.cmd build release
```

Output in `build\Release\`. With `RELEASE_TYPE=dev` in the CMake cache the binary is named
`deluge-v<DISPLAY_VERSION>+<date>-<sha>.bin`. The toolchain in `toolchain/` is `win32-x64`
only.
