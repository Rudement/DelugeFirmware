# Development notes — Rudement fork

Environment and build behaviour that has cost real time. None of this is about the code.

---

## Build new work on 1.3 first

**Standing rule: every new feature starts on the 1.3 line.** Branch `feat/<name>-13` off
`base-13`, get it right there, and backport to 1.2.1 only afterwards — as a deliberate second
step, if it is wanted at all.

1.3 is where this fork is going. 1.2.1 is a maintenance line that will eventually stop mattering,
and it is the *older* codebase, so a feature written against it is written against an API that has
already moved on.

Direction matters more than it looks. Porting 1.2.1 → 1.3 means replaying a diff whose context
lines no longer exist:

| Written on 1.2.1 | 1.3 wants |
|---|---|
| `Sound* sound` → `sound->sources[s]` | `Sound& sound` → `sound.sources[s]` |
| `soundEditor.currentSourceIndex` | the menu item's own `source_id_` / `sourceId_` |
| `NULL`, C casts | `nullptr`, `static_cast` |
| constructors zeroing members | default member initialisers |

None of that is hard, but every one of them is a silent merge that compiles wrong or does not
compile at all, and git will happily auto-merge a hunk into the wrong dialect. Going 1.3 → 1.2.1
is the easy direction: you are removing modern constructs, not guessing at them.

Plaits was done the wrong way round — 1.2.1 first, then forward-ported — and the port needed
seven conflict resolutions across `voice.cpp`, `osc/type.h`, `menus.cpp`, `english.json`,
`voice_unison_part_source.{h,cpp}`, `audio_recorder.h` and `retrigger_phase.h`. Every one of
them was 1.3 having modernised code the 1.2.1 patch still assumed. Do not repeat it.

`base-12` and `base-13` are PR targets only — no work on them. See `HANDOFF.md` for the branch map.

## A Linux container can build this, with two patches

Useful for CI, or for verifying a port when the Windows toolchain is not to hand. Ubuntu's
`gcc-arm-none-eabi` (13.2) configures and links the whole firmware:

```
apt-get install -y gcc-arm-none-eabi
ln -s /usr toolchain/v22/linux-x86_64/arm-none-eabi-gcc
DELUGE_FW_ROOT=$PWD DBT_TOOLCHAIN_PATH=$PWD \
  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=scripts/cmake/CMakeToolchainDeluge.cmake
cmake --build build --target deluge
```

Both env vars are required — `CMakeToolchainDeluge.cmake` re-reads `toolchain/REQUIRED_VERSION`
inside CMake's try-compile scratch dir, where `CMAKE_SOURCE_DIR` is not the repo.

Two spots need a **build-only, never-committed** patch, and both are the compiler version rather
than the code:

| File | gcc 13.2 | Vendor v22 |
|---|---|---|
| `src/memmove.c` | no `vld1q_u8_x2` / `_x4` | present |
| `util/container/enum_to_string_map.hpp` | rejects a function parameter in `static_assert` | accepts |

Shim the four intrinsics from `vld1q_u8`/`vst1q_u8`, and comment out the one `static_assert`.
Neither belongs in a commit. If a container build ever fails anywhere *else*, that is real.

## Toolchains differ by line

| Line | Branches | `toolchain/REQUIRED_VERSION` |
|---|---|---|
| 1.2.1 | `chopin`, `chopin-rudement`, `feat/*-12` | **v16** |
| 1.3 | `chopin-to-13`, `beta-1.3`, `feat/*-13` | **v22** |

`REQUIRED_VERSION` is tracked per branch, so checking out sets it. But **CMake caches the
absolute compiler path in `build/CMakeCache.txt`**, so a plain rebuild after crossing that
boundary silently compiles with whichever toolchain configured the cache first.

**Always `dbt nuke` when switching between the two lines.** The build scripts in
`rudement-split/` do this and assert `REQUIRED_VERSION` after checkout.

## `dbt nuke` destroys previous binaries

`dbt nuke` deletes the entire `build/` directory. Because both lines require a nuke to switch,
building Chopin and then Beta 1.3 **destroys the Chopin binary** before anything copies it out.

Get binaries out of `build/Release` before starting the next build.
`rudement-split/collect-binaries.cmd` does this and is called automatically by both build
scripts.

## Version strings: two traps

