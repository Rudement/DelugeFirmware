@echo off
REM ============================================================================
REM  RUN THIS FIRST. Nothing else — GitHub Desktop included — will work until
REM  you do.
REM
REM  The Cowork sandbox mounts this folder over FUSE, which allows file creation
REM  but denies deletion. Git took an index lock and could not release it, so
REM  .git\index.lock is sitting there blocking every git operation. Deletion
REM  works normally from Windows; it only fails from the sandbox side.
REM
REM  Run from the repo root:  rudement-split\0-cleanup-locks.cmd
REM ============================================================================

setlocal
cd /d "%~dp0.."

echo.
echo === Removing stale lock files ===
if exist ".git\index.lock"    ( del /f /q ".git\index.lock"    && echo   removed .git\index.lock )
if exist ".git\HEAD.lock"     ( del /f /q ".git\HEAD.lock"     && echo   removed .git\HEAD.lock )
if exist ".git\__probe1"      ( del /f /q ".git\__probe1"      && echo   removed .git\__probe1 )
if exist "__probe3"           ( del /f /q "__probe3"           && echo   removed __probe3 )

echo.
echo === Removing the phantom worktree ===
if exist ".git\worktrees\wt" (
  rmdir /s /q ".git\worktrees\wt" && echo   removed .git\worktrees\wt
)
git worktree prune
echo   pruned

echo.
echo === Verifying git works again ===
git status --short
if errorlevel 1 (
  echo.
  echo git is STILL failing. Look for any other *.lock under .git\ :
  dir /s /b ".git\*.lock"
  exit /b 1
)

echo.
echo === Branch state ===
git rev-parse --abbrev-ref HEAD
for %%b in (chopin chopin-to-13 midi-fx) do (
  for /f %%s in ('git rev-parse --short %%b') do echo   %%b = %%s
)

echo.
echo Expected: chopin=4d68a0b1  chopin-to-13=b6925826  midi-fx=a99b641b
echo If chopin is NOT 4d68a0b1, stop and check the reflog: git reflog show chopin
echo.
echo git is healthy. Next: rudement-split\1-import-branches.cmd
exit /b 0
