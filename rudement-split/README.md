# Branch split — 2026-08-08

Ten feature branches, each independently rooted at its line's base, plus scripts to
import them and produce both builds.

## Run these in order, from the repo root

**Close GitHub Desktop before scripts 1–3** (handoff section 6 — it moves HEAD
mid-operation). Script 0 is safe to run with it open.

```
rudement-split\0-cleanup-locks.cmd     <-- RUN THIS FIRST, git is broken until you do
rudement-split\1-import-branches.cmd
rudement-split\2-build-chopin.cmd
rudement-split\3-build-beta13.cmd
```

---

## FIRST: git is currently jammed, and it is my fault

`.git\index.lock` exists right now. **Every git operation in this repo will fail
until it is deleted, GitHub Desktop included.**

The Cowork sandbox mounts this folder over FUSE, which **permits file creation but
denies deletion**. Git took an index lock, could not release it, and left it behind.
This is also the real cause of the "stale `.git/index.lock`" hazard in handoff
section 6 — not GitHub Desktop.

`0-cleanup-locks.cmd` removes it, along with the other debris I could not delete
(`.git\__probe1`, `__probe3`, and the phantom worktree `.git\worktrees\wt`). Deletion
works normally from Windows; the restriction is only on the sandbox side.

Equivalent by hand:

```
del /f /q .git\index.lock .git\__probe1 __probe3
rmdir /s /q .git\worktrees\wt
git worktree prune
```

**Do not run further git commands from the Cowork sandbox against this folder** —
each one can leave another lock. Git work belongs on the Windows side.

---

## Backup status — better than section 1 of the handoff implied

Checked per-commit, not per-branch:

| Line | Remote state | Verdict |
|---|---|---|
| 1.3 (`chopin-to-13`) | `origin/midi-fx` = `a99b641b` = local | **Safe.** All seven 1.3 commits are ancestors of `origin/midi-fx`. |
| 1.2.1 (`chopin`) | `origin/chopin` = `2991d5d8` (upstream) | **Single copy.** `def3db2f`, `b5e2d60d`, `33c4e258`, `4d68a0b1` exist nowhere but this disk. |

Note also that `origin/chopin-to-13` is **also** at `2991d5d8`. Two different branches
on the remote both sitting on the same upstream community commit is not a coincidence —
something force-synced them to upstream. The 1.3 work survived only because it also
lives on `midi-fx`, which was pushed under a name that sync did not touch.

### Pushing chopin safely

Local `chopin` (`4d68a0b1`) and `origin/chopin` (`2991d5d8`) have **diverged**. GitHub
Desktop will therefore offer you two things, and both are wrong:

- **Pull** — merges 900+ upstream commits into your release branch.
- **Force push** — overwrites the remote, and if the local ref is ever wrong you lose
  the work with no remote copy to recover from.

Push under a **new name** instead. Nothing to diverge, nothing to overwrite:

```
git branch chopin-rudement chopin
```

then in GitHub Desktop: branch selector -> `chopin-rudement` -> **Publish branch**.

Do the same for the ten imported feature branches once script 1 has run.

Every branch below contains its feature and **nothing else** — no other feature's
commits underneath it. Each can be pushed, PR'd, reviewed, or dropped on its own.

### 1.2.1 line — base `a2e333b9` (Chopin 8.8.26)

| Branch | Tip | Commits | Contents |
|---|---|---|---|
| `feat/sear-12` | `df7cb689` | 1 | Sear rename from Heat + automatic level matching |
| `feat/eq-readout-12` | `7befe053` | 1 | EQ reports each band in Hz and dB |
| `feat/gristle-on-12` | `7f6c3896` | 1 | Gristleizer master switch (the "off") |
| `feat/ci-chopin-12` | `18c7dedb` | 1 | Build `chopin` on push |

### 1.3 line — base `134d000f` (EQ four-band on 1.3)

| Branch | Tip | Commits | Contents |
|---|---|---|---|
| `feat/gristle-13` | `6c02c5d6` | 1 | Gristleizer standalone port — **shared prerequisite** |
| `feat/sear-13` | `a0da0808` | 2 | `feat/gristle-13` + Sear |
| `feat/eq-readout-13` | `5b23e396` | 1 | EQ Hz/dB readout + reverse CC map fix |
| `feat/gristle-on-13` | `75ab6c82` | 2 | `feat/gristle-13` + master switch |
| `feat/midi-fx-13` | `e3402639` | 2 | Both arpeggiator fan-out collapses |
| `feat/version-label-13` | `5d4d0d22` | 1 | Label builds 1.3.0-rudement |

