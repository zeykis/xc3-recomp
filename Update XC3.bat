@echo off
rem Xenoblade Chronicles 3 - recompiled PC version: updater. Double-click it (or "Check for updates" in the launcher).
rem Gets the latest version from GitHub (git pull for a clone, the zip for a downloaded copy: your game files, saves
rem and settings stay), rebuilds what changed (usually a few minutes) and installs it into bin\.
rem   "Update XC3.bat" check    only says whether an update is available
setlocal
cd /d "%~dp0"
set "XARGS=-Update"
if /i "%~1"=="check" set "XARGS=-CheckOnly"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install\install.ps1" %XARGS%
set "RC=%errorlevel%"
echo.
pause
exit /b %RC%
