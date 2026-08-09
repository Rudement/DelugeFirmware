@echo off
REM ============================================================================
REM  Build Beta 1.3 (1.3.0-rudement) from the `beta-1.3` branch
REM  (= chopin-to-13 + the two arpeggiator fan-out commits).
REM  Toolchain: v22.
REM
REM  Run from the repo root:  rudement-split\3-build-beta13.cmd
REM  CLOSE GITHUB DESKTOP FIRST.
REM ============================================================================

setlocal
cd /d "%~dp0.."

REM ---------------------------------------------------------------------------
REM  g_menus.inc is GENERATED and gets rewritten with CRLF by every build while
REM  git stores LF, so it blocks the next branch switch. Discard it, but ONLY when
REM  line endings are the sole difference - a genuine edit is left alone and the
REM  guard below still catches it. See 2-build-chopin.cmd for the longer note.
REM ---------------------------------------------------------------------------
set "GENFILE=src/deluge/gui/menu_item/generate/g_menus.inc"
git diff --quiet --ignore-cr-at-eol -- "%GENFILE%" 2>nul
if not errorlevel 1 (
  git checkout -- "%GENFILE%" 2>nul && echo Discarded line-ending-only change to g_menus.inc
)

REM -uno = ignore untracked files (rudement-split\). Untracked files survive
REM branch switches and are not a reason to refuse.
for /f %%i in ('git status --porcelain -uno') do (
  echo Tracked files are modified. Commit or stash them first.
  git status --short -uno
  exit /b 1
)

git rev-parse --verify beta-1.3 >nul 2>&1
if errorlevel 1 (
  echo Branch beta-1.3 does not exist. Run 1-import-branches.cmd first.
  exit /b 1
)

REM ---------------------------------------------------------------------------
REM  HANDOFF.md trap: it is UNTRACKED on the branch you are probably standing on,
REM  but TRACKED from commit 5b24ee27 onward, which beta-1.3 descends from. Git
REM  refuses to check out over an untracked file it would have to overwrite:
REM    "untracked working tree file 'HANDOFF.md' would be overwritten"
REM  Stash our copy out of the way first; the branch's own version then lands
REM  normally and ours is preserved beside this script.
REM ---------------------------------------------------------------------------
git ls-files --error-unmatch HANDOFF.md >nul 2>&1
if errorlevel 1 (
  if exist "HANDOFF.md" (
    echo Preserving your untracked HANDOFF.md as rudement-split\HANDOFF.local.md
    copy /y "HANDOFF.md" "%~dp0HANDOFF.local.md" >nul
    del /f /q "HANDOFF.md"
  )
)

echo.
echo === Checking out beta-1.3 ===
git checkout beta-1.3
if errorlevel 1 exit /b 1

set REQ=
for /f %%v in (toolchain\REQUIRED_VERSION) do set REQ=%%v
echo Toolchain required by this branch: v%REQ%
if not "%REQ%"=="22" (
  echo EXPECTED v22 on the 1.3 line - wrong branch or a bad checkout. Stopping.
  exit /b 1
)

REM ---------------------------------------------------------------------------
REM  Mandatory when coming from the chopin build - see 2-build-chopin.cmd.
REM ---------------------------------------------------------------------------
echo.
echo === Nuking build dir (toolchain boundary) ===
call dbt nuke
if errorlevel 1 exit /b 1

echo.
echo === Building ===
REM  RELEASE_TYPE already defaults to "dev" on the 1.3 line, so the filename would
REM  pick up DISPLAY_VERSION either way. -t beta is passed so the semver prerelease
REM  id in the firmware reads "-beta" rather than "-dev".
call dbt build release -m -t beta
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

echo.
echo === Output ===
dir /b build\Release\*.bin
echo.
echo Expect a name like: deluge-v1_3_0-rudement+YYYY_MM_DD-^<sha^>.bin

REM Get the binary out of build\Release NOW - see collect-binaries.cmd for why.
call "%~dp0collect-binaries.cmd"
exit /b 0
