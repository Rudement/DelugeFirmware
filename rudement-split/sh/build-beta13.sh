#!/usr/bin/env bash
# Build Beta 1.3 (1.3.0-rudement) from `beta-1.3`. Toolchain v22.
set -euo pipefail

# --- shared helpers -----------------------------------------------------------
# Resolve the repo root from this script's location, so it works from anywhere.
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

die() { printf '%s\n' "$*" >&2; exit 1; }

# Desktop differs by platform, and on Linux may be redirected by XDG.
desktop_dir() {
  case "$(uname -s)" in
    Darwin) printf '%s\n' "$HOME/Desktop" ;;
    Linux)
      if command -v xdg-user-dir >/dev/null 2>&1; then
        xdg-user-dir DESKTOP
      else
        printf '%s\n' "$HOME/Desktop"
      fi ;;
    *) printf '%s\n' "$HOME/Desktop" ;;
  esac
}

# g_menus.inc is generated and gets rewritten with different line endings by
# every build, blocking the next branch switch. Discard it ONLY when line
# endings are the sole difference; a genuine edit is left alone.
GENFILE="src/deluge/gui/menu_item/generate/g_menus.inc"
drop_generated_noise() {
  if git diff --quiet --ignore-cr-at-eol -- "$GENFILE" 2>/dev/null; then
    if ! git diff --quiet -- "$GENFILE" 2>/dev/null; then
      git checkout -- "$GENFILE"
      echo "Discarded line-ending-only change to g_menus.inc"
    fi
  fi
}

require_clean_tracked() {
  if [ -n "$(git status --porcelain -uno)" ]; then
    git status --short -uno
    die "Tracked files are modified. Commit or stash them first."
  fi
}

# toolchain/REQUIRED_VERSION is tracked per branch. CMake caches the absolute
# compiler path, so crossing the v16/v22 boundary without a nuke silently
# compiles with the wrong toolchain.
assert_toolchain() {
  local want="$1" got
  got="$(tr -d '[:space:]' < toolchain/REQUIRED_VERSION)"
  echo "Toolchain required by this branch: v$got"
  [ "$got" = "$want" ] || die "EXPECTED v$want - wrong branch or a bad checkout. Stopping."
}

drop_generated_noise
require_clean_tracked

git rev-parse --verify --quiet beta-1.3 >/dev/null \
  || die "Branch beta-1.3 does not exist. Import the branches first."

# HANDOFF.md is tracked from 5b24ee27 onward, which beta-1.3 descends from, but
# may be untracked on the branch you are standing on. Git refuses to check out
# over an untracked file it would overwrite.
if ! git ls-files --error-unmatch HANDOFF.md >/dev/null 2>&1; then
  if [ -f HANDOFF.md ]; then
    echo "Preserving untracked HANDOFF.md as rudement-split/HANDOFF.local.md"
    cp -f HANDOFF.md rudement-split/HANDOFF.local.md
    rm -f HANDOFF.md
  fi
fi

echo
echo "=== Checking out beta-1.3 ==="
git checkout beta-1.3
assert_toolchain 22

echo
echo "=== Nuking build dir (toolchain boundary) ==="
./dbt nuke

echo
echo "=== Building ==="
# RELEASE_TYPE already defaults to "dev" on the 1.3 line, so the filename picks
# up DISPLAY_VERSION either way. -t beta sets the semver prerelease id.
./dbt build release -m -t beta

echo
echo "=== Output ==="
ls -1 build/Release/*.bin

"$REPO/rudement-split/sh/collect-binaries.sh"
