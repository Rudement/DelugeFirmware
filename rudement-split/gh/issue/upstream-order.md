Tracking issue for eventually submitting this work to `SynthstromAudible/DelugeFirmware`.

### The problem

The feature branches are independent **of each other**, but not of prior unlanded work. The 1.3
base `134d000f` is not upstream — it is this fork's own stack:

```
134d000f  EQ: four-band on 1.3            <- ours
5ea71d2a  mid-eq                           <- ours
a0d05ead  Merge branch 'upstream-main'
e05e72b2  Merge branch 'heat'
132683c5  EQ: add Mid band (CC 88 / CC 89) <- ours
0ae638cb  Heat: guard lshiftAndSaturate    <- ours
359866ac  Heat: anchor makeup gain on peak <- ours
0de58f76  Heat: move LOCAL_HEAT out        <- ours
0856ff90  Revert "wip"                     <- upstream starts here
```

So a PR to `community` from any feature branch today would either not apply, or would silently
drag in three unrelated features.

### Dependency order for submission

1. **Heat** (`0de58f76`, `359866ac`, `0ae638cb`) — nothing else lands without this; Sear is a
   rename of it and the Gristleizer needs its `softClipCubic`.
2. **EQ Mid band** (`132683c5`) → **four-band** (`134d000f`) — then `feat/eq-readout-13`.
3. **Gristleizer port** (`feat/gristle-13`) — then `feat/gristle-on-13`.
4. **Sear rename** (`feat/sear-13`) — after Heat and the Gristleizer port.

### Can go first, independent of all of the above

- **`feat/midi-fx-13`** — pure refactor, applies to the bare base, depends on none of the
  feature work. Best candidate for a first upstream PR.

### Do not submit

- `feat/ci-chopin-12` — references a branch name that exists only in this fork.
- `feat/version-label-13` — fork identity label.
- All four `-12` branches — 1.2.1 backports for this fork's release line, not upstream material.

### Before any upstream PR

- [ ] Rebase onto current `upstream/community` — these are months of drift behind.
- [ ] Run `dbt format`. `format-check.yml` only runs on PRs into `community`, so **none of this
      work has ever been format-checked.**
- [ ] The `pre-commit` config exists only on the 1.3 line; commits on chopin were made with
      `--no-verify` because the hook cannot run there.
- [ ] Hardware testing (the three linked issues) — do not submit untested DSP.