**`RELEASE_TYPE` decides the label.** On the 1.2.1 line the default is `release`, which takes
the `-c${PROJECT_VERSION}` filename branch and reports `c1.2.0`, ignoring `DISPLAY_VERSION`
entirely. Build with `dbt build release -m` to get `1.2.1-rudement`. (`-m` implies `-t dev`.)
On the 1.3 line `RELEASE_TYPE` already defaults to `dev`, so the filename is right either way;
`-t beta` is passed only to set the semver prerelease id.

**`-dirty` is cosmetic.** The build rewrites the generated `g_menus.inc` with CRLF while git
stores LF. `git diff --ignore-cr-at-eol` on that file is empty.

**`PROJECT_VERSION` stays pinned on purpose.** It feeds `BUILD_VERSION_STRING_SHORT`, which
`storage_manager.cpp` writes into every saved song as `firmwareVersion`. Changing it would make
songs saved here claim a version no official firmware ever reported. `DISPLAY_VERSION` affects
only the filename and the on-device readout, so songs stay byte-identical to stock.

## `g_menus.inc` blocks branch switches

It is generated, and every build rewrites it with CRLF against git's LF. It then shows as a
tracked modification and git refuses to switch branches over it.

Safe to discard **when line endings are the only difference**:

```
git diff --quiet --ignore-cr-at-eol -- src/deluge/gui/menu_item/generate/g_menus.inc
git checkout -- src/deluge/gui/menu_item/generate/g_menus.inc
```

Both build scripts do exactly this check and only discard when the diff is genuinely empty.

## CI coverage is thinner than it looks

`pr-build.yml` fires on pushes to specific branch names inherited from upstream at fork time.
`format-check.yml` **only runs on PRs into `community`** — so nothing on these branches has ever
been format-checked. Run `dbt format` locally before submitting anything upstream.

The `pre-commit` hook reads `.pre-commit-config.yaml`, which exists only on the 1.3 line. On the
1.2.1 line the hook cannot run at all; commit with `--no-verify` there.

---

## Cowork / sandbox mounts deny file deletion

When this folder is mounted into a Cowork sandbox it is exposed over FUSE, which **permits file
creation but denies deletion**:

```
$ touch .git/probe   # ok
$ rm .git/probe      # rm: cannot remove: Operation not permitted
```

Git takes a lock file, cannot release it, and every later git operation in the repo fails with
a confusing error until the lock is deleted **from Windows**, where deletion works normally.

**This is the real cause of "stale `.git/index.lock`", not GitHub Desktop.** Do git work from
Windows. From a sandbox, clone to sandbox-local storage first and deliver results as a bundle.

Recovery:

```
del /f /q .git\index.lock
rmdir /s /q .git\worktrees\<name>
git worktree prune
```

## GitHub Desktop moves HEAD

With it open, it has switched branches mid-operation and run a `git reset` that moved the
`chopin` pointer onto `chopin-to-13`'s tip. Cancelling its branch-switch dialog did not stop it.
It also shows stale state for minutes, at one point offering to commit 1,579 files to the wrong
branch.

**Close it while doing git work.** It is not needed for credentials — the command line
authenticates fine on its own, and `gh` handles PRs and issues, neither of which GitHub Desktop
can do.

## Interrupted checkouts leave a hybrid tree

Twice the working tree held one branch's files while HEAD pointed at another, producing ~1,500
phantom "changes". Tells: both `heat.hpp` and `sear.hpp` present, or `*.stranded` files (git's
marker for a file it could not delete).

Verify with `git diff --stat <branch> -- .` before discarding anything. If it comes back empty
or mode-only, the tree is just that branch and nothing original is at risk.

## Large git operations time out

`reset --hard` across 3,235 files exceeds some tool timeouts. When the working tree already
holds the right content, `git reset --mixed <sha>` moves the ref and index without touching
files and completes in seconds.

## Remote branches have been force-synced to upstream

`origin/chopin` and `origin/chopin-to-13` were both found sitting on `2991d5d8`, an upstream
community commit — two different branches on the same upstream commit is not drift. Local
tracking refs still read the old values and their reflogs still said `update by push`, so
**neither is trustworthy; only `git ls-remote` is.**

Work pushed under names upstream does not use (`midi-fx`, `chopin-rudement`, `feat/*`,
`beta-1.3`) survived. Prefer those names.
