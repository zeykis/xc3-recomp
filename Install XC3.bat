@echo off
rem Xenoblade Chronicles 3 - recompiled PC version: installer. Double-click it.
rem Downloads its tools into .tools\ (nothing to install first), opens XC3 Setup for YOUR game files (base game,
rem update 2.2.1, DLC; it finds Eden / yuzu-based emulators' games, keys and saves), builds the game and makes
rem shortcuts. Run it again to update or to continue after an interruption.
rem   "Install XC3.bat" setup    opens Setup again first (add DLC, reinstall the game files)
setlocal
cd /d "%~dp0"
set "XARGS="
if /i "%~1"=="setup" set "XARGS=-Setup"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install\install.ps1" %XARGS%
set "RC=%errorlevel%"
echo.
pause
exit /b %RC%
