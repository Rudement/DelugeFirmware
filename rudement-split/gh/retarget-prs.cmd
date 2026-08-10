@echo off
REM ============================================================================
REM  Make every PR target its line's BASE branch, so each shows exactly its own
REM  feature diff and nothing else.
REM
REM  Right now targets are inconsistent: seven PRs point at chopin-rudement or
REM  beta-1.3, and three point at base-13 - because feat/gristle-13 and
REM  feat/sear-13 are ancestors of beta-1.3 and GitHub refused any other base.
REM
REM  Pointing everything at base-12 / base-13 fixes that. A PR against a release
REM  branch that ALREADY contains the work shows the right diff but is a strange
REM  object; against the base commit it is exactly "this feature, alone".
REM
REM  Reversible: re-run gh pr edit --base with the old value to undo.
REM
REM  Run from the repo root:  rudement-split\gh\retarget-prs.cmd
REM ============================================================================

setlocal
cd /d "%~dp0..\.."

where gh >nul 2>&1
if errorlevel 1 ( echo gh not on PATH. & exit /b 1 )

for %%b in (base-12 base-13) do (
  git rev-parse --verify --quiet %%b >nul
  if errorlevel 1 (
    echo Missing branch %%b. Run rudement-split\gh\fix-remaining-prs.cmd first.
    exit /b 1
  )
)

echo.
echo === 1.2.1 branches -^> base-12 ===
call :retarget feat/sear-12       base-12
call :retarget feat/eq-readout-12 base-12
call :retarget feat/gristle-on-12 base-12
call :retarget feat/ci-chopin-12  base-12

echo.
echo === 1.3 branches -^> base-13 ===
call :retarget feat/gristle-13       base-13
call :retarget feat/sear-13          base-13
call :retarget feat/eq-readout-13    base-13
call :retarget feat/gristle-on-13    base-13
call :retarget feat/midi-fx-13       base-13
call :retarget feat/version-label-13 base-13

echo.
echo === Result ===
gh pr list --limit 20
exit /b 0

:retarget
gh pr edit "%~1" --base "%~2" >nul 2>&1
if errorlevel 1 (
  echo   skipped %~1 ^(already on %~2, or no PR^)
) else (
  echo   %~1 -^> %~2
)
exit /b 0
