@echo off
REM Windows build for xc3-recomp. Run from anywhere:
REM   build-windows.bat         everything. The first time (no game\ yet) it builds and opens XC3 Setup, which installs
REM                             your own base game / update / DLC into game\, then recompiles game\main.elf into gen\
REM                             and builds build-cmake\xc3.exe
REM   build-windows.bat tools   the tools only (no game files needed): xc3-setup.exe, xc3-launcher.exe
REM Needs: Python 3, LLVM (clang), Visual Studio Build Tools (C++ workload = linker + Windows SDK), CMake.
setlocal enabledelayedexpansion
cd /d "%~dp0"
set "MODE=full"
if /i "%~1"=="tools" set "MODE=tools"

echo [1/5] Python
where python >nul 2>nul || (echo   Python not found. Install it: winget install Python.Python.3.12 & exit /b 1)
python -m pip install -q --upgrade capstone ninja || exit /b 1
for /f "delims=" %%i in ('python -c "import ninja,os;print(os.path.join(ninja.BIN_DIR,'ninja.exe'))"') do set NINJA=%%i
echo   ninja: !NINJA!

echo [2/5] clang
set CLANG=
for /f "delims=" %%i in ('where clang 2^>nul') do if not defined CLANG set CLANG=%%i
if not defined CLANG if exist "%ProgramFiles%\LLVM\bin\clang.exe" set CLANG=%ProgramFiles%\LLVM\bin\clang.exe
if not defined CLANG (
  echo   clang not found. Install LLVM:  winget install LLVM.LLVM   then re-run this script.
  exit /b 1
)
set CLANGXX=!CLANG:clang.exe=clang++.exe!
echo   clang: !CLANG!

echo [3/5] cmake + MSVC linker/SDK
where cmake >nul 2>nul || (echo   CMake not found. Install it: winget install Kitware.CMake & exit /b 1)
REM clang uses the MSVC linker + Windows SDK libraries. Load the VS environment if not already loaded.
REM (no parenthesized blocks here: the ")" in "Program Files (x86)" would end them early)
if defined VCINSTALLDIR goto :vs_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" goto :vs_find
echo   Visual Studio Build Tools not found. Install with:
echo     winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
exit /b 1
:vs_find
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if defined VSPATH goto :vs_load
echo   Visual Studio is installed but the C++ build tools are missing.
echo   Open "Visual Studio Installer" ^> Modify ^> tick "Desktop development with C++".
exit /b 1
:vs_load
echo   loading "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
:vs_ready

REM hactool (SciresM, ISC licence): reads your own game dump in XC3 Setup. A pinned release, downloaded once.
set "HT_URL=https://github.com/SciresM/hactool/releases/download/1.4.0/hactool-1.4.0-win.zip"
set "HT_SHA=36e9a221c8a7949c86ada9388eb703c90663aedfe9f65b6032429614c5e1abe8"
if exist tools\hactool.exe goto :ht_ready
echo   downloading hactool 1.4.0 (once)
curl -sSL -o tools\hactool.zip "%HT_URL%" || (echo   hactool download failed. & exit /b 1)
set "HT_GOT=none"
for /f "delims=" %%h in ('python -c "import hashlib,sys,pathlib;print(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest())" tools\hactool.zip') do set "HT_GOT=%%h"
if /i not "!HT_GOT!"=="%HT_SHA%" (echo   hactool download has the wrong checksum: !HT_GOT! & exit /b 1)
tar -xf tools\hactool.zip -C tools hactool.exe || exit /b 1
del tools\hactool.zip
:ht_ready

if "%MODE%"=="tools" goto :tools_build

echo [4/5] generated code
if exist game\main.elf goto :have_game
REM First run: no game files yet. Build the setup and open it (its install folder is this folder): the player adds
REM their own base game / update 2.2.0 / DLC (or it finds an emulator's) and prod.keys, it installs them into game\.
echo   No game files yet: building XC3 Setup and opening it...
call :tools_cmake || exit /b 1
echo   In XC3 Setup: add your base game, update 2.2.0 and DLC (or let it find your emulator's), pick prod.keys,
echo   keep this folder as the install folder, then Install. The build continues when you close Setup.
start "" /wait build-cmake\xc3-setup.exe
if not exist game\main.elf (echo   The game files are not installed ^(game\main.elf missing^): run build-windows.bat again to reopen Setup. & exit /b 1)
:have_game
python recomp\recompile.py || exit /b 1

REM FFmpeg (cutscene video): a pinned LGPL shared build, downloaded once into third_party\ffmpeg (not committed)
set "FF_URL=https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-31-12-59/ffmpeg-n8.1.3-27-g8dca1586f4-win64-lgpl-shared-8.1.zip"
set "FF_SHA=5cef7bc48e513f3cee7c881e8b2999d835541edb1846c3e89443a67f8f8556ab"
if exist third_party\ffmpeg\lib\avcodec.lib goto :ff_ready
echo   downloading FFmpeg 8.1 (80 MB, once)
if not exist third_party\ffmpeg mkdir third_party\ffmpeg
curl -sSL -o third_party\ffmpeg\ffmpeg.zip "%FF_URL%" || (echo   FFmpeg download failed. & exit /b 1)
set "FF_GOT=none"
for /f "delims=" %%h in ('python -c "import hashlib,sys,pathlib;print(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest())" third_party\ffmpeg\ffmpeg.zip') do set "FF_GOT=%%h"
if /i not "!FF_GOT!"=="%FF_SHA%" (echo   FFmpeg download has the wrong checksum: !FF_GOT! & exit /b 1)
tar -xf third_party\ffmpeg\ffmpeg.zip -C third_party\ffmpeg --strip-components=1 || exit /b 1
del third_party\ffmpeg\ffmpeg.zip
:ff_ready

echo [5/5] build (first build compiles ~430 MB of C: 10-30 min depending on cores)
cmake -B build-cmake -G Ninja -DCMAKE_MAKE_PROGRAM="!NINJA!" -DCMAKE_C_COMPILER="!CLANG!" -DCMAKE_CXX_COMPILER="!CLANGXX!" -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
cmake --build build-cmake || exit /b 1

copy /y tools\hactool.exe build-cmake\ > nul
echo.
echo Built build-cmake\xc3.exe. Play: make_playable.bat, then "XC3 Launcher.exe" in ..\XC3-Playable
echo   (or directly:   cd game ^&^& ..\build-cmake\xc3.exe --game .)
endlocal
exit /b 0

:tools_build
echo [4/5] (tools only: no generated code needed)
echo [5/5] build xc3-setup + xc3-launcher
call :tools_cmake || exit /b 1
echo.
echo Built build-cmake\xc3-setup.exe. (build-windows.bat without "tools" opens it by itself when game\ is empty.)
endlocal
exit /b 0

:tools_cmake
cmake -B build-cmake -G Ninja -DCMAKE_MAKE_PROGRAM="!NINJA!" -DCMAKE_C_COMPILER="!CLANG!" -DCMAKE_CXX_COMPILER="!CLANGXX!" -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
cmake --build build-cmake --target xc3-setup xc3-launcher || exit /b 1
copy /y tools\hactool.exe build-cmake\ > nul
exit /b 0
