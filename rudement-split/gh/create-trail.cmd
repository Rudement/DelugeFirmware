@echo off
REM ============================================================================
REM  Create the GitHub trail: 4 issues, then 10 draft PRs.
REM
REM  Requires:  gh auth status   to come back clean. Run in a NEW terminal after
REM             installing gh - PATH is stale in any shell that was already open.
REM
REM  Run from the repo root:  rudement-split\gh\create-trail.cmd
REM
REM  Idempotency: this does NOT check for existing PRs/issues. Running it twice
REM  creates duplicates. If a step fails partway, delete what it made before
REM  re-running, or comment out the lines that already succeeded.
REM ============================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0..\.."

set "BODY=%~dp0"

where gh >nul 2>&1
if errorlevel 1 (
  echo gh is not on PATH. Open a NEW terminal after installing it.
  exit /b 1
)

gh auth status >nul 2>&1
if errorlevel 1 (
  echo gh is not authenticated. Run:  gh auth login
  exit /b 1
)

REM ---------------------------------------------------------------------------
REM  This repo has TWO remotes - origin (the fork) and upstream (SynthstromAudible).
REM  gh refuses to guess, and guessing wrong would file issues and PRs on
REM  Synthstrom's repository. Require the default to be set, and require it to be
REM  the fork.
REM ---------------------------------------------------------------------------
set "DEFREPO="
for /f "delims=" %%r in ('gh repo set-default --view 2^>nul') do set "DEFREPO=%%r"
if not defined DEFREPO (
  echo.
  echo No default repo set. Run this first - and make sure it is YOUR FORK:
  echo     gh repo set-default Rudement/DelugeFirmware
  exit /b 1
)
echo Default repo: %DEFREPO%
echo %DEFREPO% | find /i "SynthstromAudible" >nul
if not errorlevel 1 (
  echo.
  echo REFUSING TO RUN. The default repo is SynthstromAudible's, not your fork.
  echo This would open issues and PRs on the upstream project. Fix with:
  echo     gh repo set-default Rudement/DelugeFirmware
  exit /b 1
)

REM ---------------------------------------------------------------------------
REM  GitHub disables Issues on forks by default. Without this the four issue
REM  creations below fail in a way that looks like a label problem.
REM ---------------------------------------------------------------------------
gh issue list --limit 1 >nul 2>&1
if errorlevel 1 (
  echo.
  echo Issues appear to be disabled on this fork. Enable them with:
  echo     gh repo edit --enable-issues
  echo Then re-run this script.
  exit /b 1
)

echo.
echo === Issues ===

gh issue create --title "Sear: does kSearDriveBoost actually fix \"too tame\"? (needs hardware)" ^
  --body-file "%BODY%issue\sear-drive-boost.md" --label "needs-hardware-test" || goto :nolabel

gh issue create --title "Gristleizer: verify bypass behaviour on hardware (LFO timeline lock)" ^
  --body-file "%BODY%issue\gristle-bypass-lfo.md" --label "needs-hardware-test"

gh issue create --title "EQ: verify Hz/dB readout on hardware, Treble especially" ^
  --body-file "%BODY%issue\eq-treble-readout.md" --label "needs-hardware-test"

gh issue create --title "Upstream submission: dependency order and prerequisites" ^
  --body-file "%BODY%issue\upstream-order.md" --label "upstream"

goto :prs

:nolabel
echo.
echo   Label did not exist. Creating labels, then retrying without --label.
gh label create "needs-hardware-test" --color "D93F0B" --description "Needs listening on real hardware" 2>nul
gh label create "upstream" --color "0E8A16" --description "Relates to submitting upstream" 2>nul
gh issue create --title "Sear: does kSearDriveBoost actually fix \"too tame\"? (needs hardware)" --body-file "%BODY%issue\sear-drive-boost.md"
gh issue create --title "Gristleizer: verify bypass behaviour on hardware (LFO timeline lock)" --body-file "%BODY%issue\gristle-bypass-lfo.md"
gh issue create --title "EQ: verify Hz/dB readout on hardware, Treble especially" --body-file "%BODY%issue\eq-treble-readout.md"
gh issue create --title "Upstream submission: dependency order and prerequisites" --body-file "%BODY%issue\upstream-order.md"

:prs
echo.
echo === Draft PRs: 1.2.1 line -^> chopin-rudement ===

call :mkpr feat/sear-12         chopin-rudement "Sear: rename from Heat, automatic level matching (1.2.1)" sear-12
call :mkpr feat/eq-readout-12   chopin-rudement "EQ: report each band in Hz and dB (1.2.1)"                eq-readout-12
call :mkpr feat/gristle-on-12   chopin-rudement "Gristleizer: master switch to bypass the effect (1.2.1)"  gristle-on-12
call :mkpr feat/ci-chopin-12    chopin-rudement "CI: build this fork's chopin branch on push"              ci-chopin-12

echo.
echo === Draft PRs: 1.3 line -^> beta-1.3 ===

call :mkpr feat/gristle-13       beta-1.3 "The Gristleizer: standalone 9-parameter effect on 1.3"     gristle-13
call :mkpr feat/sear-13          beta-1.3 "Sear: rename from Heat, automatic level matching (1.3)"    sear-13
call :mkpr feat/eq-readout-13    beta-1.3 "EQ: Hz/dB readout, and fix the reverse CC map (1.3)"       eq-readout-13
call :mkpr feat/gristle-on-13    beta-1.3 "Gristleizer: master switch to bypass the effect (1.3)"     gristle-on-13
call :mkpr feat/midi-fx-13       beta-1.3 "Collapse the arpeggiator fan-out into single dispatch seams" midi-fx-13
call :mkpr feat/version-label-13 beta-1.3 "Label 1.3 builds as 1.3.0-rudement"                        version-label-13

echo.
echo === Result ===
gh pr list --limit 20
echo.
gh issue list --limit 20
exit /b 0

REM ---------------------------------------------------------------------------
REM  %1 head branch   %2 base branch   %3 title   %4 body filename stem
REM ---------------------------------------------------------------------------
:mkpr
gh pr create --draft --head "%~1" --base "%~2" --title "%~3" --body-file "%BODY%pr\%~4.md" 2>&1
if errorlevel 1 (
  echo   FAILED: %~1
  echo     If this said "No commits between" - the base already contains this work.
  echo     Fix: create a base branch at the split point and target that instead, e.g.
  echo       git branch base-12 a2e333b9   ^&^&  git push origin base-12
  echo       git branch base-13 134d000f   ^&^&  git push origin base-13
  echo     then re-run with base-12 / base-13 in place of chopin-rudement / beta-1.3.
)
exit /b 0
