@echo off
REM ============================================================================
REM  Import the ten split feature branches from the bundle.
REM
REM  Run from the repo root:   rudement-split\1-import-branches.cmd
REM
REM  CLOSE GITHUB DESKTOP FIRST. It moves HEAD mid-operation (handoff section 6).
REM  Run 0-cleanup-locks.cmd before this if you have not already.
REM ============================================================================

setlocal
cd /d "%~dp0.."

echo.
echo === Sanity checks ===

git rev-parse --git-dir >nul 2>&1
if errorlevel 1 ( echo Not in a git repo, or git is jammed. Run 0-cleanup-locks.cmd. & exit /b 1 )

REM -uno = ignore untracked files. The rudement-split folder and HANDOFF.md are
REM deliberately untracked; untracked files survive branch switches, so they are
REM not a reason to refuse. Only TRACKED modifications matter here.
for /f %%i in ('git status --porcelain -uno') do (
  echo Tracked files are modified. Commit or stash them first.
  git status --short -uno
  exit /b 1
)

REM NOTE: do not write ^{commit} here. In a .cmd file "^" is the escape character,
REM so git would receive "a2e333b9{commit}" and always report it missing.
REM rev-parse --verify on the bare SHA does the same job with no caret.
git rev-parse --verify --quiet a2e333b9 >nul || ( echo Missing 1.2.1 base a2e333b9 & exit /b 1 )
git rev-parse --verify --quiet 134d000f >nul || ( echo Missing 1.3 base 134d000f & exit /b 1 )
echo Both base commits present.

echo.
echo === Verifying bundle ===
git bundle verify "%~dp0rudement-split.bundle"
if errorlevel 1 ( echo Bundle failed verification. & exit /b 1 )

echo.
echo === Importing branches ===
git fetch "%~dp0rudement-split.bundle" ^
  "refs/heads/feat/sear-12:refs/heads/feat/sear-12" ^
  "refs/heads/feat/eq-readout-12:refs/heads/feat/eq-readout-12" ^
  "refs/heads/feat/gristle-on-12:refs/heads/feat/gristle-on-12" ^
  "refs/heads/feat/ci-chopin-12:refs/heads/feat/ci-chopin-12" ^
  "refs/heads/feat/gristle-13:refs/heads/feat/gristle-13" ^
  "refs/heads/feat/sear-13:refs/heads/feat/sear-13" ^
  "refs/heads/feat/eq-readout-13:refs/heads/feat/eq-readout-13" ^
  "refs/heads/feat/gristle-on-13:refs/heads/feat/gristle-on-13" ^
  "refs/heads/feat/midi-fx-13:refs/heads/feat/midi-fx-13" ^
  "refs/heads/feat/version-label-13:refs/heads/feat/version-label-13"
if errorlevel 1 ( echo Fetch failed. & exit /b 1 )

REM Integration branch for the 1.3 beta build: chopin-to-13 + the two arp commits.
git branch -f beta-1.3 a99b641b

REM Safety copy of the 1.2.1 line under a name nothing is syncing to upstream.
git rev-parse --verify chopin-rudement >nul 2>&1
if errorlevel 1 (
  git branch chopin-rudement chopin && echo   created chopin-rudement from chopin
) else (
  echo   chopin-rudement already exists, left alone
)

echo.
echo === Result ===
call :show feat/sear-12          a2e333b9
call :show feat/eq-readout-12    a2e333b9
call :show feat/gristle-on-12    a2e333b9
call :show feat/ci-chopin-12     a2e333b9
call :show feat/gristle-13       134d000f
call :show feat/sear-13          134d000f
call :show feat/eq-readout-13    134d000f
call :show feat/gristle-on-13    134d000f
call :show feat/midi-fx-13       134d000f
call :show feat/version-label-13 134d000f
echo.
echo   beta-1.3        -^> a99b641b (integration branch for the 1.3 beta build)
echo   chopin-rudement -^> 4d68a0b1 (safety copy of the 1.2.1 line - PUBLISH THIS)
echo.
echo Done. Next: rudement-split\2-build-chopin.cmd  and  3-build-beta13.cmd
exit /b 0

:show
for /f %%s in ('git rev-parse --short %1') do set SHA=%%s
for /f %%n in ('git rev-list --count %2..%1') do set N=%%n
echo   %1  %SHA%  (%N% commit(s) over %2)
exit /b 0
