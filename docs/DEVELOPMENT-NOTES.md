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

**A branch switch over the mount leaves the other branch's files behind.** Deleting is how git
removes a file that exists only on the branch you are leaving, so when deletion is denied the
file simply stays -- untracked, invisible to a plain `git status`, and *globbed into the build
by CMake*. On 2026-09-02 that put `cv_audio_stream.cpp` from `feat/aux-sends-13-gated` into a
build of `integration/beta-13-2026-09-01`, which then failed on a `RuntimeFeatureSettingType`
the branch does not have -- a compile error that reads like a code bug and is nothing of the
kind. The same switch had also left the working tree's *tracked* files stale while `git status`
called them clean.

**Two Cowork tasks cannot share this working tree.** This is the same refusal-to-delete, one
step worse. A second task pointed at this folder switches branches; git moves `HEAD` and writes
the index, the mount refuses the deletes, and the working tree stays on the *other* task's
branch. You end up with a repo whose `HEAD` and files disagree by an entire branch, and neither
task can see it -- both just see a working tree that has apparently been modified wholesale.

On 2026-09-03 a task working on WiFi firmware update created and checked out `feat/selfflash-13`
(at `base-13`, no commits) while this one was mid-build on `feat/aux-sends-13-gated`. What it
looked like from inside the build script: a checkout failing with `Your local changes to the
following files would be overwritten by checkout` listing two hundred files, and a `git status`
showing every file that differs between the two branches as modified, plus every file unique to
the other branch as untracked. It reads like a catastrophic accidental edit. It is not one.

Before throwing anything away, hash the files against the branch you thought you were on:

```
for f in $(git diff --name-only); do
  [ "$(git hash-object "$f")" = "$(git rev-parse <branch>:"$f")" ] || echo "REALLY MODIFIED: $f"
done
```

Silence means the tree is merely stale and `git reset --hard <branch>` loses nothing. All 202
files were silent that day.

Recover **from Windows, not from the sandbox**: 448 files over the mount takes minutes, and a
checkout killed part-way through leaves a half-updated index -- staged deletions of files that
are still sitting there. `git checkout -f <branch>` followed by `git reset --hard` fixes it.
Untracked files survive all of this, so uncommitted work in a file git does not track is never
at risk; on the day, the other task's `docs/dev/wifi-firmware-update.md` came through untouched.

Avoid it by running one task against this folder at a time. If two lines of work genuinely need
to run at once, the second one gets its own clone.

After any branch switch made from a sandbox, before building:

```
git status --porcelain --untracked-files=all -- src
```

Anything with `??` is a stray. Confirm it is recoverable (`git hash-object <path>` equals
`git rev-parse <branch>:<path>`) and delete it. `run-build-beta13-sync.cmd` does this check and
refuses to build if it finds one.

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

---

## Reading the fault display

When the firmware takes a CPU exception it does not print a code. It paints the pad grid.
All of this is `printPointers()` in `src/OSLikeStuff/fault_handler/fault_handler.c`, and it
is readable off a photo of the front panel.

**Bit order.** One byte per column, **bottom pad is the MSB**, top pad the LSB. Pads leave
in column pairs (`PIC::setColourForTwoColumns`, `idx = x >> 1`, x counted from the left);
within a pair the left column's eight pads are sent before the right column's.

**Layout.** Each pointer occupies four columns -- two column pairs -- filled left to right
as bits 31-24, 23-16, 15-8, 7-0. Groups are laid down left to right in the order the
handler finds them:

| Colour | Pointer |
|---|---|
| magenta `255,0,255` | USR-mode LR. Never appears: the abort vector passes 0 for it. |
| blue `0,0,255` | SYS-mode LR -- where it died. |
| green `0,255,0`, cyan `0,255,255`, alternating | up to four return addresses found by scanning the stack for values inside the code range |

Column pairs with nothing to show are cleared to black.

**The sidebar says which firmware.** The two sidebar columns carry the first four hex
digits of `kCommitShort`: mute/launch column is the first byte, audition column the second.
Its colour is the fault class -- **red** means it came through `handle_cpu_fault`, a real
data/prefetch abort or undefined instruction; **yellow** means `FREEZE_WITH_ERROR`, i.e.
one of the `E###` codes. Check this first. It tells you whether the unit is running the
binary you think it is.

