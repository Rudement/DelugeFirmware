#!/usr/bin/env bash
# Copy every .bin in build/Release to the "Rude Claude" folder on the Desktop.
#
# WHY: dbt nuke deletes the whole build directory, and both build scripts must
# nuke because they cross the toolchain boundary. Building one line then the
# other destroys the first binary before anything copies it out.
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

DEST="$(desktop_dir)/Rude Claude"
echo "Destination: $DEST"
[ -d "$DEST" ] || { mkdir -p "$DEST"; echo "Created it."; }

shopt -s nullglob
bins=(build/Release/*.bin)
[ ${#bins[@]} -gt 0 ] || die "Nothing in build/Release to copy. No build has run, or dbt nuke cleared it."

echo
echo "=== Copying ==="
for f in "${bins[@]}"; do
  cp -f "$f" "$DEST/"
  echo "  $(basename "$f")  ($(wc -c < "$f" | tr -d ' ') bytes)"
done

echo
echo "=== Now in $DEST ==="
ls -1 "$DEST"/*.bin
