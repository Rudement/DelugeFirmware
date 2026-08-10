@echo off
REM ============================================================================
REM  Record the 2026-08-09 hardware listening results.
REM
REM   #1 Sear drive boost  - still too tame. Comment with diagnosis, LEAVE OPEN.
REM   #2 Gristle bypass    - passes. Comment and close.
REM   #3 EQ Hz/dB readout  - passes. Comment and close.
REM
REM  Run from the repo root:  rudement-split\gh\log-hardware-results.cmd
REM ============================================================================

setlocal
cd /d "%~dp0..\.."
set "C=%~dp0comment\"

where gh >nul 2>&1
if errorlevel 1 ( echo gh not on PATH. & exit /b 1 )

echo.
echo === #1 Sear: still too tame (staying open) ===
gh issue comment 1 --body-file "%C%issue-1-still-tame.md"
gh issue edit 1 --add-label "needs-hardware-test" 2>nul

echo.
echo === #2 Gristle bypass: passes ===
gh issue comment 2 --body-file "%C%issue-2-pass.md"
gh issue close 2 --reason completed

echo.
echo === #3 EQ readout: passes ===
gh issue comment 3 --body-file "%C%issue-3-pass.md"
gh issue close 3 --reason completed

echo.
echo === Open issues ===
gh issue list
exit /b 0
