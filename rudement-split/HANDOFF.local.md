# HANDOFF

Working notes for this fork. Last updated 2026-08-08.

---

## 1. RESOLVED — the remote does NOT have the backport

**The browser was right. The local tracking ref was lying.**

```
$ git ls-remote origin chopin
2991d5d837251dfb4f58b72fcaebebe2d204105b	refs/heads/chopin
```

`2991d5d8` is the upstream community commit ("Add a toggle for the new rounded corners
styling") that the GitHub page showed. It is **not** `4d68a0b1`. So:

- `origin/chopin` on GitHub contains **none** of the backport work.
- Everything — Sear, the EQ readout, the Gristleizer switch, the CI trigger — exists
  **only on this machine**. It is unbacked up. Treat the local repo as the sole copy
  until it is pushed.
- `refs/remotes/origin/chopin` locally still reads `4d68a0b1` and its reflog still says
  `update by push`. Both are stale/wrong. Do not trust either; `git ls-remote` is the
  only honest answer.

Why the reflog lies is not established. The likeliest story is that a push did land and
was later force-overwritten by something syncing the branch to upstream — GitHub Desktop
is the obvious suspect given section 6. Not proven.

**Before anything else: get the work onto a remote.** Push to differently-named branches
so nothing can overwrite it again, e.g. `git push origin chopin:chopin-rudement`.

---

## 1b. Branch split (2026-08-08)

The four features now each have their own branch, independently rooted, in addition to the
integrated `chopin` / `chopin-to-13` lines. Ten branches, delivered as a bundle plus import
and build scripts:

```
rudement-split\README.md              full detail
rudement-split\1-import-branches.cmd  import the ten branches + create beta-1.3
rudement-split\2-build-chopin.cmd     1.2.1-rudement, toolchain v16
rudement-split\3-build-beta13.cmd     1.3.0-rudement beta, toolchain v22
```

1.2.1 (base `a2e333b9`): `feat/sear-12`, `feat/eq-readout-12`, `feat/gristle-on-12`,
`feat/ci-chopin-12` — all four genuinely independent.

1.3 (base `134d000f`): `feat/gristle-13` (standalone Gristleizer port, a **real shared
prerequisite** — `gristle.hpp` does not exist at the 1.3 base), `feat/sear-13`,
`feat/eq-readout-13`, `feat/gristle-on-13`, `feat/midi-fx-13`, `feat/version-label-13`.

Verified lossless: re-stacking the split branches reproduces tree `a54fbe45` (= `chopin`
`33c4e258`) and tree `e203afbc` (= `chopin-to-13` `5cf8edeb`) exactly. The only edits made
during the split were Heat/Sear naming context and one stray `#include`; see the README.

---

## 2. Branch map

| Branch | Tip | Line | Notes |
|---|---|---|---|
| `chopin` | `4d68a0b1` | 1.2.1-rudement | Release branch. Has the backport + CI trigger commit. |
| `chopin-to-13` | `b6925826` | 1.3.0-rudement | Where features are written first. |
| `midi-fx` | `a99b641b` | **1.3.0-rudement** | **Corrected 2026-08-08.** This row previously read "1.2.1 lineage; does not contain the backport" — both wrong. `a99b641b^^` is `b6925826`, the tip of `chopin-to-13`, so `midi-fx` is 1.3 lineage and **does** contain all three features, plus two arpeggiator commits (`f067eac7`, `a99b641b`). It is effectively the full 1.3 beta; `beta-1.3` is created pointing here. |
| `mid-eq` | `132683c5` | — | Older EQ work. |
| `chopin-backport` | = `33c4e258` | — | Scratch branch, now redundant. Safe to delete. |

Toolchain differs by line: `chopin` needs **v16**, `chopin-to-13` needs **v22**
(`toolchain/REQUIRED_VERSION`). Both are installed. Changing branches across that boundary
requires `dbt nuke` — CMake caches the compiler path and a plain rebuild will silently use the
wrong one.

---

## 3. What landed on `chopin` (2026-08-08)

Three features backported from `chopin-to-13`, plus a CI fix. Full rationale is in each commit
message; only the 1.2.1-specific deviations are repeated here.

### `def3db2f` — Sear: rename from Heat, automatic level matching

- `heat.hpp` deleted, `sear.hpp` added. All XML tags renamed (`heat`→`sear`,
  `heatTone`→`searTone`). **No read-side alias was added** — confirmed no songs had ever been
  saved with the old tags. If that turns out to be wrong, add aliases on the read path only.
- **`kSearDriveBoost` was carried across by hand.** The +50% drive fix (`a2e333b9`, 11:37 that
  morning) existed only on `chopin`; the 1.3 Sear rename was written eight hours earlier and
  never saw it. A straight cherry-pick would have deleted it silently along with `heat.hpp`.
  Verified numerically after porting: ceiling 406x, realised ratio 1.42–1.58x, octaves reaching
  8, `octaves==0` window narrowing from k<6.25 to k<2.6 — all matching the documented table.
- 1.2.1's `Voice` is placement-constructed as a bare `Voice()` with no `Sound` in reach, so the
  auto-level seed moved from the constructor (where 1.3 puts it) into `noteOn()`, which does
  have one. Same net effect — both run per note-on.
- `fixedpoint.h`: added `subtract_saturation` (this branch had no `qsub` helper at all), and
  fixed the **host** `add_saturation`, which was plain `a + b` and wrapped instead of pinning.
  Host tests previously could not observe a saturation bug.

### `b5e2d60d` — EQ: report each band in Hz and dB

- `dsp/eq_bands.hpp` ported verbatim; `processFX()` now sources its coefficients from it rather
  than from inline duplicates. Values unchanged.
- The readout was **reimplemented**, not ported. 1.3 uses `getNotificationValue(StringBuf&)`;
  this branch has no `StringBuf`, no `getColumnLabel`, no `menu_item/eq/` hierarchy. It hangs
  off `drawInteger()` / `drawValue()` instead, both already virtual. Same shapes — `466Hz`,
  `4.87kHz`, `+12.0dB`, `OFF` — formatted with integer arithmetic to avoid float printf.
- 7SEG drops the unit and the `+` sign: four characters, and `-23.9` uses all of them.
- **The `globalParamToCC` fix from the original commit was deliberately not ported.** That bug
  is a consequence of 1.3's split forward/reverse CC maps. This branch uses the single
  `paramToCC[x][y]` grid read in both directions, so the mismatch cannot occur here.

### `33c4e258` — Gristleizer: master switch

- **`UNPATCHED_GRISTLE_ON` has no MIDI Follow CC and no grid shortcut on this branch.** 1.3
  assigns it CC 89; here 89 is already Low Mid Freq. On 1.2.1 a CC is inseparable from a grid
  position, and a sweep of all three shortcut tables against `defaultParamToCCMapping` found
  **zero** positions that carry a CC and hold no param. An earlier attempt silently stole Depth's
  CC 103. Menu and automation reach the switch; MIDI Follow does not.
- `UnpatchedParamSwitch` drops `getNotificationValue()` and `renderInHorizontalMenu()` (neither
  exists here) and uses `Canvas::invertArea()` where 1.3 uses
  `invertLeftEdgeForMenuHighlighting()`, matching `Toggle::drawPixelsForOled`.
- Automation array sizes went 74/39 → 75/40, not 1.3's 97/52 — stock param counts differ. Entry
  counts were verified against the declared sizes; a mismatch is a hard compile error.

### `4d68a0b1` — CI: build `chopin` on push

`pr-build.yml` only fired on pushes to `synthstrom-official` and PRs into `community` /
`release/**` — all upstream branch names inherited at fork time. Nothing matched `chopin`, so
pushing this branch built nothing. Added `chopin` to the existing push trigger.

Note this also means **`format-check.yml` does not run on these branches either.** Formatting is
only gated if you open a PR into `community`. Run `dbt format` locally for tidiness, not because
CI will catch it.

---

## 4. Build status

Compiles clean: **468/468 objects, zero errors**, toolchain v16.

```
build/Release/deluge-v1_2_1-rudement+2026_08_08-33c4e258.bin
```

Built at `33c4e258`; `4d68a0b1` is workflow-only, so the binary is current.

Two version-string traps:

- **`RELEASE_TYPE` decides the label.** Default is `release`, which takes the
  `-c${PROJECT_VERSION}` branch and reports `c1.2.0`, ignoring `DISPLAY_VERSION` entirely. Build
  with `dbt build release -m` to get `1.2.1-rudement`. This is pre-existing — commit `568127fd`
  never applied to plain release builds.
- **`-dirty` is cosmetic.** The build rewrites the generated `g_menus.inc` with CRLF while git
  stores LF. Content diff ignoring line endings is empty.

`PROJECT_VERSION` stays pinned at 1.2.0 on purpose — it feeds `BUILD_VERSION_STRING_SHORT`,
which `storage_manager.cpp` writes into every saved song. Songs stay byte-identical to stock.

---

## 5. Untested — needs ears on hardware

**Does `kSearDriveBoost` actually fix "too tame"?** It raises pre-gain *into* the clipper, and
the auto-leveller then removes the loudness it would have added. That is the intent — extra
drive lands as saturation rather than volume — but it is a different result from what was heard
on 2026-08-08, and no one has listened to it since the port.

If it needs tuning, it's one constant in `sear.hpp`. The documented alternative: scale `level`
by 1.073 instead of adding the offset — same 384x ceiling, removes the bottom-of-knob step, but
mid-knob positions get less than the full 1.5x.

Also unheard since the backport: Gristle On bypass behaviour (LFO should stay locked to the
timeline across a toggle), and the EQ readout — check Treble specifically, its dB figure tracks
the Freq knob where the bells deliberately don't, and above Freq knob 32 it should report ~0 dB
because the band genuinely stops responding there.

---

## 6. Environment hazards

These cost hours this session. All are environmental, none are code.

**GitHub Desktop moves your HEAD.** With it open, it switched branches mid-operation and ran a
`git reset` that moved the `chopin` pointer onto `chopin-to-13`'s tip (`chopin@{0}: reset:
moving to b6925826` in the reflog). Cancelling its branch-switch dialog did not stop it. It also
shows stale state for minutes at a time — at one point offering to commit 1,579 files to the
wrong branch. **Close it while doing git work; open it only to push** (it holds the credentials,
which is the one thing the command line here doesn't).

**Interrupted checkouts leave a hybrid tree.** Twice the working tree held one branch's files
while HEAD pointed at another, producing ~1,500 phantom "changes". Tells: both `heat.hpp` and
`sear.hpp` present, or `*.stranded` files (git's marker for a file it could not delete). Verify
with `git diff --stat <branch> -- .` before discarding anything — if it comes back empty or
mode-only, the tree is just that branch and nothing original is at risk.

**Large git operations time out.** `reset --hard` across 3235 files exceeds the tool timeout.
When the working tree already holds the right content, `git reset --mixed <sha>` moves the ref
and index without touching files and completes in seconds.

**Stale `.git/index.lock` — root cause found (2026-08-08).** Not GitHub Desktop. When this
folder is mounted into a Cowork sandbox it is exposed over FUSE, and that mount **permits file
creation but denies deletion**:

```
$ touch .git/__probe1   # create ok
$ rm .git/__probe1      # rm: cannot remove: Operation not permitted
```

So git creates a lock file, cannot remove it, and every later git operation in that repo fails
until the lock is deleted **from Windows**, where deletion works normally. Any git write run
from inside the sandbox self-poisons on the first lock it takes. Do git work from Windows;
from the sandbox, clone to sandbox-local storage first and deliver the result as a bundle.

Left behind by that probing, still needing manual deletion from Windows:

```
del  .git\__probe1
rmdir /s /q .git\worktrees\wt
git worktree prune
```

**The `pre-commit` hook cannot run on `chopin`.** It reads `.pre-commit-config.yaml`, which only
exists on the 1.3 line. Use `--no-verify` on this branch.

**Nine-plus GitHub Desktop stashes have accumulated** (`git stash list`) — all cross-branch
debris from the failed switches. Worth clearing.

---

## 7. Loose ends

- [x] `git ls-remote origin chopin` — settled. Remote does **not** have the work. See section 1.
- [ ] **Push the work somewhere safe.** It is currently single-copy on this machine.
- [ ] `del .git\__probe1` and `rmdir /s /q .git\worktrees\wt`, then `git worktree prune`.
- [ ] Run `rudement-split\1-import-branches.cmd`, then the two build scripts.
- [ ] Flash and listen (section 5).
- [ ] Delete `chopin-backport`.
- [ ] Clear the GitHub Desktop stashes.
- [ ] Delete `PIPELINE-TEST.md.stranded` and `src/deluge/dsp/heat.hpp.stranded`.
- [ ] Optional: `dbt configure -DENABLE_SYSEX_LOAD=YES` then flash once via SD, so subsequent
      builds can go over USB with `dbt loadfw release` instead of shuttling the card.
