#!/usr/bin/env bash
# Remove stale git lock files left behind by a sandbox session.
#
# A FUSE-mounted working copy can permit file creation but deny deletion, so git
# takes a lock it cannot release and every later git operation fails. On a normal
# macOS/Linux checkout this is rare, but the recovery is the same.
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

echo "=== Removing stale lock files ==="
for f in .git/index.lock .git/HEAD.lock; do
  [ -e "$f" ] && rm -f "$f" && echo "  removed $f"
done

echo
echo "=== Pruning worktrees ==="
git worktree prune
echo "  pruned"

echo
echo "=== Verifying git works ==="
git status --short || die "git is still failing. Look for other locks: find .git -name '*.lock'"

echo
echo "=== Branch state ==="
git rev-parse --abbrev-ref HEAD
for b in chopin chopin-to-13 midi-fx chopin-rudement beta-1.3; do
  if git rev-parse --verify --quiet "$b" >/dev/null; then
    printf '  %-16s %s\n' "$b" "$(git rev-parse --short "$b")"
  fi
done
