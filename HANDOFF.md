# HANDOFF

Thin index. Last updated 2026-08-09.

This file used to carry everything. Most of it now lives somewhere it can be found:

| Was in here | Now |
|---|---|
| Environment hazards, build/toolchain traps | [`docs/DEVELOPMENT-NOTES.md`](docs/DEVELOPMENT-NOTES.md) |
| Branch map, "what landed and why" | The ten draft PRs — rationale sits with the diff |
| Open questions needing hardware | Issues #1–#3, label `needs-hardware-test` |
| Upstream submission plan | Issue #4, label `upstream` |

---

## State

**Everything is pushed.** Nothing is single-copy on this machine.

Both binaries in `Desktop\Rude Claude`:

- `deluge-v1_2_1-rudement+2026_08_08-4d68a0b1.bin` — 1.2.1 line, toolchain v16, 468 objects
- `deluge-v1_3_0-rudement+2026_08_08-a99b641b.bin` — 1.3 beta, toolchain v22, 502 objects

Older builds sit alongside them; those two are current.

### Branches

| Branch | Tip | Line | What |
|---|---|---|---|
| `chopin-rudement` | `70294678` | 1.2.1 | Release branch, all features integrated, plus docs. **Push here, not `chopin`.** Firmware content is `4d68a0b1`. |
| `beta-1.3` | `991939d1` | 1.3 | Everything including the arp work, plus docs. Firmware content is `a99b641b`. |
| `base-12` | `a2e333b9` | 1.2.1 | Split point. PR target only — no work on it. |
| `base-13` | `134d000f` | 1.3 | Split point. PR target only — no work on it. |
| `midi-fx` | `a99b641b` | 1.3 | Same commit as `beta-1.3`. The name that survived the force-syncs. |
| `feat/sear-12` | `df7cb689` | 1.2.1 | ┐ |
| `feat/eq-readout-12` | `7befe053` | 1.2.1 | │ all four independent, |
| `feat/gristle-on-12` | `7f6c3896` | 1.2.1 | │ rooted at `a2e333b9` |
| `feat/ci-chopin-12` | `18c7dedb` | 1.2.1 | ┘ |
| `feat/gristle-13` | `6c02c5d6` | 1.3 | **Shared prerequisite** — creates `gristle.hpp` |
| `feat/sear-13` | `a0da0808` | 1.3 | on `feat/gristle-13` |
| `feat/gristle-on-13` | `75ab6c82` | 1.3 | on `feat/gristle-13` |
| `feat/eq-readout-13` | `5b23e396` | 1.3 | independent, rooted at `134d000f` |
| `feat/midi-fx-13` | `e3402639` | 1.3 | independent — most upstream-ready of the set |
| `feat/version-label-13` | `5d4d0d22` | 1.3 | independent |

Split verified lossless: re-stacking reproduces tree `a54fbe45` (= `chopin` `33c4e258`) and
tree `e203afbc` (= `chopin-to-13` `5cf8edeb`) exactly.

### GitHub

Issues #1–#4, PRs #5–#14 on `Rudement/DelugeFirmware`.

| # | What |
|---|---|
| #1 | Sear: does `kSearDriveBoost` actually fix "too tame"? |
| #2 | Gristleizer: verify bypass behaviour (LFO timeline lock) |
| #3 | EQ: verify Hz/dB readout, Treble especially |
| #4 | Upstream submission: dependency order and prerequisites |
| #5–#8 | `feat/sear-12`, `feat/eq-readout-12`, `feat/gristle-on-12`, `feat/version-label-13` |
| #9–#11 | `feat/eq-readout-13`, `feat/gristle-on-13`, `feat/midi-fx-13` |
| #12–#14 | `feat/gristle-13`, `feat/sear-13`, `feat/ci-chopin-12` |

### Do not trust these branches

`chopin` and `chopin-to-13` on the **remote** have both been force-synced to upstream commits
at least once, taking the work with them. Local copies still exist and diverge from their
remotes. Work under names upstream does not use has survived; keep using those.

---

## Done

- Ten feature branches split from the integrated lines, verified lossless, pushed.
- Both binaries built and collected.
- Four issues and ten draft PRs created.
- `docs/DEVELOPMENT-NOTES.md` committed on **both** `chopin-rudement` and `beta-1.3`.
- Cleanup: `rudement-split.bundle` and `HANDOFF.local.md` untracked, `.gitignore` rules added,
  `chopin-backport` deleted after verifying it was contained in `chopin-rudement`, `.stranded`
  files removed.
