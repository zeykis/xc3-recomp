@echo off
rem Xenoblade Chronicles 3 - recompiled PC version: installer. Double-click it.
rem Downloads its tools into .tools\ (nothing to install first), opens XC3 Setup for YOUR game files (base game,
rem update 2.2.1, DLC; it finds Eden / yuzu-based emulators' games, keys and saves), builds the game and makes
rem shortcuts. Run it again to update or to continue after an interruption.
rem   "Install XC3.bat" setup    opens Setup again first (add DLC, reinstall the game files)
rem   "Install XC3.bat" verbose  also prints every compiler command
setlocal
cd /d "%~dp0"
set "XARGS="
for %%A in (%*) do (
  if /i "%%~A"=="setup" set "XARGS=-Setup"
  if /i "%%~A"=="verbose" set "XV=-Commands"
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install\install.ps1" %XARGS% %XV%
set "RC=%errorlevel%"
echo.
pause
exit /b %RC%
