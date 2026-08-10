#!/usr/bin/env bash
# Build Chopin (1.2.1-rudement) from the `chopin` branch. Toolchain v16.
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

echo
echo "=== Checking out chopin ==="
git checkout chopin
assert_toolchain 16

echo
echo "=== Nuking build dir (toolchain boundary) ==="
./dbt nuke

echo
echo "=== Building ==="
# -m is REQUIRED. Without it RELEASE_TYPE defaults to "release" on this branch,
# which takes the -c${PROJECT_VERSION} filename branch and reports c1.2.0,
# ignoring DISPLAY_VERSION. -m implies -t dev, which gives 1.2.1-rudement.
./dbt build release -m

echo
echo "=== Output ==="
ls -1 build/Release/*.bin
echo
echo 'A "-dirty" suffix is cosmetic: the build rewrites the generated g_menus.inc'
echo "with different line endings. Diff ignoring them is empty."

# Get it out before the next build nukes the directory.
"$REPO/rudement-split/sh/collect-binaries.sh"
