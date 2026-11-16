@echo off
rem Makes a playable copy of the current build: xc3.exe + its DLLs in a folder of their own, with "Play XC3.bat".
rem
rem   make_playable.bat                     -> ..\XC3-Playable, uses this repo's game\ folder (saves, settings shared)
rem   make_playable.bat "D:\Games\XC3"      -> that folder instead
rem   make_playable.bat "D:\Games\XC3" full -> also copies game\ there (about 20 GB): a standalone copy that keeps
rem                                            working while the repo is rebuilt or moved
rem
rem Settings: press TAB in game (window size, borderless, internal resolution, frame cap, VSync, FSR, volume);
rem they are saved to xc3.ini in the game folder. Alt+Enter toggles borderless fullscreen.
setlocal
cd /d "%~dp0"
set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"
set "DEST=%~1"
if "%DEST%"=="" set "DEST=%REPO%\..\XC3-Playable"
set "MODE=%~2"

if not exist "build-cmake\xc3.exe" (
  echo No build yet: run build-windows.bat first.
  pause & exit /b 1
)
tasklist /fi "imagename eq xc3.exe" 2> nul | find /i "xc3.exe" > nul
if not errorlevel 1 echo Note: xc3.exe is running; the copy is of the exe on disk.

if not exist "%DEST%" mkdir "%DEST%"
for %%I in ("%DEST%") do set "DEST=%%~fI"
echo Copying the build to %DEST% ...
copy /y "build-cmake\xc3.exe" "%DEST%\" > nul || (echo copy failed & pause & exit /b 1)
if exist "build-cmake\xc3-launcher.exe" copy /y "build-cmake\xc3-launcher.exe" "%DEST%\XC3 Launcher.exe" > nul
copy /y "build-cmake\*.dll" "%DEST%\" > nul
if exist "build-cmake\xc3.pdb" copy /y "build-cmake\xc3.pdb" "%DEST%\" > nul

if /i "%MODE%"=="full" (
  echo Copying the game folder ^(romfs, DLC, saves, shader cache^) - this takes a while ...
  robocopy "%REPO%\game" "%DEST%\game" /E /NFL /NDL /NJH /NP /XD frames debugdraw /XF play-log.txt > nul
  if errorlevel 8 (echo game folder copy failed & pause & exit /b 1)
  set "GAMEDIR=%DEST%\game"
) else (
  set "GAMEDIR=%REPO%\game"
)

rem (the launcher finds the game folder through launcher.ini)
> "%DEST%\launcher.ini" (
  echo [launcher]
  echo game_dir=%GAMEDIR%
)
> "%DEST%\Play XC3.bat" (
  echo @echo off
  echo rem Xenoblade Chronicles 3 ^(static recompilation^). TAB in game: settings. Alt+Enter: borderless fullscreen.
  echo cd /d "%GAMEDIR%"
  echo "%DEST%\xc3.exe" --game . --log 1 ^> "%DEST%\play-log.txt" 2^>^&1
)
> "%DEST%\README.txt" (
  echo Xenoblade Chronicles 3 - recompiled PC build
  echo.
  echo Start: "Play XC3.bat". Game data: %GAMEDIR%
  echo Settings: TAB in game ^(Esc or TAB closes^), saved to %GAMEDIR%\xc3.ini.
  echo   Window size / borderless, internal resolution ^(restart to apply^), frame cap, menu frame cap,
  echo   VSync mode, upscaler ^(bilinear or AMD FSR 1 + sharpness^), volume, frame-rate counter.
  echo Keys: WASD move, IJKL camera, Enter/Z = A, Backspace/X = B, C = X, V = Y, Q/E = L/R, 1/3 = ZL/ZR,
  echo   Esc/P = +, Shift = -, arrows = d-pad, F/H = stick clicks. Xbox-style gamepads work too.
  echo   Alt+Enter: borderless fullscreen on/off.
  echo Log of the last session: play-log.txt
)
echo.
echo Done: "%DEST%\Play XC3.bat"
if /i not "%MODE%"=="full" echo ^(uses the game folder %GAMEDIR%; add "full" to copy it too^)
endlocal
