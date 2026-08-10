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

- `deluge-v1_2_1-rudement+2026_08_09-82029181.bin` — 1.2.1 line, toolchain v16, **current**
- `deluge-v1_3_0-rudement+2026_08_08-a99b641b.bin` — 1.3 beta, toolchain v22, 502 objects

Older builds sit alongside them; those two are current. `...4d68a0b1.bin` was the 1.2.1
binary until 2026-08-09 and is superseded by `82029181` (Sear 2.0x).

**A filename does NOT identify its contents.** The hash in a build name is
`git rev-parse --short HEAD` (CMakeLists.txt:72), so a build made with uncommitted
changes carries the hash of a commit that does not contain them. `-dirty` appears in the
version string at build time but NOT in the filename. Two builds, one with an experiment
patch and one without, produce the same filename. `GIT_STATUS` is captured at
CMakeLists.txt:84 and then never used — wiring it into the suffix would fix this class of
bug permanently and is a few lines.

Also: `collect-binaries.cmd` copies **every** `.bin` in `build/Release`, so a stale binary
from an earlier build gets copied and labelled as though it were the new one. Clear
`build/Release/*.bin` before collecting, or check timestamps after.

Reference build kept out of tree: `HEAT-ORIGINAL-...a2e333b9.bin` in `build/Release` is the
pre-leveller Heat, built 2026-08-09 from `a2e333b9`. `dbt nuke` will delete it. Copy it
somewhere durable if the old sound is ever wanted again — it is the only easy way back.

### Branches

| Branch | Tip | Line | What |
|---|---|---|---|
| `chopin-rudement` | `82029181` | 1.2.1 | Release branch, all features integrated, plus docs. **Push here, not `chopin`.** Firmware content is `82029181` — no longer `4d68a0b1`. Until 2026-08-09 every commit past `chopin` was docs-only, so the two matched; the Sear 2.0x commit ended that. `2-build-chopin.cmd` still does `git checkout chopin` and would now build the OLD Sear. Fix it or build by hand. |
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

- **#1 Sear closed 2026-08-09: 2.0x accepted, committed as `82029181`.** Four hypotheses
  tested, three dead. See the Sear section below before reopening anything.

## Open

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

**Experiments** — throwaway builds, never commit the edits they make

```
experiments\sear-experiment.ps1 bypass     correction removed entirely (diagnostic)
experiments\sear-experiment.ps1 liftsteep  allowance reaches the 2.0x ceiling by mid-knob
experiments\sear-experiment.ps1 slowglide  kSearGainShift 7 -> 11 (2.9 ms -> 46 ms)
experiments\sear-experiment.ps1 noseed     drop the per-note seed (patches voice.cpp too)
experiments\sear-experiment.ps1 revert     git checkout both files - RUN THIS AFTER
```

Invoke as `powershell -ExecutionPolicy Bypass -File .\rudement-split\experiments\sear-experiment.ps1 <mode>`;
`.ps1` files are blocked by execution policy otherwise. Each mode restores both files before
patching, so modes cannot stack. Output goes to Rude Claude prefixed `EXPERIMENT-<mode>-`.

`slowglide` and `noseed` have both been run and both failed — see the Sear section. They are
kept for reference, not as open candidates.

**For a one-line change, editing the file directly and running `.\dbt.cmd build release -m` is
faster and avoids the execution-policy dance entirely.** Note PowerShell does not search the
current directory, so `dbt` alone fails — it must be `.\dbt.cmd`.

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

## Sear "too tame" — where the investigation stands

### Established, 2026-08-09

`kSearDriveBoost` (the +50% pre-gain, `def3db2f`) does **not** fix it. The numbers were already
confirmed — ceiling 406x, realised ratio 1.42–1.58x, octaves reaching 8 — so the boost applies
and it still sounds tame. More pre-gain is not the answer.

**Bypass test result: with the corrective gain forced to unity, the drive character is GOOD but
the level is too loud.** That is decisive:

- The **auto-leveller is the cause** of "tame". It divides out the loudness the drive produces,
  and loudness is a large part of how saturation is perceived.
- **`softClipCubic` is exonerated.** The curve was never the problem, so this is not a
  waveshaper question.
- Zero correction is too loud, full correction is too tame. **The answer is partial
  correction** — which is what `lift` already does, just not generously enough.

