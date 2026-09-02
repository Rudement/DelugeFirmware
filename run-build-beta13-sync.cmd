@echo off
cd /d "%~dp0"
set LOG=beta13-sync-build.log
set WANT=integration/beta-13-2026-09-01

echo === 1.3.0 beta sync (upstream 7064d10 merged) === > %LOG%
echo --- where we are now --- >> %LOG%
git rev-parse --abbrev-ref HEAD >> %LOG% 2>&1
git log -1 --oneline >> %LOG% 2>&1

rem Stranded index.lock. Git run against this folder over the Cowork bridge cannot
rem delete its own lock files, so a read-only command from there can leave one behind
rem and every later git call fails. Windows git can delete it.
rem If GitHub Desktop is mid-operation, close it before running this.
echo --- clearing any stranded git lock --- >> %LOG%
if exist .git\index.lock del /f /q .git\index.lock >> %LOG% 2>&1
if exist .git\HEAD.lock del /f /q .git\HEAD.lock >> %LOG% 2>&1

rem g_menus.inc reads as modified because a build rewrote it with CRLF line endings.
rem The content diff is empty. Discarding it is the whole fix.
echo --- discarding the CRLF-only change to g_menus.inc --- >> %LOG%
git checkout -- src/deluge/gui/menu_item/generate/g_menus.inc >> %LOG% 2>&1

echo --- switching to %WANT% --- >> %LOG%
git checkout %WANT% >> %LOG% 2>&1
if errorlevel 1 goto :checkoutfailed

for /f %%B in ('git rev-parse --abbrev-ref HEAD') do set ON=%%B
if not "%ON%"=="%WANT%" goto :checkoutfailed
echo on branch %ON% >> %LOG% 2>&1
git log -1 --oneline >> %LOG% 2>&1

rem The 1.3 line wants toolchain v22. REQUIRED_VERSION is tracked per branch, so the
rem checkout above sets it -- this only catches a checkout that did not do what it said.
for /f %%V in (toolchain\REQUIRED_VERSION) do set TCV=%%V
echo toolchain REQUIRED_VERSION=%TCV% >> %LOG%
if not "%TCV%"=="22" goto :strayfiles
echo STRAY UNTRACKED FILES UNDER src\ - NOT BUILDING. Nothing was wiped. >> %LOG%
echo These are left over from a branch switch and would be compiled into the build: >> %LOG%
findstr /b /c:"??" "%TEMP%\beta13-stray.txt" >> %LOG%
echo. >> %LOG%
echo Check each one is recoverable from the branch it came from, then delete it: >> %LOG%
echo   git rev-parse ^<that-branch^>:^<path^>  and  git hash-object ^<path^>  should match. >> %LOG%
goto :done

:wrongtoolchain

rem A branch switch made over the Cowork mount cannot delete files, so files that exist
rem only on the branch you left survive as untracked strays -- and CMake globs src\, so it
rem compiles them. That is what broke the first run of this script: cv_audio_stream.cpp from
rem the AUX Sends branch, referencing a RuntimeFeatureSettingType this branch does not have.
rem git status hides them unless you ask for untracked files, so ask.
echo --- checking for stray untracked files under src\ --- >> %LOG%
git status --porcelain --untracked-files=all -- src > "%TEMP%\beta13-stray.txt" 2>>%LOG%
findstr /b /c:"??" "%TEMP%\beta13-stray.txt" >nul
if not errorlevel 1 goto :strayfiles

rem Wiping on purpose, not out of caution about the line. The point of this build is to
rem find out whether commit b37e6db3 is sound, so nothing in build\ from an earlier
rem configure is allowed to survive into it. CMake caches the absolute compiler path,
rem and a stale cache is exactly the kind of thing that would muddy the answer.
echo --- wiping build\ so this is a clean configure --- >> %LOG%
if exist build rmdir /s /q build >> %LOG% 2>&1

echo --- build (1.3 line, toolchain v22, from scratch - this takes a while) --- >> %LOG%
call dbt.cmd build release -m >> %LOG% 2>&1
echo BUILD_EXIT=%errorlevel% >> %LOG%

echo --- what came out --- >> %LOG%
dir /b /o-d build\Release\*.bin >> %LOG% 2>&1
rem Newest only, so an older .bin left in build\Release cannot wear this build's prefix.
for /f "delims=" %%F in ('dir /b /o-d build\Release\*.bin') do (
  copy /Y "build\Release\%%F" "BETA13-%%F" >> %LOG% 2>&1
  goto :copied
)
:copied
dir /b BETA13-*.bin >> %LOG% 2>&1
goto :done

:wrongtoolchain
echo WRONG TOOLCHAIN (%TCV%, wanted 22) - NOT BUILDING. Nothing was wiped. >> %LOG%
goto :done

:checkoutfailed
echo CHECKOUT FAILED - NOT BUILDING. Nothing was wiped, nothing was built. >> %LOG%
echo. >> %LOG%
echo Still-modified files: >> %LOG%
git status --short >> %LOG% 2>&1

:done
echo.
echo ================ TAIL ================
powershell -NoProfile -Command "Get-Content '%LOG%' -Tail 40"
echo ======================================
echo.
echo Full log is in beta13-sync-build.log
echo.
echo The binary to flash is the BETA13-deluge-v1_3_0-rudement+*.bin named above.
echo NOTE: going back to a 1.2.1 branch afterwards needs the build dir wiped again.
echo   git checkout feat/grid-gestures-12   then   rmdir /s /q build
pause