**A pointer group always starts `0x20`.** `isCodePointer()` tests against
`program_code_start` / `program_code_end` from the linker script -- 0x2005f640 and
0x201a9220 for the 1.3 line as built on 2026-09-02, so the first column of any group reads
`0x20` and the second is between `0x05` and `0x1a`. If the first column of a group is dark,
that pair's bytes did not survive the trip to the PIC and the top half of that address is
gone; the PIC resyncs on the next `SET_COLOUR_FOR_TWO_COLUMNS` byte, so later pairs -- and
the commit ID, which is sent last -- are still good.

**Address to symbol.** `build/Release/deluge.nmdump` is decimal address, size, type, name:

```python
syms = sorted((int(p[0]), int(p[1]), p[3].rstrip())
              for p in (l.split(None, 3) for l in open('deluge.nmdump', errors='ignore'))
              if len(p) == 4 and p[2].lower() in 'tw' and p[0].isdigit())
def lookup(a):
    return next((s for s in syms if s[0] <= a < s[0] + max(s[1], 1)), None)
```

Subtract 8 from the LR for a data abort, 4 for a prefetch abort, before reading it as the
faulting instruction. For finding the function it rarely matters.

**Expect one pointer from a hard fault, and read nothing into the missing ones.** The abort
vector runs in abort mode, where `SP` is banked, so `handle_cpu_fault` is handed `SP_abt` and
not the program stack. `isStackPointer()` rejects it, the stack scan finds no return addresses,
and only the SYS-mode LR is ever drawn. A `FREEZE_WITH_ERROR` freeze, which runs on the normal
stack, is the case that can show more.

**Keep the `.nmdump`.** `build\` is wiped between lines and the build scripts copy out only the
`.bin`, so a build's symbol table dies with the next wipe and its fault addresses become
unreadable. It is 800 KB. Copy it out beside any binary worth flashing; `symbols/` in the repo
root is where they go.

**Worked example, 2026-09-02.** Sidebar red, `b3 7e` -- a hard fault in commit `b37e6db3`,
the 1.3.0-beta sync built in a Linux container with Ubuntu's gcc 13.2 rather than the
vendor v22 toolchain. One blue group, in the OSC1/OSC2 columns, low half `0x7358`; the
SAMPL1/SAMPL2 pair to its left was dark, so the top half was lost per the note above. Of
the 21 addresses `0x20xx7358` inside the code range, three sit on the startup path:
`__static_initialization_and_destruction_0 +10052`, `deluge_main +1560` and
`LoadSongUI::performLoad +1284`. The same tree rebuilt under the vendor v22 toolchain boots
clean, as do the stock 1.3.0 beta of the same upstream commit and the 1.2.1 build -- so the
merge was never at fault and the container toolchain was.
**Container builds are for checking that a port compiles. Do not flash them.**

---

## Flashing: what the card must not contain

The bootloader scans the root of the card for `*.BIN` and does not filter out directories.
`$RECYCLE.BIN` is a directory whose name ends in `.BIN`, and a directory entry reports a size
of zero, so the bootloader matches it, decides that is the firmware, and stops with

```
FIRMWARE FILE FOUND IS TOO SMALL TO POSSIBLY CONTAIN VALID FIRMWARE.
```

The real firmware file sitting next to it is never looked at.

Windows creates `$RECYCLE.BIN` on a removable drive the first time you delete something there
through Explorer. Which means clearing the old build off the card to leave "only one .bin" is
itself what creates the thing that stops the new one loading. Anything else tiny whose name
ends in `.bin` does the same -- a Mac `._something.bin` resource fork is the other one this
card has produced.

**Delete on the card with `del` or Shift+Delete, never a plain Explorer delete.** Before
flashing (the card is usually D:, but check):

```
dir /a D:\*.bin
rd /s /q "D:\$RECYCLE.BIN"
```

`/a` matters -- `$RECYCLE.BIN` is hidden, so a plain `dir` does not show it, and the card looks
perfectly clean while refusing every flash.

Confirmed 2026-09-02: exactly one `.bin` at the root, correct size, and the unit still refused
it. Removing the folder was the entire fix and the same file then flashed first try. Every
flash that had worked earlier that day predated the folder's creation timestamp by minutes.