Note "too loud" here matches the pre-leveller report from 2026-08-08 ("super hot, I have to
turn the volume down past 15") almost exactly. Those are the two known failure modes.

### The mechanism

In `searBuffer`, as committed at `82029181`:

```cpp
const q31_t lift   = (level < (1 << 29)) ? (level << 2) : ONE_Q31;
const q31_t target = add_saturation(raw, 2 * multiply_32x32_rshift32(raw, lift));
```

`raw` is the pure ratio `envIn/envOut`, correcting loudness to 0.00 dB. `lift` adds an
allowance back, now reaching 2.0x (+6.0 dB) at full knob.

**2.0x IS A STRUCTURAL CEILING, NOT A TUNING CHOICE.** `target = raw * (1 + lift/2^31)` and
`lift` saturates at `ONE_Q31`, so the multiplier cannot exceed 2.0 no matter what is done to
`lift`. A larger shift only reaches the same ceiling at a lower knob position. Exceeding 2.0x
requires changing the `2 *` in the target line, and `4 *` overflows int32 as written — that is
real surgery with overflow handling, not a constant change. Do not spend another session
trying to "turn it up" without reading this paragraph.

`lift` also scales LINEARLY with `level`, so the bottom of the knob gets little allowance in
absolute terms — about +2.3 dB at knob 15 against +6.0 dB at full.

### Outcome, 2026-08-09 — RESOLVED, do not re-run these

Listened on hardware in this order. **2.0x won and is committed.**

| Tried | Result |
|---|---|
| `kSearDriveBoost` +50% pre-gain (`def3db2f`) | No help. More pre-gain is not the answer. |
| `bypass` — correction forced to unity | Good character, too loud. Localised the fault to the leveller and exonerated `softClipCubic`. |
| **`lift2x` — allowance 1.5x -> 2.0x** | **Accepted. Committed as `82029181`.** |
| `slowglide` — `kSearGainShift` 7 -> 11 (2.9 ms -> 46 ms) | No improvement. Predictable in hindsight: see below. |
| `noseed` — drop the per-note seed in `voice.cpp` | Worse. The seed's stated reason (`sear.hpp:190`) is sound. |
| Original Heat rebuilt from `a2e333b9` | Reference listen. Confirmed 2.0x is preferred. |

**Why the dynamics hypothesis failed, so nobody retries it.** The theory was that the leveller
flattens note attacks: `kSearGainShift = 7` moves the gain 1/128 per sample (2.9 ms at
44.1 kHz), fast enough to duck a transient. But `voice.cpp:153` seeds each new Voice from the
Sound's converged state, so the gain *starts* already ducked and has almost nothing to glide
to — which is why slowing the glide changed nothing audible. Removing the seed made every note
start uncorrected and sounded worse, not more aggressive.

Conclusion: what the pre-leveller Heat had was **level**, not attack behaviour. Loudness reads
as aggression. That is the thing 2.0x is capped on.

### If it is ever reopened

The only remaining lever is restructuring the target expression to allow more than 2.0x, with
overflow handling. Weigh that against the fact that the uncapped version (`bypass`) and the
static-makeup version (Heat) were both rejected as too loud. There is a real possibility that
no setting of this control satisfies both complaints and the honest answer is that 2.0x is the
compromise.

### Constraint that must not be broken

The leveller exists because **no constant works** — the required correction spans nearly 14 dB
across input levels at a single knob position, and both earlier fixed-makeup attempts failed in
opposite directions ("ducked instead of dirtying", then "super hot"). Any fix must stay a
**ratio** tracker and must not become a compressor; note dynamics have to keep passing through
untouched.

---

## MIDI FX — status

`feat/midi-fx-13` is **groundwork, not a feature**. Two commits collapse the arpeggiator note
dispatch into single seams (`dispatchArpNoteOffs` / `dispatchArpNoteOns` on both the
`NonAudioInstrument` and `Sound` sides). There is no menu entry, no param, no CC, and nothing
to access on the device.

The point is to create one place to stand between the arpeggiator and the output — the seam a
MIDI FX stage would plug into. **That stage has not been written.**

**Design written 2026-08-09: [`docs/MIDI-FX-DESIGN.md`](docs/MIDI-FX-DESIGN.md).** Every code
reference in it is verified against `feat/midi-fx-13`. Three findings worth knowing before
opening it:

- **The note-trigger path is already arp-independent.** `sendNote` runs every note through the
  arp unconditionally and the arp passes through when `mode == OFF`, so the existing seam fires
  either way. Note-mapping effects — transpose, velocity, chord — need no new plumbing.
- **Only the time-driven path is gated**, in three places. `handlePendingNotes` already runs
  before the `ArpMode::OFF` check in both `render` and `doTickForward`, so there is a working
  arp-independent tick path to copy, and `doTickForwardForArp`'s return value is an existing
  scheduler that needs no new infrastructure.
- **The hard part is note identity, not the clock.** `noteOnPostArp` takes a pointer to an
  `ArpNote` inside the arp's `notes` array and writes the MPE channel back into it — and
  `noteOff` deletes that element. Any generated note that outlives its parent (an echo) would
  dangle, and would be invisible to the MPE channel survey. That decides the architecture.

Staged so Stage 1 (decouple the gates, zero functional change) is separately upstreamable
alongside the seam commits. Open questions are in §10.
