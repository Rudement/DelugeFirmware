## CI: build this fork's chopin branch on push

`pr-build.yml` only fired on pushes to `synthstrom-official` and PRs into `community` /
`release/**` — all upstream branch names inherited at fork time. Nothing matched `chopin`, so
pushing the branch built nothing, which is why 1.2.1 had only ever been built by hand.

Adding the branch to the existing push trigger is the whole change. `build.yml` references no
`pull_request` context and with its `branch` input null it checks out the triggering ref. It
pulls the toolchain container tagged from `toolchain/REQUIRED_VERSION`, which is v16 here.

Costs a Release and a Debug build per push, artifacts retained 5 days.

**Base:** `a2e333b9` (Chopin 8.8.26, 1.2.1 line). Independent — no other feature branch underneath it.

### Note

This does **not** enable `format-check.yml`, which only runs on PRs into `community`. Formatting
on this line is still ungated — run `dbt format` locally for tidiness, not because CI will
catch it.

**Do not send this upstream.** It references a branch name that exists only in this fork.
