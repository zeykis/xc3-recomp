@echo off
rem Packages the installer: dist\XC3-Recomp-Setup\ with "XC3 Setup.exe" and files\ (xc3.exe + DLLs, hactool.exe,
rem the patches and graphics mods, the shader cache). Give that folder (or the .zip, with "zip") to the player:
rem the setup asks for THEIR game files (base game .xci/.nsp, update 2.2.1 and DLC .nsp/.nca, or an emulator's
rem installs) and their console keys, and installs everything. No game data is in the package, but xc3.exe is
rem made from the game's code: only share it with people who own the game.
rem
rem   make_installer.bat          build first (build-windows.bat), then package
rem   make_installer.bat nobuild  package the current build
rem   add "zip" to also write dist\XC3-Recomp-Setup.zip
setlocal
cd /d "%~dp0"
if /i not "%~1"=="nobuild" if /i not "%~2"=="nobuild" (
  echo Building...
  call build-windows.bat > build-log.txt 2>&1 < nul
  if errorlevel 1 (echo BUILD FAILED - see build-log.txt & pause & exit /b 1)
)
if not exist build-cmake\xc3-setup.exe (echo build-cmake\xc3-setup.exe missing: build first & pause & exit /b 1)
set "HT="
if exist tools\hactool.exe set "HT=tools\hactool.exe"
if "%HT%"=="" (echo hactool.exe not found: build-windows.bat downloads it to tools\hactool.exe & pause & exit /b 1)

set "OUT=dist\XC3-Recomp-Setup"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\files" || exit /b 1
copy /y build-cmake\xc3-setup.exe "%OUT%\XC3 Setup.exe" > nul
copy /y build-cmake\xc3.exe "%OUT%\files\" > nul
if exist build-cmake\xc3-launcher.exe copy /y build-cmake\xc3-launcher.exe "%OUT%\files\XC3 Launcher.exe" > nul
copy /y build-cmake\*.dll "%OUT%\files\" > nul
copy /y "%HT%" "%OUT%\files\hactool.exe" > nul
if exist patches robocopy patches "%OUT%\files\patches" /E /NFL /NDL /NJH /NJS /NP /XF README.md > nul
if exist game\pipeline_cache.bin copy /y game\pipeline_cache.bin "%OUT%\files\" > nul
> "%OUT%\README.txt" (
  echo Xenoblade Chronicles 3 - recompiled PC version
  echo.
  echo 1. Run "XC3 Setup.exe".
  echo 2. Put your files in their boxes: base game ^(.xci / .nsp^), update 2.2.1 ^(.nsp / .nca^), DLC ^(.nsp / .nca,
  echo    any number^). You can also drop files or a folder on the window: they are sorted by what they contain.
  echo    To add DLC later: run Setup again on the same folder, tick "Keep the installed base game + update",
  echo    add the DLC and Install.
  echo    Games installed in Eden / yuzu-based emulators are found automatically ^(and their saves^).
  echo 3. Select prod.keys from your own console ^(found automatically when an emulator has them^).
  echo 4. Install. About 20 GB of disk space is needed ^(more while installing from an .xci / .nsp^).
  echo.
  echo In game: TAB = settings ^(window, resolution, frame rate, VSync, FSR, volume, graphics mods^),
  echo Alt+Enter = borderless fullscreen. Keyboard or Xbox-style gamepad.
)
echo Packaged: %OUT%
if /i "%~1"=="zip" goto zip
if /i "%~2"=="zip" goto zip
goto end
:zip
echo Zipping...
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath 'dist\XC3-Recomp-Setup.zip' -Force" && echo Wrote dist\XC3-Recomp-Setup.zip
:end
endlocal
