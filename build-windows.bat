@echo off
REM Developer build (the same steps and portable compiler as "Install XC3.bat", without shortcuts):
REM   build-windows.bat           recompiles game\main.elf into gen\ and builds build-cmake\xc3.exe; the first time (no
REM                               game\ yet) it builds and opens XC3 Setup for your game files
REM   build-windows.bat tools     XC3 Setup and the launcher only
REM   add "verbose" to also print every compiler command (progress, step times and the commands run are always shown)
REM Tools (Python, CMake, Ninja, llvm-mingw clang) are downloaded into .tools\ once: no Visual Studio needed.
setlocal
cd /d "%~dp0"
set "XARGS=-BuildOnly"
for %%A in (%*) do (
  if /i "%%~A"=="tools" set "XARGS=-ToolsOnly"
  if /i "%%~A"=="verbose" set "XV=-Commands"
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install\install.ps1" %XARGS% %XV%
exit /b %errorlevel%
