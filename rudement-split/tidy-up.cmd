@echo off
REM ============================================================================
REM  Clear the remaining loose ends.
REM
REM   1. Copy docs/DEVELOPMENT-NOTES.md + HANDOFF.md + rudement-split onto beta-1.3
REM   2. Drop the two junk files that rode along, add a .gitignore rule
REM   3. Back up then clear the GitHub Desktop stashes
REM   4. Delete chopin-backport and the two .stranded files
REM   5. Report on chopin / chopin-to-13 without touching them
REM
REM  Run from the repo root:  rudement-split\tidy-up.cmd
REM  CLOSE GITHUB DESKTOP FIRST.
REM
REM  Nothing here is destructive without a backup. Stashes are written to
REM  rudement-split\stash-backup\ as patch files before being cleared.
REM ============================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0.."
set "REPO=%CD%"

for /f %%i in ('git status --porcelain -uno') do (
  echo Tracked files are modified. Commit or stash them first.
  git status --short -uno
  exit /b 1
)

REM ---------------------------------------------------------------------------
REM  1. beta-1.3 gets the docs.
REM
REM  Done through a TEMPORARY WORKTREE, not a branch switch. rudement-split is
REM  tracked on chopin-rudement and not on beta-1.3, so checking out beta-1.3 in
REM  the main tree would delete this very script while it is still running -
REM  batch files are read line by line as they execute.
REM ---------------------------------------------------------------------------
echo.
echo === 1. Docs onto beta-1.3 ===

git rev-parse --verify --quiet beta-1.3 >nul
if errorlevel 1 ( echo   beta-1.3 not found, skipping. & goto :junk )

set "WT=%TEMP%\dfw-beta13-wt"
if exist "%WT%" rmdir /s /q "%WT%"

git worktree add "%WT%" beta-1.3
if errorlevel 1 ( echo   Could not create worktree, skipping. & goto :junk )

pushd "%WT%"
git checkout chopin-rudement -- docs/DEVELOPMENT-NOTES.md HANDOFF.md rudement-split
if errorlevel 1 (
  echo   Nothing to copy across.
) else (
  REM the junk files are dropped in step 2 on both branches; do not carry them here
  git rm -q --cached rudement-split/rudement-split.bundle 2>nul
  git rm -q --cached rudement-split/HANDOFF.local.md 2>nul
  del /f /q rudement-split\rudement-split.bundle 2>nul
  del /f /q rudement-split\HANDOFF.local.md 2>nul
  git commit --no-verify -q -m "docs: carry DEVELOPMENT-NOTES and tooling onto the 1.3 line" && echo   committed on beta-1.3
)
popd

git worktree remove "%WT%" --force
git worktree prune

:junk
REM ---------------------------------------------------------------------------
REM  2. The two files that should not have been committed.
REM     - rudement-split.bundle: 98 KB binary, redundant now the branches are on
REM       the remote. Kept on disk, just untracked.
REM     - HANDOFF.local.md: the pre-rewrite handoff, superseded.
REM ---------------------------------------------------------------------------
echo.
echo === 2. Untrack the junk ===

git rm -q --cached rudement-split/rudement-split.bundle 2>nul && echo   untracked rudement-split.bundle
git rm -q --cached rudement-split/HANDOFF.local.md 2>nul && echo   untracked HANDOFF.local.md
del /f /q rudement-split\HANDOFF.local.md 2>nul

findstr /c:"rudement-split/*.bundle" .gitignore >nul 2>&1
if errorlevel 1 (
  echo.>> .gitignore
  echo # split delivery artefacts - branches live on the remote now>> .gitignore
  echo rudement-split/*.bundle>> .gitignore
  echo rudement-split/HANDOFF.local.md>> .gitignore
  echo rudement-split/stash-backup/>> .gitignore
  git add .gitignore
  echo   added .gitignore rules
)

git diff --cached --quiet
if errorlevel 1 (
  git commit --no-verify -q -m "chore: untrack split delivery artefacts" && echo   committed
)

REM ---------------------------------------------------------------------------
REM  3. Stashes. BACKED UP AS PATCHES FIRST, then cleared.
REM ---------------------------------------------------------------------------
echo.
echo === 3. Stashes ===

set "SB=%REPO%\rudement-split\stash-backup"
set COUNT=0
for /f "delims=" %%s in ('git stash list 2^>nul') do set /a COUNT+=1

if "%COUNT%"=="0" (
  echo   No stashes.
) else (
  echo   %COUNT% stash^(es^) found. Backing up to rudement-split\stash-backup\
  if not exist "%SB%" mkdir "%SB%"
  set /a I=0
  :stashloop
  git stash list --format=%%gd >nul 2>&1
  for /l %%n in (0,1,50) do (
    git rev-parse --verify --quiet "stash@{%%n}" >nul 2>&1
    if not errorlevel 1 (
      git stash show -p "stash@{%%n}" > "%SB%\stash-%%n.patch" 2>nul
      echo     stash@{%%n} -^> stash-%%n.patch
    )
  )
  git stash clear
  echo   Cleared. Restore any of them with:  git apply rudement-split\stash-backup\stash-N.patch
)

REM ---------------------------------------------------------------------------
REM  4. chopin-backport and the stranded files.
REM ---------------------------------------------------------------------------
echo.
echo === 4. Leftovers ===

git rev-parse --verify --quiet chopin-backport >nul
if not errorlevel 1 (
  git merge-base --is-ancestor chopin-backport chopin-rudement 2>nul
  if not errorlevel 1 (
    git branch -D chopin-backport >nul && echo   deleted chopin-backport ^(fully contained in chopin-rudement^)
  ) else (
    echo   KEEPING chopin-backport - it is NOT contained in chopin-rudement. Check it.
  )
) else (
  echo   chopin-backport already gone.
)

if exist "PIPELINE-TEST.md.stranded" ( del /f /q "PIPELINE-TEST.md.stranded" && echo   deleted PIPELINE-TEST.md.stranded )
if exist "src\deluge\dsp\heat.hpp.stranded" ( del /f /q "src\deluge\dsp\heat.hpp.stranded" && echo   deleted heat.hpp.stranded )

REM ---------------------------------------------------------------------------
REM  5. Report only. chopin and chopin-to-13 are NOT deleted here - that is a
REM     judgement call, and feat/ci-chopin-12's workflow references the name.
REM ---------------------------------------------------------------------------
echo.
echo === 5. Divergent branches (report only, nothing changed) ===
for %%b in (chopin chopin-to-13) do (
  git rev-parse --verify --quiet %%b >nul
  if not errorlevel 1 (
    for /f %%s in ('git rev-parse --short %%b') do echo   %%b = %%s  ^(local^)
  )
)
echo.
echo   chopin        is preserved as chopin-rudement ^(identical commit^)
echo   chopin-to-13  is contained in beta-1.3
echo   Both remotes sit on upstream commits. Delete the local ones only if you
echo   are sure - nothing depends on them except the CI workflow's branch name.

echo.
echo === Push ===
git push origin chopin-rudement beta-1.3

echo.
echo Done.
exit /b 0