---

## Why the 1.3 line has a shared prerequisite

`src/deluge/dsp/gristle.hpp` **does not exist** at the 1.3 base. It is created by
`6c02c5d6`. Both Sear and the master switch modify that file, so neither can sit
directly on `134d000f` — this is a real dependency, not a naming artefact.

On 1.2.1 it does not arise: the standalone Gristleizer (`403fa750`) is already in
the base, so all four branches are genuinely independent there.

`feat/eq-readout-13` and `feat/midi-fx-13` need no prerequisite and sit on the bare base.

---

## What had to be adapted, and what did not

`feat/sear-12`, `feat/eq-readout-12`, `feat/ci-chopin-12`, `feat/gristle-13`,
`feat/sear-13`, `feat/midi-fx-13` and `feat/version-label-13` are byte-identical
replays of the original commits.

Three needed edits, all of them **naming context, not logic**:

- **`feat/gristle-on-12` / `feat/gristle-on-13`** — the master switch was written on
  top of Sear, so its context lines said `SEAR` where a Sear-free base says `HEAT`.
  Reverted to the base's Heat naming in three files: the `menus.cpp` comment
  (`unlike Sear` → `unlike Heat`), the `automation_view.cpp` param-count comment
  (`LOCAL_SEAR` → `LOCAL_HEAT`), and the `param.cpp` designated initializer
  (`[UNPATCHED_SEAR_TONE]` kept as `[UNPATCHED_HEAT_TONE]`, with the new
  `[UNPATCHED_GRISTLE_ON]` line added after it).
- **`feat/eq-readout-13`** — dropped one `#include "dsp/gristle.hpp"` that came along
  as stack context. Nothing else in the commit references gristle; verified by grep.

### Verification

Re-stacking the split branches reproduces the originals **exactly**:

```
1.2.1:  base + gristle-on + sear + eq   ->  tree a54fbe45  ==  chopin 33c4e258
1.3:    sear-13 + eq-readout + gristle-on -> tree e203afbc  ==  chopin-to-13 5cf8edeb
```

Both are exact tree-hash matches. The split lost nothing and invented nothing.

Additionally, `feat/sear-12`, `feat/eq-readout-12` and `feat/ci-chopin-12` were
confirmed to carry the identical set of added/removed lines and the identical file
list as their originals.

---

## Two things the split does NOT fix

Carried over from the handoff, still true on these branches:

- **`UNPATCHED_GRISTLE_ON` has no MIDI Follow CC on 1.2.1.** 1.3 assigns it CC 89;
  on 1.2.1 that is Low Mid Freq, and no free grid position carries a CC. Menu and
  automation reach the switch; MIDI Follow does not. `feat/gristle-on-13` *does*
  touch `midi_follow.cpp` — the 1.3 branch has the CC, the 1.2.1 branch cannot.
- **No read-side XML alias for `heat` → `sear`.** Still relies on no song ever
  having been saved with the old tags.

---

## Builds

| Script | Branch | Line | Toolchain | Version string |
|---|---|---|---|---|
| `2-build-chopin.cmd` | `chopin` | 1.2.1 | v16 | `1.2.1-rudement` |
| `3-build-beta13.cmd` | `beta-1.3` | 1.3 | v22 | `1.3.0-rudement`, semver `-beta` |

`beta-1.3` is created by the import script pointing at `a99b641b` — that is
`chopin-to-13` plus the two arpeggiator commits, i.e. everything.

Both scripts run `dbt nuke` before building. That is not caution, it is required:
CMake caches the absolute compiler path, so crossing the v16/v22 boundary without
nuking silently compiles with the wrong toolchain. Each script also asserts that
`toolchain/REQUIRED_VERSION` reads what it expects after checkout, and stops if not.

`dbt build release -m` on chopin is likewise required, not optional — without `-m`,
`RELEASE_TYPE` defaults to `release`, which takes the `-c${PROJECT_VERSION}` filename
branch and reports `c1.2.0`, ignoring `DISPLAY_VERSION`.