- The ten GitHub Desktop stashes are **cleared but recoverable** — each was written to
  `rudement-split\stash-backup\stash-N.patch` first. Restore with
  `git apply rudement-split\stash-backup\stash-N.patch`. Delete that folder once you are
  confident none of it is wanted; it is gitignored.

- All ten PRs retargeted at `base-12` / `base-13`, so each shows exactly its own feature diff
  against the bare split point and nothing else.

- Hardware listening 2026-08-09. **#2 Gristle bypass passes. #3 EQ readout passes.**
  Both closed.

## Open

- [ ] **#1 Sear is still too tame.** `kSearDriveBoost` did not fix it. Before touching the
      constant, run the discriminating test: force the corrective gain to unity and listen.
      The auto-leveller divides out every bit of loudness the extra drive produces, by design,
      so level-matched saturation reading as "tame" is the expected outcome of it working
      correctly — and more pre-gain will not change that. Full reasoning in the issue.
- [ ] Upstream: see issue #4. Nothing has ever been format-checked — run `dbt format` first.
      `feat/midi-fx-13` is the only branch with no prerequisites; lead with it.
- [ ] Decide whether to delete local `chopin` / `chopin-to-13`. Both are preserved elsewhere,
      but `feat/ci-chopin-12`'s workflow keys off the name `chopin`.
- [ ] Optional: `dbt configure -DENABLE_SYSEX_LOAD=YES` then flash once via SD, so later builds
      can go over USB with `dbt loadfw release` instead of shuttling the card.

### The upstream finding, in one paragraph

The 1.3 base `134d000f` is **not upstream** — it is this fork's own stack. Underneath it sit
the four-band EQ, the Mid band EQ, and three Heat commits, none of which Synthstrom has seen.
So `feat/sear-13` needs Heat (Sear is a rename of it), `feat/eq-readout-13` needs both EQ
commits, and `feat/gristle-on-13` needs the Gristleizer port which needs Heat's
`softClipCubic`. Order is forced. Full detail in issue #4.

---

## Tooling in `rudement-split/`

**Windows**

```
0-cleanup-locks.cmd       unjam git after a sandbox session          [run]
1-import-branches.cmd     import the ten branches from the bundle    [run]
2-build-chopin.cmd        1.2.1-rudement, v16                        [run]
3-build-beta13.cmd        1.3.0-rudement beta, v22                   [run]
collect-binaries.cmd      copy build/Release/*.bin to Rude Claude    [run]
tidy-up.cmd               loose-end cleanup                          [run]
gh\create-trail.cmd       create the issues and draft PRs            [run]
gh\fix-remaining-prs.cmd  base-12 / base-13 and three retries        [run]
gh\retarget-prs.cmd       point every PR at its line's base          [run]
```

**macOS / Linux** — same behaviour, platform detected at runtime

```
sh/cleanup-locks.sh
sh/build-chopin.sh
sh/build-beta13.sh
sh/collect-binaries.sh
```

The shell versions resolve the Desktop per platform (`xdg-user-dir` on Linux, `~/Desktop` on
macOS) and call `./dbt` rather than `dbt.cmd`. `dbt` itself is already cross-platform and
downloads the toolchain matching the host. If they arrive without the executable bit:
`chmod +x rudement-split/sh/*.sh`

No shell equivalent of `1-import-branches` or the `gh` scripts — those were one-shot and have
already done their job.

### gh setup gotchas, if you start on a fresh machine

- **Two remotes.** `origin` is the fork, `upstream` is SynthstromAudible. `gh` refuses to
  guess. Run `gh repo set-default Rudement/DelugeFirmware` — pointing it at `upstream` would
  file issues and PRs on Synthstrom's repository.
- **Issues are disabled on forks by default.** `gh repo edit --enable-issues`.
- **PATH is stale** in any shell open when `gh` was installed, and Windows Terminal hands new
  tabs the environment it started with. Restart the terminal or refresh `$env:Path`.

---

## MIDI FX — status

`feat/midi-fx-13` is **groundwork, not a feature**. Two commits collapse the arpeggiator note
dispatch into single seams (`dispatchArpNoteOffs` / `dispatchArpNoteOns` on both the
`NonAudioInstrument` and `Sound` sides). There is no menu entry, no param, no CC, and nothing
to access on the device.

The point is to create one place to stand between the arpeggiator and the output — the seam a
MIDI FX stage would plug into. **That stage has not been written.**
