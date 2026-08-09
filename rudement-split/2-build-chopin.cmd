@echo off
REM ============================================================================
REM  Build Chopin (1.2.1-rudement) from the `chopin` branch.
REM  Toolchain: v16.
REM
REM  Run from the repo root:  rudement-split\2-build-chopin.cmd
REM  CLOSE GITHUB DESKTOP FIRST.
REM ============================================================================

setlocal
cd /d "%~dp0.."

REM ---------------------------------------------------------------------------
REM  g_menus.inc is GENERATED. Every build rewrites it with CRLF while git stores
REM  LF, so it shows as a tracked modification afterwards and blocks the next
REM  branch switch. Discard it, but ONLY if the sole difference is line endings -
REM  git diff --quiet --ignore-cr-at-eol exits 0 when there is no real change.
REM  If someone ever makes a genuine edit there, this leaves it alone and the
REM  guard below still catches it.
REM ---------------------------------------------------------------------------
set "GENFILE=src/deluge/gui/menu_item/generate/g_menus.inc"
git diff --quiet --ignore-cr-at-eol -- "%GENFILE%" 2>nul
if not errorlevel 1 (
  git checkout -- "%GENFILE%" 2>nul && echo Discarded line-ending-only change to g_menus.inc
)

REM -uno = ignore untracked files (rudement-split\, HANDOFF.md). Untracked files
REM survive branch switches and are not a reason to refuse.
for /f %%i in ('git status --porcelain -uno') do (
  echo Tracked files are modified. Commit or stash them first.
  git status --short -uno
  exit /b 1
)

echo.
echo === Checking out chopin ===
git checkout chopin
if errorlevel 1 exit /b 1

REM toolchain/REQUIRED_VERSION is tracked per branch; after checkout it must read 16.
set REQ=
for /f %%v in (toolchain\REQUIRED_VERSION) do set REQ=%%v
echo Toolchain required by this branch: v%REQ%
if not "%REQ%"=="16" (
  echo EXPECTED v16 on chopin - wrong branch or a bad checkout. Stopping.
  exit /b 1
)

REM ---------------------------------------------------------------------------
REM  dbt nuke is MANDATORY when crossing the v16 / v22 boundary. CMake caches the
REM  absolute compiler path in build\CMakeCache.txt; a plain rebuild silently
REM  compiles with whichever toolchain configured the cache first.
REM ---------------------------------------------------------------------------
echo.
echo === Nuking build dir (toolchain boundary) ===
call dbt nuke
if errorlevel 1 exit /b 1

echo.
echo === Building ===
REM  -m is REQUIRED. Without it RELEASE_TYPE defaults to "release" on this branch,
REM  which takes the -c${PROJECT_VERSION} filename branch and reports c1.2.0,
REM  ignoring DISPLAY_VERSION entirely. -m implies -t dev, which is what produces
REM  the 1.2.1-rudement label.
call dbt build release -m
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

echo.
echo === Output ===
dir /b build\Release\*.bin
echo.
echo Expect a name like: deluge-v1_2_1-rudement+YYYY_MM_DD-^<sha^>.bin
echo A "-dirty" suffix here is cosmetic: the build rewrites the generated
echo g_menus.inc with CRLF while git stores LF. Diff ignoring line endings is empty.

REM Get the binary out of build\Release NOW. The next build script runs dbt nuke,
REM which deletes this whole directory before its own build starts.
call "%~dp0collect-binaries.cmd"
exit /b 0
