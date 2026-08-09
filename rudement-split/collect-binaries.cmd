@echo off
REM ============================================================================
REM  Copy every .bin in build\Release to the "Rude Claude" folder on the Desktop.
REM
REM  WHY THIS EXISTS: `dbt nuke` deletes the whole build directory, and both build
REM  scripts must nuke because they cross the v16 / v22 toolchain boundary. So
REM  building Chopin and then Beta 1.3 destroys the Chopin binary before anyone
REM  copies it anywhere. Binaries have to leave build\Release before the next
REM  build starts. Scripts 2 and 3 now call this automatically on success.
REM
REM  Safe to run on its own at any time.
REM ============================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0.."

REM Resolve the real Desktop path from the registry rather than assuming
REM %USERPROFILE%\Desktop - this Desktop is redirected into OneDrive.
set "DESKTOP="
for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders" /v Desktop 2^>nul') do set "DESKTOP=%%b"
call set "DESKTOP=%DESKTOP%"
if not defined DESKTOP set "DESKTOP=%USERPROFILE%\Desktop"

set "DEST=%DESKTOP%\Rude Claude"

echo.
echo Desktop resolved to: %DESKTOP%
echo Destination:         %DEST%

if not exist "%DEST%" (
  echo.
  echo Destination folder does not exist. Creating it.
  mkdir "%DEST%" || ( echo Could not create "%DEST%" & exit /b 1 )
)

set FOUND=0
echo.
echo === Copying ===
for %%f in (build\Release\*.bin) do (
  set FOUND=1
  copy /y "%%f" "%DEST%\" >nul && echo   %%~nxf  ^(%%~zf bytes^) || echo   FAILED: %%~nxf
)

if "%FOUND%"=="0" (
  echo   Nothing in build\Release to copy.
  echo   Either no build has run yet, or dbt nuke has already cleared it.
  exit /b 1
)

echo.
echo === Now in "%DEST%" ===
dir /b "%DEST%\*.bin"
exit /b 0
