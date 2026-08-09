# HANDOFF

Thin index. Last updated 2026-08-08.

This file used to carry everything. Most of it now lives somewhere it can be found:

| Was in here | Now |
|---|---|
| Environment hazards, build/toolchain traps | [`docs/DEVELOPMENT-NOTES.md`](docs/DEVELOPMENT-NOTES.md) — committed, versioned |
| Branch map, "what landed and why" | The ten draft PRs — rationale sits with the diff |
| Open questions needing hardware | GitHub Issues, label `needs-hardware-test` |
| Upstream submission plan | GitHub Issue, label `upstream` |

---

## State as of 2026-08-08

**Everything is pushed.** Nothing is single-copy on this machine any more.

Both binaries built and copied to `Desktop\Rude Claude`:

- `deluge-v1_2_1-rudement+2026_08_08-4d68a0b1.bin` — 1.2.1 line, toolchain v16, 468 objects
- `deluge-v1_3_0-rudement+2026_08_08-a99b641b.bin` — 1.3 beta, toolchain v22, 502 objects

### Branches

| Branch | Tip | Line | What |
|---|---|---|---|
| `chopin-rudement` | `4d68a0b1` | 1.2.1 | Release branch, all features integrated. **Push here, not `chopin`.** |
| `beta-1.3` | `a99b641b` | 1.3 | Everything including the arp work. Built as the 1.3 beta. |
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

The split was verified lossless: re-stacking reproduces tree `a54fbe45` (= `chopin` `33c4e258`)
and tree `e203afbc` (= `chopin-to-13` `5cf8edeb`) exactly.

### Do not trust these branches

`chopin` and `chopin-to-13` on the **remote** have both been force-synced to upstream commits at
least once, taking the work with them. See `docs/DEVELOPMENT-NOTES.md`. Work under names
upstream does not use has survived; keep using those.

---

## Open

- [ ] Run `rudement-split\gh\create-trail.cmd` — creates the 4 issues and 10 draft PRs.
- [ ] Commit `docs/DEVELOPMENT-NOTES.md` (currently untracked).
- [ ] Flash both binaries and work through the three `needs-hardware-test` issues.
- [ ] Consider `dbt configure -DENABLE_SYSEX_LOAD=YES` then flash once via SD, so later builds
      can go over USB with `dbt loadfw release` instead of shuttling the card.
- [ ] Upstream: see the `upstream` issue. Nothing has been format-checked; `feat/midi-fx-13` is
      the only branch with no prerequisites.

## Tooling in `rudement-split/`

```
0-cleanup-locks.cmd     unjam git after a sandbox session
1-import-branches.cmd   import the ten branches from the bundle (already run)
2-build-chopin.cmd      1.2.1-rudement, v16
3-build-beta13.cmd      1.3.0-rudement beta, v22
collect-binaries.cmd    copy build/Release/*.bin to Desktop\Rude Claude
gh\create-trail.cmd     create the GitHub issues and draft PRs
```
